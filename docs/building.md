# Building the A03s port

This repository contains only the **A03s-specific** sources and build glue. The
rest of the base system comes from the HakiOS project:

- <https://github.com/oosexgang/hackios-xnu-kernel>
- <https://github.com/oosexgang/hackios-xnu-userland>

You also need your own **iOS 16.5 IPSW for iPhone10,3 (d22ap)**. Nothing from
Apple is shipped here.

## Host

- Linux x86-64 (tested on Arch/Ubuntu), ~20 GB free disk.
- `clang`/`lld`, `make`, `python3`, `lz4`, `git`, `rsync`, `curl`, `unzip`.
- An AArch64 bare-metal GCC for EDK2:
  `aarch64-none-elf-gcc` (xPack or Arm GNU Toolchain).
- EDK2 build tools (`iasl`, `python` alias).

## 1. iOS 16.5 system image

Follow the HakiOS userland instructions to fetch the IPSW from Apple's CDN and
build `system.apfs` (the read-only iOS root) and the small `flashboot` image.
The A03s boot uses:

- `flashboot.apfs` (4 MiB boot volume, contains `sbin/launchd` = `stage0` and
  the two ramdisks),
- `root.apfs` (4 MiB RAM image with our init, injected libraries, launchd jobs
  and test programs),
- the iOS root volume on the phone's `userdata` partition (APFS).

The loader packs `flashboot.apfs` + `root.apfs` as the two ramdisks (`rd=md0`,
`rdsplit=0x400000`).

## 2. XNU

1. Clone Apple's XNU tag `xnu-8796.121.2`:
   ```sh
   git clone --depth 1 --branch xnu-8796.121.2 \
       https://github.com/apple-oss-distributions/xnu
   ```
2. Prepare the HakiOS XNU tree (apply HakiOS `xnu/hakios.patch` and copy
   `xnu/files/`), as described by the HakiOS kernel repository.
3. Overlay the files from this repository:
   ```sh
   cp -a kernel/files/. /path/to/xnu/
   ```
   They are at their original paths under `bsd/`, `osfmk/`, `iokit/` and
   `pexpert/`.
4. Build with the HakiOS `xnu-build.sh` / `xnu-deps/setup.sh` flow. The result
   used here is
   `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.

The A03s changes include the memory window, the framebuffer driver, the
persona/CS fixes, `exec_inject` and the persistent log.

## 3. Loader

```sh
cd loader
./build.sh
```

`build.sh` needs `identity.h`, generated from **your** iPhone10,3 device tree:

```sh
# from your IPSW:
#   Firmware/all_flash/DeviceTree.d22ap.im4p
ipsw img4 im4p extract DeviceTree.d22ap.im4p -o DeviceTree.d22ap.bin
python3 mkidentity.py DeviceTree.d22ap.bin > identity.h
```

`identity.h` is not shipped: it is derived from Apple's firmware and is
specific to the IPSW you use.

## 4. UEFI (EDK2 / Project Mu port)

The port is the `MT6765Pkg` package plus the `BootShim` entry stub. It is meant
to be dropped into an EDK2/edk2-platforms tree (the HakiOS / Project Mu
approach). The A03s tree used here is based on EDK2 stable202302 and Project Mu.

```sh
# with EDK2 in ./edk2 and edk2-platforms in ./edk2-platforms:
cp -a uefi/MT6765Pkg      edk2/MT6765Pkg
cp -a uefi/BootShim       edk2/BootShim
# then build MT6765Pkg/Devices/a03s.dsc with the usual EDK2 build command
```

`scripts/build_uefi_a03s.sh` shows the exact invocation used for the phone
(space-free build directory, `iasl`, AArch64 GCC).

## 5. Pack the boot image

The A03s boot partition is 32 MiB and uses the Android v2 boot header. The
payload is a `XNUP` container: `kernel.lz4`, the two ramdisks, the command line
and a checkpoint.

```sh
python3 uefi/mklz4payload.py \
    --lz4 "$(command -v lz4)" \
    --kernel "$KERNEL" \
    --output out/xnu-payload.bin \
    --cmdline "debug=0x100 bootcpu=3 cpus=1 cpufreq=1 touchscreen=1 touch_lat=1 \
ca_vbl=1 fbtrace=200 sb_delay=8 win_base=0xd0000000 win_size=0x6fffc000 \
late_note=86400 rd=md0 root_apfs=ufs_userdata ufs=2 amfi_get_out_of_my_way=1 \
cs_enforcement_disable=1 loader_return_after=99999" \
    --ramdisk out/ramdisks.bin --rdsplit 0x400000 --checkpoint 998
```

Then concatenate `BootShim.bin`, the UEFI FD and the payload, and pack with
`uefi/pack_boot_v2.py` using a stock A03s boot image for the header/DTB. See
`scripts/make_boot_a03s.sh` and `scripts/build_uefi_a03s.sh`.

## 6. Flash and log

```sh
# phone in TWRP, adb root
./scripts/flash_a03s.sh --adb
# boot, wait, then:
./scripts/pull_log.sh
```

`flash_a03s.sh` backs up the current boot partition and verifies the read-back.
`pull_log.sh` extracts the log from 24 MiB into the 32 MiB boot partition.

## Boot arguments used

```
debug=0x100 bootcpu=3 cpus=1 cpufreq=1 touchscreen=1 touch_lat=1
ca_vbl=1 fbtrace=200 sb_delay=8
win_base=0xd0000000 win_size=0x6fffc000
late_note=86400 rd=md0 root_apfs=ufs_userdata ufs=2
amfi_get_out_of_my_way=1 cs_enforcement_disable=1
loader_return_after=99999
```

`win_base`/`win_size` are read by the loader before it chooses the window.
`sb_delay` is the SpringBoard injection delay; `late_note` keeps the persistent
log alive for 24 h; `loader_return_after` keeps the kernel running.
