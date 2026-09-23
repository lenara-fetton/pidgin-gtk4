#!/usr/bin/env bash
# build-pidgin4.sh: configure (Meson), build, test and install the GTK 4 UI
# in pidgin4/ into the private prefix.
#
# See doc/PIDGIN-UPGRADE.md (Architecture, M2). libpurple must already be
# installed in PURPLE_PREFIX by scripts/build-libpurple.sh. Nothing is ever
# installed outside the prefix, except the optional desktop integration
# links in ~/.local/share (only with --desktop-integration).

set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build pidgin4 against the libpurple in \$PURPLE_PREFIX and install it into
\$PIDGIN4_PREFIX (bin/pidgin4, share/applications, share/icons).

Options:
  -j, --jobs N             parallel build jobs (default: ninja's)
      --builddir DIR       Meson build directory (default: <srcdir>/build-pidgin4)
      --reconfigure        wipe and re-run meson setup
      --buildtype TYPE     Meson buildtype (default: debugoptimized)
      --test               run the unit tests (meson test) after building
      --no-install         build only
      --desktop-integration
                           also link the installed .desktop file and icons
                           into \${XDG_DATA_HOME:-~/.local/share}/{applications,
                           icons} so launchers, portals, GNotification and
                           GNOME's app matching find com.minowick.Pidgin4
                           (the prefix is not on XDG_DATA_DIRS)
      --remove-desktop-integration
                           remove those links again and exit
  -h, --help               show this help

Environment:
  PURPLE_PREFIX    prefix of the libpurple to build against
                   (default: \$HOME/.local/pidgin4); its lib/pkgconfig is
                   put first on PKG_CONFIG_PATH
  PIDGIN4_PREFIX   install prefix (default: \$PURPLE_PREFIX). The binary gets
                   an rpath to \$PURPLE_PREFIX/lib either way.
  PIDGIN4_LOG      build log (default: <srcdir>/build-logs/build-pidgin4.log)

Run it with a copied profile, never ~/.purple before the cutover:
  \$PIDGIN4_PREFIX/bin/pidgin4 -c ~/.purple-gtk4 -n -d
EOF
}

srcdir=$(cd "$(dirname "$0")/.." && pwd)
purple_prefix=${PURPLE_PREFIX:-$HOME/.local/pidgin4}
prefix=${PIDGIN4_PREFIX:-$purple_prefix}
builddir=$srcdir/build-pidgin4
buildtype=debugoptimized
jobs=
reconfigure=0
run_tests=0
do_install=1
desktop=0
remove_desktop=0
app_id=com.minowick.Pidgin4
data_home=${XDG_DATA_HOME:-$HOME/.local/share}

while [ $# -gt 0 ]; do
	case "$1" in
		-j|--jobs) jobs=$2; shift 2 ;;
		-j*) jobs=${1#-j}; shift ;;
		--jobs=*) jobs=${1#--jobs=}; shift ;;
		--builddir) builddir=$2; shift 2 ;;
		--builddir=*) builddir=${1#--builddir=}; shift ;;
		--buildtype) buildtype=$2; shift 2 ;;
		--buildtype=*) buildtype=${1#--buildtype=}; shift ;;
		--reconfigure) reconfigure=1; shift ;;
		--test) run_tests=1; shift ;;
		--no-install) do_install=0; shift ;;
		--desktop-integration) desktop=1; shift ;;
		--remove-desktop-integration) remove_desktop=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

