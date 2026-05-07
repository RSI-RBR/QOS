#!/usr/bin/env bash
set -euo pipefail

SD_MOUNT="${SD_MOUNT:-/media/sd}"
ADMIN_KEY="${ADMIN_KEY:-keys/admin_ed25519.pem}"
DEV_KEY="${DEV_KEY:-keys/dev_ed25519.pem}"
OPENSSL_BIN="${OPENSSL_BIN:-openssl}"
ADMIN_LAMPORT_PRIV="${ADMIN_LAMPORT_PRIV:-}"
DEV_LAMPORT_PRIV="${DEV_LAMPORT_PRIV:-}"
ADMIN_LAMPORT_PUB="${ADMIN_LAMPORT_PUB:-}"
DEV_LAMPORT_PUB="${DEV_LAMPORT_PUB:-}"

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
if [[ -n "$ADMIN_LAMPORT_PRIV" && ! -f "$ADMIN_LAMPORT_PRIV" ]]; then
  echo "Admin Lamport private key not found: $ADMIN_LAMPORT_PRIV"
  exit 1
fi
if [[ -n "$DEV_LAMPORT_PRIV" && ! -f "$DEV_LAMPORT_PRIV" ]]; then
  echo "Developer Lamport private key not found: $DEV_LAMPORT_PRIV"
  exit 1
fi
if [[ -n "$ADMIN_LAMPORT_PUB" && ! -f "$ADMIN_LAMPORT_PUB" ]]; then
  echo "Admin Lamport public key not found: $ADMIN_LAMPORT_PUB"
  exit 1
fi
if [[ -n "$DEV_LAMPORT_PUB" && ! -f "$DEV_LAMPORT_PUB" ]]; then
  echo "Developer Lamport public key not found: $DEV_LAMPORT_PUB"
  exit 1
fi

ADMIN_KEY_ABS="$(realpath "$ADMIN_KEY")"
DEV_KEY_ABS="$(realpath "$DEV_KEY")"
ADMIN_LAMPORT_PRIV_ABS=""
DEV_LAMPORT_PRIV_ABS=""
ADMIN_LAMPORT_PUB_ABS=""
DEV_LAMPORT_PUB_ABS=""
if [[ -n "$ADMIN_LAMPORT_PRIV" ]]; then ADMIN_LAMPORT_PRIV_ABS="$(realpath "$ADMIN_LAMPORT_PRIV")"; fi
if [[ -n "$DEV_LAMPORT_PRIV" ]]; then DEV_LAMPORT_PRIV_ABS="$(realpath "$DEV_LAMPORT_PRIV")"; fi
if [[ -n "$ADMIN_LAMPORT_PUB" ]]; then ADMIN_LAMPORT_PUB_ABS="$(realpath "$ADMIN_LAMPORT_PUB")"; fi
if [[ -n "$DEV_LAMPORT_PUB" ]]; then DEV_LAMPORT_PUB_ABS="$(realpath "$DEV_LAMPORT_PUB")"; fi

echo "[1/4] Building signed kernel..."
make clean
make OPENSSL_BIN="$OPENSSL_BIN" \
  ADMIN_SIGN_KEY="$ADMIN_KEY_ABS" DEV_SIGN_KEY="$DEV_KEY_ABS" \
  ADMIN_LAMPORT_SIGN_KEY="$ADMIN_LAMPORT_PRIV_ABS" DEV_LAMPORT_SIGN_KEY="$DEV_LAMPORT_PRIV_ABS" \
  ADMIN_LAMPORT_PUB="$ADMIN_LAMPORT_PUB_ABS" DEV_LAMPORT_PUB="$DEV_LAMPORT_PUB_ABS"

echo "[2/4] Building signed programs..."
make -C programs/shell clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$ADMIN_KEY_ABS" PQ_SIGN_KEY="$ADMIN_LAMPORT_PRIV_ABS"
make -C programs/webbrowser clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$ADMIN_KEY_ABS" PQ_SIGN_KEY="$ADMIN_LAMPORT_PRIV_ABS"
make -C programs/hello clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS" PQ_SIGN_KEY="$DEV_LAMPORT_PRIV_ABS"
make -C programs/game clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS" PQ_SIGN_KEY="$DEV_LAMPORT_PRIV_ABS"

echo "[3/4] Copying artifacts to $SD_MOUNT ..."
cp -f kernel8.img "$SD_MOUNT/KERNEL8.IMG"
if [[ -f kernel8.pqs ]]; then cp -f kernel8.pqs "$SD_MOUNT/KERNEL8.PQS"; fi
cp -f programs/shell/shell.bin "$SD_MOUNT/SHELL.BIN"
if [[ -f programs/shell/shell.pqs ]]; then cp -f programs/shell/shell.pqs "$SD_MOUNT/SHELL.PQS"; fi
cp -f programs/webbrowser/webbrowser.bin "$SD_MOUNT/WEBBROWS.BIN"
if [[ -f programs/webbrowser/webbrowser.pqs ]]; then cp -f programs/webbrowser/webbrowser.pqs "$SD_MOUNT/WEBBROWS.PQS"; fi
cp -f programs/hello/program.bin "$SD_MOUNT/PROGRAM.BIN"
if [[ -f programs/hello/program.pqs ]]; then cp -f programs/hello/program.pqs "$SD_MOUNT/PROGRAM.PQS"; fi
cp -f programs/game/game.bin "$SD_MOUNT/GAME.BIN"
if [[ -f programs/game/game.pqs ]]; then cp -f programs/game/game.pqs "$SD_MOUNT/GAME.PQS"; fi

echo "[4/4] Sync..."
sync

echo "Done."
echo "Copied:"
echo "  $SD_MOUNT/KERNEL8.IMG"
if [[ -f "$SD_MOUNT/KERNEL8.PQS" ]]; then echo "  $SD_MOUNT/KERNEL8.PQS"; fi
echo "  $SD_MOUNT/SHELL.BIN"
if [[ -f "$SD_MOUNT/SHELL.PQS" ]]; then echo "  $SD_MOUNT/SHELL.PQS"; fi
echo "  $SD_MOUNT/WEBBROWS.BIN"
if [[ -f "$SD_MOUNT/WEBBROWS.PQS" ]]; then echo "  $SD_MOUNT/WEBBROWS.PQS"; fi
echo "  $SD_MOUNT/PROGRAM.BIN"
if [[ -f "$SD_MOUNT/PROGRAM.PQS" ]]; then echo "  $SD_MOUNT/PROGRAM.PQS"; fi
echo "  $SD_MOUNT/GAME.BIN"
if [[ -f "$SD_MOUNT/GAME.PQS" ]]; then echo "  $SD_MOUNT/GAME.PQS"; fi
