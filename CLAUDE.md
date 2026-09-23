# CLAUDE.md

This tree is Pidgin 2.14.14. It is being upgraded into a personal GTK 4 build, with modernized libraries and modern XMPP support. The approved plan is in **`doc/PIDGIN-UPGRADE.md`**: read it before starting any milestone. `doc/GTK4-MIGRATION.md` is the background survey of GTK 2 → GTK 4 API changes.

## Target environment

- Gentoo Linux, Wayland. Primary is Sway 1.12 with Waybar; GNOME 49 is used occasionally. There is no X11, Windows or macOS target.
- GTK 4.22 (plain GTK, **no libadwaita**), GLib 2.88, GtkSourceView 5, libspelling, libsoup 3, libomemo-c.
- App ID: **`com.minowick.Pidgin4`**.

## Hard rules

- **libpurple ABI must stay compatible with the installed `libpurple.so.0` (2.14.x).** The user's third-party protocol plugins `~/.purple/plugins/libdiscord.so` and `libsteam.so` link against it. They use the `purple_ssl_*`, `purple_dnsquery_*`, `purple_proxy_*` and `purple_util_fetch_url_*` APIs.
  - Changes to libpurple must be additive only: new functions and new signals are fine. Removing or renaming symbols, changing signatures, or changing public struct layouts is not.
  - After every libpurple change, compare the exported symbols against the system copy. The new build must be a superset:
    ```sh
    diff <(nm -D --defined-only /usr/lib64/libpurple.so.0.14.14 | awk '{print $3}' | sort) \
         <(nm -D --defined-only <prefix>/lib/libpurple.so.0 | awk '{print $3}' | sort) | grep '^<'   # must print nothing
    ```
- **Never touch the system Pidgin or the real profile.** Install only into the private prefix (e.g. `~/.local/pidgin4`), never into `/usr`. Run test builds with a copied profile, `-c ~/.purple-gtk4`, and never against `~/.purple` until the user explicitly approves the cutover (see *Cutover gate* in the plan).
- **`~/.purple` must stay loadable by Pidgin 2.14.14.** pidgin4 will eventually share it with Pidgin 2. Follow the plan's *Profile compatibility contract*:
  - existing files keep their formats;
  - new keys are additive only, and pidgin4-specific prefs go under `/pidgin4/…`;
  - new files go only under `~/.purple/pidgin4/`;
  - nothing new goes in `~/.purple/plugins`.
- **The Discord and Steam plugins (`~/purple-discord`, `~/pidgin-opensteamworks`, the user's forks) must stay loadable in stock Pidgin 2.14.14.** Build them against the system `purple.pc` and use only 2.14 symbols. If a feature can't be done that way, drop it.
- **Work milestone by milestone** (M0–M9 in the plan). Don't pull later-milestone work forward unless asked.

## Layout (per the plan; parts of it don't exist yet)

- `libpurple/`: autotools, built into the private prefix with D-Bus, voice/video and finch disabled, and dynamic prpls `irc,jabber`.
- `pidgin/`: the existing GTK 2 UI. It is kept buildable against the modified libpurple as the fallback daily driver, and removed at the end.
- `pidgin4/`: the new GTK 4 UI, a standalone **Meson** project that finds libpurple via the prefix's `purple.pc` (`PKG_CONFIG_PATH`). It uses `export_dynamic: true` because plugins resolve symbols from the executable.
- `libpurple/protocols/jabber/`: target of the XMPP work (carbons, MAM, HTTP upload, receipts/markers, corrections, reactions, replies). OMEMO lives in `libpurple/plugins/omemo/`.

## Code conventions

- C with GLib/GObject, tabs for indentation, Pidgin's existing brace and naming style. New UI types use the `Pidgin` prefix (`PidginMessageView`, `pidgin_message_view_*`). Match the surrounding code.
- Replace deprecated GTK 4 widgets rather than porting to them: use `GtkListView`/`GtkColumnView`, not `GtkTreeView`; `GtkDropDown`, not `GtkComboBox`; async `GtkAlertDialog`/`GtkFileDialog`, not `gtk_dialog_run`.
- Use no X11-specific APIs in `pidgin4/`.

## Usage budget for multi-agent work

The account has a 5-hour session window and a 7-day weekly window, and **overage billing is off**. Hitting either limit halts all work until the reset: it doesn't spill onto paid usage. When orchestrating subagents or workflows (for example Fable fanning out milestone work), check headroom first and scale to it.

```sh
~/.claude/bin/claude-usage --json   # machine-readable
~/.claude/bin/claude-usage          # human-readable summary
```

Fields that matter:

- `headroom.state` is one of `ok`, `warning`, `exhausted` or `unknown`.
- `headroom.constraining_window` is whichever window is closest to its limit.
- `windows[]`:
  - `key` `unified-5h` is the session window.
  - `key` `unified-7d` is the weekly window.
  - Each entry has `used_pct` and `reset_in_seconds`.
- `last_1h.billable_tokens` is the recent burn rate.

If the command exits non-zero, the usage proxy is unreachable. Treat that as `unknown`.

How to scale:

1. **Check before any fan-out** (spawning agents or starting a workflow), and again between phases of long runs.
2. **Session window (`unified-5h`):**
   - Below 60%: normal parallelism.
   - 60–80%: at most 2 concurrent agents, and prefer cheaper models for search/read-only work.
   - Above 80%, or `state` is `warning`: no new fan-out. Finish work in progress, then checkpoint.
3. **Weekly window (`unified-7d`):** don't pace or throttle against it; the user has a stored reset to apply when it gets close. At 95% or above, don't start new fan-out: checkpoint and tell the user, so they can apply the reset before work halts.
4. **`exhausted`:** stop. Write a checkpoint and report `reset_in_seconds` to the user instead of retrying.
5. **`unknown`:** stay conservative, with at most 2 concurrent agents.
6. **Checkpoints** go on the working branch: commit the work in progress and note the next step in the milestone's section of `doc/PIDGIN-UPGRADE.md`. That way nothing is lost when a limit hits mid-milestone.
7. **Before a large workflow**, estimate its size from `last_1h.billable_tokens` per agent-hour. Skip it if it would push the session window past 80%.
