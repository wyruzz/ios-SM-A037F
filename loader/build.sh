#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
LLD_LINK="${LLD_LINK:-lld-link}"
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece. 0x41000000 is
# just above the UEFI pool (0x40D00000..0x41000000) and below the window XNU will be given; loading
# there keeps the 7.4 MB image out of that window and avoids a failed AllocatePages at an unmapped base.
"$LLD_LINK" /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0x41000000 /out:loader.efi loader.obj
ls -la loader.efi
