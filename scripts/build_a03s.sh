#!/bin/bash
#
# Build the Samsung Galaxy A03s (SM-A037F, MT6765) edk2 boot image on macOS.
#
# Why this script exists:
#   * EDK2 rejects a WORKSPACE path containing spaces, and this project lives
#     under "Проект по умолчанию".  The script mirrors the sources into a
#     space-free directory and builds there; the original tree stays the
#     source of truth.
#   * The build needs a bare-metal AArch64 GCC, a host iasl, a "python"
#     alias for the BaseTools tests and the stock boot.img for its header,
#     DTB and load addresses.  All of them are checked before anything runs.
#
# Usage:
#   ./build_a03s.sh                 # incremental build + pack + verify
#   A03S_CLEAN=1 ./build_a03s.sh    # throw Build/ and Conf/ away first
#
# Environment overrides:
#   A03S_SRC         source tree           (default: ./edk2-mt6765)
#   A03S_BUILD_DIR   space-free mirror     (default: $HOME/a03s-build)
#   A03S_TOOLCHAIN   aarch64-none-elf-gcc  (default: $HOME/aarch64-none-elf-gcc)
#   A03S_TOOLS_BIN   python + iasl symlinks(default: $HOME/edk2-bin)
#   A03S_STOCK_BOOT  stock boot.img        (default: ~/Downloads/xnu_bootshim_SM-A037F/boot.img)
#   A03S_KERNELCACHE kernelcache payload   (default: ./kernelcache.release.iPhone10,3_6)
#   A03S_XNULOADER   xnuloader source tree (default: $HOME/hackios-xnu/xnuloader)
#   A03S_XNU_CMDLINE kernel cmdline in the payload (default: -v)
#   A03S_XNU_RAMDISK APFS/image to add to the payload as /dev/md0 (with rd=md0)
#   A03S_XNU_RAMDISK1 second image, appended after the first: /dev/md1 (one payload, split)
#   A03S_CHECKPOINT  stop XNU at bring-up checkpoint N (default: none)
#   A03S_CHECKPOINTS comma-separated checkpoints to scan in one boot
#   A03S_OUT         output boot image     (default: ./boot-uefi-a03s.img)
#
# edksetup.sh reads several optional variables without defaults, so `set -u`
# cannot be used around it.
set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${A03S_SRC:-$ROOT/edk2-mt6765}"
BUILD_DIR="${A03S_BUILD_DIR:-$HOME/a03s-build}"
TOOLCHAIN="${A03S_TOOLCHAIN:-$HOME/aarch64-none-elf-gcc}"
TOOLS_BIN="${A03S_TOOLS_BIN:-$HOME/edk2-bin}"
STOCK="${A03S_STOCK_BOOT:-$HOME/Downloads/xnu_bootshim_SM-A037F/boot.img}"
KERNELCACHE="${A03S_KERNELCACHE:-$ROOT/kernelcache.release.iPhone10,3_6}"
XNULOADER="${A03S_XNULOADER:-$HOME/hackios-xnu/xnuloader}"
OUT="${A03S_OUT:-$ROOT/boot-uefi-a03s.img}"
PACKER="$ROOT/pack_boot_v2.py"
VERIFY="$ROOT/verify_boot_img.py"
RAMDISK="$SRC/blobs/initramfs"
MKPAYLOAD="$ROOT/mklz4payload.py"
BUILD_LOADER="$ROOT/build_xnuloader.sh"

UEFI_BASE=0x50000000
UEFI_SIZE=0x00200000
XNU_PAYLOAD_BASE=0x60000000
CMDLINE="bootopt=64S3,32N2,64N2"
# Kernel command line carried in the XNUP payload.  -v makes XNU use the
# framebuffer as its verbose console; without it the screen stays on the
# loader's last line while the kernel runs (the phone has no serial port).
# debug=0x100 keeps the phone alive after a kernel panic so the panic text
# can be read from the screen (XNU otherwise reboots immediately).
XNU_CMDLINE="${A03S_XNU_CMDLINE:--v debug=0x100}"
XNU_RAMDISK="${A03S_XNU_RAMDISK:-}"
XNU_RAMDISK1="${A03S_XNU_RAMDISK1:-}"
XNU_CHECKPOINT="${A03S_CHECKPOINT:-}"
XNU_CHECKPOINTS="${A03S_CHECKPOINTS:-}"
XNU_USB="${A03S_XNU_USB:-}"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

die() { echo "error: $*" >&2; exit 1; }

