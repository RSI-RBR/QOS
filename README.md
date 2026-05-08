# QOS - Quantum OS for Raspberry Pi

Quantum OS is a bare-metal AArch64 operating system for Raspberry Pi, currently
developed and tested primarily on Raspberry Pi 3. It is still a research/dev OS,
but it now has enough kernel, user program, security, networking, and SMP
foundation to run real signed user programs and keep iterating toward larger
applications.

## Current Project Status

QOS currently boots into a signed, scheduled user-mode shell instead of a
permanent privileged kernel shell. The kernel now has:

- EL1 bare-metal boot with UART, framebuffer, timer IRQs, and exception vectors
- preemptive process scheduling with `sleep()`, `exit()`, process cleanup, and secure memory wipe
- SMP bring-up for cores 1-3 with per-core scheduler state and core-aware run queues
- per-process MMU address spaces using TTBR0/ASIDs
- user/kernel separation, user pointer validation, guard pages, W^X user mappings, and TLB shootdown plumbing
- FAT32 program loading from the SD card using signed 8.3 `*.BIN` program files
- capability-gated syscalls based on signer role/scope
- local login and remote encrypted shell login backed by `AUTH.BIN`
- Ed25519 signatures plus ML-DSA-65 post-quantum sidecar signatures for trusted artifacts
- Ethernet networking through the Raspberry Pi 3 LAN9514/SMSC95xx path
- experimental CYW43438 Wi-Fi path for Pi 3 / Pi Zero 2 W style hardware
- ARP, IPv4, ICMP ping, UDP, DNS, minimal TCP, and an experimental HTTPS client path
- kernel socket syscalls for user programs
- a text-mode user web browser demo and SDL-like game scaffold

The biggest recent architectural milestones are SMP scheduling, stronger MMU
isolation, signed/PQ-verified program loading, Argon2id login support, and the
first working Ethernet/Wi-Fi networking paths.

## Current User Programs

- `programs/shell`: signed user shell, local login, program launcher, diagnostics, network commands
- `programs/hello`: cube/demo graphics program, performance instrumentation scaffold
- `programs/webbrowser`: text web browser using DNS/socket/TCP/HTTPS syscalls
- `programs/game`: early game/SDL compatibility scaffold for future ports

Programs are built as position-independent user binaries, wrapped with a QOS
program header, signed with Ed25519, and paired with an ML-DSA-65 `.PQS`
sidecar when PQ signing keys are available.

## Shell Commands

Common shell commands:

```text
help
run
game
web
ps
validate
clear
fbinfo
usbstat
netstat
rloginstat
ip
setip <a.b.c.d>
setgw <a.b.c.d>
ping
dnscheck <domain>
httpget <host> [path]
tlstest
wifiinit
wifiload [fw83 nv83]
wifiup
wifidown
wifistat
wifiver
wifiscan
wifiscanx [passes]
wifijoin <ssid> <password>
```

`run` loads `PROGRAM.BIN`, `web` loads `WEBBROWS.BIN`, and `game` loads
`GAME.BIN` from the FAT32 SD root.

## Security Status

Implemented:

- kernel trust store with admin/developer roles and scope masks
- admin-only signing requirement for `SHELL.BIN` and `WEBBROWS.BIN`
- developer-signed user application support
- required Ed25519 program signatures
- required ML-DSA-65 PQ sidecar signatures for programs
- kernel manifest verification for in-memory and on-SD `KERNEL8.IMG`
- kernel ML-DSA-65 PQ sidecar verification support
- local shell login using `AUTH.BIN`
- Argon2id-compatible password hash format
- encrypted remote login tunnel using X25519 + AES-GCM
- remote login rate limiting, lockout status, and replay sequencing
- boot policy disables shell/remote login if kernel trust is not established

Important limitation:

- Raspberry Pi 3 does not provide a full hardware root-of-trust for this custom
  kernel. Kernel verification is valuable, but true secure boot depends on board
  support or an external boot trust mechanism.

## Networking Status

Implemented:

- Ethernet frame TX/RX through the LAN9514/SMSC95xx USB Ethernet path
- CYW43438 SDIO firmware load/up/version/scan/join path, still experimental
- ARP gateway learning
- IPv4 packet handling
- ICMP gateway ping with RTT display
- UDP send/receive and DNS A-record queries
- minimal TCP connect/send/receive path
- user socket API with blocking/non-blocking and receive timeout support
- experimental HTTPS fetch path using TLS-style crypto building blocks

TLS/HTTPS note:

- TLS record crypto, X25519, AES-GCM, key schedule tests, and HTTPS fetching are present.
- Certificate-chain validation is not complete/enforced yet. CA root sync/copy tooling exists as preparation for that step.
- Current HTTPS key exchange is X25519. ML-DSA-65 is available for artifact signatures, not as a TLS key exchange replacement.

## Known Limitations

- The loader uses FAT 8.3 filenames.
- Process count and user-memory slots are still fixed-size kernel tables.
- Wi-Fi is useful but still a bring-up path, not a polished driver.
- HTTPS works for some sites but does not yet provide browser-grade validation or compatibility.
- No DHCP yet; IP/gateway are configured manually or by current defaults.
- No full USB keyboard/mouse stack yet.
- No GPU acceleration yet; graphics are framebuffer based.
- Pi 5 and Pi Zero 2 W compatibility are planned but not completed.

