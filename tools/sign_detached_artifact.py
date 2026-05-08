#!/usr/bin/env python3
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import mldsa65_host

QOS_PQ_SIG_MAGIC = 0x51505331
QOS_PQ_SIG_VERSION = 1
QOS_SIG_ALG_ED25519 = 2
QOS_SIG_ALG_MLDSA65 = 3
MLDSA65_SIG_BYTES = 3309


def sign_ed25519(openssl_bin: str, key_pem: str, message: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="qos_file_sig_") as td:
        msg_path = os.path.join(td, "msg.bin")
        sig_path = os.path.join(td, "sig.bin")
        with open(msg_path, "wb") as f:
            f.write(message)
        subprocess.check_call([
            openssl_bin, "pkeyutl",
            "-sign",
            "-inkey", key_pem,
            "-rawin",
            "-in", msg_path,
            "-out", sig_path,
        ])
        with open(sig_path, "rb") as f:
            return f.read()


def sign_mldsa65(priv_key_path: str, digest32: bytes) -> bytes:
    sig = mldsa65_host.sign(priv_key_path, digest32)
    if len(sig) != MLDSA65_SIG_BYTES:
        raise RuntimeError(f"unexpected ML-DSA-65 signature length: {len(sig)}")
    return sig


def write_pq_sidecar(out_path: str, signer_key_id: int, sig_alg: int, sig: bytes) -> None:
    header = struct.pack("<IIIII",
                         QOS_PQ_SIG_MAGIC,
                         QOS_PQ_SIG_VERSION,
                         signer_key_id,
                         sig_alg,
                         len(sig))
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(sig)


def build_message(label: str, signer_key_id: int, file_bytes: bytes) -> bytes:
    digest = hashlib.sha256(file_bytes).digest()
    label_b = label.encode("utf-8")
    if len(label_b) > 255:
        raise RuntimeError("label too long")

    msg = bytearray()
    msg.extend(b"QOS-FILE-SIG-V1\x00")
    msg.extend((1).to_bytes(4, "little"))  # schema version
    msg.extend(int(signer_key_id).to_bytes(4, "little"))
    msg.extend((QOS_SIG_ALG_ED25519).to_bytes(4, "little"))
    msg.extend(len(file_bytes).to_bytes(8, "little"))
    msg.extend(len(label_b).to_bytes(1, "little"))
    msg.extend(label_b)
    msg.extend(digest)
    return bytes(msg)


def main() -> int:
    if len(sys.argv) < 6 or len(sys.argv) > 9:
        print(
            "Usage: sign_detached_artifact.py <input-file> <out-ed25519-sig> "
            "<signer-key-id> <ed25519-key-pem> <openssl-bin> "
            "[pq-sign-key-bin] [out-pq-sidecar] [label]"
        )
        return 1

    in_file = sys.argv[1]
    out_sig = sys.argv[2]
    signer_key_id = int(sys.argv[3], 0)
    ed_key_pem = sys.argv[4]
    openssl_bin = sys.argv[5]
    pq_key = sys.argv[6] if len(sys.argv) >= 7 else ""
    out_pqs = sys.argv[7] if len(sys.argv) >= 8 else ""
    label = sys.argv[8] if len(sys.argv) >= 9 else Path(in_file).name

    with open(in_file, "rb") as f:
        file_bytes = f.read()
    msg = build_message(label, signer_key_id, file_bytes)

    ed_sig = sign_ed25519(openssl_bin, ed_key_pem, msg)
    if len(ed_sig) != 64:
        raise RuntimeError(f"unexpected Ed25519 signature length: {len(ed_sig)}")
    with open(out_sig, "wb") as f:
        f.write(ed_sig)

    if pq_key and out_pqs:
        msg_digest = hashlib.sha256(msg).digest()
        pq_sig = sign_mldsa65(pq_key, msg_digest)
        write_pq_sidecar(out_pqs, signer_key_id, QOS_SIG_ALG_MLDSA65, pq_sig)
    elif out_pqs and os.path.exists(out_pqs):
        os.remove(out_pqs)

    print(f"Signed {in_file} -> {out_sig}")
    if pq_key and out_pqs:
        print(f"PQ sidecar {out_pqs} (ML-DSA-65)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
