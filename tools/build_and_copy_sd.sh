#!/usr/bin/env bash
set -euo pipefail

SD_MOUNT="${SD_MOUNT:-/media/sd}"
ADMIN_KEY="${ADMIN_KEY:-keys/admin_ed25519.pem}"
DEV_KEY="${DEV_KEY:-keys/dev_ed25519.pem}"
OPENSSL_BIN="${OPENSSL_BIN:-openssl}"
ADMIN_PQ_SIGN_KEY="${ADMIN_PQ_SIGN_KEY:-${ADMIN_LAMPORT_PRIV:-keys/admin_mldsa65_sk.bin}}"
DEV_PQ_SIGN_KEY="${DEV_PQ_SIGN_KEY:-${DEV_LAMPORT_PRIV:-keys/dev_mldsa65_sk.bin}}"
ADMIN_PQ_PUB="${ADMIN_PQ_PUB:-${ADMIN_LAMPORT_PUB:-keys/admin_mldsa65_pk.bin}}"
DEV_PQ_PUB="${DEV_PQ_PUB:-${DEV_LAMPORT_PUB:-keys/dev_mldsa65_pk.bin}}"
CA_BUNDLE_SRC="${CA_BUNDLE_SRC:-build/ca/ca_roots_consensus.pem}"
CA_REPORT_SRC="${CA_REPORT_SRC:-build/ca/ca_roots_consensus_report.json}"
CA_SIGNER_KEY_ID="${CA_SIGNER_KEY_ID:-0x1}"
AUTH_SRC="${AUTH_SRC:-AUTH.BIN}"
AUTH_SIGNER_KEY_ID="${AUTH_SIGNER_KEY_ID:-0x00010001}"
AUTH_SIGN_KEY="${AUTH_SIGN_KEY:-}"
AUTH_PQ_SIGN_KEY="${AUTH_PQ_SIGN_KEY:-}"
KERNEL_FILE_SIGNER_KEY_ID="${KERNEL_FILE_SIGNER_KEY_ID:-0x1}"
QF2D_ROOT="${QF2D_ROOT:-}"
GAME_SRC_DIR="${GAME_SRC_DIR:-}"
GAME_IMG_DIR="${GAME_IMG_DIR:-}"
GAME_BUILD_SRC_DIR=""

TMP_CA_DIR=""
cleanup_tmp_ca() {
  if [[ -n "$TMP_CA_DIR" && -d "$TMP_CA_DIR" ]]; then
    rm -rf "$TMP_CA_DIR"
  fi
}
trap cleanup_tmp_ca EXIT

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
if [[ -n "$ADMIN_PQ_SIGN_KEY" && ! -f "$ADMIN_PQ_SIGN_KEY" ]]; then
  echo "Admin PQ sign key not found: $ADMIN_PQ_SIGN_KEY"
  exit 1
fi
if [[ -n "$DEV_PQ_SIGN_KEY" && ! -f "$DEV_PQ_SIGN_KEY" ]]; then
  echo "Developer PQ sign key not found: $DEV_PQ_SIGN_KEY"
  exit 1
fi
if [[ -n "$ADMIN_PQ_PUB" && ! -f "$ADMIN_PQ_PUB" ]]; then
  echo "Admin PQ public key not found: $ADMIN_PQ_PUB"
  exit 1
fi
if [[ -n "$DEV_PQ_PUB" && ! -f "$DEV_PQ_PUB" ]]; then
  echo "Developer PQ public key not found: $DEV_PQ_PUB"
  exit 1
fi
if [[ -z "$AUTH_SIGN_KEY" ]]; then
  AUTH_SIGN_KEY="$DEV_KEY"
fi
if [[ -z "$AUTH_PQ_SIGN_KEY" ]]; then
  AUTH_PQ_SIGN_KEY="$DEV_PQ_SIGN_KEY"
