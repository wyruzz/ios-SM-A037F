# Scripts

These are the scripts from the A03s development tree, kept so the exact steps
are reproducible. They expect the layout used during the port:

```
<root>/
  edk2-mt6765/        # EDK2 + edk2-platforms + MT6765Pkg + BootShim
  out/                # built images and payloads
  toolchain/          # AArch64 GCC, iasl, LLVM wrappers
  boot-uefi-a03s.img  # the stock A03s boot image (for pack_boot_v2.py)
```

The public repository splits that tree into `loader/`, `uefi/` and `scripts/`.
Set the environment variables named at the top of each script
(`A03S_SRC`, `A03S_XNULOADER`, `A03S_BUILD_ROOT`, `A03S_OUT`, ...) or put the
trees back into the layout above.

`flash_a03s.sh` and `pull_log.sh` work as-is on a phone in TWRP with `adb root`.
`pack_boot_v2.py`, `mklz4payload.py` and `verify_boot_img.py` are standalone
Python tools.

The build recipe itself is in `../docs/building.md`.
