#!/bin/sh
# Rebuild novacalc32.tlb and novacalc64.tlb from novacalc.idl with widl
# (mingw-w64-tools); stdole2.tlb is only needed for widl's importlib.
set -e
cd "$(dirname "$0")"
WIDL=${WIDL:-$(command -v x86_64-w64-mingw32-widl || command -v widl)}
tmp=$(mktemp -d)
"$WIDL" -t -m64 -o "$tmp/stdole2.tlb" stdole2.idl
"$WIDL" -t -m32 -L "$tmp" -o ../novacalc32.tlb novacalc.idl
"$WIDL" -t -m64 -L "$tmp" -o ../novacalc64.tlb novacalc.idl
rm -r "$tmp"
