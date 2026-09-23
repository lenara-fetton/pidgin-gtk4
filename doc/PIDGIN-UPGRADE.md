# Pidgin 2.14.14 → GTK 4 port, library modernization, modern XMPP

## Context

Pidgin 2.14.14 (`pidgin/`, ~101k lines in 166 files) targets GTK 2.10–2.18. The goal is a personal build that runs natively on Wayland: primarily Sway 1.12 with Waybar, occasionally GNOME 49. The build:

- uses GTK 4 (4.22.5 installed), not libadwaita;
- replaces outdated libraries with current ones;
- adds the XMPP features modern clients rely on: carbons, MAM, HTTP upload, OMEMO, receipts, markers, corrections, and reactions.

Decisions made:

- Go straight to GTK 4, skipping GTK 3.
- Plain GTK 4.
- Replace GtkIMHtml instead of porting it.
- Build the new UI with Meson.
- XMPP work is part of this effort.

**Hard constraint:** the user's third-party protocol plugins, `~/.purple/plugins/libdiscord.so` and `libsteam.so`, link against `libpurple.so.0` (2.14.x ABI). They use the ssl, dnsquery, proxy, and util_fetch_url APIs, and no D-Bus symbols. libpurple changes must therefore be **ABI-compatible and additive only**: no removed symbols, no struct layout changes, no changed signatures.

Environment facts, verified:

- GLib 2.88, libspelling 0.4.10, GtkSourceView 5.20, wayland-protocols 1.49, libsoup 3.6, libgcrypt 1.12, sqlite 3.53, meson 1.12. `libomemo-c` is **not installed**.
- The installed Pidgin is built without GStreamer, so voice/video isn't used today.
- Pidgin UI plugins in use: cap, history, markerline, notify, timestamp_format.
- libpurple plugins in use: autoaccept, joinpart, psychic, statenotify, ssl-nss.
- Active protocols: 7 XMPP accounts, 10 IRC, Discord, Steam.

## Architecture

- **The tree stays autotools for libpurple.** It is configured with `--disable-consoleui --disable-dbus --disable-vv`, dynamic prpls `irc,jabber` only, and no Perl/Tcl/Mono. It installs into a **private prefix**, e.g. `~/.local/pidgin4`, so the system `net-im/pidgin` is never touched.
- **The existing `pidgin/` (GTK 2) stays buildable** by autotools against the modified libpurple. That keeps a working daily driver during the port and lets the XMPP work be tested before the GTK 4 UI is ready. It is deleted at the end.
- **New `pidgin4/` is a standalone Meson project.** It starts as a copy of `pidgin/` and is ported file by file. It finds libpurple through that prefix's `purple.pc` via `PKG_CONFIG_PATH`, links with an rpath into the prefix, and uses `export_dynamic: true`, which plugins need.
  - It provides its own `pidgin-internal.h`, replacing libpurple's uninstalled `internal.h` (72 includes): `_()`/`N_()`, `PURPLE_WEBSITE`, `BUF_LONG`, and the config.h include.
  - It generates `config.h` and `package_revision.h` equivalents from Meson.
  - It builds a GResource for pixmaps.
  - Translations come from the libpurple install, which already uses the shared `pidgin` gettext domain.
