#!/usr/bin/env python3
import argparse
import os
import socket
import struct
import threading
import hashlib
import time
from dataclasses import dataclass

from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import mlkem768_host

try:
    from argon2.low_level import Type as Argon2Type
    from argon2.low_level import hash_secret_raw as argon2_hash_secret_raw
except Exception:
    Argon2Type = None
    argon2_hash_secret_raw = None

RLOGIN_MAGIC = 0x51524C47
RLOGIN_VER = 1

TYPE_CLIENT_HELLO = 1
TYPE_SERVER_HELLO = 2
TYPE_AUTH_PROOF = 3
TYPE_AUTH_RESULT = 4
TYPE_COMMAND = 5
TYPE_COMMAND_RESULT = 6
TYPE_TTY_INPUT = 7
TYPE_TTY_OUTPUT = 8

TAG_LEN = 16
AUTH_SALT_BYTES = 16
AUTH_HASH_BYTES = 32
AUTH_KDF_SHA256 = 1
AUTH_KDF_ARGON2ID = 2
AUTH_ARGON2_DEFAULT_T_COST = 3
AUTH_ARGON2_DEFAULT_M_COST_KIB = 4096
AUTH_ARGON2_DEFAULT_PARALLELISM = 1
AUTH_ARGON2_DEFAULT_VERSION = 0x13
RLOGIN_STATUS_OK = 0
RLOGIN_STATUS_AUTH_UNAVAILABLE = 1
RLOGIN_STATUS_RATE_LIMITED = 2
RLOGIN_STATUS_LOCKED = 3
RLOGIN_KEX_EXT_MAGIC = 0x524B4558  # "RKEX"
RLOGIN_KEX_EXT_VER = 1
RLOGIN_KEX_X25519 = 1
RLOGIN_KEX_MLKEM768 = 2
RLOGIN_KEX_HYBRID = 3


def be16(v: int) -> bytes:
    return struct.pack(">H", v)


def be32(v: int) -> bytes:
    return struct.pack(">I", v)


def read_be16(b: bytes) -> int:
    return struct.unpack(">H", b)[0]


def read_be32(b: bytes) -> int:
    return struct.unpack(">I", b)[0]


def build_header(msg_type: int, session_id: int, seq: int, payload_len: int) -> bytes:
    return (
        be32(RLOGIN_MAGIC)
        + bytes([RLOGIN_VER, msg_type])
        + b"\x00\x00"
        + be32(session_id)
        + be32(seq)
        + be16(payload_len)
        + b"\x00\x00"
    )


def parse_header(pkt: bytes):
    if len(pkt) < 20:
        raise ValueError("short packet")
    if read_be32(pkt[0:4]) != RLOGIN_MAGIC or pkt[4] != RLOGIN_VER:
        raise ValueError("bad magic/version")
    msg_type = pkt[5]
    session_id = read_be32(pkt[8:12])
    seq = read_be32(pkt[12:16])
    payload_len = read_be16(pkt[16:18])
    if 20 + payload_len > len(pkt):
        raise ValueError("bad length")
    return msg_type, session_id, seq, pkt[20 : 20 + payload_len]


def kex_mode_name(mode: int) -> str:
    if mode == RLOGIN_KEX_MLKEM768:
        return "ML-KEM-768"
    if mode == RLOGIN_KEX_HYBRID:
        return "ML-KEM-768+X25519"
    return "X25519"


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def derive_password_hash(password: str,
                         salt: bytes,
                         kdf_id: int,
                         argon2_t_cost: int,
                         argon2_m_cost_kib: int,
                         argon2_parallelism: int,
                         argon2_version: int) -> bytes:
    if kdf_id == AUTH_KDF_SHA256:
        return sha256(password.encode("utf-8") + salt)
    if kdf_id == AUTH_KDF_ARGON2ID:
        if argon2_hash_secret_raw is None or Argon2Type is None:
            raise RuntimeError(
                "argon2-cffi is required for Argon2id auth (pip install argon2-cffi)"
            )
        return argon2_hash_secret_raw(
            secret=password.encode("utf-8"),
            salt=salt,
            time_cost=argon2_t_cost,
            memory_cost=argon2_m_cost_kib,
            parallelism=argon2_parallelism,
            hash_len=AUTH_HASH_BYTES,
            type=Argon2Type.ID,
            version=argon2_version,
        )
    raise RuntimeError(f"Unsupported server KDF id: {kdf_id}")


