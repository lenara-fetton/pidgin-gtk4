# Migrating Pidgin 2.14.14 from GTK+ 2 to GTK 4

This document is the background survey for porting the Pidgin GTK user
interface (`pidgin/`) from GTK+ 2 to GTK 4. It covers the current state of the
code, the API areas GTK 4 removed or redesigned, and the main risks. The path
actually chosen, with its milestones, is in
[`doc/PIDGIN-UPGRADE.md`](PIDGIN-UPGRADE.md); section 4 summarizes it.

Scope:

* **`libpurple/`** is UI-agnostic and uses GLib/GObject but not GTK. It needs
  no GTK port. It does need a newer GLib floor (the plan targets GLib 2.88), and some
  optional parts, such as `dbus-glib`, are deprecated in their own right.
* **`finch/`** uses libgnt (ncurses) and is not affected.
* **`pidgin/`** and **`pidgin/plugins/`** need the port. All numbers below
  refer to these two directories.

---

## 1. Current state

| Item | Value |
|---|---|
| Required GTK | `gtk+-2.0 >= 2.10.0` (`configure.ac:387`) |
| Required GLib | `glib-2.0 >= 2.16.0` (`configure.ac:297`) |
| Source in `pidgin/` (incl. plugins) | 166 `.c`/`.h` files, ~101k lines |
| Bundled plugins in `pidgin/plugins/` | 41 `.c` files, ~21k lines |
| Public headers exposing GTK types | 52 `pidgin/*.h` files (plugin API/ABI) |
| `GTK_CHECK_VERSION` conditionals | 76, all for 2.x versions (2.4 → 2.18) |
| Largest files | `gtkconv.c` 10.5k, `gtkblist.c` 8.4k, `gtkimhtml.c` 6.0k, `gtkutils.c` 3.6k, `gtkprefs.c` 3.6k lines |

The code targets GTK 2.10–2.18 and still uses many APIs that were already
deprecated during GTK 2. It is at least two major API generations behind
GTK 4: GTK 2 → GTK 3 changed the rendering, styling, and sealing model, and
GTK 3 → GTK 4 changed the event, container, and drawing model.

### Custom widgets (GObject subclasses of GTK classes)

These need the most per-file work because they override vfuncs whose
signatures changed or that no longer exist:

| File | What it is | Main problems |
|---|---|---|
| `gtkimhtml.c` / `gtkimhtmltoolbar.c` | HTML-ish rich text view on `GtkTextView`, used for every conversation, log, and IM entry | `expose_event`, `GdkPixbufAnimation` smileys, clipboard, DnD, `GtkTooltips`, `GtkItemFactory`-style menus. About 600 call sites across 22 files |
| `gtkstatusbox.c` | Custom `GtkContainer` status selector | Overrides `size_request`, `size_allocate`, `expose_event`, `forall`. `GtkContainer` is gone in GTK 4 |
| `gtkmenutray.c` | `GtkMenuItem` holding icons | GTK 4 has no widget-based menus |
| `gtkcellrendererexpander.c` | Custom `GtkCellRenderer` | Render vfunc changed to snapshot-based. Cell renderers are deprecated in 4.10 |
| `minidialog.c`, `pidgintooltip.c` | Inline dialogs, custom tooltips | `expose-event` painting, window positioning |
| `gtkscrollbook.c` | Notebook wrapper | `GtkContainer` API |
| `gtksourceundomanager.c`, `gtksourceiter.c` | Copied from old GtkSourceView | GTK 4 `GtkTextBuffer` has built-in undo, so these can be deleted |
| `plugins/ticker/gtkticker.c` | Scrolling ticker container | Full custom container. Rewrite |

---

## 2. API inventory: what GTK 4 removes or changes

Counts are matching lines in `pidgin/` (including plugins), measured with
`grep`. They are approximate and meant to show relative scale.

### 2.1 Removed or ancient GTK 2 APIs (fix these first, in GTK 2)

