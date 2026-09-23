# Copyright 2026 Lenara Fetton
# Distributed under the terms of the GNU General Public License v2

EAPI=8

PYTHON_COMPAT=( python3_{12..14} )

inherit autotools git-r3 python-single-r1

DESCRIPTION="libpurple 2.14 with modern XMPP and IRC support (the GTK 4 UI is net-im/pidgin4)"
HOMEPAGE="https://github.com/lenara-fetton/pidgin-gtk4"
EGIT_REPO_URI="https://github.com/lenara-fetton/pidgin-gtk4.git"
EGIT_BRANCH="main"

LICENSE="GPL-2"
# Same slot as ::gentoo's net-im/pidgin: this replaces it. libpurple stays
# ABI-compatible with 2.14 (additive changes only), so plugins built against
# the stock package (Discord, Steam, ...) keep loading.
SLOT="0/2"
IUSE="+gui nls +omemo"
REQUIRED_USE="${PYTHON_REQUIRED_USE}"
# The check-based unit tests are not wired up for a libpurple-only build.
RESTRICT="test"

RDEPEND="
	${PYTHON_DEPS}
	>=dev-libs/glib-2.66:2
	>=dev-libs/libxml2-2.6.18:=
	>=dev-libs/dbus-glib-0.71
	>=sys-apps/dbus-0.90
	dev-libs/nspr
	dev-libs/nss
	net-dns/libidn2:=
	net-libs/libsoup:3.0
	$(python_gen_cond_dep '
		dev-python/dbus-python[${PYTHON_USEDEP}]
	')
	omemo? (
		dev-db/sqlite:3
		dev-libs/libgcrypt:=
		>=net-libs/libomemo-c-0.5
	)
"
DEPEND="${RDEPEND}"
BDEPEND="
	${PYTHON_DEPS}
	dev-util/intltool
	virtual/pkgconfig
	nls? ( sys-devel/gettext )
"
# The GTK 2 UI is not built: the UI is pidgin4, which needs this libpurple.
PDEPEND="gui? ( ~net-im/pidgin4-${PV} )"

src_prepare() {
	default
	intltoolize --automake --copy --force || die
	eautoreconf
}

src_configure() {
	local myconf=(
		--disable-static
		# Don't downgrade F_S, we already set it in toolchain, bug #890276
		--disable-fortify
		--disable-gtkui
		--disable-consoleui
		# The system libpurple has always exported the D-Bus API: keep it,
		# for ABI compatibility and purple-remote.
		--enable-dbus
		--with-python3="${PYTHON}"
		--disable-vv
		--disable-gstreamer
		--disable-gstreamer-video
		--disable-gstreamer-interfaces
		--disable-farstream
		--disable-meanwhile
		--disable-avahi
		--disable-perl
		--disable-tcl
		--disable-tk
		--disable-mono
		--disable-doxygen
		--disable-schemas-install
		--disable-cyrus-sasl
		--enable-idn
		--enable-nss=yes
		--enable-gnutls=no
		--with-dynamic-prpls=irc,jabber
		--with-system-ssl-certs="${EPREFIX}/etc/ssl/certs/"
		$(use_enable nls)
		$(use_enable omemo)
	)

	econf "${myconf[@]}"
}

src_install() {
	default

	python_fix_shebang "${ED}"
	python_optimize

	find "${ED}" -type f -name "*.la" -delete || die
}

pkg_postinst() {
	elog "This replaces the stock Pidgin 2.14.14. ~/.purple stays compatible"
	elog "with it: to go back, mask this version and re-emerge"
	elog "  =net-im/pidgin-2.14.14*::gentoo"
	elog "(and unmerge net-im/pidgin4)."
}
