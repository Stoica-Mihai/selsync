#!/usr/bin/env bash
# Build selsync. Wayland backend always; X11 backend if xcb/xcb-xfixes present.
set -euo pipefail
cd "$(dirname "$0")"

command -v wayland-scanner >/dev/null || { echo "need wayland-scanner" >&2; exit 1; }
pkg-config --exists wayland-client || { echo "need wayland-client dev" >&2; exit 1; }

mkdir -p gen
gen() { # gen <xml> <basename>
	wayland-scanner private-code  "$1" "gen/$2.c"
	wayland-scanner client-header "$1" "gen/$2-client.h"
}
gen protocols/ext-data-control-v1.xml          ext-data-control-v1
gen protocols/wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1

CFLAGS="-O2 -Wall -Wextra -Igen $(pkg-config --cflags wayland-client)"
LIBS="$(pkg-config --libs wayland-client)"
SRCS=(selsync.c x11.c gen/ext-data-control-v1.c gen/wlr-data-control-unstable-v1.c)
DEFS=()

if pkg-config --exists xcb xcb-xfixes; then
	DEFS+=(-DSELSYNC_X11)
	CFLAGS="$CFLAGS $(pkg-config --cflags xcb xcb-xfixes)"
	LIBS="$LIBS $(pkg-config --libs xcb xcb-xfixes)"
	echo "X11 backend: enabled"
else
	echo "X11 backend: disabled (xcb/xcb-xfixes dev not found) — Wayland only"
fi

# shellcheck disable=SC2086
cc $CFLAGS "${DEFS[@]}" "${SRCS[@]}" -o selsync $LIBS
echo "built $(pwd)/selsync"
