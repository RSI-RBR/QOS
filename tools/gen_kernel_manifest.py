#!/usr/bin/env python3
import hashlib
import subprocess
import sys


def parse_nm_symbol(nm_bin: str, elf_path: str, sym: str) -> int:
    out = subprocess.check_output([nm_bin, "-n", elf_path], text=True, errors="strict")
    for line in out.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[2] == sym:
            return int(parts[0], 16)
    raise RuntimeError(f"symbol not found: {sym}")


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("Usage: gen_kernel_manifest.py <kernel8.elf> <kernel8.img> [nm-bin]")
        return 1

    elf_path = sys.argv[1]
    img_path = sys.argv[2]
    nm_bin = sys.argv[3] if len(sys.argv) == 4 else "aarch64-linux-gnu-nm"

    text_start = parse_nm_symbol(nm_bin, elf_path, "__kernel_text_start")
    ro_end = parse_nm_symbol(nm_bin, elf_path, "__kernel_rodata_end")
    if ro_end <= text_start:
        raise RuntimeError("invalid section bounds")

    load_base = 0x80000
    off0 = text_start - load_base
    off1 = ro_end - load_base
    if off0 < 0 or off1 < 0:
        raise RuntimeError("symbol below kernel load base")

    with open(img_path, "rb") as f:
        blob = f.read()

    if off1 > len(blob):
        raise RuntimeError("image too small for computed section range")

    digest = hashlib.sha256(blob[off0:off1]).digest()
    digest_hex = "".join(f"{b:02X}" for b in digest)

    print("Measured kernel digest (text..rodata):")
    print(digest_hex)
    print()
    print("C initializer for g_kernel_manifest.digest:")
    print("{" + ", ".join(f"0x{b:02X}" for b in digest) + "}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

