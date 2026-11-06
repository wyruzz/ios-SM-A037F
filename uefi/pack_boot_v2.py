#!/usr/bin/env python3
"""Pack the diagnostic AArch64 shim into an Android boot image v2 (2048-byte pages).

Every field that can silently change the verdict on the phone is either
preserved from the stock image or checked after packing:

  * gzip kernel payload by default. MTK LK decompresses with zlib
    ``inflateInit2(15 + 16)``, i.e. it requires a gzip wrapper; a bare Image
    fails with Z_DATA_ERROR before the first instruction runs.
  * ``--stock`` supplies the cmdline, os_version, load addresses, id, name and
    DTB. Without it the header has an empty cmdline and therefore no
    ``bootopt=64S3``, so LK never enters AArch64.
  * the packed file is re-read with the same offset arithmetic a loader uses,
    the gzip stream is fully inflated and compared byte-for-byte, and the DTB
    bytes are compared with the source. A mismatch aborts before writing.

Still not produced: AVB footer/signature. If vbmeta is enforced the image will
be rejected regardless of everything above.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

PAGE_SIZE = 2048
BOOT_MAGIC = b"ANDROID!"
IMAGE_MAGIC = b"ARM\x64"          # 0x644d5241 little endian at offset 0x38
HEADER_V2_SIZE = 1660
HEADER_V2_VERSION = 2
MAX_PAYLOAD = 32 * 1024 * 1024    # boot partitions on this class of device are 32-64 MiB

KERNEL_ADDR_DEFAULT = 0x40080000
TAGS_ADDR_DEFAULT = 0x40000100
CMDLINE_OFF, CMDLINE_LEN = 64, 512

OFF_KERNEL_SIZE, OFF_KERNEL_ADDR = 8, 12
OFF_RAMDISK_SIZE, OFF_RAMDISK_ADDR = 16, 20
OFF_SECOND_SIZE, OFF_SECOND_ADDR = 24, 28
OFF_TAGS_ADDR = 32
OFF_PAGE_SIZE, OFF_HEADER_VERSION, OFF_OS_VERSION = 36, 40, 44
OFF_RECOVERY_DTBO_SIZE, OFF_RECOVERY_DTBO_OFFSET = 1632, 1636
OFF_HEADER_SIZE, OFF_DTB_SIZE, OFF_DTB_ADDR = 1644, 1648, 1652


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf: bytes, off: int) -> int:
    return struct.unpack_from("<Q", buf, off)[0]


def put_u32(buf: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", buf, off, value)


def align(value: int) -> int:
    return (value + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE


def gzip_payload(data: bytes) -> bytes:
    """Deterministic gzip stream, equivalent to ``gzip -9 -n``.

    zlib is used instead of gzip.compress() because the ``mtime`` keyword only
    exists from Python 3.11 and the stream must be reproducible across hosts.
    """
    compressor = zlib.compressobj(9, zlib.DEFLATED, 15 + 16)
    return compressor.compress(data) + compressor.flush()


def gunzip(stream: bytes) -> bytes:
    decompressor = zlib.decompressobj(15 + 16)
    out = decompressor.decompress(stream) + decompressor.flush()
    if not decompressor.eof:
        raise ValueError("gzip stream is truncated")
    return out


def cmdline_of(header: bytes) -> str:
    return header[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN].split(b"\0", 1)[0].decode(
        "ascii", "replace")


def load_stock(path: Path) -> tuple[bytearray, bytes]:
    raw = path.read_bytes()
    if len(raw) < PAGE_SIZE or raw[:8] != BOOT_MAGIC:
        raise ValueError(f"{path}: not an Android boot image")
    if u32(raw, OFF_PAGE_SIZE) != PAGE_SIZE:
        raise ValueError(f"{path}: page_size is {u32(raw, OFF_PAGE_SIZE)}, need {PAGE_SIZE}")
    if u32(raw, OFF_HEADER_VERSION) != HEADER_V2_VERSION:
        raise ValueError(f"{path}: header_version is {u32(raw, OFF_HEADER_VERSION)}, need 2")
    if u32(raw, OFF_HEADER_SIZE) < HEADER_V2_SIZE:
        raise ValueError(f"{path}: v2 header_size is too small")
    if u32(raw, OFF_HEADER_SIZE) > PAGE_SIZE:
        raise ValueError(f"{path}: header_size exceeds one page")

    kernel_size = u32(raw, OFF_KERNEL_SIZE)
    if PAGE_SIZE + kernel_size > len(raw):
        raise ValueError(f"{path}: kernel payload runs past end of file")

    cursor = align(PAGE_SIZE + kernel_size)
    ramdisk_size = u32(raw, OFF_RAMDISK_SIZE)
    if cursor + ramdisk_size > len(raw):
        raise ValueError(f"{path}: ramdisk payload runs past end of file")
    cursor = align(cursor + ramdisk_size)

    second_size = u32(raw, OFF_SECOND_SIZE)
    if cursor + second_size > len(raw):
        raise ValueError(f"{path}: second-stage payload runs past end of file")
    cursor = align(cursor + second_size)

    recovery_size = u32(raw, OFF_RECOVERY_DTBO_SIZE)
    if recovery_size:
        recovery_off = u64(raw, OFF_RECOVERY_DTBO_OFFSET)
        if recovery_off < cursor or recovery_off + recovery_size > len(raw):
            raise ValueError(f"{path}: recovery_dtbo range is invalid")
        cursor = align(recovery_off + recovery_size)

    dtb = b""
    dtb_size = u32(raw, OFF_DTB_SIZE)
    if dtb_size:
        if cursor + dtb_size > len(raw):
            raise ValueError(f"{path}: DTB range {cursor:#x}+{dtb_size:#x} exceeds file")
        dtb = raw[cursor:cursor + dtb_size]
        if dtb[:4] not in (b"\xd0\x0d\xfe\xed", b"\xed\xfe\x0d\xd0"):
            # Samsung/MTK sometimes ship a table wrapper instead of a bare FDT.
            print(f"WARNING: stock DTB payload at {cursor:#x} does not start with "
                  f"the FDT magic ({dtb[:4].hex()}); copying it verbatim",
                  file=sys.stderr)

    first = raw[PAGE_SIZE:PAGE_SIZE + 2]
    if first != b"\x1f\x8b":
        print(f"WARNING: stock kernel starts with {first.hex()}, not gzip 1f8b; "
              f"confirm this LK accepts a bare Image", file=sys.stderr)

    return bytearray(raw[:PAGE_SIZE]), dtb, raw[PAGE_SIZE:PAGE_SIZE + kernel_size]


def build(header: bytearray, kernel: bytes, dtb: bytes, ramdisk: bytes) -> bytes:
    put_u32(header, OFF_KERNEL_SIZE, len(kernel))
    put_u32(header, OFF_RAMDISK_SIZE, len(ramdisk))
    put_u32(header, OFF_SECOND_SIZE, 0)
    put_u32(header, OFF_PAGE_SIZE, PAGE_SIZE)
    put_u32(header, OFF_HEADER_VERSION, HEADER_V2_VERSION)
    put_u32(header, OFF_RECOVERY_DTBO_SIZE, 0)
    struct.pack_into("<Q", header, OFF_RECOVERY_DTBO_OFFSET, 0)
    put_u32(header, OFF_HEADER_SIZE, HEADER_V2_SIZE)
    put_u32(header, OFF_DTB_SIZE, len(dtb))
    if not dtb:
        # dtb_addr is 64-bit; writing only the low word would keep a stale
        # high word from the stock header.
        struct.pack_into("<Q", header, OFF_DTB_ADDR, 0)
    header[:8] = BOOT_MAGIC

    out = bytearray(header)
    out += kernel
    out += b"\0" * (align(len(kernel)) - len(kernel))
    if ramdisk:
        out += ramdisk
        out += b"\0" * (align(len(ramdisk)) - len(ramdisk))
    if dtb:
        out += dtb
        out += b"\0" * (align(len(dtb)) - len(dtb))
    return bytes(out)


def verify(blob: bytes, kernel: bytes, image: bytes | None, source_dtb: bytes,
           source_ramdisk: bytes, allow_no_bootopt: bool) -> dict:
    """Re-parse the output exactly like a loader would. Raises on any mismatch."""
    if len(blob) < PAGE_SIZE or blob[:8] != BOOT_MAGIC:
        raise ValueError("output lost the ANDROID! magic")
    if u32(blob, OFF_PAGE_SIZE) != PAGE_SIZE:
        raise ValueError("output page_size is not 2048")
    if u32(blob, OFF_HEADER_VERSION) != HEADER_V2_VERSION:
        raise ValueError("output header_version is not 2")
    if u32(blob, OFF_HEADER_SIZE) < HEADER_V2_SIZE:
        raise ValueError("output header_size is shorter than the v2 header")
    if u32(blob, OFF_HEADER_SIZE) > PAGE_SIZE:
        raise ValueError("output header_size exceeds one page")

    kernel_size = u32(blob, OFF_KERNEL_SIZE)
    kernel_off = PAGE_SIZE
    kernel_end = kernel_off + kernel_size
    if kernel_end > len(blob):
        raise ValueError("kernel payload runs past end of file")

    payload = blob[kernel_off:kernel_end]
    if payload != kernel:
        raise ValueError("kernel payload differs from the one that was packed")
    if image is not None:
        if payload[:2] != b"\x1f\x8b":
            raise ValueError("kernel payload is not a gzip stream")
        if gunzip(payload) != image:
            raise ValueError("kernel payload does not inflate to the input Image")

    ramdisk_size = u32(blob, OFF_RAMDISK_SIZE)
    ramdisk_off = align(kernel_end)
    if ramdisk_size != len(source_ramdisk):
        raise ValueError("ramdisk_size in the header does not match the payload")
    if ramdisk_size and blob[ramdisk_off:ramdisk_off + ramdisk_size] != source_ramdisk:
        raise ValueError("ramdisk payload differs from the input")

    second_off = align(ramdisk_off + ramdisk_size)
    if u32(blob, OFF_SECOND_SIZE) != 0:
        raise ValueError("second_size must be zero")

    if u32(blob, OFF_RECOVERY_DTBO_SIZE) != 0:
        raise ValueError("recovery_dtbo_size must be zero")

    dtb_off = align(second_off)
    dtb_size = u32(blob, OFF_DTB_SIZE)
    if dtb_size != len(source_dtb):
        raise ValueError("dtb_size in the header does not match the payload")
    if dtb_size:
        if dtb_off + dtb_size > len(blob):
            raise ValueError("DTB payload does not fit at the loader-computed offset")
        if blob[dtb_off:dtb_off + dtb_size] != source_dtb:
            raise ValueError("DTB payload differs from the source")

    expected_end = align(dtb_off + dtb_size) if dtb_size else align(second_off)
    if len(blob) != expected_end:
        raise ValueError(f"output length {len(blob)} != computed layout end {expected_end}")

    cmdline = cmdline_of(blob)
    if "bootopt=" not in cmdline and not allow_no_bootopt:
        raise ValueError(
            f"cmdline {cmdline!r} has no bootopt=...; LK will not enter AArch64. "
            "Pass --stock or --cmdline (or --allow-no-bootopt if the stock image "
            "genuinely has none)")

    return {
        "file_size": len(blob),
        "kernel_offset": kernel_off,
        "kernel_size": kernel_size,
        "ramdisk_offset": ramdisk_off,
        "ramdisk_size": ramdisk_size,
        "dtb_offset": dtb_off if dtb_size else None,
        "dtb_size": dtb_size,
        "cmdline": cmdline,
        "kernel_addr": u32(blob, OFF_KERNEL_ADDR),
        "tags_addr": u32(blob, OFF_TAGS_ADDR),
        "os_version": u32(blob, OFF_OS_VERSION),
        "sha256": hashlib.sha256(blob).hexdigest(),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", type=Path, default=Path("out/bootshim/Image"),
                    help="uncompressed AArch64 Image from build.sh")
    ap.add_argument("--stock", type=Path,
                    help="stock boot.img: source of cmdline, os_version, addresses, DTB")
    ap.add_argument("--cmdline", help="override cmdline (must contain bootopt=)")
    ap.add_argument("--ramdisk", type=Path,
                    help="optional cpio(.gz) ramdisk; empty by default")
    ap.add_argument("--raw", action="store_true",
                    help="do not gzip the kernel (diagnostic only)")
    ap.add_argument("--stock-kernel", action="store_true",
                    help="reuse the stock kernel payload byte-for-byte (packer sanity check)")
    ap.add_argument("--allow-no-bootopt", action="store_true",
                    help="permit a cmdline without bootopt= (only for stock images that lack it)")
    ap.add_argument("--output", type=Path, default=Path("out/bootshim/boot-shim.img"))
    args = ap.parse_args()

    try:
        ramdisk = args.ramdisk.read_bytes() if args.ramdisk else b""

        if args.stock:
            if args.output.resolve() == args.stock.resolve():
                raise ValueError("output must not overwrite the reference image")
            header, dtb, stock_kernel = load_stock(args.stock)
        else:
            header, dtb, stock_kernel = bytearray(PAGE_SIZE), b"", b""
            put_u32(header, OFF_KERNEL_ADDR, KERNEL_ADDR_DEFAULT)
            put_u32(header, OFF_TAGS_ADDR, TAGS_ADDR_DEFAULT)

        if args.stock_kernel:
            if not args.stock:
                raise ValueError("--stock-kernel needs --stock")
            kernel = stock_kernel
            image = gunzip(stock_kernel) if stock_kernel[:2] == b"\x1f\x8b" else None
            stock_kind = "gzip" if stock_kernel[:2] == b"\x1f\x8b" else "raw"
            print(f"kernel: reusing the stock payload verbatim "
                  f"({len(kernel)} bytes, {stock_kind})", file=sys.stderr)
        else:
            image = args.kernel.read_bytes()
            if len(image) < 0x40 or image[0x38:0x3C] != IMAGE_MAGIC:
                raise ValueError(f"{args.kernel}: no AArch64 Image magic at 0x38")
            # A real Linux Image starts with B.  An edk2 BootShim starts with
            # ADR (it loads the address of the payload appended after itself),
            # followed by a B over the fake header at offset 4.  Accept both;
            # the magic and the gzip round-trip below are the real checks.
            first = u32(image, 0)
            if not ((first & 0x9F000000) == 0x10000000 or
                    (first & 0xFC000000) == 0x14000000):
                raise ValueError(
                    f"Image entry {first:#010x} is neither ADR nor B")
            kernel = gzip_payload(image) if not args.raw else image

        if ramdisk and u32(header, OFF_RAMDISK_ADDR) == 0:
            raise ValueError("a ramdisk was given but ramdisk_addr is unknown; "
                             "pass --stock so the stock load address is preserved")

        if args.cmdline is not None:
            encoded = args.cmdline.encode("ascii")
            if len(encoded) >= CMDLINE_LEN:
                raise ValueError(f"cmdline is {len(encoded)} bytes, limit is {CMDLINE_LEN - 1}")
            header[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN] = encoded.ljust(CMDLINE_LEN, b"\0")

        if args.raw:
            print("WARNING: --raw removes the gzip wrapper. MTK LK decompresses with "
                  "inflateInit2(15+16), which rejects a bare Image with Z_DATA_ERROR.",
                  file=sys.stderr)
        if args.allow_no_bootopt:
            print("WARNING: cmdline without bootopt= is only valid if the stock A03s "
                  "image really has none; LK otherwise stays in AArch32.",
                  file=sys.stderr)

        blob = build(header, kernel, dtb, ramdisk)
        info = verify(blob, kernel, image, dtb, ramdisk,
                      allow_no_bootopt=args.allow_no_bootopt)

        if len(blob) > MAX_PAYLOAD:
            print(f"WARNING: image is {len(blob)} bytes; boot partitions on this "
                  f"device class are usually 32-64 MiB", file=sys.stderr)

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(blob)

        print(f"wrote {args.output} ({info['file_size']} bytes)")
        kind = "gzip" if kernel[:2] == b"\x1f\x8b" else "raw"
        print(f"kernel: {kind}, {info['kernel_size']} bytes "
              f"@ file {info['kernel_offset']:#x} -> 0x{info['kernel_addr']:08x}")
        print(f"ramdisk: {info['ramdisk_size']} bytes @ file {info['ramdisk_offset']:#x}")
        if info["dtb_offset"] is not None:
            print(f"dtb: {info['dtb_size']} bytes @ file {info['dtb_offset']:#x}")
        else:
            print("dtb: none")
        print(f"cmdline: {info['cmdline']!r}")
        print(f"os_version: 0x{info['os_version']:08x}")
        print(f"sha256: {info['sha256']}")
        return 0
    except (OSError, ValueError, struct.error, zlib.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
