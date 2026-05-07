#!/usr/bin/env python3
import hashlib
import os
import sys

LAMPORT_BITS = 256
ELEM_BYTES = 32
PRIV_BYTES = LAMPORT_BITS * 2 * ELEM_BYTES
PUB_BYTES = LAMPORT_BITS * 2 * ELEM_BYTES


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: gen_mldsa65_compat_keypair.py <out-private-bin> <out-public-bin>")
        return 1

    priv_path = sys.argv[1]
    pub_path = sys.argv[2]

    sk = os.urandom(PRIV_BYTES)
    pk = bytearray(PUB_BYTES)

    for i in range(LAMPORT_BITS * 2):
        s = sk[i * ELEM_BYTES:(i + 1) * ELEM_BYTES]
        h = hashlib.sha256(s).digest()
        pk[i * ELEM_BYTES:(i + 1) * ELEM_BYTES] = h

    with open(priv_path, "wb") as f:
        f.write(sk)
    with open(pub_path, "wb") as f:
        f.write(pk)

    print(f"Wrote PQ compatibility private key: {priv_path} ({PRIV_BYTES} bytes)")
    print(f"Wrote PQ compatibility public key:  {pub_path} ({PUB_BYTES} bytes)")
    print("WARNING: This is a temporary ML-DSA compatibility bridge, not a real Dilithium keypair.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
