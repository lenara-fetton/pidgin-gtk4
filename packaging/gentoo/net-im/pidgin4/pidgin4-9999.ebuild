# Copyright 2026 Lenara Fetton
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit git-r3 meson optfeature xdg

DESCRIPTION="GTK 4 user interface for libpurple (Pidgin 2.14, modernized)"
HOMEPAGE="https://github.com/lenara-fetton/pidgin-gtk4"
EGIT_REPO_URI="https://github.com/lenara-fetton/pidgin-gtk4.git"
EGIT_BRANCH="main"
# The UI is the pidgin4/ Meson project of the repository.
S="${WORKDIR}/${P}/pidgin4"

LICENSE="GPL-2"
SLOT="0"
# The tests need a display (Xvfb or Wayland).
RESTRICT="test"

RDEPEND="
	>=dev-db/sqlite-3.35:3
	>=dev-libs/glib-2.80:2
	dev-libs/wayland
	>=gui-libs/gtk-4.14:4[wayland]
	gui-libs/gtksourceview:5
	>=app-text/libspelling-0.2
	media-libs/gsound
	net-libs/libsoup:3.0
	~net-im/pidgin-${PV}
"
DEPEND="${RDEPEND}"
BDEPEND="
	dev-libs/wayland-protocols
	dev-util/glib-utils
	dev-util/wayland-scanner
	virtual/pkgconfig
"

pkg_postinst() {
	xdg_pkg_postinst

	optfeature "inline audio and video playback" "gui-libs/gtk:4[gstreamer]"
	optfeature "the Discord protocol" x11-plugins/purple-discord
	optfeature "the Steam protocol" x11-plugins/pidgin-opensteamworks
}