fi
if [[ -f "$AUTH_SRC" ]]; then
  if [[ ! -f "$AUTH_SIGN_KEY" ]]; then
    echo "AUTH signing key not found: $AUTH_SIGN_KEY"
    exit 1
  fi
  if [[ -z "$AUTH_PQ_SIGN_KEY" || ! -f "$AUTH_PQ_SIGN_KEY" ]]; then
    echo "AUTH PQ signing key not found: $AUTH_PQ_SIGN_KEY"
    exit 1
  fi
fi
if [[ -n "$QF2D_ROOT" ]]; then
  if [[ ! -d "$QF2D_ROOT" ]]; then
    echo "QF2D_ROOT not found: $QF2D_ROOT"
    exit 1
  fi
  if [[ -z "$GAME_SRC_DIR" ]]; then
    GAME_SRC_DIR="$QF2D_ROOT/src"
  fi
  if [[ -z "$GAME_IMG_DIR" ]]; then
    GAME_IMG_DIR="$QF2D_ROOT/img"
  fi
fi
if [[ -n "$GAME_SRC_DIR" && ! -d "$GAME_SRC_DIR" ]]; then
  echo "GAME_SRC_DIR not found: $GAME_SRC_DIR"
  exit 1
fi
if [[ -n "$GAME_IMG_DIR" && ! -d "$GAME_IMG_DIR" ]]; then
  echo "GAME_IMG_DIR not found: $GAME_IMG_DIR"
  exit 1
fi

# Preserve CA artifacts across "make clean" (which removes build/ by default).
if [[ -f "$CA_BUNDLE_SRC" || -f "$CA_REPORT_SRC" ]]; then
  TMP_CA_DIR="$(mktemp -d -t qos-ca-XXXXXX)"
  if [[ -f "$CA_BUNDLE_SRC" ]]; then
    cp -f "$CA_BUNDLE_SRC" "$TMP_CA_DIR/ca_roots_consensus.pem"
    CA_BUNDLE_SRC="$TMP_CA_DIR/ca_roots_consensus.pem"
  fi
  if [[ -f "$CA_REPORT_SRC" ]]; then
    cp -f "$CA_REPORT_SRC" "$TMP_CA_DIR/ca_roots_consensus_report.json"
    CA_REPORT_SRC="$TMP_CA_DIR/ca_roots_consensus_report.json"
  fi
fi

ADMIN_KEY_ABS="$(realpath "$ADMIN_KEY")"
DEV_KEY_ABS="$(realpath "$DEV_KEY")"
ADMIN_PQ_SIGN_KEY_ABS=""
DEV_PQ_SIGN_KEY_ABS=""
ADMIN_PQ_PUB_ABS=""
DEV_PQ_PUB_ABS=""
AUTH_SIGN_KEY_ABS=""
AUTH_PQ_SIGN_KEY_ABS=""
if [[ -n "$ADMIN_PQ_SIGN_KEY" ]]; then ADMIN_PQ_SIGN_KEY_ABS="$(realpath "$ADMIN_PQ_SIGN_KEY")"; fi
if [[ -n "$DEV_PQ_SIGN_KEY" ]]; then DEV_PQ_SIGN_KEY_ABS="$(realpath "$DEV_PQ_SIGN_KEY")"; fi
if [[ -n "$ADMIN_PQ_PUB" ]]; then ADMIN_PQ_PUB_ABS="$(realpath "$ADMIN_PQ_PUB")"; fi
if [[ -n "$DEV_PQ_PUB" ]]; then DEV_PQ_PUB_ABS="$(realpath "$DEV_PQ_PUB")"; fi
if [[ -n "$AUTH_SIGN_KEY" ]]; then AUTH_SIGN_KEY_ABS="$(realpath "$AUTH_SIGN_KEY")"; fi
if [[ -n "$AUTH_PQ_SIGN_KEY" ]]; then AUTH_PQ_SIGN_KEY_ABS="$(realpath "$AUTH_PQ_SIGN_KEY")"; fi

