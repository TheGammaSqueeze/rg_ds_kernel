#!/bin/bash
# Build gammaos_odd (the GPU helper for CONFIG_GAMMAOS_OD) with the Android NDK.
#   NDK=<ndk root> rgds/od_helper/build.sh   -> rgds/od_helper/gammaos_odd
# od_comp_spv.h is regenerated from od.comp when glslc is available.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${NDK:-/work/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
GLSLC="$NDK/shader-tools/linux-x86_64/glslc"
cd "$HERE"
if [ -x "$GLSLC" ]; then
  "$GLSLC" -O --target-env=vulkan1.1 -fshader-stage=compute od.comp -o od.spv
  python3 - <<'PY'
import struct
d = open('od.spv', 'rb').read()
w = struct.unpack('<%dI' % (len(d) // 4), d)
rows = (', '.join('0x%08x' % x for x in w[i:i + 8]) for i in range(0, len(w), 8))
open('od_comp_spv.h', 'w').write('/* generated from od.comp by glslc */\nstatic const uint32_t od_comp_spv[] = {\n' + ',\n'.join(rows) + '\n};\n')
PY
  rm -f od.spv
fi
INC="$(mktemp -d)"
trap 'rm -rf "$INC"' EXIT
cp "$HERE/../../include/uapi/linux/gammaos_od.h" "$INC/"
"$CC" -O2 -Wall -Wextra -I"$INC" -I. -o gammaos_odd gammaos_odd.c -lvulkan -llog
echo "built $HERE/gammaos_odd"
