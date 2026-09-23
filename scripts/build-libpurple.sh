#!/usr/bin/env bash
# build-libpurple.sh: configure, build and install libpurple plus the GTK 2
# pidgin/ UI into the private pidgin4 prefix.
#
# See doc/PIDGIN-UPGRADE.md (Architecture, M0). Nothing is ever installed
# outside the prefix; the system Pidgin in /usr is not touched.
#
# The script is idempotent:
#   - autotools files are regenerated (intltoolize + autoreconf -fi) only when
#     configure.ac, acinclude.m4 or m4macros/ are newer than configure;
#   - configure is re-run only when config.status is missing or stale, or when
#     the configure arguments changed since the last run;
#   - make does the rest incrementally.

set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build libpurple (dynamic prpls irc,jabber) and the GTK 2 pidgin/ UI and
install them into \${PIDGIN4_PREFIX:-\$HOME/.local/pidgin4}.

Options:
  -j, --jobs N       parallel make jobs (default: nproc)
      --check        also run 'make check' in libpurple/
      --reconfigure  force configure to run again
      --autoreconf   force intltoolize + autoreconf -fi
      --no-install   build only, do not run 'make install'
      --no-dbus      configure with --disable-dbus (see "D-Bus" below)
  -h, --help         show this help

Environment:
  PIDGIN4_PREFIX     install prefix (default: \$HOME/.local/pidgin4)
  PIDGIN4_LOG        build log file (default: <srcdir>/build-logs/build-libpurple.log)
  CFLAGS etc.        passed through to configure as usual

D-Bus:
  D-Bus is left ENABLED by default. The system libpurple.so.0.14.14 exports
  153 D-Bus symbols (purple_dbus_*, PURPLE_DBUS_TYPE_*, dbus_signals), so a
  --disable-dbus build fails the ABI superset gate (scripts/check-abi.sh).
  Turning D-Bus off is an M1 item and needs that conflict resolved first.
EOF
}

srcdir=$(cd "$(dirname "$0")/.." && pwd)
prefix=${PIDGIN4_PREFIX:-$HOME/.local/pidgin4}
jobs=$(nproc 2>/dev/null || echo 4)
do_check=0
force_configure=0
force_autoreconf=0
do_install=1
dbus_opt=--enable-dbus

while [ $# -gt 0 ]; do
	case "$1" in
		-j|--jobs) jobs=$2; shift 2 ;;
		-j*) jobs=${1#-j}; shift ;;
		--jobs=*) jobs=${1#--jobs=}; shift ;;
		--check) do_check=1; shift ;;
		--reconfigure) force_configure=1; shift ;;
		--autoreconf) force_autoreconf=1; force_configure=1; shift ;;
		--no-install) do_install=0; shift ;;
		--no-dbus) dbus_opt=--disable-dbus; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

case "$prefix" in
	/usr|/usr/*|/) echo "Refusing to install into system prefix '$prefix'." >&2; exit 1 ;;
esac

log=${PIDGIN4_LOG:-$srcdir/build-logs/build-libpurple.log}
mkdir -p "$(dirname "$log")"
: > "$log"

# Echo a step both to the terminal and to the log.
step() { printf '==> %s\n' "$*" | tee -a "$log"; }

# Run a command with all output in the log; on failure show the log tail.
run() {
	printf '\n$ %s\n' "$*" >> "$log"
	if ! "$@" >> "$log" 2>&1; then
		echo "FAILED: $*" >&2
		echo "---- last 40 lines of $log ----" >&2
		tail -n 40 "$log" >&2
		exit 1
	fi
}

cd "$srcdir"
step "source: $srcdir"
step "prefix: $prefix"
step "log:    $log"

# Configure options. Keep the GTK 2 build minimal: no X extras that do not
# work on Wayland anyway, no media, no scripting, NSS as the only SSL backend.
configure_args=(
	--prefix="$prefix"
	--disable-consoleui
	--enable-gtkui
	"$dbus_opt"
	--disable-vv
	--disable-gstreamer
	--disable-gstreamer-video
	--disable-gstreamer-interfaces
	--disable-farstream
	--disable-meanwhile
	--disable-avahi
	--disable-screensaver
	--disable-sm
	--disable-startup-notification
	--disable-gtkspell
	--disable-gevolution
	--disable-cap
	--disable-gestures
	--disable-unity
	--disable-perl
	--disable-tcl
	--disable-tk
	--disable-mono
	--disable-doxygen
	--disable-schemas-install
	--disable-cyrus-sasl
	--enable-nss=yes
	--enable-gnutls=no
	--with-dynamic-prpls=irc,jabber
)

# 1. Regenerate the build system if its inputs changed.
need_autoreconf=$force_autoreconf
if [ ! -x configure ]; then
	need_autoreconf=1
else
	for f in configure.ac acinclude.m4 m4macros/*.m4; do
		if [ -e "$f" ] && [ "$f" -nt configure ]; then
			step "$f is newer than configure"
			need_autoreconf=1
			break
		fi
	done
fi
if [ "$need_autoreconf" = 1 ]; then
	step "regenerating build system (intltoolize, autoreconf -fi)"
	run intltoolize --force --copy --automake
	run autoreconf -fi
	force_configure=1
fi

# 2. Configure if needed.
stamp=.pidgin4-configure-args
args_now=$(printf '%s\n' "${configure_args[@]}" "CFLAGS=${CFLAGS-}" "LDFLAGS=${LDFLAGS-}")
if [ "$force_configure" = 1 ] || [ ! -f config.status ] || [ ! -f Makefile ] \
   || [ configure -nt config.status ] || [ ! -f "$stamp" ] \
   || [ "$(cat "$stamp")" != "$args_now" ]; then
	step "configure ${configure_args[*]}"
	rm -f "$stamp"
	run ./configure "${configure_args[@]}"
	printf '%s\n' "$args_now" > "$stamp"
else
	step "configure is up to date, skipping"
fi

# 3. Build.
step "make -j$jobs"
run make -j"$jobs"

# 4. Optional libpurple tests.
if [ "$do_check" = 1 ]; then
	# libpurple's unit tests need the 'check' framework (dev-libs/check);
	# without it configure silently turns them into a no-op.
	if ! pkg-config --exists check; then
		step "WARNING: 'check' (dev-libs/check) is not installed; libpurple tests are compiled out"
		step "         install it and re-run with --reconfigure --check to run them"
	fi
	step "make -C libpurple check"
	run make -j"$jobs" -C libpurple check
	grep -E '^# (TOTAL|PASS|FAIL|ERROR)' libpurple/tests/test-suite.log 2>/dev/null | tee -a "$log" || true
fi

# 5. Install and verify.
if [ "$do_install" = 1 ]; then
	step "make install"
	run make install

	missing=0
	for f in lib/libpurple.so.0 lib/purple-2/libjabber.so lib/purple-2/libirc.so \
	         bin/pidgin lib/pkgconfig/purple.pc; do
		if [ -e "$prefix/$f" ]; then
			step "ok      $prefix/$f"
		else
			step "MISSING $prefix/$f"
			missing=1
		fi
	done
	[ "$missing" = 0 ] || { echo "Install is incomplete." >&2; exit 1; }
fi

step "done"