[ -d "$SRC/edk2" ]            || die "edk2/ missing in $SRC (clone edk2-stable202302 with submodules)"
[ -d "$SRC/edk2-platforms" ]  || die "edk2-platforms/ missing in $SRC"
[ -f "$SRC/MT6765Pkg/Devices/a03s.dsc" ] || die "MT6765Pkg/Devices/a03s.dsc missing"
[ -x "$TOOLCHAIN/bin/aarch64-none-elf-gcc" ] || die "toolchain not found: $TOOLCHAIN/bin/aarch64-none-elf-gcc"
[ -f "$STOCK" ]               || die "stock boot.img not found: $STOCK"
[ -f "$RAMDISK" ]             || die "dummy ramdisk not found: $RAMDISK"
[ -f "$PACKER" ]              || die "packer not found: $PACKER"
[ -f "$VERIFY" ]              || die "verifier not found: $VERIFY"
[ -f "$KERNELCACHE" ]         || die "kernelcache not found: $KERNELCACHE"
[ -f "$MKPAYLOAD" ]           || die "payload generator not found: $MKPAYLOAD"
[ -x "$BUILD_LOADER" ]        || die "loader build script not executable: $BUILD_LOADER"
[ -f "$XNULOADER/loader.c" ]  || die "xnuloader source not found: $XNULOADER/loader.c"
if [ -n "$XNU_RAMDISK" ]; then
  [ -f "$XNU_RAMDISK" ]       || die "payload ramdisk not found: $XNU_RAMDISK"
fi
if [ -n "$XNU_RAMDISK1" ]; then
  [ -n "$XNU_RAMDISK" ]       || die "A03S_XNU_RAMDISK1 needs A03S_XNU_RAMDISK (it is the first disk)"
  [ -f "$XNU_RAMDISK1" ]      || die "second payload ramdisk not found: $XNU_RAMDISK1"
fi
command -v lz4 >/dev/null     || die "lz4 not found (needed for the kernelcache payload)"

# BaseTools' unit-test target calls "python", not "python3".
mkdir -p "$TOOLS_BIN"
if [ ! -e "$TOOLS_BIN/python" ]; then
  ln -sfn "$(command -v python3)" "$TOOLS_BIN/python"
fi
if [ ! -x "$TOOLS_BIN/iasl" ]; then
  if [ -d "$ROOT/tools/acpica-master" ]; then
    echo "==> building iasl from ACPICA"
    make -C "$ROOT/tools/acpica-master/generate/unix" -j"$JOBS" >/dev/null
    ln -sfn "$ROOT/tools/acpica-master/generate/unix/bin/iasl" "$TOOLS_BIN/iasl"
  else
    die "iasl not found; build ACPICA and put a symlink in $TOOLS_BIN"
  fi
fi

echo "==> building the XNU loader"
A03S_XNULOADER="$XNULOADER" A03S_SRC="$SRC" "$BUILD_LOADER"