| API | Uses | Files | Replacement |
|---|---:|---:|---|
| `GtkItemFactory` | 87 | 4 (`gtkblist.c`, `gtkconv.c`, headers) | `GMenu` + `GAction` (GTK 4). In GTK 2/3 as an interim step: `GtkUIManager` or plain `GtkMenu` |
| `GtkTooltips` | 28 | 9 | `gtk_widget_set_tooltip_text/markup` |
| `GtkOptionMenu` | 20 | 6 | `GtkComboBox`, then `GtkDropDown` in GTK 4 |
| Old combo (`gtk_combo_box_new_text`, etc.) | 45 | 6 | `GtkComboBoxText`, then `GtkDropDown` |
| `GtkFileSelection` | 5 | 2 | `GtkFileChooser`, then `GtkFileDialog` (4.10) |
| `GtkObject` / `GTK_OBJECT` | 38 | 15 | `GObject`; the `destroy` signal moves to `GtkWidget` |
| `GTK_WIDGET_*` flag macros | 60 | 15 | `gtk_widget_get_visible()`, `gtk_widget_get_realized()`, etc. |
| Direct struct access (`->window`, `->allocation`, `->style`, …) | ~400 | 40 | Accessors. Required: the structs are sealed from GTK 3 on |
| `GdkPixmap`, `GdkBitmap`, `GdkGC`, `gdk_draw_*` | ~20 | 6 | cairo (GTK 3), then `GtkSnapshot`/`GtkDrawingArea` (GTK 4) |
| `GdkColor` / `gdk_colormap` | 150 | 19 | `GdkRGBA` |
| Keysyms `GDK_Return`, etc. | 25 | 6 | `GDK_KEY_Return` |

### 2.2 Removed or redesigned in GTK 3

| API | Uses | Files | Notes |
|---|---:|---:|---|
| `expose-event` handlers and vfuncs | 21 | 6 | Becomes `draw` (cairo) in GTK 3, then snapshot or `GtkDrawingArea` draw func in GTK 4 |
| `size_request` / `size-request` | 76 | 22 | Becomes `get_preferred_*` in GTK 3, then `measure` in GTK 4 |
| `GtkStyle`, `gtk_paint_*`, `gtk_style_*` | 55 | 18 | `GtkStyleContext` in GTK 3, then CSS plus `gtk_widget_get_color` in GTK 4 |
| gtkrc (`gtk_rc_*`, `GtkRcStyle`, `doc/gtkrc-2.0`, `plugins/pidginrc.c`) | 14 | 4 | Replace with CSS providers. `pidginrc` must be redesigned or dropped |
| `GtkHBox` / `GtkVBox` | 248 | 52 | `GtkBox` with an orientation |
| `GtkTable` | 84 | 8 | `GtkGrid` |
| `GtkAlignment`, `GtkMisc` | 119 | 28 | Widget `halign`/`valign`/`margin-*` properties |

### 2.3 Removed or redesigned in GTK 4

| API | Uses | Files | GTK 4 replacement |
|---|---:|---:|---|
| `gtk_box_pack_start/end` | 519 | 51 | `gtk_box_append/prepend` plus `hexpand`/`vexpand` |
| `GtkContainer` (`gtk_container_add`, border width, `forall`) | 294 | 55 | Per-widget setters (`gtk_window_set_child`, `gtk_scrolled_window_set_child`, …) and margins |
| `gtk_widget_show_all` / `hide_all` | 154 | 49 | Widgets are visible by default. Remove the calls |
| `gtk_widget_destroy` | 162 | 48 | `gtk_window_destroy` for toplevels; `gtk_*_remove` / unparent otherwise |
| `GdkEvent*` struct access, `event->button`, etc. | 274 | 31 | Opaque events; use event controllers |
| `button-press-event`, `key-press-event`, `motion-notify-event`, … signals | 86 | 26 | `GtkGestureClick`, `GtkEventControllerKey`, `GtkEventControllerMotion`, `GtkEventControllerScroll` |
| `GtkMenu` / `GtkMenuItem` / `gtk_menu_popup` | 221 | 21 | `GMenuModel` + `GtkPopoverMenu`; context menus become popovers |
| Stock items (`GTK_STOCK_*`, `gtk_*_new_from_stock`, `GtkIconFactory`) | 239 | 36 | Icon names (`"dialog-error"`, …) and themed icons; plain labels on buttons |
| `gtk_dialog_run` and `GTK_DIALOG()->vbox`/`action_area` | 60 | 14 | Async `GtkAlertDialog` / `GtkFileDialog` (4.10+) or plain `GtkWindow` with callbacks |
| Drag and drop (`gtk_drag_*`, `GtkSelectionData`) | 70 | 10 | `GtkDragSource`, `GtkDropTarget`, `GdkContentProvider` |
| Clipboard (`gtk_clipboard_*`) | 31 | 2 | `GdkClipboard` (async only) |
| `GdkWindow` (`gdk_window_*`) | 70 | 24 | `GdkSurface`; most uses must be removed |
| Window placement (`gtk_window_move`, `get_position`, `stick`, `set_keep_above`, …) | 57 | 27 | **Removed and not replaced.** Wayland does not allow client-side positioning |
| `GtkStatusIcon` (tray docklet) | 13 | 2 | **Removed.** Use StatusNotifierItem over D-Bus (for example via libayatana-appindicator) or drop it |
| `GtkSocket` / `GtkPlug` | 5 | 1 | Removed. Delete the compat shim in `gtkgaim-compat.h` |
| `GtkEventBox` | 23 | 10 | Removed. Attach event controllers to the widget directly |
| `GtkToolbar` / `GtkToolItem` | 31 | 2 | Removed. Use a `GtkBox` of buttons |
| `GtkRadioButton` | part of 261 | 20 | `GtkCheckButton` with `gtk_check_button_set_group` |
| `GtkTreeView` / `GtkCellRenderer` | 762 | 30 | Still present but **deprecated in 4.10**. The long-term target is `GtkListView`/`GtkColumnView` with `GListModel`. The buddy list is the biggest user |
| `GtkComboBox` | — | — | Deprecated in 4.10 in favor of `GtkDropDown` |
| `GtkTextView` / `GtkTextBuffer` | 662 | 19 | Still supported. Tag API mostly unchanged, but child anchors, pixbuf insertion, and event handling changed |
| `GdkPixbuf` in widgets (`gtk_image_new_from_pixbuf`, …) | many | — | Prefer `GdkTexture`/`GdkPaintable`. `GdkPixbuf` still works for loading |
| `gtk_main` / `gtk_main_quit` | 4 | 1 | Removed. Use `GtkApplication` and `g_application_run` |

