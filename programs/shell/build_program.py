import struct
import sys

QOS_MAGIC = 0x514F5350  # "QOSP"

if len(sys.argv) != 3:
    print("Usage: build_program.py input.raw output.bin")
    sys.exit(1)

with open(sys.argv[1], "rb") as f:
    code = f.read()

header = struct.pack("<III", QOS_MAGIC, len(code), 0)

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(code)

print("Built program.bin (size:", len(code), ")")
