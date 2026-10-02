#!/bin/sh
# One-time passwords against the RFC's own vectors, on this machine.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." "$here/test_totp.c" "$here/../totp.c" -o "$out/test_totp"
"$out/test_totp"
rm -rf "$out"
