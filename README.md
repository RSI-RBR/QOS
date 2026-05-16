# QOS - Quantum OS for Raspberry Pi

Quantum OS is a bare-metal AArch64 operating system for Raspberry Pi. It is
currently developed and tested primarily on Raspberry Pi 3, with Raspberry Pi
Zero 2 W and Raspberry Pi 5 support planned.

The project is still a research/development OS, but it now has a real kernel
foundation: preemptive SMP scheduling, per-process MMU isolation, signed user
program loading, local and remote login, basic graphics/session management,
Ethernet, experimental Wi-Fi, and a growing TCP/HTTPS stack.

## Board Targets

QOS now has a board-target layer so Raspberry Pi variants can diverge without
forking the kernel. Raspberry Pi 3 remains the default target.

Current target files:

- `configs/boards/pi3.mk`: current tested Raspberry Pi 3 target.
- `configs/boards/pi_zero2w.mk`: BCM2837-based Pi Zero 2 W scaffold.
- `configs/boards/pi5.mk`: placeholder for the future BCM2712/RP1 Pi 5 port.
- `boards/pi3/` and `boards/pi_zero2w/`: board capability/setup hooks.
- `soc/bcm2837/`: shared SoC identity layer for Pi 3 and Pi Zero 2 W.

Build examples:

```bash
make BOARD=pi3
make BOARD=pi_zero2w
```

The normal SD helper also accepts `BOARD`:

```bash
BOARD=pi3 bash tools/build_and_copy_sd.sh
BOARD=pi_zero2w bash tools/build_and_copy_sd.sh
```

Pi 3 and Pi Zero 2 W currently share the BCM2837 MMIO map through
`include/platform/mmio.h`. Pi 5 is intentionally blocked until its separate
BCM2712/RP1 platform code is implemented.

`BOARD=pi5` is scaffold-only for now. The Makefile target reserves the future
shape, but the SD helper intentionally refuses Pi 5 builds until BCM2712/RP1
MMIO, boot filenames, and kernel-file verification are ported.

Networking defaults are now board-aware: Pi 3 probes USB LAN9514/SMSC95xx
Ethernet first, while Pi Zero 2 W can switch to CYW43 Wi-Fi when firmware is
loaded and joined. Both targets still fall back to the loopback stub if no NIC
is ready.

## Current Capabilities

Kernel and process model:

- EL1 bare-metal boot with UART, framebuffer, timer IRQs, exception vectors, and SMP core bring-up.
- Scheduled user-mode shell instead of a permanent privileged kernel shell.
- Preemptive process scheduling with `sleep()`, `exit()`, cleanup, and secure memory wipe.
- Cores 1-3 are released and used by the per-core scheduler.
- Per-core run queues, sleep/wakeup handling, simple load balancing, and IPI reschedule pokes.
- Fixed process table today: `MAX_PROCESSES=8`.

MMU and isolation:

- Per-process TTBR0 address spaces with ASIDs.
- Kernel mappings are EL1-only; EL0 gets only its own process slot.
- Program code is user RX.
- Program data, stack, and heap are user RW + NX.
- W^X is enforced for user mappings.
- User stack guard pages are unmapped.
- User pointer validation is used for syscalls through `copy_from_user`, `copy_to_user`, and bounded C-string copies.
- SMP TLB shootdown plumbing exists for page table changes.

Display, terminal, and input:

- `tty0` is the shell text terminal.
- Graphics apps get separate `gfxN` sessions.
- HDMI shows only the active session.
- Shell output can mirror to UART, HDMI, or both with `termout`.
- USB HID keyboard input works, including Alt+Left/Alt+Right session switching.
- UART remains useful as a debug console.
- Remote shell input is routed through terminal plumbing after authentication.

Program loading:

- FAT32 loader uses 8.3 filenames from the SD card.
- Programs are QOS-wrapped binaries with a security header.
- Program signatures are required.
- Program memory is wiped on allocation/free.
- Program slots are fixed today: 8 slots, 2 MiB each, from a 16 MiB user pool.

Networking:

- LAN9514/SMSC95xx Ethernet path on Raspberry Pi 3.
- Experimental CYW43438 SDIO Wi-Fi path.
- ARP, IPv4, ICMP ping, UDP, DNS, TCP, and socket syscalls.
- DNS supports normal UDP and a secure-DNS path where available.
- HTTPS fetch path exists in the kernel socket layer.
- User programs can use a BSD-like socket API from `include/syscall.h`.

Security and crypto:

