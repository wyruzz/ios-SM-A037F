# xnuloader

A small EFI application that starts XNU on non-Apple hardware. It is the last
stage of the HakiOS boot chain:

```
Android boot.img -> BootShim -> EDK2 UEFI -> xnuloader -> XNU + iOS user space
```

It replaces iBoot's job: builds a device tree and `boot_args`, places the kernel
and the ramdisks in memory, installs the return slot / exception vectors, and
jumps to the kernel entry.

## A03s additions

- **`win_base=` / `win_size=`** on the payload command line force the physical
  memory window before `choose_window()` runs. This is how the A03s gets
  1.75 GB (`0xD0000000`, `0x6FFFC000`) instead of the old 224 MB.
- **Persistent live log** at LBA `1216720` (24 MiB into the 32 MiB boot
  partition), written through eMMC MSDC0. The loader keeps writing the log every
  ~4 s and stops drawing it to the screen after ~35 s; `scripts/pull_log.sh`
  extracts it.
- **`loader_return_after=`** keeps the kernel alive for the session; the loader
  can return later and hand over the log.
- **Display** bring-up for the 720x1600 panel at `0x7C250000`.
- **A03s platform** (`platform a03s`): the PMIC buttons, battery over the UEFI
  charger protocols, the MT6765 display and the eMMC block service.

## Build

```sh
cd loader
./build.sh
```

Needs `clang` and `lld-link` (or the HakiOS loader toolchain). The build needs
`identity.h`, which is generated from **your** iPhone10,3 device tree:

```sh
ipsw img4 im4p extract DeviceTree.d22ap.im4p -o DeviceTree.d22ap.bin
python3 mkidentity.py DeviceTree.d22ap.bin > identity.h
```

`identity.h` is deliberately not in this repository: it is derived from Apple's
firmware and is specific to the IPSW you build for. `loader.c` uses a neutral
placeholder serial; replace it with your own value only in a private build.

## Files

- `loader.c` — the loader itself.
- `efi.h` — the minimal EFI definitions (no EDK2 headers needed).
- `mkidentity.py` — device-tree identity generator.
- `mkpayload.py` — helper for the XNUP payload.
- `build.sh` — the cross build.

See `../docs/building.md` for the whole chain.
