# pidgin4 M2: manual sign-in checklist

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
- **What M2 can't do yet.** There are no conversation windows (M4) and no
  buddy list (M3). Received messages are logged to `logs/` but not shown.
  This build is for signing in and staying connected, not for chatting.

## Starting

```sh
P=~/.local/pidgin4/bin/pidgin4
$P -c ~/.purple-gtk4 -d                  # all enabled accounts, debug window
$P -c ~/.purple-gtk4 -l 'user@host' -d   # enable/sign in only this account
$P -c ~/.purple-gtk4 -n                  # no sign-in at all
```

The accounts window is the main window until M3. Closing it hides it, and
pidgin4 keeps running. Quit with Ctrl+Q, the main menu → Quit, or
`kill -TERM`. Starting pidgin4 again brings the accounts window back.

## Checklist

### Startup and shutdown
- [ ] With Pidgin 2 running on `~/.purple`, `pidgin4` without `-c` refuses
      to start: a dialog says Pidgin 2 is running, and the exit status is 1.
      The same happens with `-c ~/.purple`, `-c ~/.purple/` or a relative
      path to it. With `-m` it starts anyway, so **don't** do that on
      `~/.purple`: use it only to check the dialog, then quit.
- [ ] `pidgin4 -v` prints the version, git revision and libpurple version.
- [ ] A second `pidgin4 -c ~/.purple-gtk4` raises the running instance's
      accounts window and exits.
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
- [ ] **Wrong password**: type a wrong one. You get an alert saying
      "<account> disconnected", with Close / Modify Account / Re-enable.
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
      doubling on repeated failures. No alert is shown for these
      non-fatal errors; the Status column shows the error while it waits.
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

## Developer aids

For headless test runs only:
- `PIDGIN4_REQUEST_SELFTEST=1` opens one request of every kind at startup.
- `PIDGIN4_ACCOUNT_SELFTEST=1` opens and cancels every account editor.

Neither saves anything or signs anything in. See `scripts/check-profile-compat.sh`
for the Xvfb setup (`GDK_BACKEND=x11`, `G_DEBUG=fatal-criticals`,
`dbus-run-session`).
