import struct
import sys
import hashlib

QOS_MAGIC = 0x514F5350  # "QOSP"
QOS_SEC_MAGIC = 0x53454331  # "SEC1"
QOS_PROG_FLAG_SHA256 = 0x00000001

if len(sys.argv) != 3:
    print("Usage: build_program.py input.raw output.bin")
    sys.exit(1)

with open(sys.argv[1], "rb") as f:
    code = f.read()

header = struct.pack("<III", QOS_MAGIC, len(code), 0)
digest = hashlib.sha256(code).digest()
sec_header = struct.pack("<III32s", QOS_SEC_MAGIC, struct.calcsize("<III32s"), QOS_PROG_FLAG_SHA256, digest)

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(sec_header)
    f.write(code)

print("Built program.bin (size:", len(code), ")")
