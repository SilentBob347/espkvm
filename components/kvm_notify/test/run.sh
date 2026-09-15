#!/bin/sh
# Build and run the Telegram reply reader's tests on this machine - no device needed.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." \
   "$here/test_tg_chats.c" "$here/../tg_chats.c" -o "$out/test_tg_chats"
"$out/test_tg_chats"
rm -rf "$out"
