#!/bin/bash
#
# Build the XNU loader and regenerate the C array embedded in the firmware.
#
# The loader source lives in the hackios-xnu tree (A03S_XNULOADER).  This
# script runs its freestanding clang build, then rewrites
# MT6765Pkg/Library/XnuLoaderImageLib/loader_efi.c so the next edk2 build
# carries the new loader.
#
set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XNULOADER="${A03S_XNULOADER:-$HOME/hackios-xnu/xnuloader}"
SRC="${A03S_SRC:-$ROOT/edk2-mt6765}"
OUT="$SRC/MT6765Pkg/Library/XnuLoaderImageLib/loader_efi.c"

die() { echo "error: $*" >&2; exit 1; }

[ -f "$XNULOADER/loader.c" ] || die "loader source not found: $XNULOADER/loader.c"
[ -d "$(dirname "$OUT")" ] || die "edk2 tree not found: $SRC"

# lld-link is not on PATH on a stock macOS/MacPorts install.
if [ -z "${LLD_LINK:-}" ]; then
  LLD_LINK="$(command -v lld-link 2>/dev/null || true)"
fi
if [ -z "${LLD_LINK:-}" ]; then
  for candidate in \
      /opt/local/libexec/llvm-20/bin/lld-link \
      /opt/local/libexec/llvm-18/bin/lld-link \
      /opt/local/bin/lld-link; do
    if [ -x "$candidate" ]; then
      LLD_LINK="$candidate"
      break
    fi
  done
fi
[ -n "${LLD_LINK:-}" ] || die "lld-link not found (set LLD_LINK)"
export LLD_LINK

echo "==> building xnuloader ($XNULOADER)"
"$XNULOADER/build.sh"

echo "==> generating $OUT"
python3 - "$XNULOADER/loader.efi" "$OUT" <<'PY'
import sys

efi = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as out:
    out.write("/** @file\n")
    out.write("  Embedded HackiOS XNU loader (a03s build). "
              "Generated from xnuloader/loader.efi.\n\n")
    out.write("  Do not edit by hand; rebuild with build_xnuloader.sh.\n")
    out.write("**/\n\n")
    out.write("#include <Uefi.h>\n\n")
    out.write("CONST UINT8 gXnuLoaderEfi[] = {\n")
    for i in range(0, len(efi), 16):
        chunk = efi[i:i + 16]
        out.write("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
    out.write("};\n\n")
    out.write(f"CONST UINTN gXnuLoaderEfiSize = {len(efi)};\n")
PY
