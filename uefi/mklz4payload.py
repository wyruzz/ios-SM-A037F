#!/usr/bin/env python3
"""Build the XNUP payload that carries the iOS kernelcache.

The entry is ``kernel.lz4``::

    u64 uncompressed_size (little endian)
    LZ4 legacy stream produced by ``lz4 -l``

The stream is decompressed again and compared with the input before the
payload is written, so a broken compressor cannot reach the boot image.

usage: mklz4payload.py --kernel kernelcache --output payload.bin [--cmdline "..."] [--ramdisk image]
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import lz4legacy

XNUP_MAGIC = 0x50554E58  # "XNUP"


def add_entry(body: bytearray, name: str, data: bytes) -> None:
    body += struct.pack("<16sII", name.encode(), len(data), 0)
    body += data
    body += b"\0" * (-len(data) % 16)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", type=Path, required=True,
                    help="raw arm64 Mach-O kernelcache")
    ap.add_argument("--output", type=Path, required=True,
                    help="XNUP payload to write")
    ap.add_argument("--lz4", default="lz4", help="lz4 command (default: lz4)")
    ap.add_argument("--level", default="9", help="lz4 compression level (default: 9)")
    ap.add_argument("--cmdline", default="", help="optional XNU command line")
    ap.add_argument("--checkpoint", type=int, default=None,
                    help="stop the kernel at bring-up checkpoint N and return to the loader")
    ap.add_argument("--checkpoints", default=None,
                    help="comma-separated checkpoint list: run the kernel once per checkpoint")
    ap.add_argument("--ramdisk", type=Path, default=None,
                    help="image to hand to XNU as the RAM disk (/dev/md0, the root with rd=md0)")
    ap.add_argument("--rdsplit", type=lambda v: int(v, 0), default=None,
                    help="offset in --ramdisk where a second RAM disk (/dev/md1) begins; hex or decimal")
    ap.add_argument("--rdres", type=lambda v: int(v, 0), default=None,
                    help="size of a resident root image already in RAM (QEMU); hex or decimal")
    ap.add_argument("--usb", action="store_true",
                    help="stay in the loader and take commands over USB (the \"usb\" entry)")
    args = ap.parse_args()
    if args.checkpoint is not None and not 0 <= args.checkpoint <= 999:
        print("error: --checkpoint must be between 0 and 999", file=sys.stderr)
        return 2
    if args.checkpoints is not None:
        try:
            cps = [int(v) for v in args.checkpoints.replace(" ", "").split(",") if v]
        except ValueError:
            print("error: --checkpoints must be a comma-separated list of integers",
                  file=sys.stderr)
            return 2
        if not cps or any(not 0 <= v <= 999 for v in cps):
            print("error: --checkpoints values must be between 0 and 999", file=sys.stderr)
            return 2

    raw = args.kernel.read_bytes()
    if len(raw) < 0x40:
        print(f"error: {args.kernel} is too small to be a Mach-O", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        compressed = Path(tmp) / "kernel.lz4"
        run = subprocess.run(
            [args.lz4, "-l", f"-{args.level}", "-f",
             str(args.kernel), str(compressed)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if run.returncode != 0:
            sys.stderr.write(run.stderr.decode("utf-8", "replace"))
            print(f"error: {args.lz4} failed with status {run.returncode}",
                  file=sys.stderr)
            return 2
        stream = compressed.read_bytes()

    # Never trust the compressor: expand it and compare byte for byte.
    try:
        expanded = lz4legacy.decompress(stream, len(raw))
    except ValueError as exc:
        print(f"error: generated LZ4 stream is invalid: {exc}", file=sys.stderr)
        return 2
    if hashlib.sha256(expanded).digest() != hashlib.sha256(raw).digest():
        print("error: LZ4 round-trip mismatch", file=sys.stderr)
        return 2

    entry = struct.pack("<Q", len(raw)) + stream
    body = bytearray()
    add_entry(body, "kernel.lz4", entry)
    rd_size = 0
    if args.ramdisk is not None:
        rd = args.ramdisk.read_bytes()
        rd_size = len(rd)
        if rd_size < 512:
            print(f"error: {args.ramdisk} is too small to be a RAM disk", file=sys.stderr)
            return 2
        add_entry(body, "ramdisk", rd)
    if args.rdsplit is not None:
        if rd_size == 0:
            print("error: --rdsplit needs --ramdisk", file=sys.stderr)
            return 2
        if not 0 < args.rdsplit < rd_size or args.rdsplit % 4096 != 0:
            print(f"error: --rdsplit {args.rdsplit:x} is not a 4 KB boundary inside the "
                  f"{rd_size:x}-byte ramdisk", file=sys.stderr)
            return 2
        add_entry(body, "rdsplit", format(args.rdsplit, "x").encode("ascii") + b"\0")
    if args.cmdline:
        add_entry(body, "cmdline", args.cmdline.encode("ascii") + b"\0")
    if args.rdres is not None:
        add_entry(body, "rdres", format(args.rdres, "x").encode("ascii") + b"\0")
    if args.usb:
        add_entry(body, "usb", b"")
    if args.checkpoint is not None:
        add_entry(body, "checkpoint", str(args.checkpoint).encode("ascii") + b"\0")
    if args.checkpoints is not None:
        add_entry(body, "checkpoints",
                  ",".join(str(v) for v in cps).encode("ascii") + b"\0")
    body += b"\0" * 24  # empty entry terminates the list

    payload = struct.pack("<II", XNUP_MAGIC, len(body)) + bytes(body)
    payload += b"\0" * (-len(payload) % 16)  # BootShim copies 16 bytes at a time

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)

    print(f"{args.output}: {len(payload)} bytes "
          f"(kernel {len(raw)} -> {len(stream)} bytes LZ4)")
    print(f"  kernel sha256  {hashlib.sha256(raw).hexdigest()}")
    print(f"  payload sha256 {hashlib.sha256(payload).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
