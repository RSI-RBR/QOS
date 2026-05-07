#!/usr/bin/env python3
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import mldsa65_host


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: gen_mldsa65_keypair.py <out-private-bin> <out-public-bin>")
        return 1
    priv_path = sys.argv[1]
    pub_path = sys.argv[2]
    mldsa65_host.keygen(pub_path, priv_path)
    print(f"Wrote ML-DSA-65 private key: {priv_path} (4032 bytes)")
    print(f"Wrote ML-DSA-65 public key:  {pub_path} (1952 bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
