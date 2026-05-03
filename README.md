# QOS - Quantum OS for Raspberry Pi

Minimal bare-metal operating system for Raspberry Pi 3 (AArch64).  
Work in progress.

## Current Status

QOS boots to a user-mode shell process and now includes:
- preemptive multitasking (timer IRQ)
- SMP bring-up and per-core scheduling foundations
- MMU-based user/kernel isolation foundations
- dynamic FAT32 program loading
- early networking stack + user socket syscalls
- strict Ed25519 signature verification policy for kernel/program trust flow

## Current Capabilities

- AArch64 bare-metal boot to EL1
- UART console and interactive user shell
- Preemptive scheduler (timer IRQ driven)
- SMP core bring-up (multi-core online)
- Process model with PID/state/exit/cleanup
- External program loading from FAT32 (`*.BIN`, 8.3 naming)
- Secure process teardown and memory wipe on exit
- Phase-1 MMU enabled with kernel/user mapping foundations
- Framebuffer init and drawing syscalls
- USB host + LAN9514/SMSC95xx networking path
- IPv4/ARP/ICMP/UDP basics
- Socket syscall scaffold for user programs:
  - `socket`, `connect`, `send`, `recv`, `close`
  - blocking/non-blocking mode
  - recv timeout configuration
- User-mode DNS over UDP via socket syscalls
- User-mode text web browser demo (`programs/webbrowser`)
- Trust framework:
  - kernel trust store with role/scope policy
  - admin-only enforcement for `SHELL.BIN` and `WEBBROWS.BIN`
  - developer-signed user program support
- Signature verification:
  - Ed25519 verification integrated in kernel
  - kernel manifest verification (memory + storage image checks)
  - program manifest verification during load

## Progress Notes

- Shell is a scheduled user process (not a permanent privileged loop).
- Scheduler now stays responsive during long operations (program load / network waits).
- TTY ownership handoff was added so foreground apps (like webbrowser) can exclusively read input and return cleanly to shell.
- Build/sign pipeline now supports embedding trusted public keys and signing artifacts with OpenSSL Ed25519 keys.

## Known Limitations

- FAT loader is currently 8.3 filename based.
- Socket `SOCK_STREAM` path is currently minimal and HTTP-oriented for demo usage.
- Networking is early-stage and not yet full POSIX-like behavior.
- No TLS yet.
- Secure boot root-of-trust is still board/bootloader dependent (Pi 3 limitation).

## Requirements

- Raspberry Pi 3 B/B+
- FAT32 microSD card
- Linux build machine
- USB-UART adapter for serial console
- OpenSSL (for Ed25519 signing)

## Build

### 1) Install Cross Toolchain

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu openssl
```

### 2) Generate Signing Keys (First Time)

```bash
bash tools/gen_ed25519_keys.sh keys
```

This creates:
- `keys/admin_ed25519.pem`
- `keys/dev_ed25519.pem`

### 3) Build Signed Kernel + Signed Programs + Copy to SD (Recommended)

```bash
bash tools/build_and_copy_sd.sh
```

Defaults used by the script:
- SD mount: `/media/sd`
- Admin key: `keys/admin_ed25519.pem`
- Dev key: `keys/dev_ed25519.pem`

Override if needed:
```bash
SD_MOUNT=/media/sd ADMIN_KEY=keys/admin_ed25519.pem DEV_KEY=keys/dev_ed25519.pem bash tools/build_and_copy_sd.sh
```

### 4) Manual Build (Advanced)

```bash
git clone https://github.com/RSI-RBR/QOS.git
cd QOS
make clean
make ADMIN_SIGN_KEY=keys/admin_ed25519.pem DEV_SIGN_KEY=keys/dev_ed25519.pem
```

Build output: `kernel8.img`

## SD Card Setup

Copy files to the SD card boot/root FAT partition:

```bash
cp kernel8.img /path/to/sd/
cp boot/config.txt /path/to/sd/
```

Copy user programs (`*.BIN`) to SD root as needed (8.3 names for loader lookup):
- `SHELL.BIN`
- `WEBBROWS.BIN`
- `PROGRAM.BIN`
- `GAME.BIN`

## Boot

1. Insert SD card into Pi
2. Connect UART (`TX/RX/GND`)
3. Open serial terminal:

```bash
screen /dev/ttyUSB0 115200
```

4. Power on the Pi

## Shell / Program Notes

- Default shell command: `run` loads default `PROGRAM.BIN`
- Web browser demo command: `web` (expects `WEBBROWS.BIN` on SD root)
- Game demo command: `game` (expects `GAME.BIN`)

## Build Your Own Signed Program

### 1) Create Program

Use `programs/hello` or `programs/game` as a template.

### 2) Pick a Key + Key ID

Current built-in trust table includes:
- Admin key ID: `0x00000001` (admin scope: kernel/shell/web/user)
- Dev key ID: `0x00010001` (user-app scope)

### 3) Build and Sign

Example using dev key:
```bash
make -C programs/hello clean all SIGN_KEY=../../keys/dev_ed25519.pem
```

This generates a signed `PROGRAM.BIN`.

### 4) Copy to SD

```bash
cp programs/hello/program.bin /media/sd/PROGRAM.BIN
```

### 5) Run

Boot QOS and run:
```text
UQOS> run
```

## Adding Another Developer Key

Current implementation keeps the trust table in kernel source.

To add another developer:
1. Add a new key entry in `kernel/trust.c` with a new `key_id`, `TRUST_ROLE_DEVELOPER`, and `TRUST_SCOPE_USER_APP`.
2. Extend `include/trust_keys_autogen.h` (or key generator) with that user’s Ed25519 public key bytes.
3. Rebuild kernel with updated trust keys.
4. Have that developer sign program manifests with their private key and matching key ID.

## Roadmap Direction (Near Term)

- Expand trust-key management tooling (more than admin/dev slots)
- Replace static trust-key slots with scalable key manifest ingestion
- Continue network stack hardening and protocol completeness
- Add post-quantum signature path alongside Ed25519 (hybrid transition)

## License

All rights reserved.

