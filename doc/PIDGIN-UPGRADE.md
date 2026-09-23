# Pidgin 2.14.14 → GTK 4 port, library modernization, modern XMPP and IRCv3

## Context

Pidgin 2.14.14 (`pidgin/`, ~101k lines in 166 files) targets GTK 2.10–2.18. The goal is a personal build that runs natively on Wayland: primarily Sway 1.12 with Waybar, occasionally GNOME 49. The build:

- uses GTK 4 (4.22.5 installed), not libadwaita;
- replaces outdated libraries with current ones;
- adds the XMPP features modern clients rely on: carbons, MAM, HTTP upload, OMEMO, receipts, markers, corrections, and reactions;
- brings IRC up to TLS, built-in SASL, and core IRCv3.

Decisions made:

- Go straight to GTK 4, skipping GTK 3.
- Plain GTK 4.
- Replace GtkIMHtml instead of porting it.
- Build the new UI with Meson.
- XMPP work is part of this effort, and so is core IRCv3.
- Passwords stay in `accounts.xml` as they are today; there is no keyring integration.
- **pidgin4's production profile is `~/.purple` itself**, shared with Pidgin 2.14.14, which must keep working on it at any time. See *Profile compatibility contract*. Development and testing use the copy `~/.purple-gtk4` until the cutover gate passes.

**Hard constraint:** the user's third-party protocol plugins, `~/.purple/plugins/libdiscord.so` and `libsteam.so`, link against `libpurple.so.0` (2.14.x ABI). They use the ssl, dnsquery, proxy, and util_fetch_url APIs, and no D-Bus symbols. libpurple changes must therefore be **ABI-compatible and additive only**: no removed symbols, no struct layout changes, no changed signatures.

Both plugins are the user's own forks and are built from source; the `.so` files are symlinks into the checkouts:

- `~/purple-discord` (`lenara-fetton/purple-discord`). Its HTTP comes from a bundled `purple2compat/http.c` running over `purple_proxy_connect`/`purple_ssl`, and the gateway websocket runs over `purple_ssl`. Reactions and edits are currently rendered as text lines (`libdiscord.c:2829`, `:3166`), and replies as quoted text (`:1553`). Custom emoji are emitted as `<img src="https://cdn.discordapp.com/emojis/…">` (`:2202`).
- `~/pidgin-opensteamworks/steam-mobile` (`lenara-fetton/pidgin-opensteamworks`, 2.0 rewrite). It speaks the CM protocol over a WebSocket on `purple_ssl_connect`, with `purple_dnsquery_a`. Login goes through Steam Guard request prompts. Game presence is parsed (`steam_cm.c:886`), and the refresh token is kept in libsecret, loaded at runtime.

Any plugin changes must leave both `.so` files loadable and working in the **stock** Pidgin 2.14.14. New features have to be built on the original ABI (signals, `purple_core_get_ui_info`, status attributes). A feature that can't be built that way is dropped.

Environment facts, verified:

- GLib 2.88, libspelling 0.4.10, GtkSourceView 5.20, wayland-protocols 1.49, libsoup 3.6, libgcrypt 1.12, sqlite 3.53, meson 1.12, GSound 1.0.3, libomemo-c 0.5.1 (`net-libs/libomemo-c`).
- GTK 4 is built with `USE=-gstreamer`, so it has no media backend and `GtkMediaFile` can't play sound.
- The installed Pidgin is built without GStreamer, so voice/video isn't used today.
- Pidgin UI plugins in use: cap, history, markerline, notify, timestamp_format.
- libpurple plugins in use: autoaccept, joinpart, psychic, statenotify, ssl-nss.
- Accounts in the profile: 7 XMPP, 10 IRC, 2 Discord, 2 Steam. Several are on dead services (GTalk, HipChat, the Slack gateway, Freenode), and there are also accounts for prpls that are no longer built (MSN, Yahoo, AIM/ICQ, Skype, Instagram, Twitter). They are all copied unchanged, since libpurple keeps accounts for unknown prpls.
- All IRC accounts use plaintext on port 6667. IRC SASL only works through cyrus-sasl, which is not installed.
- `~/.purple/logs` is 2.6 GB of HTML logs.

## Architecture

- **The tree stays autotools for libpurple.** It is configured with `--disable-consoleui --disable-dbus --disable-vv` (D-Bus stays on until the ABI question in the M0 findings is decided), dynamic prpls `irc,jabber` only, and no Perl/Tcl/Mono. It installs into a **private prefix**, e.g. `~/.local/pidgin4`, so the system `net-im/pidgin` is never touched.
- **The existing `pidgin/` (GTK 2) stays buildable** by autotools against the modified libpurple. That keeps a working daily driver during the port and lets the XMPP work be tested before the GTK 4 UI is ready. It is deleted at the end.
- **New `pidgin4/` is a standalone Meson project.** It starts as a copy of `pidgin/` and is ported file by file. It finds libpurple through that prefix's `purple.pc` via `PKG_CONFIG_PATH`, links with an rpath into the prefix, and uses `export_dynamic: true`, which plugins need.
  - It provides its own `pidgin-internal.h`, replacing libpurple's uninstalled `internal.h` (72 includes): `_()`/`N_()`, `PURPLE_WEBSITE`, `BUF_LONG`, and the config.h include.
  - It generates `config.h` and `package_revision.h` equivalents from Meson.
  - It builds a GResource for pixmaps.
  - Translations come from the libpurple install, which already uses the shared `pidgin` gettext domain.
- **Testing always uses a copied profile** (`pidgin4 -c ~/.purple-gtk4`) so `~/.purple` is never mutated.
- **New profile files** all live in `<profile>/pidgin4/` (see the contract above):
  - `plugins/`: pidgin4 UI plugins;
  - `messages.db`: the message index, MAM state and the SASL2 FAST tokens;
  - `omemo.db`;
  - `image-cache/`;
  - `gtk4.css`.
  Pointing the system Pidgin at the same profile ignores that directory.
- **Desktop entry.** The private prefix isn't on `XDG_DATA_DIRS`. The install script therefore also installs `com.minowick.Pidgin4.desktop` and the app icons into `~/.local/share/{applications,icons}`, with `Exec=` pointing into the prefix. GNotification, portals and GNOME's app matching need this.
- **App ID:** `com.minowick.Pidgin4`. It is used for the `.desktop` file name (`com.minowick.Pidgin4.desktop`), the Wayland `app_id`, the GApplication D-Bus name, GNotification, the SNI item id, icon/metainfo names, and portals. The Sway rule `assign [class="Pidgin"] 2:chat` (`~/.config/sway/config:291`) needs an `app_id` equivalent for the native-Wayland build.

## Profile compatibility contract

pidgin4 and the plugins must leave `~/.purple` in a state that Pidgin 2.14.14 can load, use and save without loss. The rules:

1. **Existing files keep their formats.** These are `accounts.xml`, `blist.xml`, `prefs.xml`, `status.xml`, `pounces.xml`, `smileys.xml`, `logs/`, `icons/`, `custom_smiley/`, `certificates/` and `plugins/`. libpurple still writes all of them; no new elements, no changed element meanings.
2. **Additive keys only, and under new names.** 2.14's parsers keep unknown account `<setting>`s, blist node settings and prefs through a load/save cycle, so new keys are safe. A key's type or meaning never changes. Wherever pidgin4 means something different from the GTK 2 UI (sound method, browser, window geometry, theme names, conversation placement, plugin list), it uses its own `/pidgin4/…` pref subtree. Plain shared semantics (e.g. `/purple/…`, timestamps, logging on/off) stay shared.
3. **The plugin list is separate.** `/pidgin/plugins/loaded` holds absolute paths to GTK 2 plugins (`/usr/lib64/pidgin/history.so`, …), and loading those into a GTK 4 process aborts. pidgin4 uses `/pidgin4/plugins/loaded`. Its search paths are the prefix's `lib/purple-2`, `lib/pidgin4` and `~/.purple/pidgin4/plugins/`, plus `~/.purple/plugins` for prpls. It never searches `/usr/lib64/pidgin` or `/usr/lib64/purple-2`.
4. **Nothing new goes in `~/.purple/plugins`.** libpurple probes (dlopens) every `.so` there at startup (`purple_plugins_probe`), so a GTK 4 plugin there would crash Pidgin 2. It keeps only the Discord and Steam `.so` files, which load in both (see M9). pidgin4 UI plugins go in `~/.purple/pidgin4/plugins/`; the OMEMO plugin lives in the prefix.
5. **New files only under `~/.purple/pidgin4/`.** No 2.14 file or directory uses that name.
6. **UI id stays `gtk-gaim`** (`PIDGIN_UI`). Per-UI account state, such as enabled/auto-login and the buddy icon path, is therefore shared: an account enabled in one UI is enabled in the other.
7. **Logs stay self-contained.** Native events (reactions, corrections, retractions, replies) also get a readable text fallback line in the HTML log, e.g. `X reacted 👍 to: …`, or the corrected text marked as an edit. Pidgin 2 and plain backups therefore see the full history without `messages.db`.
8. **One UI at a time.** At startup, pidgin4 checks for a running Pidgin 2 that uses the same profile directory, via `/proc/*/cmdline` + `-c` and a default-profile match. If it finds one, it shows a dialog and refuses to start. Pidgin 2 can't check the other way.
9. **Pidgin 2 in between is expected.** After a Pidgin 2 session:
   - the message index catches up on log files changed since its last pass (by mtime and size) before MAM runs;
   - MAM dedup falls back to fuzzy matching (time ±2 min, sender, plain text) for lines that have no ids.

**Consequences (accepted):**
- Features that only exist in pidgin4 degrade while Pidgin 2 is running:
  - OMEMO messages show only the fallback body. pidgin4 recovers them later through MAM, as long as they are within the server's archive retention.
  - Carbons, 0490 read-sync and SASL2/FAST don't happen.
- Structured metadata (message ids, reactions as data) exists only in `messages.db`. Losing it degrades features but loses no history.
- A pidgin4-only setting can never be stored in a shared key, even when that would be convenient.

**Cutover gate.** pidgin4 runs against `~/.purple` only after the user explicitly approves, and after this check passes on a copy:
1. Run pidgin4 for a session.
2. Run Pidgin 2.14.14 on the same copy: it must load without errors, with all accounts, buddies, prefs and plugins intact, and save.
3. Run pidgin4 again: no lost settings.
4. `diff -r` against the starting copy shows only the expected changes: settings, logs, and the new `pidgin4/` directory.

Back up `~/.purple` before the first real run.

## Library modernization

| Area | Current | Replacement | Where |
|---|---|---|---|
| Toolkit | GTK 2 | GTK 4.22 (plain) | pidgin4 |
| Spellcheck | gtkspell-2.0 | libspelling + GtkSourceView 5 buffer (built-in undo; delete `gtksourceundomanager.c`, `gtksourceiter.c`) | pidgin4 |
| Idle | XScreenSaver (`gtkidle.c:72`) | Wayland `ext-idle-notify-v1` (Sway), `org.gnome.Mutter.IdleMonitor` D-Bus (GNOME), libpurple's "purple" idle mode as fallback | pidgin4 |
| Session | libSM/ICE (`gtksession.c`) | Drop; GtkApplication | pidgin4 |
| Tray | GtkStatusIcon | StatusNotifierItem + a small `com.canonical.dbusmenu` exporter built from GMenuModel, written directly on GDBus (avoids the unmaintained libdbusmenu) | pidgin4 |
| Single instance / CLI | libpurple D-Bus + bundled getopt | GApplication uniqueness + `g_application_add_main_option_entries`; delete `getopt*.c` | pidgin4 |
| Icons | GtkIconFactory/stock, `DATADIR/pixmaps` paths (~20 hard-coded sites) | GResource + `GtkIconTheme` resource path, icon names | pidgin4 |
| Sounds | GStreamer playbin w/ gconf/esd sinks, custom command | GSound (`gsound_context_play_simple` with `media.filename`) plus the custom-command option. GTK has no media backend here (`USE=-gstreamer`). | pidgin4 |
| URL opening | gconf/registry handler discovery (`gtkutils.c:3504`) | `GtkUriLauncher` / `GtkFileLauncher` (portal-aware) | pidgin4 |
| Accel map | `~/.purple/accels` (`gtk_accel_map_*`) | Fixed `gtk_application_set_accels_for_action` | pidgin4 |
| Theming | gtkrc, pidginrc | CSS provider (+ optional `~/.purple/gtk4.css`) | pidgin4 |
| Desktop/appdata i18n | intltool | gettext `msgfmt` (Meson i18n) | build |
| IDN | libidn 1.x | libidn2 (`network.c`, `protocols/jabber/jutil.c`) | libpurple (internal) |
| Network state | NetworkManager / libnm | `GNetworkMonitor` (`network.c`) | libpurple (internal) |
| Proxy detection | `gconftool-2` exec (`proxy.c:359`) | `GProxyResolver` / GSettings; drop gconf | libpurple (internal) |
| DNS / SRV | fork-based resolver child, `res_query` | `GResolver` async behind the same `purple_dnsquery_*` / `purple_srv_*` API (Steam uses dnsquery) | libpurple (internal) |
| D-Bus | dbus-glib/libdbus | Disabled (drops purple-remote, musicmessaging, dbus-example) | build flag |
| HTTP (XMPP upload) | — | libsoup 3 inside the jabber prpl | jabber prpl |
| HTTP (remote inline images) | — (GtkIMHtml ignores `<img src=URL>`) | libsoup 3 in pidgin4, with a host allowlist and a disk cache in `<profile>/pidgin4/image-cache/` | pidgin4 |
| Message search/metadata | Linear scan of HTML logs | SQLite (FTS5) index, `<profile>/pidgin4/messages.db`, alongside the unchanged HTML logs | pidgin4 |
| IRC SASL | cyrus-sasl (not installed) | Built-in SASL PLAIN and EXTERNAL (client cert) | irc prpl |
| TLS channel binding | — | New additive `purple_ssl_get_channel_binding()` (tls-exporter, RFC 9266), implemented in ssl-nss with `SSL_ExportKeyingMaterial`. NSS stays the backend. | libpurple (additive) + ssl-nss |

Deferred, optional: a GIO-TLS `ssl-gio` plugin to replace ssl-nss/gnutls, and moving custom ciphers onto GChecksum/GHmac. Neither is needed. ssl-nss stays; channel binding is added to it.

Kept as-is: plaintext passwords in `accounts.xml`, which was a deliberate choice.

## Milestones

### M0: Repository baseline
- `.gitignore` for autotools output, baseline commit of the pristine tree, work branch.
- Script: configure/build/install libpurple (+ GTK 2 pidgin) into the private prefix; copy `~/.purple` → `~/.purple-gtk4` **verbatim**, with no pruning or rewriting, for development. Changes such as moving IRC accounts to TLS are made through the UI, using existing 2.14 setting keys (`ssl`, `port`).
- Script: `check-profile-compat.sh`, which runs the cutover-gate round-trip on a scratch copy. It is re-run at the end of every milestone that touches prefs, accounts, blist or logs.
- Record this plan in `doc/GTK4-MIGRATION.md`, replacing the generic survey sections with the chosen path.

**Status: done** (branch `gtk4-port`). Scripts, all with `--help`:
- `scripts/build-libpurple.sh [-j N] [--check] [--reconfigure] [--autoreconf] [--no-dbus]`: in-tree configure/build/install of libpurple (dynamic `irc,jabber`, NSS, no gnutls/vv/GStreamer/NM/meanwhile/avahi/Perl/Tcl/Mono/finch/gtkspell/XSS/SM) and the GTK 2 `pidgin/` into `${PIDGIN4_PREFIX:-~/.local/pidgin4}`. Re-runs `intltoolize` + `autoreconf -fi` when `configure.ac`, `acinclude.m4` or `m4macros/` are newer than `configure`. Logs to `build-logs/`. The pristine tree builds with GCC 16, GLib 2.88 and GTK 2.24.33 without patches.
- `scripts/check-abi.sh`: the exported-symbol superset gate against `/usr/lib64/libpurple.so.0.14.14` (plus `abidiff` when installed), and the plugin gate: every undefined `purple_*`/`serv_*` symbol in `~/.purple/plugins/libdiscord.so` and `libsteam.so` must be defined by the new `libpurple.so.0`. Both pass; the symbol sets are identical.
- `scripts/setup-dev-profile.sh [--no-logs] [--force]`: verbatim `rsync -a` of `~/.purple` to `~/.purple-gtk4`. Done once (2.7 GB).
- `scripts/check-profile-compat.sh [--pidgin4 BIN] [--dry-run|--self-test]` with `scripts/profile-compat.py`: the cutover-gate round-trip on a scratch copy. Step (b), the system Pidgin 2.14.14 with `--nologin` in a private Xvfb and D-Bus session, passes on `~/.purple-gtk4`. The pidgin4 steps are hooks until M2.

Findings for later milestones:
- **D-Bus vs. the ABI gate.** The system libpurple is built with D-Bus and exports 153 D-Bus symbols (`purple_dbus_*`, `PURPLE_DBUS_TYPE_*`, `dbus_signals`). A `--disable-dbus` build drops them and fails the superset gate, so M0 builds with D-Bus **on** (`--no-dbus` exists but is not the default). M1's "D-Bus off" needs a decision first: keep D-Bus, export ABI-compatible stubs for those symbols when D-Bus is disabled, or explicitly exempt them from the gate (no consumer of the prefix libpurple uses them).
- `make check` is a no-op: the `check` framework (`dev-libs/check`) is not installed, so libpurple's unit tests are compiled out. Install it before relying on `--check`.
- Autotools here are autoconf 2.73, automake 1.18, libtool 2.5 (the tarball was generated with 2.72/1.16.5). `--autoreconf` works but rewrites the tracked `configure`, `Makefile.in`s, `aclocal.m4`, `ltmain.sh` etc.; commit those together with any `configure.ac` change.
- GCC 16 warnings worth a look when touching the code: `abs()` on `long` in `libpurple/buddy.c:741-742` (truncation), `-Wunterminated-string-initialization` in `libpurple/prefs.c:1625`, `-Wstringop-truncation` in `libpurple/util.c:3313`, and many `-Wcast-function-type` casts in callback tables.
- Pidgin 2 itself drops untyped account `<setting>`s on load (one Steam account has `<setting name='buddy_icon'/>`); the compat check allows that removal.

### M1: libpurple modernization (ABI-safe, testable with the GTK 2 UI)
- libidn2, GNetworkMonitor, GProxyResolver, GResolver-backed dnsquery/SRV.
- `purple_ssl_get_channel_binding()` (additive) + the ssl-nss implementation.
- ABI gate: `nm -D --defined-only` symbol list must be a superset of `/usr/lib64/libpurple.so.0.14.14`. Use `abidiff` if libabigail is installed.
- **D-Bus stays enabled in libpurple.** Disabling it drops 153 exported `purple_dbus_*` symbols, which the ABI superset rule forbids. pidgin4 doesn't use it; `scripts/build-libpurple.sh --no-dbus` exists for experiments only. (Decided 2026-09-22.)

**Status: done 2026-09-22** (merged from three parallel branches; ABI gate passes on the merged build).

DNS / SRV / TXT:
- `dnsquery.c` uses `g_resolver_lookup_by_name_async`; `dnssrv.c` uses `g_resolver_lookup_records_async` (SRV/TXT). The fork()ed resolver children, their pipe protocol and the Win32 threads are gone. Public API, UI-ops hooks, callback contract (async only, `addrlen`/`sockaddr` pairs with IPv6, RFC 2782 SRV order) and structs are unchanged; cancel is a `GCancellable` plus detach.
- Behaviour changes: error text comes from GResolver; multi-string TXT records are concatenated; the UI-ops failure path calls TXT callbacks with the TXT signature.
- libpurple links `$(GIO_UNIX_LIBS)` (existing optional configure check; `libpurple/Makefile.in` hand-edited to match). Making GIO a hard `configure.ac` requirement is left to whoever next edits `configure.ac`. `-lresolv` stays: `network.c` still calls `res_init()`.

IDN, network state, proxy:
- **libidn2**: `configure.ac` checks `libidn2` (`IDN2_*`, `USE_IDN` kept) and now requires GLib/GIO ≥ 2.66; `--enable-nm` is gone. `purple_network_convert_idn_to_ascii()` is IDNA2008/UTS #46 non-transitional, with STD3 checked by hand (libidn2's STD3 flag silently deletes characters). Jabber node/resource/SASLprep are GLib reimplementations of stringprep (no bidi check, current Unicode; domains lowercased, not case folded, and validated via the libpurple IDN function); see the comment in `jutil.c`.
- **GNetworkMonitor** backs `purple_network_is_available()` and `network-configuration-changed` (which fires on every routing change now); UI `network_connected`/`disconnected` run only on availability transitions. NM and the Win32 NLA code are removed.
- **GProxyResolver** (synchronous lookup, results interned) replaces the gconftool-2 exec for "Use GNOME Proxy Settings", and serves "Use Environmental Settings" when no `http_proxy` is set, followed by `all_proxy` (libproxy ignores it). Non-GNOME "use global" still means `/purple/proxy` prefs.

