#!/usr/bin/env bash
# check-abi.sh: ABI gates for the private libpurple build.
#
# 1. Symbol superset gate (CLAUDE.md, hard rule): every dynamic symbol that
#    the stock libpurple 2.14.14 defines must also be defined by
#    <prefix>/lib/libpurple.so.0. The reference is scripts/libpurple-2.14.14.syms
#    (the stock library's exports, built from the pidgin-2.14.14 tarball with
#    Gentoo's former USE flags: D-Bus on, voice/video off), since the system
#    copy is replaced by packaging/gentoo. Missing symbols are printed and the
#    script exits non-zero. With --ref LIB and libabigail's abidiff
#    installed, abidiff runs as well and any removed or changed
#    function/variable fails the gate.
#
# 2. Plugin loading gate: every undefined purple_* / serv_* symbol referenced
#    by the user's third-party prpls (~/.purple/plugins/libdiscord.so and
#    libsteam.so by default) must be defined by the new libpurple.so.0.
#
# Prints nothing but the gate headers on success.

set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") [OPTIONS] [PLUGIN.so ...]

Compare the exported symbols of the private libpurple build against the
stock 2.14.14 ones, and check that the given prpl plugins (default: libdiscord.so
and libsteam.so from ~/.purple/plugins) resolve against the new build.

Options:
  --new LIB        new library (default: \$PIDGIN4_PREFIX/lib/libpurple.so.0,
                   PIDGIN4_PREFIX defaults to \$HOME/.local/pidgin4)
  --ref REF        reference: a library, or a list of symbols, one per line
                   (default: scripts/libpurple-2.14.14.syms)
  --no-abidiff     skip abidiff even if it is installed
  --no-plugins     skip the plugin loading gate
  -h, --help       show this help

Exit status is 0 when all gates pass, 1 when a gate fails, 2 on usage errors.
EOF
}

prefix=${PIDGIN4_PREFIX:-$HOME/.local/pidgin4}
new_lib=$prefix/lib/libpurple.so.0
ref_lib=$(dirname "$0")/libpurple-2.14.14.syms
use_abidiff=1
check_plugins=1
plugins=()

while [ $# -gt 0 ]; do
	case "$1" in
		--new) new_lib=$2; shift 2 ;;
		--ref) ref_lib=$2; shift 2 ;;
		--no-abidiff) use_abidiff=0; shift ;;
		--no-plugins) check_plugins=0; shift ;;
		-h|--help) usage; exit 0 ;;
		-*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
		*) plugins+=("$1"); shift ;;
	esac
done

if [ ${#plugins[@]} -eq 0 ]; then
	plugins=("$HOME/.purple/plugins/libdiscord.so" "$HOME/.purple/plugins/libsteam.so")
fi

for f in "$ref_lib" "$new_lib"; do
	[ -e "$f" ] || { echo "No such library: $f" >&2; exit 2; }
done

status=0

# Defined dynamic symbols of a shared object (or a .syms list), one per
# line, sorted.
defined_syms() {
	case "$1" in
		*.syms) sort -u "$1" ;;
		*) nm -D --defined-only "$1" | awk '{print $3}' | sort -u ;;
	esac
}

# --- Gate 1: exported symbol superset --------------------------------------
echo "== ABI: symbols of $ref_lib missing from $new_lib"
missing=$(comm -23 <(defined_syms "$ref_lib") <(defined_syms "$new_lib"))
if [ -n "$missing" ]; then
	printf '%s\n' "$missing"
	echo "FAIL: $(printf '%s\n' "$missing" | wc -l) symbol(s) missing" >&2
	status=1
fi

if [ "$use_abidiff" = 1 ] && [ "${ref_lib%.syms}" = "$ref_lib" ] &&
   command -v abidiff >/dev/null 2>&1; then
	echo "== ABI: abidiff $ref_lib $new_lib"
	# abidiff's exit status is a bit field: 4 = ABI change, 8 = incompatible
	# change. Added symbols alone only set bit 4 with nothing removed, so
	# fail on bit 8 or on any removed/changed function or variable.
	set +e
	report=$(abidiff --no-added-syms "$ref_lib" "$new_lib" 2>&1)
	rc=$?
	set -e
	if [ $((rc & 8)) -ne 0 ] || [ $((rc & 1)) -ne 0 ] \
	   || printf '%s\n' "$report" | grep -Eq '[1-9][0-9]* (Removed|Changed)'; then
		printf '%s\n' "$report"
		echo "FAIL: abidiff reports removed or changed ABI (exit $rc)" >&2
		status=1
	fi
fi

# --- Gate 2: plugin loading ------------------------------------------------
if [ "$check_plugins" = 1 ]; then
	new_syms=$(defined_syms "$new_lib")
	for p in "${plugins[@]}"; do
		echo "== plugin: undefined purple_*/serv_* in $p not defined by $new_lib"
		if [ ! -e "$p" ]; then
			echo "FAIL: plugin not found: $p" >&2
			status=1
			continue
		fi
		unresolved=$(comm -23 \
			<(nm -D --undefined-only "$p" | awk '{print $NF}' | sed 's/@.*//' | sort -u) \
			<(printf '%s\n' "$new_syms") | grep -E '^(purple_|serv_)' || true)
		if [ -n "$unresolved" ]; then
			printf '%s\n' "$unresolved"
			echo "FAIL: $p has unresolved libpurple symbols" >&2
			status=1
		fi
	done
fi

if [ "$status" = 0 ]; then
	echo "== all ABI gates passed"
fi
exit "$status"