- Admin/developer trust store compiled into the kernel.
- Admin-signed kernel, shell, browser, and privileged artifacts.
- Developer-signed user application support.
- Signed `AUTH.BIN` login password file with Ed25519 + ML-DSA-65.
- Ed25519 signatures for kernel/program artifacts.
- ML-DSA-65 post-quantum sidecar signatures for programs.
- Required kernel ML-DSA-65 sidecar verification for the manifest and SD kernel file.
- ML-KEM-768 + X25519 hybrid HTTPS key exchange support.
- ML-DSA signature algorithms are advertised for TLS where supported by servers.
- AES-GCM, SHA-256, HKDF, X25519, ML-KEM-768, ML-DSA-65, RSA verify, and ECDSA verify are present.
- Argon2id-compatible local/remote password file format.
- Remote login uses encrypted transport, replay sequencing, rate limiting, and lockout.
- Stack canaries and panic-on-detected-corruption are enabled for kernel builds.

## User Programs

Current programs:

- `programs/shell`: signed user shell, login, launcher, diagnostics, network commands, session commands.
- `programs/hello`: cube/demo graphics program and performance test scaffold.
- `programs/webbrowser`: text-mode web browser using DNS/socket/TCP/HTTPS syscalls.
- `programs/game`: optional QOS wrapper/Makefile for a private local game build; missing private source/assets are skipped so public QOS builds still work.

SD filenames:

- `SHELL.BIN` and `SHELL.PQS`
- `WEBBROWS.BIN` and `WEBBROWS.PQS`
- `PROGRAM.BIN` and `PROGRAM.PQS`
- `GAME.BIN` and `GAME.PQS` when a private local game build is present.

## Shell Commands

Core commands:

```text
help
run
runbg
exit [pid]
gfx <pid>
game
web
tty
chvt <0-3>
termout both|uart|hdmi|status
dma on|off|status
securitylog
ps
validate
clear
fbinfo
usbstat
```

Networking commands:

```text
netstat
rloginstat
ip
setip <a.b.c.d>
setgw <a.b.c.d>
ping
dnscheck <domain>
httpget <host> [path]
tlstest
```

Wi-Fi commands:

```text
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

Graphics/session behavior:

- `run` loads `PROGRAM.BIN`, creates a graphics session, and switches HDMI to it.
- `runbg` loads `PROGRAM.BIN` without switching away from `tty0`.
- `game` loads `GAME.BIN` when you copy/build a private game locally.
- `web` loads `WEBBROWS.BIN`.
- `chvt 0` switches to the shell text terminal.
- `chvt 1`, `chvt 2`, and `chvt 3` switch to graphics sessions.
- `gfx <pid>` switches to the graphics session owned by a PID.
- `exit` kills the foreground graphics process from the shell.
- `exit <pid>` kills a specific process.
- USB Alt+Left and Alt+Right cycle display sessions.

## Web Browser

Start the browser:

```text
web
```

Browser commands:

```text
help
open <url|host> [path]
gzip on|off|status
exit
```

Examples:

```text
open https://example.com/
open https://en.wikipedia.org/wiki/Main_Page
gzip status
exit
```

HTTPS behavior:

- Port 443 sockets are upgraded through the kernel HTTPS/TLS path.
- X.509 hostname and chain-anchor validation are implemented.
- CA roots are loaded from signed `CA_ROOTS.PEM` artifacts on the SD card.
- Certificate chain signatures are verified with RSA/ECDSA backends.
- The client prefers hybrid `X25519+ML-KEM-768` when the server supports it.
- The client advertises ML-DSA signatures, but most public sites still negotiate classic RSA/ECDSA signatures.
- Browser output strips HTML tags and supports gzip decoding.

## Build Requirements

Build host:

- Linux
- `aarch64-linux-gnu` cross toolchain
- OpenSSL
- Python 3
- Python virtual environment recommended
- Python packages: `argon2-cffi`, `cryptography`

Example:

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu openssl python3 python3-venv
python3 -m venv .venv
. .venv/bin/activate
pip install argon2-cffi cryptography
```

## Key Setup

Generate Ed25519 admin/developer keys:

```bash
bash tools/gen_ed25519_keys.sh keys
```

Generate ML-DSA-65 admin/developer keys:

```bash
python3 tools/gen_mldsa65_keypair.py keys/admin_mldsa65_sk.bin keys/admin_mldsa65_pk.bin
python3 tools/gen_mldsa65_keypair.py keys/dev_mldsa65_sk.bin keys/dev_mldsa65_pk.bin
```

Default key IDs:

- admin: `0x00000001`
- developer: `0x00010001`