@dataclass
class CryptoState:
    key: bytes
    nonce_base: bytes
    client_seq: int = 0

    def next_client_seq(self) -> int:
        self.client_seq += 1
        if self.client_seq == 0:
            self.client_seq = 1
        return self.client_seq

    def make_iv(self, seq: int) -> bytes:
        iv = bytearray(self.nonce_base)
        iv[8] ^= (seq >> 24) & 0xFF
        iv[9] ^= (seq >> 16) & 0xFF
        iv[10] ^= (seq >> 8) & 0xFF
        iv[11] ^= seq & 0xFF
        return bytes(iv)

    def encrypt(self, msg_type: int, session_id: int, plain: bytes) -> bytes:
        seq = self.next_client_seq()
        hdr = build_header(msg_type, session_id, seq, len(plain) + TAG_LEN)
        aes = AESGCM(self.key)
        iv = self.make_iv(seq)
        ct_all = aes.encrypt(iv, plain, hdr)  # ciphertext + 16-byte tag
        return hdr + ct_all

    def decrypt(self, hdr: bytes, seq: int, payload: bytes) -> bytes:
        if len(payload) < TAG_LEN:
            raise ValueError("short encrypted payload")
        aes = AESGCM(self.key)
        iv = self.make_iv(seq)
        return aes.decrypt(iv, payload, hdr)


def derive_key_material(shared: bytes, client_nonce: bytes, server_nonce: bytes):
    salt = client_nonce + server_nonce
    hkdf_key = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt,
        info=b"qos-rlogin-key-v1",
    ).derive(shared)
    hkdf_iv = HKDF(
        algorithm=hashes.SHA256(),
        length=12,
        salt=salt,
        info=b"qos-rlogin-iv-v1",
    ).derive(shared)
    return hkdf_key, hkdf_iv


def recv_one(sock: socket.socket, timeout_s: float = 3.0):
    sock.settimeout(timeout_s)
    pkt, _ = sock.recvfrom(2048)
    return pkt


def recv_one_with_addr(sock: socket.socket, timeout_s: float = 3.0):
    sock.settimeout(timeout_s)
    return sock.recvfrom(2048)


