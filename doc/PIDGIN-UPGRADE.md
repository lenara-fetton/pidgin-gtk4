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
- **M3**
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

### M5: Remaining windows
Prefs, pounces, saved statuses, log viewer, privacy, room list, certificate manager, file transfers, smiley manager, plugins dialog, about/build info, join chat.
- Lists become `GtkListView`/`GtkColumnView`.
- Dialogs become async `GtkAlertDialog`/`GtkFileDialog`.
- Whiteboard and media UIs are dropped.

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
