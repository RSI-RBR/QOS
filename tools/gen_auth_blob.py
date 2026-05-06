#!/usr/bin/env python3
import argparse
import hashlib
import os
from pathlib import Path

AUTH_MAGIC = b"QAUTHV1\x00"
AUTH_VERSION = 1
AUTH_USER_MAX = 31
AUTH_SALT_BYTES = 16
AUTH_HASH_BYTES = 32


def make_password_hash(password: str, salt: bytes) -> bytes:
    h = hashlib.sha256()
    h.update(password.encode("utf-8"))
    h.update(salt)
    return h.digest()


def parse_hex_salt(s: str) -> bytes:
    raw = bytes.fromhex(s)
    if len(raw) != AUTH_SALT_BYTES:
        raise ValueError(f"salt must be {AUTH_SALT_BYTES} bytes (32 hex chars)")
    return raw


def build_blob(username: str, password: str, salt: bytes) -> bytes:
    if not username or len(username) > AUTH_USER_MAX:
        raise ValueError(f"username length must be 1..{AUTH_USER_MAX}")
    u = username.encode("ascii")
    if any(ch < 32 or ch > 126 for ch in u):
        raise ValueError("username must be printable ASCII")

    stored_hash = make_password_hash(password, salt)
    if len(stored_hash) != AUTH_HASH_BYTES:
        raise RuntimeError("invalid hash length")

    out = bytearray()
    out += AUTH_MAGIC
    out.append(AUTH_VERSION)
    out.append(len(u))
    user_field = bytearray(AUTH_USER_MAX + 1)
    user_field[: len(u)] = u
    out += user_field
    out += salt
    out += stored_hash
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate Quantum OS AUTH.BIN")
    ap.add_argument("--username", required=True, help="login username")
    ap.add_argument("--password", required=True, help="login password")
    ap.add_argument("--out", required=True, help="output path (for AUTH.BIN)")
    ap.add_argument("--salt-hex", default="", help="optional fixed 16-byte salt (hex)")
    args = ap.parse_args()

    salt = parse_hex_salt(args.salt_hex) if args.salt_hex else os.urandom(AUTH_SALT_BYTES)
    blob = build_blob(args.username, args.password, salt)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)

    print(f"Wrote {len(blob)} bytes to {out_path}")
    print(f"Username: {args.username}")
    print(f"Salt hex: {salt.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
