# NamelessOS

myOS is an experimental x86_64 operating system built from scratch in C. It is a learning project for exploring boot protocols, memory mapping, hardware drivers, filesystems, and networking without building on an existing kernel such as Linux or BSD.

It boots with [Limine](https://limine-bootloader.org/), runs in QEMU, and has also been used for bring-up work on real x86_64 hardware.

> [!WARNING]
> myOS is early-stage kernel software. Expect incomplete drivers, missing safety features, hardware-specific behavior, and breaking changes. Do not use it with data or hardware you cannot afford to lose.

## Current state

### Kernel and console

- Freestanding 64-bit C kernel using the Limine boot protocol
- BIOS and UEFI bootable ISO generation
- COM1 serial output mirrored to a 32-bit framebuffer text console
- PS/2 keyboard input, plus USB HID boot-keyboard input through the xHCI driver
- Limine memory-map and higher-half direct-map handling
- Page-table walking and on-demand 4 KiB MMIO mappings
- 16 MiB aligned bump allocator
- PCI bus enumeration, BAR discovery, power-state handling, and bus mastering
- ACPI DMAR discovery and early VT-d/firmware DMA-protection shutdown for the current physical-DMA model

### Shell and filesystem

The kernel starts an interactive shell after initialization. It includes:

- Navigation and inspection: `ls`, `cd`, `pwd`, `cat`
- File operations: `mkdir`, `mkfile`, `write`, `rm`, and `rm -rf`
- A small full-screen text editor with cursor movement and `Ctrl+S` save confirmation
- Console and system commands: `help`, `clear`, `echo`, and `sysinfo`
- Network diagnostics: `netstat`, `dhcp`, `dhcpretry`, `wifistat`, and `wifiprep`

The initial filesystem is a tar archive built from [`files/`](files/) and loaded as a Limine module. It can be modified in memory, but disk reads, writes, and `sync` are currently disabled, so changes do **not** survive a reboot.

### USB

The custom polling-mode xHCI driver currently implements:

- PCI discovery and MMIO controller initialization
- DCBAA, command ring, event ring, and ERST setup
- Root-port reset and device-slot allocation
- Device/input contexts and endpoint-zero configuration
- USB device addressing and descriptor requests
- HID boot-protocol keyboard configuration and key polling

xHCI support remains experimental, especially across different real controllers and USB devices.

### Networking

The networking path is polling-based and includes:

- A generic network-device layer
- Ethernet II
- ARP with a small in-memory neighbor table
- IPv4 without fragmentation support
- ICMP echo replies
- UDP sockets
- A DHCP client with retry support
- A deliberately small, single-connection TCP server implementation
- An HTTP test server on TCP port `8080` that accepts a request body and returns a plain-text response

Available Ethernet drivers:

| Driver | Hardware | State |
| --- | --- | --- |
| `e1000` | Intel 82540/82545-compatible devices used by QEMU (`8086:100e`, `100f`, `1010`) | Working QEMU/test path |
| `rtl8168` | Realtek RTL8168/RTL8111 (`10ec:8168`) | Experimental real-hardware path, including RTL8168H firmware and revision-specific setup |

DNS, outbound application protocols, TCP concurrency, retransmission, congestion control, and IPv4 fragmentation are not implemented.

### Wi-Fi

Intel Wi-Fi support is at an early transport bring-up stage. The current code recognizes one verified Intel Wi-Fi 6 AX201 configuration (`8086:a0f0`, subsystem `0074`), parses and validates its API 77 firmware, reads controller identity registers, and can prepare a firmware DMA map.

Firmware upload, controller CPU start, scanning, authentication, association, encryption, and packet transport are **not** implemented. In other words, myOS cannot connect to a Wi-Fi network yet.

## Build requirements

The build is driven by GNU Make. Use `make` on systems where it is GNU Make (commonly Linux), or `gmake` on systems such as macOS and BSD.

Required for an ISO build:

- GNU Make
- A GNU-compatible x86_64 ELF toolchain, or Clang/LLVM with `ld.lld`
- A host C compiler
- `git`, `curl`, `gzip`, and `tar` for first-build dependencies
- `xorriso`

Optional tools:

- `qemu-system-x86_64` to run the OS
- OVMF/EDK2 support is downloaded automatically for the UEFI run target
- `sgdisk` and `mtools` for the experimental raw HDD-image targets

The first build downloads pinned freestanding headers, compiler runtime sources, and Limine protocol headers. If the bundled Limine binary directory is absent, the build downloads and builds that too.

## Building

From the repository root, build the recommended ISO image with Clang/LLVM:

```sh
gmake TOOLCHAIN=llvm all
```

On a GNU/Linux host where `make` is GNU Make:

```sh
make TOOLCHAIN=llvm all
```

The output is `myos.iso`. A prefixed cross-toolchain can be selected instead:

```sh
gmake TOOLCHAIN_PREFIX=x86_64-elf- all
```

Useful maintenance targets:

```sh
gmake clean
gmake distclean
gmake -C kernel test-firmware
```

`distclean` also removes downloaded dependencies and generated disk images, so the next build needs network access.

## Running in QEMU

Choose a target based on what you are testing:

| Command | QEMU configuration |
| --- | --- |
| `gmake run` | Headless Q35 machine with serial I/O |
| `gmake run-video` | Graphical display, xHCI controller, and USB keyboard |
| `gmake run-net` | Headless machine with an Intel E1000 NIC |
| `gmake run-video-net` | Graphical display, USB keyboard, and E1000 networking |
| `gmake run-uefi` | Headless boot through downloaded OVMF firmware |

For example:

```sh
gmake TOOLCHAIN=llvm run-video-net
```

Headless QEMU uses `-nographic`; press `Ctrl+A`, then `x` to exit. Networking uses QEMU user-mode networking, and the DHCP client should normally receive `10.0.2.15`.

The `all-hdd`, `run-hdd`, and `run-hdd-uefi` targets are still experimental. The ISO workflow is the supported development path.

## Running on real hardware

The generated ISO is hybrid BIOS/UEFI bootable. The included [`flash.sh`](flash.sh) helper can write it to an external/removable drive on macOS or Linux:

```sh
./flash.sh
```

The script lists eligible drives, asks you to select the ISO and target, checks the selection against the current system disk, and requires an exact confirmation before invoking `dd`. It still overwrites the selected drive completely—check the device name and size carefully.

Hardware support is narrow and experimental. Serial logs are the most useful source of information during failed xHCI, Ethernet, IOMMU, or Wi-Fi bring-up.

## Project layout

```text
.
├── files/                    Initial initrd contents
├── kernel/
│   ├── firmware/             RTL8168 and Intel Wi-Fi firmware
│   ├── linker-scripts/       x86_64 kernel linker script
│   ├── src/
│   │   ├── drivers/          xHCI, keyboard, Ethernet, and Wi-Fi drivers
│   │   ├── net/              Ethernet through the small HTTP/TCP path
│   │   ├── system/           CPU and system-information helpers
│   │   └── ...               Kernel, console, memory, PCI, shell, and tar FS
│   └── tests/                Host-side heap and firmware-parser tests
├── GNUmakefile               Image and QEMU targets
├── flash.sh                  Interactive removable-drive flashing helper
└── limine.conf               Boot entry and initrd configuration
```

## Near-term work

- Stabilize xHCI keyboard enumeration across real controllers
- Continue RTL8168/RTL8111 validation and DMA debugging on real hardware
- Complete Intel Wi-Fi firmware upload and transport initialization
- Replace the intentionally limited TCP path with fuller connection handling
- Restore persistent storage and filesystem syncing
- Add stronger memory management, interrupts, scheduling, processes, and user space

myOS is primarily a personal learning and hardware-experimentation project. Suggestions, bug reports, and focused improvements are welcome, but its internal interfaces may change without notice.

Built from scratch, one hardware bug at a time.