## Requirements

- Raspberry Pi 3 B/B+ for current main testing
- FAT32 microSD card
- Linux build machine
- `aarch64-linux-gnu` cross toolchain
- OpenSSL
- Python 3
- Python packages for host tooling:
  - `argon2-cffi`
  - `cryptography`

Example Linux setup:

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu openssl python3 python3-venv
python3 -m venv .venv
. .venv/bin/activate
pip install argon2-cffi cryptography
```

## First-Time Key Setup

Generate Ed25519 admin/developer keys:

```bash
bash tools/gen_ed25519_keys.sh keys
```

Generate ML-DSA-65 admin/developer PQ keys:

```bash
python3 tools/gen_mldsa65_keypair.py keys/admin_mldsa65_sk.bin keys/admin_mldsa65_pk.bin
python3 tools/gen_mldsa65_keypair.py keys/dev_mldsa65_sk.bin keys/dev_mldsa65_pk.bin
```

Default key IDs:

- admin: `0x00000001`
- developer: `0x00010001`

Admin scope includes kernel, shell, web, and user apps. Developer scope is for
normal user apps.

## Create Login Password File

Create `AUTH.BIN` with Argon2id password hashing:

```bash
python3 tools/gen_auth_blob.py --username admin --password 'change-me' --out AUTH.BIN
cp AUTH.BIN /media/sd/AUTH.BIN
```

Use your real password instead of `change-me`. The default shell username is
whatever username you place into `AUTH.BIN`; most current testing uses `admin`.

## Build And Copy To SD

Recommended full build:

```bash
bash tools/build_and_copy_sd.sh
```

Defaults:

- SD mount: `/media/sd`
- admin Ed25519 key: `keys/admin_ed25519.pem`
- developer Ed25519 key: `keys/dev_ed25519.pem`
- admin PQ key: `keys/admin_mldsa65_sk.bin`
- developer PQ key: `keys/dev_mldsa65_sk.bin`

Override paths if needed:

```bash
SD_MOUNT=/media/sd \
ADMIN_KEY=keys/admin_ed25519.pem \
DEV_KEY=keys/dev_ed25519.pem \
ADMIN_PQ_SIGN_KEY=keys/admin_mldsa65_sk.bin \
DEV_PQ_SIGN_KEY=keys/dev_mldsa65_sk.bin \
bash tools/build_and_copy_sd.sh
```

The script builds and copies:

- `KERNEL8.IMG`
- `KERNEL8.PQS`
- `SHELL.BIN` / `SHELL.PQS`
- `WEBBROWS.BIN` / `WEBBROWS.PQS`
- `PROGRAM.BIN` / `PROGRAM.PQS`
- `GAME.BIN` / `GAME.PQS`
- signed CA root artifacts if they already exist under `build/ca`

Also copy boot config if needed:

```bash
cp boot/config.txt /media/sd/config.txt
```

## Manual CA Root Sync

CA root sync is intentionally manual. Run it only when you want to refresh the
local root bundle:

```bash
make ca-roots-sync
```

It cross-checks Mozilla NSS, curl, and Debian sources, then writes:

- `build/ca/ca_roots_consensus.pem`
- `build/ca/ca_roots_consensus_report.json`

The build/copy script signs and copies these to SD if present.

## Build A User Program

Use `programs/hello` or `programs/game` as a starting point.

Example developer-signed app:

```bash
make -C programs/hello clean all \
  SIGN_KEY="$(pwd)/keys/dev_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/dev_mldsa65_sk.bin"
cp programs/hello/program.bin /media/sd/PROGRAM.BIN
cp programs/hello/program.pqs /media/sd/PROGRAM.PQS
```

Admin-only examples:

```bash
make -C programs/shell clean all \
  SIGN_KEY="$(pwd)/keys/admin_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/admin_mldsa65_sk.bin"

make -C programs/webbrowser clean all \
  SIGN_KEY="$(pwd)/keys/admin_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/admin_mldsa65_sk.bin"
```

## Remote Login

Remote login uses UDP port `2222`, X25519 key exchange, AES-GCM transport
encryption, and the same `AUTH.BIN` password material as local login.

Example client:

```bash
python3 tools/rlogin_client.py --host 10.0.0.88 --username admin --password 'change-me'
```

Broadcast discovery mode:

```bash
python3 tools/rlogin_client.py --host 255.255.255.255 --broadcast --username admin --password 'change-me'
```

## Wi-Fi Firmware Notes

The current CYW43438 path expects firmware/NVRAM blobs on the SD card using
8.3-compatible names:

- `4343WIFI.BIN`
- `4343NVRM.TXT`

Typical sequence from the shell:

```text
wifiload
wifiup
wifiver
wifiscan
wifijoin "SSID" "password"
ping
```

`wifiinit` is a low-level SDIO probe helper. In normal use, prefer `wifiload`
first because it handles the current SDIO/storage handoff path.

## Roadmap

Near-term priorities:

- finish certificate-chain validation for HTTPS
- harden TCP/socket behavior for real user programs and future servers
- expand process/user memory limits beyond fixed tables
- add DHCP
- improve Wi-Fi reliability and Pi Zero 2 W portability
- add USB keyboard/mouse input
- continue Pi 5 compatibility work
- improve graphics performance and explore acceleration options
- evolve PQ crypto from artifact signatures toward network/session use where practical

## License

All rights reserved.
