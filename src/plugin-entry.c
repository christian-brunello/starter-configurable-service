/*
 * starter-configurable-service - plugin-entry.c
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

#include <gmodule.h>
#include <gio/gio.h>

#include "plugin-entry.h"

struct _STCSPluginEntry {
  GObject parent_instance;
  gchar *path;
  STCSPluginInfo *info;
  GModule *module;
  STCSPluginCreateFunc create;
};

G_DEFINE_TYPE(STCSPluginEntry, stcs_plugin_entry, G_TYPE_OBJECT)

static void
stcs_plugin_entry_dispose(GObject *object)
{
  STCSPluginEntry *self = STCS_PLUGIN_ENTRY(object);

  self->create = NULL;

  if (self->module != NULL)
    {
      g_module_close(self->module);
      self->module = NULL;
    }

  g_clear_object(&self->info);

  G_OBJECT_CLASS(stcs_plugin_entry_parent_class)->dispose(object);
}

static void
stcs_plugin_entry_finalize(GObject *object)
{
  STCSPluginEntry *self = STCS_PLUGIN_ENTRY(object);

  g_free(self->path);

  G_OBJECT_CLASS(stcs_plugin_entry_parent_class)->finalize(object);
}

static void
stcs_plugin_entry_class_init(STCSPluginEntryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stcs_plugin_entry_dispose;
  object_class->finalize = stcs_plugin_entry_finalize;
}

static void
stcs_plugin_entry_init(STCSPluginEntry *self)
{
  self->path = NULL;
  self->info = NULL;
  self->module = NULL;
  self->create = NULL;
}

STCSPluginEntry *
stcs_plugin_entry_new(const gchar *path, STCSPluginInfo *info)
{
  STCSPluginEntry *self;

  g_return_val_if_fail(path != NULL, NULL);
  g_return_val_if_fail(STCS_IS_PLUGIN_INFO(info), NULL);

  self = g_object_new(STCS_TYPE_PLUGIN_ENTRY, NULL);
  self->path = g_strdup(path);
  self->info = g_object_ref(info);

  return self;
}

STCSPluginEntry *
stcs_plugin_entry_lookup(GHashTable *plugins, const gchar *name)
{
  gpointer entry;

  g_return_val_if_fail(plugins != NULL, NULL);
  g_return_val_if_fail(name != NULL, NULL);

  entry = g_hash_table_lookup(plugins, name);
  if (entry == NULL)
    return NULL;

  return STCS_PLUGIN_ENTRY(entry);
}

const gchar *
stcs_plugin_entry_get_path(STCSPluginEntry *self)
{
  g_return_val_if_fail(STCS_IS_PLUGIN_ENTRY(self), NULL);

  return self->path;
}

STCSPluginInfo *
stcs_plugin_entry_get_info(STCSPluginEntry *self)
{
  g_return_val_if_fail(STCS_IS_PLUGIN_ENTRY(self), NULL);

  return self->info;
}

static gboolean
stcs_plugin_entry_ensure_loaded(STCSPluginEntry *self, GError **error)
{
  if (self->create != NULL)
    return TRUE;

  if (self->module == NULL)
    {
      self->module = g_module_open(self->path, G_MODULE_BIND_LOCAL);
      if (self->module == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_FAILED,
		      "unable to open module %s: %s",
		      self->path,
		      g_module_error());
	  return FALSE;
	}
    }

  if (!g_module_symbol(self->module, "stcs_plugin_create", (gpointer *) &self->create)
      || self->create == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "unable to lookup symbol `stcs_plugin_create' from module at %s",
		  self->path);
      self->create = NULL;
      return FALSE;
    }

  return TRUE;
}

GObject *
stcs_plugin_entry_create(STCSPluginEntry *self, GError **error)
{
  GObject *object;

  g_return_val_if_fail(STCS_IS_PLUGIN_ENTRY(self), NULL);

  if (!stcs_plugin_entry_ensure_loaded(self, error))
    return NULL;

  object = self->create();
  if (object == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "stcs_plugin_create() returned NULL for module %s",
		  self->path);
      return NULL;
    }

  return object;
}
