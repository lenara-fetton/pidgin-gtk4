#!/bin/sh
# run.sh: build and run the M9 Discord native-metadata tests (test_discord.c).
#
# test_discord.c #includes the plugin's libdiscord.c, so it needs the
# purple-discord checkout with the pidgin4-message-meta branch
# (DISCORD_SRC, default ~/purple-discord-pidgin4). It builds against the
# SYSTEM libpurple 2.14 (PKG_CONFIG_PATH is unset), like the plugin itself,
# starts a null UI whose ui_info has message-meta = 1 (and one without it),
# registers the M8 conversation signals as pidgin4's libpurple does, loads
# the plugin as a prpl and feeds it gateway payloads in the shapes of the
# Discord API documentation: MESSAGE_CREATE (plain, reply, reply to a
# deleted message, image attachment, custom emoji), MESSAGE_UPDATE (edit,
# embed-only), MESSAGE_REACTION_ADD/REMOVE (custom, Unicode, own, DM) and
# MESSAGE_DELETE (known and unknown author), own sends echoed by the gateway
# or the REST reply, a failed send, and the IPC commands. No network: the
# fake connection isn't in purple_connections_get_all(), so REST requests
# fail at once. The same payloads without message-meta give the old output.
#
# Usage: scripts/tests/discord-m9/run.sh [--asan]
set -e
D=${DISCORD_SRC:-$HOME/purple-discord-pidgin4}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/discord-m9.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
unset PKG_CONFIG_PATH
SAN=
[ "${1:-}" = --asan ] && SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

cc -O0 -g -std=c99 -Wall -Wno-unused-function $SAN \
	-DDISCORD_PLUGIN_VERSION='"test"' -DMARKDOWN_PIDGIN -DENABLE_NLS \
	-DUSE_QRCODE_AUTH -DLOCALEDIR='"/usr/share/locale"' \
	$(pkg-config --cflags nss) -I"$D" -I"$D/purple2compat" \
	-o "$OUT/test_discord" "$HERE/test_discord.c" \
	"$D/markdown.c" "$D/purple2compat/http.c" "$D/purple2compat/purple-socket.c" \
	$(pkg-config --libs nss) -lqrencode -lm \
	$(pkg-config --cflags --libs purple glib-2.0 json-glib-1.0 zlib)

mkdir "$OUT/run"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_discord" "$OUT/run"

# The stock-output gate (stock_dump.c): the same payloads on a UI without
# message-meta must give exactly what the base commit's plugin gives.
BASE=${STOCK_BASE:-c5f7c41}
mkdir "$OUT/base" "$OUT/run-new" "$OUT/run-base"
git -C "$D" archive "$BASE" | tar -x -C "$OUT/base"
for which in new base; do
	if [ $which = new ]; then S=$D; else S=$OUT/base; fi
	cc -O0 -g -std=c99 -w $SAN \
		-DDISCORD_PLUGIN_VERSION='"test"' -DMARKDOWN_PIDGIN -DENABLE_NLS \
		-DUSE_QRCODE_AUTH -DLOCALEDIR='"/usr/share/locale"' \
		$(pkg-config --cflags nss) -I"$HERE" -I"$S" -I"$S/purple2compat" \
		-o "$OUT/stock_dump_$which" "$HERE/stock_dump.c" \
		"$S/markdown.c" "$S/purple2compat/http.c" "$S/purple2compat/purple-socket.c" \
		$(pkg-config --libs nss) -lqrencode -lm \
		$(pkg-config --cflags --libs purple glib-2.0 json-glib-1.0 zlib)
	# The old embed block ends in the current time: mask it
	ASAN_OPTIONS=detect_leaks=0 "$OUT/stock_dump_$which" "$OUT/run-$which" 2>/dev/null |
		sed -E 's/[A-Z][a-z]{2} [A-Z][a-z]{2} +[0-9]+ [0-9:]{8} [0-9]{4}/<now>/g' > "$OUT/stock-$which.txt"
done
if cmp -s "$OUT/stock-base.txt" "$OUT/stock-new.txt"; then
	echo "stock output: identical to $BASE ($(grep -c '^step' "$OUT/stock-new.txt") payloads)"
else
	echo "stock output: DIFFERS from $BASE"
	diff -u "$OUT/stock-base.txt" "$OUT/stock-new.txt"
	exit 1
fi
