# pidgin4: manual checklists (M2 sign-in, M3 buddy list and status, M4 conversations, M5 windows, M6 desktop integration, M7 plugins, M9 Discord and Steam)

The automated checks (unit tests, the headless selftests and
`scripts/check-profile-compat.sh`) never sign an account in. The checks
below do, so they are for you to run by hand.

## Before you start

- **Duplicate sessions.** Your Pidgin 2 is signed in to these accounts. A
  second sign-in of the same account behaves differently per service:
  - XMPP gets a second resource, which is harmless.
  - IRC hits a nick collision.
  - Discord and Steam may kick the other session.

  Either quit Pidgin 2 first, or test one account at a time with
  `-l NAME` (see below).
- **Steam's refresh token is shared.** It lives in libsecret, keyed by
  account, so it is the same one Pidgin 2 uses. Signing Steam in from
  pidgin4 can refresh or rotate it. Test Steam while Pidgin 2 is not
  running, and check Steam in Pidgin 2 afterwards.
- **Use the dev profile only.** Always pass `-c ~/.purple-gtk4`, never the
  default `~/.purple`. The cutover gate in `doc/PIDGIN-UPGRADE.md` still
  applies.
- **Build and install** into the prefix, and optionally set up desktop
  integration:
  ```sh
  scripts/build-pidgin4.sh --test                       # ~/.local/pidgin4/bin/pidgin4
  scripts/build-pidgin4.sh --desktop-integration        # optional: launcher, icons
  ```
- **What it can't do yet.** Since M4 conversations work (see the M4
  section). Still missing: the windows of M5 (preferences, log viewer,
  pounces, file transfer list, ...), the tray and sounds (M6) and the
  ported plugins (M7).
- **Receipts and markers go out.** pidgin4 advertises `message-meta`, so
  the XMPP prpl sends receipt requests and chat markers, and pidgin4 sends
  "displayed" markers and publishes XEP-0490 read state when you look at a
  conversation (`/pidgin4/conversations/send_markers`, default on). Your
  other clients will see those reads.

## Starting

```sh
P=~/.local/pidgin4/bin/pidgin4
$P -c ~/.purple-gtk4 -d                  # all enabled accounts, debug window
$P -c ~/.purple-gtk4 -l 'user@host' -d   # enable/sign in only this account
$P -c ~/.purple-gtk4 -n                  # no sign-in at all
```