echo "[1/5] Building signed kernel..."
make clean
make OPENSSL_BIN="$OPENSSL_BIN" \
  ADMIN_SIGN_KEY="$ADMIN_KEY_ABS" DEV_SIGN_KEY="$DEV_KEY_ABS" \
  ADMIN_PQ_SIGN_KEY="$ADMIN_PQ_SIGN_KEY_ABS" DEV_PQ_SIGN_KEY="$DEV_PQ_SIGN_KEY_ABS" \
  ADMIN_PQ_PUB="$ADMIN_PQ_PUB_ABS" DEV_PQ_PUB="$DEV_PQ_PUB_ABS"

echo "[2/5] Building signed programs..."
make -C programs/shell clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$ADMIN_KEY_ABS" PQ_SIGN_KEY="$ADMIN_PQ_SIGN_KEY_ABS"
make -C programs/webbrowser clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$ADMIN_KEY_ABS" PQ_SIGN_KEY="$ADMIN_PQ_SIGN_KEY_ABS"
make -C programs/hello clean all OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS" PQ_SIGN_KEY="$DEV_PQ_SIGN_KEY_ABS"
if [[ -d programs/game && -f programs/game/Makefile ]]; then
  if [[ -n "$GAME_SRC_DIR" ]]; then
    GAME_BUILD_SRC_DIR="build/private_game_src"
    rm -rf "$GAME_BUILD_SRC_DIR"
    mkdir -p "$GAME_BUILD_SRC_DIR/src"
    cp -a "$GAME_SRC_DIR"/. "$GAME_BUILD_SRC_DIR/src/"
    make -C programs/game clean all GAME_SRC_DIR="../../$GAME_BUILD_SRC_DIR/src" GAME_ENTRY="${GAME_ENTRY:-gui_main.c}" OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS" PQ_SIGN_KEY="$DEV_PQ_SIGN_KEY_ABS"
  else
    make -C programs/game clean all GAME_ENTRY="${GAME_ENTRY:-gui_main.c}" OPENSSL_BIN="$OPENSSL_BIN" SIGN_KEY="$DEV_KEY_ABS" PQ_SIGN_KEY="$DEV_PQ_SIGN_KEY_ABS"
  fi
else
  echo "programs/game not present; skipping private game build."
fi

echo "[3/5] Signing data artifacts..."
mkdir -p build
KERNEL_FILE_SIG="build/kernel8.file.sig"
KERNEL_FILE_PQS="build/kernel8.file.pqs"
python3 tools/sign_detached_artifact.py \
  kernel8.img "$KERNEL_FILE_SIG" \
  "$KERNEL_FILE_SIGNER_KEY_ID" "$ADMIN_KEY_ABS" "$OPENSSL_BIN" \
  "$ADMIN_PQ_SIGN_KEY_ABS" "$KERNEL_FILE_PQS" "KERNEL8_IMG"

AUTH_SIG=""
AUTH_PQS=""
if [[ -f "$AUTH_SRC" ]]; then
  AUTH_SIG="build/auth.sig"
  AUTH_PQS="build/auth.pqs"
  python3 tools/sign_detached_artifact.py \
    "$AUTH_SRC" "$AUTH_SIG" \
    "$AUTH_SIGNER_KEY_ID" "$AUTH_SIGN_KEY_ABS" "$OPENSSL_BIN" \
    "$AUTH_PQ_SIGN_KEY_ABS" "$AUTH_PQS" "AUTH_BIN"
else
  echo "AUTH.BIN not found; skipping AUTH signing/copy: $AUTH_SRC"
fi

