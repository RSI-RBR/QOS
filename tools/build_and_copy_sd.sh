#!/usr/bin/env bash
set -euo pipefail

SD_MOUNT="${SD_MOUNT:-/media/sd}"
ADMIN_KEY="${ADMIN_KEY:-keys/admin_ed25519.pem}"
DEV_KEY="${DEV_KEY:-keys/dev_ed25519.pem}"
OPENSSL_BIN="${OPENSSL_BIN:-openssl}"

if [[ ! -d "$SD_MOUNT" ]]; then
  echo "SD mount path not found: $SD_MOUNT"
  exit 1
fi
if [[ ! -f "$ADMIN_KEY" ]]; then
  echo "Admin key not found: $ADMIN_KEY"
  exit 1
fi
if [[ ! -f "$DEV_KEY" ]]; then
  echo "Developer key not found: $DEV_KEY"
  exit 1
fi

ADMIN_KEY_ABS="$(realpath "$ADMIN_KEY")"
DEV_KEY_ABS="$(realpath "$DEV_KEY")"

echo "[1/4] Building signed kernel..."
make clean
make OPENSSL_BIN="$OPENSSL_BIN" ADMIN_SIGN_KEY="$ADMIN_KEY_ABS" DEV_SIGN_KEY="$DEV_KEY_ABS"

echo "[2/4] Building signed programs..."
make -C programs/shell clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$ADMIN_KEY_ABS"
make -C programs/webbrowser clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$ADMIN_KEY_ABS"
make -C programs/hello clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS"
make -C programs/game clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS"

echo "[3/4] Copying artifacts to $SD_MOUNT ..."
cp -f kernel8.img "$SD_MOUNT/KERNEL8.IMG"
cp -f programs/shell/shell.bin "$SD_MOUNT/SHELL.BIN"
cp -f programs/webbrowser/webbrowser.bin "$SD_MOUNT/WEBBROWS.BIN"
cp -f programs/hello/program.bin "$SD_MOUNT/PROGRAM.BIN"
cp -f programs/game/game.bin "$SD_MOUNT/GAME.BIN"

echo "[4/4] Sync..."
sync

echo "Done."
echo "Copied:"
echo "  $SD_MOUNT/KERNEL8.IMG"
echo "  $SD_MOUNT/SHELL.BIN"
echo "  $SD_MOUNT/WEBBROWS.BIN"
echo "  $SD_MOUNT/PROGRAM.BIN"
echo "  $SD_MOUNT/GAME.BIN"