The buddy list is the main window (M3). While the tray icon is shown (M6:
a StatusNotifierWatcher such as Waybar's tray accepted it), closing the
list hides it into the tray; without a tray, closing it quits pidgin4
(saving everything) unless `/pidgin4/blist/close_hides` is set in
`prefs.xml`. Quit also with Ctrl+Q, Buddies → Quit, the tray menu's Quit,
or `kill -TERM`. Starting pidgin4 again raises the buddy list. The accounts
window (Accounts → Manage Accounts) opens by itself only when no account
is enabled.

## Checklist

### Startup and shutdown
- [ ] With Pidgin 2 running on `~/.purple`, `pidgin4` without `-c` refuses
      to start: a dialog says Pidgin 2 is running, and the exit status is 1.
      The same happens with `-c ~/.purple`, `-c ~/.purple/` or a relative
      path to it. With `-m` it starts anyway, so **don't** do that on
      `~/.purple`: use it only to check the dialog, then quit.
- [ ] `pidgin4 -v` prints the version, git revision and libpurple version.
- [ ] A second `pidgin4 -c ~/.purple-gtk4` raises the running instance's
      buddy list and exits.
- [ ] Ctrl+Q and `kill -TERM <pid>` both disconnect, save and exit with
      status 0. With `-d`, the log ends with `Unloading plugin` lines.
- [ ] Under Sway, `swaymsg -t get_tree` shows `app_id: com.minowick.Pidgin4`.
      Add `assign [app_id="com.minowick.Pidgin4"] 2:chat` next to the
      `class="Pidgin"` rule if you want it on the chat workspace.

### Signing in
- [ ] **XMPP**: each enabled XMPP account reaches "Online" in the Status
      column, and TLS is used (see the debug window).
- [ ] **IRC**: each enabled IRC account connects and the MOTD arrives. To
      see it, open the debug window and filter on `irc`.
- [ ] **Password prompt**: in Modify, untick "Remember password" on one
      account and save. Enable it. A password prompt appears with a masked
      entry, a "Save password" check and OK disabled until something is
      typed. Signing in works. Cancel also works: the account goes back
      to offline and nothing crashes.
- [ ] **Wrong password**: type a wrong one. The buddy list shows
      "<account> disabled" at the top, with Re-enable / Modify Account /
      Dismiss.
      The account is disabled (fatal error). Modify Account opens the
      editor.
- [ ] **Discord, token login**: the Discord accounts sign in.
- [ ] **Discord QR login**: add a new Discord account in Add with the
      username only (no password/token), or use the plugin's QR option if
      your fork has one. A request window with the QR code image appears.
      It must be large and sharp enough to scan with the phone app. After
      approving on the phone, the window closes and the account signs in.
- [ ] **Steam, stored token**: the Steam accounts sign in without a prompt.
- [ ] **Steam Guard**: force a fresh login (for example, a new Steam
      account entry for the same user). First comes a Steam Guard code
      prompt: enter the code from the app or e-mail. If mobile approval is
      offered instead, an action prompt appears; approve on the phone. Each
      prompt closes when the plugin moves on, and consecutive prompts
      don't pile up.
- [ ] **Captcha**, if a service asks for one: an image field and a text
      field in one window. The image shows and the answer is accepted.

### Staying connected
- [ ] Turn networking off and on again, for example
      `nmcli networking off; sleep 20; nmcli networking on`, or unplug the
      cable. Accounts drop and then reconnect by themselves.
- [ ] Kill one connection server-side, or wait for an IRC ping timeout.
      The account reconnects after a random 8–60 s delay, with the delay
      doubling on repeated failures. The buddy list shows
      "<account> disconnected" for these
      non-fatal errors until it reconnects; the Status column shows it too.
- [ ] Leave it running for a few hours. Nothing disconnects for good, and
      memory use (`ps -o rss`) stays flat.

### Accounts window and editor
- [ ] All 34 accounts are listed, with protocol icons. Accounts of prpls
      that no longer exist (MSN, Yahoo, ...) show protocol "Unknown".
- [ ] Toggling Enabled signs an account in or out.
- [ ] **Account Actions** for a connected account lists the prpl's actions
      (XMPP: "Set User Info...", "Search for Users...", ...; Discord and
      Steam: their own), and each one opens its request or notify window.
      - XMPP "Search for Users…" leads to a search-results window with
        columns and buttons.
      - "Set User Info…" opens a multi-line request.
- [ ] **Modify** shows the Basic, Advanced and Proxy tabs with the current
      values. Change something harmless, such as the alias or an advanced
      option, and save. `accounts.xml` then shows only that change
      (`diff` against a copy taken before).
- [ ] **Add** a throwaway account, then **Delete** it and confirm the
      question. `accounts.xml` is back to what it was.
- [ ] **Proxy tab**: set a proxy on a test account, save and reopen. The
      values are kept. Set it back to "Use Global Proxy Settings" and the
      `<proxy>` element is removed again.

### Notifications and requests
- [ ] A server message or error from a prpl appears as an alert dialog.
- [ ] XMPP user info (for example "Get Info" through Account Actions or a
      search result's Info button) opens a "Buddy Information" window with
      clickable links.
- [ ] A file request, if a prpl or plugin asks for one, opens the portal
      or GTK file dialog.

### Profile compatibility afterwards
- [ ] Quit pidgin4, then run
      `scripts/check-profile-compat.sh --pidgin4 ~/.local/pidgin4/bin/pidgin4`.
      It must PASS.
- [ ] Optionally, run Pidgin 2 on `~/.purple-gtk4` (`pidgin -m -c ~/.purple-gtk4`)
      and check that the accounts, buddy list and prefs are intact.

## M3: buddy list and status (with accounts signed in)

The headless checks never sign in, so everything about live presence is
for you. Sign in as above (quit Pidgin 2 first, or use `-l NAME`).

### Presence
- [ ] Online buddies of each signed-in account appear under their groups,
      sorted as Buddies → Sort Buddies says (alphabetical by default).
      Status icons match: available, away, busy, extended away, and the
      greyed idle look.
- [ ] With Buddies → Show → Buddy Details on, the second line shows the
      status message (XMPP), the game (Steam) or "Offline", and
      "Idle 1h 05m" for idle buddies. With it off, idle buddies show
      "1:05" at the right and no second line.
- [ ] A buddy signing on or off shows the log-in/log-out icon for about
      10 seconds, then the normal one (offline buddies then disappear
      unless Show → Offline Buddies is on).
- [ ] Buddy icons (avatars) appear for XMPP, Discord and Steam contacts,
      greyed when offline or idle.
- [ ] Show → Protocol Icons adds the protocol icon to each row; prpl
      emblems (e.g. Steam's game emblem, blocked, mobile) show at the
      right.
- [ ] Hovering a buddy shows the tooltip: name, protocol icon, the
      prpl's fields (XMPP: status, subscription, resource; Discord/Steam:
      their own), idle, "Logged In", and the avatar. A contact with
      several online buddies shows each of them. If the `cap` plugin is
      loaded, its lines appear too (the `drawing-tooltip` signal).
- [ ] Group headers show "(online/total)" when collapsed. Collapse a
      group, quit, start again: it stays collapsed, and Pidgin 2 shows
      it collapsed as well (the shared `collapsed` setting in
      `blist.xml`).

### Menus
- [ ] Accounts: every enabled account has a submenu with Edit Account,
      the prpl's actions when connected (XMPP: Set User Info..., Search
      for Users...; Discord/Steam: theirs) and Disable. Disabled
      accounts are under "Enable Account".
- [ ] Right-click a buddy: Get Info, IM, Send File (where the prpl can),
      Show/Hide When Offline, the prpl's own items (e.g. XMPP
      subscription items, Discord's), Move to, Block/Unblock, Alias...,
      Remove. The Menu key and Shift+F10 open it for the selected row.
- [ ] Get Info (menu or Ctrl+O on a row) opens the Buddy Information
      window.
- [ ] Alias... / Rename (F2) change the name; Pidgin 2 shows the new
      alias afterwards.
- [ ] Buddies → Add Buddy... adds a buddy on a connected account and the
      server list gets it (check with another client). Add Group and
      Add Chat work; Join a Chat joins (the chat window is M4, but the
      room is joined: see the debug window).
- [ ] Right-click a chat → Auto-Join: after reconnecting, the chat is
      joined automatically.

### Status box
- [ ] The button at the bottom shows the current status. Picking Away,
      typing a message and waiting 4 seconds (or pressing Enter) sets
      it on every account; other clients see the message. Offline signs
      everything off; Available signs it back on.
- [ ] A popular saved status from the list activates it.
- [ ] While accounts connect, a spinner replaces the status icon.
- [ ] The icon button next to it → Choose Buddy Icon...: pick an image;
      other clients see the new avatar on accounts that use the global
      icon. Remove Buddy Icon clears it.
- [ ] In Modify Account, "Use this buddy icon for this account" with an
      image, saved: that account gets its own avatar.

### Errors and requests
- [ ] A wrong password (or other fatal error) shows "<account> disabled"
      at the top of the buddy list with Re-enable / Modify Account /
      Dismiss, instead of an alert. A non-fatal disconnect shows
      "<account> disconnected" with Reconnect until it reconnects by
      itself.
- [ ] Signing in the same account elsewhere with a prpl that reports it
      (XMPP resource conflict, some IRC networks) shows "Welcome back!"
      listing the accounts, with Re-enable.
- [ ] An XMPP subscription request shows "Authorize buddy?" in the
      buddy list; the name link opens the user info; Authorize adds them
      (and offers Add Buddy if they are not on the list).

### Drag and drop
- [ ] Drag a contact onto another group: it moves there (and in Pidgin 2
      afterwards). Drag onto a contact to reorder (with Sort Buddies →
      Manually) or into an expanded contact to merge.
- [ ] Drag an image file from the file manager onto a buddy: you are
      asked to set it as the buddy icon or send it. Another file is
      sent with a file transfer (the transfer window is M5; watch the
      debug window).

## M6: desktop integration

Install with `scripts/build-pidgin4.sh --desktop-integration` once: it
links the `.desktop` file (with `X-GNOME-UsesNotifications=true` and
`StartupNotify=true`) and the icons, including the tray's
`com.minowick.Pidgin4-<status>` icons, into `~/.local/share`. The tray also
works without it (the item sends its icons as pixmaps and names the
prefix's `share/icons` as its `IconThemePath`), but GNOME's notifications
need the `.desktop` file.

### Sway (Waybar)
- [ ] **Workspace rule.** Pidgin 2 is `class="Pidgin"` (XWayland);
      pidgin4 is native Wayland with `app_id` `com.minowick.Pidgin4`. To
      keep it on the chat workspace, add next to the existing
      `assign [class="Pidgin"] 2:chat` in `~/.config/sway/config`:
      ```
      assign [app_id="com.minowick.Pidgin4"] 2:chat
      ```
      and `swaymsg reload`.
- [ ] **Tray.** The Pidgin icon appears in Waybar's tray (the `tray`
      module must be in the bar). `busctl --user call
      org.kde.StatusNotifierWatcher /StatusNotifierWatcher
      org.freedesktop.DBus.Properties Get ss org.kde.StatusNotifierWatcher
      RegisteredStatusNotifierItems` lists `org.kde.StatusNotifierItem-<pid>-1`.
      The icon follows the status (available, away, busy, extended away,
      invisible, offline, connecting).
- [ ] **Left click** toggles the buddy list (hidden ↔ shown and focused);
      with unread messages it presents the next conversation instead.
- [ ] **Right click** shows the menu: Show Buddy List (check), Unread
      Messages (one entry per conversation, presents it), New Message,
      Join Chat (both disabled while offline), Change Status (Available …
      Offline as radio items with status icons, the popular saved statuses,
      New…/Saved… which log TODO(M5) until the status editor exists),
      Accounts, Plugins, Preferences, File Transfers (M5 windows), Mute
      Sounds (check, the same pref as Buddies → Mute Sounds), plugin
      actions, Quit. Each item does what it says.
- [ ] **Unread messages** (needs M4b conversations): an IM arriving in an
      unfocused conversation turns the icon into the "pending" icon (Waybar
      shows the attention icon for NeedsAttention), the tooltip lists "N
      unread messages from X" and the title says "Pidgin (N unread
      messages)". Reading the conversation clears it.
- [ ] **Closing the buddy list** hides it into the tray; quitting while it
      is hidden and starting again keeps it hidden (in the tray).
      `killall waybar` (restart it afterwards): the list is shown again at
      once, and the icon comes back when Waybar is back.
- [ ] **Idle.** Set Preferences → Status/Idle "Change status when idle"
      after 1 minute (or `/purple/away/mins_before_away` = 1 with
      `idle_reporting` = `system`), don't touch the keyboard or mouse:
      after about a minute the status goes to Away (idle), and back on
      the first input. With `-d` the log shows `idle: ext-idle-notify:
      idled` / `resumed`. `swayidle` keeps working alongside.
- [ ] **Sounds.** Receiving an IM plays the receive sound (unless Mute
      Sounds is on, or the conversation has the focus and
      `/pidgin/sound/conv_focus` is off). Custom files from Pidgin 2's
      sound prefs are used. `/pidgin4/sound/method` = `command` with
      `/pidgin4/sound/command` = `paplay %s` plays through that command.
- [ ] **Notifications.** An IM in an unfocused conversation shows a
      notification (mako) with the sender, the text and the buddy icon;
      clicking it presents the conversation. Several messages within 10 s
      give one notification ("N new messages"). It disappears when you
      read the conversation. `/pidgin4/notifications/new_message` = false
      turns them off.
- [ ] **Attention.** With the buddy list on another workspace, presenting
      it from the tray or a notification: Sway follows its
      `focus_on_window_activation` setting (default `urgent`: the
      workspace is marked urgent in Waybar instead of switching).
- [ ] **Floating dialogs.** Sway floats a toplevel that has a parent
      (`xdg_toplevel.set_parent`) and tiles the others. The buddy list and
      the conversation windows tile; every other window (Preferences,
      Accounts and the account editor, Buddy Pounces and its editor, Saved
      Statuses and the status editor, Plugins and plugin options, File
      Transfers, the log viewers, Privacy, Room List, Certificates, Custom
      Smileys, About, OMEMO, the XMPP console and service discovery, the
      debug window, the New IM/Get Info/View Log/Join Chat and Add
      Buddy/Chat/Group dialogs, requests and notifications) is a dialog of
      the buddy list and floats. `swaymsg -t get_tree | jq -r '.. |
      objects | select(.app_id? == "com.minowick.Pidgin4") | "\(.type)
      \(.name)"'` shows them as `floating_con`, the list as `con`.
      A request that belongs to a conversation is a dialog of that
      conversation window. With the buddy list hidden in the tray a new
      dialog floats over a conversation window or another open dialog;
      with no pidgin4 window shown at all (e.g. Preferences from the tray
      menu while everything is hidden) there is nothing to attach it to and
      it tiles. Hiding the list leaves open dialogs open and usable.
      Preferences → Interface → "Secondary windows are dialogs of the buddy
      list (float on tiling compositors)"
      (`/pidgin4/windows/secondary_transient`, default on) switches this off
      for windows opened afterwards (non-resizable dialogs such as Add
      Buddy still float: Sway floats fixed-size windows anyway).
      Headless: `PIDGIN4_WINDOWS_SELFTEST=1` checks every window it opens
      (`PIDGIN4_SELFTEST_SECONDARY=off` for the pref switched off); live:
      `PIDGIN4_SELFTEST_WAYLAND=1 PIDGIN4_WINDOWS_SELFTEST=secondary
      PIDGIN4_SELFTEST_HOLD=10 scripts/run-pidgin4-selftest.sh BIN
      SCRATCH-PROFILE` holds for 10 s with everything open, then again
      with the list hidden, for `swaymsg -t get_tree`.

### GNOME 49
- [ ] **Tray.** GNOME has no tray of its own. Without the
      "AppIndicator and KStatusNotifierItem Support" Shell extension there
      is no StatusNotifierWatcher: no icon, and closing the buddy list
      quits pidgin4 (as intended). With the extension enabled, the icon,
      its menu and left click behave as under Sway (the extension shows
      NeedsAttention by its own style, not always with the attention
      icon).
- [ ] **Idle.** Mutter does not offer ext-idle-notify to applications;
      the log says `idle method: system, org.gnome.Mutter.IdleMonitor
      (polled every 10 s)`. Auto-away after the configured minutes and
      back on input (within 10 s).
- [ ] **Notifications** go to GNOME Shell (org.gtk.Notifications) and
      need the installed `com.minowick.Pidgin4.desktop` (desktop
      integration). Clicking one presents the conversation; they appear
      under "Pidgin 4" in Settings → Notifications. Run without `-m` so
      GNOME can activate the running instance.
- [ ] **Sounds** play through GSound (PipeWire/PulseAudio) as under Sway.
## M4: conversations (with accounts signed in)

Use one account at a time (`-l NAME`) and a second client on the other
side: Conversations or Dino for XMPP (a test account on the same server
is best), another IRC client, the Discord web client.

### Opening and windows
- [ ] Double-click a buddy, "IM" in its menu, Buddies → New Instant
      Message (Ctrl+M) and Join a Chat: a window opens with the
      conversation; the second one becomes a tab (placement pref
      `/pidgin/conversations/placement`, default "last").
- [ ] Tabs: drag to reorder; drag a tab out of the window to make a new
      window, and onto another window's tab bar to move it there. Right
      click on a tab: Close other tabs / Detach / Close. Middle click
      closes. With one conversation there is no tab bar.
- [ ] Keys: Ctrl+W closes the tab, Ctrl+Tab / Ctrl+Shift+Tab go to the
      next/previous tab with unread text, Ctrl+PgDn/PgUp (and Ctrl+] /
      Ctrl+[) next/previous tab, Alt+1..9 tab N, Ctrl+, / Ctrl+. move the
      tab, Ctrl+F find, Ctrl+L clear, Ctrl+O get info.
- [ ] The window keeps its size (`/pidgin4/conversations/width|height`).
- [ ] With `/pidgin/conversations/im/hide_new` = `always` (or `away`
      while away), a new IM opens no window; the buddy list row shows it
      as unread; double-clicking the buddy shows it.

### Messages
- [ ] Send and receive in an IM and a chat on each protocol (XMPP 1:1 and
      MUC, IRC channel and query, Discord DM and channel, Steam). Names
      have the send/receive colours; chat nicks are coloured (XEP-0392
      colours for XMPP, matching Dino/Conversations); a message that
      says your nick is highlighted and its tab turns bold blue.
- [ ] A tab with unread text turns red, with an event grey; typing shows
      the typing icon and "X is typing..." above the entry. The buddy
      list row turns bold with the unread count; reading the tab clears
      both.
- [ ] Formatting: XMPP offers bold/italic/strike/code only and sends
      `*bold*` etc. (check the other client shows them); IRC/Discord
      offer what their connection flags allow. Incoming formatting shows
      unless "show incoming formatting" is off.
- [ ] Smileys: the toolbar's smiley picker inserts them; received
      shortcuts show as the theme's images.
- [ ] Images: Insert Image (IM on protocols with images); dropping an
      image file on an IM does what a paste does (below); only where
      neither works does it offer "Set as Buddy Icon"; any other file is
      sent (HTTP upload on XMPP when the server has it). Our own upload's
      URL, and images on the account's own XMPP domain, show inline;
      Discord CDN images show inline.
- [ ] Paste a screenshot (Ctrl+V, or right click → Paste Image) into an
      XMPP chat: a "Send Image" dialog floats over the conversation
      window (modal to it) with the preview, the name, size in pixels,
      file size and PNG/JPEG, and "Send to <buddy> via <account>";
      Escape (Cancel) sends nothing and leaves nothing in
      `~/.purple…/pidgin4/paste/`. Paste again and press Enter (Send):
      it uploads (HTTP upload; a "Sending the image pasted-….png as a
      file." line, then the transfer lines), and
      `~/.purple…/pidgin4/paste/` is empty again once it finished.
      Dropping a non-image file asks the same way (icon, name, size).
      Closing the tab with the dialog open closes it, sending nothing.
      With Preferences → Conversations → "Confirm before sending a
      pasted or dropped file" off, it sends at once as before. Into
      a Discord DM: the image appears in the entry inline. Copy
      spreadsheet cells (text and an image on the clipboard): Ctrl+V
      pastes the text. A photo pastes as `.jpg` with Preferences →
      Conversations → "Send pasted images as" Automatic, `.png` with PNG.
- [ ] From Conversations or Dino, share a photo whose upload host is not
      your server's (a contact on another server): it shows inline under
      its link, and clicking it opens the link. A shared non-image file
      stays a link. Preferences → Conversations → "Show images shared
      over XMPP inline" off: links only. Receive an image by file
      transfer (Jingle/SI): libpurple's "Transfer of file … complete"
      line keeps its blue link and the picture shows under it; clicking
      the picture opens the file ("Show received image files inline"
      turns it off).
- [ ] Audio and video: receive a voice message or clip over XMPP (a
      share from Conversations/Dino, and a Jingle/SI transfer) and a
      Discord video attachment: libpurple's line and the link stay, and
      a card under it shows the name, the size when known, "Play" (opens
      the default player, e.g. mpv) and, for a received file, "Open
      Folder". With this machine's GTK (USE=-gstreamer) there is no
      inline player; rebuilding gtk with USE=gstreamer turns inline
      playback on by itself (a player in the card), unless Preferences →
      Conversations → "Play audio and video inline" is off.
- [ ] The toolbar's paperclip (Send File) shows in XMPP IMs and MUCs
      (HTTP upload) and wherever the protocol can send a file, not on
      Steam; clicking it opens the file chooser and sends the file. It
      follows tab switches and Send To.
- [ ] `/help`, `/me waves`, `/clear`, `/debug version`, and a protocol
      command (`/topic`, `/nick`, `/op` on IRC; `/role` on XMPP).
- [ ] Chat user list: ops first, then voiced, buddies bold; right click
      offers IM, Info, Ignore, Add, and Op/Deop/Voice/Kick/Ban where the
      protocol has those commands; double click opens an IM. Tab
      completes nicks. The topic entry sets the topic where allowed.
- [ ] Find (Ctrl+F) filters and highlights; Save As writes an HTML file.
- [ ] Scroll to the top: older messages load from `messages.db` (after a
      backfill), then from the server archive on XMPP (a spinner shows
      while it loads).
- [ ] Options → Enable Logging / Enable Sounds / Show Formatting
      Toolbars / Show Timestamps work (Sounds takes effect with M6).

### Modern XMPP round trips (with Conversations or Dino)
- [ ] Receipts: a sent message gets one tick when delivered, two
      (blue) when displayed on the other side.
- [ ] Up-arrow in the empty entry edits your last message (banner
      "Editing"); the other client shows it corrected. A correction from
      the other side updates the message ("edited" marker, original in
      its tooltip).
- [ ] Right click a message → Reply: the banner shows the quote; the
      other client shows a reply. Replies from the other side show the
      quoted message above.
- [ ] React from the row menu or a reaction chip; reactions from the
      other side appear under the message; removing works both ways.
- [ ] Hover action bar: resting the pointer on a message (~150 ms) shows
      React/Reply/Edit/Delete/More at its top right (Edit and Delete on
      your own messages only; none on system lines, messages without ids,
      IRC or Steam); it goes on leave, text selection under it still
      works, each button does what the row menu does, and the entry has
      the keyboard focus afterwards (also after the row menu). Check it in
      a light and a dark theme.
- [ ] Delete for Everyone (your message) retracts it on the other side;
      a retraction from the other side shows "This message was deleted."
      In a room where you are a moderator, Delete works on others'
      messages too.
- [ ] Read a conversation in the other client: its unread state clears
      here (XEP-0490); reading here clears it there.
- [ ] Afterwards the HTML log has readable lines for each of those
      (`X edited: ...`, `X reacted 👍 to: ...`, `X retracted a message`);
      open it in Pidgin 2's log viewer.
- [ ] Restart pidgin4: the MAM catch-up doesn't repeat messages already
      shown, including after a Pidgin 2 session in between.

### Two pidgin4 instances against a local Prosody (no real accounts)

`scripts/tests/xmpp-live/` runs the round trips above between two (or
three) pidgin4 instances, each on its own Xvfb and D-Bus session, signed
in to a throwaway user-local Prosody 13 on 127.0.0.1. Only the test users
`alice@localhost` and `bob@localhost` ever sign in; nothing touches
`~/.purple` or `~/.purple-gtk4`.

Needs Prosody 13 with its Lua rocks (luasocket, luasec, luaexpat,
luafilesystem; `lua5.4`), Xvfb, xdotool, ImageMagick's `import` and
openssl. A user-local Prosody (`./configure --prefix=... && make install`,
the rocks built into the same prefix) is enough: point `PROSODY_PREFIX`
at it.

```sh
export PROSODY_PREFIX=~/prosody-13/prefix       # bin/prosody, share/lua, lib/lua
export PIDGIN4=~/.local/pidgin4/bin/pidgin4
export XMPP_LIVE_DIR=/tmp/pidgin4-xmpp-live      # the default
scripts/tests/xmpp-live/drive.sh                 # scripted scenario, ~2 min
scripts/tests/xmpp-live/run.sh start alice bob   # or: just start them
. scripts/tests/xmpp-live/lib.sh; DISPLAY=$(disp alice) ...  # drive by hand
scripts/tests/xmpp-live/run.sh stop
```

- `run.sh start` wipes the server data (unless `XMPP_LIVE_KEEP=1`),
  writes a config (c2s on port 25322 with STARTTLS and a fresh
  self-signed certificate, HTTP on 25380, carbons, MAM, smacks, CSI, PEP,
  bookmarks, the MUC `conference.localhost` with MAM and occupant-id,
  HTTP file share `upload.localhost`), creates the users (password
  `pencil1234`) and one profile per instance: `accounts.xml` with just
  that account (`connect_server` 127.0.0.1, the port, `require_tls`),
  `blist.xml` with the other user and the room `test`, and the server
  certificate in `certificates/x509/tls_peers/127.0.0.1` so it is
  trusted without a prompt (libpurple verifies against the connect
  server, not the domain). `alice2` is alice's second resource, for
  carbons. The instances run `pidgin4 -m -d`; their logs are
  `$XMPP_LIVE_DIR/pidgin4-<name>.log`.
- `drive.sh` drives the UIs with xdotool (there is no window manager:
  windows sit at 0,0 and are raised explicitly) and checks the stanzas
  in the `-d` logs (33 checks): styling, receipts, markers, carbons,
  Up-arrow correction, react/reply/retract from the row menu, MAM
  catch-up after restarting bob, HTTP upload through Send File... and
  the session's file chooser, and the same message actions in the room.
  It leaves screenshots of each step in `$XMPP_LIVE_DIR` and exits 0
  when every check passes (`XMPP_LIVE_NO_STOP=1` keeps everything
  running afterwards). The row and menu positions assume the default
  theme and window sizes; if a step fails, look at its screenshot.

## M5: the remaining windows

Most of this works without signing in (`-n`); the items marked
**(signed in)** need an account online. Afterwards, run
`scripts/check-profile-compat.sh --pidgin4 <binary>` again.

### Preferences (Tools → Preferences)
- [ ] Every page opens: Interface, Conversations, Smiley Themes, Themes,
      Sounds, Network, Browser, Logging, Status / Idle, Message Index.
- [ ] Changes apply at once, with no OK button: e.g. untick "Show
      timestamps", and it is saved in `prefs.xml` after a few seconds.
      Toggle Buddies → Show → Offline Buddies while Preferences is open:
      nothing else changes.
- [ ] Shared settings: change "Log all chats", quit, run Pidgin 2 on the
      same copy (`pidgin -m -c ~/.purple-gtk4`): its Preferences show the
      same value. The pidgin4-only ones (sound method, browser, send
      button, conversation placement) live under `/pidgin4` and are not
      in Pidgin 2's dialog.
- [ ] Conversations → Font: untick "Use the system font", pick a font:
      the sample entry below uses it at once. Default Formatting: bold,
      a face, a size and colours show in the preview line.
- [ ] Smiley Themes: picking another theme changes the smileys shown
      below the list (and in conversations).
- [ ] Themes: "Open" creates `~/.purple-gtk4/pidgin4/gtk4.css` with
      examples and opens it in your editor. Uncomment the
      `.pidgin-blist-away` rule, change the colour and save: away
      buddies in the buddy list change colour without a restart. Put a
      typo in a property name, press Reload: the error is shown under
      the buttons.
- [ ] Sounds: Browse picks a sound file per event, Reset goes back to
      "(default)". Preview plays nothing until the sound backend (M6)
      exists.
- [ ] Network: the detected IP is shown next to "Use automatically
      detected IP address". Proxy type "Use System Proxy Settings" greys
      out host and port.
- [ ] Browser: "Manual" with `firefox --new-window %s`: a link in a
      notification (e.g. a user info window) opens with that command;
      "Desktop Default" opens it through the portal/xdg-open.
- [ ] Status / Idle: "Change to this status when idle" picks a saved
      status; with idle time "Based on keyboard or mouse use" the idle
      status is reached after the minutes given (needs M6 idle under
      Sway/GNOME; until then libpurple's own timer applies).
- [ ] Message Index **(signed in, or after an "Index Now")**: the progress
      bar moves while logs are indexed; Pause/Resume work; the database
      size and message count grow. Don't press Rebuild on a big profile
      unless you have time.

### Pounces (Tools → Buddy Pounces, or a buddy's "Add Buddy Pounce")
- [ ] The manager lists the pounces from `pounces.xml`. Modify one,
      change nothing, Save: `pounces.xml` is unchanged (diff it),
      including an `execute-command` action's command.
- [ ] Add a pounce on a buddy for "Signs on" with a popup and a command
      (`notify-send pounce`), recurring. **(signed in)** When the buddy
      signs on, the popup shows and the command runs.
- [ ] Delete asks first and removes it from `pounces.xml`.

### Saved statuses (status box → Saved statuses… / New status…)
- [ ] The window lists the saved statuses with type and message; Use
      activates one (the status box follows).
- [ ] New status: title, type, message with formatting; "Use different
      status for some accounts" with a per-account status. Save shows it
      in the list and in the status box popover; Pidgin 2 shows it too.
- [ ] Duplicate and Delete (with confirmation) work.

### Log viewer (buddy → View Log, Buddies → View User Log…, Tools → System Log)
- [ ] A buddy with logs: the months are listed newest first, the newest
      conversation is shown with times, names and formatting.
- [ ] The title shows the total log size.
- [ ] Search for a word from an old conversation: only logs containing it
      remain, and the word is highlighted. It works for logs the message
      index has not covered yet (linear scan) as well.
- [ ] A contact with several buddies (e.g. XMPP + IRC) shows the logs of
      all of them.
- [ ] Tools → System Log shows the system logs of all accounts.
- [ ] Nothing in `logs/` changes by viewing (`find logs -newer <stamp>`
      prints nothing). Delete Log asks first and deletes only that file.

### Privacy (Tools → Privacy) **(signed in)**
- [ ] Pick an XMPP account: the policy is shown; Allow/Block lists show
      the server's lists. Add a name to the block list: the server gets it
      (check with another client), Remove takes it off.

### Room list (Tools → Room List, or Join a Chat → Room List) **(signed in)**
- [ ] On an XMPP account, Get List shows the conference service's rooms
      (with the room name, users, description columns); on IRC the
      channel list with a progress indicator and Stop.
- [ ] Join joins the selected room; Add Chat adds it to the buddy list;
      Bookmark (XMPP) adds a server bookmark, which another client (or a
      reconnect with autojoin) sees.

### Certificates (Tools → Certificates)
- [ ] The list shows the hosts from `certificates/x509/tls_peers`.
      View shows the subject, validity and SHA-1 and SHA-256
      fingerprints; compare one with
      `openssl x509 -in <file> -noout -fingerprint -sha256`.
- [ ] Export writes a PEM file; Import of that file under another name
      adds it; Delete (with confirmation) removes it again.

### File transfers **(signed in)**
- [ ] Send a file to an XMPP buddy with HTTP upload (M8): the File
      Transfers window opens, the progress bar, speed and remaining time
      move, and the row ends as "Finished".
- [ ] Receive a file: accept it in the request, the row shows progress,
      then "Open File" and "Open Folder" work.
- [ ] Stop cancels a running transfer; Clear Finished removes finished
      rows; with "Keep the dialog open" off, the window closes when all
      transfers are done.

### Custom smileys (Tools → Custom Smileys)
- [ ] Add: pick an image, give a shortcut; it is listed, and Pidgin 2
      shows it too (`smileys.xml`, `custom_smiley/`). Edit changes the
      shortcut; Delete removes it after asking.

### Plugins (Tools → Plugins)
- [ ] The list shows the libpurple plugins (Psychic Mode, Autoaccept,
      Join/Part Hiding, ...) and pidgin4 UI plugins, not prpls.
- [ ] Enable Psychic Mode: it is added to `/pidgin4/plugins/loaded` in
      `prefs.xml`, and `/pidgin/plugins/loaded` is unchanged. Configure
      Plugin shows its options; changes are kept. Disable it again.
- [ ] Put `/usr/lib64/pidgin/history.so` into `/pidgin4/plugins/loaded`
      of a scratch copy's `prefs.xml`: pidgin4 starts, logs that it
      refuses the GTK 2 plugin, and does not crash.

### About (Help → About)
- [ ] The versions (pidgin4, git revision, libpurple, GTK, GLib) are
      right; Build info lists the plugin directories; Copy puts it on the
      clipboard; Credits lists the developers.

### OMEMO (Tools → OMEMO Fingerprints)
- [ ] Without the OMEMO plugin loaded the window says so and offers the
      Plugins dialog.
- [ ] **(signed in, OMEMO plugin loaded)** Your own fingerprint matches
      what Conversations/Dino show for this device; contacts' devices are
      listed with their trust; setting one to "Verified" is kept after a
      restart (`pidgin4/omemo.db`).

### Name completion
- [ ] In Buddies → New Instant Message / Get User Info / View User Log,
      typing the start of a buddy name or alias shows suggestions under
      the entry; Up/Down/Enter picks one and selects its account.

## M9: Discord and Steam (with the patched plugins, accounts signed in)

The patched plugins are on the branches `pidgin4-message-meta`
(`~/purple-discord-pidgin4`) and `pidgin4-rich-presence`
(`~/pidgin-opensteamworks-pidgin4/steam-mobile`); see M9 in
`doc/PIDGIN-UPGRADE.md` for building them. To try them without touching
the `.so` files Pidgin 2 uses, point the dev profile's plugins at the
worktree builds:

```sh
ln -sf ~/purple-discord-pidgin4/libdiscord.so ~/.purple-gtk4/plugins/libdiscord.so
ln -sf ~/pidgin-opensteamworks-pidgin4/steam-mobile/libsteam.so ~/.purple-gtk4/plugins/libsteam.so
```

Quit Pidgin 2 first (Discord and Steam don't like two sessions; see
*Before you start*), and sign in one account at a time with `-l`.

### Discord
Use a second Discord client (the web app or the phone) as the other side,
in a DM and in a small server channel.

- [ ] Messages from the other client appear once. Reopening a channel (the
      plugin fetches its history again) or a gateway reconnect doesn't
      repeat them.
- [ ] Your own messages appear right after the server confirms them (a
      short delay; Discord assigns the id). A failed send (e.g. a channel
      you can't write to) shows "Unable to send message: …". Messages you
      send from the other client appear as sent ("outgoing").
- [ ] Replies from the other client show the replied-to message above the
      row (its text, even when that message isn't loaded here); there is
      no "┌──@name: …" line any more.
- [ ] Reply from the row menu: the other client shows a Discord reply to
      that message.
- [ ] An edit on the other side updates the row ("edited" marker) instead
      of an `EDIT:` line. Up-arrow edits your last message; the other
      client shows the new text.
- [ ] Reactions from the other side appear as chips under the message
      (custom emoji as `:name:`); removing them works. Reacting here, and
      removing your reaction, shows up on the other side; custom server
      emoji work if they were seen before (in a message or a reaction).
- [ ] Deleting one of your messages here deletes it on Discord; a deletion
      on the other side shows "This message was deleted." (for messages
      shown in this session; older ones get the usual "Message at … was
      deleted" line).
- [ ] Custom emoji in messages render as images, and image attachments as
      inline images (from `cdn.discordapp.com`/`media.discordapp.net`),
      with a link to the file. Spoiler images stay links.
- [ ] Threads look as before (indicator and colour-coded timestamp).
- [ ] The HTML log has the readable lines for edits, reactions and
      deletions (`X edited: …`, `X reacted 👍 to: …`); open it in Pidgin
      2's log viewer.
- [ ] Afterwards, with the same `.so` files, Pidgin 2 on the dev profile
      still shows replies as quote lines, edits as `EDIT:` lines and
      reactions as text: its output is unchanged.

### Steam
- [ ] A friend who is playing shows the game emblem on the buddy row and
      "In game <name>" as the secondary line (large list), and the game in
      the tooltip. When they stop, the emblem and the line go.
- [ ] A non-Steam game shows its name too ("In non-Steam game …").
- [ ] Steam Guard and login are unchanged; Pidgin 2 shows friends' games
      as before.

## M7: plugins (with accounts signed in)

The plugins install into `<prefix>/lib/pidgin4`. Load them in Tools →
Plugins (M5), or, before M5, by listing their paths in
`/pidgin4/plugins/loaded` in the scratch profile's `prefs.xml`. pidgin4
never loads anything from `/pidgin/plugins/loaded`.

### The plugins in use
- [ ] With cap, history, markerline, notify and timestamp_format loaded,
      quit and start again: all five are loaded (`-d` log: "Loading saved
      plugin …/lib/pidgin4/…"), and Pidgin 2 on the same profile still
      loads its own list.
- [ ] **history**: open an IM with someone you talked to before: the end
      of the last conversation is shown dimmed, with a header and a grey
      line, before anything new. The same for a chat you have a log of.
      With logging off for IMs the plugin warns once and shows nothing.
- [ ] **markerline**: with a conversation open, focus another window;
      have someone write: a red line separates what arrived while you
      were away. Switching tabs moves it too. Conversation → More →
      Jump to markerline scrolls to it. The IM/chat prefs switch it.
- [ ] **timestamp_format**: set "24 hour" and dates "Always": row
      timestamps change at once (e.g. `(2026-09-23 14:05:09)`), and new
      log lines use the log format. Right click a message → Timestamp
      Format Options opens the prefs.
- [ ] **notify**: with "Prepend string" and "Insert count" on, a message
      in an unfocused conversation window makes its title
      `(*)[1] name`; focusing, clicking or typing in it (per the prefs)
      or sending a message clears it. "Raise"/"Present" bring the window
      up (Sway marks it urgent instead of focusing it). "Show a desktop
      notification" sends one per conversation (mako/GNOME).
- [ ] **cap**: after messaging a buddy and getting an answer, their
      tooltip shows "Response Probability"; `<profile>/pidgin4/cap.db`
      grows; Pidgin 2's `<profile>/cap.db` is untouched.

### The other ports
- [ ] **convcolors**: sent/received/system/error/highlighted bodies take
      the configured colours and styles; "Ignore incoming format" drops
      a buddy's fonts and colours; the colour buttons open the GTK
      colour chooser.
- [ ] **spellchk**: typing `teh ` gives `the `; ending a message with a
      listed word corrects it and needs a second Enter; the editor adds,
      edits (double click a cell) and deletes words, and `<profile>/dict`
      reads back in Pidgin 2.
- [ ] **sendbutton**: a Send button appears (insensitive while the entry
      is empty) and goes away when the plugin is unloaded.
- [ ] **gtkbuddynote**: a buddy's menu has Edit Notes...; the note shows
      in the tooltip and in Pidgin 2.
- [ ] **timestamp**: time rows appear between messages at the interval.
- [ ] **iconaway**: going Away hides the buddy list (GNOME also
      minimizes the conversation windows; Sway doesn't minimize).
- [ ] **xmppconsole** (Tools → XMPP Console, with an XMPP account
      signed in): stanzas scroll by, tinted by direction; the popovers
      fill in stanzas; Enter sends; invalid XML turns the entry red; the
      account drop-down appears with two XMPP accounts.
- [ ] **xmppdisco** (Tools → XMPP Service Discovery): Browse asks for a
      server and lists its services; expanding a row loads its items;
      Register and Add to Buddy List work (buttons and right click).
- [ ] Unload every plugin in the dialog with conversations open: no
      criticals, and the markers, time rows' styling and send button go.

## Developer aids

For headless test runs only:
- `PIDGIN4_REQUEST_SELFTEST=1` opens one request of every kind at startup.
- `PIDGIN4_ACCOUNT_SELFTEST=1` opens the accounts window, then opens and
  cancels every account editor.
- `PIDGIN4_BLIST_SELFTEST=1` shows every node (Show → Offline Buddies,
  Empty Groups and Buddies of Disconnected Accounts), collapses and
  expands every group, builds every context menu and tooltip, toggles
  every Show option and sort method, logs the counts
  (`gtkblist: selftest: ...`, e.g. "visible 72 groups, 319 contacts"),
  restores the prefs and group states, and quits.
- `PIDGIN4_STATUS_SELFTEST=1` picks Away with a message through the
  status box and checks the current saved status, then quits. It only
  runs when **no account is enabled** (otherwise it logs "skipped"), so
  use it on a scratch profile whose accounts are all disabled.

- `PIDGIN4_DOCKLET_SELFTEST=<seconds>` adds a stand-in conversation with
  3 unread messages that only the tray sees (pending icon,
  NeedsAttention, tooltip, Unread Messages menu) and clears it after
  the given time. With `PIDGIN4_DOCKLET_CLOSE_BLIST=1` it then closes the
  buddy list: hidden into the tray when there is one, else pidgin4 quits.
- `build-pidgin4/fake-sni-watcher` is a minimal
  org.kde.StatusNotifierWatcher: start it inside `dbus-run-session`
  before pidgin4 to get a tray headless (it prints registrations).
- `PIDGIN4_IDLE_TIMEOUT=<seconds>` shortens the ext-idle-notify threshold
  (default 60); `PIDGIN4_IDLE_DEBUG=1` logs a sync round trip through the
  idle event queue every 5 s.
- `PIDGIN4_SOUND_SELFTEST=1` plays the default receive sound at startup
  (even when muted; you will hear it).
- `PIDGIN4_NOTIFY_SELFTEST=1` sends one test notification and withdraws
  it after 5 s.
- `PIDGIN4_CONV_SELFTEST=1` (M4) registers an in-process protocol
  plugin (`prpl-pidgin4-selftest`, which logs in without a network and
  records the M8 IPC calls) and a throwaway account on it, and then
  checks conversations of both types: writes with every flag, the
  metadata signals (ids, dedup by id and fuzzily, corrections, reactions,
  receipts, retractions, moderation), the entry (send, commands,
  Up-arrow correction, reply), the hover action bar (buttons per
  message kind, React/Reply/Edit, focus back in the entry), the IPC
  calls, the contract rule 7 log lines (read back from the log file), the index rows and log offsets,
  index scroll-back, chat users and the topic, tabs and window actions,
  unseen state, hidden conversations and detaching. It removes the
  account and quits with status 0 when all checks pass ("PASS (N
  checks)"). It writes logs and `messages.db` rows, so run it on a
  **scratch copy** of the profile (logs/ can be left out).
  `PIDGIN4_CONV_SELFTEST_HOLD=N` pauses N seconds with the chat tab and
  then the IM tab current (for screenshots or xdotool key presses).
- `PIDGIN4_WINDOWS_SELFTEST=1` (M5) opens every M5 window in turn and
  quits with status 0, or 1 if a step failed: it walks every
  preferences page (and checks that opening it changes no pref), opens
  the pounce and status editors (cancelled), the log viewer with a
  search on the profile's logs (read-only), privacy, room list,
  certificates (views every tls_peers certificate), file transfers,
  custom smileys, plugins (toggles Psychic Mode on and off and checks
  that only `/pidgin4/plugins/loaded` changes), About and OMEMO, and
  (module `secondary`) the accounts window and editor, the debug window,
  the Add Buddy/Group and gtkdialogs.c requests, a request, a formatted
  notification and a dialog opened with the buddy list hidden. Every
  window mapped on the way must have a transient parent, except the
  buddy list and conversation windows (`PIDGIN4_SELFTEST_SECONDARY=off`:
  with `/pidgin4/windows/secondary_transient` off, none may be a dialog
  of the buddy list). A comma-separated list (`prefs,log`) runs only
  those modules. `PIDGIN4_SELFTEST_HOLD=N` pauses the `secondary` module
  N seconds twice (for `swaymsg -t get_tree`).
  `PIDGIN4_SELFTEST_SHOTS=DIR` saves screenshots of the windows.
  `scripts/run-pidgin4-selftest.sh BIN PROFILE [TIMEOUT]` runs a binary
  this way on a private Xvfb (or, with `PIDGIN4_SELFTEST_WAYLAND=1`, on
  the current Wayland session).

- `PIDGIN4_PLUGINS_SELFTEST=1` (M7) loads every ported plugin from
  `<prefix>/lib/pidgin4` (or `PIDGIN4_PLUGINS_SELFTEST_DIR`, e.g.
  `build-pidgin4/plugins`; `PIDGIN4_PLUGINS_SELFTEST_SKIP=a,b` leaves
  some out), reports the ones `/pidgin4/plugins/loaded` loaded at
  startup, and on the selftest protocol checks: history rows from a
  conversation's last log (whole, and the tail through the index), the
  marker after a tab switch and a focus change, timestamp_format in a
  row's label and its menu item, notify's title prefix and its removal,
  cap rows in `<profile>/pidgin4/cap.db` and the tooltip line,
  buddynote's menu item and tooltip, spellchk (typing and on send),
  sendbutton, convcolors' classes, timestamp's rows, the XMPP console and
  disco windows, every config frame, unloading, and that
  `/pidgin/plugins/loaded` did not change. It restores the prefs it set
  and quits with status 0 ("PASS (N checks)"). Scratch profile only: it
  writes logs, index rows and `cap.db`.

None of them signs anything in. See `scripts/check-profile-compat.sh`
for the Xvfb setup (`GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`,
`dbus-run-session`).
