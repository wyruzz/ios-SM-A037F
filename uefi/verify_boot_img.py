#!/usr/bin/env python3
"""Say what is actually inside a boot.img, before it goes to the phone.

    python3 verify_boot_img.py boot.img

Walks the file with the same offset arithmetic a loader uses, then inspects the
decompressed AArch64 Image:

  * header: magic, page size, header version, kernel/ramdisk/dtb bounds,
    cmdline containing ``bootopt=`` (without it LK stays in AArch32),
  * payload: gzip wrapper, full inflate, ``ARM\\x64`` magic, image_size,
  * entry point: the ``b`` in code0, the WDT-disable sequence at that exact
    address, the canary ``wfe`` loop, ``msr scr_el3`` anywhere in the text.

The last two are what tell a patched build from an old one: a file can be
freshly copied and still contain the original code. Verdict is about content,
never about the file's timestamp.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

import lz4legacy

PAGE = 2048
MAGIC = b"ANDROID!"
IMAGE_MAGIC = b"ARM\x64"          # 0x644d5241, offset 0x38 of an AArch64 Image

# AArch64 encodings, little endian.
WDT_SEQ = (
    0xD28E0009,   # mov  x9, #0x7000
    0xF2A20009,   # movk x9, #0x1000, lsl #16      -> 0x10007000
    0x52A4400A,   # movz w10, #0x2200, lsl #16    -> 0x22000000 (KEY | !EN)
    0xB900012A,   # str  w10, [x9]
)
# edk2 BootShim.S: the same disable, written as a 7/8-register pair because
# the literal pool would otherwise land between _Payload and the appended FD.
WDT_SEQ_UEFI = (
    0xD2A20007,   # mov  x7, #0x10000000
    0x91401CE7,   # add  x7, x7, #0x7000
    0x52A44008,   # movz w8, #0x2200, lsl #16
    0xB90000E8,   # str  w8, [x7]
)
MSR_DAIFSET = 0xD5034FDF
DSB_SY = 0xD5033F9F
WFE = 0xD503205F
B_SELF = 0x17FFFFFF
SCR_EL3 = 0xD51E1109              # msr scr_el3, x9
AVB_MAGIC = b"AVBf"

FDT_MAGICS = (b"\xd0\x0d\xfe\xed", b"\xed\xfe\x0d\xd0")


def u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def u64(b: bytes, o: int) -> int:
    return struct.unpack_from("<Q", b, o)[0]


def align(v: int) -> int:
    return (v + PAGE - 1) // PAGE * PAGE


def align16(v: int) -> int:
    return (v + 15) // 16 * 16


def xnup_entries(data: bytes) -> list[tuple[str, bytes]]:
    """Parse an XNUP container; raises ValueError on a malformed one."""
    if len(data) < 8 or data[:4] != b"XNUP":
        raise ValueError("not an XNUP container")
    body_size = u32(data, 4)
    if 8 + body_size > len(data):
        raise ValueError("XNUP body runs past the end of the payload")

    entries: list[tuple[str, bytes]] = []
    pos = 8
    end = 8 + body_size
    while pos + 24 <= end:
        raw_name, size, _ = struct.unpack_from("<16sII", data, pos)
        name = raw_name.split(b"\0", 1)[0].decode("ascii", "replace")
        if not name:
            break
        start = pos + 24
        if start + size > end:
            raise ValueError(f"XNUP entry {name!r} runs past the body")
        entries.append((name, data[start:start + size]))
        pos = start + align16(size)
    return entries


def is_back_branch(word: int) -> bool:
    """A B instruction that jumps backwards a few instructions (a spin loop)."""
    if word & 0xFC000000 != 0x14000000:
        return False
    imm = word & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 0x04000000
    return -64 <= imm < 0


def is_movz(word: int, imm16: int, hw: int) -> bool:
    """MOVZ Xd, #imm16, lsl #(16*hw) (sf=1)."""
    return ((word & 0xFF800000) == 0xD2800000
            and ((word >> 21) & 0x3) == hw
            and ((word >> 5) & 0xFFFF) == imm16)


