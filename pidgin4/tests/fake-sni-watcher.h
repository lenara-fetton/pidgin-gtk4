/*
 * pidgin4: a minimal org.kde.StatusNotifierWatcher for tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef PIDGIN4_FAKE_SNI_WATCHER_H
#define PIDGIN4_FAKE_SNI_WATCHER_H

#include <gio/gio.h>

typedef struct _FakeWatcher FakeWatcher;

/** Exports the watcher on @connection and owns its well-known name. */
FakeWatcher *fake_watcher_new(GDBusConnection *connection, gboolean verbose);
void fake_watcher_free(FakeWatcher *watcher);

guint fake_watcher_get_n_items(FakeWatcher *watcher);
/** "<bus name>/StatusNotifierItem" of the @index'th item. */
const char *fake_watcher_get_item(FakeWatcher *watcher, guint index);

#endif
