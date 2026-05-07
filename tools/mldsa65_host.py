#!/usr/bin/env python3
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import List


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _tool_path() -> Path:
    name = "mldsa65_tool.exe" if os.name == "nt" else "mldsa65_tool"
    return _repo_root() / "tools" / name


def _source_list() -> List[Path]:
    root = _repo_root()
    return [
        root / "tools" / "mldsa65_tool.c",
        root / "third_party" / "pqclean" / "common" / "fips202.c",
        root / "third_party" / "pqclean" / "common" / "randombytes.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "ntt.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "packing.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "poly.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "polyvec.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "reduce.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "rounding.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "sign.c",
        root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean" / "symmetric-shake.c",
    ]


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
    if not _needs_rebuild(bin_path, srcs):
        return bin_path

    cc = os.environ.get("HOST_CC")
    if not cc:
        cc = "gcc" if shutil.which("gcc") else "cc"
    include_common = root / "third_party" / "pqclean" / "common"
    include_clean = root / "third_party" / "pqclean" / "crypto_sign" / "ml-dsa-65" / "clean"

    cmd = [
        cc,
        "-O2",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-I",
        str(include_common),
        "-I",
        str(include_clean),
        "-o",
        str(bin_path),
    ] + [str(s) for s in srcs]

    subprocess.check_call(cmd, cwd=str(root))
    return bin_path


def keygen(pub_path: str, priv_path: str) -> None:
    tool = ensure_tool()
    subprocess.check_call([str(tool), "keygen", pub_path, priv_path])


def sign(priv_path: str, message: bytes) -> bytes:
    tool = ensure_tool()
    with tempfile.TemporaryDirectory(prefix="qos_mldsa_sign_") as td:
        msg_path = Path(td) / "msg.bin"
        sig_path = Path(td) / "sig.bin"
        msg_path.write_bytes(message)
        subprocess.check_call([str(tool), "sign", priv_path, str(msg_path), str(sig_path)])
        return sig_path.read_bytes()


def verify(pub_path: str, message: bytes, signature: bytes) -> bool:
    tool = ensure_tool()
    with tempfile.TemporaryDirectory(prefix="qos_mldsa_verify_") as td:
        msg_path = Path(td) / "msg.bin"
        sig_path = Path(td) / "sig.bin"
        msg_path.write_bytes(message)
        sig_path.write_bytes(signature)
        rc = subprocess.call([str(tool), "verify", pub_path, str(msg_path), str(sig_path)])
        return rc == 0
