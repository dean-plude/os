#!/usr/bin/env bash
# Stage what tools/selftest.py --suite graphics puts on drive C::
#   DIR/7zip       7-Zip (C:\Programs\7-Zip), which the App Store unpacks with
#   DIR/downloads  the Mesa 3D and DXVK archives the App Store lists, and
#                  Venus's venus.7z (tools/build_venus.py; VENUS_7Z names one
#                  already built) (C:\Downloads: the Store installs them
#                  without a network)
#   DIR/tests      tools/gltest, tools/d3dtest, tools/hlsltest, tools/d2dtest, tools/dwtest and tools/dw3test,
#                  64- and 32-bit,
#                  and d2dtest's reference image (C:\Tests)
# Needs curl, 7z (p7zip-full) and MinGW-w64 (x86-64 and i686), and what
# tools/build_venus.py needs unless VENUS_7Z is set.
#     tools/ci/stage-graphics.sh DIR [CACHE]
# CACHE (default DIR/cache) keeps the downloads between runs.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$1"
CACHE="${2:-$OUT/cache}"
mkdir -p "$OUT/7zip" "$OUT/downloads" "$OUT/tests" "$CACHE"

# the URLs the App Store's catalog has (kernel/apps/store.c); 7-Zip from
# its GitHub releases
URLS=(
  "https://github.com/ip7z/7zip/releases/download/26.03/7z2603-x64.exe"
  "$(grep -o 'pal1000/mesa-dist-win/releases/download/[^"]*\.7z' "$ROOT/kernel/apps/store.c" | sed 's|^|https://github.com/|')"
  "$(grep -o 'doitsujin/dxvk/releases/download/[^"]*\.tar\.gz' "$ROOT/kernel/apps/store.c" | sed 's|^|https://github.com/|')"
)
for u in "${URLS[@]}"; do
  f="$CACHE/$(basename "$u")"
  [ -s "$f" ] || curl -sSLf --retry 4 -o "$f" "$u"
done
7z x -y -o"$OUT/7zip" "$CACHE/$(basename "${URLS[0]}")" >/dev/null
cp "$CACHE/$(basename "${URLS[1]}")" "$CACHE/$(basename "${URLS[2]}")" "$OUT/downloads/"
if [ -z "${VENUS_7Z:-}" ]; then
  python3 "$ROOT/tools/build_venus.py" "$CACHE/venus"
  VENUS_7Z="$CACHE/venus/venus.7z"
fi
cp "$VENUS_7Z" "$OUT/downloads/venus.7z"

for arch in x86_64 i686; do
  sfx=$([ $arch = i686 ] && echo 32 || true)
  $arch-w64-mingw32-gcc -O2 -o "$OUT/tests/gltest$sfx.exe" "$ROOT/tools/gltest/gltest.c" \
    -lopengl32 -lgdi32 -luser32 -lm
  $arch-w64-mingw32-gcc -O2 -o "$OUT/tests/d3dtest$sfx.exe" "$ROOT/tools/d3dtest/d3dtest.c" \
    -ld3d9 -ld3d11 -ldxgi -luser32 -lgdi32 -lole32
  $arch-w64-mingw32-gcc -O2 -o "$OUT/tests/hlsltest$sfx.exe" "$ROOT/tools/hlsltest/hlsltest.c" \
    -ld3dcompiler_47 -ld3d11 -ldxguid
  $arch-w64-mingw32-gcc -O2 -o "$OUT/tests/d2dtest$sfx.exe" "$ROOT/tools/d2dtest/d2dtest.c" \
    -ld2d1 -luser32 -lgdi32 -luuid
  $arch-w64-mingw32-gcc -O2 -o "$OUT/tests/dwtest$sfx.exe" "$ROOT/tools/dwtest/dwtest.c" \
    -ldwrite -lgdi32 -luser32
  $arch-w64-mingw32-gcc -O2 -o "$OUT/tests/dw3test$sfx.exe" "$ROOT/tools/dw3test/dw3test.c" -ldwrite
done
python3 "$ROOT/tools/d2dtest/png2bmp.py" "$ROOT/tools/d2dtest/d2dref.png" "$OUT/tests/d2dref.bmp"
ls -l "$OUT/downloads" "$OUT/tests"
