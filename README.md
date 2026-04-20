QOS (Quantum OS for Raspberry Pi)

Minimal bare-metal operating system for Raspberry Pi (AArch64).

---

## Features

- UART input/output
- Basic interactive shell
- Simple memory allocator
- Runs without Linux (bare metal)

---

## Requirements

- Raspberry Pi 3 B(+)
- SD card (8GB+ recommended)
- Linux system (tested on Kali Linux / Debian-based)
- USB-to-UART adapter (for serial output)

---

1. Install Cross Compiler

Install the AArch64 cross compiler:

sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

Verify installation:

aarch64-linux-gnu-gcc --version

---

2. Build the OS

Clone the repository:

git clone https://github.com/RSI-RBR/QOS.git
cd QOS

Build:

make

This produces:

kernel8.img

---

3. Prepare SD Card

⚠️ WARNING: This will erase the SD card.

3.1 Insert SD card and identify it

lsblk

Look for your device (example):

sdb
└─sdb1

We will assume "/dev/sdX" (replace with your actual device).

---

3.2 Unmount existing partitions

sudo umount /dev/sdX*

---

3.3 Create partition table

sudo fdisk /dev/sdX

Inside fdisk:

o    (create new DOS partition table)
n    (new partition)
p    (primary)
1    (partition number)
     (press Enter for default start)
     (press Enter for default end)
t    (change type)
c    (W95 FAT32 (LBA))
w    (write and exit)

---

3.4 Format as FAT32

sudo mkfs.vfat /dev/sdX1

---

3.5 Mount the SD card

mkdir -p ~/sdcard
sudo mount /dev/sdX1 ~/sdcard

---

4. Download Raspberry Pi Firmware (Minimal)

Instead of cloning the full firmware repository, download only the required boot files:

mkdir -p ~/rpi-fw
cd ~/rpi-fw

wget https://github.com/raspberrypi/firmware/raw/master/boot/bootcode.bin
wget https://github.com/raspberrypi/firmware/raw/master/boot/start.elf
wget https://github.com/raspberrypi/firmware/raw/master/boot/fixup.dat

Copy them to the SD card:

cp bootcode.bin ~/sdcard/
cp start.elf ~/sdcard/
cp fixup.dat ~/sdcard/

---

5. Copy OS Files

Copy your kernel and config:

cp kernel8.img ~/sdcard/
cp boot/config.txt ~/sdcard/

---

6. config.txt

Example configuration:

arm_64bit=1
enable_uart=1
kernel=kernel8.img

---

7. Unmount SD Card

sync
sudo umount ~/sdcard

---

8. Boot

1. Insert SD card into Raspberry Pi
2. Connect UART (TX/RX/GND)
3. Open serial:

screen /dev/ttyUSB0 115200

4. Power on the Pi

You should see:

Kernel booted
Simple Shell Ready
>

---

Notes

- This OS is bare metal (no Linux)
- No GPU / framebuffer yet
- UART is the only I/O

---

Future Goals

- Process system
- File system
- Networking
- Security features
- GPU acceleration (2D)

---

License

All available rights reserved
