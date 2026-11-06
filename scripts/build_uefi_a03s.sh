#!/bin/bash
#
# Build the HakiOS A03s UEFI firmware with Clang/lld.
#
# EDK2's build system is unreliable when the source path contains spaces, so
# this script mirrors the tree into a space-free directory under /tmp and
# builds there.  The result is copied back to out/.
#
# Requirements:
#   * clang and llvm-objcopy (LLVM 18+; tested with LLVM 21)
#   * iasl in toolchain/bin (or A03S_IASL pointing at it)
#   * EDK2 BaseTools host tools in edk2/BaseTools/Source/C/bin
#     (the script tries to build them if they are missing)
#
# Usage:
#   ./build_uefi_a03s.sh            # build and refresh out/
#   ./build_uefi_a03s.sh --clean    # also drop the mirrored workspace first
#
# Environment:
#   A03S_BUILD_ROOT  mirror directory (default /tmp/opencode/a03s-build)
#   A03S_LLVM_BIN    directory containing clang/llvm-objcopy
#   A03S_IASL        path to the iasl binary
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/edk2-mt6765"
OUT="$ROOT/out"
BUILD_ROOT="${A03S_BUILD_ROOT:-/tmp/opencode/a03s-build}"
MIRROR="$BUILD_ROOT/edk2-mt6765"
LLVM_BIN="$BUILD_ROOT/llvm-bin"
CLEAN=0

for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    *) echo "error: unknown argument: $arg" >&2; exit 2 ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }
note() { echo "==> $*"; }

[ -d "$SRC" ] || die "$SRC not found"
[ -f "$SRC/MT6765Pkg/Devices/a03s.dsc" ] || die "a03s.dsc not found"

# ---------------------------------------------------------------------------
# Locate LLVM and iasl
# ---------------------------------------------------------------------------
find_llvm_tool() {
  local names=("$@") d n
  for d in ${A03S_LLVM_BIN:+"$A03S_LLVM_BIN"}; do
    for n in "${names[@]}"; do
      [ -x "$d/$n" ] && { echo "$d/$n"; return 0; }
    done
  done
  for n in "${names[@]}"; do
    if command -v "$n" >/dev/null 2>&1; then
      command -v "$n"
      return 0
    fi
  done
  for d in /usr/lib/llvm-*/bin; do
    for n in "${names[@]}"; do
      [ -x "$d/$n" ] && { echo "$d/$n"; return 0; }
    done
  done
  return 1
}

CLANG="$(find_llvm_tool clang)" || die "clang not found; set A03S_LLVM_BIN"
OBJCOPY="$(find_llvm_tool llvm-objcopy)" || die "llvm-objcopy not found"
LLVM_AR="$(find_llvm_tool llvm-ar)" || die "llvm-ar not found"
LLVM_NM="$(find_llvm_tool llvm-nm)" || die "llvm-nm not found"
LLVM_STRIP="$(find_llvm_tool llvm-strip)" || die "llvm-strip not found"
LLVM_RANLIB="$(find_llvm_tool llvm-ranlib)" || die "llvm-ranlib not found"
LD_LLD="$(find_llvm_tool ld.lld)" || die "ld.lld not found"

IASL="${A03S_IASL:-$ROOT/toolchain/bin/iasl}"
[ -x "$IASL" ] || die "iasl not found at $IASL; set A03S_IASL"

# ---------------------------------------------------------------------------
# Mirror the tree without spaces
# ---------------------------------------------------------------------------
mkdir -p "$BUILD_ROOT"
note "mirroring to $MIRROR"
rsync -a --delete \
  --exclude workspace \
  --exclude 'Build' \
  "$SRC/" "$MIRROR/"
if [ "$CLEAN" = "1" ]; then
  rm -rf "$MIRROR/workspace"
fi
mkdir -p "$MIRROR/workspace"

# ---------------------------------------------------------------------------
# BaseTools host tools
# ---------------------------------------------------------------------------
if [ ! -x "$MIRROR/edk2/BaseTools/Source/C/bin/GenFw" ] || \
   [ ! -x "$MIRROR/edk2/BaseTools/Source/C/bin/VfrCompile" ]; then
  note "building BaseTools host tools"
  # The Tests subdirectory needs a `python` binary that this machine may not
  # have; the C tools we need are built before that stage.
  make -C "$MIRROR/edk2/BaseTools" -j"$(nproc)" || true
fi
[ -x "$MIRROR/edk2/BaseTools/Source/C/bin/GenFw" ] || \
  die "BaseTools host tools are missing (GenFw)"
[ -x "$MIRROR/edk2/BaseTools/Source/C/bin/VfrCompile" ] || \
  die "BaseTools host tools are missing (VfrCompile)"

