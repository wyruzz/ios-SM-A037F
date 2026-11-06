#!/bin/bash
#
# One turn of the automated bring-up loop:
#   the phone is in TWRP -> pull the log of the run that just ended ->
#   run CYCLE_BUILD (if set: it rebuilds and leaves the image where asked) ->
#   flash the image -> clear the recovery request -> reboot into it.
#
# The loader side asks for recovery by itself (boot-arg cycle=SECONDS), so after
# the first cold boot no one has to touch the phone.
#
# usage: IMG=out/boot-uefi-a03s-sbNNN.img [CYCLE_BUILD='./build_cycle.sh'] ./auto_cycle.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="${IMG:-${1:-}}"

[ -n "$IMG" ] || { echo "usage: IMG=image.img $0   (or $0 image.img)" >&2; exit 2; }
[ -f "$IMG" ] || { echo "image not found: $IMG" >&2; exit 2; }

echo "== waiting for TWRP =="
adb wait-for-device
for i in $(seq 1 120); do
	[ "$(adb shell id -u 2>/dev/null | tr -d '\r')" = "0" ] && break
	sleep 2
done
[ "$(adb shell id -u 2>/dev/null | tr -d '\r')" = "0" ] || { echo "TWRP did not come up" >&2; exit 1; }

echo "== pulling the log of the last run =="
"$ROOT/pull_log.sh"

if [ -n "${CYCLE_BUILD:-}" ]; then
	echo "== building: $CYCLE_BUILD =="
	bash -c "$CYCLE_BUILD"
	[ -f "$IMG" ] || { echo "build did not produce $IMG" >&2; exit 1; }
fi

echo "== clearing the recovery request =="
adb shell 'dd if=/dev/zero of=/dev/block/by-name/misc bs=512 count=1 >/dev/null 2>&1; sync'

echo "== flashing $IMG =="
IMG="$IMG" A03S_ALLOW_ANY=1 "$ROOT/flash_a03s.sh" --adb

echo "== rebooting into it =="
adb reboot
