import sys
import struct

QOS_MAGIC = 0x514F5350  # "QOSP"

if len(sys.argv) != 3:
    print("Usage: build_program.py input.raw output.bin")
    sys.exit(1)

with open(sys.argv[1], "rb") as f:
    code = f.read()

size = len(code)
entry_offset = 0  # _start is at 0

header = struct.pack("<III", QOS_MAGIC, size, entry_offset)

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(code)

print("Built program.bin (size:", size, ")")