Admin scope includes kernel, shell, web, and user apps. Developer scope is for
normal user apps.

## Login Password File

Create `AUTH.BIN` with Argon2id:

```bash
python3 tools/gen_auth_blob.py --username admin --password 'change-me' --out AUTH.BIN
```

Use a real password instead of `change-me`. The username is whatever you place
in `AUTH.BIN`; current testing usually uses `admin`.

The full build/copy script signs and copies `AUTH.BIN` automatically when
`AUTH.BIN` exists in the repository root. By default it uses the developer key
so a device user can set their own password without the admin key:

```bash
bash tools/build_and_copy_sd.sh
```

This copies:

- `AUTH.BIN`
- `AUTH.SIG`
- `AUTH.PQS`

Override the auth signer if needed:

```bash
AUTH_SIGNER_KEY_ID=0x00000001 \
AUTH_SIGN_KEY=keys/admin_ed25519.pem \
AUTH_PQ_SIGN_KEY=keys/admin_mldsa65_sk.bin \
bash tools/build_and_copy_sd.sh
```

Important: only device-owner/developer keys should receive auth scope. Do not
grant auth scope to third-party app developer keys.

## CA Root Sync

CA root sync is intentionally manual. Run it only when you want to refresh the
local CA bundle:

```bash
make ca-roots-sync
```

This writes:

- `build/ca/ca_roots_consensus.pem`
- `build/ca/ca_roots_consensus_report.json`

The build/copy script signs and copies these artifacts if they exist.

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

Required kernel trust artifacts copied by the script:

- `KERNEL8.IMG`
- `KERNEL8.PQS`
- `KERNFILE.SIG`
- `KERNFILE.PQS`

Override paths:

```bash
SD_MOUNT=/media/sd \
ADMIN_KEY=keys/admin_ed25519.pem \
DEV_KEY=keys/dev_ed25519.pem \
ADMIN_PQ_SIGN_KEY=keys/admin_mldsa65_sk.bin \
DEV_PQ_SIGN_KEY=keys/dev_mldsa65_sk.bin \
bash tools/build_and_copy_sd.sh
```

Copy boot config if needed:

```bash
cp boot/config.txt /media/sd/config.txt
```

## Build A User Program

Use `programs/hello` as the public example. `programs/game/` keeps a tracked QOS wrapper, but its private source/assets are ignored so commercial game code can live there without being pushed to the public QOS repo. If private game sources are absent, `make -C programs/game` and `tools/build_and_copy_sd.sh` skip `GAME.BIN` instead of failing the OS build.

Developer-signed app:

```bash
make -C programs/hello clean all \
  SIGN_KEY="$(pwd)/keys/dev_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/dev_mldsa65_sk.bin"
cp programs/hello/program.bin /media/sd/PROGRAM.BIN
cp programs/hello/program.pqs /media/sd/PROGRAM.PQS
```

Private game wrapper:

```bash
mkdir -p programs/game/src programs/game/img
cp /path/to/QuantumFront2D/src/* programs/game/src/
cp /path/to/QuantumFront2D/img/* programs/game/img/

make -C programs/game clean all \
  SIGN_KEY="$(pwd)/keys/dev_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/dev_mldsa65_sk.bin"

cp programs/game/game.bin /media/sd/GAME.BIN
cp programs/game/game.pqs /media/sd/GAME.PQS
```

Full public SD builds do not require these private files. The build/copy script removes stale `GAME.BIN`/`GAME.PQS` from the SD card when no fresh game binary is produced.

The game wrapper force-includes QOS stdio compatibility:

- `printf(...)` writes through the active QOS terminal.
- `fprintf(stderr, ...)` writes through the same safe path for now.
- `QOS_LOG(...)` and `QOS_ERR(...)` are available for new QOS-specific code.
- `snprintf(...)` is provided for asset path construction.
- `FILE*` calls are compatibility stubs for now: developer-only text files read
  as EOF, and `/dev/urandom` returns lightweight pseudo-random bytes.
- `qos_isqrt_u64()` is available for deterministic fixed-point distance math.
- `sqrt(double)` exists as a small compatibility wrapper while the game is being
  migrated away from desktop floating-point hot paths.
- Floating-point format specifiers are consumed safely but currently print as
  `<float>`; keep important numeric gameplay diagnostics integer-based until the
  formatter grows real float conversion.

Admin-only programs:

