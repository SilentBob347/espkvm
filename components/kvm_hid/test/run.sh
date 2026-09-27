#!/bin/sh
# Build and run the optical drive's MMC replies on this machine - no device needed.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_mmc.c" "$here/../mmc.c" -o "$out/test_mmc"
"$out/test_mmc"
rm -rf "$out"
