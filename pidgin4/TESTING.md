# pidgin4: manual checklists (M2 sign-in, M3 buddy list and status, M4 conversations)

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

The buddy list is the main window (M3). Closing it quits pidgin4 (saving
everything) unless `/pidgin4/blist/close_hides` is set in `prefs.xml`,
until the tray arrives in M6. Quit also with Ctrl+Q, Buddies → Quit, or
`kill -TERM`. Starting pidgin4 again raises the buddy list. The accounts
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
      image file on an IM asks to send it as a file or insert it; any
      other file is sent (HTTP upload on XMPP when the server has it).
      Our own upload's URL, and images on the account's own XMPP domain,
      show inline; Discord CDN images show inline.
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

- `PIDGIN4_CONV_SELFTEST=1` (M4) registers an in-process protocol
  plugin (`prpl-pidgin4-selftest`, which logs in without a network and
  records the M8 IPC calls) and a throwaway account on it, and then
  checks conversations of both types: writes with every flag, the
  metadata signals (ids, dedup by id and fuzzily, corrections, reactions,
  receipts, retractions, moderation), the entry (send, commands,
  Up-arrow correction, reply), the IPC calls, the contract rule 7 log
  lines (read back from the log file), the index rows and log offsets,
  index scroll-back, chat users and the topic, tabs and window actions,
  unseen state, hidden conversations and detaching. It removes the
  account and quits with status 0 when all checks pass ("PASS (N
  checks)"). It writes logs and `messages.db` rows, so run it on a
  **scratch copy** of the profile (logs/ can be left out).
  `PIDGIN4_CONV_SELFTEST_HOLD=N` pauses N seconds with the chat tab and
  then the IM tab current (for screenshots or xdotool key presses).

None of them signs anything in. See `scripts/check-profile-compat.sh`
for the Xvfb setup (`GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`,
`dbus-run-session`).
