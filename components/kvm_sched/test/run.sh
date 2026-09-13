#!/bin/sh
# Build and run the cron parser's tests on this machine - no device needed.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_sched_cron.c" "$here/../sched_cron.c" -o "$out/test_sched_cron"
"$out/test_sched_cron"
rm -rf "$out"
