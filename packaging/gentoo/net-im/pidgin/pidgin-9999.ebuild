# Copyright 2026 Lenara Fetton
# Distributed under the terms of the GNU General Public License v2

EAPI=8

PYTHON_COMPAT=( python3_{12..14} )

inherit autotools git-r3 meson optfeature python-single-r1 xdg

DESCRIPTION="Pidgin 2.14 with a GTK 4 interface and modern XMPP and IRC support"
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
# libpurple's check-based tests are not wired up here, and pidgin4's need a
# display (Xvfb or Wayland).
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
	gui? (
		>=app-text/libspelling-0.2
		>=dev-db/sqlite-3.35:3
		>=dev-libs/glib-2.80:2
		dev-libs/wayland
		>=gui-libs/gtk-4.14:4[wayland]
		gui-libs/gtksourceview:5
		media-libs/gsound
	)
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
	gui? (
		dev-libs/wayland-protocols
		dev-util/glib-utils
		dev-util/wayland-scanner
	)
	nls? ( sys-devel/gettext )
"

src_prepare() {
	xdg_environment_reset
	default
	intltoolize --automake --copy --force || die
	eautoreconf
}

src_configure() {
	# libpurple (autotools, in the source tree). The GTK 2 UI and finch are
	# not built; the UI is pidgin4, configured in src_compile once libpurple
	# is built.
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

	# configure asks gconftool-2 for a schema source unless one is given,
	# even with --disable-schemas-install.
	GCONF_SCHEMA_INSTALL_SOURCE=unused econf "${myconf[@]}"
}

# pidgin4 (the Meson project in pidgin4/) builds against the libpurple
# built above, before it is installed: stage its install and give Meson a
# purple.pc whose variables name the final /usr paths (compiled into
# pidgin4) and whose flags point into the stage.
_pidgin4_setup() {
	export EMESON_SOURCE="${S}/pidgin4"
	export BUILD_DIR="${WORKDIR}/pidgin4-build"
}

src_compile() {
	default

	use gui || return

	local stage="${T}/purple-stage"
	local pcdir="${T}/purple-pc"
	local libdir="${EPREFIX}/usr/$(get_libdir)"

	emake DESTDIR="${stage}" install
	mkdir -p "${pcdir}" || die
	sed -e "s|^Cflags:.*|Cflags: -I${stage}${EPREFIX}/usr/include/libpurple|" \
	    -e "s|^Libs:.*|Libs: -L${stage}${libdir} -lpurple|" \
	    "${stage}${libdir}/pkgconfig/purple.pc" > "${pcdir}/purple.pc" || die

	_pidgin4_setup
	PKG_CONFIG_PATH="${pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" meson_src_configure
	meson_src_compile
}

src_install() {
	default

	python_fix_shebang "${ED}"
	python_optimize

	find "${ED}" -type f -name "*.la" -delete || die

	if use gui; then
		_pidgin4_setup
		meson_src_install
	fi
}

pkg_postinst() {
	use gui && xdg_pkg_postinst

	elog "This replaces the stock Pidgin 2.14.14; the program is pidgin4."
	elog "~/.purple stays compatible with Pidgin 2: to go back, mask this"
	elog "version and re-emerge =net-im/pidgin-2.14.14*::gentoo."

	if use gui; then
		optfeature "inline audio and video playback" "gui-libs/gtk:4[gstreamer]"
		optfeature "the Discord protocol" x11-plugins/purple-discord
		optfeature "the Steam protocol" x11-plugins/pidgin-opensteamworks
	fi
}

pkg_postrm() {
	use gui && xdg_pkg_postrm
}
