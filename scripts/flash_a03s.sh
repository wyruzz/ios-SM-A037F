#!/bin/bash
#
# Flash boot-uefi-a03s.img to the Galaxy A03s (SM-A037F) boot partition.
#
# Two transports are supported:
#
#   TWRP / ADB   root adbd in TWRP; dd straight to /dev/block/by-name/boot.
#                The current boot partition is backed up first and the write
#                is read back and compared byte for byte.
#   heimdall     download mode; heimdall 2.2.2 has no dump action, so the
#                rollback is the backup image you already keep on the host.
#
# Usage:
#   ./flash_a03s.sh              # TWRP/ADB if a device is there, else heimdall
#   ./flash_a03s.sh --adb        # force TWRP/ADB
#   ./flash_a03s.sh --heimdall   # force download mode
#
# Environment:
#   IMG=/path/to/boot.img        image to flash (default: ./boot-uefi-a03s.img)
#   A03S_ALLOW_ANY=1             skip the SM-A037F model check
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="${IMG:-$ROOT/boot-uefi-a03s.img}"
BOOT_PART="/dev/block/by-name/boot"
BOOT_FALLBACK="/dev/block/mmcblk0p37"
PARTITION_SIZE=33554432          # 32 MiB
MODE="auto"

for arg in "$@"; do
  case "$arg" in
    --adb)      MODE="adb" ;;
    --heimdall) MODE="heimdall" ;;
    -h|--help)  sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "error: unknown argument: $arg" >&2; exit 2 ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "image not found: $IMG"
SIZE="$(wc -c < "$IMG" | tr -d ' ')"
[ "$SIZE" -le "$PARTITION_SIZE" ] || \
  die "$IMG is $SIZE bytes, larger than the $PARTITION_SIZE-byte boot partition"
IMG_HASH="$(shasum -a 256 "$IMG" | cut -d' ' -f1)"
echo "image   $IMG"
echo "        $SIZE bytes, sha256 $IMG_HASH"

# ---------------------------------------------------------------------------
# TWRP / ADB
# ---------------------------------------------------------------------------
flash_adb() {
  adb get-state >/dev/null 2>&1 || die "no ADB device (connect the phone in TWRP)"
  [ "$(adb shell id -u 2>/dev/null | tr -d '\r')" = "0" ] || \
    die "adb shell is not root; boot TWRP, not Android"

  local device model twrp boot
  device="$(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r')"
  model="$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
  twrp="$(adb shell getprop ro.twrp.version 2>/dev/null | tr -d '\r')"
  echo "device  $model ($device), TWRP ${twrp:-unknown}"

  if [ -z "$twrp" ] && ! adb shell '[ -x /sbin/recovery ]' >/dev/null 2>&1; then
    die "this does not look like TWRP (no ro.twrp.version, no /sbin/recovery)"
  fi
  if [ "${A03S_ALLOW_ANY:-0}" != "1" ] &&
     ! printf '%s %s' "$device" "$model" | grep -qi 'a03\|a037'; then
    die "device is '$model' ($device), expected SM-A037F/a03s (set A03S_ALLOW_ANY=1 to override)"
  fi

  boot="$(adb shell "if [ -e $BOOT_PART ]; then echo $BOOT_PART; else echo $BOOT_FALLBACK; fi" 2>/dev/null | tr -d '\r')"
  [ -n "$boot" ] || die "cannot locate the boot partition"
  echo "boot    $boot"

  # --- backup -------------------------------------------------------------
  local backup
  backup="$ROOT/boot-backup-$(date +%Y%m%d-%H%M%S).img"
  echo "==> backing up $boot to $backup"
  adb exec-out "dd if=$boot bs=4096 count=$((PARTITION_SIZE / 4096)) 2>/dev/null" > "$backup"
  [ "$(wc -c < "$backup" | tr -d ' ')" -eq "$PARTITION_SIZE" ] || \
    die "backup is not $PARTITION_SIZE bytes; nothing was written"
  echo "    backup sha256 $(shasum -a 256 "$backup" | cut -d' ' -f1)"

  # --- write --------------------------------------------------------------
  echo "==> pushing the image to /tmp/boot-new.img"
  adb push "$IMG" /tmp/boot-new.img >/dev/null
  [ "$(adb shell 'wc -c < /tmp/boot-new.img' 2>/dev/null | tr -d '\r')" = "$SIZE" ] || \
    die "push size mismatch; nothing was written"

  echo "==> writing $boot"
  adb shell "dd if=/tmp/boot-new.img of=$boot bs=4096 2>&1" | tail -3
  adb shell sync
  adb shell rm -f /tmp/boot-new.img

  # --- read back ----------------------------------------------------------
  local count readback
  count=$(( (SIZE + 4095) / 4096 ))
  readback="$(mktemp "${TMPDIR:-/tmp}/a03s-readback.XXXXXX")"
  trap 'rm -f "$readback"' RETURN
  echo "==> reading back $count blocks from $boot"
  adb exec-out "dd if=$boot bs=4096 count=$count 2>/dev/null" > "$readback"
  head -c "$SIZE" "$readback" > "$readback.trim"
  local got
  got="$(shasum -a 256 "$readback.trim" | cut -d' ' -f1)"
  rm -f "$readback.trim"
  [ "$got" = "$IMG_HASH" ] || die "read-back hash $got does not match $IMG_HASH"
  echo "    read-back sha256 $got"
  echo
  echo "OK: $boot matches the image.  Reboot the phone to test it."
}

# ---------------------------------------------------------------------------
# heimdall
# ---------------------------------------------------------------------------
flash_heimdall() {
  heimdall detect >/dev/null 2>&1 || \
    die "no download-mode device (hold Vol- while connecting USB, or use TWRP/ADB)"
  echo "==> flashing --boot with heimdall"
  # Partition flag is lowercase on this PIT: --boot, not --BOOT.
  heimdall flash --boot "$IMG"
  echo
  echo "OK: heimdall reported success.  This version has no dump action, so"
  echo "    the rollback is the boot-backup-*.img you already keep on the host."
}

case "$MODE" in
  adb)      flash_adb ;;
  heimdall) flash_heimdall ;;
  auto)
    if adb get-state >/dev/null 2>&1; then
      flash_adb
    elif heimdall detect >/dev/null 2>&1; then
      flash_heimdall
    else
      die "no phone found: connect it in TWRP (ADB) or download mode"
    fi
    ;;
esac