### 2.4 Platform-specific code

* **X11-only code** (`gdkx.h`, `GDK_WINDOW_XID`, `gdk_x11_*`) in
  `pidgin.h`, `gtkidle.c`, `gtksession.c`, `gtkrequest.c`, `gtkprefs.c`,
  `gtkutils.c`, and `plugins/gestures/stroke-draw.c`. On Wayland:
  * Idle detection (`gtkidle.c`, XScreenSaver) needs
    `org.freedesktop.ScreenSaver`, the `ext-idle-notify` protocol, or
    `GtkApplication` inhibit/idle hints.
  * X session management (`gtksession.c`, libSM/ICE) should be replaced by
    `GtkApplication` session features, or removed.
  * Window "urgency", raise/present, and positioning behave differently.
* **Voice/video** (`gtkmedia.c`) embeds video by passing an X11 XID or
  Win32 HWND to `GstXOverlay`, a GStreamer 0.10 interface. GTK 4 has no
  native window handles to embed into. Use GStreamer 1.x with
  `gtk4paintablesink` (gst-plugins-rs) and a `GtkPicture`. This is a
  rewrite of the video display path, and the whole GStreamer 0.10 path
  should be dropped.
* **Windows** (`pidgin/win32/`, `Makefile.mingw`): `gtkdocklet-win32.c`,
  `MinimizeToTray.c`, `gtkwin32dep.c`, and the `transparency` plugin all use
  `GdkWindow`/HWND access and will need rework. The NSIS installer must bundle
  the GTK 4 runtime instead of GTK 2.

### 2.5 External dependencies tied to GTK 2

| Dependency | Status | Replacement |
|---|---|---|
| `gtkspell-2.0` (61 uses, 9 files) | GTK 2 only. gtkspell3 is GTK 3 only | `libspelling` (GTK 4, GtkSourceView-based) or a custom `GtkTextView` + Enchant integration |
| `gtk+-2.0` Perl bindings (`plugins/perl/common/Gtk*.xs`) | Gtk2-perl is dead | GTK 4 has no maintained XS bindings (Gtk3 uses GObject-Introspection). Drop Perl UI bindings, or keep only libpurple Perl bindings |
| GStreamer 0.10 / farsight2 paths | Obsolete | GStreamer 1.x + farstream 0.2 only |
| `dbus-glib` (libpurple, optional) | Deprecated | GDBus (not a GTK issue, but worth doing alongside) |
| Evolution integration plugin (`gevolution`) | Uses old EDS/GTK 2 APIs | Rewrite against current EDS or drop |
| Unity/messaging-menu plugin | Ubuntu Unity is gone | Drop |

---

## 3. Plugin API and ABI impact

Pidgin exposes its GTK internals to plugins. `PidginConversation`,
`PidginBuddyList`, `PidginWindow`, `GtkIMHtml`, and others appear in 52
public headers with raw `GtkWidget *`, `GtkItemFactory *`, `GtkTooltips *`
and `GdkColor` fields. The port therefore:

* **breaks every third-party Pidgin UI plugin**, both source and binary.
  libpurple protocol plugins are unaffected as long as libpurple stays 2.x;
* invalidates the documented signals in `doc/gtk*-signals.dox`, several of
  which pass `GtkItemFactory*`, `GtkMenu*`, or `GdkEvent*` arguments;
