#!/usr/bin/env bash
# drive.sh: the scripted two-instance scenario against the local Prosody.
#
# Starts everything with run.sh (alice, bob and alice2, alice's second
# resource), then drives the UIs with xdotool on their Xvfb displays and
# checks the stanzas in the -d logs (screenshots of each step are left in
# the work directory for a look):
#
#   IM:  alice sends (with *styling*), Up on the empty entry edits it,
#        bob reacts, replies, alice retracts; receipts, markers, carbons
#        to alice2; bob restarts and gets alice's new message from MAM.
#   MUC: both join test@conference.localhost; alice sends and edits with
#        Up, bob reacts and replies, alice retracts.
#   Server features (round 2), through the ctl plugin (ctl-plugin.c):
#        MAM prefs "roster" -> "always" once (not again after a restart),
#        XEP-0191 block/unblock with pushes and a bounced message, the
#        privacy-modes/report-spam IPC and a XEP-0377 report, XEP-0319
#        idle both ways, XEP-0447 <file-sharing> on uploads (a text file
#        from the UI, a PNG with dimensions from the ctl plugin), and the
#        invisible status on a server without XEP-0186.
#        Up, bob reacts and replies, alice retracts; alice inserts an image
#        (Conversation → Insert Image) with some text and sends: the text
#        goes as a message and the image by HTTP upload, bob gets its URL.
#
# The coordinates assume the default window sizes and a fresh server (no
# history), which run.sh start provides. Exit status 0 when all checks
# pass. Environment as for run.sh; XMPP_LIVE_NO_STOP=1 leaves everything
# running afterwards.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/lib.sh"

pass=0
fail=0
check() {
	if [ "$1" = 0 ]; then
		pass=$((pass + 1)); echo "ok   $2"
	else
		fail=$((fail + 1)); echo "FAIL $2"
	fi
}
# expect USER MARK REGEX DESCRIPTION
expect() { wait_log "$1" "$2" "$3" 10; check $? "$4"; }

# popup USER: the geometry (PX PY PW PH) of the popup that is showing.
# There is no window manager, so every other window sits at 0,0.
popup() {
	local id X Y WIDTH HEIGHT SCREEN WINDOW
	PX= PY= PW= PH=
	for id in $(DISPLAY=$(disp "$1") xdotool search --onlyvisible --name '' 2>/dev/null); do
		eval "$(DISPLAY=$(disp "$1") xdotool getwindowgeometry --shell "$id")"
		[ "$X" != 0 ] || [ "$Y" != 0 ] || continue
		[ "$HEIGHT" -gt 100 ] || continue
		PX=$X PY=$Y PW=$WIDTH PH=$HEIGHT
	done
	[ -n "$PY" ] || { echo "no popup for $1" >&2; key "$1" Escape; return 1; }
}

# row_menu USER WIN X Y ITEM: right-click the row at (X, Y) of WIN and
# pick ITEM (reply, react, edit, delete). The items are addressed from
# the menu's bottom, since a label's Cut/Copy/... section may or may not
# come first.
row_menu() {
	local u=$1 off
	case "$5" in
		reply) off=219 ;; react) off=189 ;; edit) off=159 ;; delete) off=129 ;;
	esac
	click "$u" "$2" "$3" "$4" 3
	popup "$u" || return 1
	DISPLAY=$(disp "$u") xdotool mousemove $((PX + 60)) $((PY + PH - off)) click 1
	sleep 0.7
}

# to_entry USER WIN: put the keyboard focus into the compose entry
to_entry() { click "$1" "$2" 320 450; }

# react_with USER TEXT: search the open emoji chooser, click the first match
react_with() {
	popup "$1" || return 1          # not into the compose entry
	type_text "$1" "$2"
	sleep 0.5
	popup "$1" || return 1
	DISPLAY=$(disp "$1") xdotool mousemove $((PX + 37)) $((PY + 112)) click 1
	sleep 0.5
}

"$here/run.sh" start alice bob alice2 | grep -v '^$' || exit 1
for u in alice bob alice2; do
	wait_log "$u" 0 "UI handles message metadata" 30
	check $? "$u signed in"
