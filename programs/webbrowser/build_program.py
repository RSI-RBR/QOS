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
QOS_PROG_FLAG_MEM_LAYOUT_V1 = 0x00000002
QOS_PROG_FLAG_RELOC_RELATIVE_V1 = 0x00000004
QOS_SIG_ALG_DIGEST_ONLY = 0x00000001
QOS_SIG_ALG_ED25519 = 0x00000002
QOS_SIG_ALG_MLDSA65 = 0x00000003
QOS_MAX_SIGNATURE_BYTES = 64
MLDSA65_SIG_BYTES = 3309
DEFAULT_SIGNER_KEY_ID = 0x00010001  # dev-main
PROGRAM_ALLOC_GRANULE_BYTES = 2 * 1024 * 1024
PROGRAM_MAX_MEMORY_BYTES = 16 * 1024 * 1024
PROGRAM_MAX_RELOCS = 4096
PROGRAM_RELOC_RELATIVE_ENTRY_BYTES = 12
PAGE_SIZE = 4096
R_AARCH64_NONE = 0
R_AARCH64_RELATIVE = 1027
SHT_RELA = 4
SHT_REL = 9


def parse_size(value):
    s = str(value).strip()
    mul = 1
    if s.lower().endswith("m"):
        mul = 1024 * 1024
        s = s[:-1]
    elif s.lower().endswith("k"):
        mul = 1024
        s = s[:-1]
    return int(s, 0) * mul


PROGRAM_MEMORY_BYTES = parse_size(os.environ.get("QOS_PROGRAM_MEMORY_BYTES", str(PROGRAM_MAX_MEMORY_BYTES)))

if (PROGRAM_MEMORY_BYTES <= 0 or
        PROGRAM_MEMORY_BYTES > PROGRAM_MAX_MEMORY_BYTES or
        (PROGRAM_MEMORY_BYTES % PROGRAM_ALLOC_GRANULE_BYTES) != 0):
    print("Invalid QOS_PROGRAM_MEMORY_BYTES; must be a 2 MiB multiple up to 16 MiB")
    sys.exit(1)


def parse_nm_symbol(nm_bin, elf_path, sym):
    out = subprocess.check_output([nm_bin, "-n", elf_path], text=True, errors="strict")
    for line in out.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[2] == sym:
            return int(parts[0], 16)
    raise RuntimeError(f"symbol not found: {sym}")


def read_cstr(buf, off):
    end = off
    while end < len(buf) and buf[end] != 0:
        end += 1
    return buf[off:end].decode("ascii", errors="replace")


def extract_relative_relocations(elf_path, program_memory_bytes):
    if not elf_path:
        return []

    with open(elf_path, "rb") as f:
        elf = f.read()

    if len(elf) < 64 or elf[:4] != b"\x7fELF":
        raise RuntimeError("invalid ELF file")
    if elf[4] != 2 or elf[5] != 1:
        raise RuntimeError("expected ELF64 little-endian program")

    e_shoff = struct.unpack_from("<Q", elf, 40)[0]
    e_shentsize = struct.unpack_from("<H", elf, 58)[0]
    e_shnum = struct.unpack_from("<H", elf, 60)[0]
    e_shstrndx = struct.unpack_from("<H", elf, 62)[0]
    if e_shoff == 0 or e_shnum == 0 or e_shentsize < 64:
        return []
    if e_shoff + (e_shentsize * e_shnum) > len(elf):
        raise RuntimeError("ELF section headers out of range")

    sections = []
    for i in range(e_shnum):
        off = e_shoff + (i * e_shentsize)
        sh = struct.unpack_from("<IIQQQQIIQQ", elf, off)
        sections.append({
            "name_off": sh[0],
            "type": sh[1],
            "offset": sh[4],
            "size": sh[5],
            "entsize": sh[9],
            "name": "",
        })

    if e_shstrndx < e_shnum:
        shstr = sections[e_shstrndx]
        start = shstr["offset"]
        end = start + shstr["size"]
        if end <= len(elf):
            names = elf[start:end]
            for s in sections:
                if s["name_off"] < len(names):
                    s["name"] = read_cstr(names, s["name_off"])

    relocs = []
    unsupported = []
    for s in sections:
        if s["type"] == SHT_REL and s["size"] != 0:
            unsupported.append(f"{s['name'] or '<unnamed>'}: SHT_REL without explicit addends")
            continue
        if s["type"] != SHT_RELA or s["size"] == 0:
            continue
        entsize = s["entsize"] or 24
        if entsize < 24 or (s["size"] % entsize) != 0:
            raise RuntimeError(f"bad RELA section entry size: {s['name']}")
        if s["offset"] + s["size"] > len(elf):
            raise RuntimeError(f"RELA section out of range: {s['name']}")
        count = s["size"] // entsize
        for i in range(count):
            off = s["offset"] + (i * entsize)
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", elf, off)
            r_type = r_info & 0xFFFFFFFF
            if r_type == R_AARCH64_NONE:
                continue
            if r_type != R_AARCH64_RELATIVE:
                unsupported.append(f"{s['name'] or '<unnamed>'}: type {r_type} at 0x{r_offset:x}")
                continue
            if r_offset & 7:
                raise RuntimeError(f"unaligned relative relocation target: 0x{r_offset:x}")
            if r_offset + 8 > program_memory_bytes:
                raise RuntimeError(f"relative relocation target outside reservation: 0x{r_offset:x}")
            if r_addend < 0 or r_addend >= program_memory_bytes:
                raise RuntimeError(f"relative relocation addend outside reservation: 0x{r_addend:x}")
            relocs.append((r_offset, r_addend))

    if unsupported:
        raise RuntimeError("unsupported ELF relocations:\n  " + "\n  ".join(unsupported[:32]))
    if len(relocs) > PROGRAM_MAX_RELOCS:
        raise RuntimeError(f"too many relative relocations: {len(relocs)} > {PROGRAM_MAX_RELOCS}")

    relocs.sort()
    return relocs


