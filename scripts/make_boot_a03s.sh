#!/bin/bash
#
# Build a flashable HakiOS A03s boot.img from the phone's current boot
# partition.
#
# Why this exists:
#   The source archive does not ship stock-boot.img, the XNU kernelcache or
#   the ramdisk.  The phone, however, already runs a working HakiOS UEFI
#   image whose boot partition contains all three: the Android v2 header,
#   the A03s DTB and the XNUP payload (kernel.lz4 + ramdisk).  Dumping that
#   partition is enough to rebuild the image with the new UEFI firmware.
#
# Usage:
#   ./make_boot_a03s.sh --dump              # dump boot via TWRP/ADB, rebuild
#   ./make_boot_a03s.sh --stock boot.img    # use an existing dump
#   ./make_boot_a03s.sh --dump --flash      # rebuild and flash in one go
#   ./make_boot_a03s.sh --uefi-only         # no XNUP: menu/Shell test image
#
# Environment:
#   A03S_TOOLCHAIN   xpack aarch64-none-elf toolchain directory
#   A03S_LLVM_BIN    directory with clang/llvm-objcopy (LLVM fallback)
#   ADB_SERIAL       adb serial when several devices are connected
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2="$ROOT/edk2-mt6765"
OUT="$ROOT/out"
BOOT_PART="/dev/block/by-name/boot"
BOOT_FALLBACK="/dev/block/mmcblk0p37"
PARTITION_SIZE=33554432          # 32 MiB
PAYLOAD_OFF=0x2000E0             # BootShim (0xE0) + UEFI FD (0x200000)
MODE=""
STOCK=""
FLASH=0
UEFI_ONLY=0

die() { echo "error: $*" >&2; exit 1; }
note() { echo "==> $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dump)       MODE="dump" ;;
    --stock)      MODE="stock"; shift; STOCK="${1:-}" ;;
    --stock=*)    MODE="stock"; STOCK="${1#--stock=}" ;;
    --flash)      FLASH=1 ;;
    --uefi-only)  UEFI_ONLY=1 ;;
    -h|--help)    sed -n '2,23p' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

[ -n "$MODE" ] || die "choose --dump or --stock boot.img (see --help)"
if [ "$MODE" = "stock" ]; then
  [ -n "$STOCK" ] || die "--stock needs a path to a boot.img dump"
fi
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# Locate the AArch64 toolchain
# ---------------------------------------------------------------------------
find_toolchain() {
  local candidates=(
    "${A03S_TOOLCHAIN:-}"
    "$ROOT/toolchain/xpack-aarch64-none-elf-gcc-13.2.1-1.1"
    "/tmp/opencode/toolchain/xpack-aarch64-none-elf-gcc-13.2.1-1.1"
    "$HOME/xpack-aarch64-none-elf-gcc-13.2.1-1.1"
    "$HOME/aarch64-none-elf-gcc"
  )
  local c
  for c in "${candidates[@]}"; do
    [ -n "$c" ] || continue
    if [ -x "$c/bin/aarch64-none-elf-gcc" ]; then
      echo "$c/bin/aarch64-none-elf-"
      return 0
    fi
    if [ -x "$c/aarch64-none-elf-gcc" ]; then
      echo "$c/aarch64-none-elf-"
      return 0
    fi
  done
  if command -v aarch64-none-elf-gcc >/dev/null 2>&1; then
    echo "$(dirname "$(command -v aarch64-none-elf-gcc)")/aarch64-none-elf-"
    return 0
  fi
  return 1
}

