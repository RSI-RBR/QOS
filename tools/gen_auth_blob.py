#!/usr/bin/env python3
import argparse
import hashlib
import os
from pathlib import Path

try:
    from argon2.low_level import Type as Argon2Type
    from argon2.low_level import hash_secret_raw as argon2_hash_secret_raw
except Exception:
    Argon2Type = None
    argon2_hash_secret_raw = None

AUTH_MAGIC_V1 = b"QAUTHV1\x00"
AUTH_MAGIC_V2 = b"QAUTHV2\x00"
AUTH_VERSION_V1 = 1
AUTH_VERSION_V2 = 2
AUTH_USER_MAX = 31
AUTH_SALT_BYTES = 16
AUTH_HASH_BYTES = 32

AUTH_KDF_SHA256 = 1
AUTH_KDF_ARGON2ID = 2

AUTH_ARGON2_DEFAULT_T_COST = 3
AUTH_ARGON2_DEFAULT_M_COST_KIB = 4096
AUTH_ARGON2_DEFAULT_PARALLELISM = 1
AUTH_ARGON2_DEFAULT_VERSION = 0x13


def make_password_hash(password: str, salt: bytes) -> bytes:
    h = hashlib.sha256()
    h.update(password.encode("utf-8"))
    h.update(salt)
    return h.digest()


def make_password_hash_argon2id(
    password: str,
    salt: bytes,
    t_cost: int,
    m_cost_kib: int,
    parallelism: int,
    version: int,
) -> bytes:
    if argon2_hash_secret_raw is None or Argon2Type is None:
        raise RuntimeError(
            "argon2-cffi is required for Argon2id auth generation (pip install argon2-cffi)"
        )
    return argon2_hash_secret_raw(
        secret=password.encode("utf-8"),
        salt=salt,
        time_cost=t_cost,
        memory_cost=m_cost_kib,
        parallelism=parallelism,
        hash_len=AUTH_HASH_BYTES,
        type=Argon2Type.ID,
        version=version,
    )


def parse_hex_salt(s: str) -> bytes:
    raw = bytes.fromhex(s)
    if len(raw) != AUTH_SALT_BYTES:
        raise ValueError(f"salt must be {AUTH_SALT_BYTES} bytes (32 hex chars)")
    return raw


def parse_hex_hash(s: str) -> bytes:
    raw = bytes.fromhex(s)
    if len(raw) != AUTH_HASH_BYTES:
        raise ValueError(f"stored hash must be {AUTH_HASH_BYTES} bytes ({AUTH_HASH_BYTES * 2} hex chars)")
    return raw


def u32le(v: int) -> bytes:
    return bytes(
        [
            (v >> 0) & 0xFF,
            (v >> 8) & 0xFF,
            (v >> 16) & 0xFF,
            (v >> 24) & 0xFF,
        ]
    )


def validate_username(username: str) -> bytes:
    if not username or len(username) > AUTH_USER_MAX:
        raise ValueError(f"username length must be 1..{AUTH_USER_MAX}")
    u = username.encode("ascii")
    if any(ch < 32 or ch > 126 for ch in u):
        raise ValueError("username must be printable ASCII")
    return u


def build_blob_v1(username: str, salt: bytes, stored_hash: bytes) -> bytes:
    u = validate_username(username)

    if len(stored_hash) != AUTH_HASH_BYTES:
        raise RuntimeError("invalid hash length")

    out = bytearray()
    out += AUTH_MAGIC_V1
    out.append(AUTH_VERSION_V1)
    out.append(len(u))
    user_field = bytearray(AUTH_USER_MAX + 1)
    user_field[: len(u)] = u
    out += user_field
    out += salt
    out += stored_hash
    return bytes(out)