echo "      Signing CA bundle artifacts (if present)..."
CA_BUNDLE_SIG=""
CA_BUNDLE_PQS=""
CA_REPORT_SIG=""
CA_REPORT_PQS=""
if [[ -f "$CA_BUNDLE_SRC" ]]; then
  CA_BUNDLE_SIG="${CA_BUNDLE_SRC}.sig"
  CA_BUNDLE_PQS="${CA_BUNDLE_SRC}.pqs"
  python3 tools/sign_detached_artifact.py \
    "$CA_BUNDLE_SRC" "$CA_BUNDLE_SIG" \
    "$CA_SIGNER_KEY_ID" "$ADMIN_KEY_ABS" "$OPENSSL_BIN" \
    "$ADMIN_PQ_SIGN_KEY_ABS" "$CA_BUNDLE_PQS" "CA_ROOTS_PEM"

  if [[ -f "$CA_REPORT_SRC" ]]; then
    CA_REPORT_SIG="${CA_REPORT_SRC}.sig"
    CA_REPORT_PQS="${CA_REPORT_SRC}.pqs"
    python3 tools/sign_detached_artifact.py \
      "$CA_REPORT_SRC" "$CA_REPORT_SIG" \
      "$CA_SIGNER_KEY_ID" "$ADMIN_KEY_ABS" "$OPENSSL_BIN" \
      "$ADMIN_PQ_SIGN_KEY_ABS" "$CA_REPORT_PQS" "CA_ROOTS_REPORT"
  else
    echo "CA report not found; skipping report signing: $CA_REPORT_SRC"
  fi
else
  echo "CA bundle not found; skipping CA signing/copy: $CA_BUNDLE_SRC"
fi

echo "[4/5] Copying artifacts to $SD_MOUNT ..."
cp -f kernel8.img "$SD_MOUNT/KERNEL8.IMG"
if [[ -f kernel8.pqs ]]; then cp -f kernel8.pqs "$SD_MOUNT/KERNEL8.PQS"; fi
cp -f "$KERNEL_FILE_SIG" "$SD_MOUNT/KERNFILE.SIG"
cp -f "$KERNEL_FILE_PQS" "$SD_MOUNT/KERNFILE.PQS"
cp -f programs/shell/shell.bin "$SD_MOUNT/SHELL.BIN"
if [[ -f programs/shell/shell.pqs ]]; then cp -f programs/shell/shell.pqs "$SD_MOUNT/SHELL.PQS"; fi
cp -f programs/webbrowser/webbrowser.bin "$SD_MOUNT/WEBBROWS.BIN"
if [[ -f programs/webbrowser/webbrowser.pqs ]]; then cp -f programs/webbrowser/webbrowser.pqs "$SD_MOUNT/WEBBROWS.PQS"; fi
cp -f programs/hello/program.bin "$SD_MOUNT/PROGRAM.BIN"
if [[ -f programs/hello/program.pqs ]]; then cp -f programs/hello/program.pqs "$SD_MOUNT/PROGRAM.PQS"; fi
if [[ -f programs/game/game.bin ]]; then
  cp -f programs/game/game.bin "$SD_MOUNT/GAME.BIN"
  if [[ -f programs/game/game.pqs ]]; then cp -f programs/game/game.pqs "$SD_MOUNT/GAME.PQS"; fi
  if [[ -n "$GAME_IMG_DIR" && -d "$GAME_IMG_DIR" ]]; then
    mkdir -p "$SD_MOUNT/QF2D/IMG"
    cp -a "$GAME_IMG_DIR"/. "$SD_MOUNT/QF2D/IMG/"
  fi
else
  echo "Private game binary not found; removing stale GAME.BIN/GAME.PQS from SD."
  rm -f "$SD_MOUNT/GAME.BIN" "$SD_MOUNT/GAME.PQS"
fi
if [[ -f "$AUTH_SRC" && -n "$AUTH_SIG" && -f "$AUTH_SIG" && -n "$AUTH_PQS" && -f "$AUTH_PQS" ]]; then
  cp -f "$AUTH_SRC" "$SD_MOUNT/AUTH.BIN"
  cp -f "$AUTH_SIG" "$SD_MOUNT/AUTH.SIG"
  cp -f "$AUTH_PQS" "$SD_MOUNT/AUTH.PQS"
fi
if [[ -f "$CA_BUNDLE_SRC" && -n "$CA_BUNDLE_SIG" && -f "$CA_BUNDLE_SIG" ]]; then
  cp -f "$CA_BUNDLE_SRC" "$SD_MOUNT/CA_ROOTS.PEM"
  cp -f "$CA_BUNDLE_SIG" "$SD_MOUNT/CA_ROOTS.SIG"
  if [[ -n "$CA_BUNDLE_PQS" && -f "$CA_BUNDLE_PQS" ]]; then cp -f "$CA_BUNDLE_PQS" "$SD_MOUNT/CA_ROOTS.PQS"; fi
