#!/usr/bin/env bash
# check-profile-compat.sh: the cutover-gate round-trip from
# doc/PIDGIN-UPGRADE.md (Profile compatibility contract, Cutover gate).
#
# On a scratch copy of a profile:
#   0. copy SRC to <work>/profile and keep a baseline copy + sha256 manifest;
#   a. run pidgin4 for a bounded time            (only with --pidgin4 BIN)
#   b. run the SYSTEM Pidgin 2.14.14 on the copy (/usr/bin/pidgin)
#   c. run pidgin4 again                          (only with --pidgin4 BIN)
#   d. compare the result with the baseline and allow only the expected
#      changes (see scripts/profile-compat.py for the exact rules).
# After every step accounts.xml, blist.xml, prefs.xml and status.xml must
# parse (xmllint --noout) and the account/buddy/contact/group/chat counts
# must be unchanged.
#
# Nothing is ever run against SRC itself; it is only read.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
helper=$here/profile-compat.py

usage() {
	cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Run the cutover-gate profile round-trip on a scratch copy.

Options:
  --src DIR          profile to test (default: \$HOME/.purple-gtk4). Only read.
  --pidgin4 BIN      pidgin4 binary; runs steps (a) and (c). Without it those
                     steps are skipped (pidgin4 does not exist before M2).
  --pidgin4-args S   extra arguments for pidgin4 (word-split), in addition to
                     '-c <scratch profile>'
  --pidgin2 BIN      Pidgin 2 binary for step (b) (default: /usr/bin/pidgin)
  --time SECS        how long each UI runs before SIGTERM (default: 20)
  --with-logs        copy logs/ too. By default logs/ (~2.6 GB) is skipped
                     for speed; the UIs then write new logs into an empty
                     logs/, which is allowed.
  --live             run the UIs on the current session (Pidgin 2 on
                     \$DISPLAY/Xwayland, pidgin4 on Wayland) instead of a
                     private headless Xvfb (pidgin4 then uses GDK_BACKEND=x11)
  --workdir DIR      scratch directory (default: a new mktemp dir); must not
                     exist or be empty
  --keep             keep the scratch directory (it is always kept on failure)
  --allow K:RE       extra XML change rule, passed to profile-compat.py
  --allow-file K:RE  extra file change rule, passed to profile-compat.py
  --dry-run          launch no UI: make simulated "expected" edits instead,
                     to exercise the copy/validate/compare logic
  --simulate-unexpected
                     with --dry-run, also make edits that must be rejected
  --self-test        run --dry-run twice: expected edits must pass and
                     unexpected edits must fail
  -v, --verbose      list the allowed changes too
  -h, --help         show this help

Accounts are never signed in: Pidgin 2 runs with --nologin (-n) and
--multiple (-m), inside its own D-Bus session (dbus-run-session), so it
cannot reach the running Pidgin or the keyring. --nologin makes the offline
status current, which creates a transient status; that is an allowed change.

pidgin4 hooks: pidgin4 must accept '-c DIR', save and quit on SIGTERM, and
should get its "don't sign in" option via --pidgin4-args until it is defined.

Exit status: 0 = pass, 1 = gate failed, 2 = usage/environment error.
EOF
}

src=$HOME/.purple-gtk4
pidgin4=
pidgin4_args=
pidgin2=/usr/bin/pidgin
run_time=20
with_logs=0
live=0
workdir=
keep=0
dry_run=0
sim_unexpected=0
self_test=0
verbose=0
compare_args=()

while [ $# -gt 0 ]; do
	case "$1" in
		--src) src=$2; shift 2 ;;
		--pidgin4) pidgin4=$2; shift 2 ;;
		--pidgin4-args) pidgin4_args=$2; shift 2 ;;
		--pidgin2) pidgin2=$2; shift 2 ;;
		--time) run_time=$2; shift 2 ;;
		--with-logs) with_logs=1; shift ;;
		--live) live=1; shift ;;
		--workdir) workdir=$2; shift 2 ;;
		--keep) keep=1; shift ;;
		--allow) compare_args+=(--allow "$2"); shift 2 ;;
		--allow-file) compare_args+=(--allow-file "$2"); shift 2 ;;
		--dry-run) dry_run=1; shift ;;
		--simulate-unexpected) sim_unexpected=1; shift ;;
		--self-test) self_test=1; shift ;;
		-v|--verbose) verbose=1; compare_args+=(--verbose); shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

