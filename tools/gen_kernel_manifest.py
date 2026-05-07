#!/usr/bin/env python3
import hashlib
import os
import subprocess
import sys
import tempfile
import struct
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import mldsa65_host

QOS_PQ_SIG_MAGIC = 0x51505331
QOS_PQ_SIG_VERSION = 1
QOS_SIG_ALG_MLDSA65 = 3
MLDSA65_SIG_BYTES = 3309


def parse_nm_symbol(nm_bin: str, elf_path: str, sym: str) -> int:
    out = subprocess.check_output([nm_bin, "-n", elf_path], text=True, errors="strict")
    for line in out.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[2] == sym:
            return int(parts[0], 16)
    raise RuntimeError(f"symbol not found: {sym}")


def digest_c_initializer(digest: bytes) -> str:
    return "{ " + ", ".join(f"0x{b:02X}" for b in digest) + " }"


def sign_ed25519(openssl_bin: str, key_pem: str, message: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="qos_kernsig_") as td:
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
            "-out", sig_path,
        ])
        with open(sig_path, "rb") as f:
            return f.read()


def sign_mldsa65(priv_key_path: str, message: bytes) -> bytes:
    sig = mldsa65_host.sign(priv_key_path, message)
    if len(sig) != MLDSA65_SIG_BYTES:
        raise RuntimeError(f"unexpected ML-DSA-65 signature length: {len(sig)}")
    return sig


def write_pq_sidecar(out_path: str, signer_key_id: int, sig_alg: int, sig: bytes) -> None:
    header = struct.pack("<IIIII",
                         QOS_PQ_SIG_MAGIC,
                         QOS_PQ_SIG_VERSION,
                         signer_key_id,
                         sig_alg,
                         len(sig))
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(sig)