done
# XEP-0313 prefs: the server's default is "roster"; set to "always" once
for u in alice bob; do
	expect $u 0 "MAM prefs: default is 'roster', setting 'always'" "$u found the archive default 'roster'"
	expect $u 0 "Sending.*<prefs xmlns='urn:xmpp:mam:2' default='always'" "$u set the archive default to 'always'"
	expect $u 0 "MAM prefs: default is now 'always'" "the server accepted $u's archive prefs"
done

# ---- IM ---------------------------------------------------------------
bl=$(win alice "Buddy List"); focus alice "$bl"; key alice ctrl+m
d=$(win alice "New Instant Message"); focus alice "$d"
type_text alice bob@localhost; key alice Return
ca=$(win alice "bob@localhost"); focus alice "$ca"

m=$(mark alice); mb=$(mark bob); m2=$(mark alice2)
type_text alice "hello *bob*"; key alice Return
expect alice "$m" "Sending \(ssl\).*<body>hello \*bob\*</body>.*<origin-id" "IM sent with 0393 styling and an origin-id"
expect bob "$mb" "Recv.*<body>hello \*bob\*</body>" "bob received it"
expect alice2 "$m2" "<sent xmlns='urn:xmpp:carbons:2'>.*hello \*bob\*" "alice2 got the sent carbon"
expect alice "$m" "Recv.*<received [^>]*urn:xmpp:receipts" "delivery receipt from bob"
cb=$(win bob "alice@localhost"); focus bob "$cb"
expect alice "$m" "Recv.*<displayed [^>]*urn:xmpp:chat-markers:0" "displayed marker from bob"
shot alice im-1-sent "$ca"; shot bob im-1-received "$cb"

oid=$(since alice "$m" | grep -o "<origin-id xmlns='urn:xmpp:sid:0' id='[^']*'" | head -n1 | sed "s/.*id='//; s/'$//")
focus alice "$ca"
m=$(mark alice); mb=$(mark bob)
key alice Up
shot alice im-2-editing "$ca"
key alice ctrl+a BackSpace
type_text alice "hello bob, edited"; key alice Return
expect alice "$m" "<replace xmlns='urn:xmpp:message-correct:0' id='$oid'" "Up on the empty entry sent a correction of the last message"
expect bob "$mb" "Recv.*hello bob, edited.*<replace" "bob received the correction"
sleep 1; shot bob im-3-edited "$cb"

focus bob "$cb"
m=$(mark alice); mb=$(mark bob)
row_menu bob "$cb" 420 76 react
shot bob im-4-chooser
react_with bob "thumbs up"
expect bob "$mb" "Sending.*<reactions xmlns='urn:xmpp:reactions:0' id='$oid'>" "bob reacted from the row menu"
expect alice "$m" "Recv.*<reactions [^>]*urn:xmpp:reactions:0" "alice received the reaction"
sleep 1; shot alice im-5-reaction "$ca"

focus bob "$cb"
m=$(mark alice); mb=$(mark bob)
row_menu bob "$cb" 420 76 reply && { type_text bob "a reply from bob"; key bob Return; }
# (the body starts with the quote, so the stanza spans several log lines)
expect bob "$mb" "^a reply from bob</body>.*<reply [^>]*id='$oid'" "bob replied from the row menu"
expect alice "$m" "^a reply from bob</body>.*<reply " "alice received the reply"
sleep 1; shot alice im-6-reply "$ca"

focus alice "$ca"
m=$(mark alice); mb=$(mark bob)
row_menu alice "$ca" 420 76 delete
expect alice "$m" "Sending.*<retract xmlns='urn:xmpp:message-retract:1' id='$oid'/>" "alice retracted her message"
expect bob "$mb" "Recv.*<retract [^>]*urn:xmpp:message-retract:1" "bob received the retraction"
sleep 1; shot bob im-7-retracted "$cb"; shot alice im-7-retracted "$ca"

