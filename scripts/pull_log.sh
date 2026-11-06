#!/bin/bash
# Pull the A03s persistent log from the tail of the boot partition.
# The loader writes its log (and the kernel log window) 24 MiB into the
# 32 MiB boot partition; a dump of that partition is all we need.
set -euo pipefail
OUT="${1:-hakios-boot.log}"
TMP="boot-tail-$(date +%Y%m%d-%H%M%S).img"

echo "==> dumping the boot partition over ADB (TWRP)"
adb shell 'dd if=/dev/block/by-name/boot of=/tmp/boot-dump.img bs=1M' >/dev/null
adb pull /tmp/boot-dump.img "$TMP" >/dev/null
adb shell 'rm -f /tmp/boot-dump.img' >/dev/null || true

echo "==> extracting the log at 24 MiB"
dd if="$TMP" of="$OUT" bs=1M skip=24 count=8 status=none
echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
echo "--- first lines ---"
head -40 "$OUT" || true
