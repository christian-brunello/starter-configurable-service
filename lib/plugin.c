/*
 * libstarter-configurable-service - plugin.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "plugin-entry.h"

#include "internals.h"

#define LOGD(msg...) g_log("plugin", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("plugin", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("plugin", G_LOG_LEVEL_WARNING, msg)
#define LOGE(msg...) g_log("plugin", G_LOG_LEVEL_ERROR, msg)

struct _STCSPluginInfo {
    GObject parent_instance;
    gchar *name;
    STCSPluginCategory category;
    gchar *version;
};

G_DEFINE_TYPE(STCSPluginInfo, stcs_plugin_info, G_TYPE_OBJECT)

static void stcs_plugin_info_finalize(GObject *object) 
{
    STCSPluginInfo *self = STCS_PLUGIN_INFO(object);

    g_free(self->name);
    g_free(self->version);

    G_OBJECT_CLASS(stcs_plugin_info_parent_class)->finalize(object);
}

static void stcs_plugin_info_class_init(STCSPluginInfoClass *klass) 
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = stcs_plugin_info_finalize;
}

static void stcs_plugin_info_init(STCSPluginInfo *self) 
{
    self->name = NULL;
    self->category = STCS_PLUGIN_CATEGORY_NULL;
    self->version = NULL;
}

const char *stcs_plugin_info_category_to_string(STCSPluginCategory cat)
{
  static char *cat_strings[] = {
      [STCS_PLUGIN_CATEGORY_NULL] = "NULL",
      [STCS_PLUGIN_CATEGORY_TRANSPORT] = "transport",
      [STCS_PLUGIN_CATEGORY_READER] = "reader",
      [STCS_PLUGIN_CATEGORY_WRITER] = "writer",
      [STCS_PLUGIN_CATEGORY_TRANSFORM] = "transform"
  };

  if(cat >= 0 && cat < (int) (sizeof cat_strings / sizeof cat_strings[0]))
    return cat_strings[cat];

  return NULL;
}

STCSPluginInfo *stcs_plugin_info_new(const gchar *name,
                                     STCSPluginCategory category,
                                     const gchar *version)
{
  STCSPluginInfo *r = NULL;

  r = g_object_new(STCS_TYPE_PLUGIN_INFO, NULL);

  r->name = g_strdup(name);
  r->category = category;
  r->version = g_strdup(version);

  return r;
}

const gchar *stcs_plugin_info_get_name(STCSPluginInfo *self)
{
  return self->name;
}

STCSPluginCategory stcs_plugin_info_get_category(STCSPluginInfo *self)
{
  return self->category;
}

const gchar *stcs_plugin_info_get_version(STCSPluginInfo *self)
{
  return self->version;
}