# MAM catch-up: bob is offline while alice writes
"$here/run.sh" stop-instance bob
focus alice "$ca"; to_entry alice "$ca"
type_text alice "while you were away"; key alice Return
sleep 1
"$here/run.sh" start-instance bob >/dev/null
wait_log bob 0 "<result [^>]*urn:xmpp:mam:2.*while you were away" 30
check $? "bob got the missed message from MAM after restarting"
# the prefs were remembered (kv mam/prefs-set in messages.db): no prefs IQ
expect bob 0 "MAM prefs: already set once" "bob's archive prefs are not fetched again after the restart"
! since bob 0 | grep -q "Sending.*<prefs xmlns='urn:xmpp:mam:2'"
check $? "no prefs IQ from bob after the restart"
cb=$(win bob "alice@localhost") && shot bob im-8-mam "$cb"

# HTTP upload (XEP-0363): Conversation → Send File..., through the file
# chooser of the session's portal (xdg-desktop-portal-gtk) or GTK's own
head -c 3000 /dev/urandom | base64 > "$work/upload-test.txt"
m=$(mark alice); mb=$(mark bob)
focus alice "$ca"
click alice "$ca" 46 11
DISPLAY=$(disp alice) xdotool mousemove 50 250 click 1
if f=$(win alice "Open File|Send File"); then
	focus alice "$f"; key alice ctrl+l
	type_text alice "$work/upload-test.txt"; sleep 0.5; key alice Return
fi
expect alice "$m" "http-upload: upload-test.txt uploaded to http://127.0.0.1:" "alice uploaded a file"
expect bob "$mb" "Recv.*<body>http://127.0.0.1:[0-9]*/file_share/[^<]*upload-test.txt</body>" "bob got the upload link"
# XEP-0447: the same message carries the file's metadata
expect alice "$m" "Sending.*<file-sharing xmlns='urn:xmpp:sfs:0'><file xmlns='urn:xmpp:file:metadata:0'><media-type>text/plain</media-type><name>upload-test.txt</name><size>[0-9]*</size><hash xmlns='urn:xmpp:hashes:2' algo='sha-256'>" "alice's upload carried <file-sharing> with a SHA-256"
expect bob "$mb" "jabber: sfs: upload-test.txt \(text/plain, [0-9]* bytes, sha-256:[A-Za-z0-9+/=]*\) http://127.0.0.1:[0-9]*/file_share/" "bob parsed the file metadata (sfs-* meta)"

# ---- MUC --------------------------------------------------------------
# Close the IMs, so that the room is the only tab (no tab bar: the row
# positions below assume that).
for u in alice bob; do
	if w=$(win $u "@localhost$"); then focus $u "$w"; key $u ctrl+w; fi
done
m=$(mark alice)
# (joined through the ctl plugin: with the contacts online, the room's
# row in the buddy list moves)
ctl alice join test@conference.localhost alice
if d=$(win alice "Create New Room"); then
	focus alice "$d"; click alice "$d" 295 94
fi
expect alice "$m" "Recv.*<presence[^>]*from='test@conference.localhost/alice'" "alice joined the room"
mb=$(mark bob)
ctl bob join test@conference.localhost bob
expect bob "$mb" "Recv.*<presence[^>]*from='test@conference.localhost/bob'" "bob joined the room"
sleep 1
ca=$(win alice "^test$"); cb=$(win bob "^test$")

focus alice "$ca"; to_entry alice "$ca"
m=$(mark alice); mb=$(mark bob)
type_text alice "hello room"; key alice Return
expect bob "$mb" "Recv.*hello room" "room message reached bob"
# the room's stanza-id: what replies and reactions in rooms address
rsid=$(since bob "$mb" | grep "hello room" | grep -o "<stanza-id [^>]*>" | head -n1 | sed "s/.* id='\([^']*\)'.*/\1/")
roid=$(since alice "$m" | grep "Sending" | grep -o "<origin-id xmlns='urn:xmpp:sid:0' id='[^']*'" | head -n1 | sed "s/.*id='//; s/'$//")
m=$(mark alice); mb=$(mark bob)
key alice Up
shot alice muc-1-editing "$ca"
key alice ctrl+a BackSpace
type_text alice "hello room, edited"; key alice Return
expect alice "$m" "Sending.*<replace xmlns='urn:xmpp:message-correct:0' id='$roid'" "Up in the room sent a correction"
expect bob "$mb" "Recv.*hello room, edited.*<replace" "bob received the room correction"