echo "==> mirroring sources to $BUILD_DIR"
mkdir -p "$BUILD_DIR"
rsync -a --exclude '.git' --exclude 'Build' "$SRC/edk2/" "$BUILD_DIR/edk2/"
rsync -a --exclude '.git' "$SRC/edk2-platforms/" "$BUILD_DIR/edk2-platforms/"
rsync -a --exclude '.git' "$SRC/MT6765Pkg/" "$BUILD_DIR/MT6765Pkg/"
rsync -a --exclude '.git' "$SRC/BootShim/" "$BUILD_DIR/BootShim/"
rsync -a "$SRC/blobs/" "$BUILD_DIR/blobs/"
cp "$SRC"/*.sh "$BUILD_DIR/" 2>/dev/null || true

export PATH="$TOOLS_BIN:$TOOLCHAIN/bin:$BUILD_DIR/edk2/BaseTools/BinWrappers/PosixLike:$BUILD_DIR/edk2/BaseTools/Source/C/bin:$PATH"
export WORKSPACE="$BUILD_DIR/workspace"
export PACKAGES_PATH="$BUILD_DIR/edk2:$BUILD_DIR/edk2-platforms:$BUILD_DIR"
export EDK_TOOLS_PATH="$BUILD_DIR/edk2/BaseTools"
export PYTHON_COMMAND=python3
# edksetup.sh tests "$PYTHON3_ENABLE" without quoting it, which aborts
# under `set -u`; define it so the source can complete.
export PYTHON3_ENABLE=TRUE
export GCC5_AARCH64_PREFIX="$TOOLCHAIN/bin/aarch64-none-elf-"
export CONF_PATH="$WORKSPACE/Conf"

mkdir -p "$WORKSPACE/Conf"
if [ "${A03S_CLEAN:-0}" = "1" ]; then
  echo "==> cleaning $WORKSPACE/Build and Conf"
  rm -rf "$WORKSPACE/Build" "$WORKSPACE/Conf"
  mkdir -p "$WORKSPACE/Conf"
fi

if [ ! -x "$BUILD_DIR/edk2/BaseTools/Source/C/bin/GenFv" ]; then
  echo "==> building EDK2 BaseTools"
  make -C "$BUILD_DIR/edk2/BaseTools" -j"$JOBS"
fi

mkdir -p "$WORKSPACE"

echo "==> compressing the kernelcache into an XNUP payload"
CHECKPOINT_ARGS=()
if [ -n "$XNU_CHECKPOINT" ]; then
  CHECKPOINT_ARGS+=(--checkpoint "$XNU_CHECKPOINT")
fi
if [ -n "$XNU_CHECKPOINTS" ]; then
  CHECKPOINT_ARGS+=(--checkpoints "$XNU_CHECKPOINTS")
fi
RAMDISK_ARGS=()
if [ -n "$XNU_RAMDISK" ]; then
  if [ -n "$XNU_RAMDISK1" ]; then
    # Two RAM disks in one payload entry, the second right after the first
    # (there is no host upload path on the A03s): the loader's rdsplit entry
    # says where the first ends and the device tree gets RAMDisk and RAMDisk1.
    cat "$XNU_RAMDISK" "$XNU_RAMDISK1" > "$WORKSPACE/ramdisk.bin"
    RAMDISK1_SIZE="$(wc -c < "$XNU_RAMDISK" | tr -d ' ')"
    RAMDISK_ARGS+=(--ramdisk "$WORKSPACE/ramdisk.bin" --rdsplit "$RAMDISK1_SIZE")
  else
    RAMDISK_ARGS+=(--ramdisk "$XNU_RAMDISK")
  fi
fi
USB_ARGS=()
if [ -n "$XNU_USB" ]; then
  # "usb": the loader stays on USB (fastboot protocol) and runs the kernel on
  # "oem run", keeping the command channel up during the run (oem log,
  # get_staged, oem screenshot). Its one-shot boot is the fallback when the
  # firmware has no USB device stack.
  USB_ARGS+=(--usb)
fi
python3 "$MKPAYLOAD" --kernel "$KERNELCACHE" --output "$WORKSPACE/payload.bin" \
  "--cmdline=$XNU_CMDLINE" "${CHECKPOINT_ARGS[@]}" "${RAMDISK_ARGS[@]}" "${USB_ARGS[@]}"
PAYLOAD_SIZE="$(wc -c < "$WORKSPACE/payload.bin" | tr -d ' ')"
[ $((PAYLOAD_SIZE % 16)) -eq 0 ] || die "payload size $PAYLOAD_SIZE is not a multiple of 16"
[ "$PAYLOAD_SIZE" -le $((0x02000000)) ] || \
  die "payload is $PAYLOAD_SIZE bytes; it must stay below the decompression buffer at 0x62000000"

echo "==> building BootShim (payload $PAYLOAD_SIZE bytes)"
make -B -C "$BUILD_DIR/BootShim" \
  CROSS_COMPILE="$TOOLCHAIN/bin/aarch64-none-elf-" \
  UEFI_BASE="$UEFI_BASE" UEFI_SIZE="$UEFI_SIZE" \
  XNU_PAYLOAD_BASE="$XNU_PAYLOAD_BASE" PAYLOAD_SIZE="$PAYLOAD_SIZE" >/dev/null

echo "==> building a03s.dsc"
cd "$BUILD_DIR"
. edk2/edksetup.sh --reconfig >/dev/null 2>&1 || true
build -a AARCH64 -t GCC5 -b DEBUG -n "$JOBS" -p MT6765Pkg/Devices/a03s.dsc

FD="$WORKSPACE/Build/MT6765Pkg/DEBUG_GCC5/FV/MT6765PKG_UEFI.fd"
[ -f "$FD" ] || die "build finished without $FD"
FD_SIZE="$(wc -c < "$FD" | tr -d ' ')"
[ "$FD_SIZE" -eq "$((UEFI_SIZE))" ] || \
  die "firmware volume is $FD_SIZE bytes, expected UEFI_SIZE ($UEFI_SIZE)"

echo "==> concatenating BootShim + firmware FV + XNU payload"
cat "$BUILD_DIR/BootShim/BootShim.bin" "$FD" "$WORKSPACE/payload.bin" > "$WORKSPACE/UEFI"

echo "==> packing $OUT"
python3 "$PACKER" \
  --kernel "$WORKSPACE/UEFI" \
  --stock "$STOCK" \
  --ramdisk "$RAMDISK" \
  --cmdline "$CMDLINE" \
  --output "$OUT"

echo "==> verifying $OUT"
python3 "$VERIFY" "$OUT" --kernelcache "$KERNELCACHE"
