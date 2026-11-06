# A03s UEFI port (MT6765)

`MT6765Pkg` is the EDK2 / Project Mu platform package for the Samsung Galaxy A03s
(SM-A037F, MediaTek MT6765). `BootShim` is the small AArch64 entry stub that the
Android boot image starts; it sets up the EL3 entry state and jumps to the EDK2
firmware.

## Memory map

The A03s memory map is in `MT6765Pkg/Include/Configuration/DeviceMemoryMap.h`.
The important entry for the iOS port is:

```c
{"HLOS 3", 0xD0000000, 0x2E000000, NoHob, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},
```

`NoHob` keeps the range out of the UEFI HOBs (UEFI would otherwise mark large
free blocks as `EfiLoaderData` and the loader would see no contiguous RAM).
The loader's `win_base=` / `win_size=` boot arguments are authoritative and can
describe a larger window (`0x6FFFC000` = 1.75 GB) than this entry; the kernel
maps that window itself.

The rest of the file is the MT6765 map: TEE, modem, SCP/SSPM, the display
framebuffer at `0x7C250000`, the UEFI pool at `0x40D00000`, etc. Do not move
those without the phone's own memory map.

## Build

The package expects an EDK2 tree with `edk2-platforms`, an AArch64 GCC and
`iasl`:

```sh
cp -a MT6765Pkg      /path/to/edk2/
cp -a BootShim       /path/to/edk2/
# then build MT6765Pkg/Devices/a03s.dsc
```

`../scripts/build_uefi_a03s.sh` is the exact build used for the phone. It
mirrors the sources to a path without spaces (EDK2 rejects spaces in
`WORKSPACE`), builds `MT6765Pkg/Devices/a03s.dsc`, and concatenates
`BootShim.bin` + `MT6765PKG_UEFI.fd`.

`MT6765Pkg/Library/XnuLoaderImageLib/loader_efi.c` is **generated** from the
built `loader.efi` by `scripts/build_xnuloader.sh`; it is not committed here.
Build the loader first, then the UEFI.

## Boot image

The boot partition is 32 MiB. `BootShim` + the UEFI FD live at offset `0xE0` /
`0x2000E0`; the XNUP payload starts at `0x2020E0`. `../uefi/pack_boot_v2.py`
writes the Android v2 header around a stock A03s boot image (header + DTB +
cmdline).

`../uefi/mklz4payload.py` builds the XNUP payload (kernel, two ramdisks, command
line, checkpoint) and `../uefi/verify_boot_img.py` checks the result.

## License

The package is derived from EDK2 / Project Mu / edk2-platforms and keeps their
BSD-2-Clause-Patent notices where they exist. See `../NOTICE`.
