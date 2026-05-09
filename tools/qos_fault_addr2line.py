#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys

PROGRAM_GRANULE = 2 * 1024 * 1024


def parse_int(value):
    s = value.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 16 if any(c in s.lower() for c in "abcdef") else 10)


def run_tool(argv):
    return subprocess.check_output(argv, text=True, errors="replace").strip()


def main():
    ap = argparse.ArgumentParser(
        description="Resolve a QOS user-process ELR to a source location."
    )
    ap.add_argument("elf", help="Local ELF with debug symbols, e.g. programs/game/game.elf")
    ap.add_argument("elr", help="ELR_EL1 value printed by QOS")
    ap.add_argument("--base", help="PROG_BASE printed by QOS; default aligns ELR to 2 MiB")
    ap.add_argument("--addr2line", default=os.environ.get("ADDR2LINE", "aarch64-linux-gnu-addr2line"))
    ap.add_argument("--nm", default=os.environ.get("NM", "aarch64-linux-gnu-nm"))
    args = ap.parse_args()

    elr = parse_int(args.elr)
    base = parse_int(args.base) if args.base else (elr & ~(PROGRAM_GRANULE - 1))
    off = elr - base
    if off < 0:
        print("ELR is below program base", file=sys.stderr)
        return 2

    print(f"ELR      = 0x{elr:08X}")
    print(f"BASE     = 0x{base:08X}")
    print(f"OFFSET   = 0x{off:X}")

    try:
        loc = run_tool([args.addr2line, "-f", "-C", "-e", args.elf, f"0x{off:X}"])
        print("\naddr2line:")
        print(loc)
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"\naddr2line failed: {e}", file=sys.stderr)

    try:
        nm = run_tool([args.nm, "-n", args.elf])
        best_addr = -1
        best_name = ""
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            if addr <= off and addr >= best_addr:
                best_addr = addr
                best_name = parts[2]
        if best_addr >= 0:
            print("\nnearest symbol:")
            print(f"{best_name}+0x{off - best_addr:X} (symbol at 0x{best_addr:X})")
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"\nnm failed: {e}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
