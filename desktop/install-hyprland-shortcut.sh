#!/bin/sh
set -eu

desktop_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
local_config="$config_dir/hypr/hyprland.local.lua"
include='dofile("'"$desktop_dir"'/hyprland.local.lua")'

if [ ! -f "$local_config" ]; then
    printf 'Cyber Deck: expected Hyprland config at %s\n' "$local_config" >&2
    exit 1
fi

if ! grep -Fqx "$include" "$local_config"; then
    printf '\n-- Cyber Deck floating Ghostty shortcut.\n%s\n' "$include" >> "$local_config"
fi

printf 'Cyber Deck shortcut saved in %s\n' "$local_config"
if command -v hyprctl >/dev/null 2>&1; then
    if hyprctl eval "dofile(\"$desktop_dir/hyprland.local.lua\")" >/dev/null 2>&1; then
        printf '%s\n' 'Ctrl+Super+Alt+Y is active in this Hyprland session.'
        exit 0
    fi
fi
printf '%s\n' 'Reload Hyprland or sign in again to activate Ctrl+Super+Alt+Y.'