def is_movk(word: int, imm16: int, hw: int) -> bool:
    """MOVK Xd, #imm16, lsl #(16*hw) (sf=1)."""
    return ((word & 0xFF800000) == 0xF2800000
            and ((word >> 21) & 0x3) == hw
            and ((word >> 5) & 0xFFFF) == imm16)


def find_seq(words: tuple, seq: tuple) -> list[int]:
    """All word indices where seq occurs. Plain tuple slicing, no window games."""
    n, m = len(words), len(seq)
    return [i for i in range(n - m + 1) if words[i:i + m] == seq]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Say what is actually inside a boot.img, before it goes to the phone.")
    ap.add_argument("image", type=Path)
    ap.add_argument("--kernelcache", type=Path,
                    help="verify that the appended kernel.lz4 payload expands "
                         "to this kernelcache")
    args = ap.parse_args()

    path = args.image
    try:
        blob = path.read_bytes()
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    problems: list[str] = []

    def check(ok: bool, message: str) -> bool:
        print(f"  {'ok  ' if ok else 'FAIL'}  {message}")
        if not ok:
            problems.append(message)
        return ok

    if len(blob) < PAGE or blob[:8] != MAGIC:
        print(f"{path}: not an Android boot image (magic {blob[:8]!r})")
        return 1

    page = u32(blob, 36)
    version = u32(blob, 40)
    header_size = u32(blob, 1644)
    kernel_size = u32(blob, 8)
    kernel_addr = u32(blob, 12)
    ramdisk_size = u32(blob, 16)
    ramdisk_addr = u32(blob, 20)
    dtb_size = u32(blob, 1648)
    cmdline = blob[64:576].split(b"\0", 1)[0].decode("ascii", "replace")

    print(f"file      {path}")
    print(f"          {len(blob)} bytes, sha256 {hashlib.sha256(blob).hexdigest()}")
    print(f"header    v{version}, page {page}, header_size {header_size}")
    print(f"kernel    {kernel_size} bytes -> 0x{kernel_addr:08x}")
    print(f"ramdisk   {ramdisk_size} bytes -> 0x{ramdisk_addr:08x}")
    print(f"dtb       {dtb_size} bytes")
    print(f"cmdline   {cmdline!r}")
    print()
    print(f"checks for {path.name}:")

    # ---- header -----------------------------------------------------------
    check(version == 2, f"header_version is 2 (got {version})")
    check(page == PAGE, f"page_size is {PAGE} (got {page})")
    check(header_size >= 1660 and header_size <= PAGE,
          f"header_size fits the v2 header (got {header_size})")
    check("bootopt=" in cmdline,
          "cmdline carries bootopt= (LK enters AArch64 only with it)")

    # ---- kernel payload at the loader-computed offsets ---------------------
    kernel_off = PAGE
    kernel_end = kernel_off + kernel_size
    if not check(kernel_end <= len(blob), "kernel payload fits in the file"):
        return 1
    payload = blob[kernel_off:kernel_end]

    if payload[:2] != b"\x1f\x8b":
        check(False, f"kernel payload is gzip (starts {payload[:2].hex()}); "
                     f"LK inflateInit2(15+16) rejects a bare Image")
        return 1
    check(True, "kernel payload is gzip")

    try:
        inflater = zlib.decompressobj(15 + 16)
        image = inflater.decompress(payload) + inflater.flush()
    except zlib.error as exc:
        check(False, f"gzip inflates ({exc})")
        return 1
    check(inflater.eof, "gzip stream is complete")
    check(not inflater.unused_data,
          f"no trailing bytes after the gzip stream "
          f"({len(inflater.unused_data)} found)")
    print(f"  ....  payload inflates to {len(image)} bytes")

    ramdisk_off = align(kernel_end)
    ramdisk_end = ramdisk_off + ramdisk_size
    check(ramdisk_end <= len(blob), "ramdisk payload fits in the file")

    dtb_off = align(ramdisk_end)
    dtb_end = dtb_off + dtb_size
    check(dtb_size == 0 or dtb_end <= len(blob), "dtb payload fits in the file")
    if dtb_size:
        # Samsung/MTK wrap the FDT in a 64-byte 0xd7b7ab1e header
        # (magic, fdt size, ...), so the magic is at +0 or at +0x40.
        window = blob[dtb_off:dtb_off + min(dtb_size, 0x1000)]
        hit = next((i for i in range(0, len(window) - 3, 4)
                    if window[i:i + 4] in FDT_MAGICS), None)
        check(hit is not None,
              f"dtb region contains an FDT magic"
              + (f" at +{hit:#x} (vendor header)" if hit else
                 f" (first bytes {window[:8].hex()})"))

    expected_end = dtb_end if dtb_size else ramdisk_end
    check(len(blob) == align(expected_end),
          f"file length matches the loader layout ({len(blob)} vs {align(expected_end)})")

    print(f"  ....  no AVB footer in this file" if blob[-64:-60] != AVB_MAGIC
          else "  ....  AVB footer present (copying it verbatim is the only "
               "safe way to keep a signature)")
    if len(blob) - align(expected_end) >= 64 and blob[-64:-60] == AVB_MAGIC:
        print("        (footer belongs to a stale, larger image - see below)")

    # ---- AArch64 Image ----------------------------------------------------
    if len(image) < 0x40 or image[0x38:0x3C] != IMAGE_MAGIC:
        check(False, f"Image magic at 0x38 is {image[0x38:0x3C]!r}, expected "
                     f"{IMAGE_MAGIC!r} (old branch emitted 'ARM\\x64')")
        return 1
    check(True, "Image magic at 0x38 is ARMd")

    code0 = u32(image, 0)

    # ---- edk2 UEFI BootShim ----------------------------------------------
    # BootShim.S begins with `adr x1, _Payload`, then branches over a fake
    # AArch64 Image header.  It copies exactly image_size bytes from _Payload
    # to text_offset and jumps there.  image_size covers only the firmware FD,
    # not the stub, and code0 is an ADR, so the Linux-Image checks below do
    # not apply to this variant.
    if (code0 & 0x9F000000) == 0x10000000:
        rd = code0 & 0x1F
        immhi = (code0 >> 5) & 0x7FFFF
        immlo = (code0 >> 29) & 0x3
        payload_off = (immhi << 2) | immlo
        check(rd == 1, f"BootShim adr writes x1 (payload pointer), Rd={rd}")
        check(0 < payload_off <= len(image) - 0x100,
              f"BootShim payload begins at image offset 0x{payload_off:x}")
        if not 0 < payload_off <= len(image) - 0x100:
            return 1

        load = u64(image, 8)
        image_size = u64(image, 16)
        check(load == 0x50000000,
              f"BootShim copies to 0x{load:08x} (UEFI_BASE)")
        check(load % 16 == 0, "BootShim copy destination is 16-byte aligned")
        if not check(payload_off + image_size <= len(image),
                     f"BootShim image_size fits the payload "
                     f"(0x{payload_off:x}+0x{image_size:x} <= 0x{len(image):x})"):
            return 1
        check(u32(image, 4) & 0xFC000000 == 0x14000000,
              "BootShim branches over its fake Image header")

        fv = image[payload_off:payload_off + image_size]
        entry_branch = u32(fv, 0)
        check(entry_branch & 0xFC000000 == 0x14000000,
              f"UEFI FD starts with a B (0x{entry_branch:08x})")
        if entry_branch & 0xFC000000 == 0x14000000:
            imm = entry_branch & 0x03FFFFFF
            if imm & 0x02000000:
                imm -= 0x04000000
            fd_entry = imm * 4
            check(0 < fd_entry < len(fv),
                  f"UEFI FD entry vector branches to +0x{fd_entry:x}")
            if 0 < fd_entry < len(fv):
                print(f"  ....  FD entry +0x{fd_entry:x}, first word "
                      f"0x{u32(fv, fd_entry):08x}")

        fv_sig = fv.find(b"_FVH", 0x20, 0x60)
        check(fv_sig == 0x28,
              "UEFI payload is an FV (_FVH at +0x28)" if fv_sig == 0x28
              else f"UEFI payload has no _FVH at +0x28 (found {fv_sig:#x})")
        if fv_sig == 0x28:
            fv_len = u64(fv, 0x20)
            check(fv_len == image_size,
                  f"FV length matches image_size "
                  f"(0x{fv_len:x} vs 0x{image_size:x})")

        # The XNUP payload is appended after the FV.  BootShim copies it to
        # 0x60000000; the loader expands kernel.lz4 to 0x62000000.
        appended = image[payload_off + image_size:]
        if appended:
            check(appended[:4] == b"XNUP",
                  f"appended payload starts with XNUP (got {appended[:4]!r})")
            if appended[:4] == b"XNUP":
                try:
                    body_size = u32(appended, 4)
                    check(len(appended) == align16(8 + body_size),
                          f"XNUP container is padded to 16 bytes "
                          f"(0x{len(appended):x} vs 0x{align16(8 + body_size):x})")
                    entries = xnup_entries(appended)
                    names = [name for name, _ in entries]
                    check("kernel.lz4" in names,
                          f"XNUP carries kernel.lz4 (entries: {names})")
                    if "kernel.lz4" in names:
                        entry = dict(entries)["kernel.lz4"]
                        check(len(entry) >= 8,
                              f"kernel.lz4 has a size prefix ({len(entry)} bytes)")
                        if len(entry) >= 8:
                            usize = u64(entry, 0)
                            stream = entry[8:]
                            check(stream[:4] == b"\x02\x21\x4c\x18",
                                  f"kernel.lz4 is an LZ4 legacy frame "
                                  f"({stream[:4].hex()})")
                            check(usize <= 0x06000000,
                                  f"kernel.lz4 expands to 0x{usize:x} "
                                  f"(fits below 0x68000000)")
                            if stream[:4] == b"\x02\x21\x4c\x18":
                                blocks, end = lz4legacy.parse_blocks(stream)
                                check(end == len(stream),
                                      f"LZ4 frame has no trailing garbage "
                                      f"(end 0x{end:x} of 0x{len(stream):x})")
                                covered = sum(
                                    min(lz4legacy.BLOCK, usize - i * lz4legacy.BLOCK)
                                    for i in range(len(blocks)))
                                check(covered == usize,
                                      f"LZ4 block count covers the declared size "
                                      f"({len(blocks)} blocks, 0x{covered:x} vs 0x{usize:x})")
                            if args.kernelcache:
                                source = args.kernelcache.read_bytes()
                                check(usize == len(source),
                                      f"kernel.lz4 size matches {args.kernelcache.name} "
                                      f"(0x{usize:x} vs 0x{len(source):x})")
                                if stream[:4] == b"\x02\x21\x4c\x18":
                                    try:
                                        expanded = lz4legacy.decompress(stream, usize)
                                    except ValueError as exc:
                                        check(False, f"kernel.lz4 decompresses ({exc})")
                                    else:
                                        check(hashlib.sha256(expanded).digest() ==
                                              hashlib.sha256(source).digest(),
                                              "kernel.lz4 decompresses to the kernelcache")
                except ValueError as exc:
                    check(False, f"XNUP container is valid ({exc})")

            # BootShim builds the destination with movz/movk.  Find
            # `movz Xd, #0x6000, lsl #16` in the shim's code.
            code_words = struct.unpack_from(
                f"<{min(len(image), 0x100) // 4}I", image)
            check(any(is_movz(w, 0x6000, 1) or is_movk(w, 0x6000, 1)
                      for w in code_words),
                  "BootShim builds XNU_PAYLOAD_BASE 0x60000000")
        else:
            check(args.kernelcache is None,
                  "no appended XNU payload to check against the kernelcache")

        shim_words = struct.unpack_from("<24I", image)
        check(find_seq(shim_words, WDT_SEQ_UEFI) != [],
              "BootShim disables the MTK watchdog before the copy")

        print("  ....  variant: edk2 UEFI "
              "(BootShim -> firmware FD at 0x50000000)")
        print()
        if problems:
            for p in problems:
                print(f"problem: {p}")
            print()
            print("VERDICT   BROKEN: the UEFI image will not execute as built")
            return 1
        print("VERDICT   READY: edk2 UEFI BootShim + firmware FV, WDT disabled")
        if dtb_size and dtb_off:
            print(f"          loader reads: kernel@0x{kernel_addr:08x}, "
                  f"ramdisk@0x{ramdisk_addr:08x}, dtb@{dtb_off:#x} in file")
        return 0

    image_size = u64(image, 16)
    check(image_size == len(image),
          f"Image header image_size matches the payload ({image_size} vs {len(image)})")

    if code0 & 0xFC000000 != 0x14000000:
        check(False, f"entry code0=0x{code0:08x} is not a B instruction")
        return 1
    imm = code0 & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 0x04000000
    entry = imm * 4
    check(0 <= entry <= len(image) - 8,
          f"code0 branches to image offset 0x{entry:x}")
    if not 0 <= entry <= len(image) - 8:
        return 1
    print(f"  ....  entry code0=0x{code0:08x} -> offset 0x{entry:x}")

    words = struct.unpack_from(f"<{len(image) // 4}I", image)
    e = entry // 4

    # ---- the fixes, at their actual locations -----------------------------
    if words[e] != MSR_DAIFSET:
        check(False, f"_start begins with msr daifset (got 0x{words[e]:08x})")
        print()
        print("VERDICT   NOT THE SHIM: the payload at code0 is not the boot shim "
              "(stock kernel?)")
        return 1
    check(True, "_start begins with msr daifset")

    wdt_at_entry = tuple(words[e + 1:e + 5]) == WDT_SEQ
    all_wdt = find_seq(words, WDT_SEQ)
    check(wdt_at_entry,
          "WDT-disable sequence is the first MMIO write after entry"
          + ("" if wdt_at_entry else
             f" (found at offsets {[hex(i * 4) for i in all_wdt] or 'nowhere'})"))

    # The canary parks in a wfe/self-loop within the first few instructions
    # after entry. Older canary builds jump straight there; newer ones pet the
    # watchdog first, so the loop is a few words further down.
    window = words[e + 4:e + 24]
    canary_at = next((i for i in range(len(window) - 1)
                      if window[i] == WFE and is_back_branch(window[i + 1])), None)
    canary = canary_at is not None
    if canary:
        pets = 0x52832E2A in window[:canary_at]   # mov w10, #0x1971
        print(f"  ....  variant: canary (WDT off"
              f"{', WDT_RESTART petting' if pets else ''}, wfe at entry+"
              f"0x{(canary_at + 4) * 4:x})")
    else:
        check(words[e + 6] == 0xAA0003F3,   # mov x19, x0
              f"entry falls through to the real shim (got 0x{words[e + 6]:08x})")

    has_scr = SCR_EL3 in words
    if canary:
        # The EL3 code is still linked in, but the wfe loop above it never
        # falls through there. Its presence says nothing about the verdict.
        print(f"  ....  msr scr_el3 {'present but unreachable' if has_scr else 'absent'} "
              f"(canary never executes it)")
    else:
        check(has_scr, "msr scr_el3 is present (EL3 -> EL2 drop)")

    print()
    if problems:
        for p in problems:
            print(f"problem: {p}")
        print()
        print("VERDICT   STALE/BROKEN: do not expect boot progress from this file")
        return 1

    print("VERDICT   READY: this file contains the fixes")
    if dtb_size and dtb_off:
        print(f"          loader reads: kernel@0x{kernel_addr:08x}, "
              f"ramdisk@0x{ramdisk_addr:08x}, dtb@{dtb_off:#x} in file")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