for p in "$prefix" "$purple_prefix"; do
	case "$p" in
		/usr|/usr/*|/) echo "Refusing to use the system prefix '$p'." >&2; exit 1 ;;
	esac
done

say() { printf '==> %s\n' "$*"; }

# The files --desktop-integration links, relative to the prefix's share/.
desktop_files() {
	echo "applications/$app_id.desktop"
	for size in 16x16 22x22 24x24 32x32 48x48; do
		echo "icons/hicolor/$size/apps/$app_id.png"
	done
	echo "icons/hicolor/scalable/apps/$app_id.svg"
	# M6: the tray's status icons. The StatusNotifierItem's IconName is
	# $app_id-<status>, which hosts such as Waybar look up in the icon
	# theme (the item also sends them as pixmaps and names the prefix's
	# share/icons as IconThemePath).
	for size in 16x16 22x22 32x32 48x48; do
		for variant in available away busy extended-away invisible \
		               offline pending connecting; do
			echo "icons/hicolor/$size/apps/$app_id-$variant.png"
		done
	done
}

refresh_desktop_caches() {
	if command -v update-desktop-database >/dev/null 2>&1; then
		update-desktop-database -q "$data_home/applications" || true
	fi
	if [ -f "$data_home/icons/hicolor/index.theme" ]; then
		for cache_tool in gtk4-update-icon-cache gtk-update-icon-cache; do
			if command -v "$cache_tool" >/dev/null 2>&1; then
				"$cache_tool" -q -t "$data_home/icons/hicolor" || true
				break
			fi
		done
	fi
}

remove_desktop_integration() {
	local rel target
	while read -r rel; do
		target=$data_home/$rel
		# Only remove our own links, never a file someone else put there.
		if [ -L "$target" ]; then
			rm -f "$target"
			say "removed $target"
		fi
	done < <(desktop_files)
	refresh_desktop_caches
}

install_desktop_integration() {
	local rel src target
	while read -r rel; do
		src=$prefix/share/$rel
		target=$data_home/$rel
		if [ ! -e "$src" ]; then
			echo "Missing $src; install first (without --no-install)." >&2
			exit 1
		fi
		if [ -e "$target" ] && [ ! -L "$target" ]; then
			echo "Not replacing $target: it is not a link made by this script." >&2
			exit 1
		fi
		mkdir -p "$(dirname "$target")"
		ln -sfn "$src" "$target"
		say "linked $target -> $src"
	done < <(desktop_files)
	refresh_desktop_caches
}

if [ "$remove_desktop" = 1 ]; then
	remove_desktop_integration
	exit 0
fi

pcdir=$purple_prefix/lib/pkgconfig
if [ ! -f "$pcdir/purple.pc" ]; then
	echo "No purple.pc in $pcdir; run scripts/build-libpurple.sh first." >&2
	exit 1
fi
export PKG_CONFIG_PATH=$pcdir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}

log=${PIDGIN4_LOG:-$srcdir/build-logs/build-pidgin4.log}
mkdir -p "$(dirname "$log")"
: > "$log"

setup_args=(--prefix="$prefix" --libdir=lib --buildtype="$buildtype")
if [ "$reconfigure" = 1 ] && [ -d "$builddir" ]; then
	say "meson setup --wipe $builddir"
	meson setup --wipe "${setup_args[@]}" "$builddir" "$srcdir/pidgin4" >> "$log" 2>&1
elif [ ! -f "$builddir/build.ninja" ]; then
	say "meson setup $builddir (libpurple: $purple_prefix, prefix: $prefix)"
	meson setup "${setup_args[@]}" "$builddir" "$srcdir/pidgin4" >> "$log" 2>&1
else
	# Keep an existing build dir, but make sure it installs where asked.
	meson configure "${setup_args[@]}" "$builddir" >> "$log" 2>&1
fi

say "building (log: $log)"
if ! meson compile -C "$builddir" ${jobs:+-j "$jobs"} >> "$log" 2>&1; then
	echo "Build failed; last lines of $log:" >&2
	tail -n 30 "$log" >&2
	exit 1
fi

if [ "$run_tests" = 1 ]; then
	say "running unit tests"
	if ! meson test -C "$builddir" --print-errorlogs >> "$log" 2>&1; then
		echo "Tests failed; see $log and $builddir/meson-logs/testlog.txt" >&2
		exit 1
	fi
fi

if [ "$do_install" = 1 ]; then
	say "installing into $prefix"
	meson install -C "$builddir" --no-rebuild >> "$log" 2>&1
fi

if [ "$desktop" = 1 ]; then
	install_desktop_integration
fi

say "done: $prefix/bin/pidgin4 ($("$builddir/pidgin4" --version 2>/dev/null || echo '?'))"
