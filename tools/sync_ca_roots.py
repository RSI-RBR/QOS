#!/usr/bin/env python3
"""
Build a consensus root CA bundle from three upstream sources:
1) Mozilla NSS certdata.txt (release branch)
2) curl CA extract bundle
3) Debian ca-certificates binary package bundle

Output:
- PEM bundle containing certs present in >= N sources (default N=2).
- JSON report with source counts and fingerprints.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import gzip
import hashlib
import io
import json
import lzma
import os
import re
import tarfile
import urllib.parse
import urllib.request
from typing import Dict, Iterable, List, Optional, Set, Tuple

MOZILLA_CERTDATA_URL = (
    "https://hg.mozilla.org/releases/mozilla-release/raw-file/default/"
    "security/nss/lib/ckfw/builtins/certdata.txt"
)
CURL_CA_BUNDLE_URL = "https://curl.se/ca/cacert.pem"
DEBIAN_POOL_INDEX_URL = "https://ftp.debian.org/debian/pool/main/c/ca-certificates/"
DEBIAN_DEB_NAME_RE = r"ca-certificates_([^\"<> ]+)_all\.deb"


def fetch_url_bytes(url: str, timeout_s: int) -> bytes:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "QOS-CA-Sync/1.0"},
    )
    with urllib.request.urlopen(req, timeout=timeout_s) as resp:
        return resp.read()


def decode_multiline_octal(lines: Iterable[str]) -> bytes:
    out = bytearray()
    for line in lines:
        i = 0
        s = line.strip()
        while i < len(s):
            if s[i] == "\\" and i + 3 < len(s):
                a = s[i + 1]
                b = s[i + 2]
                c = s[i + 3]
                if a.isdigit() and b.isdigit() and c.isdigit():
                    out.append(int(a + b + c, 8))
                    i += 4
                    continue
            i += 1
    return bytes(out)


def parse_blacklist_labels(text: str) -> Set[str]:
    labels: Set[str] = set()
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith('"') and line.endswith('"') and len(line) >= 2:
            labels.add(line[1:-1])
    return labels


def parse_nss_certdata_server_auth_roots(certdata_text: str, blacklist: Set[str]) -> Dict[str, bytes]:
    lines = certdata_text.splitlines()
    cert_by_label: Dict[str, bytes] = {}
    trust_by_label: Dict[str, str] = {}

    cur_class: Optional[str] = None
    cur_label: Optional[str] = None
    cur_value: Optional[bytes] = None
    cur_server_auth: Optional[str] = None

    def flush_current() -> None:
        nonlocal cur_class, cur_label, cur_value, cur_server_auth
        if cur_class == "CKO_CERTIFICATE" and cur_label and cur_value:
            cert_by_label[cur_label] = cur_value
        elif cur_class == "CKO_NSS_TRUST" and cur_label and cur_server_auth:
            trust_by_label[cur_label] = cur_server_auth
        cur_class = None
        cur_label = None
        cur_value = None
        cur_server_auth = None

    i = 0
    while i < len(lines):
        raw = lines[i]
        line = raw.strip()
        i += 1

        if not line:
            flush_current()
            continue
        if line.startswith("#"):
            continue

        if line.startswith("CKA_CLASS CK_OBJECT_CLASS "):
            tail = line[len("CKA_CLASS CK_OBJECT_CLASS "):].strip()
            cur_class = tail
            continue

        if line.startswith("CKA_LABEL UTF8 "):
            m = re.search(r'"(.*)"', line)
            if m:
                cur_label = m.group(1)
            continue

        if line == "CKA_VALUE MULTILINE_OCTAL":
            blob_lines: List[str] = []
            while i < len(lines):
                t = lines[i].strip()
                i += 1
                if t == "END":
                    break
                blob_lines.append(t)
            cur_value = decode_multiline_octal(blob_lines)
            continue

        if line.startswith("CKA_TRUST_SERVER_AUTH CK_TRUST "):
            cur_server_auth = line.split()[-1]
            continue

    flush_current()

    roots: Dict[str, bytes] = {}
    for label, der in cert_by_label.items():
        if label in blacklist:
            continue
        trust = trust_by_label.get(label, "")
        if trust != "CKT_NSS_TRUSTED_DELEGATOR":
            continue
        roots[label] = der
    return roots


def parse_pem_bundle_to_der_map(pem_bytes: bytes) -> Dict[str, bytes]:
    text = pem_bytes.decode("utf-8", errors="ignore")
    out: Dict[str, bytes] = {}
    pattern = re.compile(
        r"-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----",
        flags=re.DOTALL,
    )
    for m in pattern.finditer(text):
        b64 = re.sub(r"\s+", "", m.group(1))
        if not b64:
            continue
        der = base64.b64decode(b64)
        fp = hashlib.sha256(der).hexdigest()
        out[fp] = der
    return out


def der_to_pem(der: bytes) -> str:
    b64 = base64.b64encode(der).decode("ascii")
    lines = [b64[i:i + 64] for i in range(0, len(b64), 64)]
    return "-----BEGIN CERTIFICATE-----\n" + "\n".join(lines) + "\n-----END CERTIFICATE-----\n"


def deb_version_key(version: str) -> Tuple[Tuple[int, object], ...]:
    # Good-enough ordering for the date-like versions used by ca-certificates.
    parts = re.split(r"([0-9]+)", version)
    key: List[Tuple[int, object]] = []
    for part in parts:
        if not part:
            continue
        if part.isdigit():
            key.append((1, int(part)))
        else:
            key.append((0, part.replace("~", "\x00")))
    return tuple(key)


def find_latest_debian_deb_name(index_html: str) -> str:
    versions = re.findall(DEBIAN_DEB_NAME_RE, index_html)
    if not versions:
        raise RuntimeError("unable to find Debian ca-certificates .deb versions")
    latest = max(versions, key=deb_version_key)
    return f"ca-certificates_{latest}_all.deb"


def ar_extract_member(archive_bytes: bytes, wanted_names: Set[str]) -> bytes:
    if len(archive_bytes) < 8 or archive_bytes[:8] != b"!<arch>\n":
        raise RuntimeError("invalid ar archive header")

    off = 8
    while off + 60 <= len(archive_bytes):
        hdr = archive_bytes[off:off + 60]
        off += 60

        name = hdr[0:16].decode("ascii", errors="ignore").strip()
        size_s = hdr[48:58].decode("ascii", errors="ignore").strip()
        magic = hdr[58:60]
        if magic != b"`\n":
            raise RuntimeError("invalid ar member header")
        try:
            size = int(size_s)
        except ValueError as exc:
            raise RuntimeError("invalid ar member size") from exc

        if off + size > len(archive_bytes):
            raise RuntimeError("truncated ar member")
        data = archive_bytes[off:off + size]
        off += size
        if off & 1:
            off += 1

        norm_name = name.rstrip("/")
        if norm_name in wanted_names:
            return data

    raise RuntimeError(f"ar member not found: {sorted(wanted_names)}")


def extract_debian_ca_bundle_from_deb(deb_bytes: bytes) -> bytes:
    data_tar = ar_extract_member(
        deb_bytes,
        {"data.tar.xz", "data.tar.gz", "data.tar.zst"},
    )

    # Debian currently ships xz; gz support is included for resilience.
    if data_tar[:6] == b"\xfd7zXZ\x00":
        tar_bytes = lzma.decompress(data_tar)
    elif data_tar[:2] == b"\x1f\x8b":
        tar_bytes = gzip.decompress(data_tar)
    else:
        raise RuntimeError("unsupported Debian data.tar compression (expected xz/gz)")

    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as tf:
        names = set(tf.getnames())

        # Some package variants may already include the assembled bundle.
        for member_name in ("etc/ssl/certs/ca-certificates.crt", "./etc/ssl/certs/ca-certificates.crt"):
            if member_name in names:
                member = tf.getmember(member_name)
                f = tf.extractfile(member)
                if f:
                    return f.read()

        # Fallback: construct Debian bundle from packaged Mozilla CRT files.
        # In many Debian builds the final /etc/ssl/certs/ca-certificates.crt
        # is generated at install time by update-ca-certificates.
        crt_members = []
        for name in names:
            if not name.endswith(".crt"):
                continue
            norm = name[2:] if name.startswith("./") else name
            if norm.startswith("usr/share/ca-certificates/mozilla/"):
                crt_members.append(name)

        if not crt_members:
            raise RuntimeError(
                "Debian package missing both prebuilt ca-certificates.crt and "
                "usr/share/ca-certificates/mozilla/*.crt"
            )

        bundle_parts: List[bytes] = []
        for name in sorted(crt_members):
            member = tf.getmember(name)
            f = tf.extractfile(member)
            if not f:
                continue
            data = f.read()
            if not data:
                continue
            bundle_parts.append(data.rstrip() + b"\n")

        if not bundle_parts:
            raise RuntimeError("no readable Debian CRT members found")

        return b"\n".join(bundle_parts)


def build_consensus_bundle(
    min_sources: int,
    source_maps: Dict[str, Dict[str, bytes]],
) -> Tuple[List[str], Dict[str, int], Dict[str, bytes]]:
    fp_counts: Dict[str, int] = {}
    fp_to_der: Dict[str, bytes] = {}
    for src_name, certs in source_maps.items():
        _ = src_name
        for fp, der in certs.items():
            fp_counts[fp] = fp_counts.get(fp, 0) + 1
            if fp not in fp_to_der:
                fp_to_der[fp] = der

    selected = sorted(fp for fp, c in fp_counts.items() if c >= min_sources)
    return selected, fp_counts, fp_to_der


def write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main() -> int:
    ap = argparse.ArgumentParser(description="Sync CA roots from Mozilla + curl + Debian.")
    ap.add_argument("--out-pem", default="build/ca/ca_roots_consensus.pem")
    ap.add_argument("--out-report", default="build/ca/ca_roots_consensus_report.json")
    ap.add_argument("--min-sources", type=int, default=2, choices=[1, 2, 3])
    ap.add_argument("--timeout", type=int, default=25, help="HTTP timeout in seconds")
    args = ap.parse_args()

    timeout_s = int(args.timeout)
    if timeout_s <= 0:
        raise RuntimeError("timeout must be > 0")

    # 1) Mozilla certdata (roots trusted for SERVER_AUTH)
    mozilla_certdata = fetch_url_bytes(MOZILLA_CERTDATA_URL, timeout_s).decode("utf-8", errors="replace")
    mozilla_roots = parse_nss_certdata_server_auth_roots(mozilla_certdata, blacklist=set())
    mozilla_map: Dict[str, bytes] = {}
    for _, der in mozilla_roots.items():
        mozilla_map[hashlib.sha256(der).hexdigest()] = der

    # 2) curl CA extract (PEM bundle)
    curl_pem = fetch_url_bytes(CURL_CA_BUNDLE_URL, timeout_s)
    curl_map = parse_pem_bundle_to_der_map(curl_pem)

    # 3) Debian package bundle (ca-certificates.crt from latest .deb in pool)
    deb_index = fetch_url_bytes(DEBIAN_POOL_INDEX_URL, timeout_s).decode("utf-8", errors="replace")
    deb_name = find_latest_debian_deb_name(deb_index)
    deb_url = urllib.parse.urljoin(DEBIAN_POOL_INDEX_URL, deb_name)
    deb_bytes = fetch_url_bytes(deb_url, timeout_s)
    deb_pem = extract_debian_ca_bundle_from_deb(deb_bytes)
    deb_map = parse_pem_bundle_to_der_map(deb_pem)

    source_maps = {
        "mozilla_nss": mozilla_map,
        "curl_extract": curl_map,
        "debian_bundle": deb_map,
    }
    selected, fp_counts, fp_to_der = build_consensus_bundle(args.min_sources, source_maps)

    header = [
        "## QOS CA Consensus Bundle",
        "## Sources:",
        f"## - Mozilla NSS certdata: {MOZILLA_CERTDATA_URL}",
        f"## - curl CA extract: {CURL_CA_BUNDLE_URL}",
        f"## - Debian bundle: {deb_url}",
        f"## Policy: include cert if present in >= {args.min_sources} source(s)",
        f"## Generated: {dt.datetime.now(dt.timezone.utc).isoformat()}",
        "",
    ]
    pem_parts = ["\n".join(header)]
    for fp in selected:
        pem_parts.append(f"## sha256={fp}")
        pem_parts.append(der_to_pem(fp_to_der[fp]).rstrip("\n"))
        pem_parts.append("")
    write_text(args.out_pem, "\n".join(pem_parts))

    report = {
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "policy_min_sources": args.min_sources,
        "sources": {
            "mozilla_nss_url": MOZILLA_CERTDATA_URL,
            "curl_extract_url": CURL_CA_BUNDLE_URL,
            "debian_bundle_url": deb_url,
        },
        "counts": {
            "mozilla_nss": len(mozilla_map),
            "curl_extract": len(curl_map),
            "debian_bundle": len(deb_map),
            "selected": len(selected),
        },
        "selected_fingerprints_sha256": selected,
        "fingerprint_presence_count": {fp: fp_counts[fp] for fp in selected},
    }
    write_text(args.out_report, json.dumps(report, indent=2, sort_keys=True) + "\n")

    print(f"Wrote PEM: {args.out_pem}")
    print(f"Wrote report: {args.out_report}")
    print(
        "Source counts: "
        f"mozilla={len(mozilla_map)} "
        f"curl={len(curl_map)} "
        f"debian={len(deb_map)} "
        f"selected={len(selected)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
