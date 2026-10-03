/*
 * starter-configurable-service - plugin-entry.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef STCS_PLUGIN_ENTRY_H_INCLUDED
#define STCS_PLUGIN_ENTRY_H_INCLUDED

#include <glib.h>
#include <glib-object.h>

#include "plugin.h"

G_BEGIN_DECLS

#define STCS_TYPE_PLUGIN_ENTRY (stcs_plugin_entry_get_type())
G_DECLARE_FINAL_TYPE(STCSPluginEntry, stcs_plugin_entry, STCS, PLUGIN_ENTRY, GObject)

STCSPluginEntry *stcs_plugin_entry_new(const gchar *path, STCSPluginInfo *info);

STCSPluginEntry *stcs_plugin_entry_lookup(GHashTable *plugins, const gchar *name);

const gchar *stcs_plugin_entry_get_path(STCSPluginEntry *self);
STCSPluginInfo *stcs_plugin_entry_get_info(STCSPluginEntry *self);

/* Opens the module on first use, caches create(), returns a new instance (transfer full). */
GObject *stcs_plugin_entry_create(STCSPluginEntry *self, GError **error);

G_END_DECLS

#endif /* STCS_PLUGIN_ENTRY_H_INCLUDED */
