#!/bin/sh
set -eu

desktop_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app="$desktop_dir/../build/cyber-deck"

if ! command -v ghostty >/dev/null 2>&1; then
    printf '%s\n' 'Cyber Deck: Ghostty is not installed.' >&2
    exit 1
fi
if [ ! -x "$app" ]; then
    printf '%s\n' 'Cyber Deck: build the app first with cmake -S repository -B repository/build && cmake --build repository/build.' >&2
    exit 1
fi

exec ghostty --gtk-single-instance=false --config-file="$desktop_dir/ghostty.conf" -e "$app"