# ---------------------------------------------------------------------------
# LLVM fallback
#
# The BootShim is a single assembly file, so Clang can build it without a
# full cross-GCC installation.  Generate a tiny CROSS_COMPILE-style wrapper
# directory so the existing Makefile keeps working unchanged.
# ---------------------------------------------------------------------------
find_llvm_tool() {
  local names=("$@") d
  for d in ${A03S_LLVM_BIN:+"$A03S_LLVM_BIN"}; do
    for n in "${names[@]}"; do
      [ -x "$d/$n" ] && { echo "$d/$n"; return 0; }
    done
  done
  local n
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

make_llvm_toolchain() {
  local clang objcopy dir
  clang="$(find_llvm_tool clang)" || return 1
  objcopy="$(find_llvm_tool llvm-objcopy)" || return 1

  dir="$ROOT/toolchain/llvm-aarch64/bin"
  mkdir -p "$dir"
  cat > "$dir/aarch64-none-elf-gcc" <<EOF
#!/bin/sh
exec "$clang" --target=aarch64-none-elf "\$@"
EOF
  cat > "$dir/aarch64-none-elf-objcopy" <<EOF
#!/bin/sh
exec "$objcopy" "\$@"
EOF
  chmod +x "$dir/aarch64-none-elf-gcc" "$dir/aarch64-none-elf-objcopy"
  echo "$dir/aarch64-none-elf-"
}

find_cross_prefix() {
  local prefix
  if prefix="$(find_toolchain)"; then
    echo "$prefix"
    return 0
  fi
  if prefix="$(make_llvm_toolchain)"; then
    echo "$prefix"
    return 0
  fi
  return 1
}

# ---------------------------------------------------------------------------
# Dump the boot partition through TWRP/ADB
# ---------------------------------------------------------------------------
dump_boot() {
  local adb=(adb)
  [ -n "${ADB_SERIAL:-}" ] && adb=(adb -s "$ADB_SERIAL")

  "${adb[@]}" get-state >/dev/null 2>&1 || \
    die "no ADB device: connect the phone in TWRP (adb enabled)"
  [ "$("${adb[@]}" shell id -u 2>/dev/null | tr -d '\r')" = "0" ] || \
    die "adb shell is not root; boot TWRP, not Android"

  local twrp model device
  twrp="$("${adb[@]}" shell getprop ro.twrp.version 2>/dev/null | tr -d '\r')"
  model="$("${adb[@]}" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
  device="$("${adb[@]}" shell getprop ro.product.device 2>/dev/null | tr -d '\r')"
  echo "device  $model ($device), TWRP ${twrp:-unknown}"
  if [ -z "$twrp" ] && ! "${adb[@]}" shell '[ -x /sbin/recovery ]' >/dev/null 2>&1; then
    die "this does not look like TWRP (no ro.twrp.version, no /sbin/recovery)"
  fi

  local boot
  boot="$("${adb[@]}" shell "if [ -e $BOOT_PART ]; then echo $BOOT_PART; else echo $BOOT_FALLBACK; fi" 2>/dev/null | tr -d '\r')"
  [ -n "$boot" ] || die "cannot locate the boot partition"

  STOCK="$OUT/boot-dump-$(date +%Y%m%d-%H%M%S).img"
  note "dumping $boot to $STOCK"
  # exec-out is binary-safe; plain `adb shell` would translate LF to CRLF.
  "${adb[@]}" exec-out "dd if=$boot bs=4096 count=$((PARTITION_SIZE / 4096)) 2>/dev/null" > "$STOCK"
  local size
  size="$(wc -c < "$STOCK" | tr -d ' ')"
  [ "$size" -eq "$PARTITION_SIZE" ] || \
    die "dump is $size bytes, expected $PARTITION_SIZE; nothing was built"
  [ "$(head -c 8 "$STOCK")" = "ANDROID!" ] || \
    die "$STOCK does not start with the Android boot magic"
  echo "    sha256 $(sha256sum "$STOCK" | cut -d' ' -f1)"
}

# ---------------------------------------------------------------------------
# Extract the XNUP payload from the dumped kernel
# ---------------------------------------------------------------------------
extract_payload() {
  local stock="$1" payload="$2" dump_json="$3"
  python3 - "$stock" "$payload" "$dump_json" "$PAYLOAD_OFF" <<'PY'
import json, struct, sys, zlib
from pathlib import Path

stock, payload_path, json_path, payload_off = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
raw = Path(stock).read_bytes()
if raw[:8] != b"ANDROID!":
    sys.exit("not an Android boot image")
kernel_size = struct.unpack_from("<I", raw, 8)[0]
kernel = raw[2048:2048 + kernel_size]
if kernel[:2] == b"\x1f\x8b":
    # Tolerate zero padding after the gzip stream: some boot images round
    # kernel_size up, and gzip.decompress() rejects trailing bytes.
    d = zlib.decompressobj(15 + 16)
    image = d.decompress(kernel) + d.flush()
    if not d.eof:
        sys.exit("kernel gzip stream is truncated")
else:
    image = kernel
if image[0x38:0x3C] != b"ARM\x64":
    sys.exit("decompressed kernel is not an AArch64 Image (no ARM\\x64 magic)")

def align16(v):
    return (v + 15) // 16 * 16

def parse(at):
    if image[at:at + 4] != b"XNUP":
        return None
    body = struct.unpack_from("<I", image, at + 4)[0]
    end = at + align16(8 + body)
    if end > len(image):
        return None
    entries, pos, stop = [], at + 8, at + 8 + body
    while pos < stop:
        if pos + 24 > stop:
            return None
        name = image[pos:pos + 16].split(b"\0", 1)[0].decode("ascii", "replace")
        size = struct.unpack_from("<I", image, pos + 16)[0]
        data_off = pos + 24
        if data_off + size > stop:
            return None
        entries.append((name, size))
        pos = data_off + align16(size)
    if not entries:
        return None
    return end, entries

candidates = []
if image[payload_off:payload_off + 4] == b"XNUP":
    candidates.append(payload_off)
start = 0
while True:
    at = image.find(b"XNUP", start)
    if at < 0:
        break
    if at != payload_off:
        candidates.append(at)
    start = at + 4

chosen = None
for at in candidates:
    parsed = parse(at)
    if parsed is None:
        continue
    end, entries = parsed
    # The payload is the last thing in the kernel: shim + FD + XNUP.
    if end == len(image) or payload_off in candidates and at == payload_off:
        chosen = (at, end, entries)
        break
if chosen is None:
    sys.exit("no valid XNUP payload found in the dumped kernel")

at, end, entries = chosen
blob = image[at:end]
Path(payload_path).write_bytes(blob)
Path(json_path).write_text(json.dumps({
    "offset": at,
    "size": len(blob),
    "entries": entries,
    "kernel_image_size": len(image),
}, indent=2) + "\n")
print(f"payload  XNUP at 0x{at:x}, {len(blob)} bytes")
for name, size in entries:
    print(f"         {name}: {size} bytes")
if not any(name == "kernel.lz4" for name, _ in entries):
    sys.exit("XNUP has no kernel.lz4 entry")
PY
}

# ---------------------------------------------------------------------------
# Build the new image
# ---------------------------------------------------------------------------
build_image() {
  local payload="$1"
  local prefix fd shim kernel
  prefix="$(find_cross_prefix)" || \
    die "no AArch64 toolchain found; set A03S_TOOLCHAIN (xpack) or A03S_LLVM_BIN (clang/llvm-objcopy)"
  fd="$OUT/MT6765PKG_UEFI.fd"
  [ -f "$fd" ] || die "$fd is missing; build the firmware first"
  [ "$(wc -c < "$fd" | tr -d ' ')" -eq $((0x200000)) ] || \
    die "$fd is not 2 MiB"

  local payload_size=0
  if [ -n "$payload" ]; then
    payload_size="$(wc -c < "$payload" | tr -d ' ')"
  fi

  note "building BootShim (payload $payload_size bytes)"
  make -B -C "$EDK2/BootShim" \
    CROSS_COMPILE="$prefix" \
    UEFI_BASE=0x50000000 UEFI_SIZE=0x00200000 \
    XNU_PAYLOAD_BASE=0x60000000 PAYLOAD_SIZE="$payload_size" >/dev/null
  shim="$EDK2/BootShim/BootShim.bin"
  [ "$(wc -c < "$shim" | tr -d ' ')" -eq $((0xE0)) ] || \
    die "$shim is not 0xE0 bytes"

  kernel="$OUT/kernel-uefi-a03s.bin"
  if [ -n "$payload" ]; then
    cat "$shim" "$fd" "$payload" > "$kernel"
  else
    cat "$shim" "$fd" > "$kernel"
  fi
  echo "kernel   $(wc -c < "$kernel" | tr -d ' ') bytes (shim + FD + payload)"

  local stub="$OUT/ramdisk-stub.bin"
  printf '\0\0\0\0\0\0\0\0' > "$stub"

  note "packing boot-uefi-a03s.img"
  python3 "$ROOT/pack_boot_v2.py" \
    --kernel "$kernel" \
    --stock "$STOCK" \
    --ramdisk "$stub" \
    --output "$OUT/boot-uefi-a03s.img"

  note "verifying boot-uefi-a03s.img"
  python3 "$ROOT/verify_boot_img.py" "$OUT/boot-uefi-a03s.img"

  local size
  size="$(wc -c < "$OUT/boot-uefi-a03s.img" | tr -d ' ')"
  [ "$size" -le "$PARTITION_SIZE" ] || \
    die "image is $size bytes, larger than the $PARTITION_SIZE-byte boot partition"
  echo
  echo "OK: $OUT/boot-uefi-a03s.img ($size bytes)"
  echo "    sha256 $(sha256sum "$OUT/boot-uefi-a03s.img" | cut -d' ' -f1)"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
case "$MODE" in
  dump) dump_boot ;;
  stock)
    [ -f "$STOCK" ] || die "stock image not found: $STOCK"
    ;;
esac

PAYLOAD="$OUT/payload-from-dump.bin"
rm -f "$PAYLOAD"
if [ "$UEFI_ONLY" = "1" ]; then
  note "--uefi-only: building a menu/Shell test image without XNU"
  build_image ""
else
  note "extracting the XNUP payload from $STOCK"
  extract_payload "$STOCK" "$PAYLOAD" "$OUT/payload-from-dump.json"
  build_image "$PAYLOAD"
fi

if [ "$FLASH" = "1" ]; then
  note "flashing via TWRP/ADB"
  IMG="$OUT/boot-uefi-a03s.img" "$ROOT/flash_a03s.sh" --adb
else
  echo
  echo "Next: boot TWRP and run"
  echo "  IMG=$OUT/boot-uefi-a03s.img $ROOT/flash_a03s.sh --adb"
fi