# --- self-test: two dry runs, one must pass and one must fail -------------
if [ "$self_test" = 1 ]; then
	st=$(mktemp -d "${TMPDIR:-/tmp}/purple-compat-selftest.XXXXXX")
	trap 'rm -rf "$st"' EXIT
	echo "### self-test 1/2: expected edits must pass"
	if ! "$0" --src "$src" --workdir "$st/1" --dry-run; then
		echo "SELF-TEST FAILED: expected edits were rejected" >&2
		exit 1
	fi
	echo
	echo "### self-test 2/2: unexpected edits must fail"
	if "$0" --src "$src" --workdir "$st/2" --dry-run --simulate-unexpected; then
		echo "SELF-TEST FAILED: unexpected edits were accepted" >&2
		exit 1
	fi
	echo
	echo "### self-test passed"
	exit 0
fi

for tool in rsync xmllint python3 timeout dbus-run-session; do
	command -v "$tool" >/dev/null 2>&1 || { echo "Missing tool: $tool" >&2; exit 2; }
done
[ -d "$src" ] || { echo "Source profile not found: $src" >&2; exit 2; }
[ -f "$src/accounts.xml" ] || { echo "$src has no accounts.xml; not a profile?" >&2; exit 2; }
if [ -n "$pidgin4" ] && [ ! -x "$pidgin4" ]; then
	echo "pidgin4 binary not executable: $pidgin4" >&2; exit 2
fi
if [ "$dry_run" = 0 ] && [ ! -x "$pidgin2" ]; then
	echo "Pidgin 2 binary not executable: $pidgin2" >&2; exit 2
fi

if [ -n "$workdir" ]; then
	if [ -e "$workdir" ] && [ -n "$(ls -A "$workdir")" ]; then
		echo "Work directory is not empty: $workdir" >&2; exit 2
	fi
	mkdir -p "$workdir"
else
	workdir=$(mktemp -d "${TMPDIR:-/tmp}/purple-compat.XXXXXX")
fi
workdir=$(realpath "$workdir")
profile=$workdir/profile
baseline=$workdir/baseline

