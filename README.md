# HakiOS on the Samsung Galaxy A03s (SM-A037F, MT6765)

An unfinished research port of **iOS 16.5 user space** to a cheap MediaTek phone,
using Apple's open-source **XNU 8796.121.2** kernel and our own boot chain.

This is **not a ROM** and it is **not usable as a phone**. It is a bring-up
project: a boot menu, a display driver, a persistent log on eMMC, a UEFI port for
the MT6765, and enough of a kernel to run `launchd`, `backboardd` and SpringBoard
on the iPhone X (`iPhone10,3`, `d22ap`) system image.

## Status (6 November 2026)

| Works | Does not work / open |
|---|---|
| Boot menu (Vol Up/Down + Power), UEFI on the A03s | SpringBoard still respawns every ~11.5 s |
| Display (720x1600 panel, `IOMobileFramebuffer`) | Touchscreen: no real driver yet |
| Apple logo, `launchd`, `backboardd`, SpringBoard | Metal/OpenGL ES: no GPU driver |
| **1.75 GB of RAM** given to XNU (`0xD0000000`, 1.75 GB window) | Cellular, Wi-Fi, audio, camera, sensors |
| Persistent live log on eMMC (pull with `./pull_log.sh`) | Multi-range DRAM (the rest of the 4 GB is fragmented) |
| iOS 16.5 root from APFS on `userdata` + RAM disks | |

The main result of this stage is the memory window:

```
phys window d0000000 size 6fffc000 (forced)
memory 1791 MB, page size 16384, 1 cpu
```

It is one contiguous `System RAM` range from the phone's own `/proc/iomem`
(`0xD0000000..0x13FFFEFFF`), above the TEE/modem/SCP carve-outs. Earlier windows
were 224 MB and 736 MB; 1.75 GB is the largest clean contiguous range available.

SpringBoard currently starts, runs for a few seconds and exits cleanly
(`exit(0)`), and launchd starts it again. The last stack shows it blocked in
LaunchServices/CoreAnimation, not in the kernel and not in jetsam. See
[`STATUS.md`](STATUS.md) for the details.

## Repository layout

| Path | What |
|---|---|
| `kernel/README.md` | what the A03s port changes in XNU (description only, no Apple source) |
| `loader/` | `xnuloader`, the EFI loader that starts XNU |
| `userland/src/` | our init, injected libraries, drivers and test programs |
| `userland/etc/` | `inject.conf`, `rc`, `rc.jobs`, the data-volume skeleton |
| `uefi/MT6765Pkg/` | the EDK2/Project Mu port for the MT6765 (A03s) |
| `uefi/BootShim/` | the tiny AArch64 shim that enters the UEFI image |
| `scripts/` | build, pack, flash and log-pull scripts |
| `docs/` | memory map and boot-chain notes |

The **user-space system image and the HakiOS drivers** are not duplicated here.
This port is built on top of the HakiOS project; see
[`docs/building.md`](docs/building.md).

## Quick start (outline)

You need a Linux x86-64 host, an AArch64 cross toolchain, EDK2 build tools,
`lz4`, and your own copy of the **iOS 16.5 IPSW for iPhone10,3** (Apple's public
restore image). No Apple file is in this repository.

1. Fetch and extract the iOS 16.5 IPSW (Apple's CDN) and build the HakiOS
   system image / userland as described in `docs/building.md`.
2. Build the XNU kernel from Apple's `xnu-8796.121.2`, with the HakiOS patch and
   the A03s changes described in `kernel/README.md` applied.
3. Build `xnuloader` (`loader/build.sh`) and the UEFI image
   (`uefi/MT6765Pkg` + `uefi/BootShim`).
4. Pack a 32 MiB `boot.img` with `uefi/pack_boot_v2.py`.
5. Flash it in TWRP (`scripts/flash_a03s.sh --adb`) and boot.

Every boot writes its log into the tail of the 32 MiB boot partition; pull it
with `scripts/pull_log.sh` while in TWRP.

## Hardware

- Samsung Galaxy A03s `SM-A037F`, MediaTek **MT6765** (Helio P35), 4 GB RAM,
  64 GB eMMC.
- Panel 720x1600; no GPU driver, so CoreAnimation runs in software.
- The boot partition is 32 MiB; the loader puts its live log at 24 MiB and the
  kernel payload at `0x60000000`.

## Credits and licenses

This port stands on the **HakiOS** project by OOSEX GANG and contributors
(`hackios-xnu-kernel`, `hackios-xnu-userland`). The A03s changes to Apple's XNU
are described in [`kernel/README.md`](kernel/README.md) but are **not**
redistributed here; if you reproduce them, they are a derivative work of XNU
under the **Apple Public Source License 2.0**. Everything else that is ours is
under the **MIT License**. Third-party components and their terms are listed in
[`NOTICE`](NOTICE).

iOS, iPhone and Apple are trademarks of Apple Inc. This project is not
affiliated with, authorised or endorsed by Apple. No iOS binary, image, cache,
font or firmware is redistributed here; you build the system image from your own
IPSW and your own phone's firmware.
