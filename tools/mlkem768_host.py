#!/usr/bin/env python3
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import List, Tuple


PK_BYTES = 1184
SK_BYTES = 2400
CT_BYTES = 1088
SS_BYTES = 32


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _tool_path() -> Path:
    name = "mlkem768_tool.exe" if os.name == "nt" else "mlkem768_tool"
    return _repo_root() / "tools" / name


def _backend_info() -> Tuple[Path, str]:
    root = _repo_root()
    mlkem = root / "third_party" / "pqclean" / "crypto_kem" / "ml-kem-768" / "clean"
    if mlkem.exists():
        return mlkem, "QOS_HAVE_PQCLEAN_MLKEM768"
    kyber = root / "third_party" / "pqclean" / "crypto_kem" / "kyber768" / "clean"
    if kyber.exists():
        return kyber, "QOS_HAVE_PQCLEAN_KYBER768"
    raise RuntimeError("No PQClean ML-KEM-768/Kyber768 clean backend found")


def _source_list() -> List[Path]:
    root = _repo_root()
    clean_dir, _ = _backend_info()
    srcs = [
        root / "tools" / "mlkem768_tool.c",
        root / "third_party" / "pqclean" / "common" / "fips202.c",
        root / "third_party" / "pqclean" / "common" / "randombytes.c",
    ]
    srcs.extend(sorted(clean_dir.glob("*.c")))
    return srcs


def _needs_rebuild(bin_path: Path, srcs: List[Path]) -> bool:
    if not bin_path.exists():
        return True
    bin_mtime = bin_path.stat().st_mtime
    for s in srcs:
        if s.stat().st_mtime > bin_mtime:
            return True
    return False


def ensure_tool() -> Path:
    root = _repo_root()
    bin_path = _tool_path()
    srcs = _source_list()
    clean_dir, backend_define = _backend_info()
    if not _needs_rebuild(bin_path, srcs):
        return bin_path

    cc = os.environ.get("HOST_CC")
    if not cc:
        cc = "gcc" if shutil.which("gcc") else "cc"

    include_common = root / "third_party" / "pqclean" / "common"
    cmd = [
        cc,
        "-O2",
        "-std=c99",
        "-Wall",
        "-Wextra",
        f"-D{backend_define}",
        "-I",
        str(include_common),
        "-I",
        str(clean_dir),
        "-o",
        str(bin_path),
    ] + [str(s) for s in srcs]

    subprocess.check_call(cmd, cwd=str(root))
    return bin_path


def keygen(pub_path: str, priv_path: str) -> None:
    tool = ensure_tool()
    subprocess.check_call([str(tool), "keygen", pub_path, priv_path])


def keypair_bytes() -> Tuple[bytes, bytes]:
    with tempfile.TemporaryDirectory(prefix="qos_mlkem_keygen_") as td:
        pub_path = Path(td) / "mlkem_pub.bin"
        priv_path = Path(td) / "mlkem_priv.bin"
        keygen(str(pub_path), str(priv_path))
        return pub_path.read_bytes(), priv_path.read_bytes()


def encaps(pub_path: str) -> Tuple[bytes, bytes]:
    tool = ensure_tool()
    with tempfile.TemporaryDirectory(prefix="qos_mlkem_enc_") as td:
        ct_path = Path(td) / "ct.bin"
        ss_path = Path(td) / "ss.bin"
        subprocess.check_call([str(tool), "encap", pub_path, str(ct_path), str(ss_path)])
        return ct_path.read_bytes(), ss_path.read_bytes()


def decaps(priv_path: str, ciphertext: bytes) -> bytes:
    tool = ensure_tool()
    with tempfile.TemporaryDirectory(prefix="qos_mlkem_dec_") as td:
        ct_path = Path(td) / "ct.bin"
        ss_path = Path(td) / "ss.bin"
        ct_path.write_bytes(ciphertext)
        subprocess.check_call([str(tool), "decap", priv_path, str(ct_path), str(ss_path)])
        return ss_path.read_bytes()


def decaps_with_sk(priv_key: bytes, ciphertext: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="qos_mlkem_dec_mem_") as td:
        sk_path = Path(td) / "mlkem_priv.bin"
        ct_path = Path(td) / "ct.bin"
        ss_path = Path(td) / "ss.bin"
        sk_path.write_bytes(priv_key)
        ct_path.write_bytes(ciphertext)
        tool = ensure_tool()
        subprocess.check_call([str(tool), "decap", str(sk_path), str(ct_path), str(ss_path)])
        return ss_path.read_bytes()
