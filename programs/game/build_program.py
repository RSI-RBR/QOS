import sys
import struct
import hashlib

QOS_MAGIC = 0x514F5350  # "QOSP"
QOS_SEC_MAGIC = 0x53454331  # "SEC1"
QOS_PROG_FLAG_SHA256 = 0x00000001
QOS_SIG_ALG_DIGEST_ONLY = 0x00000001
QOS_MAX_SIGNATURE_BYTES = 64
DEFAULT_SIGNER_KEY_ID = 0x00010001  # dev-main

if len(sys.argv) not in (3, 4):
    print("Usage: build_program.py input.raw output.bin [signer_key_id]")
    sys.exit(1)

signer_key_id = DEFAULT_SIGNER_KEY_ID
if len(sys.argv) == 4:
    signer_key_id = int(sys.argv[3], 0)

with open(sys.argv[1], "rb") as f:
    code = f.read()

size = len(code)
entry_offset = 0
header = struct.pack("<III", QOS_MAGIC, size, entry_offset)
digest = hashlib.sha256(code).digest()
sig = bytes(QOS_MAX_SIGNATURE_BYTES)
sec_header = struct.pack(
    "<IIIIII32s64s",
    QOS_SEC_MAGIC,
    struct.calcsize("<IIIIII32s64s"),
    QOS_PROG_FLAG_SHA256,
    signer_key_id,
    QOS_SIG_ALG_DIGEST_ONLY,
    0,
    digest,
    sig,
)

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(sec_header)
    f.write(code)

print("Built game.bin (size:", size, ")")
