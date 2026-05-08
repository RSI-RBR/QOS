#!/usr/bin/env sh
set -eu

if [ $# -lt 1 ]; then
  echo "Usage: $0 /path/to/PQClean"
  exit 1
fi

SRC_ROOT="$1"
DST_ROOT="third_party/pqclean"

if [ ! -d "$SRC_ROOT" ]; then
  echo "Source PQClean path not found: $SRC_ROOT"
  exit 1
fi

mkdir -p "$DST_ROOT/crypto_kem"

if [ -d "$SRC_ROOT/crypto_kem/ml-kem-768" ]; then
  echo "Importing ml-kem-768..."
  rm -rf "$DST_ROOT/crypto_kem/ml-kem-768"
  cp -a "$SRC_ROOT/crypto_kem/ml-kem-768" "$DST_ROOT/crypto_kem/"
elif [ -d "$SRC_ROOT/crypto_kem/kyber768" ]; then
  echo "Importing kyber768 (compat)..."
  rm -rf "$DST_ROOT/crypto_kem/kyber768"
  cp -a "$SRC_ROOT/crypto_kem/kyber768" "$DST_ROOT/crypto_kem/"
else
  echo "Neither crypto_kem/ml-kem-768 nor crypto_kem/kyber768 found in: $SRC_ROOT"
  exit 1
fi

echo "Import complete."
