#!/bin/sh
# copy-ca-into-esp.sh SRC DST
#
# ESP-assembly helper (invoked from the top-level CMakeLists.txt). Copies the
# fetched CA trust bundle SRC into the ESP at DST when it exists; otherwise writes
# an empty placeholder so the build never depends on the network. A dedicated
# script keeps the conditional out of the CMake command string (CMake treats ';'
# as a list separator, which mangles an inline `sh -c "if ...; fi"`).
set -eu
SRC="$1"
DST="$2"
if [ -f "$SRC" ]; then
    cp "$SRC" "$DST"
else
    : > "$DST"
fi
