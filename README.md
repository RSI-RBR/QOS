# QOS - Quantum OS for Raspberry Pi

Minimal bare-metal operating system for Raspberry Pi 3 (AArch64).  
**WORK IN PROGRESS**

## Features (Current)

- UART input/output with basic interactive shell
- Simple memory allocator
- SD card driver with FAT32 support (read-only)
- Program loading from FAT filesystem (`PROGRAM.BIN`)
- Basic exception level management (EL2 → EL1)
- Phase 1 MMU identity mapping
- Framebuffer size exposure to shell/programs
- Preemption-safe program loading improvements
- Timer access in EL1

## Current Capabilities & Progress

QOS now successfully boots on Raspberry Pi 3 and provides a functional UART shell. The kernel can read from a FAT32-formatted SD card and load external programs (e.g. `PROGRAM.BIN`).

**Recent Improvements:**
- Stabilized SD block reads and FAT filesystem access
- Better program loading with preemption safety
- Enhanced boot process with EL1 timer support and initial MMU mapping
- Improved reliability of root directory walking and cluster chain following

**Known Limitation (SD Card Timing):**
After cold boot, there can be a brief period (~30-60 seconds) where the SD card has not fully initialized. During this window, attempts to read `PROGRAM.BIN` may fail with "end of chain" or `fat read_cluster fail` errors (e.g. settle error 0x20). Once the card settles, reads work reliably. A short delay + retry logic is being added to address cold-boot timing.

## Requirements

- Raspberry Pi 3 B(+)
- Micro SD card (FAT32 formatted)
- Linux build machine (tested on Kali Linux)
- USB-to-UART adapter (for serial console)

## Build Instructions

### 1. Install Cross Compiler

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

2. Build the OS bash

git clone https://github.com/RSI-RBR/QOS.git
cd QOS
make

This produces kernel8.img. SD Card Preparation(Keep your existing detailed SD card preparation steps here — they remain valid) Copy OS Files bash

cp kernel8.img ~/sdcard/
cp boot/config.txt ~/sdcard/
# Copy your PROGRAM.BIN (and any other programs) to the root of the SD card

config.txt

arm_64bit=1
enable_uart=1
kernel=kernel8.img

Booting Insert SD card into Raspberry Pi 3
Connect UART (TX/RX/GND)
Open serial terminal: screen /dev/ttyUSB0 115200
Power on the Pi

Expected output includes:

init messages and usable shell prompt

You should then be able to interact with the shell and load programs from the SD card.Notes Bare-metal (no Linux)
UART is currently the primary I/O
Framebuffer support is partially exposed but not yet used for graphics output (except for the test program)
SD/FAT driver is functional with the noted cold-boot timing caveat

Future Goals Full process / multitasking system
Robust file system (read + write)
Improved SD card initialization (cold boot reliability)
Networking stack
Security features
GPU / 2D framebuffer acceleration
Raspberry Pi 5 and secure board compatibility

License: All rights reserved

