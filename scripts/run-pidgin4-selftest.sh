#!/usr/bin/env bash
# run-pidgin4-selftest.sh: run a pidgin4 binary headless (private Xvfb,
# GDK_BACKEND=x11, G_DEBUG=fatal-criticals, its own D-Bus session) with -n,
# for the PIDGIN4_*_SELFTEST developer aids. Accounts are never signed in.
#
# Usage: run-pidgin4-selftest.sh BIN PROFILE [TIMEOUT] [-- EXTRA PIDGIN4 ARGS]
#   Environment variables (e.g. PIDGIN4_WINDOWS_SELFTEST=1) pass through.
#   PROFILE must not be ~/.purple. PIDGIN4_SELFTEST_WAYLAND=1 runs on the
#   current Wayland session instead of a private Xvfb.
set -euo pipefail

bin=${1:?usage: $0 BIN PROFILE [TIMEOUT] [-- ARGS]}
profile=${2:?usage: $0 BIN PROFILE [TIMEOUT] [-- ARGS]}
timeout_s=${3:-300}
shift 2
[ $# -gt 0 ] && [ "$1" != "--" ] && shift
[ $# -gt 0 ] && [ "$1" = "--" ] && shift

real_profile=$(realpath -m "$profile")
case "$real_profile" in
	"$(realpath -m "$HOME/.purple")"|"$(realpath -m "$HOME/.purple")"/*)
		echo "refusing to run on ~/.purple" >&2; exit 2 ;;
esac

export G_DEBUG=fatal-criticals
export GTK_A11Y=none
export GSETTINGS_BACKEND=memory

if [ "${PIDGIN4_SELFTEST_WAYLAND:-}" = 1 ]; then
	export GDK_BACKEND=wayland
	exec timeout --signal=TERM "$timeout_s" dbus-run-session -- \
		"$bin" -m -n -d -c "$profile" "$@"
fi

fifo=$(mktemp -u)
mkfifo "$fifo"
exec 3<>"$fifo"
rm -f "$fifo"
Xvfb -displayfd 3 -nolisten tcp -screen 0 1280x800x24 >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT
read -r -t 10 num <&3 || { echo "Xvfb did not start" >&2; exit 2; }
export DISPLAY=:$num
export GDK_BACKEND=x11
unset WAYLAND_DISPLAY

status=0
timeout --signal=TERM "$timeout_s" dbus-run-session -- \
	"$bin" -m -n -d -c "$profile" "$@" || status=$?
exit $status
