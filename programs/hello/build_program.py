import sys
import struct
import hashlib
import subprocess
import tempfile
import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
import sys as _sys
if str(TOOLS_DIR) not in _sys.path:
    _sys.path.insert(0, str(TOOLS_DIR))
import mldsa65_host

QOS_MAGIC = 0x514F5350  # "QOSP"
QOS_SEC_MAGIC = 0x53454331  # "SEC1"
QOS_PQ_SIG_MAGIC = 0x51505331  # "QPS1"
QOS_PQ_SIG_VERSION = 0x00000001
QOS_PROG_FLAG_SHA256 = 0x00000001
QOS_SIG_ALG_DIGEST_ONLY = 0x00000001
QOS_SIG_ALG_ED25519 = 0x00000002
QOS_SIG_ALG_MLDSA65 = 0x00000003
QOS_MAX_SIGNATURE_BYTES = 64
MLDSA65_SIG_BYTES = 3309
DEFAULT_SIGNER_KEY_ID = 0x00010001  # dev-main


def sign_ed25519(openssl_bin, key_pem, message):
    with tempfile.TemporaryDirectory(prefix="qos_progsig_") as td:
        msg_path = os.path.join(td, "msg.bin")
        sig_path = os.path.join(td, "sig.bin")
        with open(msg_path, "wb") as f:
            f.write(message)
        subprocess.check_call([
            openssl_bin, "pkeyutl",
            "-sign",
            "-inkey", key_pem,
            "-rawin",
            "-in", msg_path,
            "-out", sig_path
        ])
        with open(sig_path, "rb") as f:
            return f.read()


def sign_mldsa65(priv_path, message):
    sig = mldsa65_host.sign(priv_path, message)
    if len(sig) != MLDSA65_SIG_BYTES:
        raise RuntimeError(f"unexpected ML-DSA-65 signature length: {len(sig)}")
    return sig


def write_pq_sidecar(path, signer_key_id, sig_alg, sig_bytes):
    with open(path, "wb") as f:
        f.write(struct.pack("<IIIII",
                            QOS_PQ_SIG_MAGIC,
                            QOS_PQ_SIG_VERSION,
                            signer_key_id,
                            sig_alg,
                            len(sig_bytes)))
        f.write(sig_bytes)


if len(sys.argv) < 4 or len(sys.argv) > 9:
    print("Usage: build_program.py input.raw output.bin fat_name_83 [signer_key_id] [sign_key_pem] [openssl_bin] [pq_sign_key_bin] [pq_out_file]")
    sys.exit(1)

signer_key_id = DEFAULT_SIGNER_KEY_ID
fat_name_83 = sys.argv[3]
if len(sys.argv) >= 5:
    signer_key_id = int(sys.argv[4], 0)
if len(fat_name_83) != 11:
    print("fat_name_83 must be exactly 11 chars (FAT 8.3, space padded)")
    sys.exit(1)
sign_key_pem = sys.argv[5] if len(sys.argv) >= 6 else ""
openssl_bin = sys.argv[6] if len(sys.argv) >= 7 else "openssl"
pq_sign_key = sys.argv[7] if len(sys.argv) >= 8 else ""
pq_out_file = sys.argv[8] if len(sys.argv) >= 9 else (sys.argv[2] + ".pqs")

with open(sys.argv[1], "rb") as f:
    code = f.read()

size = len(code)
entry_offset = 0  # _start is at 0

header = struct.pack("<III", QOS_MAGIC, size, entry_offset)
digest = hashlib.sha256(code).digest()

sig_alg = QOS_SIG_ALG_DIGEST_ONLY
sig_len = 0
sig = bytes(QOS_MAX_SIGNATURE_BYTES)
if sign_key_pem:
    msg = bytearray()
    msg.extend(b"QOS-PROG-SIG-V1\x00")
    msg.extend(fat_name_83.encode("ascii"))
    msg.extend(struct.pack("<I", QOS_PROG_FLAG_SHA256))
    msg.extend(struct.pack("<I", signer_key_id))
    msg.extend(struct.pack("<I", QOS_SIG_ALG_ED25519))
    msg.extend(struct.pack("<I", size))
    msg.extend(digest)
    s = sign_ed25519(openssl_bin, sign_key_pem, bytes(msg))
    if len(s) != 64:
        print("Ed25519 signature length was not 64 bytes")
        sys.exit(1)
    sig_alg = QOS_SIG_ALG_ED25519
    sig_len = 64
    sig = s

sec_header = struct.pack(
    "<IIIIII32s64s",
    QOS_SEC_MAGIC,
    struct.calcsize("<IIIIII32s64s"),
    QOS_PROG_FLAG_SHA256,
    signer_key_id,
    sig_alg,
    sig_len,
    digest,
    sig,
)

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(sec_header)
    f.write(code)

if pq_sign_key:
    msg = bytearray()
    msg.extend(b"QOS-PROG-SIG-V1\x00")
    msg.extend(fat_name_83.encode("ascii"))
    msg.extend(struct.pack("<I", QOS_PROG_FLAG_SHA256))
    msg.extend(struct.pack("<I", signer_key_id))
    msg.extend(struct.pack("<I", sig_alg))
    msg.extend(struct.pack("<I", size))
    msg.extend(digest)
    # ML-DSA sidecar signs SHA-256(canonical program-sign message).
    msg_digest = hashlib.sha256(bytes(msg)).digest()
    pq_sig = sign_mldsa65(pq_sign_key, msg_digest)
    if len(pq_sig) != MLDSA65_SIG_BYTES:
        print("ML-DSA-65 signature length mismatch")
        sys.exit(1)
    write_pq_sidecar(pq_out_file, signer_key_id, QOS_SIG_ALG_MLDSA65, pq_sig)
    print("Built PQ sidecar:", pq_out_file)
    print("PQ signature mode: ML-DSA-65")
else:
    print("PQ sidecar not generated (no pq_sign_key_bin provided).")

print("Built program.bin (size:", size, ")")
