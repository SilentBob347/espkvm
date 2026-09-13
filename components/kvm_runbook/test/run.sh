#!/bin/sh
# Build and run the runbook parser's tests on this machine - no device needed.
# The parser is plain C over a string for exactly this reason.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_runbook_script.c" "$here/../runbook_script.c" -o "$out/test_runbook_script"
"$out/test_runbook_script"
rm -rf "$out"