- **Testing always uses a copied profile** (`pidgin4 -c ~/.purple-gtk4`) so `~/.purple` is never mutated.
- **App ID:** `com.minowick.Pidgin4`. It is used for the `.desktop` file name (`com.minowick.Pidgin4.desktop`), the Wayland `app_id`, the GApplication D-Bus name, GNotification, the SNI item id, icon/metainfo names, and portals. The Sway rule `assign [class="Pidgin"] 2:chat` (`~/.config/sway/config:291`) needs an `app_id` equivalent for the native-Wayland build.

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
| Sounds | GStreamer playbin w/ gconf/esd sinks, custom command | `GtkMediaFile` (GTK's GStreamer media backend) plus the custom-command option | pidgin4 |
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

Deferred, optional: a GIO-TLS `ssl-gio` plugin to replace ssl-nss/gnutls, and moving custom ciphers onto GChecksum/GHmac. Neither is needed.

## Milestones

### M0: Repository baseline
- `.gitignore` for autotools output, baseline commit of the pristine tree, work branch.
- Script: configure/build/install libpurple (+ GTK 2 pidgin) into the private prefix; copy `~/.purple` → `~/.purple-gtk4`.
- Record this plan in `doc/GTK4-MIGRATION.md`, replacing the generic survey sections with the chosen path.

### M1: libpurple modernization (ABI-safe, testable with the GTK 2 UI)
- libidn2, GNetworkMonitor, GProxyResolver, GResolver-backed dnsquery/SRV, D-Bus off.
- ABI gate: `nm -D --defined-only` symbol list must be a superset of `/usr/lib64/libpurple.so.0.14.14`. Use `abidiff` if libabigail is installed.

### M2: `pidgin4/` skeleton: sign in and stay connected
- Meson project, `pidgin-internal.h`, GResource, `GtkApplication` startup that replaces `main()`/`gtk_main` in `gtkmain.c` and reuses its core-init order.
  - `gtkeventloop.c` is reused unchanged: it is GLib-only.
- Signal handling moves from the `socketpair` hack to `g_unix_signal_add`.
- Port the ops needed to connect: connection (`gtkconn.c`), request (`gtkrequest.c`, fully async, no `gtk_dialog_run`), notify (`gtknotify.c`), debug (plain `GtkTextView`, no IMHtml), account manager and editor (`gtkaccount.c`).
- Unported UiOps structs are left unset. libpurple tolerates NULL ops.
- Menus: a shared helper converts a `PurpleMenuAction` tree into `GMenuModel` + a per-menu `GSimpleActionGroup`. It replaces `pidgin_append_menu_action` (`gtkutils.c:1770`) and is used for blist-node, protocol, plugin, and conversation extended menus.

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
   - tags: B, I, U, S, SUB, SUP, FONT color/back/face/size/sml, SPAN style, A, IMG id/src, BR, HR, P, comments;
   - output as a Pango attribute list + inline-object list.

   It is also used in reverse to serialize entry tags to the HTML that `get_markup` produced, including the `USE_POINTSIZE` and whole-buffer-formatting (WBFO) cases. It is unit-tested.
2. **`PidginMessage`** is a GObject holding sender, alias, flags, time, HTML body, and the parsed form. It also has **metadata**: stanza/origin id, correction-of id, receipt/marker state, reactions, and reply-to. It lives in a `GListStore`.
3. **`PidginMessageView`** is a `GtkListView` of message rows used for conversations, the log viewer, and history. Rows show:
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
4. **`PidginComposeEntry`** is a GtkSourceView 5 `GtkTextView` with libspelling. It provides:
   - formatting tags for bold/italic/underline/strike/size/face/colors/links;
   - `setup_entry(features)` semantics;
   - Enter to send and `set_return_inserts_newline`;
   - a `message-send` signal (sendbutton/spellchk depend on it);
   - typing notifications from buffer changes;
   - a formatting toolbar replacing `gtkimhtmltoolbar.c` and a smiley picker popover.

   It is used by conversations, the prefs font preview, pounces, saved statuses, the status message, multiline request fields, and plugin prefs.
5. **`PidginRichLabel`** is a read-only `GtkLabel`/`GtkTextView` built on PidginMarkup. It is used for notify, userinfo, and about dialogs.

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
- **Notifications and attention.** `GNotification` for new messages (the notify plugin's option). `gtk_window_present` uses xdg-activation, which Sway marks urgent. The X11 `_PurpleUnseenCount` property is dropped; the count is shown in the SNI tooltip/title.

### M7: Plugins
- **Port the plugins in use:** history (MessageView API), markerline (marker item), timestamp_format (MessageView context-menu hook replaces the GtkTextView `populate_popup` emission hook), notify (event controllers, GNotification), cap (GtkGrid prefs).
- **Also port (cheap and useful):** convcolors, spellchk, sendbutton, gtkbuddynote, iconaway, relnot, timestamp, xmppconsole, xmppdisco.
- **Drop:** gestures, transparency, extplacement, ticker, pidginrc, themeedit, musicmessaging, gevolution, unity, vvconfig, Perl/Tcl/Mono loaders.
- **Plugin API:** the pidgin plugin API is not preserved. PidginConversation etc. get accessors where the ported plugins need them.

### M8: Modern XMPP (protocol work can start after M1; UI parts need M4)
**Additive libpurple API** for message metadata (ABI-safe: only new functions and new signals, no struct changes):

- `purple_conversation_message_meta_*`: the prpl attaches a meta object (stanza id, origin id, correction-of, reply-to) before `serv_got_im`/`serv_got_chat_in`. The UI reads it in `write_conv` and gets it on the sent path via a new `sending-message-meta` signal.
- New conversation signals: `message-corrected`, `message-reaction`, `message-receipt` (delivered/displayed), `message-retracted`.
- UI → prpl actions go through the existing `purple_plugin_ipc_*` mechanism on the jabber plugin: `send-correction`, `send-reaction`, `send-marker`. No new PurplePluginProtocolInfo fields.

Features in `libpurple/protocols/jabber/`:

- **Unique stanza ids (XEP-0359)**: the foundation for everything below.
- **Carbons (XEP-0280)**: sent carbons use the existing `PURPLE_MESSAGE_REMOTE_SEND` flag.
- **MAM (XEP-0313)**: catch-up on connect and scroll-back history. Deduplicate by stanza-id. Per-account state (last archive id) is kept in `~/.purple-gtk4/xmpp-mam.db` (sqlite). Fetched messages are logged normally.
- **HTTP File Upload (XEP-0363)** via libsoup 3. It is chosen in `send_file` when the server advertises it, with fallback to SI/Jingle. Received OOB/`aesgcm` URLs render as links, and images as inline previews.
- **Delivery receipts (0184), chat markers (0333), last message correction (0308), reactions (0444), replies (0461)**: each renders in PidginMessageView. Correction and reply are available from the entry (Up-arrow to edit the last message, reply from the row menu).
- **OMEMO**: a separate in-tree libpurple plugin, lurch-style. It hooks `jabber-receiving-xmlnode`/`jabber-sending-xmlnode`.
  - It needs **libomemo-c** (the maintained fork of libsignal-protocol-c used by Dino). Install it; it may need an overlay ebuild. Crypto uses libgcrypt.
  - It includes device list PEP, bundles, trust-on-first-use, and a fingerprint trust UI in pidgin4 (conversation info + a lock indicator).
  - Encrypted media (aesgcm) download is decrypted too.
- **Low-cost extras:**
  - SCRAM-SHA-256 (extend `auth_scram.c`);
  - direct TLS via `_xmpps-client` SRV (XEP-0368);
  - bookmarks with autojoin (XEP-0402, 0048 fallback);
  - stream management resume (`stream_management.c` currently only acks);
  - Client State Indication (0352) tied to window focus/idle.

## Critical files
- **Build:** `configure.ac`; new `pidgin4/meson.build`, `pidgin4/pidgin-internal.h`, `pidgin4/resources/*.gresource.xml`.
- **Core UI:** `pidgin4/gtkmain.c`, `gtkutils.c`, `gtkblist.c`, `gtkconv.c`, `gtkstatusbox.c`, `gtkrequest.c`, `gtknotify.c`, `gtkaccount.c`, `gtkprefs.c`, `gtkdocklet*.c`, `gtkidle.c`, `pidginstock.c`.
- **New:** `pidgin4/pidginmarkup.c`, `pidginmessage.c`, `pidginmessageview.c`, `pidgincomposeentry.c`, `pidginrichlabel.c`, `pidginmenu.c` (PurpleMenuAction→GMenuModel), `pidginsni.c`, `pidginidle-wayland.c`.
- **Removed from the new UI:** `gtkimhtml*.c`, `gtksourceundomanager.c`, `gtksourceiter.c`, `gtksession.c`, `gtkcellrendererexpander.c`, `gtkmenutray.c`, `getopt*.c`, `gtkmedia.c`, `gtkwhiteboard.c`.
- **libpurple:** `network.c`, `proxy.c`, `dnsquery.c`, `dnssrv.c`, `conversation.[ch]` (additive meta API), `protocols/jabber/{message,jabber,auth_scram,stream_management,chat,si}.c` + new `carbons.c`, `mam.c`, `httpupload.c`, `receipts.c`, `correction.c`, `reactions.c`, `bookmarks.c`; new `libpurple/plugins/omemo/`.

## Verification
- **ABI:** after every libpurple change, the exported-symbol diff against `/usr/lib64/libpurple.so.0.14.14` shows additions only. Discord and Steam accounts connect in both the GTK 2 and pidgin4 builds.
- **Tests:** libpurple `make check`; Meson unit tests for PidginMarkup (HTML subset ↔ Pango/tags round-trips, including WBFO and point sizes) and the dbusmenu exporter.
- **Runtime:** runs use `-c ~/.purple-gtk4`, `G_DEBUG=fatal-criticals`, and optionally an ASan build. `GTK_DEBUG=interactive` is used for layout checks.
- **Per milestone:**
  - M2: all accounts sign in and a password prompt works.
  - M3: buddy list shows presence; DnD reorder works; status changes.
  - M4: IRC/XMPP/Discord chats send and receive formatted text, smileys, images, find, scrollback, history plugin, markerline.
  - M6: the tray appears in Waybar with a working menu; auto-away triggers under Sway (idle N min) and GNOME; notifications appear under both.
- **XMPP interop:** test against a Dino or Gajim client and Conversations on a phone, using an existing account or a local Prosody test server. Check:
  - carbons both directions;
  - MAM catch-up after being offline;
  - upload of an image both directions;
  - OMEMO 1:1 exchange + fingerprint verification;
  - receipts/markers, edit, reaction, reply rendering and sending.
- **Sessions:** a full-day daily-driver run under Sway, and a smoke test under GNOME 49.

## Open items
- **D-Bus.** The plan builds libpurple with D-Bus disabled, which removes `purple-remote` and any scripts that rely on it. Re-enable it if that is needed.
- **OMEMO dependency.** `libomemo-c` is not installed and may need an overlay ebuild.
- **GNOME tray.** A tray icon under GNOME requires the AppIndicator/KStatusNotifierItem Shell extension, which is not installed.

See also `doc/GTK4-MIGRATION.md` for the general survey of GTK 2 → GTK 4 API changes this plan is based on.
