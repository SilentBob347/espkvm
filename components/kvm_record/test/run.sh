#!/bin/sh
# Build and run the recorder's host tests: the transport stream muxer, and the
# keystroke subtitles.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_ts_mux.c" "$here/../ts_mux.c" -o "$out/test_ts_mux"
"$out/test_ts_mux"
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_keylog.c" "$here/../keylog.c" "$here/../keymap_layouts.c" -o "$out/test_keylog"
"$out/test_keylog"
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_frame_ring.c" "$here/../frame_ring.c" -o "$out/test_frame_ring"
"$out/test_frame_ring"
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_ts_to_mp4.c" "$here/../ts_to_mp4.c" "$here/../ts_mux.c" -o "$out/test_ts_to_mp4"
"$out/test_ts_to_mp4"
rm -rf "$out"