def main() -> int:
    if len(sys.argv) < 4 or len(sys.argv) > 10:
        print("Usage: gen_kernel_manifest.py <kernel8.elf> <kernel8.img> <out-header> [nm-bin] [signer-key-id] [signing-key-pem] [openssl-bin] [pq-sign-key-bin] [pq-out-file]")
        return 1

    elf_path = sys.argv[1]
    img_path = sys.argv[2]
    out_header = sys.argv[3]
    nm_bin = sys.argv[4] if len(sys.argv) >= 5 else "aarch64-linux-gnu-nm"
    signer_key_id = int(sys.argv[5], 0) if len(sys.argv) >= 6 else 0x00000001
    signing_key_pem = sys.argv[6] if len(sys.argv) >= 7 else ""
    openssl_bin = sys.argv[7] if len(sys.argv) >= 8 else "openssl"
    pq_sign_key_bin = sys.argv[8] if len(sys.argv) >= 9 else ""
    pq_out_file = sys.argv[9] if len(sys.argv) >= 10 else ""

    text_start = parse_nm_symbol(nm_bin, elf_path, "__kernel_text_start")
    ro_verify_end = parse_nm_symbol(nm_bin, elf_path, "__kernel_rodata_verify_end")
    man_start = parse_nm_symbol(nm_bin, elf_path, "__kernel_manifest_start")
    man_end = parse_nm_symbol(nm_bin, elf_path, "__kernel_manifest_end")
    if ro_verify_end <= text_start:
        raise RuntimeError("invalid section bounds")
    if man_end <= man_start:
        raise RuntimeError("invalid manifest section bounds")

    load_base = 0x80000
    text_off0 = text_start - load_base
    text_off1 = ro_verify_end - load_base
    man_off0 = man_start - load_base
    man_off1 = man_end - load_base
    if text_off0 < 0 or text_off1 < 0 or man_off0 < 0 or man_off1 < 0:
        raise RuntimeError("symbol below kernel load base")

    with open(img_path, "rb") as f:
        blob = f.read()

    if text_off1 > len(blob) or man_off1 > len(blob):
        raise RuntimeError("image too small for computed section range")

    mem_digest = hashlib.sha256(blob[text_off0:text_off1]).digest()

    # Full image digest excluding the manifest section bytes.
    h = hashlib.sha256()
    h.update(blob[:man_off0])
    h.update(blob[man_off1:])
    file_digest = h.digest()

    mem_digest_hex = "".join(f"{b:02X}" for b in mem_digest)
    file_digest_hex = "".join(f"{b:02X}" for b in file_digest)
    mem_digest_init = digest_c_initializer(mem_digest)
    file_digest_init = digest_c_initializer(file_digest)

    sig_alg = "QOS_SIG_ALG_DIGEST_ONLY"
    sig_len = 0
    sig_bytes = bytes(64)
    msg_sig_alg = 1  # QOS_SIG_ALG_DIGEST_ONLY
    if signing_key_pem:
        msg_sig_alg = 2  # QOS_SIG_ALG_ED25519
    msg = bytearray()
    msg.extend(b"QOS-KERN-SIG-V1\x00")
    msg.extend((1).to_bytes(4, "little"))
    msg.extend(int(signer_key_id).to_bytes(4, "little"))
    msg.extend(msg_sig_alg.to_bytes(4, "little"))
    msg.extend((1).to_bytes(4, "little"))  # QOS_PROG_FLAG_SHA256
    msg.extend(mem_digest)
    msg.extend(file_digest)
    if signing_key_pem:
        sig = sign_ed25519(openssl_bin, signing_key_pem, bytes(msg))
        if len(sig) != 64:
            raise RuntimeError(f"unexpected Ed25519 signature length: {len(sig)}")
        sig_alg = "QOS_SIG_ALG_ED25519"
        sig_len = 64
        sig_bytes = sig

    if pq_sign_key_bin:
        if not pq_out_file:
            raise RuntimeError("PQ sign key provided but pq-out-file missing")
        # ML-DSA sidecar signs SHA-256(canonical kernel-sign message).
        msg_digest = hashlib.sha256(bytes(msg)).digest()
        pq_sig = sign_mldsa65(pq_sign_key_bin, msg_digest)
        if len(pq_sig) != MLDSA65_SIG_BYTES:
            raise RuntimeError(f"unexpected ML-DSA-65 signature length: {len(pq_sig)}")
        write_pq_sidecar(pq_out_file, int(signer_key_id), QOS_SIG_ALG_MLDSA65, pq_sig)
    elif pq_out_file and os.path.exists(pq_out_file):
        os.remove(pq_out_file)

    sig_init = digest_c_initializer(sig_bytes)

    header = f"""#ifndef KERNEL_MANIFEST_AUTOGEN_H
#define KERNEL_MANIFEST_AUTOGEN_H

// Auto-generated by tools/gen_kernel_manifest.py
// Memory source region: __kernel_text_start .. __kernel_rodata_verify_end
// Memory digest: {mem_digest_hex}
// File digest scope: kernel8.img excluding __kernel_manifest_start..__kernel_manifest_end
// File digest: {file_digest_hex}

#define KERNEL_MANIFEST_VERSION       1u
#define KERNEL_MANIFEST_SIGNER_KEY_ID 0x{signer_key_id:08X}u
#define KERNEL_MANIFEST_SIG_ALG       {sig_alg}
#define KERNEL_MANIFEST_FLAGS         QOS_PROG_FLAG_SHA256
#define KERNEL_MANIFEST_SIG_LEN       {sig_len}u
#define KERNEL_MANIFEST_DIGEST_INIT   {mem_digest_init}
#define KERNEL_MANIFEST_FILE_DIGEST_INIT {file_digest_init}
#define KERNEL_MANIFEST_SIGNATURE_INIT {sig_init}

#endif
"""

    with open(out_header, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)

    print("Measured kernel memory digest (text..rodata_verify_end):")
    print(mem_digest_hex)
    print("Measured kernel file digest (kernel8.img excluding .kmanifest):")
    print(file_digest_hex)
    if signing_key_pem:
        print(f"Kernel manifest Ed25519 signed with: {signing_key_pem}")
    else:
        print("Kernel manifest in digest-only mode (unsigned).")
    if pq_sign_key_bin:
        print(f"Kernel PQ signature written to: {pq_out_file}")
        print("Kernel PQ signature mode: ML-DSA-65")
    else:
        print("Kernel PQ signature not generated.")
    print(f"Wrote header: {out_header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
