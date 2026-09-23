#!/usr/bin/env bash
# setup-dev-profile.sh: copy the real profile ~/.purple to the development
# profile ~/.purple-gtk4, verbatim.
#
# Per doc/PIDGIN-UPGRADE.md (M0) the copy is exact: rsync -a with no pruning
# and no rewriting. Changes to the development profile (e.g. moving IRC to
# TLS) are made later through the UI. The source is only ever read.

set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Copy the Pidgin profile SRC to DEST verbatim (rsync -a).

Options:
  --src DIR     source profile (default: \$HOME/.purple)
  --dest DIR    destination profile (default: \$HOME/.purple-gtk4)
  --no-logs     skip logs/ (opt-in, for quick scratch copies; the default
                copy includes logs/, which is most of the ~2.7 GB)
  --force       replace an existing DEST (it is synced to match SRC exactly,
                including deleting files that are not in SRC)
  -h, --help    show this help

Tip: quit the running Pidgin first for a fully consistent snapshot.
libpurple writes its XML files atomically, so copying while it runs is
safe, but the copy may be a few seconds behind.
EOF
}

src=$HOME/.purple
dest=$HOME/.purple-gtk4
no_logs=0
force=0

while [ $# -gt 0 ]; do
	case "$1" in
		--src) src=$2; shift 2 ;;
		--dest) dest=$2; shift 2 ;;
		--no-logs) no_logs=1; shift ;;
		--force) force=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

src=${src%/}
dest=${dest%/}

[ -d "$src" ] || { echo "Source profile not found: $src" >&2; exit 1; }

# Never write into the real profile.
src_real=$(realpath "$src")
dest_real=$(realpath -m "$dest")
if [ "$dest_real" = "$src_real" ] || [ "$dest_real" = "$(realpath -m "$HOME/.purple")" ]; then
	echo "Refusing to write to the real profile ($dest_real)." >&2
	exit 1
fi
case "$dest_real/" in
	"$src_real"/*) echo "Destination is inside the source profile." >&2; exit 1 ;;
esac

if [ -e "$dest" ]; then
	if [ "$force" != 1 ]; then
		echo "Destination exists: $dest (use --force to overwrite)" >&2
		exit 1
	fi
	echo "==> $dest exists, syncing it to match $src (--force)"
fi

rsync_opts=(-a --delete --info=stats1)
if [ "$no_logs" = 1 ]; then
	rsync_opts+=(--exclude=/logs/)
	echo "==> skipping logs/ (--no-logs)"
fi

echo "==> rsync ${rsync_opts[*]} $src/ $dest/"
mkdir -p "$dest"
chmod 700 "$dest"
rsync "${rsync_opts[@]}" "$src/" "$dest/"

# Relative symlinks (e.g. plugins/libdiscord.so -> ../../purple-discord/...)
# are copied as-is; they only resolve if DEST sits at the same depth as SRC.
broken=$(find "$dest" -xtype l 2>/dev/null || true)
if [ -n "$broken" ]; then
	echo "WARNING: dangling symlinks in the copy:" >&2
	printf '%s\n' "$broken" | sed 's/^/  /' >&2
fi

echo "==> done: $(du -sh "$dest" | cut -f1) in $dest"
