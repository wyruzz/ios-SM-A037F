#!/bin/bash
#
# Deploy a freshly built XNU kernel to the Galaxy A03s.
#
# Takes the Mach-O kernel built by 02_HACKIOS-XNU/xnu-build.sh, wraps it in an
# XNUP payload, rebuilds BootShim with the new payload size, concatenates
# BootShim + the already-built EDK2 firmware + XNUP, and packs a boot.img using
# the known-good donor image for its header, DTB and command line.
#
# Usage:
#   ./deploy_xnu_a03s.sh [kernel] [--flash]
#
# Defaults:
#   kernel  $HOME/a03s-xnu/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
#   donor   build-inputs/boot-uefi-a03s-hackios-scan1.img
#   output  out/boot-uefi-a03s-xnu.img
#
# The firmware itself comes from out/MT6765PKG_UEFI.fd (build_uefi_a03s.sh).
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="${1:-$HOME/a03s-xnu/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}"
shift || true
FLASH=0
for arg in "$@"; do
  case "$arg" in
    --flash) FLASH=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

DONOR="$ROOT/build-inputs/boot-uefi-a03s-hackios-scan1.img"
FD="$ROOT/out/MT6765PKG_UEFI.fd"
SHIM_DIR="$ROOT/edk2-mt6765/BootShim"
LLVM_BIN="$ROOT/toolchain/llvm-aarch64/bin"
OUT="$ROOT/out/boot-uefi-a03s-xnu.img"
PAYLOAD="$ROOT/out/xnu-payload.bin"
KERNEL_BLOB="$ROOT/out/xnu-kernel-blob.bin"
RAMDISK="$ROOT/out/donor-ramdisk.bin"
CMDLINE="-v debug=0x100 cpus=1"

die() { echo "error: $*" >&2; exit 1; }

[ -f "$KERNEL" ]   || die "kernel not found: $KERNEL"
[ -f "$DONOR" ]    || die "donor boot.img not found: $DONOR"
[ -f "$FD" ]       || die "firmware not found: $FD (run ./build_uefi_a03s.sh)"
[ -x "$LLVM_BIN/aarch64-none-elf-gcc" ] || die "LLVM wrappers missing in $LLVM_BIN"
[ -f "$ROOT/mklz4payload.py" ] || die "mklz4payload.py missing"
[ -f "$ROOT/pack_boot_v2.py" ] || die "pack_boot_v2.py missing"

# lz4 may live in the local tools prefix when the distro has no package.
if ! command -v lz4 >/dev/null 2>&1 && [ -x /tmp/opencode/tools/usr/bin/lz4 ]; then
  export PATH="/tmp/opencode/tools/usr/bin:$PATH"
fi
command -v lz4 >/dev/null || die "lz4 not found"

echo "==> kernel: $KERNEL"
ls -l "$KERNEL"
file "$KERNEL" 2>/dev/null || true

echo "==> building XNUP payload"
python3 "$ROOT/mklz4payload.py" \
  --kernel "$KERNEL" \
  --output "$PAYLOAD" \
  --cmdline "$CMDLINE"
PAYLOAD_SIZE="$(wc -c < "$PAYLOAD" | tr -d ' ')"
[ $((PAYLOAD_SIZE % 16)) -eq 0 ] || die "payload size $PAYLOAD_SIZE is not 16-byte aligned"
[ "$PAYLOAD_SIZE" -le $((0x02000000)) ] || die "payload is larger than 32 MiB"

echo "==> building BootShim (payload $PAYLOAD_SIZE bytes)"
make -B -C "$SHIM_DIR" \
  CROSS_COMPILE="$LLVM_BIN/aarch64-none-elf-" \
  UEFI_BASE=0x50000000 UEFI_SIZE=0x00200000 \
  XNU_PAYLOAD_BASE=0x60000000 PAYLOAD_SIZE="$PAYLOAD_SIZE" >/dev/null
SHIM="$SHIM_DIR/BootShim.bin"
[ "$(wc -c < "$SHIM" | tr -d ' ')" -eq $((0xE0)) ] || die "BootShim is not 0xE0 bytes"

echo "==> concatenating BootShim + firmware + XNUP"
cat "$SHIM" "$FD" "$PAYLOAD" > "$KERNEL_BLOB"
BLOB_SIZE="$(wc -c < "$KERNEL_BLOB" | tr -d ' ')"
[ "$BLOB_SIZE" -eq $((0xE0 + 0x200000 + PAYLOAD_SIZE)) ] || \
  die "unexpected kernel blob size $BLOB_SIZE"

# Preserve the donor's 8-byte Android ramdisk (the kernel's real root comes
# from the XNUP payload, but keeping the header identical avoids surprises).
python3 - "$DONOR" "$RAMDISK" <<'PY'
import struct, sys
from pathlib import Path
img = Path(sys.argv[1]).read_bytes()
u32 = lambda o: struct.unpack_from('<I', img, o)[0]
page = u32(36)
off = (page + u32(8) + page - 1) // page * page
Path(sys.argv[2]).write_bytes(img[off:off + u32(16)])
PY

echo "==> packing $OUT"
python3 "$ROOT/pack_boot_v2.py" \
  --kernel "$KERNEL_BLOB" \
  --stock "$DONOR" \
  --ramdisk "$RAMDISK" \
  --cmdline "bootopt=64S3,32N2,64N2" \
  --output "$OUT"

echo "==> verifying"
python3 "$ROOT/verify_boot_img.py" "$OUT"
sha256sum "$OUT"

if [ "$FLASH" = 1 ]; then
  echo "==> flashing with heimdall"
  IMG="$OUT" "$ROOT/flash_a03s.sh" --heimdall
fi

cat <<EOF

Ready: $OUT
Flash with:
  IMG=$OUT ./flash_a03s.sh --heimdall
EOF
