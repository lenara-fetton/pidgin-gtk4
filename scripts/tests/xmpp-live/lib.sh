# lib.sh: xdotool helpers for driving the pidgin4 instances started by
# run.sh. Source it; USER is "alice" or "bob" in every function.
work=${XMPP_LIVE_DIR:-${TMPDIR:-/tmp}/pidgin4-xmpp-live}

disp() { cat "$work/display-$1"; }

# win USER TITLE-REGEX: wait (10 s) for a visible window, print its id.
win() {
	local i id
	for i in $(seq 40); do
		id=$(DISPLAY=$(disp "$1") xdotool search --onlyvisible --name "$2" 2>/dev/null | tail -n1)
		[ -n "$id" ] && { echo "$id"; return 0; }
		sleep 0.25
	done
	echo "no window '$2' for $1" >&2
	return 1
}

# There is no window manager: raise and focus explicitly.
focus() {
	DISPLAY=$(disp "$1") xdotool windowraise "$2" windowfocus --sync "$2"
	sleep 0.2
}
key() { local u=$1; shift; DISPLAY=$(disp "$u") xdotool key --delay 60 "$@"; sleep 0.4; }
type_text() { DISPLAY=$(disp "$1") xdotool type --delay 20 "$2"; sleep 0.2; }
# click USER WIN X Y [BUTTON]: coordinates relative to the window
click() {
	DISPLAY=$(disp "$1") xdotool mousemove --window "$2" "$3" "$4" click "${5:-1}"
	sleep 0.5
}
shot() { DISPLAY=$(disp "$1") import -window "${3:-root}" "$work/$2.png"; }
log_of() { echo "$work/pidgin4-$1.log"; }
# since USER MARK: log lines after a byte offset (from mark USER)
mark() { stat -c %s "$(log_of "$1")"; }
since() { tail -c +"$(( $2 + 1 ))" "$(log_of "$1")"; }
# wait_log_file FILE REGEX [TIMEOUT]: wait for REGEX anywhere in FILE
wait_log_file() {
	local i
	for i in $(seq $(( ${3:-10} * 4 ))); do
		grep -qE "$2" "$1" 2>/dev/null && return 0
		sleep 0.25
	done
	return 1
}
# ctl USER COMMAND...: one command line for the instance's ctl plugin
# (ctl-plugin.c); waits (5 s) until the plugin has taken it
ctl() {
	local u=$1 i; shift
	printf '%s\n' "$*" > "$work/ctl-$u.new"
	mv "$work/ctl-$u.new" "$work/ctl-$u"
	for i in $(seq 20); do
		[ -e "$work/ctl-$u" ] || return 0
		sleep 0.25
	done
	echo "ctl: $u did not take '$*'" >&2
	return 1
}
# wait_log USER MARK REGEX [TIMEOUT]: wait for REGEX in the log after MARK
wait_log() {
	local i
	for i in $(seq $(( ${4:-10} * 4 ))); do
		since "$1" "$2" | grep -qE "$3" && return 0
		sleep 0.25
	done
	return 1
}