# bob's rows: topic 103, the message 126 (alice's: + "bob entered" = 149)
focus bob "$cb"
m=$(mark alice); mb=$(mark bob)
row_menu bob "$cb" 420 126 react
react_with bob "thumbs up"
expect bob "$mb" "Sending.*<message[^>]*type='groupchat'.*<reactions xmlns='urn:xmpp:reactions:0'" "bob reacted in the room"
expect alice "$m" "Recv.*<reactions [^>]*urn:xmpp:reactions:0" "alice got the room reaction"

focus bob "$cb"
m=$(mark alice); mb=$(mark bob)
row_menu bob "$cb" 420 126 reply && { type_text bob "room reply from bob"; key bob Return; }
expect bob "$mb" "^room reply from bob</body>.*<reply [^>]*id='$rsid'" "bob replied in the room"
expect alice "$m" "^room reply from bob</body>.*<reply " "alice got the room reply"
sleep 1; shot alice muc-2-reply "$ca"; shot bob muc-2-reply "$cb"

focus alice "$ca"
m=$(mark alice); mb=$(mark bob)
row_menu alice "$ca" 420 149 delete
expect alice "$m" "Sending.*<retract xmlns='urn:xmpp:message-retract:1'" "alice retracted her room message"
expect bob "$mb" "Recv.*<retract [^>]*urn:xmpp:message-retract:1" "bob got the room retraction"
sleep 1; shot bob muc-3-retracted "$cb"

# ---- server features round 2 ----------------------------------------
# IPC: what the server can enforce
m=$(mark bob)
ctl bob modes; ctl bob caps
expect bob "$m" "xmpp-live-ctl: privacy-modes: allow-all deny-users" "privacy-modes: allow-all and deny-users (XEP-0191)"
expect bob "$m" "status-invisible-supported: 0, report-spam-supported: 1" "no XEP-0186 here; XEP-0377 advertised by the test module"

# XEP-0191: bob blocks alice; her message bounces, the push is mirrored
m=$(mark bob)
ctl bob block alice@localhost
expect bob "$m" "Sending.*<block xmlns='urn:xmpp:blocking'><item jid='alice@localhost'/></block>" "bob sent a XEP-0191 block"
expect bob "$m" "Recv.*<iq [^>]*type='set'[^>]*>.*<block xmlns='urn:xmpp:blocking'><item jid='alice@localhost'/>" "bob got the blocklist push"
ma=$(mark alice); mb=$(mark bob)
ctl alice send bob@localhost blocked words
expect alice "$ma" "Recv.*<message[^>]*type='error'.*(service-unavailable|<blocked )" "alice's message to bob bounced"
sleep 2
! since bob "$mb" | grep -q "blocked words"
check $? "bob did not get the message from blocked alice"
m=$(mark bob)
ctl bob unblock alice@localhost
expect bob "$m" "Sending.*<unblock xmlns='urn:xmpp:blocking'><item jid='alice@localhost'/></unblock>" "bob sent a XEP-0191 unblock"
expect bob "$m" "Recv.*<unblock xmlns='urn:xmpp:blocking'><item jid='alice@localhost'/>" "bob got the unblock push"
mb=$(mark bob)
ctl alice send bob@localhost unblocked words
expect bob "$mb" "Recv.*<body>unblocked words</body>" "bob gets alice's messages again"

# XEP-0377 through the report-spam IPC
m=$(mark bob)
ctl bob report spammer@localhost buy now
expect bob "$m" "Sending.*<item jid='spammer@localhost'><report xmlns='urn:xmpp:reporting:1' reason='urn:xmpp:reporting:spam'><text>buy now</text></report></item>" "bob sent a XEP-0377 spam report"
wait_log_file "$work/prosody/prosody.log" "XEP-0377 report on spammer@localhost: urn:xmpp:reporting:spam" 10
check $? "the server got the report"
ctl bob unblock spammer@localhost