def main():
    ap = argparse.ArgumentParser(description="Quantum OS remote shell tunnel client (UDP)")
    ap.add_argument("--host", required=True, help="Pi IP address")
    ap.add_argument("--port", type=int, default=2222, help="remote login UDP port")
    ap.add_argument("--username", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--broadcast", action="store_true", help="send first hello to 255.255.255.255")
    ap.add_argument(
        "--timeout",
        type=float,
        default=15.0,
        help="seconds to wait for handshake/auth replies (default: 15; Pi Zero 2W can be slower)",
    )
    ap.add_argument(
        "--kex",
        choices=["hybrid", "pq", "x25519"],
        default="hybrid",
        help="key exchange mode preference (default: hybrid)",
    )
    ap.add_argument(
        "--allow-x25519-fallback",
        action="store_true",
        help="allow downgrade to X25519 if server does not negotiate PQ",
    )
    args = ap.parse_args()

    server = (args.host, args.port)
    hello_server = ("255.255.255.255", args.port) if args.broadcast else server
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("", 0))
    else:
        sock.connect(server)

    sk = x25519.X25519PrivateKey.generate()
    pk = sk.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    req_kex_mode = RLOGIN_KEX_HYBRID
    if args.kex == "pq":
        req_kex_mode = RLOGIN_KEX_MLKEM768
    elif args.kex == "x25519":
        req_kex_mode = RLOGIN_KEX_X25519

    mlkem_pk = b""
    mlkem_sk = b""
    if req_kex_mode != RLOGIN_KEX_X25519:
        try:
            mlkem_pk, mlkem_sk = mlkem768_host.keypair_bytes()
        except Exception as exc:
            raise SystemExit(f"ML-KEM keygen failed: {exc}")
        if len(mlkem_pk) != mlkem768_host.PK_BYTES or len(mlkem_sk) != mlkem768_host.SK_BYTES:
            raise SystemExit("ML-KEM key material size mismatch")

    kex_ext = (
        be32(RLOGIN_KEX_EXT_MAGIC)
        + bytes([RLOGIN_KEX_EXT_VER, req_kex_mode])
        + be16(len(mlkem_pk))
        + mlkem_pk
    )
    client_nonce = os.urandom(32)
    user = args.username.encode("ascii")
    if len(user) == 0 or len(user) > 31:
        raise SystemExit("username must be 1..31 chars")

    hello_payload = pk + client_nonce + bytes([len(user)]) + user + kex_ext
    hello = build_header(TYPE_CLIENT_HELLO, 0, 0, len(hello_payload)) + hello_payload
    if args.broadcast:
        sock.sendto(hello, hello_server)
    else:
        sock.send(hello)

    if args.broadcast:
        pkt, addr = recv_one_with_addr(sock, args.timeout)
        server = (addr[0], args.port)
        sock.connect(server)
        print(f"Discovered Quantum OS at {server[0]}:{server[1]}")
    else:
        pkt = recv_one(sock, args.timeout)
    msg_type, session_id, _seq, payload = parse_header(pkt)
    if msg_type != TYPE_SERVER_HELLO:
        raise SystemExit("unexpected server reply")
    if len(payload) < 81:
        raise SystemExit("short server hello")

    server_pub = payload[0:32]
    server_nonce = payload[32:64]
    salt = payload[64:80]
    status = payload[80]
    kdf_id = AUTH_KDF_SHA256
    argon2_t_cost = AUTH_ARGON2_DEFAULT_T_COST
    argon2_m_cost_kib = AUTH_ARGON2_DEFAULT_M_COST_KIB
    argon2_parallelism = AUTH_ARGON2_DEFAULT_PARALLELISM
    argon2_version = AUTH_ARGON2_DEFAULT_VERSION
    kex_ext_off = 81
    if len(payload) >= 98:
        kdf_id = payload[81]
        argon2_t_cost = read_be32(payload[82:86])
        argon2_m_cost_kib = read_be32(payload[86:90])
        argon2_parallelism = read_be32(payload[90:94])
        argon2_version = read_be32(payload[94:98])
        kex_ext_off = 98
    if status != RLOGIN_STATUS_OK:
        if status == RLOGIN_STATUS_AUTH_UNAVAILABLE:
            raise SystemExit("server auth store unavailable")
        if status == RLOGIN_STATUS_RATE_LIMITED:
            raise SystemExit("server rate-limited auth attempts; wait and retry")
        if status == RLOGIN_STATUS_LOCKED:
            raise SystemExit("server auth temporarily locked due to repeated failures")
        raise SystemExit(f"server rejected auth start (status={status})")
    if kdf_id == AUTH_KDF_ARGON2ID:
        print(
            "Server auth KDF: argon2id "
            f"(t={argon2_t_cost} m_kib={argon2_m_cost_kib} "
            f"p={argon2_parallelism} v=0x{argon2_version:02x})"
        )
    else:
        print("Server auth KDF: sha256")

    negotiated_kex = RLOGIN_KEX_X25519
    server_mlkem_ct = b""
    if len(payload) >= kex_ext_off + 8 and read_be32(payload[kex_ext_off : kex_ext_off + 4]) == RLOGIN_KEX_EXT_MAGIC:
        if payload[kex_ext_off + 4] != RLOGIN_KEX_EXT_VER:
            raise SystemExit("unsupported remote-login KEX extension version")
        negotiated_kex = payload[kex_ext_off + 5]
        ct_len = read_be16(payload[kex_ext_off + 6 : kex_ext_off + 8])
        ct_off = kex_ext_off + 8
        if ct_off + ct_len > len(payload):
            raise SystemExit("short server KEX extension")
        server_mlkem_ct = payload[ct_off : ct_off + ct_len]

    if req_kex_mode == RLOGIN_KEX_X25519 and negotiated_kex != RLOGIN_KEX_X25519:
        raise SystemExit("server did not honor requested X25519-only mode")
    if req_kex_mode == RLOGIN_KEX_MLKEM768 and negotiated_kex == RLOGIN_KEX_X25519 and not args.allow_x25519_fallback:
        raise SystemExit("server downgraded to X25519; rerun with --allow-x25519-fallback to permit")
    if req_kex_mode == RLOGIN_KEX_HYBRID:
        if negotiated_kex == RLOGIN_KEX_X25519 and not args.allow_x25519_fallback:
            raise SystemExit("server downgraded to X25519; rerun with --allow-x25519-fallback to permit")
        if negotiated_kex == RLOGIN_KEX_MLKEM768 and not args.allow_x25519_fallback:
            raise SystemExit("server did not provide hybrid KEX; rerun with --allow-x25519-fallback to permit")

    x_shared = sk.exchange(x25519.X25519PublicKey.from_public_bytes(server_pub))
    pq_shared = b""
    if negotiated_kex in (RLOGIN_KEX_MLKEM768, RLOGIN_KEX_HYBRID):
        if len(server_mlkem_ct) != mlkem768_host.CT_BYTES:
            raise SystemExit("invalid ML-KEM ciphertext length in server hello")
        if not mlkem_sk:
            raise SystemExit("server selected PQ KEX but client has no ML-KEM key")
        try:
            pq_shared = mlkem768_host.decaps_with_sk(mlkem_sk, server_mlkem_ct)
        except Exception as exc:
            raise SystemExit(f"ML-KEM decapsulation failed: {exc}")
        if len(pq_shared) != mlkem768_host.SS_BYTES:
            raise SystemExit("invalid ML-KEM shared secret length")

    if negotiated_kex == RLOGIN_KEX_X25519:
        shared = x_shared
    elif negotiated_kex == RLOGIN_KEX_MLKEM768:
        shared = pq_shared
    elif negotiated_kex == RLOGIN_KEX_HYBRID:
        shared = x_shared + pq_shared
    else:
        raise SystemExit(f"unsupported negotiated KEX mode: {negotiated_kex}")

    print(f"Remote login negotiated KEX: {kex_mode_name(negotiated_kex)}")
    key, nonce_base = derive_key_material(shared, client_nonce, server_nonce)
    cs = CryptoState(key=key, nonce_base=nonce_base)

    pw_hash = derive_password_hash(
        args.password,
        salt,
        kdf_id,
        argon2_t_cost,
        argon2_m_cost_kib,
        argon2_parallelism,
        argon2_version,
    )
    auth_resp = sha256(pw_hash + client_nonce + server_nonce)
    sock.send(cs.encrypt(TYPE_AUTH_PROOF, session_id, auth_resp))

    pkt = recv_one(sock, args.timeout)
    msg_type, sid2, seq2, payload = parse_header(pkt)
    if sid2 != session_id or msg_type != TYPE_AUTH_RESULT:
        raise SystemExit("unexpected auth result packet")
    plain = cs.decrypt(pkt[:20], seq2, payload)
    if len(plain) != 1 or plain[0] != 1:
        raise SystemExit("authentication failed")

    print("Authenticated. Tunnel attached. Type shell commands, '/quit' to exit, '/cmd <x>' for control commands.")

    stop = threading.Event()

    def recv_loop():
        while not stop.is_set():
            try:
                pkt = recv_one(sock, 0.5)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                msg_type, sid, seq, payload = parse_header(pkt)
            except Exception:
                continue
            if sid != session_id:
                continue
            if msg_type in (TYPE_COMMAND_RESULT, TYPE_TTY_OUTPUT):
                try:
                    plain = cs.decrypt(pkt[:20], seq, payload)
                except Exception:
                    continue
                try:
                    print(plain.decode("utf-8", errors="replace"), end="", flush=True)
                except Exception:
                    pass

    t = threading.Thread(target=recv_loop, daemon=True)
    t.start()

    # Ask server for immediate status line.
    sock.send(cs.encrypt(TYPE_COMMAND, session_id, b"status"))
    time.sleep(0.05)

    try:
        while True:
            line = input()
            if line == "/quit":
                break
            if line.startswith("/cmd "):
                cmd = line[5:].encode("utf-8")
                sock.send(cs.encrypt(TYPE_COMMAND, session_id, cmd))
                continue
            data = (line + "\n").encode("utf-8")
            sock.send(cs.encrypt(TYPE_TTY_INPUT, session_id, data))
    finally:
        stop.set()
        try:
            sock.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
