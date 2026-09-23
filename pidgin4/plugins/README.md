# pidgin4 UI plugins (M7)

These are Pidgin 2's GTK plugins, ported to GTK 4 and the pidgin4 UI.
Each one is a `PurplePlugin` of type `PURPLE_PLUGIN_STANDARD` with the
ui_requirement `PIDGIN_PLUGIN_TYPE` (`"gtk-gaim"`, `PIDGIN_UI`), built as
a shared module and installed into `<prefix>/lib/pidgin4/<name>.so`.

| File | Plugin id | Name | Prefs |
|---|---|---|---|
| `history.so` | `gtk-history` | History | `/pidgin4/plugins/history/lines` (new) |
| `markerline.so` | `gtk-plugin_pack-markerline` | Markerline | Pidgin 2's `/plugins/gtk/gtk-plugin_pack-markerline/*` |
| `timestamp_format.so` | `core-timestamp_format` | Message Timestamp Formats | Pidgin 2's `/plugins/gtk/timestamp_format/*` |
| `notify.so` | `gtk-x11-notify` | Message Notification | Pidgin 2's `/plugins/gtk/X11/notify/*`, plus `/pidgin4/plugins/notify/method_notification` |
| `cap.so` | `gtk-g-off_-cap` | Contact Availability Prediction | Pidgin 2's `/plugins/gtk/cap/*`; data in `<profile>/pidgin4/cap.db` |
| `convcolors.so` | `gtk-plugin_pack-convcolors` | Conversation Colors | Pidgin 2's `/plugins/gtk/gtk-plugin_pack-convcolors/*` |
| `spellchk.so` | `gtk-spellcheck` | Text replacement | the word list `<profile>/dict` (Pidgin 2's file) |
| `sendbutton.so` | `gtksendbutton` | Send Button | sets `/pidgin4/conversations/send_button`; keeps the old value in `/pidgin4/plugins/sendbutton/previous` |
| `gtkbuddynote.so` | `gtkbuddynote` | Buddy Notes | the `notes` blist node setting (libpurple's buddynote) |
| `iconaway.so` | `gtk-iconaway` | Iconify on Away | none |
| `relnot.so` | `gtk-relnot` | Release Notification | Pidgin 2's `/plugins/gtk/relnot/last_check` |
| `timestamp.so` | `gtk-timestamp` | Timestamp | Pidgin 2's `/plugins/gtk/timestamp/interval` |
| `xmppconsole.so` | `gtk-xmpp` | XMPP Console | none |
| `xmppdisco.so` | `gtk-xmppdisco` | XMPP Service Discovery | none |

Plugin ids and shared pref keys are Pidgin 2's where the meaning is the
same, so one profile works in both UIs. Anything that only pidgin4 means
lives under `/pidgin4/plugins/<name>/` (profile contract, rule 2).

Not ported (dropped for pidgin4): gestures, transparency, extplacement,
ticker, pidginrc, themeedit, musicmessaging, gevolution, unity, vvconfig,
mailchk, contact_priority, raw, gtk-signals-test, the Perl/Tcl/Mono
loaders, and the win32 plugins (including disco's Windows-only bits).

## Loading and installing

- pidgin4 keeps its own list of loaded plugins in `/pidgin4/plugins/loaded`
  (never `/pidgin/plugins/loaded`, which holds GTK 2 paths).
- It searches `<profile>/pidgin4/plugins`, `<profile>/plugins` (prpls
  shared with Pidgin 2 only), `<prefix>/lib/pidgin4` and the libpurple
  prefix's `lib/purple-2`; never `/usr/lib64/pidgin`.
- Nothing may go into `<profile>/plugins`: Pidgin 2 probes every `.so` in
  there and a GTK 4 plugin would crash it (contract, rule 4). Put your own
  pidgin4 plugins into `<profile>/pidgin4/plugins`.
- The plugins dialog (Tools → Plugins, M5) lists them and shows their
  configuration.

## Building one

`meson.build` builds every entry of `ported_plugins` with
`-DPIDGIN4_PLUGIN`, linked against GTK 4, GLib and libpurple only (cap
also links sqlite3). Other libraries are used for their headers only
(`partial_dependency`). Every `pidgin_*` symbol comes from the pidgin4
executable, which is linked with `export_dynamic`. libpurple opens plugins
with `RTLD_NOW`, so a plugin that uses a symbol the executable doesn't
export fails to load ("not loadable"); `pidgin4-plugin.h` declares M5's
optional API weak for that reason.

`pidgin4-plugin.h` has the common includes (libpurple, `gtkconv.h`,
`gtkconvwin.h`, `gtkblist.h`, `gtkutils.h`, `pidginmessageview.h`,
`pidginmessage.h`, `pidgincomposeentry.h`, M5's `gtkplugin.h` and
`gtkpluginpref.h`) and two helpers:

- `pidgin4_plugin_conv_view(conv)`: the conversation's
  `PidginMessageView`, or NULL.
- `pidgin4_plugin_show_config(plugin)`: the plugin's configuration in a
  small window of its own (timestamp_format's row menu item uses it).

Prefer a `PurplePluginPrefFrame` (`prefs_info`) for prefs: the M5 dialog
turns it into widgets (`pidgin_plugin_pref_frame_to_widget()`). Use a
`PidginPluginUiInfo` `get_config_frame` (a GTK 4 widget) only when the
frame needs more than prefs (colour buttons, the spellchk word list).

## What changed from Pidgin 2's plugin API

The GTK 2 plugin API is not preserved; `PidginConversation` and
`PidginWindow` keep their names and the members that still mean
something, but use the accessors.

**Conversations** (`gtkconv.h`):
- `gtkconv->imhtml` is a `PidginMessageView`, not a GtkIMHtml:
  `pidgin_conv_get_message_view(gtkconv)`.
- `gtkconv->entry` is a `PidginComposeEntry` (a GtkSourceView):
  `pidgin_conv_get_compose_entry(gtkconv)` (or `pidgin_conv_get_entry()`).
- `pidgin_conv_get_send_button()`, `pidgin_conv_get_toolbar()`,
  `pidgin_conv_get_tab_container()` (the notebook page),
  `pidgin_conv_get_window()`, `pidgin_conv_get_conversation()`.
- Windows (`gtkconvwin.h`): `pidgin_conv_window_get_window(win)` (a
  `GtkApplicationWindow`), `pidgin_conv_window_get_notebook(win)`,
  `pidgin_conv_window_get_gtkconvs()`, `_get_active_gtkconv()`,
  `_raise()` (`gtk_window_present()`). There is no urgency hint and no X
  property on Wayland.
- Signals on `pidgin_conversations_get_handle()`, with Pidgin 2's
  signatures: `conversation-timestamp`, `displaying-im-msg`,
  `displayed-im-msg`, `displaying-chat-msg`, `displayed-chat-msg`,
  `conversation-switched`, `conversation-hiding`, `conversation-displayed`,
  `chat-nick-autocomplete`, `chat-nick-clicked`. `conversation-dragging`
  is gone.

**The message view** (`pidginmessageview.h`, `pidginmessage.h`), instead
of writing into the GtkTextBuffer:
- `pidgin_message_view_append()`, `_prepend_many()` (history),
  `_get_model()` (a `GListModel` of `PidginMessage`), `_refresh()` (rows
  ask for their timestamp again), `_scroll_to_message()`.
- The marker item: `pidgin_message_view_set_marker()`, `_remove_marker()`
  and (new) `_get_marker()`, replacing markerline's expose handler.
- Rows carry the message type as CSS classes (`msg-send`, `msg-recv`,
  `msg-nick`, `msg-system`, `msg-error`, `msg-whisper`, `msg-auto-resp`,
  `msg-delayed`) and a message's own classes
  (`pidgin_message_add_css_class()`; history uses `history`, timestamp
  `timestamp-plugin`). Style them with a `GtkCssProvider` (convcolors).
- The row context menu: connect to the view's `"populate-menu"`
  (msg, GMenu section) and add `app.*`/`win.*` items; it replaces the
  GtkTextView `populate-popup` emission hook (timestamp_format).

**The compose entry** (`pidgincomposeentry.h`):
- `"message-send"` (markup) → gboolean, as before but with the markup.
- New `"pre-send"` → gboolean: emitted first, so a plugin can edit the
  buffer; return TRUE to hold the message back (spellchk).
- The buffer is a `GtkSourceBuffer`; `gtk_text_view_get_buffer()` works.

**Buddy list** (`gtkblist.h`): `drawing-tooltip` (node, GString of Pango
markup, full) and `drawing-buddy` keep Pidgin 2's signatures; node menus
come from libpurple's `blist-node-extended-menu`.

**Notifications**: `pidgin_notification_new_message()` and
`_withdraw()` (`pidginnotify.h`, a GNotification); the tray follows the
unseen state (`pidgin_docklet_update_icon()`).

**Dialogs**: no `gtk_dialog_run()`. Use `pidgin_dialog_new()` and
friends (`gtkutils.h`), `GtkAlertDialog`, `purple_request_*`, or a
popover.

## Testing

`PIDGIN4_PLUGINS_SELFTEST=1` loads every plugin here and checks what each
one does (see `pidgin4/TESTING.md`, Developer aids).