def pack_relative_relocations(relocs):
    if not relocs:
        return b""
    out = bytearray()
    out.extend(struct.pack("<II", len(relocs), PROGRAM_RELOC_RELATIVE_ENTRY_BYTES))
    for off, addend in relocs:
        out.extend(struct.pack("<IQ", off, addend))
    return bytes(out)


def build_sig_message(fat_name_83, flags, signer_key_id, sig_alg,
                      size, user_rw_offset, user_rw_size, digest,
                      reloc_blob):
    msg = bytearray()
    msg.extend(b"QOS-PROG-SIG-V1\x00")
    msg.extend(fat_name_83.encode("ascii"))
    msg.extend(struct.pack("<I", flags))
    msg.extend(struct.pack("<I", signer_key_id))
    msg.extend(struct.pack("<I", sig_alg))
    msg.extend(struct.pack("<I", size))
    msg.extend(struct.pack("<I", user_rw_offset))
    msg.extend(struct.pack("<I", user_rw_size))
    msg.extend(digest)
    if flags & QOS_PROG_FLAG_RELOC_RELATIVE_V1:
        msg.extend(reloc_blob)
    return bytes(msg)


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


if len(sys.argv) < 4 or len(sys.argv) > 11:
    print("Usage: build_program.py input.raw output.bin fat_name_83 [signer_key_id] [sign_key_pem] [openssl_bin] [pq_sign_key_bin] [pq_out_file] [elf_path] [nm_bin]")
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
elf_path = sys.argv[9] if len(sys.argv) >= 10 else ""
nm_bin = sys.argv[10] if len(sys.argv) >= 11 else "aarch64-linux-gnu-nm"

with open(sys.argv[1], "rb") as f:
    code = f.read()

size = len(code)
entry_offset = 0  # _start is at 0

user_rw_offset = size
if elf_path:
    user_rw_offset = parse_nm_symbol(nm_bin, elf_path, "__qos_data_start")
if user_rw_offset >= PROGRAM_MEMORY_BYTES:
    print("Invalid layout: __qos_data_start beyond program memory reservation")
    sys.exit(1)
if (user_rw_offset & (PAGE_SIZE - 1)) != 0:
    print("Invalid layout: __qos_data_start must be page-aligned")
    sys.exit(1)
if entry_offset >= user_rw_offset:
    print("Invalid layout: entry_offset must be inside RX region")
    sys.exit(1)
user_rw_size = PROGRAM_MEMORY_BYTES - user_rw_offset
relocs = extract_relative_relocations(elf_path, PROGRAM_MEMORY_BYTES) if elf_path else []
reloc_blob = pack_relative_relocations(relocs)

header = struct.pack("<III", QOS_MAGIC, size, entry_offset)
digest = hashlib.sha256(code).digest()
flags = QOS_PROG_FLAG_SHA256 | QOS_PROG_FLAG_MEM_LAYOUT_V1
if relocs:
    flags |= QOS_PROG_FLAG_RELOC_RELATIVE_V1

sig_alg = QOS_SIG_ALG_DIGEST_ONLY
sig_len = 0
sig = bytes(QOS_MAX_SIGNATURE_BYTES)
if sign_key_pem:
    msg = build_sig_message(fat_name_83, flags, signer_key_id, QOS_SIG_ALG_ED25519,
                            size, user_rw_offset, user_rw_size, digest, reloc_blob)
    s = sign_ed25519(openssl_bin, sign_key_pem, msg)
    if len(s) != 64:
        print("Ed25519 signature length was not 64 bytes")
        sys.exit(1)
    sig_alg = QOS_SIG_ALG_ED25519
    sig_len = 64
    sig = s

sec_core = struct.pack(
    "<IIIIII32s64s",
    QOS_SEC_MAGIC,
    struct.calcsize("<IIIIII32s64s") + struct.calcsize("<II") + len(reloc_blob),
    flags,
    signer_key_id,
    sig_alg,
    sig_len,
    digest,
    sig,
)
sec_layout = struct.pack("<II", user_rw_offset, user_rw_size)

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(sec_core)
    f.write(sec_layout)
    f.write(reloc_blob)
    f.write(code)

if pq_sign_key:
    msg = build_sig_message(fat_name_83, flags, signer_key_id, sig_alg,
                            size, user_rw_offset, user_rw_size, digest, reloc_blob)
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

print("Built program.bin (size:", size, "reservation:", PROGRAM_MEMORY_BYTES,
      "relocs:", len(relocs), ")")