# ---------------------------------------------------------------------------
# LLVM CROSS_COMPILE wrappers
# ---------------------------------------------------------------------------
note "preparing LLVM wrappers in $LLVM_BIN"
mkdir -p "$LLVM_BIN"
for pair in \
  "llvm-ar:$LLVM_AR" "llvm-objcopy:$OBJCOPY" "llvm-nm:$LLVM_NM" \
  "llvm-strip:$LLVM_STRIP" "llvm-ranlib:$LLVM_RANLIB" "ld.lld:$LD_LLD"; do
  name="${pair%%:*}"
  target="${pair#*:}"
  ln -sfn "$target" "$LLVM_BIN/$name"
done
ln -sfn "$LD_LLD" "$LLVM_BIN/lld"

cat > "$LLVM_BIN/clang" <<EOF
#!/bin/sh
link=1
for a in "\$@"; do
  case "\$a" in
    -c|-S|-E|-M|-MM|-fsyntax-only|-emit-llvm) link=0 ;;
  esac
done
if [ "\$link" = 1 ]; then
  exec "$CLANG" -fuse-ld=lld -no-pie "\$@"
fi
exec "$CLANG" "\$@"
EOF
chmod +x "$LLVM_BIN/clang"
ln -sfn clang "$LLVM_BIN/clang++"

cat > "$LLVM_BIN/aarch64-linux-gnu-gcc" <<EOF
#!/bin/sh
exec "$CLANG" --target=aarch64-none-elf "\$@"
EOF
cat > "$LLVM_BIN/aarch64-linux-gnu-objcopy" <<EOF
#!/bin/sh
exec "$OBJCOPY" "\$@"
EOF
chmod +x "$LLVM_BIN/aarch64-linux-gnu-gcc" "$LLVM_BIN/aarch64-linux-gnu-objcopy"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
cd "$MIRROR"
rm -rf workspace/Build
rm -f edk2/Conf/tools_def.txt

export WORKSPACE="$MIRROR/workspace"
export PACKAGES_PATH="$MIRROR/edk2:$MIRROR/edk2-platforms:$MIRROR"
export EDK_TOOLS_PATH="$MIRROR/edk2/BaseTools"
export CLANG38_BIN="$LLVM_BIN/"
export CLANG38_AARCH64_PREFIX="$LLVM_BIN/llvm-"
export IASL_PREFIX="$(dirname "$IASL")/"
export PYTHON_COMMAND=python3
export PYTHONWARNINGS=ignore
export PATH="$LLVM_BIN:$(dirname "$IASL"):$MIRROR/edk2/BaseTools/BinWrappers/PosixLike:$PATH"

note "building MT6765Pkg (DEBUG_CLANG38)"
set +u
. edk2/edksetup.sh >/dev/null
set -u
build -a AARCH64 -t CLANG38 -b DEBUG -p MT6765Pkg/Devices/a03s.dsc -n "$(nproc)"

FD="$WORKSPACE/Build/MT6765Pkg/DEBUG_CLANG38/FV/MT6765PKG_UEFI.fd"
[ -f "$FD" ] || die "build finished without $FD"
[ "$(wc -c < "$FD" | tr -d ' ')" -eq $((0x200000)) ] || \
  die "$FD is not 2 MiB"

# ---------------------------------------------------------------------------
# Refresh out/
# ---------------------------------------------------------------------------
note "refreshing $OUT"
mkdir -p "$OUT"
cp "$FD" "$OUT/MT6765PKG_UEFI.fd"

note "building BootShim (PAYLOAD_SIZE=0)"
make -B -C "$MIRROR/BootShim" \
  CROSS_COMPILE="$LLVM_BIN/aarch64-linux-gnu-" \
  UEFI_BASE=0x50000000 UEFI_SIZE=0x00200000 \
  XNU_PAYLOAD_BASE=0x60000000 PAYLOAD_SIZE=0 >/dev/null
[ "$(wc -c < "$MIRROR/BootShim/BootShim.bin" | tr -d ' ')" -eq $((0xE0)) ] || \
  die "BootShim is not 0xE0 bytes"
cp "$MIRROR/BootShim/BootShim.bin" "$OUT/BootShim.bin"

cat "$OUT/BootShim.bin" "$OUT/MT6765PKG_UEFI.fd" > "$OUT/UEFI"

echo
echo "firmware  $OUT/MT6765PKG_UEFI.fd ($(wc -c < "$OUT/MT6765PKG_UEFI.fd" | tr -d ' ') bytes)"
echo "shim      $OUT/BootShim.bin ($(wc -c < "$OUT/BootShim.bin" | tr -d ' ') bytes)"
echo "combined  $OUT/UEFI ($(wc -c < "$OUT/UEFI" | tr -d ' ') bytes)"
sha256sum "$OUT/MT6765PKG_UEFI.fd" "$OUT/BootShim.bin" "$OUT/UEFI"
echo
echo "Next: ./make_boot_a03s.sh --dump   (phone in TWRP with adb root)"