# XEP-0319 idle, both ways
ma=$(mark alice); mb=$(mark bob)
ctl alice idle 600
expect alice "$ma" "Sending.*<presence.*<idle xmlns='urn:xmpp:idle:1' since='" "alice's presence carries <idle since=''/>"
expect bob "$mb" "XEP-0319: alice@localhost/pidgin4 idle since" "bob parsed alice's idle time"
mb=$(mark bob)
ctl bob buddy-idle alice@localhost
expect bob "$mb" "buddy-idle alice@localhost: idle, (59[0-9]|60[0-9]|61[0-9]) s" "the buddy list has alice idle for ~600 s"
ctl alice idle 0
sleep 1
mb=$(mark bob)
ctl bob buddy-idle alice@localhost
expect bob "$mb" "buddy-idle alice@localhost: not idle" "alice is not idle any more"

# XEP-0447 on an image: dimensions from the PNG header
python3 - "$work/pixel.png" <<'PY'
import struct, sys, zlib
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
raw = b''.join(b'\0' + b'\xff\x00\x00' * 40 for _ in range(30))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 40, 30, 8, 2, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
ma=$(mark alice); mb=$(mark bob)
ctl alice sendfile bob@localhost "$work/pixel.png"
expect alice "$ma" "Sending.*<file-sharing xmlns='urn:xmpp:sfs:0' disposition='inline'><file xmlns='urn:xmpp:file:metadata:0'><media-type>image/png</media-type><name>pixel.png</name><size>[0-9]*</size><width>40</width><height>30</height>" "the PNG upload carried its dimensions"
expect bob "$mb" "jabber: sfs: pixel.png \(image/png, [0-9]* bytes, sha-256:" "bob parsed the PNG's metadata"

# the invisible status on a server without XEP-0186: sent as available
ma=$(mark alice)
ctl alice status invisible
expect alice "$ma" "XEP-0186: the server has no invisibility" "invisible without XEP-0186 falls back to available"
! since alice "$ma" | grep -q "Sending.*urn:xmpp:invisible:0"
check $? "no <invisible/> sent to a server without XEP-0186"
ctl alice status available
# Insert Image in the room: with the server's upload service the image
# goes into the entry; on send the text goes as a message and the image
# is uploaded (XEP-0363), its URL posted to the room.
python3 - "$work/insert-test.png" <<'EOF'
import sys, zlib, struct
w, h = 24, 16
raw = b''.join(b'\x00' + bytes([0x22, 0x66, 0xcc]) * w for _ in range(h))
def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' +
    chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
    chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
EOF
focus alice "$ca"
m=$(mark alice); mb=$(mark bob)
click alice "$ca" 46 11
DISPLAY=$(disp alice) xdotool mousemove 50 546 click 1
if f=$(win alice "Open File|Insert Image"); then
	focus alice "$f"; key alice ctrl+l
	type_text alice "$work/insert-test.png"; sleep 0.5; key alice Return
fi
sleep 1
focus alice "$ca"; to_entry alice "$ca"
type_text alice "a picture"
shot alice muc-4-image-inserted "$ca"
key alice Return
expect alice "$m" "Sending.*<body>a picture</body>" "alice sent the text next to the image"
wait_log alice "$m" "http-upload: insert-test.png uploaded to http://127.0.0.1:" 20
check $? "alice uploaded the inserted image"
expect bob "$mb" "Recv.*<body>http://127.0.0.1:[0-9]*/file_share/[^<]*insert-test.png</body>" "bob got the image's upload link"
sleep 1; shot bob muc-5-image "$cb"
[ -z "$(ls -A "$work/profile-alice/pidgin4/paste" 2>/dev/null)" ]
check $? "the uploaded image's copy in pidgin4/paste/ was removed"

# ---- logs -------------------------------------------------------------
for u in alice bob alice2; do
	! grep -E "Signal data for .* not found|CRITICAL|in .* failed$" "$(log_of "$u")" >/dev/null
	check $? "no signal/IPC errors or criticals in $u's log"
done

echo "$pass passed, $fail failed (screenshots and logs in $work)"
[ "${XMPP_LIVE_NO_STOP:-}" = 1 ] || "$here/run.sh" stop >/dev/null
[ "$fail" = 0 ]