elif [[ -f "$CA_BUNDLE_SRC" ]]; then
  echo "CA bundle exists but is unsigned; not copying: $CA_BUNDLE_SRC"
fi
if [[ -f "$CA_REPORT_SRC" && -n "$CA_REPORT_SIG" && -f "$CA_REPORT_SIG" ]]; then
  cp -f "$CA_REPORT_SRC" "$SD_MOUNT/CA_RPT.JSN"
  cp -f "$CA_REPORT_SIG" "$SD_MOUNT/CA_RPT.SIG"
  if [[ -n "$CA_REPORT_PQS" && -f "$CA_REPORT_PQS" ]]; then cp -f "$CA_REPORT_PQS" "$SD_MOUNT/CA_RPT.PQS"; fi
elif [[ -f "$CA_REPORT_SRC" ]]; then
  echo "CA report exists but is unsigned; not copying: $CA_REPORT_SRC"
fi

echo "[5/5] Sync..."
sync

echo "Done."
echo "Copied:"
echo "  $SD_MOUNT/KERNEL8.IMG"
if [[ -f "$SD_MOUNT/KERNEL8.PQS" ]]; then echo "  $SD_MOUNT/KERNEL8.PQS"; fi
if [[ -f "$SD_MOUNT/KERNFILE.SIG" ]]; then echo "  $SD_MOUNT/KERNFILE.SIG"; fi
if [[ -f "$SD_MOUNT/KERNFILE.PQS" ]]; then echo "  $SD_MOUNT/KERNFILE.PQS"; fi
echo "  $SD_MOUNT/SHELL.BIN"
if [[ -f "$SD_MOUNT/SHELL.PQS" ]]; then echo "  $SD_MOUNT/SHELL.PQS"; fi
echo "  $SD_MOUNT/WEBBROWS.BIN"
if [[ -f "$SD_MOUNT/WEBBROWS.PQS" ]]; then echo "  $SD_MOUNT/WEBBROWS.PQS"; fi
echo "  $SD_MOUNT/PROGRAM.BIN"
if [[ -f "$SD_MOUNT/PROGRAM.PQS" ]]; then echo "  $SD_MOUNT/PROGRAM.PQS"; fi
if [[ -f "$SD_MOUNT/GAME.BIN" ]]; then
  echo "  $SD_MOUNT/GAME.BIN"
  if [[ -f "$SD_MOUNT/GAME.PQS" ]]; then echo "  $SD_MOUNT/GAME.PQS"; fi
fi
if [[ -f "$SD_MOUNT/AUTH.BIN" ]]; then
  echo "  $SD_MOUNT/AUTH.BIN"
  if [[ -f "$SD_MOUNT/AUTH.SIG" ]]; then echo "  $SD_MOUNT/AUTH.SIG"; fi
  if [[ -f "$SD_MOUNT/AUTH.PQS" ]]; then echo "  $SD_MOUNT/AUTH.PQS"; fi
fi
if [[ -f "$SD_MOUNT/CA_ROOTS.PEM" ]]; then
  echo "  $SD_MOUNT/CA_ROOTS.PEM"
  if [[ -f "$SD_MOUNT/CA_ROOTS.SIG" ]]; then echo "  $SD_MOUNT/CA_ROOTS.SIG"; fi
  if [[ -f "$SD_MOUNT/CA_ROOTS.PQS" ]]; then echo "  $SD_MOUNT/CA_ROOTS.PQS"; fi
fi
if [[ -f "$SD_MOUNT/CA_RPT.JSN" ]]; then
  echo "  $SD_MOUNT/CA_RPT.JSN"
  if [[ -f "$SD_MOUNT/CA_RPT.SIG" ]]; then echo "  $SD_MOUNT/CA_RPT.SIG"; fi
  if [[ -f "$SD_MOUNT/CA_RPT.PQS" ]]; then echo "  $SD_MOUNT/CA_RPT.PQS"; fi
fi
