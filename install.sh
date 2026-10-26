#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
desktop=false
termux=false
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}

case "${PREFIX:-}" in
    /data/data/*/files/usr|/data/user/0/*/files/usr) termux=true ;;
esac
if [ -n "${TERMUX_VERSION:-}" ]; then termux=true; fi

case "${1:-}" in
    '') ;;
    --desktop) desktop=true ;;
    --help|-h)
        printf '%s\n' 'usage: ./install.sh [--desktop]'
        printf '%s\n' 'Installs dependencies and builds Cyber Deck. In Termux, builds the experimental core TUI.'
        printf '%s\n' 'On a configured Hyprland desktop, also installs its shortcut.'
        printf '%s\n' 'Use --desktop to require desktop setup when no Hyprland config is detected.'
        exit 0
        ;;
    *)
        printf 'Cyber Deck: unknown option: %s\n' "$1" >&2
        exit 2
        ;;
esac
if [ "$#" -gt 1 ]; then
    printf '%s\n' 'Cyber Deck: too many arguments.' >&2
    exit 2
fi

if [ -f "$config_dir/hypr/hyprland.local.lua" ]; then
    desktop=true
fi

if [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Cyber Deck: run this as your normal user; the script uses sudo for packages.' >&2
    exit 1
fi

if [ "$termux" = true ]; then
    if [ "$desktop" = true ]; then
        printf '%s\n' 'Cyber Deck: the desktop shortcut setup is not available in Termux.' >&2
        exit 1
    fi
    if ! command -v pkg >/dev/null 2>&1; then
        printf '%s\n' 'Cyber Deck: Termux was detected, but its pkg command is missing.' >&2
        exit 1
    fi
    printf '%s\n' 'Installing Cyber Deck and module dependencies in Termux...'
    pkg install -y clang cmake pkg-config ncurses libvterm nano elinks mpv
    for program in clang cmake pkg-config nano elinks mpv; do
        if ! command -v "$program" >/dev/null 2>&1; then
            printf 'Cyber Deck: required Termux program is missing after installation: %s\n' "$program" >&2
            exit 1
        fi
    done
    cmake -S "$repo_dir" -B "$repo_dir/build" \
        -DCYBERDECK_ENABLE_LINUX_INTEGRATIONS=OFF
    cmake --build "$repo_dir/build"
    printf 'Cyber Deck is ready: %s\n' "$repo_dir/build/cyber-deck"
    printf '%s\n' 'Run it from Termux with: ./build/cyber-deck'
    printf '%s\n' 'Wave plays through mpv inside Termux; Android lock-screen media controls are unavailable.'
    exit 0
fi

if ! command -v sudo >/dev/null 2>&1; then
    printf '%s\n' 'Cyber Deck: sudo is required to install packages.' >&2
    exit 1
fi

if command -v pacman >/dev/null 2>&1; then
    set -- gcc cmake pkgconf ncurses libvterm libnm libpulse glib2 \
        networkmanager bluez nano elinks mpv mpv-mpris
    if [ "$desktop" = true ]; then set -- "$@" ghostty; fi
    printf '%s\n' 'Installing Cyber Deck packages with pacman...'
    sudo pacman -S --needed "$@"
elif command -v apt-get >/dev/null 2>&1; then
    set -- g++ cmake pkg-config libncurses-dev libvterm-dev \
        libnm-dev libpulse-dev libglib2.0-dev \
        network-manager bluez nano elinks mpv mpv-mpris
    printf '%s\n' 'Updating package lists...'
    sudo apt-get update
    printf '%s\n' 'Installing Cyber Deck packages with apt...'
    sudo apt-get install "$@"
    if [ "$desktop" = true ] && ! command -v ghostty >/dev/null 2>&1; then
        printf '%s\n' 'Cyber Deck: --desktop requires Ghostty; install it for this system and rerun.' >&2
        exit 1
    fi
else
    printf '%s\n' 'Cyber Deck: supported installers are pacman and apt-get.' >&2
    exit 1
fi

for program in nano elinks mpv pkg-config; do
    if ! command -v "$program" >/dev/null 2>&1; then
        printf 'Cyber Deck: required program is missing after installation: %s\n' "$program" >&2
        exit 1
    fi
done
if [ ! -f /etc/mpv/scripts/mpris.so ] && \
   [ ! -f /usr/lib/mpv-mpris/mpris.so ] && \
   [ ! -f "$HOME/.config/mpv/scripts/mpris.so" ]; then
    printf '%s\n' 'Cyber Deck: mpv-mpris is missing after installation; Wave cannot report Linux media status.' >&2
    exit 1
fi
for library in libnm libpulse gio-2.0; do
    if ! pkg-config --exists "$library"; then
        printf 'Cyber Deck: required feature library is missing after installation: %s\n' "$library" >&2
        exit 1
    fi
done

cmake -S "$repo_dir" -B "$repo_dir/build" \
    -DCYBERDECK_ENABLE_LINUX_INTEGRATIONS=ON
cmake --build "$repo_dir/build"

if [ "$desktop" = true ]; then
    "$repo_dir/desktop/install-hyprland-shortcut.sh"
fi

printf 'Cyber Deck is ready: %s\n' "$repo_dir/build/cyber-deck"