case "$(realpath "$src")" in
	"$workdir"|"$workdir"/*) echo "Source is inside the work directory." >&2; exit 2 ;;
esac

xvfb_pid=
failed=1
cleanup() {
	if [ -n "$xvfb_pid" ]; then
		kill "$xvfb_pid" 2>/dev/null || true
		wait "$xvfb_pid" 2>/dev/null || true
	fi
	if [ "$failed" = 0 ] && [ "$keep" = 0 ]; then
		rm -rf "$workdir"
	else
		echo "Scratch directory kept: $workdir"
	fi
}
trap cleanup EXIT

say() { printf '==> %s\n' "$*"; }

# --- step 0: scratch copy and baseline ------------------------------------
rsync_opts=(-a --copy-unsafe-links)
# --copy-unsafe-links: plugins/*.so are symlinks out of the profile (some
# relative), which would dangle in the scratch dir; copy their targets.
if [ "$with_logs" = 0 ]; then
	rsync_opts+=(--exclude=/logs/)
	say "copying $src (without logs/) to $profile"
else
	say "copying $src (with logs/) to $profile"
fi
rsync "${rsync_opts[@]}" "$src/" "$profile/"
rsync -a "$profile/" "$baseline/"
(cd "$baseline" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum) > "$workdir/baseline.sha256"
python3 "$helper" counts "$baseline" > "$workdir/baseline.counts"
say "baseline: $(tr '\n' ' ' < "$workdir/baseline.counts")"

# --- validation after each step -------------------------------------------
validate() {
	local step=$1 f ok=1
	for f in accounts.xml blist.xml prefs.xml status.xml pounces.xml; do
		[ -e "$baseline/$f" ] || continue
		if [ ! -e "$profile/$f" ]; then
			echo "FAIL [$step]: $f is missing" >&2; ok=0; continue
		fi
		if ! xmllint --noout "$profile/$f" 2>"$workdir/xmllint.err"; then
			echo "FAIL [$step]: $f does not parse:" >&2
			sed 's/^/    /' "$workdir/xmllint.err" >&2
			ok=0
		fi
	done
	if [ "$ok" = 1 ]; then
		python3 "$helper" counts "$profile" > "$workdir/$step.counts"
		# prefs may grow (additive /pidgin4 keys); everything else must match.
		if ! diff <(grep -v '^prefs=' "$workdir/baseline.counts") \
		          <(grep -v '^prefs=' "$workdir/$step.counts") > "$workdir/$step.counts.diff"; then
			echo "FAIL [$step]: object counts changed (< baseline, > now):" >&2
			sed 's/^/    /' "$workdir/$step.counts.diff" >&2
			ok=0
		fi
	fi
	[ "$ok" = 1 ] || return 1
	say "[$step] XML parses, counts unchanged"
}

# --- running a UI -------------------------------------------------------------
start_xvfb() {
	[ -z "$xvfb_pid" ] || return 0
	command -v Xvfb >/dev/null 2>&1 || { echo "Xvfb not found; use --live" >&2; exit 2; }
	local fifo=$workdir/xvfb.displayfd
	mkfifo "$fifo"
	Xvfb -displayfd 3 -nolisten tcp -screen 0 1280x800x24 3>"$fifo" \
		> "$workdir/xvfb.log" 2>&1 &
	xvfb_pid=$!
	local num
	read -r -t 10 num < "$fifo" || { echo "Xvfb did not start" >&2; exit 2; }
	rm -f "$fifo"
	xvfb_display=:$num
	say "headless Xvfb on $xvfb_display"
}

# run_ui STEP KIND CMD...: KIND is pidgin2 or pidgin4.
run_ui() {
	local step=$1 kind=$2; shift 2
	local log=$workdir/$step.log rc
	local envs=()
	if [ "$live" = 1 ]; then
		[ "$kind" = pidgin2 ] && envs=(-u WAYLAND_DISPLAY)
	else
		start_xvfb
		envs=(-u WAYLAND_DISPLAY DISPLAY="$xvfb_display")
		[ "$kind" = pidgin4 ] && envs+=(GDK_BACKEND=x11)
	fi
	say "[$step] running for ${run_time}s: $*"
	set +e
	env "${envs[@]}" dbus-run-session -- \
		timeout -k 10 "$run_time" "$@" > "$log" 2>&1
	rc=$?
	set -e
	# 124 = still running at the deadline and stopped with SIGTERM (normal).
	if [ "$rc" != 0 ] && [ "$rc" != 124 ]; then
		echo "FAIL [$step]: exited with status $rc before the deadline; last lines of $log:" >&2
		tail -n 20 "$log" | sed 's/^/    /' >&2
		return 1
	fi
	if grep -Eq 'Segmentation fault|Aborted|core dumped|lost its connection to the display' "$log"; then
		echo "FAIL [$step]: crash signature in $log" >&2
		return 1
	fi
	say "[$step] stopped cleanly (status $rc), log: $log"
}

# Pidgin 2 specific checks on its debug log (-d).
check_pidgin2_log() {
	local step=$1 log=$workdir/$1.log ok=1 files
	# Plugins from the profile (Discord, Steam) must load.
	if grep -E "plugins: .*$profile/plugins/.*(not usable|could not|failed)" "$log" >&2; then
		echo "FAIL [$step]: a profile plugin did not load" >&2
		ok=0
	fi
	if grep -Eq "plugins: Unloading plugin" "$log"; then :; else
		echo "FAIL [$step]: no clean shutdown seen in the debug log" >&2
		ok=0
	fi
	if grep -E '\(util\): Error (reading|parsing) file|Error parsing file' "$log" >&2; then
		echo "FAIL [$step]: libpurple reported a parse error" >&2
		ok=0
	fi
	files=$(grep -oE 'util: Writing file [^ ]+' "$log" | awk '{n = split($4, a, "/"); print a[n]}' | sort -u | tr '\n' ' ' || true)
	say "[$step] files saved: ${files:-none}"
	[ "$ok" = 1 ]
}

pidgin4_step() {
	local step=$1
	if [ -z "$pidgin4" ]; then
		say "[$step] skipped: no --pidgin4 binary (pidgin4 arrives in M2)"
		return 0
	fi
	# shellcheck disable=SC2086  # pidgin4_args is word-split on purpose
	run_ui "$step" pidgin4 "$pidgin4" -c "$profile" $pidgin4_args
	validate "$step"
}

pidgin2_step() {
	local step=$1
	run_ui "$step" pidgin2 "$pidgin2" -m -n -d -c "$profile"
	check_pidgin2_log "$step"
	validate "$step"
}

# --- the round trip -----------------------------------------------------------
if [ "$dry_run" = 1 ]; then
	say "[dry-run] no UI is launched; simulating a session's edits"
	if [ "$sim_unexpected" = 1 ]; then
		python3 "$helper" simulate --unexpected "$profile"
		say "[dry-run] also made edits that must be rejected"
	else
		python3 "$helper" simulate "$profile"
	fi
	validate dry-run || true   # counts may legitimately fail in --simulate-unexpected
else
	pidgin4_step a-pidgin4
	pidgin2_step b-pidgin2
	pidgin4_step c-pidgin4
fi

# --- step d: compare with the baseline -----------------------------------------
say "[d-compare] comparing with the baseline"
if python3 "$helper" compare "${compare_args[@]}" "$baseline" "$profile"; then
	say "PASS: profile round-trip preserved everything"
	failed=0
else
	echo "FAIL: unexpected changes (see above)" >&2
	exit 1
fi
