#!/usr/bin/env bash
# Regenerate kernel/net/tls_roots.c — NovaOS's built-in root certificate
# store — from a PEM bundle of trusted CAs (default: Mozilla's list as
# shipped by the Debian/Ubuntu ca-certificates package).
#
#   scripts/gen-tls-roots.sh [bundle.pem]
#
# Builds BearSSL's host "brssl" tool from third_party/bearssl and runs
# "brssl ta", which turns each certificate into a br_x509_trust_anchor.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BSSL="$ROOT/third_party/bearssl"
OUT="$ROOT/kernel/net/tls_roots.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ $# -ge 1 ]; then
    BUNDLE="$1"; SOURCE="$(basename "$1")"
else
    BUNDLE="$TMP/mozilla.pem"
    cat /usr/share/ca-certificates/mozilla/*.crt > "$BUNDLE"
    SOURCE="Mozilla CA list, ca-certificates $(dpkg-query -W -f '${Version}' ca-certificates 2>/dev/null || echo '?')"
fi

cc -O1 -w -I"$BSSL/inc" -I"$BSSL/src" -o "$TMP/brssl" \
   "$BSSL"/tools/*.c $(find "$BSSL/src" -name '*.c') 
"$TMP/brssl" ta "$BUNDLE" > "$TMP/ta.c"
N=$(sed -n 's/^#define TAs_NUM *\([0-9]*\).*/\1/p' "$TMP/ta.c")

{
    echo "/*"
    echo " * tls_roots.c — built-in trusted root certificates (GENERATED, do not edit)"
    echo " *"
    echo " * Source: $SOURCE ($N roots)."
    echo " * Regenerate with scripts/gen-tls-roots.sh."
    echo " */"
    echo
    echo '#include "tls.h"'
    sed -e '/^#define TAs_NUM/d' \
        -e 's/^static const br_x509_trust_anchor TAs\[[0-9]*\]/const br_x509_trust_anchor g_tls_builtin_roots[]/' \
        "$TMP/ta.c"
    echo
    echo "const int g_tls_builtin_root_count = $N;"
} > "$OUT"
echo "Wrote $OUT ($N roots)"
