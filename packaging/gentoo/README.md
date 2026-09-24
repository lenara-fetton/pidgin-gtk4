# Gentoo packages

Live (9999) ebuilds for a local overlay. `net-im/pidgin-9999` replaces
`::gentoo`'s `net-im/pidgin` with this tree (`main`), in one package:

- **libpurple:** `/usr/lib64/libpurple.so.0`, the `irc` and `jabber` prpls,
  OMEMO and the other libpurple plugins in `/usr/lib64/purple-2`, and D-Bus
  (`purple-remote`).
- **pidgin4, with USE=gui (the default):** `/usr/bin/pidgin4`, the UI plugins
  in `/usr/lib64/pidgin4`, the desktop file, icons and smiley themes.

It has the same slot as the stock package (`0/2`). Like the stock ebuild's
GTK 2 UI, the GTK 4 UI is switched by `gui`. The GTK 2 Pidgin, finch and the
SIMPLE protocol are not built.

libpurple stays ABI-compatible with the stock 2.14.14 (every symbol the
stock library exports is still exported), so third-party plugins built
against either package keep loading.

The Discord and Steam plugins have their own live ebuilds in their own
repositories:

- `~/purple-discord`: `gentoo/x11-plugins/purple-discord`
- `~/pidgin-opensteamworks`: `gentoo/x11-plugins/pidgin-opensteamworks`

## Installing

1. Copy the package directories into the overlay, then generate their
   Manifests:

   ```sh
   sudo cp -r packaging/gentoo/net-im /usr/local/portage/
   sudo rm -rf /usr/local/portage/net-im/pidgin4   # from the earlier two-package layout
   sudo cp -r ~/purple-discord/gentoo/x11-plugins \
              ~/pidgin-opensteamworks/gentoo/x11-plugins /usr/local/portage/
   for e in /usr/local/portage/net-im/pidgin/*-9999.ebuild \
            /usr/local/portage/x11-plugins/{purple-discord,pidgin-opensteamworks}/*-9999.ebuild; do
       sudo ebuild "$e" manifest
   done
   ```

2. Accept the live versions in `/etc/portage/package.accept_keywords`:

   ```
   =net-im/pidgin-9999 **
   =x11-plugins/purple-discord-9999 **
   =x11-plugins/pidgin-opensteamworks-9999 **
   ```

3. Install:

   ```sh
   emerge -av net-im/pidgin x11-plugins/purple-discord x11-plugins/pidgin-opensteamworks
   ```

   Update later with `emerge -av @live-rebuild`, or re-emerge the packages
   you want to update.

## After installing

- **Remove the hand-built plugins** from `~/.purple/plugins`
  (`libdiscord.so`, `libsteam.so`). libpurple probes its own
  `/usr/lib64/purple-2` first and skips same-named copies (with a warning in
  the debug log), so the packaged ones win. An old copy would only take over
  if the packaged one failed to load, which is better avoided.
- **Remove the development install's shortcuts**, which shadow the packaged
  ones: `~/.local/bin/pidgin4` (it comes before `/usr/bin` in `PATH`) and
  `~/.local/share/applications/com.minowick.Pidgin4.desktop`. The prefix
  itself, `~/.local/pidgin4`, can stay for development builds.

## Going back to Pidgin 2

`~/.purple` stays readable by the stock Pidgin 2.14.14. To switch back, mask
`=net-im/pidgin-9999` and run:

```sh
emerge -av --oneshot '=net-im/pidgin-2.14.14*::gentoo'
```

The plugins need no rebuild: they use only the libpurple 2.14 API.

## Branches the live ebuilds build

- **pidgin (this repo):** `main`.
- **Discord and Steam:** each repo's default branch on GitHub (`master`).
  Their pidgin4 work is on the `pidgin4-message-meta` and
  `pidgin4-rich-presence` branches, which fast-forward from `master`.
  Push those to `master` for the ebuilds to pick them up. To build a branch
  instead, set it in the environment, for example in `/etc/portage/env`:

  ```sh
  EGIT_OVERRIDE_BRANCH_LENARA_FETTON_PURPLE_DISCORD=pidgin4-message-meta
  ```
