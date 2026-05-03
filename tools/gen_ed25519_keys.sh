#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-keys}"
mkdir -p "$OUT_DIR"

if ! command -v openssl >/dev/null 2>&1; then
  echo "openssl not found"
  exit 1
fi

if [[ ! -f "$OUT_DIR/admin_ed25519.pem" ]]; then
  openssl genpkey -algorithm Ed25519 -out "$OUT_DIR/admin_ed25519.pem"
  echo "Generated $OUT_DIR/admin_ed25519.pem"
fi

if [[ ! -f "$OUT_DIR/dev_ed25519.pem" ]]; then
  openssl genpkey -algorithm Ed25519 -out "$OUT_DIR/dev_ed25519.pem"
  echo "Generated $OUT_DIR/dev_ed25519.pem"
fi

openssl pkey -in "$OUT_DIR/admin_ed25519.pem" -pubout -out "$OUT_DIR/admin_ed25519.pub.pem"
openssl pkey -in "$OUT_DIR/dev_ed25519.pem" -pubout -out "$OUT_DIR/dev_ed25519.pub.pem"

echo "Key generation complete."
echo "Admin private: $OUT_DIR/admin_ed25519.pem"
echo "Admin public:  $OUT_DIR/admin_ed25519.pub.pem"
echo "Dev private:   $OUT_DIR/dev_ed25519.pem"
echo "Dev public:    $OUT_DIR/dev_ed25519.pub.pem"

