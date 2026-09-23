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

# ---- MUC --------------------------------------------------------------
# Close the IMs, so that the room is the only tab (no tab bar: the row
# positions below assume that).
for u in alice bob; do
	if w=$(win $u "@localhost$"); then focus $u "$w"; key $u ctrl+w; fi
done
m=$(mark alice)
bl=$(win alice "Buddy List"); focus alice "$bl"
DISPLAY=$(disp alice) xdotool mousemove --window "$bl" 60 62 click --repeat 2 --delay 80 1
if d=$(win alice "Create New Room"); then
	focus alice "$d"; click alice "$d" 295 94
fi
expect alice "$m" "Recv.*<presence[^>]*from='test@conference.localhost/alice'" "alice joined the room"
mb=$(mark bob)
bl=$(win bob "Buddy List"); focus bob "$bl"
DISPLAY=$(disp bob) xdotool mousemove --window "$bl" 60 62 click --repeat 2 --delay 80 1
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

# ---- logs -------------------------------------------------------------
for u in alice bob alice2; do
	! grep -E "Signal data for .* not found|CRITICAL|in .* failed$" "$(log_of "$u")" >/dev/null
	check $? "no signal/IPC errors or criticals in $u's log"
done

echo "$pass passed, $fail failed (screenshots and logs in $work)"
[ "${XMPP_LIVE_NO_STOP:-}" = 1 ] || "$here/run.sh" stop >/dev/null
[ "$fail" = 0 ]
