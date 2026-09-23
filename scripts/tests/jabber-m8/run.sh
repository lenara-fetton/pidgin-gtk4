#!/bin/sh
# run.sh: build and run the M8 jabber protocol-core tests (test_m8.c).
#
# They link libpurple and libjabber from the private prefix (build it first
# with scripts/build-libpurple.sh), start a null UI with a GLib event loop and
# a scratch user dir, fake a connected JabberStream (no network, no accounts
# signed in) and feed synthetic stanzas through the parsers: XEP-0359 ids,
# carbons, MAM paging, 0421 occupant-id, 0428 fallback stripping, the send
# path, MUC join/catch-up and bookmarks. Outgoing stanzas are captured from
# jabber-sending-xmlnode.
#
# Message semantics: XEP-0184 receipts, 0333 markers, 0308 corrections,
# 0444 reactions, 0461 replies, 0424/0425 retraction and moderation, 0393
# styling of GTK 2 HTML, 0490 displayed sync, the send-* / mds-publish IPC
# commands, the disco features and the jabber connection flags. Each is
# checked with and without a "message-meta" UI and with handlers that do
# and don't render the event.
#
# Server features round 2: XEP-0191 blocklist/pushes/privacy modes, 0186
# invisibility, 0377 reports, MAM preferences and their kv gate, 0447/0446
# file sharing (parse incl. thumbnails and hashes, SIMS, the element sent
# after an HTTP upload to a local SoupServer), 0380 EME, 0319 idle.
#
# Usage: PIDGIN4_PREFIX=~/.local/pidgin4 scripts/tests/jabber-m8/run.sh
set -e
TREE=$(cd "$(dirname "$0")/../../.." && pwd)
PREFIX=${PIDGIN4_PREFIX:-$HOME/.local/pidgin4}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/jabber-m8.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

gcc -g -O0 -Wall -Wno-deprecated-declarations -DHAVE_CONFIG_H \
	-I"$TREE" -I"$TREE/libpurple" -I"$TREE/libpurple/protocols/jabber" \
	$(pkg-config --cflags glib-2.0 gmodule-2.0 libxml-2.0 libsoup-3.0) \
	-o "$OUT/test_m8" "$TREE/scripts/tests/jabber-m8/test_m8.c" \
	-L"$PREFIX/lib" -lpurple -L"$PREFIX/lib/purple-2" -ljabber \
	-Wl,-rpath,"$PREFIX/lib" -Wl,-rpath,"$PREFIX/lib/purple-2" \
	$(pkg-config --libs glib-2.0 gmodule-2.0 libxml-2.0 libsoup-3.0)

mkdir -p "$OUT/home"
G_DEBUG=fatal-criticals "$OUT/test_m8" "$OUT/home"