* makes the Pidgin major version bump to 3.0 unavoidable. Upstream Pidgin
  reached the same conclusion: the GTK 3/GTK 4 port happened on the Pidgin 3
  development line, together with a libpurple 3 API break.

We recommend making struct fields private (accessors plus opaque types) as part
of the port, so that future GTK changes do not break plugins again.

---

## 4. Chosen path

The generic staged path this survey originally suggested (deprecation-clean
GTK 2.24, then GTK 3, then GTK 4) was not taken, and neither were the
alternatives (stay on GTK 2, stop at GTK 3.24, rebase on upstream Pidgin 3).
The approved plan is **[`doc/PIDGIN-UPGRADE.md`](PIDGIN-UPGRADE.md)**. In short:

* **Straight to GTK 4.22, plain GTK, no libadwaita**, Wayland only (Sway
  first, GNOME second). No GTK 3 stage.
* **libpurple stays autotools and ABI-compatible** with the installed
  `libpurple.so.0` (2.14.x): additive changes only, checked by
  `scripts/check-abi.sh`, because the user's Discord and Steam prpls link
  against it. It installs into a private prefix (`~/.local/pidgin4`) via
  `scripts/build-libpurple.sh`.
* **The GTK 2 `pidgin/` stays buildable** against that libpurple as the
  fallback daily driver until the end.
* **The new UI is a separate Meson project, `pidgin4/`**, copied from
  `pidgin/` and ported file by file. Deprecated GTK 4 widgets are replaced,
  not ported to (`GtkListView`/`GtkColumnView`, `GtkDropDown`, async
  dialogs, `GMenuModel`).
* **GtkIMHtml is replaced, not ported** (section 5, item 1): a markup parser,
  a `GtkListView`-based message view, a GtkSourceView 5 compose entry with
  libspelling, and a SQLite message index next to the unchanged HTML logs.
* **The profile is shared with Pidgin 2.14.14** under the plan's *Profile
  compatibility contract*, verified by `scripts/check-profile-compat.sh`.
* The work also modernizes libraries (libidn2, GNetworkMonitor, GResolver,
  GSound, SNI tray, Wayland idle) and adds modern XMPP (carbons, MAM, HTTP
  upload, OMEMO, receipts, corrections, reactions) and core IRCv3.

The work is split into milestones M0–M9; see the plan for their scope and
verification. Sections 1–3 and 5 of this document remain as background.

---

## 5. The hardest parts

1. **`GtkIMHtml` (≈6k lines plus ≈600 call sites).** This is the core of
   the conversation window, log viewer, and message entry. It parses a
   subset of HTML into `GtkTextBuffer` tags, handles animated smileys,
   custom smiley upload, links, the formatting toolbar, and copy-as-HTML.
   Options:
   * Port it. It is already `GtkTextView`-based, so GTK 4 keeps the model,
     but its event handling, drawing, clipboard, DnD, and animation code all
     change.
   * Replace it: a `GtkTextView` with markup-to-tags conversion for the
     entry, and a `GtkListView` of message rows for history. Upstream
     Pidgin 3 did this (Talkatu, then a list-based conversation view).
2. **Buddy list (`gtkblist.c`, 8.4k lines).** Menus, tooltips, DnD, theming,
   custom cell renderers, the headline/error mini-dialog area, and the
   status box are all in one file. Expect a near rewrite.
3. **Conversation window (`gtkconv.c`, 10.5k lines).** Tabs, menus,
   typing notifications, DnD, the infopane, and window placement code.
4. **Theming.** GTK 2 themes (`gtkrc`) and Pidgin's own blist/status-icon
   theme loaders have no direct GTK 4 equivalent. Everything moves to CSS.
   Users' existing `~/.purple/gtkrc-2.0` customizations will stop working.
5. **Wayland-driven feature loss.** Window positioning, tray icon, X idle,
   and embedding video by XID all need new designs or must be removed.
6. **Plugins.** 41 bundled plugins need porting. Several depend on removed
   concepts and should probably be dropped or redesigned: `gestures` (X
   stroke drawing), `transparency` (Win32 window alpha), `extplacement`,
   `ticker`, `pidginrc`, `unity`.

---

## 6. References

* GTK docs: "Migrating from GTK 2.x to GTK 3" and "Migrating from GTK 3.x to
  GTK 4" (docs.gtk.org)
* `configure.ac`: GTK/GLib/gtkspell/GStreamer dependency checks
* `pidgin/gtkgaim-compat.h`, `pidgin/pidgin.h`: current compatibility shims
* `doc/gtk*-signals.dox`: UI signals exposed to plugins, which need
  re-specifying