def build_blob_v2(
    username: str,
    salt: bytes,
    stored_hash: bytes,
    kdf_id: int,
    argon2_t_cost: int,
    argon2_m_cost_kib: int,
    argon2_parallelism: int,
    argon2_version: int,
) -> bytes:
    u = validate_username(username)

    if len(stored_hash) != AUTH_HASH_BYTES:
        raise RuntimeError("invalid hash length")

    out = bytearray()
    out += AUTH_MAGIC_V2
    out.append(AUTH_VERSION_V2)
    out.append(len(u))
    user_field = bytearray(AUTH_USER_MAX + 1)
    user_field[: len(u)] = u
    out += user_field

    out.append(kdf_id & 0xFF)
    out.append(0)  # reserved
    out += u32le(argon2_t_cost)
    out += u32le(argon2_m_cost_kib)
    out += u32le(argon2_parallelism)
    out += u32le(argon2_version)
    out += salt
    out += stored_hash
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate Quantum OS AUTH.BIN")
    ap.add_argument("--username", required=True, help="login username")
    ap.add_argument("--password", required=True, help="login password")
    ap.add_argument("--out", required=True, help="output path (for AUTH.BIN)")
    ap.add_argument("--format", choices=["v1", "v2"], default="v2", help="AUTH file format version")
    ap.add_argument("--kdf", choices=["sha256", "argon2id"], default="argon2id", help="password KDF")
    ap.add_argument("--salt-hex", default="", help="optional fixed 16-byte salt (hex)")
    ap.add_argument("--stored-hash-hex", default="", help="optional precomputed 32-byte hash (hex)")
    ap.add_argument("--argon2-t-cost", type=int, default=AUTH_ARGON2_DEFAULT_T_COST, help="Argon2id iterations")
    ap.add_argument("--argon2-m-kib", type=int, default=AUTH_ARGON2_DEFAULT_M_COST_KIB, help="Argon2id memory in KiB")
    ap.add_argument("--argon2-parallelism", type=int, default=AUTH_ARGON2_DEFAULT_PARALLELISM, help="Argon2id parallelism")
    ap.add_argument("--argon2-version", type=lambda x: int(x, 0), default=AUTH_ARGON2_DEFAULT_VERSION, help="Argon2 version (default 0x13)")
    args = ap.parse_args()

    salt = parse_hex_salt(args.salt_hex) if args.salt_hex else os.urandom(AUTH_SALT_BYTES)
    if args.stored_hash_hex:
        stored_hash = parse_hex_hash(args.stored_hash_hex)
    else:
        if args.kdf == "sha256":
            stored_hash = make_password_hash(args.password, salt)
        else:
            stored_hash = make_password_hash_argon2id(
                args.password,
                salt,
                args.argon2_t_cost,
                args.argon2_m_kib,
                args.argon2_parallelism,
                args.argon2_version,
            )

    if args.format == "v1":
        if args.kdf != "sha256":
            raise ValueError("v1 format only supports sha256")
        blob = build_blob_v1(args.username, salt, stored_hash)
        kdf_id = AUTH_KDF_SHA256
    else:
        kdf_id = AUTH_KDF_ARGON2ID if args.kdf == "argon2id" else AUTH_KDF_SHA256
        blob = build_blob_v2(
            args.username,
            salt,
            stored_hash,
            kdf_id,
            args.argon2_t_cost,
            args.argon2_m_kib,
            args.argon2_parallelism,
            args.argon2_version,
        )

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)

    print(f"Wrote {len(blob)} bytes to {out_path}")
    print(f"Username: {args.username}")
    print(f"Format: {args.format}")
    print(f"KDF: {args.kdf} (id={kdf_id})")
    if args.format == "v2":
        print(
            "Argon2 params: "
            f"t={args.argon2_t_cost} m_kib={args.argon2_m_kib} "
            f"p={args.argon2_parallelism} v=0x{args.argon2_version:02x}"
        )
    print(f"Salt hex: {salt.hex()}")
    print(f"Stored hash hex: {stored_hash.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
