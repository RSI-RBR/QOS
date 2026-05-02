# QOS - Quantum OS for Raspberry Pi

Minimal bare-metal operating system for Raspberry Pi 3 (AArch64).  
Work in progress.

## Current Status

QOS now boots reliably to a user-mode shell process and supports preemptive multitasking, dynamic program loading, MMU-based isolation foundations, and early networking with user-mode socket APIs.

## Current Capabilities

- AArch64 bare-metal boot to EL1
- UART console and interactive user shell
- Preemptive scheduler (timer IRQ driven)
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

## Progress Notes

- Shell is a scheduled user process (not a permanent privileged loop).
- Scheduler now stays responsive during long operations (program load / network waits).
- TTY ownership handoff was added so foreground apps (like webbrowser) can exclusively read input and return cleanly to shell.

## Known Limitations

- FAT loader is currently 8.3 filename based.
- Socket `SOCK_STREAM` path is currently minimal and HTTP-oriented for demo usage.
- Networking is early-stage and not yet full POSIX-like behavior.
- No TLS yet.

## Requirements

- Raspberry Pi 3 B/B+
- FAT32 microSD card
- Linux build machine
- USB-UART adapter for serial console

## Build

### 1) Install Cross Toolchain

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

### 2) Build Kernel

```bash
git clone https://github.com/RSI-RBR/QOS.git
cd QOS
make
```

Build output: `kernel8.img`

## SD Card Setup

Copy files to the SD card boot/root FAT partition:

```bash
cp kernel8.img /path/to/sd/
cp boot/config.txt /path/to/sd/
```

Copy user programs (`*.BIN`) to SD root as needed (8.3 names for loader lookup).

Example `config.txt`:

```txt
arm_64bit=1
enable_uart=1
kernel=kernel8.img
```

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

## Roadmap Direction (Near Term)

- Make stream sockets fully generic (not HTTP-specialized)
- Complete TCP socket semantics for user apps
- Continue syscall surface hardening (reduce low-level direct net calls)
- Expand user-mode networking clients

## License

All rights reserved.

