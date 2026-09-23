#!/bin/sh
# run.sh: build and run the M9 Steam native-metadata tests (test_steam.c).
#
# test_steam.c #includes the plugin's libsteam.c and links the rest of the
# plugin's sources, so it needs the pidgin-opensteamworks checkout with the
# pidgin4-rich-presence branch (STEAM_SRC, default
# ~/pidgin-opensteamworks-pidgin4/steam-mobile). It builds against the SYSTEM
# libpurple 2.14 (PKG_CONFIG_PATH is unset), like the plugin itself, starts a
# null UI whose ui_info has message-meta = 1 (and one without it), registers
# the M8 conversation signals as pidgin4's libpurple does, loads the plugin
# as a prpl and drives a test-mode CM session (steam_cm__test_new: no
# socket; packets are fed in and everything sent is collected). The samples
# are protobufs built with the plugin's own codec in the shapes of
# SteamDatabase/Protobufs' steammessages_friendmessages.steamclient.proto,
# plus an appdetails JSON reply for the game icon. No network, no accounts.
#
# Usage: scripts/tests/steam-m9/run.sh [--asan]
set -e
S=${STEAM_SRC:-$HOME/pidgin-opensteamworks-pidgin4/steam-mobile}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/steam-m9.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
unset PKG_CONFIG_PATH
SAN=
[ "${1:-}" = --asan ] && SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

cc -O0 -g -std=gnu99 -Wall -Wno-unused-function $SAN -I"$S" \
	$(pkg-config --cflags glib-2.0 json-glib-1.0 purple libsecret-1 nss zlib) \
	-o "$OUT/test_steam" "$HERE/test_steam.c" \
	"$S/steam_proto.c" "$S/steam_msgs.c" "$S/steam_ws.c" "$S/steam_connection.c" \
	"$S/steam_auth.c" "$S/steam_cm.c" \
	$(pkg-config --libs glib-2.0 json-glib-1.0 purple nss zlib)

mkdir "$OUT/run"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_steam" "$OUT/run"