Channel binding:
- **Channel binding:** `purple_ssl_get_channel_binding(gsc, type, &len)`. The only new export; ABI gate passes. The backend hook uses the `_purple_reserved2` slot of `PurpleSslOps` (now `get_channel_binding`), so the struct layout is unchanged. ssl-nss supports `tls-exporter` (TLS 1.3 only; checked against OpenSSL's server-side exporter) and `tls-server-end-point`. `tls-unique` returns NULL because NSS has no public API for it. M8's SCRAM-PLUS therefore needs TLS 1.3 for `tls-exporter`, or falls back to `tls-server-end-point`. NSS 3.129 already enables TLS 1.2–1.3 by default.

### M2: `pidgin4/` skeleton: sign in and stay connected
- Meson project, `pidgin-internal.h`, GResource, `GtkApplication` startup that replaces `main()`/`gtk_main` in `gtkmain.c` and reuses its core-init order.
  - `gtkeventloop.c` is reused unchanged: it is GLib-only.
- Signal handling moves from the `socketpair` hack to `g_unix_signal_add`.
- Port the ops needed to connect: connection (`gtkconn.c`), request (`gtkrequest.c`, fully async, no `gtk_dialog_run`), notify (`gtknotify.c`), debug (plain `GtkTextView`, no IMHtml), account manager and editor (`gtkaccount.c`).
- Unported UiOps structs are left unset. libpurple tolerates NULL ops.
- Profile rules from the contract: the `gtk-gaim` UI id, the `/pidgin4/…` pref subtree, `/pidgin4/plugins/loaded`, the restricted plugin search paths, and the single-UI startup check.
- `gtkrequest.c` has to cover every field type the plugins use, including **image fields**, which Discord's QR login uses (via libqrencode), and multi-step input prompts (Steam Guard code and approve, captcha flows).
- Menus: a shared helper converts a `PurpleMenuAction` tree into `GMenuModel` + a per-menu `GSimpleActionGroup`. It replaces `pidgin_append_menu_action` (`gtkutils.c:1770`) and is used for blist-node, protocol, plugin, and conversation extended menus.

**Status: done, except the sign-in checks, which are left to the user** (see `pidgin4/TESTING.md`).

**Build and run:**
- `scripts/build-pidgin4.sh [--test] [--reconfigure] [--no-install] [--desktop-integration | --remove-desktop-integration]` does the Meson setup, compile, test and install.
- It builds against `PURPLE_PREFIX` and installs into `PIDGIN4_PREFIX`; both default to `~/.local/pidgin4`. The binary gets a runpath to `$PURPLE_PREFIX/lib`, and the script refuses `/usr`.
- `--desktop-integration` symlinks the installed `.desktop` file and icons into `~/.local/share`, and only then touches it.
- Run it with `~/.local/pidgin4/bin/pidgin4 -c ~/.purple-gtk4 [-n] [-d]`.
- Options: `-c DIR`, `-d` (stdout plus the debug window for this session), `-f`, `-l[NAME]`, `-m`/`--allow-multiple`, `-n`, `-v`. `--help` comes from GOption.

**What's in `pidgin4/`:**
- **Build files**
  - `meson.build`: `export_dynamic`, runpath, `config.h`, `package_revision.h` via `vcs_tag`.
  - The GResource with the app icon and the jabber/irc/gtalk/facebook protocol icons as themed icon names.
  - `style.css`, the desktop file and the hicolor icons.
- **`pidgin-internal.h`**: replaces libpurple's `internal.h`.
- **`gtkmain.c`**: GtkApplication startup.
  - It keeps Pidgin 2's core-init order.
  - It uses `g_unix_signal_add` for SIGINT, SIGTERM and SIGHUP (save and exit 0).
  - GApplication uniqueness: a second launch raises the running instance, and `-m` turns uniqueness off.
  - App actions `quit` (Ctrl+Q), `accounts`, `debug` and `about`.
  - The app holds itself, so it keeps running with no window open.
- **`gtkeventloop.c`**: unchanged.
- **Ported UI**
  - **`gtkconn.c`**: auto-reconnect as before. A fatal error disables the account and shows a `GtkAlertDialog` (Close / Modify Account / Re-enable).
  - **`gtkrequest.c`**: fully async. All field types, including image (QR code, integer-scaled to at least 256 px), account and list; file and folder via `GtkFileDialog`.
  - **`gtknotify.c`**:
    - messages use `GtkAlertDialog`;
    - formatted text and user info use a selectable label, with purple HTML turned into Pango markup;
    - search results use a `GtkColumnView`;
    - there is a simple New Mail window;
    - URIs open via `GtkUriLauncher`.
  - **`gtkdebug.c`**: a `GtkTextView` with level and regex filter, pause, clear and save.
  - **`gtkaccount.c`**:
    - the accounts window is a `GtkColumnView` with enable toggles, status and error, an Account Actions menu and a main menu;
    - the editor has Basic, Advanced and Proxy tabs, and its save logic follows `ok_account_prefs_cb`;
    - the account UI ops (added, add request, authorize) are small windows.
- **Helpers**
  - `gtkutils.[ch]`: `PidginItem`; item, account and protocol `GtkDropDown`s; protocol `GIcon`s, with a fallback to Pidgin 2's `pixmaps/pidgin/protocols` directories for third-party prpls; dialog windows; HTML → Pango; URI opening.
  - `pidginmenu.[ch]`: `PurpleMenuAction` and `PurplePluginAction` → `GMenu` + `GSimpleActionGroup`.
  - `pidginsingleui.[ch]`: the `/proc` single-UI check.
  - `gtkdialogs.c`: About.
  - `gtkprefs.c`: the `/pidgin4` pref registration.
- **Tests**: `meson test` runs the `pidginmenu` test (nested submenus, separators, callbacks, plugin actions), the `singleui` test (cmdline parsing) and a `desktop-file-validate` check.
- **Headless selftests**: `PIDGIN4_REQUEST_SELFTEST=1` and `PIDGIN4_ACCOUNT_SELFTEST=1`.

**Profile contract, as implemented:**
- The UI id is `gtk-gaim`.
- Every pidgin4 pref is under `/pidgin4`: `plugins/loaded`, `debug/*`, `accounts/dialog/*`, `filelocations/*` and `last_version`. `/pidgin/…` is never written; `/pidgin/filelocations` is only read, as a fallback.
- Plugin search paths are `<profile>/pidgin4/plugins` (created), `<profile>/plugins`, `<prefix>/lib/pidgin4`, and libpurple's own `<purple prefix>/lib/purple-2`, with no `/usr` path. The debug log confirms this.
- The single-UI check matches Pidgin 2 by argv[0] or exe name. It handles `-c`, `-cDIR`, `--config[=]`, clustered short options and relative paths (via `/proc/PID/cwd`), and falls back to `$HOME/.purple` from `/proc/PID/environ`. It was tested against a live Pidgin 2 on a scratch profile (exit 1) and read-only against the running one on `~/.purple` (detected).

**UI ops left NULL** (libpurple checks every call site):
- conversations (M4): messages are logged but not shown;
- xfers, privacy and roomlist (M5);
- sound and idle (M6): see below;
- whiteboard and media: dropped.

**Exceptions to "libpurple tolerates NULL ops":**
- **Blist ops.** libpurple saves `blist.xml` only through the blist UI ops: `purple_blist_set_ui_ops()` fills in its own savers for NULL slots, but only when the struct is set. So `stubs.c` sets an all-NULL `PurpleBlistUiOps`. TODO(M3).
- **Pounces.** While parsing `pounces.xml`, libpurple drops an action's `<param>`s unless the UI registered that action when the pounce was created, and then rewrites the file without them. This is a real data loss seen on the dev profile: the `execute-command` command vanished. `stubs.c` registers the pounce handler and the five actions as `gtkpounce.c` does, and runs `execute-command`. The editor and the other actions are TODO(M5).

**Stubs and TODOs by milestone:**
- **M3** (all done; see the M3 status note)
  - the buddy list (`stubs.c` blist ops);
  - connection-error mini-dialogs (alerts for now);
  - the status box and connecting throbber;
  - buddy icons in the account editor;
  - moving the account request windows into the blist;
  - add-buddy from the account request;
  - `pidgin_dialogs_destroy_all`.
- **M4**
  - conversations;
  - rich text in notify/request (`pidgin_html_to_pango_markup` for now; images in user info are dropped);
  - the "html" input hint;
  - "Send IM" from the authorize window.
- **M5**
  - the preferences window;
  - the pounce UI;
  - account DnD reordering;
  - the GTK 2 mail dialog;
  - xfer, privacy and roomlist;
  - `~/.purple/pidgin4/gtk4.css`.
- **M6**
  - system idle (`gtkidle.c` returns no ops, so with the shared `idle_reporting=system` libpurple falls back to its own "purple" last-activity timer for auto-away and reports no idle time);
  - sounds (`gtksound.c` returns no ops);
  - the tray.

**Verification done:**
- **Build**: clean, with zero warnings under `-Wall -Wextra` and `GDK_VERSION_MIN_REQUIRED=4.14` (so no deprecated GTK 4 API). All 3 unit tests pass.
- **Headless**: Xvfb, `GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`, `dbus-run-session`, `pidgin4 -c ~/.purple-gtk4 -n -d`.
  - It starts, probes the Discord and Steam plugins from the profile, loads all 34 accounts and opens the accounts window.
  - The account selftest opened 34 Modify editors and cycled Add through all 4 protocols.
  - The request selftest opened every request kind. The request port was also driven with xdotool; the callbacks received the typed values, and required fields blocked OK.
  - The debug window works.
  - SIGTERM saves and exits 0.
  - `prefs.xml` gains the `/pidgin4` subtree, and `<profile>/pidgin4/plugins/` is created.
- **Wayland**: the same run under the Sway session with `GDK_BACKEND=wayland` is clean and exits 0.
- **Profile round-trip**: `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4-m2/bin/pidgin4` **PASSES**. The script now always passes `-n -d` to pidgin4 and applies the Pidgin 2 log checks to it.
- **No account was signed in.**

**Known gaps:**
- **libpurple criticals.** With the dev profile, libpurple logs about 120 of them, which pidgin4 does not cause: `purple_presence_set_status_active: assertion 'status != NULL'` for buddies of accounts whose prpl is gone, and more of the same kind at quit. `G_DEBUG=fatal-criticals` would abort on them. pidgin4 therefore uses `g_test_log_set_fatal_handler` to make criticals without a log domain (libpurple, prpls) non-fatal; GTK, GLib and `pidgin4`-domain criticals stay fatal.
- **GtkAlertDialog** has no title or icon, so error, warning and info messages look alike.
- **GTK 4 structured logs.** The debug window hooks the classic `g_log` handlers, as Pidgin 2 did. Messages GTK emits through structured logging probably go straight to stderr and never reach the window (not checked).
- **Sign-in not yet run**: Discord QR login, Steam Guard prompts and real sign-in have not been run. They are the checklist in `pidgin4/TESTING.md`.

### M3: Buddy list and status
- Rewrite `gtkblist.c` on `GtkListView` + `GtkTreeListModel` over `PurpleBlistNode` (no deprecated GtkTreeView in new code).
- Rows are widgets with icon, name, status, and emblems, replacing `gtkcellrendererexpander.c` and the theme-driven cell drawing.
- `PidginBlistTheme` colors/fonts map to CSS classes.
- Menubar becomes a `GMenuModel` built from the `blist_menu[]` definitions (`gtkblist.c:3614`), with dynamic Accounts/Tools/Sort sections.
- Context menus become `GtkPopoverMenu`.
- Tooltips use `GtkWidget::query-tooltip` (the `drawing-tooltip` signal is kept for cap).
- The mini-dialog/error area (`minidialog.c`, `gtkscrollbook.c`) is reimplemented as simple `GtkBox`/`GtkStack` widgets.
- `gtkstatusbox.c` (custom GtkContainer) is replaced by a `GtkMenuButton` + popover with a compose entry for the status message.
- DnD for buddy reordering and file drops uses `GtkDragSource`/`GtkDropTarget`.

**Status: done, except the checks with accounts signed in, which are left to the user** (the M3 section of `pidgin4/TESTING.md`).

**What's in `pidgin4/`:**
- **`pidginblistmodel.[ch]`**: the model, apart from the widgets so it is unit tested.
  - `PidginBlistNodeItem` wraps a `PurpleBlistNode`. Its display properties (`name`, `secondary`, `idle`, `status-icon`, `emblem`, `protocol-icon`, `buddy-icon`, `style`) notify only on change.
  - `PidginBlistModel` keeps a hash `node → item` and one `GListStore` per level: groups (buddy list order), a group's contacts and chats (sorted), and the buddies of contacts with more than one buddy. Visible means "in the parent's store"; the visibility rules are Pidgin 2's (`buddy_is_displayable`, empty groups, recent sign-on/off).
  - The sort methods are comparators: alphabetical, status (`purple_presence_compare`), log activity, and `NULL` for buddy list order ("Manually"). Changing the method re-sorts every group at once.
  - `update`/`remove` implement the placement half of the blist UI ops; the UI computes the display in the `item-refresh` signal.
- **`gtkblist.[ch]`**: the full `PurpleBlistUiOps`, replacing the stub. `save_node`/`remove_node`/`save_account` stay `NULL`, so libpurple's own savers still write `blist.xml`.
  - The window is a `GtkApplicationWindow` and the app's main window. It holds a `GtkPopoverMenuBar`, the alert area, a `GtkListView` over a `GtkTreeListModel` of the model's stores, and the status box.
  - Rows are `GtkTreeExpander` > `GtkBox` with the status icon, name and status/idle line, the short idle time, emblem, protocol icon and buddy icon (`GtkPicture`), bound to the item. Contacts with several buddies expand like groups.
  - `PidginBlistTheme` is gone. Its colours and fonts are the CSS classes `.pidgin-blist-online`, `-away`, `-idle`, `-offline`, `-group`, `-contact` and `-chat` in `resources/style.css`, for a future `gtk4.css`. Status, emblem and mood icons are in the GResource as `pidgin-status-*`, `pidgin-emblem-*` and `pidgin-mood-*`.
  - Group collapse is stored in the shared `collapsed` group setting, written only when it changes.
  - The menubar is a `GMenuModel` from `blist_menu[]` with `win.*` actions. Show and Sort submenus are stateful actions bound to prefs. The Accounts menu is dynamic: Enable Account, and per enabled account Edit Account, the prpl actions (via `pidginmenu`) and Disable. Plugin actions appear under Tools. Accelerators: Ctrl+M, I, L, B, Y, U, P, T, F1 (Ctrl+A and Ctrl+C are left to text entries), plus Ctrl+O and F2 on the list.
  - Context menus are `GtkPopoverMenu`s built per node from Pidgin 2's `create_*_menu`, including the prpl's `blist_node_menu` and the `blist-node-extended-menu` signal. They open on right click, long press, Menu and Shift+F10.
  - Tooltips are custom widgets from `::query-tooltip`. The `drawing-tooltip` (for cap), `drawing-buddy`, `gtkblist-created`, `-hiding` and `-unhiding` signals keep Pidgin 2's names and signatures.
  - Connection errors are mini-dialogs in the alert area, from `account-error-changed` as in Pidgin 2: generic errors with Reconnect or Re-enable, Modify Account and Dismiss (and SSL FAQs), and a single "Welcome back!" dialog for accounts signed on elsewhere. Errors are cleared only when dismissed, never at quit. `gtkconn.c` no longer shows alerts. `pidgin_blist_add_alert()` is the API for other code.
  - Drag and drop uses Pidgin 2's drop semantics for groups, contacts, buddies and chats (move, reorder, merge into a contact), and offers `application/x-im-contact` to other apps. Files dropped on a buddy: an image offers "Set as buddy icon" (the contact's custom icon) or "Send image file"; anything else is sent with `serv_send_file`.
  - Dialogs: Add Buddy, Add Chat, Join a Chat (account drop-down, the prpl's chat fields) and Add Group. `gtkdialogs.c` gains New IM, Get User Info, View User Log, alias, rename group, remove and merge groups.
  - Account requests (authorize, "added you", "add buddy?") are mini-dialogs in the buddy list again.
  - Closing the window quits, unless `/pidgin4/blist/close_hides` is set. A second launch raises the list. The accounts window opens at startup only when no account is enabled.
- **`gtkstatusbox.[ch]`**: a `GtkMenuButton` showing the current saved status.
  - Its popover lists Available, Away, Do Not Disturb, Invisible and Offline, the popular saved statuses, and "New status…" and "Saved statuses…" (both TODO(M5)). The status message is a plain `GtkTextView` (TODO(M4): `PidginComposeEntry`), applied 4 s after typing stops, on Enter or when the popover closes. The saved-status logic is Pidgin 2's.
  - A spinner shows while accounts connect, and "Waiting for network connection" while the network is down.
  - The buddy icon button next to it (`GtkFileDialog`) sets `/pidgin/accounts/buddyicon` and converts the image for every account that uses the global icon.
- **`pidginminidialog.[ch]`**: `PidginMiniDialog`, a `GtkBox` subclass with icon, title, description, extra contents and closing or non-closing buttons.
- **`gtkaccount.c`**: the editor has "Use this buddy icon for this account" (chooser, Remove, image drops on the window), saved as in Pidgin 2. `gtkutils.c` gains `pidgin_convert_buddy_icon()`.

**Prefs:**
- The shared keys have the same meaning in both UIs: `/pidgin/blist/{show_buddy_icons, show_empty_groups, show_idle_time, show_offline_buddies, show_protocol_icons, sort_type}`, `/pidgin/sound/mute` (the Mute Sounds toggle; sounds are M6) and `/pidgin/accounts/buddyicon`. pidgin4 registers them with Pidgin 2's types and defaults, so a profile Pidgin 2 has used gains nothing.
- New pidgin4 keys:
  - `/pidgin4/blist/width`, `/pidgin4/blist/height`: the window size (Wayland gives clients no position).
  - `/pidgin4/blist/close_hides` (default FALSE until the M6 tray).
  - `/pidgin4/blist/list_visible`.
  - `/pidgin4/blist/show_disconnected_accounts`: Buddies → Show → "Buddies of Disconnected Accounts", default FALSE. It also shows the buddies and chats of accounts that are not connected; Pidgin 2 never shows those, so with `-n` the list is otherwise empty.
  - `/pidgin4/filelocations/last_icon_folder`.

**Verification done:**
- **Build**: zero warnings; no deprecated GTK API even with `GDK_VERSION_MIN_REQUIRED=4.22` (checked by hand).
- **Unit tests**: `meson test` passes all 4, including the new `blistmodel` test, which drives the model through real libpurple blist UI ops. It covers the sort comparators, ordering and re-sorting, moves between groups, visibility flags, `show_offline`/recent sign-on/invisible nodes, expandable contacts (add, remove, merge) and notify-on-change.
- **Headless** (Xvfb, `GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`, `dbus-run-session`, `-c ~/.purple-gtk4 -n -d`) with `PIDGIN4_BLIST_SELFTEST=1`:
  - "everything shown: visible 72 groups, 319 contacts" (of 72/319; 674 buddies);
  - collapsed and expanded all 72 groups;
  - built 1065 context menus and tooltips;
  - toggled every Show option and every sort method; the model stays consistent;
  - restored the prefs and group states. `profile-compat.py` finds no `blist.xml` change afterwards.
- **Status selftest**: with every account disabled (scratch copy), `PIDGIN4_STATUS_SELFTEST=1` set Away with a message through the status box. `status.xml` gained that transient status.
- **Driven with xdotool** on Xvfb:
  - right-click menus, tooltips and the menubar menus;
  - dragging a contact onto another group moves it in `blist.xml`;
  - collapsing a group persists across a restart;
  - fake fatal and "name in use" `current_error`s in `accounts.xml` show as mini-dialogs and survive quitting.
- **Other runs**:
  - `PIDGIN4_ACCOUNT_SELFTEST=1` still passes; it now opens the accounts window itself.
  - SIGTERM saves and exits 0.
  - The same selftest under Wayland (Sway, `GDK_BACKEND=wayland`) is clean.
- **Profile round-trip**: `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4-m3/bin/pidgin4` **PASSES** with one extra allowance, `--allow 'added:^prefs\.xml:/pref/pref\[plugins\]/pref\[core\]/pref\[omemo\]'`. Without it the only failures are those three prefs. They come from the `omemo.so` that the M8 work installed into the shared libpurple prefix, which libpurple probes; M3 does not cause them.
- **No account was signed in.**

**TODOs by milestone:**
- **M4**
  - IM and join open no window yet (the conversation is created or joined);
  - unseen-message markers on rows (conversation signals);
  - the status message as `PidginComposeEntry`;
  - "Send IM" in the authorize mini-dialog.
- **M5**
  - windows that log `TODO(M5)`: preferences, privacy, pounces, file transfers, room list, system and user logs, plugins, custom smileys, certificates, the status editor and saved statuses;
  - blist themes and `gtk4.css`;
  - Set Mood;
  - screenname completion in requests (`GtkEntryCompletion` is deprecated).
- **M6**: the tray (then `close_hides` can default to TRUE) and Mute Sounds taking effect.
- **Dropped**: per-account status boxes in the account editor, the headline, `x-im-contact`/vCard drops from other apps (drags out still offer `x-im-contact`), and inline in-place editing of names (alias and rename are dialogs).

**Known gaps:**
- Row widths follow the longest name; very long status texts are ellipsized.
- `purple_log_get_activity_score` is cached by libpurple but reads the log index once per buddy, so the first "By recent log activity" sort on a big log directory is slow (as in Pidgin 2).
- The popover for a context menu is parented to the window's content box and positioned at the pointer. It closes when its node is removed.

### M4: Message view and compose entry (GtkIMHtml replacement) — daily-driver threshold
New components in `pidgin4/`:

1. **`PidginMarkup`** is a parser for the HTML subset libpurple emits. It handles:
   - tags: B, I, U, S, SUB, SUP, FONT color/back/face/size/sml, SPAN style, A, IMG id/src, BR, HR, P, PRE, CODE, comments;
   - Discord's spoiler span (`foreground: black; background: black`) becomes a click-to-reveal spoiler;
   - an **XEP-0393 Message Styling** mode for plain-text bodies: `*bold*`, `_italic_`, `~strike~`, `` `code` ``, ```` ``` ```` blocks and `>` quotes, following the XEP's parsing rules;
   - `IMG src=https://…` is loaded asynchronously through the remote-image loader. It's limited to allowlisted hosts (`cdn.discordapp.com`, `media.discordapp.net`, and the account's XEP-0363 upload host) and cached on disk. Other hosts show the alt text as a link;
   - output as a Pango attribute list + inline-object list.

   It is also used in reverse to serialize entry tags to the HTML that `get_markup` produced, including the `USE_POINTSIZE` and whole-buffer-formatting (WBFO) cases. It is unit-tested.
2. **`PidginMessageIndex`** is a SQLite store, `<profile>/pidgin4/messages.db`. It's written alongside libpurple's normal HTML logging (the logs themselves are unchanged) and holds, per message:
   - account, conversation, time and sender;
   - stanza/origin/server ids;
   - the log file and offset;
   - FTS5 text.

   It resolves correction, reply and reaction targets across restarts, deduplicates MAM, and serves find and log search. A one-off background indexer backfills it from the existing HTML logs. On every startup, an incremental pass picks up log files that Pidgin 2 wrote in the meantime (by mtime and size).

   When a native event (reaction, correction, retraction, reply) is shown, a text fallback line is also written to the HTML log (contract rule 7).
3. **`PidginMessage`** is a GObject holding sender, alias, flags, time, HTML body, and the parsed form. It also has **metadata**: stanza/origin id, correction-of id, receipt/marker state, reactions, and reply-to. It lives in a `GListStore`.
4. **`PidginMessageView`** is a `GtkListView` of message rows used for conversations, the log viewer, and history. Rows show:
   - a timestamp label; the existing `conversation-timestamp` signal and timestamp_format keep working;
   - the name with CSS classes send/receive/highlight/action/whisper and chat nick colors;
   - a selectable body label;
   - inline `GtkPicture`s for imgstore images and custom smileys. Animated smileys use a custom `GdkPaintable`.

   It also supports:
   - correction/receipt/reaction decorations;
   - find (filter + highlight);
   - scrollback trim (`/pidgin/conversations/scrollback_lines`);
   - a marker item, which replaces markerline's expose-based drawing;
   - prepending older messages (for MAM);
   - context menu actions: copy (plain/HTML), save image, and link actions.

   Links go through a small URI-scheme registry that replaces `gtk_imhtml_class_register_protocol` (`gtkutils.c:3576–3636`).
5. **`PidginComposeEntry`** is a GtkSourceView 5 `GtkTextView` with libspelling. It provides:
   - formatting tags for bold/italic/underline/strike/size/face/colors/links;
   - `setup_entry(features)` semantics;
   - Enter to send and `set_return_inserts_newline`;
   - a `message-send` signal (sendbutton/spellchk depend on it);
   - typing notifications from buffer changes;
   - a formatting toolbar replacing `gtkimhtmltoolbar.c` and a smiley picker popover;
   - a per-conversation formatting capability, so XMPP conversations only offer bold, italic, strike and code (see M8).

   It is used by conversations, the prefs font preview, pounces, saved statuses, the status message, multiline request fields, and plugin prefs.
6. **`PidginRichLabel`** is a read-only `GtkLabel`/`GtkTextView` built on PidginMarkup. It is used for notify, userinfo, and about dialogs.

Construction goes through a replacement for `pidgin_create_imhtml()` / `pidgin_setup_imhtml()` (`gtkutils.c:94–251`), so callers change minimally.

Then port `gtkconv.c`:
- The window uses `GtkNotebook` tabs. The menubar becomes a GMenuModel from `menu_items[]` (`gtkconv.c:3091`).
- The chat user list becomes a `GtkListView`.
- `pidgin_conv_write_conv` (`gtkconv.c:5803–6177`) builds `PidginMessage` objects instead of HTML strings. The `displaying-*`/`displayed-*` signals are kept.
- Typing and infopane.
- Drag-and-drop file send.

**Status (components): done (M4a).** Items 1–6 above, the message index with its backfill, the remote-image loader and the construction helpers are in `pidgin4/`, tested, and used by notify and request. `gtkconv.c` is not ported yet: that is M4b (see *Status (gtkconv)* below).

**Build and layout:**
- `meson.build` now needs gtksourceview-5, libspelling-1, libsoup-3.0 and sqlite3 (FTS5).
- The reusable code (`gtkutils`, `pidginmenu` and every M4 file) is a static library, `pidgin4-components`. It is linked whole into the executable, so `export_dynamic` still exports every `pidgin_*` symbol to plugins, and it is linked into the tests.
- `pidgin_utils_init()` starts the M4 parts: the link schemes, the smiley theme, the conversation signals and the message index.

**Public API by component:**
- **`pidginmarkup.[ch]`**
  - Parsing:
    - `pidgin_markup_parse_html(html, options)` returns a `PidginMarkupResult` (`text`, `attrs`, `objects`). `PidginMarkupOptions` holds the flags `NO_COLOURS`, `NO_FONTS`, `NO_SIZES`, `NO_FORMATTING`, `NO_INCOMING_FORMATTING`, `NO_SMILEYS`, `NO_LINKIFY`, `SHOW_COMMENTS`, `USE_POINTSIZE`, `WBFO`, `NO_IMAGES`, `KEEP_NEWLINES` and `STYLING` (parse the body as XEP-0393 if it is plain), plus `protocol_sml` and a custom smiley matcher.
    - `pidgin_markup_parse_styling(text, options)` parses XEP-0393.
    - Helpers: `pidgin_markup_is_plain()`, `_plain_from_html()`, `_html_to_plain()` and `_parse_color()`, and for sizes `_size_scale()`, `_size_to_points()` and `_points_to_size()`.
  - Results:
    - `pidgin_markup_result_ref()` and `_unref()`, `_has_graphics()` and `_has_object()`.
    - `_to_pango_markup(result, revealed_spoilers)` makes GtkLabel markup: `<a href>` for links, and hidden spoilers become `pidgin-spoiler:N` links.
    - Object types: `IMAGE` (imgstore id), `REMOTE_IMAGE`, `SMILEY` (id 1 marks a custom smiley), `HR`, `LINK`, `SPOILER`, `QUOTE` (id is the depth) and `CODE_BLOCK`. Every object's range holds a text fallback, so `text` is always usable as plain text.
  - Compose buffer:
    - `pidgin_markup_buffer_ensure_tags()`, `_get_size_tag()`, `_get_face_tag()`, `_get_fore_tag()`, `_get_back_tag()`, `_create_link_tag()` and `_remove_family()`.
    - `_insert_html(buffer, iter, html, caps, flags, insert_object_cb, data)` loads HTML into the buffer.
    - `_to_html(buffer, start, end, flags)` gives `gtk_imhtml_get_markup()`'s output, with `USE_POINTSIZE` and `WBFO`. `_to_styling(buffer, start, end)` gives XEP-0393.
    - Anchors carry their HTML as object data under `PIDGIN_MARKUP_HTML_KEY`.
  - Capabilities: `PidginFormatCaps`, `pidgin_format_caps_from_features(PurpleConnectionFlags)` and `pidgin_format_caps_for_account(account)`. XMPP gets `PIDGIN_FORMAT_STYLING_ALL`: bold, italic, strike, code and smileys, serialized as 0393.
  - Link schemes: `pidgin_markup_register_scheme(scheme, activate_cb, menu_cb, data)`, `_uri_is_known()`, `_activate_uri()` and `_populate_link_menu()`. The built-in schemes are http, https, ftp, mailto, xmpp, irc and ircs, all opened through `pidgin_open_uri()`.
- **`pidginsmileytheme.[ch]`**
  - Themes: `pidgin_smiley_themes_init()`, `_get_names()`, `pidgin_smiley_theme_get_current()` and `_set_current()`, `_load_file()`, `_get_smileys(sml)`, `_match()` and `_lookup()`. The current theme comes from the shared `/pidgin/smileys/theme` pref, read only.
  - Smileys: `pidgin_smiley_get_shortcut()`, `_get_file()`, `_is_hidden()` and `_get_paintable()` (the paintable is shared).
  - Custom smileys: `pidgin_custom_smiley_match()`, `_get_paintable()` and `_match_func()`.
- **`pidginanimation.[ch]`**: `pidgin_paintable_new_from_data()`, `_from_file()` and `_from_imgstore()` (a `PidginAnimation` or a `GdkTexture`), `pidgin_texture_new_from_pixbuf()` and `pidgin_animation_set_playing()`.
- **`pidginnickcolor.[ch]`**: `pidgin_nick_color_get(scheme, name, bg, &color)` with the schemes `PIDGIN` and `XEP0392`, plus `pidgin_nick_colors_generate()`, `pidgin_nick_color_xep0392_hue()`, `pidgin_hsluv_to_rgb()` and `pidgin_color_is_visible()`.
- **`pidginmessage.[ch]`** (`PidginMessage`; every property notifies):
  - Creation: `pidgin_message_new(sender, alias, html, flags, time)`, `_new_marker()`, and `_apply_meta(meta)`, which takes the `receiving-message-meta` table.
  - Body: `_set_parse_options()`, `_get_markup()` (parsed on first use) and `_get_plain_text()`.
  - Ids: getters and setters for the stanza, origin, server and occupant ids, and `_has_id()`.
  - Corrections and replies: `_get/_set_correction_of()`, `_apply_correction(new_html, new_id)`, `_get_edited()` and `_get_history()`; `_set_reply(id, sender, preview)` and its getters.
  - State: `_get/_set_receipt()` (it only moves forward) and `pidgin_receipt_state_from_string()`.
  - Reactions: `_add_reaction()`, `_remove_reaction()`, `_has_reaction()`, `_get_reactions()` (emoji → GList of senders), `_get_reaction_emojis()` and the "reactions-changed" signal.
  - Also `_get/_set_retracted()` and `_get/_set_index_id()`.
- **`pidginmessageview.[ch]`** (`PidginMessageView`)
  - Messages: `pidgin_message_view_new()`, `_append()`, `_prepend()`, `_prepend_many()`, `_clear()`, `_get_model()`, `_find_by_id()` and `_get_last_sent()`.
  - Scrolling: `_scroll_to_message()`, `_scroll_to_bottom()` and `_is_at_bottom()`.
  - Marker: `_set_marker()` and `_remove_marker()`.
  - Find: `_get_search_bar()`, `_set_search_mode()`, `_set_search_text()` and `_get_n_visible()`.
  - Settings: `_set_scrollback()` (the default is the shared `/pidgin/conversations/scrollback_lines`), `_set_conversation()`, `_set_is_chat()`, `_set_nick_color_scheme()` and `_set_self_id()`.
  - Signals: "reaction-toggled" (msg, emoji, add), "reply-requested", "edit-requested", "retract-requested", and "populate-menu" (msg, GMenu section), the context-menu hook for plugins.
  - Conversation UI signals, with Pidgin 2's signatures, on `pidgin_message_view_get_conv_handle()`: `conversation-timestamp`, `displaying-im-msg`, `displayed-im-msg`, `displaying-chat-msg` and `displayed-chat-msg`. Helpers: `pidgin_message_view_format_timestamp()`, `_emit_displaying()`, `_emit_displayed()` and `_signals_init()`/`_signals_uninit()`.
- **`pidgincomposeentry.[ch]`** (`PidginComposeEntry`, a GtkSourceView)
  - Setup: `pidgin_compose_entry_new()`, `_setup(features)` (which applies the default-formatting prefs), `_set_caps()`, `_get_caps()`, `_set_markup_flags()` (`USE_POINTSIZE`), `_set_smiley_category()`, `_set_return_inserts_newline()` and `_set/_get_spellcheck()`.
  - Content: `_get_markup()`, `_get_text()`, `_set_markup()`, `_clear()`, `_is_empty()`, `_send()`, `_insert_smiley()`, `_insert_image(imgstore_id)` and `_insert_link()`.
  - Formatting: `_toggle_bold()`, `_toggle_italic()`, `_toggle_underline()`, `_toggle_strike()` and `_toggle_code()`; `_grow_font()` and `_shrink_font()`; `_set_font_face()`, `_set_forecolor()` and `_set_backcolor()`; `_clear_formatting()`, `_get_format()` and `_get_font_size()`.
  - History: `_history_up()`, `_history_down()` and `_get_history()`.
  - Signals: "message-send" (markup) → gboolean handled, "typing-changed" (PurpleTypingState), "edit-last-requested" and "format-changed".
- **`pidginformattoolbar.[ch]`**: `pidgin_format_toolbar_new(entry)`, `_set_entry()`, `_get_entry()`, `_update()` and `_get_smiley_grid()`.
- **`pidginrichlabel.[ch]`**: `pidgin_rich_label_new()`, `_set_html()`, `_set_result()`, `_set_text()`, `_get_text()`, `_set_highlight()`, `_set_force_text_view()`, `_set_extra_menu()`, `_set_max_image_size()`, `_is_text_view()`, `_get_inner()`, `_get_image(x, y, &id)` and `_get_link_at(x, y)`.
- **`pidginmessageindex.[ch]`** (`<profile>/pidgin4/messages.db`)
  - Setup: `pidgin_message_index_open()`, `_get_default()` and `_ui_init()`/`_ui_uninit()`. `_ui_init()` registers the prefs, answers `jabber-kv-load`/`jabber-kv-store`, and starts the backfill on the first sign-on.
  - Keys: `_account_key(account)` and `_conv_key(account, name)`, which match the log directory layout.
  - Messages: `_insert()`, `_set_ids()`, `_get()`, `_delete()`, `_find_by_id(account, conv, id)`, `_find_fuzzy(account, conv, time, sender, plain)`, `_search(query, account, conv, limit)`, `_get_recent()` and `_count()`.
  - Reactions and receipts: `_add_reaction()`, `_remove_reaction()`, `_get_reactions()`, `_set_receipt()` and `_get_receipt()`.
  - Key/value store: `_kv_get()` and `_kv_set()`.
  - Log positions: `_mark_log_position()`, plus the file-cursor calls the backfill uses.
  - `PidginIndexedMessage` is the boxed row type.
- **`pidginbackfill.[ch]`**
  - `pidgin_backfill_new(idx, logs_dir)`, `_get_default()`, `_start()`, `_pause()`, `_resume()`, `_cancel()`, `_is_running()` and `_run_sync()`.
  - Signals: "progress" (files_done, files_total, bytes_done, bytes_total) and "finished" (completed).
  - Line parsers: `_parse_html_line()`, `_parse_txt_line()` and `_parse_file_name()`.
- **`pidginimageloader.[ch]`**: `pidgin_image_loader_get_default()`, `_new(cache_dir)`, `_allow_host()`, `_is_allowed()`, `_load_async()` and `_load_finish()` (which gives a `GdkTexture`, or the errors `NOT_ALLOWED`, `TOO_LARGE`, `HTTP`, `DECODE` or `DECRYPT`), `_lookup_cached()`, `_set_max_image_size()`, `_set_cache_limit()` and `_trim_cache()`.
- **`gtkutils`**: `pidgin_create_message_view()` and `pidgin_create_compose_entry(features, with_toolbar, &entry, &toolbar)`.

**What M4b has to do to port `gtkconv.c`:**
- **Signal handle.** `pidgin_conversations_get_handle()` must return `pidgin_message_view_get_conv_handle()`. The conversation-timestamp and displaying/displayed signals are registered there already, so M4b registers its other conversation signals on the same handle.
- **Replacing the IMHtml widgets.** Use `pidgin_create_message_view()` and `pidgin_create_compose_entry(features, TRUE, &entry, &toolbar)`, then:
  - call `pidgin_message_view_set_conversation()` on the view;
  - on the entry, call `set_markup_flags(USE_POINTSIZE)` for `OPT_PROTO_USE_POINTSIZE` prpls, and `set_caps(pidgin_format_caps_for_account())` whenever the connection flags change;
  - on the view, call `set_self_id()`, and `set_nick_color_scheme(XEP0392)` for XMPP.
- **`pidgin_conv_write_conv()`** (`gtkconv.c:5803`):
  - Linkify unless `NO_LINKIFY`, then call `pidgin_message_view_emit_displaying()`.
  - Build a `PidginMessage` with parse options:
    - `NO_INCOMING_FORMATTING` when `show_incoming_formatting` is off;
    - `STYLING` for XMPP;
    - `protocol_sml` set to the prpl name;
    - a custom smiley matcher for the conversation's custom smileys, which libpurple feeds through the `custom_smiley_*` conversation ops.
  - Call `pidgin_message_apply_meta()` with the pending `receiving-message-meta` table, then `pidgin_message_view_append()`, then `_emit_displayed()`.
  - Index the message: `pidgin_message_index_insert()` and `mark_log_position()`.
- **Message-event signals.** Handle `message-corrected`, `-reaction`, `-receipt` and `-retracted` with `find_by_id` on the view, falling back to the index, and the matching `PidginMessage` setters. Write the text fallback line to the log (contract rule 7).
- **The entry.** Connect "message-send" to the send path and return TRUE. Connect "typing-changed" to `purple_conv_im_set_typing_state()`/`serv_send_typing()`. Connect "edit-last-requested" to `pidgin_message_view_get_last_sent()` + `set_markup()`, with the next send going out as a correction.
- **View actions.** Wire "reaction-toggled", "reply-requested" and "retract-requested" to the M8 IPC (`send-reaction` and friends). Add `"message-meta" = "1"` to the UI info only once those handlers exist.
- **Menus and plugins.** Bind Find (Ctrl+F) to `pidgin_message_view_set_search_mode()`. Markerline uses `set_marker()` when the window loses focus. History and MAM scroll-back use `prepend_many()`. The timestamp_format context menu uses "populate-menu".

**Verification:**
- **Build and unit tests.** `scripts/build-pidgin4.sh --test` passes all 9 tests: pidginmenu, singleui, desktop-file, pidginmarkup, nickcolor, imageloader, messageindex, backfill and msgview-selftest. The build has 0 warnings.
  - msgview-selftest runs only when `PIDGIN4_TEST_DISPLAY` names an Xvfb display; otherwise it is skipped.
- **The selftest.** `msgview-demo` with `PIDGIN4_MSGVIEW_SELFTEST=1` appends 5000 messages in about 2.7 s, using about 30 MB more RSS. It also checks find (1250 of 5000 shown), prepend while scrolled (the view stays put), scrollback trim, the marker, compose to HTML and to 0393, and the send history. It passes on Xvfb and under Sway (Wayland) with `G_DEBUG=fatal-criticals` and `GTK_A11Y=none`.
- **The demo.** Showing the samples plus a real dev-profile log produced no criticals.
- **The backfill scale run.** Over about 130 MB of dev-profile logs it processed 12k files and 1.04M rows in 12 s unthrottled (55 s at the default throttle), with a peak RSS of 23 MB. The database came to 253 MB, and an incremental rerun took 0.15 s.
- **Profile round-trip.** `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4-m4a/bin/pidgin4` passes. The allowed changes are the new `/pidgin4/index` prefs and `pidgin4/messages.db`.
  - It passes only with the three `/plugins/core/omemo` prefs allowed. Those come from the `omemo.so` that the concurrent M8 OMEMO work installed into the shared libpurple prefix, not from M4.
- **Sign-in.** No account was signed in; the backfill therefore never ran in pidgin4 itself.

**Gaps and notes:**
- **Deprecated gdk-pixbuf.** `GdkPixbufAnimation` has been deprecated since gdk-pixbuf 2.44 and has no GTK 4 replacement without GStreamer. `pidginanimation.c` confines its use and silences the warnings.
- **Link menus.** In label mode, links get GtkLabel's own link menu items; the scheme registry's context-menu callbacks only apply in the text view.
- **Formatting limits:**
  - An HR in label mode is a line of box-drawing characters.
  - XEP-0393 quotes are indented only in text-view mode.
  - `<sub>`/`<sup>` in the text view approximate the baseline shift with rise.
- **XEP-0393 output.** Overlapping bold/italic ranges are split to nest, and formatting in the middle of a word is written as-is (receivers won't style it).
- **Smiley matching** is longest-prefix at any position, as in GtkIMHtml, so a smiley can match inside a word.
- **The compose entry:**
  - Undo does not cover tag-only changes (GtkTextBuffer history).
  - The Enter handling runs before the input method; composing with an IME that uses Enter is untested.
- **The index and backfill:**
  - Only numeric timestamp formats are recognised.
  - Text logs don't record direction.
  - A file that changes but keeps its size is not reindexed.
  - The database is roughly 2× the size of the logs, so the full 1.5 GB of non-system logs will give about 3 GB. Consider an opt-in to leave out old years.
- **The image loader:**
  - It uses the system proxy (GProxyResolver), not libpurple's per-account proxy.
  - The successful aesgcm decrypt path is untested until the OMEMO plugin provides the IPC below.
- **Not covered by tests:** the context-menu actions (save image, emoji chooser) and the toolbar dialogs (font, colour, file) were built but only exercised in the demo, not by the selftest.

**Status (gtkconv): done (M4b), except the checks with accounts signed in, which are left to the user** (the M4 section of `pidgin4/TESTING.md`). With it M4 is done and pidgin4 can be used for chatting. Left for later milestones: the log viewer and pounce editor behind View Log / Add Buddy Pounce (M5), sounds and notifications (M6, which gets the `make_sound` field and the conversation signals), and the ported plugins (M7).

**What's in `pidgin4/`:**
- **`gtkconv.[ch]`**: the conversation pane and the full `PurpleConversationUiOps`. `write_chat`/`write_im` pass through to `purple_conversation_write()`, as in Pidgin 2.
  - A pane has:
    - the infopane: buddy icon (an animated `GtkPicture`, if `/pidgin/conversations/im/show_buddy_icons`), name, status or topic, protocol icon, and a spinner while history loads;
    - the chat topic entry;
    - the `PidginMessageView`;
    - for chats, a `GtkListView` of the users;
    - the typing line;
    - a "Replying to"/"Editing" banner;
    - the compose area from `pidgin_create_compose_entry()`, plus a Send button (`/pidgin4/conversations/send_button`; Pidgin 2 has no such pref, only the sendbutton plugin).
  - `write_conv`:
    - linkifies, and emits `displaying-*-msg`, which may cancel or rewrite;
    - builds a `PidginMessage` with parse options: the incoming-formatting pref, XEP-0393 for XMPP unless the meta says `unstyled`, point sizes, the protocol's smiley category, and our custom smileys on sent messages;
    - applies the pending metadata;
    - appends it, or queues it for a `mam-query=older` page;
    - indexes it, updates the unseen state and emits `displayed-*-msg`.

    Lines without SEND/RECV show without a name.
  - The entry:
    - `/` commands through `purple_cmd_do_command()` with Pidgin 2's status messages, and the built-in `say`, `me`, `debug`, `clear`, `clearall` and `help`;
    - `NO_NEWLINES` splits a message into lines;
    - typing notifications through `serv_send_typing()`, with the type-again interval;
    - Up in an empty entry edits the last message;
    - Tab completes nicks in chats (`chat-nick-autocomplete` first);
    - Escape cancels the banner.
  - Chat users are `PidginChatUser` objects in a `GListStore`, sorted as in Pidgin 2 (rank, then buddies, then alias).
    - Rows have flag icons, nick colours from the same scheme as the view, bold buddies and yourself, and struck-out ignored users.
    - Double click (or `chat-nick-clicked`) opens an IM.
    - The context menu has IM, Send File, Ignore, Info, Get Away Message and Add/Remove; then Op/Deop/Voice/Devoice/Kick/Ban, for whichever of those commands the prpl registered for the conversation; then the buddy's own menu (`pidgin_blist_build_node_menu()`).
  - Unseen state:
    - `PIDGIN_UNSEEN_*` on the struct, and as the `unseen-count`/`unseen-state` conversation data (as in Pidgin 2);
    - `pidgin_conv_set_unseen()`, `pidgin_conversations_find_unseen_list()`, `pidgin_conversations_get_unseen_count()`, and `pidgin_conversations_fill_menu()` (a `GMenu` with `app.pidgin4-present-conv`, for the M6 tray).

    Seeing a tab in the active window clears it and sends read markers.
  - `hide_new` (`always`/`away`) puts new IMs into a hidden window. Presenting a conversation, or a change of `hide_new`, attaches it.
  - Drops:
    - files are sent (`serv_send_file`; `serv_chat_send_file` in chats);
    - an image dropped on an IM asks "Send Image File" or "Insert in Message" (`GtkAlertDialog`);
    - a buddy dragged from the list opens an IM, or is invited into a chat.
  - Menu actions:
    - Find, Save As (HTML), Clear, Send File, Get Attention, Get Info;
    - Invite (`purple_conv_chat_invite_user`, with its own dialog);
    - Alias, Block (confirmed) and Unblock, Add and Remove;
    - Insert Link (request fields) and Insert Image (`GtkFileDialog`);
    - Close, and More (the `conversation-extended-menu` actions via `pidginmenu`);
    - Enable Logging (the `enable-logging` node setting is written only when it differs from the pref, as in Pidgin 2) and Enable Sounds (`gtk-mute-sound`);
    - the toolbar and timestamp prefs.
  - Signals on `pidgin_conversations_get_handle()` (which is the message view's handle): `conversation-switched`, `conversation-hiding`, `conversation-displayed`, `chat-nick-autocomplete` and `chat-nick-clicked`, with Pidgin 2's signatures. They sit beside the view's `conversation-timestamp` and `displaying-`/`displayed-im-msg`/`-chat-msg`. `conversation-dragging` is dropped.
  - Accessors for the M7 plugins: `pidgin_conv_get_window()`, `_get_message_view()`, `_get_entry()`, `_get_toolbar()`, `_get_tab_container()` and `_get_conversation()`; `pidgin_conv_get_tab_icon()` (a `GIcon` now), `pidgin_conv_is_hidden()`, `pidgin_conv_present_conversation()`, `pidgin_conv_attach_to_conversation()` and `pidgin_conversations_get_conv_ui_ops()`. `PidginConversation` keeps Pidgin 2's member names where they still mean something: `imhtml` is the message view, `entry` the compose entry, and `toolbar`, `tab_cont` and `u.chat->list` are what they were.
  - Remote custom smileys (XHTML-IM BoB) are not supported. `custom_smiley_add` returns FALSE, so the prpl doesn't fetch them and the shortcut stays text. `send_confirm` puts the message back in the entry, as in Pidgin 2.
- **`gtkconvwin.[ch]`**: `PidginWindow`, a `GtkApplicationWindow` with a `GtkPopoverMenuBar` and a `GtkNotebook`.
  - The menubar is a `GMenuModel` of Pidgin 2's `menu_items[]` over a `conv` action group. Items that don't apply are disabled, or hidden (Invite, Block/Unblock, Add/Remove).
  - Accelerators, set with `gtk_application_set_accels_for_action()`:
    - Ctrl+M new IM, Ctrl+F find, Ctrl+L clear, Ctrl+O info, Ctrl+W close;
    - Ctrl+Tab / Ctrl+Shift+Tab: the next/previous tab with unread text, else the next/previous tab;
    - Ctrl+PgDn/PgUp and Ctrl+] / Ctrl+[: the next/previous tab;
    - Alt+1..9: tab N;
    - Ctrl+, / Ctrl+.: move the tab.
  - Tabs are reorderable and detachable. The notebooks share a group, so tabs move between windows by drag, and "create-window" makes a new window.
    - The window's `gtkconvs` list follows the notebook's `page-added`, `-removed` and `-reordered`; an emptied window goes.
    - Tab labels have a status/typing icon, the title, and a close button (`close_on_tabs`). The title's colours for the unseen states and typing are Pidgin 2's defaults, as CSS classes.
    - Middle click closes a tab. The context menu has Close other tabs, Detach and Close.
    - There is no tab bar with one conversation. `tab_side` sets the tab position; the rotated variants map to left/right.
  - Placement: Pidgin 2's API, and the functions `last`, `im_chat`, `new`, `group` and `account`, from `/pidgin/conversations/placement`. With `/pidgin/conversations/tabs` off, each conversation gets its own window.
  - The window size is kept in `/pidgin4/conversations/width|height` (Wayland gives no position).
  - Presenting uses `gtk_window_present()`, which passes an xdg-activation token when GTK has one; otherwise Sway marks the window urgent.
- **`pidginconvmeta.[ch]`** (in the components library): the M8 glue. pidgin4's ui_info now has `message-meta` = `1`.
  - Pending metadata:
    - `receiving-message-meta` and `sending-message-meta` tables are kept per conversation (account + normalized name), and taken by the next `write_conv` with SEND/RECV. The sending table goes only to SEND.
    - A table that isn't taken is dropped on the next idle.
    - A sending table with `correction-of` is not kept, since no write follows it.
  - Dedup against `messages.db`:
    - a server-id hit sets `discard` = `1`;
    - a stanza-id or origin-id hit does so only for MAM results, or for prpls whose ids come from the server (IRC msgid; not XMPP);
    - a MAM result without an id hit is matched fuzzily (±2 min; the nick in rooms, any sender in IMs; the text) in `writing-im-msg`/`writing-chat-msg`, which cancels the write before libpurple logs it;
    - a scroll-back page still shows its duplicates, from the index row.
  - Indexing: every logged message, and any message with ids, is inserted with:
    - its account and conversation keys, time, the logged alias as sender, and the plain text;
    - its ids, and its reply and correction targets;
    - its log file and the offset of its line.

    libpurple's common log writer doesn't record the file name, so it is read from `/proc/self/fd/<fileno>`. The offset is the `ftell()` taken in `writing-*-msg` before the write, or the last line's start for a log that was just opened.
  - Events:
    - `message-corrected`: the sender is checked (bare JID in IMs, nick in rooms);
    - `message-reaction`: identities are bare JIDs in IMs and nicks in rooms; an add we already have is a no-op;
    - `message-receipt`: `displayed` covers earlier sent messages, and one from our own bare JID clears the unseen state (XEP-0490);
    - `message-retracted`: in a room, a different sender means moderation.

    Each handler finds the target in the view, and updates it and the index. If the target isn't shown, it returns FALSE, so the prpl writes its fallback; reactions and receipts still update the index.
  - Contract rule 7: corrections, reactions and retractions write straight into the conversation's `PurpleLog`, as system lines that are never displayed and that `writing-*` plugins don't see:
    - `X edited: …`;
    - `X reacted 👍 to: <40 chars>` or `X removed the reaction 👍 from: …`;
    - `X retracted a message`, or `mod removed a message[: reason]`.

    A correction also gets an index row (with `correction_of`), so the new text is searchable.
  - Actions go over the prpl's IPC, and are offered only when the prpl has the command and is connected (`pidgin_message_view_set_message_actions()`):
    - reactions: `send-reaction`, with our complete new set;
    - reply: `send-reply`, with the target's plain text as the quote and, in rooms, `room/nick` as the JID;
    - edit: `send-correction`, with the entry's text as plain 0393;
    - delete: `send-retraction`; or `send-moderation` for others' messages, in rooms where we are op, founder or halfop.

    Nothing is jabber-specific beyond the command names, so Discord (M9) works by registering the same commands.
  - Read state: a conversation is seen when its tab is current in the active window, or when a message arrives while it is. If `/pidgin4/conversations/send_markers` is on (the default), for the newest incoming message it then sends:
    - `send-marker` `displayed`, if the message was `markable` (its stanza-id in IMs, its server-id in rooms);
    - `mds-publish` with its server-id.

    Each goes out once per message.
  - Scroll-back (the view's new `top-reached`) takes:
    - first the index (`get_recent` before the oldest shown message): any prpl, offline too, as plain text;
    - then `mam-fetch-older`, with the oldest shown server-id. That page's messages (`mam-query=older`) are queued and prepended on `mam-query-done` (30 s timeout).
  - Inline images: a body that is a lone http(s) or aesgcm image URL is shown with an `IMG` when the image loader allows the host.
    - The jabber prpl doesn't expose its XEP-0363 service, so the host is **learnt from our own uploads**: the SEND echo whose body is the GET URL.
    - Beyond that, **hosts on the account's own XMPP domain** (e.g. `upload.example.net`) are allowed: our server sees our address anyway.
    - Contacts' upload hosts elsewhere stay links (the plan's allowlist).
- **`pidginmessageview`** gained `top-reached`, `pidgin_message_view_set_message_actions()` and `pidgin_message_view_refresh()`.
- **Buddy list**: rows whose conversation has unread text are bold, with the count. The authorize mini-dialog has "Send Instant Message" again. IM, Join and double click already presented the conversation; that now opens a window.

**Prefs:**
- `pidgin_prefs_init()` registers them through `pidgin_conversations_prefs_init()`. That includes every `/pidgin/conversations/*` key that Pidgin 2's `gtkconv.c` registers, with its type and default, so a profile Pidgin 2 has used gains nothing.
- Read:
  - `close_on_tabs`, `show_incoming_formatting`, `show_timestamps`, `show_formatting_toolbar`, `spellcheck`;
  - `placement`, `tabs`, `tab_side`, `scrollback_lines`;
  - `chat/entry_height`, `chat/userlist_width`, `im/entry_height`, `im/show_buddy_icons`, `im/hide_new`;
  - and, in the entry, the font and colour ones.
- Not used: the GTK 2 window positions and sizes, `use_smooth_scrolling`, `minimum_entry_lines`, `resize_custom_smileys`, `custom_smileys_size`, `placement_number`, `im/animate_buddy_icons` (animations always play) and `im/close_immediately` (always immediate).
- New: `/pidgin4/conversations/{width,height}` (640×480), `send_button` (FALSE) and `send_markers` (TRUE).

**Verification done:**
- **Build**: zero warnings.
- **Unit tests**: `meson test` passes all 11. That includes the new `convmeta`, which covers:
  - discard by server-id;
  - client ids with and without MAM, and for server-assigned ids;
  - the fuzzy match's time window, sender and text;
  - the fallback texts and the helpers.

  It also includes `msgview-selftest` on Xvfb (`PIDGIN4_TEST_DISPLAY`).
- **`PIDGIN4_CONV_SELFTEST=1`** PASSES its 114 checks on a scratch copy of the dev profile, in two setups:
  - under Xvfb (`GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`, `dbus-run-session`, `-n`);
  - under a headless nested Sway (`GDK_BACKEND=wayland`, `GSK_RENDERER=cairo`).

  pidgin4 logged no criticals or GTK warnings, apart from libpurple's usual presence assertions for accounts whose prpl is missing.
- **Real keys** (xdotool on Xvfb, during `PIDGIN4_CONV_SELFTEST_HOLD`): Alt+1, Ctrl+PgDn and Ctrl+Tab switch tabs and Ctrl+W closes one, even with the focus in the compose entry.
- **Screenshots** of the IM and chat tabs looked right: names, colours, receipts, deleted and edited rows, the user list's order and icons, and the topic.
- **Profile round-trip**: `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4-m4b/bin/pidgin4` **PASSES** (0 unexpected changes; 6 or 7 allowed ones, depending on the run), with no extra allowance.
- **No account was signed in.** Nothing was tested against a real server or another client.

**Gaps and notes:**
- There is one `PurpleConversation` per tab: Pidgin 2's "Send To" (several buddies of a contact in one tab) is gone.
- Remote custom smileys aren't shown (see above).
- The buddy icon's context menu (save, set a custom icon) and inline alias editing in the infopane are dropped; Alias is in the menu.
- Tab completion completes nicks only (no `/command` completion), and shows the candidates as a NO_LOG line.
- The log viewer, the pounce editor and the file transfer window are M5; View Log and Add Buddy Pounce are disabled until then.
- libpurple logs a MAM scroll-back page into the current log file (the M8 design); the index keeps the real times.
- Index scroll-back shows plain text (the index keeps no markup), and only rows older than the oldest shown message.
- The fallback log lines are system lines (`(time) <b>text</b>`); the backfill indexes them like any other line.
- The status box's message is still a plain text view (an M3 TODO for M4, not done here).

### M5: Remaining windows
Prefs, pounces, saved statuses, log viewer, privacy, room list, certificate manager, file transfers, smiley manager, plugins dialog, about/build info, join chat.
- Lists become `GtkListView`/`GtkColumnView`.
- Dialogs become async `GtkAlertDialog`/`GtkFileDialog`.
- Whiteboard and media UIs are dropped.

**Status: done, except the checks that need accounts signed in, which are left to the user** (the M5 section of `pidgin4/TESTING.md`).

**What's in `pidgin4/`** (all lists are `GtkListView`/`GtkColumnView` over `GListModel`s, choices are `GtkDropDown`s, confirmations `GtkAlertDialog`, files `GtkFileDialog`/`GtkFileLauncher`; nothing deprecated):
- **`gtkprefs.c`**: the preferences window, a `GtkStackSidebar` with the pages Interface, Conversations, Smiley Themes, Themes, Sounds, Network (with the global proxy), Browser, Logging, Status / Idle and Message Index. Widgets are bound to prefs both ways (no Apply). The font override uses `GtkFontDialogButton`, colours `GtkColorDialogButton`; the default-formatting preview is a `PidginRichLabel`, the conversation-font sample a `PidginComposeEntry`. The proxy type offers "Use System Proxy Settings" (libpurple's `envvar`, which is GProxyResolver since M1). The Message Index page shows the database size and row count, and starts, pauses, resumes and rebuilds (forget every log file, then backfill) the index. Sound Preview goes through `purple_sound_play_event()`, i.e. M6's sound UI ops, with the event/mute/status temporarily overridden as Pidgin 2's test did (not a new `pidgin_sound_play_file`: no such function is needed).
- **`pidginprefbinding.[ch]`** (components library): `pidgin_pref_bind_bool/_int/_string/_path/_dropdown_string/_dropdown_int/_sensitive/_sensitive_string/_insensitive_string`, and the constructors `pidgin_pref_checkbox_new`, `_spin_new`, `_entry_new`, `_dropdown_string_new`, `_dropdown_int_new`. Bindings die with their widget.
- **`gtkthemes.[ch]`**: `<profile>/pidgin4/gtk4.css` at `GTK_STYLE_PROVIDER_PRIORITY_USER`, watched with a `GFileMonitor` on its directory (reloads after a save, also by rename; parse errors are logged and shown on the Themes page, whose Open button creates the file with examples). It also turns the shared `use_theme_font`/`custom_font` prefs into CSS for `.pidgin-compose-entry` and `.pidgin-conversation-font`.
- **`gtkpounce.c`**: manager (account, target, events, Recurring toggle) and editor (Pidgin 2's fields; message in a `PidginComposeEntry`); saving is Pidgin 2's `save_pounce_cb`, so `pounces.xml` is unchanged in format (byte-identical after edit+save without changes). The handler runs all five actions; popups use `purple_notify_info`. The registration moved here from `stubs.c`, which is gone.
- **`gtksavedstatuses.c`**: the saved statuses window (Use, Add, Modify, Duplicate, Delete) and the editor with per-account substatuses; the status box's "New status…"/"Saved statuses…" open them. The status box's message is now a `PidginComposeEntry`.
- **`gtklog.c`**: log viewer for buddies, contacts (all their buddies' logs), chats and the system log: a month tree (`GtkTreeListModel`), the log in a `PidginMessageView` (one row per line via the backfill line parsers), total size in the title, Delete (confirmed) and Browse folder. Search asks `PidginMessageIndex` for files it has indexed at their current size and scans the rest linearly in idle slices. Read-only otherwise.
- **`gtkprivacy.c`**, **`gtkroomlist.c`**: Pidgin 2's behaviour and UI ops. The room list is a `GtkColumnView` over a `GtkTreeListModel` (categories expand lazily); XMPP rooms can be bookmarked through the jabber `bookmark-add` IPC.
- **`gtkcertmgr.c`**: the `tls_peers` pool: view (subject, issuer, validity, SHA-1 and SHA-256), import, export, delete.
- **`gtkft.c`**: the `PurpleXferUiOps` and the File Transfers window (progress bar per row, size, speed, remaining, status; Open File/Open Folder, Stop, Remove, Clear Finished). HTTP-upload transfers show as "Sending as <account>".
- **`gtksmiley.c`**: custom smiley manager and editor.
- **`gtkplugin.c`**, **`gtkpluginpref.c`** (components): the plugins dialog and the pref-frame converter; see below for the list and the GTK 2 guard.
- **`pidginabout.c`**: About, Build Information (copyable) and Credits pages.
- **`pidginomemo.c`**: OMEMO fingerprints window (own fingerprint, devices of our account, of every buddy and of a typed JID, with a trust drop-down), greyed out with a "Plugins…" button when `core-omemo` isn't loaded. Tools → OMEMO Fingerprints.
- **`pidgincompletion.c`** (components): buddy-name completion popover; `gtkrequest.c` attaches it to "screenname" fields, which covers New IM, Get User Info and View User Log.
- **`pidginselftest.[ch]`** and **`scripts/run-pidgin4-selftest.sh`**: `PIDGIN4_WINDOWS_SELFTEST=1|module,...`, `PIDGIN4_SELFTEST_SHOTS=DIR`.
- The buddy list's Tools menu, the node menus' pounce and View Log items, the join-chat dialog's Room List button, the View User Log dialog and `gtkmain.c` (UI ops for xfers, privacy and room list; init/uninit) are wired to these windows. The join-chat dialog itself was already done in M3.

**Helper APIs for other milestones:**
- M4b: `pidgin_omemo_show_fingerprints(account, jid)` (conversation lock/toggle); `pidgin_smiley_add_from_image(parent, data, len, shortcut)` ("save as custom smiley"); `pidgin_log_show(type, name, account)` / `pidgin_log_show_contact()`; `pidgin_pounce_editor_show()`; `pidgin_xfer_dialog_show()`; `pidgin_roomlist_dialog_show_with_account()`; `pidgin_buddy_completion_attach(entry, account_dropdown, all_accounts)`. Please add the CSS class `pidgin-conversation-font` to the conversation's message view so the font override applies to it, and read `/pidgin4/conversations/placement` and `/pidgin4/conversations/show_send_button` (defined here; change the placement ids in `gtkprefs.c` if `pidgin_conv_placement_get_options()` uses others).
- M6: the Sounds page writes `/pidgin4/sound/method` (`automatic` = GSound, `custom`, `none`) and `/pidgin4/sound/command` (`%s` = file); the shared `/pidgin/sound/enabled/*`, `/pidgin/sound/file/*`, `conv_focus`, `mute`, `volume` and `/purple/sound/while_status` keep Pidgin 2's meaning. Idle uses the shared `/purple/away/idle_reporting` (`system` = M6's Wayland/GNOME idle).
- M7: `pidgin_plugin_pref_frame_to_widget(frame)` (alias `pidgin_plugin_pref_create_frame`), `PidginPluginUiInfo`/`PIDGIN_IS_PIDGIN_PLUGIN` in `gtkplugin.h` (Pidgin 2's layout), `pidgin_plugin_get_config_frame()`, the `pidgin_pref_bind_*` helpers.
- Everyone: `pidgin_application_set_exit_status()`, `pidgin_selftest_*`.

**Plugin list and GTK 2 plugins:** `pidgin_plugins_load_saved()` replaces `purple_plugins_load_saved()` and `pidgin_plugins_save()` writes `/pidgin4/plugins/loaded`; `/pidgin/plugins/loaded` is never read or written (the selftest checks it). Before probing or loading any file, `pidgin_plugin_file_is_foreign_toolkit()` reads its ELF `DT_NEEDED` entries (a bounds-checked parser, ELF32/64, no `dlopen`) and refuses `libgtk-x11-2.0`, `libgdk-x11-2.0`, `libgtk-3`, `libgdk-3` and similar; refused entries are dropped from the list. Tested: `/usr/lib64/pidgin/history.so` in `/pidgin4/plugins/loaded` is logged as refused, not loaded, and pidgin4 exits 0. Remaining risk: libpurple's own `purple_plugins_probe()` at startup dlopens every `.so` in the search paths before pidgin4 can check, so a GTK 2 plugin copied into `<profile>/pidgin4/plugins` by hand would still be mapped (the dialog then shows it greyed out). The system directories are not search paths (contract rule 3).

**New `/pidgin4` prefs:** `sound/method`, `sound/command`, `browser/method`, `browser/command`, `conversations/placement`, `conversations/show_send_button`, `filetransfer/clear_finished`, `filetransfer/keep_open`, `pounces/width|height`, `status/width|height`. Shared keys newly registered (Pidgin 2's types and defaults, same meaning): the `/pidgin/conversations/*` keys the pages bind, `/pidgin/sound/{enabled,file}/*`, `conv_focus`, `volume`, `/pidgin/smileys/theme`, `/pidgin/pounces/default_actions/*`.

**Verification:**
- Build: 0 warnings (`-Wall -Wextra`); the M5 files also build warning-free with `GDK_VERSION_MIN_REQUIRED=4_22`/`GLIB_VERSION_2_88`.
- `meson test`: 12 OK, 1 skipped (msgview-selftest without `PIDGIN4_TEST_DISPLAY`). New: `prefbinding`, `pluginpref` (every pref type both ways, destruction, the ELF check on history.so/psychic.so/garbage), `completion`. Widget tests start their own Xvfb (`tests/test-display.c`) and skip without it.
- `PIDGIN4_WINDOWS_SELFTEST=1` passes with `G_DEBUG=fatal-criticals` on Xvfb, under Sway (Wayland), and once on `~/.purple-gtk4` itself: 10 prefs pages with 90 bound prefs unchanged by opening, pounce and status editors, the system log (48635 logs) and a buddy's logs with a search, 63 certificates viewed, psychic toggled on and off with `/pidgin/plugins/loaded` unchanged, the history.so refusal, About, OMEMO. No file under `logs/` changed.
- `gtk4.css`: a parse error is reported, and an edit is picked up live (about 200 ms).
- Profile round-trip: `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4-m5/bin/pidgin4` **PASSES** (no extra allowances).
- No account was signed in.

**Gaps:**
- Not run with accounts online: privacy lists, room lists, transfers, pounces firing, OMEMO trust changes, the message-index progress (TESTING.md).
- Not ported: Set Mood (the buddy list still has a `TODO(M5)`), account reordering by drag and drop in the accounts window, Pidgin 2's mail dialog grouping, the pounce list notification (popups are plain notify dialogs), name completion in the pounce editor, buddy/sound/blist themes (replaced by `gtk4.css`), the MSN-only xfer thumbnail op.
- Log search: the index matches whole words (all of them), the linear scan substrings.
- Async dialogs (confirmations, file choosers, launchers) can't be answered headless, so import/export/delete paths were tested by hand on scratch copies only.
- OMEMO: asking for fingerprints makes the plugin create the account's identity in `omemo.db` even while offline (plugin behaviour).
- The completion popover was tested on X11 only.

### M6: Desktop integration (Sway first, GNOME second)
- **Tray.** An SNI tray replaces `gtkdocklet-gtk.c`. The `docklet_ui_ops` interface (`gtkdocklet.h:30`) and `gtkdocklet.c` logic are kept: status/pending icons and the "attention" status replace blinking; the tooltip keeps the unread list.
  - The menu is built from the existing docklet menu (`gtkdocklet.c:675`).
  - Left click toggles the buddy list or presents the pending conversation.
  - If no SNI watcher exists (GNOME without the AppIndicator extension), the tray is not shown and closing the buddy list window keeps the app running.
- **Idle.** A `get_time_idle()` UI op backed by `ext_idle_notifier_v1`:
  - get `wl_display` via `gdk_wayland_display_get_wl_display`; generate code with `wayland-scanner` from wayland-protocols 1.49;
  - use a 60 s notification; idle time = now − (idled_at − 60 s).
  - On GNOME, if the protocol is unavailable, poll `org.gnome.Mutter.IdleMonitor.GetIdletime`.
  - libpurple's polling (`libpurple/idle.c:115`) is unchanged.
- **Sounds.** GSound replaces `gtksound.c`'s playbin, keeping the custom-command option.
- **Notifications and attention.** `GNotification` for new messages (the notify plugin's option). `gtk_window_present` uses xdg-activation, which Sway marks urgent. The X11 `_PurpleUnseenCount` property is dropped; the count is shown in the SNI tooltip/title.

**Status: done under Sway; the GNOME checks and everything that needs signed-in accounts or M4b's conversation windows are left to the user** (the M6 section of `pidgin4/TESTING.md`).

**Sway rule.** pidgin4 is native Wayland with `app_id` `com.minowick.Pidgin4`, so the existing `assign [class="Pidgin"] 2:chat` (`~/.config/sway/config:291`, for Pidgin 2 on XWayland) does not match it. Add `assign [app_id="com.minowick.Pidgin4"] 2:chat` next to it (not done: the user's config is not edited).

**What's in `pidgin4/`:**
- **`pidgindbusmenu.[ch]`**: `PidginDBusMenu`, a `com.canonical.dbusmenu` (v3) exporter for a `GMenuModel` + `GActionGroup` (actions under one prefix), GIO only. `GetLayout` (depth, property filter), `GetGroupProperties`, `GetProperty`, `Event`/`EventGroup` ("clicked" activates the action with its target), `AboutToShow`/`AboutToShowGroup` (an "about-to-show" signal lets the owner refresh the model; the reply says whether the layout changed), the `Version`/`TextDirection`/`Status`/`IconThemePath` properties, `LayoutUpdated` on any model change (coalesced) and `ItemsPropertiesUpdated` on action state/enabled changes. Sections become separators, submenus `children-display`, boolean-state actions `checkmark` and targeted stateful actions `radio` toggles, `hidden-when` `visible`, themed icons `icon-name` and file/bytes/resource icons `icon-data`. Labels lose their mnemonics; literal underscores are escaped as `__`. Ids are renumbered on every rebuild.
- **`pidginsni.[ch]`**: `PidginSni`, an `org.kde.StatusNotifierItem` on GDBus. It exports `/StatusNotifierItem` and the menu at `/MenuBar`, owns `org.kde.StatusNotifierItem-<pid>-<n>`, and follows `org.kde.StatusNotifierWatcher` with `g_bus_watch_name`: it registers whenever a watcher appears (also after a Waybar restart) and reports "registered"/"unregistered". Properties: `Category` (Communications), `Id` (`com.minowick.Pidgin4`), `Title`, `Status`, `WindowId`, `IconThemePath` (the prefix's `share/icons`), `Menu`, `ItemIsMenu` (false), `IconName` + `IconPixmap` (ARGB32 big-endian from the GResource PNGs, via `GdkTextureDownloader` `A8R8G8B8`), `OverlayIconName`, `AttentionIconName` + `AttentionIconPixmap`, `ToolTip`. Methods `Activate`, `SecondaryActivate`, `ContextMenu`, `Scroll` and KDE's `ProvideXdgActivationToken`; signals `NewTitle`, `NewIcon`, `NewAttentionIcon`, `NewOverlayIcon`, `NewToolTip`, `NewStatus`.
- **`gtkdocklet.[ch]`**: Pidgin 2's docklet logic with `docklet_ui_ops` kept (the GTK 2 `position_menu` is an unused `gpointer`). Status and pending icons, "connecting", the unread tooltip (5 lines), show `always`/`pending`/`never`. Blinking is replaced by `NeedsAttention` (the Blink item is gone). The menu is a `GMenuModel` with `tray.*` actions, rebuilt when the state changes and on `AboutToShow`: Show Buddy List, Unread Messages, New Message, Join Chat, Change Status (radio primitives with status icons, popular saved statuses, New…/Saved…), Accounts, Plugins, Preferences, File Transfers (these forward to the buddy list's `win.*` actions, so M5's windows appear there by themselves), Mute Sounds, plugin actions, Quit. The GTK 2 docklet registers no signals, so there are none to keep. New API: `pidgin_docklet_is_embedded()`, `_start_hidden()`, `_get_menu()`, `_get_actions()`, `_refresh_menu()`, `_get_unread_count()`, `_present_pending()`.
- **`gtkdocklet-sni.c`**: the only `docklet_ui_ops` implementation. Left click = `pidgin_docklet_clicked(1)` (next unread conversation, else toggle the buddy list, using the host's activation token when one was provided), middle click = button 2 (nothing, as in Pidgin 2). Icon names `com.minowick.Pidgin4-{available,away,busy,extended-away,invisible,offline,connecting,pending}` (Pidgin 2's tray icons, in the GResource and installed into hicolor). The title is "Pidgin (N unread messages)", replacing `_PurpleUnseenCount`.
- **`gtkblist.c`** (small): closing hides into the tray while `pidgin_docklet_is_embedded()`, otherwise quits (unless `close_hides`). This follows the M6 task rather than the bullet above: without a watcher (GNOME without the extension) closing **quits**, since there would be no way back to a hidden list. The quit now runs from an idle, because quitting inside `close-request` finalized the window (and the status box's destroy handler) after libpurple had shut down (a critical, fatal under `G_DEBUG=fatal-criticals`). The list starts hidden when it was hidden in the tray at the last quit and a watcher is running (checked once with `NameHasOwner`; shown after 5 s if the icon does not register). `set_visible` keeps `/pidgin4/blist/list_visible` current. Losing the tray shows a hidden list again.
- **`gtkidle.c` + `pidginidle-wayland.[ch]`**: `get_time_idle` again. `/pidgin4/idle/method` `system` tries ext-idle-notify-v1 (version 2 `get_input_idle_notification` when offered, so inhibitors like a playing video do not count as activity), then `org.gnome.Mutter.IdleMonitor.GetIdletime` polled every 10 s, then no ops (libpurple's own "purple" tracking); `purple` = no ops; `none` = never idle. Idle time = now − (idled_at − 60 s); `resumed` calls `purple_idle_touch()` so libpurple notices at once. Meson generates the client code with `wayland-scanner` from wayland-protocols' `staging/ext-idle-notify/ext-idle-notify-v1.xml`. **Event queue, as tested:** GTK 4.22 reads the display fd for all queues but dispatches only its own queue, so proxies on the default queue received `idled`/`resumed` only during GDK's roundtrips at shutdown. Our registry, notifier and notification therefore live on a private `wl_event_queue`, dispatched by a GSource whose `prepare` probes the queue with `wl_display_prepare_read_queue()` and cancels at once; it never reads the fd (a second reader in one thread would deadlock in `wl_display_read_events()`).
- **`gtksound.[ch]`**: `PurpleSoundUiOps` again, with Pidgin 2's event rules (conv_focus, 10 s of silence after sign-on; while_status stays in libpurple). GSound `gsound_context_play_full` with `media.filename`, the sound-naming-spec `event.id` (`message-new-instant`, `message-sent-instant`, `service-login`, …), `event.description`, `media.role=event`, `canberra.volume` in dB (volume 50 = 0 dB, Pidgin 2's scale) and `canberra.cache-control=never` (a cached sample is named by its event id, which would swap sounds with other applications). Or the custom command with `%s` → the shell-quoted file (`g_spawn_async`). Default files from `<libpurple prefix>/share/sounds/purple`. `pidgin_sound_play_file()` is exported for M5's "Play" button.
- **`pidginnotify.[ch]`**: `GNotification`s, see the header. `pidgin_notification_new_message()`/`_withdraw()` are the API for the notify plugin (M7). App action `app.present-conversation` `(ssis)`: protocol id, account, conversation type, name.
- **`stubs.[ch]`**: weak fallbacks, replaced at link time by the real functions: `pidgin_conversations_find_unseen_list()` (Pidgin 2's signature, reading the `unseen-state` conversation data), **`pidgin_conversations_get_unseen_count(PurpleConversation *)`** (new; reads `unseen-count`), `pidgin_status_editor_show()`, `pidgin_status_window_show()`. **M4b**: provide `pidgin_conversations_find_unseen_list()` and `pidgin_conversations_get_unseen_count()` with these names (the latter returning `PidginConversation.unseen_count`), and emit `conversation-updated` `PURPLE_CONV_UPDATE_UNSEEN` when the unseen state changes (as Pidgin 2 does): the tray and the notification withdrawal follow that. When `gtkconv.h` exists, `stubs.h` includes it instead of its own `PidginUnseenState`.
- **Build**: `meson.build` adds wayland-client, gtk4-wayland, wayland-protocols, wayland-scanner and gsound; installs the tray icons into hicolor. `scripts/build-pidgin4.sh --desktop-integration` also links the 32 tray icons; the `.desktop` file has `X-GNOME-UsesNotifications=true` (and `StartupNotify=true` already).

**Prefs** (all new keys are under `/pidgin4`; the shared ones are registered with Pidgin 2's types and defaults):
- `/pidgin4/docklet/show`: empty (default) = follow the shared `/pidgin/docklet/show`; else `always`/`pending`/`never` for pidgin4 only. `/pidgin/docklet/blink` is registered but unused.
- `/pidgin4/idle/method`: `system` (default), `purple` or `none`. For M5's preferences page.
- `/pidgin4/sound/method` (`gsound` default, `command`, `none`), `/pidgin4/sound/command` (string), `/pidgin4/sound/volume` (0–100, default 50). Shared: `/pidgin/sound/enabled/<event>`, `/pidgin/sound/file/<event>`, `/pidgin/sound/conv_focus`, `/pidgin/sound/mute`. Pidgin 2's `/pidgin/sound/{method,command,volume,theme}` are neither registered nor read.
- `/pidgin4/notifications/new_message`, `/pidgin4/notifications/show_message` (both TRUE).

**Verification done:**
- **Build**: zero warnings; `scripts/build-pidgin4.sh --test` passes all 14 tests (13 OK, the GUI selftest skipped as before). New: `dbusmenu` (labels, layout tree with separators/submenus/checkmark/radio/disabled/hidden/icon-name/icon-data, depth and property filters, `GetProperty`/`GetGroupProperties`/properties, `Event` and `EventGroup` running actions, `ItemsPropertiesUpdated` on state and enabled changes, `LayoutUpdated` on submenu and section changes, `AboutToShow` refresh, model replacement, unexport), `sni` (with a fake watcher on a private `GTestDBus`: no registration without a watcher, registration when it appears, "unregistered" when it vanishes, re-registration, removal on stop, every property, pixmap sizes and the A-R-G-B byte order against GDK's own RGBA download, all methods, the activation token, `New*` signals only on change, the exported menu), `idle` (the arithmetic, no display) and `sound` (command substitution and quoting, volume → dB, event names, pref types and defaults, default and custom files).
- **Headless** (Xvfb, `GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`, `dbus-run-session`, `-c ~/.purple-gtk4 -n -d`):
  - no watcher: the item is exported but never registers; closing the buddy list (`PIDGIN4_DOCKLET_CLOSE_BLIST`) quits with status 0;
  - with `fake-sni-watcher`: the item registers; `busctl --user tree` shows `/StatusNotifierItem` and `/MenuBar`; `GetAll` gives `NeedsAttention`, "Pidgin (3 unread messages)", `com.minowick.Pidgin4-pending` and the pixmaps during the docklet selftest; `GetLayout` returns the docklet menu (Unread Messages → "Docklet Selftest (3)", the status radio items with Offline checked, the popular saved statuses); closing the list hides it into the tray; two `Activate` calls show and hide it; the next start keeps it hidden; SIGTERM exits 0.
- **Sway 1.12 (real session, `-n -m`, briefly, `G_DEBUG=fatal-criticals GTK_A11Y=none`)**: the item registers with Waybar's watcher (listed in `RegisteredStatusNotifierItems`), with the installed binary's `IconThemePath`, `NeedsAttention` during the docklet selftest and the full menu over `GetLayout`. ext-idle-notify-v1 binds at version 2; with `PIDGIN4_IDLE_TIMEOUT=1`/`2`, live `idled`/`resumed` pairs arrived during the run (the user was active, pausing 1–5 s). `PIDGIN4_SOUND_SELFTEST` played `receive.wav` through GSound (completion without error). `PIDGIN4_NOTIFY_SELFTEST` sent `Notify` to mako (id 234, `desktop-entry` hint `com.minowick.Pidgin4`, category `im.received`) and `CloseNotification` 5 s later (seen with `dbus-monitor`). Every run was stopped with SIGTERM and exited 0; no pidgin4 of this milestone was left running.
- **Desktop integration** against a scratch `XDG_DATA_HOME`: 39 links (the 32 tray icons included), the `.desktop` file validates, and `--remove-desktop-integration` removes all 39. The real `~/.local/share` was not touched.
- **Profile round-trip**: `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4-m6/bin/pidgin4` **PASSES** (2 allowed changes: `list_visible` and a transient status's use count). The dev profile had already seen M6's keys from the test runs, so they are part of the baseline; all of them are under `/pidgin4` apart from the shared sound and docklet keys that Pidgin 2 registers itself.
- **No account was signed in.**

**Gaps:**
- **No conversation windows yet (M4b).** The unread state, the Unread Messages menu, "present the pending conversation", the notification withdrawal on focus and the sound/notification focus rules all depend on gtkconv; they were exercised with the docklet selftest's stand-in conversation and libpurple signals, not with real conversations.
- **GNOME not run.** The Mutter IdleMonitor path, the AppIndicator extension and GNOME Shell notifications are untested (checklist in `TESTING.md`). GNOME activates notifications through the app's bus name, so that needs a run without `-m`.
- **Attention.** The SNI `Activate` carries no activation token from Waybar (only hosts that call `ProvideXdgActivationToken` give one), so presenting from the tray relies on GTK requesting its own xdg-activation token; Sway then applies `focus_on_window_activation` (default `urgent`). Notification clicks get the server's token through GLib/GTK. Not checked by eye.
- **Not ported:** sound themes (`/pidgin/sound/theme`, the loader is M5), the per-conversation "Enable Sounds" toggle (gtkconv, M4b), per-account statuses in the tray's status submenu, Pidgin 2's "beep" sound method. The CSI hook from M8 (`csi-set-active` when idle) is not wired: libpurple's idle/away already drives it.
- **Idle below the threshold.** ext-idle-notify only reports crossing the 60 s threshold, so idle times below a minute read as 0; the Mutter poll lags by up to 10 s.
- **Menu ids** are renumbered on every rebuild; a click on a menu that was open across a rebuild (e.g. a status change while it is shown) could hit the item that now has that id.

### M7: Plugins
- **Port the plugins in use:** history (MessageView API), markerline (marker item), timestamp_format (MessageView context-menu hook replaces the GtkTextView `populate_popup` emission hook), notify (event controllers, GNotification), cap (GtkGrid prefs).
- **Also port (cheap and useful):** convcolors, spellchk, sendbutton, gtkbuddynote, iconaway, relnot, timestamp, xmppconsole, xmppdisco.
- **Drop:** gestures, transparency, extplacement, ticker, pidginrc, themeedit, musicmessaging, gevolution, unity, vvconfig, Perl/Tcl/Mono loaders.
- **Plugin API:** the pidgin plugin API is not preserved. PidginConversation etc. get accessors where the ported plugins need them.

### M8: Modern XMPP and IRCv3 (protocol work can start after M1; UI parts need M4)
**Additive libpurple API, signal-based**, so that prpl plugins built against it still load and run on stock libpurple 2.14.14 (see M9). There are no new exported functions for prpls to call, and no struct changes.

- New signals on the conversations handle, registered by libpurple:
  - `receiving-message-meta`: emitted by the prpl just before `serv_got_im`/`serv_got_chat_in`. It carries account, conversation name and a `GHashTable` of string keys: `stanza-id`, `origin-id`, `server-id`, `correction-of`, `reply-to`, `reply-to-sender`, `fallback-ranges`. The UI consumes it on the next `write_conv` for that conversation.
  - `sending-message-meta`: emitted on the send path, so the prpl can report the id it assigned.
  - `message-corrected`, `message-reaction` (add/remove, emoji, sender), `message-receipt` (delivered/displayed), `message-retracted`.
- **Capability detection.** pidgin4 adds `"message-meta" → "1"` to its `purple_core_get_ui_info()` table. Prpls check that key before emitting and otherwise fall back to their current text output. That avoids the "Signal data for … not found" error that stock libpurple logs for unregistered signals (`signals.c:277`).
- **UI → prpl actions** go through `purple_plugin_ipc_*` on the prpl plugin: `send-correction`, `send-reaction`, `send-retraction`, `send-marker` and `send-reply`. The same mechanism works for jabber and Discord. There are no new PurplePluginProtocolInfo fields.
- The message index (M4) is the single store for ids. MAM state lives in it as well; there is no separate `xmpp-mam.db`.

Features in `libpurple/protocols/jabber/`:

- **Unique stanza ids (XEP-0359)**: the foundation for everything below.
- **Carbons (XEP-0280)**: sent carbons use the existing `PURPLE_MESSAGE_REMOTE_SEND` flag.
- **MAM (XEP-0313)**: catch-up on connect and scroll-back history, for 1:1 and for MUC archives. Messages are deduplicated by stanza-id against the message index, which also holds the per-account and per-MUC last archive id. Fetched messages are logged normally.
  - For log lines Pidgin 2 wrote (no ids), dedup is fuzzy: time ±2 min, sender and plain text.
  - OMEMO messages that Pidgin 2 logged as the "unsupported" fallback are decrypted from the archive and logged as new lines. The ratchet state in `omemo.db` was untouched while Pidgin 2 ran, so this works within the archive's retention.
- **HTTP File Upload (XEP-0363)** via libsoup 3. It is chosen in `send_file` when the server advertises it, with fallback to SI/Jingle. Received OOB/`aesgcm` URLs render as links, and images as inline previews.
- **Message semantics**. Each of these renders in PidginMessageView:
  - delivery receipts (0184);
  - chat markers (0333);
  - last message correction (0308);
  - reactions (0444);
  - replies (0461);
  - retraction (0424), plus moderation (0425) in MUCs where you have the rights.

  Correction and reply are available from the entry: Up-arrow edits the last message, and reply is in the row menu.
- **Fallback indication (XEP-0428)**: the reply quote and reaction fallback text are stripped from the body when the feature is rendered natively, and kept for plain clients.
- **Occupant id (XEP-0421)**: MUC corrections, reactions and retractions are matched by occupant-id rather than by nick.
- **MUC self-ping (XEP-0410)**: detects silently dropped MUC joins and rejoins them.
- **Message Styling (XEP-0393)**. Incoming plain bodies are rendered in PidginMarkup's 0393 mode. **Outgoing**: XHTML-IM (0071) is no longer sent. Toolbar bold, italic, strike and code are serialized as 0393 markers in the plain body; fonts, sizes and colours are not offered for XMPP. Incoming XHTML-IM is still rendered.
- **Displayed-state sync (XEP-0490)**: the last displayed message per conversation is synced through PEP. Reading on another device clears unread state here, and reading here publishes it.
- **Consistent colours (XEP-0392)**: XMPP nick and avatar-placeholder colours match those in Dino and Conversations.
- **Authentication:**
  - SCRAM-SHA-256 (extend `auth_scram.c`), plus the `-PLUS` variants using `purple_ssl_get_channel_binding()` (tls-exporter);
  - **SASL2 (0388) + Bind2 (0386) + FAST (0484)**: one-round-trip login and reconnect with a FAST token. Falls back to legacy SASL + bind. The FAST token is stored in `<profile>/pidgin4/messages.db`, keeping `accounts.xml` unchanged.
- **OMEMO**: a separate in-tree libpurple plugin, lurch-style. It hooks `jabber-receiving-xmlnode`/`jabber-sending-xmlnode`. State is kept in `<profile>/pidgin4/omemo.db`.
  - Scope: **legacy OMEMO 0.3** (`eu.siacs.conversations.axolotl`), which is the interop baseline with Conversations, Dino and Gajim. It covers 1:1 chats and **private (members-only, non-anonymous) MUCs**. OMEMO 2 is out of scope.
  - It needs **libomemo-c** (the maintained fork of libsignal-protocol-c used by Dino); 0.5.1 is installed. Crypto uses libgcrypt.
  - It includes device list PEP, bundles, trust-on-first-use, and a fingerprint trust UI in pidgin4 (conversation info + a lock indicator).
  - Encrypted media (aesgcm) download is decrypted too.
  - The IPC contract `pidgin4/pidginimageloader.c` calls (M4):
    - The plugin id is `core-omemo`, and the command is `"omemo-decrypt-url"`.
    - Its signature is `gboolean (const char *aesgcm_url, GByteArray *data)`, marshalled with `purple_marshal_BOOLEAN__POINTER_POINTER` and registered with two `PURPLE_TYPE_POINTER` parameters.
    - `data` is the downloaded ciphertext followed by the 16-byte GCM tag. The plugin decrypts it in place with the IV and key from the URL fragment, resizes it to the plaintext and returns TRUE.
    - It is called on the main thread. Decrypted images are cached in memory only, never on disk.
- **Low-cost extras:**
  - direct TLS via `_xmpps-client` SRV (XEP-0368);
  - bookmarks with autojoin (XEP-0402, 0048 fallback);
  - stream management resume (`stream_management.c` currently only acks);
  - Client State Indication (0352) tied to window focus/idle.

Features in `libpurple/protocols/irc/` (testable with the GTK 2 UI):

- **TLS**: accounts are moved to TLS on 6697 in the copied profile, through the account editor.
- **Built-in SASL** PLAIN and EXTERNAL, independent of cyrus-sasl. When `HAVE_CYRUS_SASL` is set, cyrus is still used for other mechanisms.
- **CAP 302 negotiation** with `server-time` (message timestamps), `echo-message` (sent messages confirmed by the server), `away-notify`, `account-notify`, `multi-prefix` and `message-tags`. `msgid` tags feed `receiving-message-meta`.

**Landed (HTTP upload), 2026-09-22:**
- New `protocols/jabber/httpupload.[ch]` (XEP-0363 over libsoup 3). `configure.ac` requires `libsoup-3.0` whenever the jabber prpl is built, with a clear error otherwise. Only `libjabber.so` links libsoup; `libpurple.so` doesn't, and the ABI gate passes unchanged (no new libpurple symbols).
- **Discovery** on `signed-on`: disco#info on the server domain and on each disco#items entry (without a node) finds `urn:xmpp:http:upload:0`; the JID and the XEP-0128 `max-file-size` are kept per connection. `disco.c` is untouched: httpupload sends its own queries. prpls are probed before `purple_connections_init()`, so the connection signals are connected from a 0 ms timeout.
- **Sending:** `jabber_si_xfer_init()` offers the transfer to `jabber_http_upload_send_xfer()` first (when the service is known, the file fits, and the account setting `http_upload` isn't FALSE). It requests a slot (filename, size, `g_content_type_guess` type), then PUTs the file with `soup_session_send_and_read_async`, streaming a `GFileInputStream`. Only the slot's `Authorization`/`Cookie`/`Expires` headers are sent, with CR/LF removed. Progress goes through `purple_xfer_set_bytes_sent`/`update_progress` (throttled to 10/s), and cancel through a `GCancellable`. On success the GET URL goes out as the body plus `<x xmlns='jabber:x:oob'><url>` (1:1 with a local echo into the IM conversation; MUC as groupchat, echoed by the room). A failure before the PUT hands 1:1 transfers back to SI; after it, or for MUCs, the transfer fails with `purple_xfer_error()`. Proxy: the account's proxy, else the default `GProxyResolver` for GNOME/environment settings, or the explicit global HTTP/SOCKS proxy (Tor → socks5), mapped onto the `SoupSession`.
- **MUC:** `chat_can_receive_file`/`chat_send_file` implemented; `can_receive_file` is TRUE for everyone while an upload service is available.
- **Receiving:** `jm_body_with_oob()` (message.c) calls the new `jabber_oob_x_append_to_body()` (oob.c). A body that already is or contains the OOB URL (0363 uploads, OMEMO `aesgcm://`) passes through unchanged. Before, the prpl rewrote it into an `<a href='…'>` with an unescaped attribute and duplicated URLs containing `&`. Nothing is downloaded by the prpl. There's no `oob-url` meta yet, because `receiving-message-meta` doesn't exist in this tree: the UI should detect "body is a single URL".
- `libxmpp.c` changes: 3 prpl-table entries and a one-line `http_upload` account option (bool, default TRUE; a new additive account setting).
- **Verified:**
  - unit tests for slot/error/disco parsing, the request element, content types, OOB folding and the PUT against a `SoupServer` (headers, body SHA-256 on 3 MiB, 403, cancel mid-PUT, missing file);
  - an end-to-end run of a throwaway libpurple client against a fake XMPP server with an upload service (discovery, 1:1 and MUC uploads with the GET URL serving identical bytes, SI fallback on a refused slot, the MUC quota error, upload through an account HTTP proxy, the `http_upload=FALSE` switch, cancel mid-PUT, disconnect with a pending slot request, an incoming body+OOB URL passing through), also under ASan;
  - the GTK 2 pidgin from the prefix loads `libxmpp.so` cleanly headless with `-n`.
  - Not tested against a real Prosody or ejabberd: neither is installed.
- **Open:**
  - OMEMO: a 1:1 upload's URL message goes through `jabber_send`, so `jabber-sending-xmlnode` hooks (the OMEMO plugin) see it. The plugin should either skip it or do aesgcm uploads itself; plain uploads are unencrypted on the server.
  - The URL message bypasses `jabber_message_send`, so it gets none of the send-path extras (receipt request, origin-id, `sending-message-meta`) that the message agent adds.
  - `retry` errors aren't retried automatically.
  - pidgin4's transfer UI should show upload progress, and it can offer "send file" for MUCs.
**Landed (IRC)** (2026-09-22, branch `worktree-agent-a21ed33ad0cf90230`; all in `libpurple/protocols/irc/`, no libpurple API change, ABI gate passes):
- **TLS:** with `ssl` on and `port` still 6667, the prpl connects to 6697 (the saved `port` is not rewritten); `IRC_DEFAULT_SSL_PORT` is 6697. ssl-nss negotiates TLS 1.3 with no change. **User action:** flip the 10 IRC accounts to TLS in `~/.purple-gtk4` through the account editor (tick *Use SSL*; set the port to 6697, or leave 6667 and let the connect-time mapping pick 6697), then enable SASL where the network supports it.
- **Message tags** (`parse.c`): `@tags` are parsed with the IRCv3 unescaping into `irc->tags` for the handler (`irc_msg_tag()`). Prefix-less server messages (e.g. `AUTHENTICATE +`) are dispatched normally.
- **CAP 302** (`cap.c`): `CAP LS 302` goes first, then `PASS`/`USER`/`NICK`; multi-line LS, `REQ` (NAK retried per cap), `ACK`, runtime `NEW`/`DEL`. Requested when offered: `sasl` (only with SASL on), `server-time`, `echo-message`, `away-notify`, `account-notify`, `multi-prefix`, `message-tags`, `cap-notify`, `extended-join`, `chghost`. Servers without CAP: a 421 or a 001 ends negotiation; no reply at all → `CAP END` after 5 s.
- **Built-in SASL** (`sasl.c`): `PLAIN` and `EXTERNAL`, 400-byte `AUTHENTICATE` chunking, `903`/`907` → `CAP END`, `902`/`904`/`905`/`906` (+ `908` mechanism list) → a specific connection error. PLAIN over plaintext needs `auth_plain_in_clear`. With `HAVE_CYRUS_SASL`, mechanism `cyrus` runs the old Cyrus code.
- **Account option keys:** `sasl` (bool, *Use SASL authentication*, default off), `sasl_mechanism` (**new**; list `PLAIN` | `EXTERNAL` [| `cyrus`], default `PLAIN`), `saslname` (*SASL login name*, default: the nick), `auth_plain_in_clear` (bool). `sasl`, `saslname` and `auth_plain_in_clear` are the pre-existing Cyrus-only keys, now always shown. Existing accounts are unaffected (SASL off by default); Pidgin 2.14 keeps the extra key.
- **server-time** timestamps received and echoed messages. **echo-message**: no local echo; the server echo is shown with `PURPLE_MESSAGE_SEND` (a message from our own nick counts as sent; our own CTCPs other than ACTION are ignored). **away-notify** updates chat user away flags and buddy away status (ISON polling stays for online/offline). **account-notify**/**extended-join** store the services account (chat user attribute `account`, buddy tooltip). **multi-prefix** in NAMES and WHO. **chghost** updates `userhost`.
- **`receiving-message-meta`**: emitted before `serv_got_im`/`serv_got_chat_in` (or the IM write of an echo) when the message has a `msgid` and ui_info has `message-meta` = `1`; keys `stanza-id` (msgid) and `reply-to` (`+draft/reply`). The emitter doesn't register the signal (conversation.c does, on its branch); it's all in `irc_emit_message_meta()` in `msgs.c`.
- **Verified** against a scripted fake ircd (Python asyncio, TLS 1.3, self-signed cert pinned in a scratch profile) with a throwaway libpurple client: 16 scenarios (full CAP/SASL/tags flow, no-CAP 421, silent server, 5 s timeout, PASS ordering, NAK retry, SASL failure, 400-byte chunking both ways, EXTERNAL, 908, mechanism not offered, sasl cap missing, PLAIN in clear, 6667 → 6697), plus unit tests for the tag parser, server-time, CAP list parsing, PLAIN payload/chunking and prefixes. The Cyrus path is only syntax-checked (no cyrus-sasl headers here). The GTK 2 pidgin from the prefix loads `libirc.so` cleanly (`-n`, 20 s).
- **Open:** EXTERNAL needs a client certificate, and ssl-nss has no client-cert support (no `SSL_GetClientAuthDataHook`; NSS runs with `NSS_NoDB_Init`). Until ssl-nss gains that, EXTERNAL only sends the empty response. Not yet tested against a real network (Libera: TLS + SASL PLAIN, server-time on bouncer playback, echo-message).
**Landed (protocol core), 2026-09-22.** XMPP only; the IRC items above are separate work. The UI side (M4) implements against this list. Everything is additive; the libpurple ABI gate passes (no new exported libpurple symbols, only signal registrations).

- **Capability gate.** `jabber_ui_supports_message_meta()` (in `message.c`) reads `purple_core_get_ui_info()["message-meta"] == "1"` once and caches it. Only then does the jabber prpl emit the signals below and strip fallback text. Without it (the GTK 2 UI), the output is unchanged except where noted under *Behaviour without message-meta*.
- **Conversation signals** (conversations handle; documented in `doc/conversation-signals.dox`):
  - `void receiving-message-meta(PurpleAccount *account, const char *conv_name, GHashTable *meta)`: emitted immediately before `serv_got_im()`/`serv_got_chat_in()`, or before `purple_conv_im_write()` for messages this account sent from another device. `conv_name` is what goes to `serv_got_im()` (the sender's full JID for IMs, for example `friend@example.net/phone`), the room's bare JID for chats, and the counterpart's JID for outgoing messages. `meta` maps strings to strings, and handlers may add keys. Keys:
    - `conv-type`: `im` or `chat`;
    - `sender`: the full JID (IM), the nick (chat), or our own full JID (outgoing);
    - `outgoing`: `1` for messages we sent from another device (sent carbons, archived outgoing messages). These are written with `PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_REMOTE_SEND` (plus `DELAYED` from MAM);
    - `timestamp`: decimal seconds since the epoch (the archive's `<delay/>` for MAM);
    - `stanza-id`: the `<message id=''>`. Receipts, markers, 1:1 corrections and replies to our own outgoing messages refer to it. Our own sent messages use a UUID here, equal to their origin-id;
    - `origin-id`: the XEP-0359 `<origin-id/>`;
    - `server-id` and `server-id-by`: the XEP-0359 `<stanza-id/>` assigned by our own archive (1:1) or by the room (MUC), and the archive's bare JID. This is the MAM id: MAM dedup, `mam/last-id` and the `before` argument of `mam-fetch-older` all use it, and MUC replies/reactions refer to it. A `<stanza-id/>` from any other `by` is ignored;
    - `occupant-id`: XEP-0421, set only for rooms whose disco#info lists `urn:xmpp:occupant-id:0`;
    - `correction-of`: XEP-0308 `<replace id=''>`;
    - `reply-to` and `reply-to-sender`: XEP-0461 `<reply id='' to=''>`;
    - `fallback-ranges`: XEP-0428, `for:start-end` entries joined with `;` (code points, end exclusive). `for:` alone means the whole body;
    - `fallback-stripped`: `1` when the reply quote or reaction fallback was removed from the body. That happens only when the message also carries `<reply/>` or `<reactions/>`, respectively;
    - `carbon`: `received` or `sent`;
    - `mam`: `1` for archive results, and `mam-query`: `catchup` or `older`. `older` means a scroll-back page, older than anything shown; prepend it.

    A handler may insert `discard` = `1`, and the prpl then drops the message: it is neither written nor logged. That is how the UI dedups MAM against `messages.db`.
  - `void sending-message-meta(PurpleAccount *account, const char *conv_name, GHashTable *meta)`: emitted from `send_im`/`send_chat` right after the stanza is sent. For IMs, that is before libpurple writes the sent message. In rooms, the message is written when the room reflects it, and the reflection carries the same `stanza-id`/`origin-id` in its `receiving-message-meta`. Keys: `conv-type`, `stanza-id`, `origin-id` (equal to each other).
  - `gboolean message-corrected(PurpleAccount *account, const char *conv_name, const char *target_id, const char *new_id, const char *new_body, const char *sender)`
  - `gboolean message-reaction(PurpleAccount *account, const char *conv_name, const char *target_id, const char *emoji, const char *sender, gboolean add)`. The marshaller reads six pointers, so emitters pass `GINT_TO_POINTER(add)`.
  - `gboolean message-receipt(PurpleAccount *account, const char *conv_name, const char *id, const char *state, const char *sender)`, where `state` is `delivered` or `displayed`.
  - `gboolean message-retracted(PurpleAccount *account, const char *conv_name, const char *target_id, const char *sender, const char *reason)`

    For these four, a handler returns TRUE if it rendered the event, and emitters fall back to their text output when none does. They are registered but **not yet emitted by jabber**: that is the message-semantics follow-up. The dispatcher `jabber_message_parse_modern_child()` in `message.c` has `TODO(XEP-…)` slots for 0308, 0444, 0184, 0333 and 0424/0425, and `jabber_message_send()` has one for the outgoing elements.
- **Jabber plugin signal:** `void mam-query-done(PurpleAccount *account, const char *conv_name, const char *first_id, const char *last_id, gboolean complete)`, with `complete` marshalled as a `guint`. It is emitted once per query, after its last page. `conv_name` is NULL for the account catch-up and the room JID for a room catch-up. `first_id` and `last_id` are server-ids, or NULL when the page was empty.
- **Jabber plugin IPC** (`purple_plugin_ipc_call(prpl_jabber, name, &ok, ...)`):
  - `gboolean mam-fetch-older(PurpleAccount *account, const char *conv_name, const char *before_id, guint count)` fetches one page, of up to `count` messages (1–100), before the server-id `before_id`. NULL or `""` means the newest page. It reads the room archive if `conv_name` is a joined room and the room has MAM; otherwise it reads our own archive filtered by `with` = the bare JID (or the full JID for a room PM). Results arrive as ordinary messages with `mam-query` = `older`, oldest first, followed by `mam-query-done`, whose `first_id` is the next `before_id`. It returns FALSE if the account is not connected or has no archive.
  - `gboolean bookmark-add(PurpleAccount *account, const char *room_jid, const char *nick, gboolean autojoin)` (marshalled `guint`) and `gboolean bookmark-remove(PurpleAccount *account, const char *room_jid)`. They publish or retract in whichever format the account uses (XEP-0402 native items, or the whole XEP-0048 `storage:bookmarks` item). A new autojoin bookmark is joined immediately.
- **kv keys** (`jabber_kv_load/store`, answered by the UI from `messages.db`):
  - `mam/last-id`: the newest server-id of our own archive that we have shown;
  - `mam/last-id/<room@server>`: the same per room, with the room JID lower-cased as the prpl stores it.

  They are stored after every MAM page and, once the catch-up has finished, on every live message with a trusted server-id. They are cleared when the archive answers `item-not-found` for them (expired), and the catch-up then restarts. The prpl also keeps them in a per-process session table, so a reconnect catches up even without a UI store.
- **Account catch-up.** After the server disco, a disco#info to our bare JID looks for `urn:xmpp:mam:2` (and `urn:xmpp:bookmarks:1#compat`). If `mam/last-id` is known, the catch-up reads forward from it with RSM `after`, in pages of 50 up to 20 pages. If not, a message-meta UI gets the last 24 h, bounded the same way. Any other UI only learns the newest id (a `max=1`, `<before/>` "position" query with nothing delivered), so the GTK 2 UI never replays history into its logs. Results go through `serv_got_im()` with `PURPLE_MESSAGE_DELAYED` and the archived time, so libpurple logs them as usual. Chat states and resource binding are ignored for archived messages; invites, headlines, events and buzzes from the archive are dropped.
- **Room catch-up.** `jabber_join_chat()` sends a disco#info to the room before the join presence (`chat->mam_supported`, `chat->occupant_id_supported`). After our own presence (status 110) arrives, the catch-up reads forward from `mam/last-id/<room>`, or learns the position on a first join, where the room's own history covers it. When a position is known, the join asks for `<history maxstanzas='0'/>` unless the user configured history.
- **Session dedup guard.** A bounded (4096) per-account set of `s:<by>:<server-id>` keys and our own `o:<origin-id>` keys drops a message already shown in this process: live then archive, room history after a rejoin, or our own message coming back from the archive.
- **Carbons (XEP-0280).** They are enabled when the server lists `urn:xmpp:carbons:2`. Carbons are accepted only from our bare JID (or with no `from`). Groupchat, `<private/>`, and carbons of MUC PMs for rooms we are not in are ignored. Received carbons are handled like live messages, with the inner message's ids. Sent carbons go to the counterpart's IM conversation as `SEND | REMOTE_SEND`, never through `serv_got_im()`, so no auto-reply is sent.
- **MUC self-ping (XEP-0410).** A 30 s per-stream tick pings `room@server/nick` after 5 min without room traffic, and every room 10 s after `network-configuration-changed`. That signal is connected lazily, since the network signals are registered after prpls are probed. A result, `service-unavailable`, `feature-not-implemented` or `item-not-found` means still joined: XEP-0410 §3 says so, so none of them triggers a rejoin. `remote-server-*` retries in a minute. Anything else, or no answer within 90 s, triggers a silent rejoin: a join presence with `maxstanzas=0` (MAM rooms) or `since=<last activity>`. After our presence comes back, the room catch-up fills the gap.
- **Bookmarks (XEP-0402 + XEP-0048 PEP).** They are fetched after the account disco. If the native node is empty or missing and the server has no `#compat`, the legacy node is read instead. Autojoin rooms are joined through `serv_join_chat()` with the stored nick and password once the stream is connected. The account setting `bookmarks_autojoin` (default TRUE; not written, and not shown in the account editor) turns this off. The `+notify` PEP handlers apply changes made by other clients. The room list and extended-menu integration was skipped.
- **Behaviour without message-meta** (GTK 2 UI, other UIs):
  - outgoing messages carry `<origin-id/>`, and their id is a UUID;
  - carbons are shown, sent ones as `SEND|REMOTE_SEND`;
  - the account catch-up only learns the position (a reconnect in the same process does catch up);
  - rooms that were caught up once in this process rejoin with `maxstanzas=0` plus a MAM catch-up;
  - bookmarks autojoin;
  - self-ping runs;
  - fallback text is kept, and no metadata signals are emitted.
- **Tests:** `scripts/tests/jabber-m8/run.sh` (190 checks) links the prefix's libpurple and libjabber and runs a null UI with a fake connected stream, without network or accounts. It covers the pure helpers (`jabber_message_get_stanza_id/origin_id`, `jabber_fallback_*`, `jabber_carbons_unwrap`, `jabber_mam_unwrap`/`query_build`/`page_done`, `jabber_chat_selfping_decide/classify`, and bookmark (de)serialization). It also covers end-to-end stanza flows: carbons, MAM paging, expiry and dedup, `discard`, fallback stripping, the send path, MUC join and catch-up, `mam-fetch-older`, and bookmark autojoin and IPC. The GTK 2 `pidgin -n` ran headless for 20 s with the new libjabber and loaded it cleanly.
- **Open issues / follow-ups:**
  - The message-semantics XEPs (0184, 0333, 0308 emission, 0444, 0424/0425, and the send-* IPC commands) are the next agent's work. Until 0444 is emitted, a message-meta UI loses reaction-only messages, because their whole-body fallback is stripped. *(Done, including the reaction-only fix: see* Landed (message semantics) *below.)*
  - `jabber-receiving-message` is emitted for the outer stanza only. The OMEMO plugin has to decrypt `<encrypted/>` inside `<forwarded/>` itself, for carbons and MAM.
  - Offline messages that the server delivers without a `<stanza-id/>` can be shown again by the catch-up (the dedup key is missing). Prosody adds it.
  - MUC history on a first join, and on a non-MAM rejoin (`since`), can repeat lines that have no stanza-id.
  - The IQ layer has no timeouts, so an unanswered MAM query only ends with the stream.
Landed (OMEMO):

- **Plugin** `libpurple/plugins/omemo/` (`omemo.c` glue, `omemo-xmpp.c` stanzas, `omemo-store.c` SQLite, `omemo-crypto.c` libgcrypt), id `core-omemo`, installed as `<prefix>/lib/purple-2/omemo.so`. Built by default; `configure --disable-omemo` turns it off. Only the plugin links libomemo-c, libgcrypt and sqlite3; `libpurple.so` gains no dependency and the ABI gate passes.
- **No prpl changes.** It hooks `jabber-receiving-xmlnode` (decrypt in place, including carbons from our own bare JID and MAM `<result><forwarded>`; MUC occupant tracking; device list PEP events), `jabber-sending-xmlnode` (encrypt; our own IQs are sent by emitting it, as `jabber_send()` does), `jabber-receiving-iq` (consumes the results of its own IQs, ids `omemo…`), and the prpl's `add_feature` IPC for `eu.siacs.conversations.axolotl.devicelist+notify`. The core `sending-im-msg`/`sending-chat-msg` signals hold a message back while device lists and bundles are fetched, then send it.
- **Protocol:** legacy OMEMO 0.3 (`eu.siacs.conversations.axolotl`). Our device id is merged into the existing device list (other ids are never dropped; a transient fetch error publishes nothing). The bundle (signed pre-key, identity key, 100 pre-keys) is published with `publish-options` `access_model=open`, retried without the options on error, and republished when fewer than 20 pre-keys are left. Messages: AES-128-GCM payload, 12-byte IV, key‖tag in each `<key rid prekey>`; `<store xmlns='urn:xmpp:hints'/>`, EME (`urn:xmpp:eme:0`) and the fallback body are added; other children (receipts request, chat states, origin-id, …) pass through; XHTML-IM is removed. Messages go to trusted and verified devices of the contact and our own other devices; undecided and untrusted devices get no key. If no trusted device of the contact is left, the message is not sent (the body is stripped from the stanza as a last line of defence). MUC: members-only + non-anonymous rooms only (checked by disco#info), recipients are the occupants' real JIDs from presence; our own echo is shown from an in-memory sent cache.
- **Incoming:** the `<body>` is replaced with the plaintext so the prpl displays and logs it. Key-only messages lose their body and are not shown. Failures become `[OMEMO: could not decrypt (reason)]`. Messages sent by this very device (no key for us) show the cached plaintext or `[OMEMO: sent from this device; the text is in the local log]`.
- **IPC** on the plugin (`purple_plugins_find_with_id("core-omemo")`):
  - `omemo-list-devices(PurpleAccount *, const char *jid /* NULL = own */)` → `GList *` of `GHashTable *` (string→string: `device-id`, `fingerprint`, `trust`, `active`, `session`, `own`); free with `g_list_free_full(l, (GDestroyNotify)g_hash_table_unref)`.
  - `omemo-set-trust(account, jid, guint device_id, "undecided"|"trusted"|"verified"|"untrusted")` → gboolean.
  - `omemo-own-fingerprint(account)` → newly allocated string (8 groups of 8 hex digits).
  - `omemo-conv-enabled(account, conv name)` → gboolean; `omemo-conv-set-enabled(account, conv name, gboolean)`.
  - `omemo-decrypt-url(const char *aesgcm_url, GBytes *ciphertext)` → `GByteArray *` plaintext or NULL.
  - `omemo-encrypt-file(const char *path, char **fragment)` → path of a temporary ciphertext file (to upload and unlink); `*fragment` is the hex `<iv 12><key 32>` for `aesgcm://…#fragment`. 16-byte IVs are accepted on decryption.
- **Signals** on the plugin: `omemo-new-device (PurpleAccount *, const char *jid, guint device_id, const char *fingerprint)`, emitted when a device's identity key is first learnt; `omemo-conv-state-changed (PurpleAccount *, const char *conv_name, gboolean enabled, gboolean all_trusted)`.
- **Settings:** blist node (buddy or chat) bool setting `omemo-enabled` (explicit on/off per conversation; without a blist node the choice is kept in memory). Prefs `/plugins/core/omemo/tofu` (default on: blind trust before verification, i.e. new devices are trusted until a device of that JID is verified) and `/plugins/core/omemo/encrypt_by_default` (default **off**; when on, conversations with an OMEMO-capable contact or a private room are encrypted unless disabled).
- **GTK 2 UI meanwhile:** `/omemo [status|on|off|own|refresh|trust [jid] <device> <state>]` in XMPP conversations, and the plugin action *OMEMO fingerprints…* (formatted list + trust request).
- **`<profile>/pidgin4/omemo.db`** (SQLite, WAL, mode 0600), every table keyed by `account` = our bare JID: `own` (device id, registration id, identity key pair, signed pre-key id, next pre-key id), `prekeys`, `signed_prekeys`, `sessions` (jid, device id, record), `identities` (jid, device id, identity key, `trust` = undecided/trusted/verified/untrusted, first seen), `devices` (jid, device id, `active`, last seen), `device_lists` (which JIDs' lists are known), `meta` (`schema_version` = 1).
- **Verified** (scratchpad tests against the prefix, `G_DEBUG=fatal-criticals`, also under ASan/UBSan: no errors, no leaks in plugin code): store round trips; a two-device loopback (bundle → session, pre-key message then normal messages both ways, replay rejected, tampered bundle signature rejected, BTBV); AES-GCM against the GCM spec vectors and OpenSSL, aesgcm files both ways with 12- and 16-byte IVs; the loaded plugin end to end with a fake connection (pre-key message, carbon, forged carbon ignored, MAM result, PEP device list, encrypt on send and decrypt by the peer, untrusted → no plaintext leak, own device list merge and publish-options fallback, bundle fetch → session, MUC disco + encrypt + echo + member message, `/omemo`); the GTK 2 pidgin from the prefix loads and unloads it headless (`-n`, Xvfb, private profile) without warnings.
- **Open:**
  - Not tested against a live server or another client (no Prosody installed); the interop check with Conversations/Dino/Gajim in *Verification* is still to do.
  - An archived copy of a message already decrypted live can't be decrypted again (message keys are one-shot) and comes out as `[OMEMO: could not decrypt (duplicate message)]`: MAM dedup by stanza-id (M4 message index) must drop it before it is logged.
  - MUC recipients are the occupants seen in presence; offline members of a members-only room are not included (no affiliation-list query yet).
  - The signed pre-key is never rotated. No empty "key transport" message is sent after receiving a pre-key message; the session completes with the next reply.
  - The `+notify` caps feature stays advertised after the plugin is unloaded until restart (the prpl has no `remove_feature` IPC).
  - Messages held back while keys are fetched are written to the conversation when sent; `sent-im-msg` is not emitted for them.
  - `libpurple/protocols/*/Makefile.in` were not regenerated on this branch (they only lack the unused `OMEMO_*` substitutions).
**Landed (auth/connection)** — jabber prpl only, no libpurple API change (ABI gate passes):
- **SCRAM** (`auth_scram.c`, now on GHmac/GChecksum): SCRAM-SHA-1/-256/-512 and their `-PLUS` variants. Preference: SHA-512-PLUS > SHA-256-PLUS > SHA-1-PLUS > SHA-512 > SHA-256 > SHA-1 > DIGEST-MD5 > PLAIN (PLAIN in clear still needs `auth_plain_in_clear`). Channel binding via `jabber_auth_get_channel_binding()`: `tls-exporter` (TLS 1.3), else `tls-server-end-point`, restricted to the XEP-0440 `<sasl-channel-binding/>` list when the server sends one. GS2 header `p=<type>,,` / `y,,` (we could bind, server offers no -PLUS) / `n,,`. A rejected -PLUS attempt is retried once on the same stream without binding (`n,,`), for servers that support only other binding types and don't advertise XEP-0440 (e.g. Prosody on TLS 1.2 offers only `tls-unique`, which ssl-nss can't do).
- **SASL2 + Bind2 + FAST** (`sasl2.c`): used when the stream is encrypted and `<authentication xmlns='urn:xmpp:sasl:2'>` offers Bind2; otherwise legacy SASL + restart + RFC 6120 bind. One `<authenticate>` carries the initial response, `<user-agent id>` (software `Pidgin`, device = short host name), Bind2 `<tag>pidgin4</tag>` with inline carbons enable and SM `<enable resume='true'/>` when listed, and SM `<resume/>` when a dropped session exists. FAST: HT-SHA-256-EXPR/-ENDP/-NONE (draft-schmaus-kitten-sasl-ht, responder proof verified); a token is requested on password logins, used first when stored and usable (mechanism still offered, binding available, not expiring within 60 s), rotated tokens are stored, a rejected token is deleted and the password is used on the same stream. `<continue/>` (2FA etc.) fails cleanly with a debug message. The stream header now carries `from=` once encrypted (RFC 6120 4.7.1; Prosody only offers FAST then). FAST tokens and PLAIN/HT initial responses are kept out of the debug log.
  - kv keys (`jabber_kv_load/store`, never `accounts.xml`): `sasl2/user-agent-id` (UUID), `fast/token`, `fast/mechanism`, `fast/expiry` (ISO 8601). Without a UI store (GTK 2) there is no user-agent id and no FAST.
  - `JabberStream.carbons_enabled_inline` is TRUE when carbons were enabled through Bind2; `carbons.c` can skip its enable IQ then.
- **Direct TLS (XEP-0368)**: `_xmpps-client._tcp` and `_xmpp-client._tcp` are looked up in parallel; direct TLS targets go first (TLS on connect, certificate checked against the XMPP domain), then STARTTLS targets, then the domain on `port`. A failed TCP connect, a failed handshake, or a direct TLS port that doesn't answer with an XMPP stream moves on to the next target; a rejected certificate doesn't. `connect_server` and `old_ssl` behave as before. No ALPN (`xmpp-client`) is sent: `purple_ssl_*` has no way to ask for it.
- **Stream management (XEP-0198)** (`stream_management.c`): `<enable resume='true'/>`; per-account (bare JID) session state in a process-lifetime hash: SM-ID, bound JID, `max`, h counters and a bounded (1000) queue of unacked stanzas. A non-fatal connection error (network drop, ping timeout; seen through `connection-error`) leaves the stream unclosed so the server hibernates the session; the next connection sends `<resume/>` (inline in SASL2, or instead of bind), re-sends what the server didn't ack (raw, without re-running `jabber-sending-xmlnode`), then sends unavailable presence and re-runs disco/roster/initial presence, since libpurple dropped buddy presence and chats with the old connection. `<failed/>` → normal bind, and only the queued `<message/>`s of the old session are re-sent. A clean sign-off forgets the session.
- **CSI (XEP-0352)** (`csi.c`): `<inactive/>` when idle, away/extended away, or the UI says nobody is looking; `<active/>` otherwise; only sent on change and when advertised (stream feature or Bind2 inline). UI entry points for M6: `jabber_csi_set_active(js, gboolean)` and the IPC command `csi-set-active(PurpleAccount *, gboolean) → gboolean` on the jabber plugin.
- No new account settings; `connection_security`, `auth_plain_in_clear`, `port`, `connect_server` keep their meanings.
- Verified end to end against a local Prosody 13.0.6 (+ community `mod_sasl2*`) with a libpurple test client, on TLS 1.3 and on a TLS 1.2-only direct TLS port: direct TLS and each fallback, SASL2 + Bind2 with SCRAM-SHA-256-PLUS (`tls-exporter`), FAST issue/use/rejection, SM resume after a dropped socket (SASL2 inline and legacy `<resume/>`, message sent during the gap delivered), legacy SASL SCRAM-SHA-1-PLUS, the -PLUS retry, CSI, no-kv-store mode. Unit-tested: RFC 5802/7677 vectors (and SHA-512, -PLUS `c=`) checked against a Python reference, HT hashes against `openssl dgst -hmac`, FAST choice, SM counters/queue. Not covered by a live server: SCRAM-SHA-512 (Prosody has none), HT-SHA-256-ENDP and SCRAM `tls-server-end-point` (Prosody offers neither), `<continue/>`.

**Landed (message semantics), 2026-09-22.** XMPP only, on top of the protocol core above. All in `libpurple/protocols/jabber/` (`receipts.c`, `correction.c`, `reactions.c`, `retraction.c`, `styling.c`, `displayed.c`, plus `message.c`/`chat.c`). No libpurple API change; the ABI gate passes. The M4b UI implements against this list.

- **Gate.** Everything below emits signals only when `jabber_ui_supports_message_meta()` is TRUE. Otherwise (the GTK 2 UI), or when every handler returns FALSE, the prpl writes the text fallback given for each item. When a handler returns TRUE, the prpl writes nothing and the UI owns the log line (contract rule 7).
- **Event conventions.** `conv_name` and `sender` are the same as in `receiving-message-meta`:
  - IMs: `conv_name` is the sender's full JID and `sender` the same;
  - rooms: `conv_name` is the room JID and `sender` the nick;
  - events we caused from another device (sent carbons, our own archive): `conv_name` is the counterpart and `sender` our own full JID;
  - "displayed elsewhere" receipts use our own **bare** JID as `sender`.
- **Receipts (XEP-0184).**
  - Outgoing IMs carry `<request/>`, unless the resource's caps are known and lack `urn:xmpp:receipts`. Outgoing IMs and room messages carry `<markable/>`. Both only with a message-meta UI.
  - Requests are answered (`<received id=''/>` + `<store/>`) on live 1:1 messages from contacts in the buddy list, whatever the UI. They aren't answered for rooms, carbons or MAM.
  - An incoming `<received/>` becomes `message-receipt(account, conv, id, "delivered", sender)`. Not in rooms. Fallback: nothing (as before).
- **Chat markers (XEP-0333).**
  - `<received/>` → `"delivered"`; `<displayed/>` and `<acknowledged/>` → `"displayed"`. In rooms, too.
  - Our own `<displayed/>` from another device (sent carbon, or reflected from our nick in a room) → `message-receipt(…, "displayed", <own bare JID>)`.
  - Incoming `<markable/>` → meta key `markable` = `1`.
  - IPC `gboolean send-marker(PurpleAccount *account, const char *conv_name, const char *message_id, const char *marker)`. `marker` is `displayed` (the default for NULL/""), `received` or `acknowledged`. Sends `<marker id=''/>` + `<store/>` to `conv_name`: the peer's JID as given, or type groupchat to the room. It returns FALSE for rooms not listing `muc_nonanonymous` in disco#info. In rooms `message_id` should be the room's stanza-id (`server-id`).
- **Corrections (XEP-0308).**
  - An incoming `<replace id=''/>` with a body → `message-corrected(account, conv, target_id, new_id, new_body, sender)`. `new_id` is the correction's own `<message id=''>`, and `new_body` is libpurple markup, as `write_conv` would get it.
  - TRUE: nothing is written or logged. Otherwise the message is written normally, prefixed `edit: ` (translatable, `JABBER_CORRECTION_PREFIX`), and its `receiving-message-meta` carries `correction-of`. No `PURPLE_MESSAGE_*` flag marks it: 2.14 has none that fits, and none was added (the ABI is additive, but GTK 2 would ignore a new bit).
  - In rooms the prpl keeps a bounded (512) per-room map of message id / origin-id / server-id → occupant-id. A correction or retraction whose target is known to come from a different occupant-id is dropped, so nick changes don't break matching and nick takeovers can't fake it. Without occupant-id the UI has to compare nicks.
  - IPC `gboolean send-correction(PurpleAccount *account, const char *conv_name, const char *target_id, const char *new_body)`. `new_body` is **plain text**, already 0393-styled. It sends the body + `<replace id=target/>` + a fresh id/origin-id (+ request/markable), then emits `sending-message-meta` with `correction-of`.
    - 1:1: **no write follows** that meta. The prpl then emits `message-corrected(…, target, new_id, markup, <own full JID>)`; if that isn't handled, it writes `edit: …` as a SEND message.
    - Rooms: the room's reflection comes back through the incoming path.
- **Reactions (XEP-0444).**
  - A message with `<reactions id=''>` is always a reaction: its body (if any) is treated as the fallback, and the message is never written as-is. This fixes the core's open issue: reaction-only messages were dropped once their fallback body was stripped.
  - The prpl keeps each sender's last set per (conversation, target) for the session. The identity is the bare JID for IMs, the full JID for room PMs, and the occupant-id (else the nick) in rooms. It emits `message-reaction(account, conv, target, emoji, sender, add)` for each removal, then each addition. A repeated set emits nothing. In rooms the target is whatever the sender used (normally the room's stanza-id).
  - Fallback, if any emission is unhandled or there's no meta UI: a system line `X reacted 👍 🎉 to a message`, or `X removed the reaction 👍` when there are only removals.
  - Sets are cleaned up: trimmed, unique, ≤ 32 entries of ≤ 64 bytes.
  - IPC `gboolean send-reaction(PurpleAccount *account, const char *conv_name, const char *target_id, const char *emoji_list)`. `emoji_list` is the **complete** new set, space-separated; `""` removes all. It sends `<reactions>` + `<store/>` + a fallback body (the emoji) with `<fallback for='urn:xmpp:reactions:0'/>`; an empty set has no body. In 1:1 the diff against our last sent set is emitted locally as `message-reaction` with our own full JID (text fallback as above). Rooms: the reflection does it.
- **Replies (XEP-0461).** Incoming as in the core (`reply-to`, `reply-to-sender`, quote stripped).
  - IPC `gboolean send-reply(PurpleAccount *account, const char *conv_name, const char *reply_to_id, const char *reply_to_jid, const char *quoted_text, const char *body)`. `reply_to_jid` and `quoted_text` may be NULL; `body` is plain text. The UI supplies the quoted text; the prpl doesn't look anything up.
  - Each line of `quoted_text` becomes `> line\n`, followed by `body`. The quote is marked with `<fallback for='urn:xmpp:reply:0'><body start='0' end='N'/>` (code points), plus `<reply to='' id=''/>`.
  - It emits `sending-message-meta` with `reply-to`/`reply-to-sender`. In 1:1 it then writes `body` alone (without the quote) as a SEND message; rooms: the reflection.
- **Retraction (XEP-0424) and moderation (XEP-0425).**
  - Incoming `<retract xmlns='urn:xmpp:message-retract:1' id=''/>`, or the 0.2/0.3 form `<apply-to xmlns='urn:xmpp:fasten:0' id=''><retract xmlns='…:0'/></apply-to>` (`:1` wins if both are present), → `message-retracted(account, conv, target, sender, NULL)`. Room retractions are occupant-id checked.
  - Moderation (`<retract><moderated xmlns='urn:xmpp:message-moderate:1' by=''/><reason/></retract>`, or the `:0` `<apply-to><moderated>` form) is accepted **only from the room's bare JID**. It becomes `message-retracted(…, target, <moderator nick, else the by JID>, reason-or-NULL)`.
  - Retraction messages are always consumed. Fallbacks: `X retracted a message`; `mod removed a message[: reason]` (`A moderator` when there's no `by`). `<retracted/>` tombstones from archives are ignored.
  - IPC `gboolean send-retraction(PurpleAccount *account, const char *conv_name, const char *target_id)` sends `:1` and the legacy `:0` form together, with a fallback body + `<fallback for='urn:xmpp:message-retract:1'/>` + `<store/>`. In 1:1 it's reported locally as `message-retracted` with our own full JID (fallback line otherwise). Rooms: reflection. In rooms it is only meaningful for our own messages, with `target_id` the stanza-id.
  - IPC `gboolean send-moderation(PurpleAccount *account, const char *room_jid, const char *target_id, const char *reason)` sends the 0425 IQ to the room: `<moderate xmlns='urn:xmpp:message-moderate:1' id=''><retract xmlns='urn:xmpp:message-retract:1'/><reason/></moderate>`, or the `:0` `<apply-to>` form if the room only advertises that. It returns FALSE for non-room targets. An error result is written into the room as an ERROR line. The UI decides when to offer it (affiliation/role).
- **Styling (XEP-0393).**
  - Neither send path sends XHTML-IM any more. `send_im`/`send_chat` convert the UI's markup to a plain body:
    - `<b>`/`<strong>` → `*`, `<i>`/`<em>` → `_`, `<s>`/`<strike>`/`<del>` → `~`, `<code>`/`<tt>` → `` ` ``;
    - `<pre>` → a ```` ``` ```` fence;
    - `span style` bold/italic/line-through map the same way;
    - everything else is stripped as by `purple_markup_strip_html` (entities, `<br>`, links as "text (url)").

    Markers hug non-whitespace, nested duplicates merge, and empty spans vanish.
  - **pidgin4 contract:** pass libpurple markup as usual, i.e. HTML-escape the plain text (`purple_strdup_withhtml` for newlines). 0393 markers typed or serialized by the entry pass through unchanged.
  - Outgoing custom smileys (BoB `<img>` in XHTML-IM) are gone with XHTML-IM. Incoming XHTML-IM and custom smileys are still rendered.
  - Incoming `<unstyled xmlns='urn:xmpp:styling:0'/>` → meta key `unstyled` = `1`.
  - Jabber connections now have `PURPLE_CONNECTION_HTML | ALLOW_CUSTOM_SMILEY | NO_FONTSIZE | NO_BGCOLOR | NO_URLDESC | NO_IMAGES`. These are runtime flags, not ABI. GTK 2 therefore hides font size, background colour, link text and image insertion for XMPP. Bold/italic/underline/strike stay (underline is stripped).
- **Displayed-state sync (XEP-0490).**
  - The `urn:xmpp:mds:displayed:0` PEP handler advertises `+notify`.
  - Notifications from our own bare JID, and an items fetch on `signed-on` (message-meta UI only), become `message-receipt(account, <item id: the conversation's bare JID>, <stanza-id>, "displayed", <own bare JID>)`. The id is a `server-id` (our archive's for 1:1, the room's for rooms). The UI must match `conv_name` by bare JID.
  - IPC `gboolean mds-publish(PurpleAccount *account, const char *conv_name, const char *stanza_id)` publishes `<displayed><stanza-id id='' by=''/></displayed>` into item `<bare conv JID>`. `by` is our bare JID for 1:1 and the room for rooms. Publish-options: `persist_items=true`, `max_items=max`, `send_last_published_item=never`, `access_model=whitelist`. It returns FALSE without PEP.
- **IPC common rules.** All commands live on the prpl-jabber plugin. Call them with `GPOINTER_TO_INT(purple_plugin_ipc_call(prpl, "send-…", NULL, …))`: the gboolean is the **return value**, while the `ok` out-parameter only says whether the call was dispatched.
  - They return FALSE for a NULL/empty id or a missing account, a non-XMPP account, or an account that isn't connected.
  - `conv_name` is a room if it is the bare JID of a joined room; anything else (including `room@server/nick` PMs) is a 1:1 target.
  - Every outgoing stanza gets a UUID id that is also its origin-id, remembered so that the archive copy isn't shown again.
- **Disco features added:** `urn:xmpp:receipts`, `urn:xmpp:chat-markers:0`, `urn:xmpp:message-correct:0`, `urn:xmpp:reactions:0`, `urn:xmpp:message-retract:1`, `urn:xmpp:message-retract:0`, `urn:xmpp:reply:0`, `urn:xmpp:styling:0`, `urn:xmpp:mds:displayed:0+notify`. They are advertised regardless of the UI, since each has a readable fallback.
- **Meta keys added** to `receiving-message-meta`: `markable`, `unstyled`. Added to `sending-message-meta`: `correction-of`, `reply-to`, `reply-to-sender` (IPC sends).
- **Tests:** `scripts/tests/jabber-m8/run.sh` now has 405 checks (190 before), all passing against the prefix build. They cover every item above with and without a message-meta UI, and with handlers that do and don't render: 0184 answering/privacy/carbons/send path, 0333 incl. anonymous rooms, 0308 incl. the occupant-id check, 0444 diffs, reaction-only messages and fallback, 0461 serialization re-stripped with the 0428 helpers, 0424 in both namespaces + 0425 in both + spoofing + error path, 0393 conversion of GTK 2 HTML, 0490 notify/fetch/publish, IPC argument handling, features and connection flags. The GTK 2 `pidgin -n` from the prefix ran 20 s under Xvfb and loaded `libxmpp.so` cleanly. valgrind isn't installed; the tests pass under `MALLOC_CHECK_=3 MALLOC_PERTURB_`.
- **Open issues / follow-ups:**
  - The HTTP-upload URL message still bypasses `jabber_message_send()`, so it has no receipt request or `<markable/>` (and no `sending-message-meta`).
  - Reaction diffs are per process. After a restart, the first set seen from a sender is reported as all-additions: the UI should treat an add it already has as a no-op, and a set arriving from a scroll-back (`mam-query=older`) page may be older than the one shown.
  - When the UI renders reactions/corrections/retractions natively, the HTML log gets no line. The UI must write its own readable line (e.g. `purple_conversation_write(…, PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LINKIFY)`), as rule 7 requires. The same holds for reply quotes, which are stripped for message-meta UIs in both directions.
  - The prpl doesn't check that a 1:1 correction/retraction comes from the original sender (it keeps no message store). The UI must compare the bare JID of `sender` with the original's.
  - 0490 is only fetched on `signed-on`. If PEP isn't ready yet, the error is ignored and the next notification brings the state.
  - The 0184/0333 requests are only added for a message-meta UI, so GTK 2 users don't get receipts they can't show.

### M9: Discord and Steam plugin integration (patches to the user's forks; needs M4 + M8)
Constraint: every change must keep `libdiscord.so` and `libsteam.so` loadable and fully working in the stock Pidgin 2.14.14. They use only symbols from the original ABI, gate new behaviour on the `message-meta` ui_info key, and emit signals only when it's present. Anything that can't be done that way is dropped.

- The plugins keep building against the **system** `purple.pc` (stock 2.14 headers). The signal/IPC design needs no new headers, so there is no way to link a new symbol by accident.
- Gate: every undefined `purple_*`/`serv_*` symbol in each `.so` must be exported by `/usr/lib64/libpurple.so.0.14.14`:
  ```sh
  comm -23 <(nm -D --undefined-only libdiscord.so | awk '{print $2}' | sort) \
           <(nm -D --defined-only /usr/lib64/libpurple.so.0.14.14 | awk '{print $3}' | sort) | grep -E '^(purple_|serv_)'   # must print nothing
  ```
- A load test in the system Pidgin comes before every plugin commit.

- **Discord native metadata** (`~/purple-discord`):
  - message ids and reply targets go through `receiving-message-meta`;
  - edits emit `message-corrected` instead of `EDIT:` lines;
  - deletes emit `message-retracted`;
  - `MESSAGE_REACTION_ADD`/`REMOVE` emit `message-reaction` instead of the text line.
  - It registers the `send-correction`/`send-reaction`/`send-retraction`/`send-reply` IPC commands, so editing, reacting and replying in pidgin4 go back to Discord.
  - On stock Pidgin, today's text output is unchanged.
  - Threads stay as they are.
- **Remote inline images**: the pidgin4 loader (M4) renders Discord's `<img src=cdn…>` custom emoji and attachments. The plugin needs no change, but its own imgstore download path can be skipped when the ui_info key is set.
- **Steam rich presence** (`~/pidgin-opensteamworks`): the plugin publishes `game` (name) and `game_app_id` as status attributes on the existing status type, which is plain 2.14 API. pidgin4 shows them as a secondary line or emblem on the buddy row and in the tooltip. Stock Pidgin ignores the extra attributes.
- **Request-UI coverage**: the M2 verification includes Discord QR login, Steam Guard (code and mobile-approve) and captcha prompts.

## Critical files
- **Build:** `configure.ac`; new `pidgin4/meson.build`, `pidgin4/pidgin-internal.h`, `pidgin4/resources/*.gresource.xml`.
- **Core UI:** `pidgin4/gtkmain.c`, `gtkutils.c`, `gtkblist.c`, `gtkconv.c`, `gtkstatusbox.c`, `gtkrequest.c`, `gtknotify.c`, `gtkaccount.c`, `gtkprefs.c`, `gtkdocklet*.c`, `gtkidle.c`, `pidginstock.c`.
- **New:** `pidgin4/pidginmarkup.c`, `pidginmessage.c`, `pidginmessageview.c`, `pidgincomposeentry.c`, `pidginrichlabel.c`, `pidginmenu.c` (PurpleMenuAction→GMenuModel), `pidginsni.c`, `pidginidle-wayland.c`.
- **Removed from the new UI:** `gtkimhtml*.c`, `gtksourceundomanager.c`, `gtksourceiter.c`, `gtksession.c`, `gtkcellrendererexpander.c`, `gtkmenutray.c`, `getopt*.c`, `gtkmedia.c`, `gtkwhiteboard.c`.
- **New (UI):** `pidgin4/pidginmessageindex.c`, `pidginimageloader.c`, `pidginsound.c` (GSound).
- **libpurple:** `network.c`, `proxy.c`, `dnsquery.c`, `dnssrv.c`, `sslconn.[ch]` + `plugins/ssl/ssl-nss.c` (channel binding), `conversation.c` (new signal registrations only), `protocols/irc/{irc,msgs,parse}.c` + new `protocols/irc/sasl.c`, `protocols/jabber/{message,jabber,auth_scram,stream_management,chat,si}.c` + new `carbons.c`, `mam.c`, `httpupload.c`, `receipts.c`, `correction.c`, `reactions.c`, `bookmarks.c`, `styling.c`, `sasl2.c`, `displayed.c`, `retraction.c`; new `libpurple/plugins/omemo/`.
- **Out of tree:** `~/purple-discord/libdiscord.c`, `~/pidgin-opensteamworks/steam-mobile/steam_cm.c`/`libsteam.c` (M9).

## Verification
- **ABI:** after every libpurple change, the exported-symbol diff against `/usr/lib64/libpurple.so.0.14.14` shows additions only. Discord and Steam accounts connect in both the GTK 2 and pidgin4 builds.
- **Tests:** libpurple `make check`; Meson unit tests for PidginMarkup (HTML subset ↔ Pango/tags round-trips, including WBFO and point sizes) and the dbusmenu exporter.
- **Runtime:** runs use `-c ~/.purple-gtk4`, `G_DEBUG=fatal-criticals`, and optionally an ASan build. `GTK_DEBUG=interactive` is used for layout checks.
- **Per milestone:**
  - M2: all accounts sign in and a password prompt works. Discord QR login and Steam Guard prompts work.
  - M3: buddy list shows presence; DnD reorder works; status changes.
  - M4: IRC/XMPP/Discord chats send and receive formatted text, smileys, images, find, scrollback, history plugin, markerline.
  - M6: the tray appears in Waybar with a working menu; auto-away triggers under Sway (idle N min) and GNOME; notifications and sounds work under both.
  - M9: the patched plugins load in the **system** Pidgin 2.14.14 with unchanged behaviour. In pidgin4, Discord reactions, edits and replies render natively and round-trip; Steam game presence shows.
  - Profile: the cutover-gate round-trip (`check-profile-compat.sh`) passes at the end of M2, M3, M4, M5, M8 and M9.
- **XMPP interop:** test against a Dino or Gajim client and Conversations on a phone, using an existing account or a local Prosody test server. Check:
  - carbons both directions;
  - MAM catch-up after being offline;
  - upload of an image both directions;
  - OMEMO 1:1 exchange + fingerprint verification;
  - receipts/markers, edit, reaction, reply, retraction rendering and sending;
  - 0393 styling both directions; 0490 read-state sync with Conversations;
  - SASL2/FAST login and SCRAM-SHA-256-PLUS against Prosody 13.
- **IRC:** TLS + SASL PLAIN login on a network that supports it (e.g. Libera or QuakeNet); server-time on scrollback; echo-message.
- **Sessions:** a full-day daily-driver run under Sway, and a smoke test under GNOME 49.

## Open items
- **D-Bus.** The plan builds libpurple with D-Bus disabled, which removes `purple-remote` and any scripts that rely on it. Re-enable it if that is needed.
- **GNOME tray.** A tray icon under GNOME requires the AppIndicator/KStatusNotifierItem Shell extension, which is not installed.
- **Log index backfill.** Indexing 2.6 GB of HTML logs runs once, in the background, and can be resumed. Expect it to take minutes, and the database to be several hundred MB.

See also `doc/GTK4-MIGRATION.md` for the general survey of GTK 2 → GTK 4 API changes this plan is based on.