```bash
make -C programs/shell clean all \
  SIGN_KEY="$(pwd)/keys/admin_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/admin_mldsa65_sk.bin"

make -C programs/webbrowser clean all \
  SIGN_KEY="$(pwd)/keys/admin_ed25519.pem" \
  PQ_SIGN_KEY="$(pwd)/keys/admin_mldsa65_sk.bin"
```

## Remote Login

Remote login uses UDP port `2222`.

Current properties:

- password material comes from `AUTH.BIN`
- Argon2id is supported
- X25519, ML-KEM-768, and hybrid ML-KEM-768+X25519 modes are supported
- AES-GCM protects the remote shell stream after the key exchange
- replay window, 5 second failure delay, and lockout are implemented

Example:

```bash
python3 tools/rlogin_client.py --host 10.0.0.88 --username admin --password 'change-me'
```

Discovery example:

```bash
python3 tools/rlogin_client.py --host 255.255.255.255 --broadcast --username admin --password 'change-me'
```

## Wi-Fi Firmware Notes

The CYW43438 path expects 8.3-compatible firmware/NVRAM names on the SD card:

- `4343WIFI.BIN`
- `4343NVRM.TXT`

For Pi Zero 2 W testing, keep the filenames short but use the Pi Zero 2 W
firmware/NVRAM contents. Example SD names:

- `P0WIFI36.BIN`
- `P0WIFI36.TXT`

Load that pair with raw FAT 8.3 names:

```text
wifiload P0WIFI36BIN P0WIFI36TXT
```

If scan/join is unreliable, try the alternate Pi Zero 2 W firmware variant
with another short pair such as `P0WIFI3S.BIN` / `P0WIFI3S.TXT`.

Typical sequence:

```text
wifiload
wifiup
wifiver
wifiscan
wifiscanfor "SSID"
wifijoin "SSID" "password"
ping
```

`wifiinit` is a low-level SDIO probe helper. For normal use, prefer `wifiload`
first because it handles the current SDIO/storage handoff path.

## Security Status And Remaining Risks

Implemented security foundations:

- signed kernel/program trust model with role and scope checks
- required program Ed25519 signatures
- required program ML-DSA-65 sidecar signatures
- required kernel ML-DSA-65 sidecars for manifest and file verification
- admin-only shell/browser signing
- capability-gated syscalls tied to signer scope
- per-process MMU address spaces and ASIDs
- strict EL0/EL1 memory separation
- W^X user pages
- user stack guard pages
- syscall user-pointer validation
- stack canaries and panic-on-corruption
- signed `AUTH.BIN` for local and remote password login
- remote-login replay/rate-limit/lockout controls
- signed CA root bundle for HTTPS validation
- X.509 hostname, anchor, and chain-signature checks

Important remaining security work:

- Raspberry Pi 3 does not provide a complete secure-boot root of trust for this custom kernel.
- `AUTH.BIN` is signed and PQ-signed, but the default auth signer is `dev-main` for owner-controlled password changes.
- CA root bundle PQ sidecar verification is supported, but the PQ sidecar is optional today.
- The entropy source is still early-stage and should be replaced or strengthened with hardware/jitter/persistent entropy before relying on secrets.
- Remote login still needs explicit long-term server identity authentication or a real PAKE-style protocol to resist active MITM/offline guessing risks.
- X.509 revocation checking is local/optional; there is no live OCSP/CRL fetch policy yet.
- Certificate validation time falls back to build time unless explicitly set; a trusted clock/NTP path is still needed.
- TLS, X.509, DNS, gzip, FAT, Wi-Fi, and USB parsers are hand-rolled and should be fuzzed heavily.
- Networking global state is still mostly single-stack/single-connection oriented and should be further locked/audited for SMP.
- `wifijoin <ssid> <password>` accepts the password on the visible command line; a hidden prompt should replace it.
- Debug/status prints can leak kernel/program addresses and should be reduced for release builds.
- Process, socket, display, and memory tables are fixed-size and need resource quotas/DoS policy.
- DMA is experimental and should remain off unless a specific path is verified.

## Roadmap

Near-term priorities:

- decide whether auth signing should use a separate owner key instead of `dev-main`
- strengthen entropy collection
- add trusted time and stricter certificate revocation policy
- harden/fuzz TLS, X.509, DNS, FAT, gzip, USB, and Wi-Fi parsers
- expand process/user memory limits beyond fixed tables
- add DHCP
- improve Wi-Fi reliability and Pi Zero 2 W portability
- add USB mouse support
- continue Pi 5 compatibility work
- improve graphics performance and session/window ergonomics
- evolve PQ crypto from artifact signatures and hybrid KEX toward broader network/session use where interoperable

## License

All rights reserved.
