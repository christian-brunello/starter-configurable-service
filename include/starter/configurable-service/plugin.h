/*
 * libstarter-configurable-service - plugin.h
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

#ifndef STCS_PLUGIN_H_INCLUDED
#define STCS_PLUGIN_H_INCLUDED

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum _STCSPluginCategory {
    STCS_PLUGIN_CATEGORY_NULL = 0,
    STCS_PLUGIN_CATEGORY_TRANSPORT,
    STCS_PLUGIN_CATEGORY_READER,
    STCS_PLUGIN_CATEGORY_WRITER,
    STCS_PLUGIN_CATEGORY_TRANSFORM
} STCSPluginCategory;

const char *stcs_plugin_info_category_to_string(STCSPluginCategory cat);

#define STCS_TYPE_PLUGIN_INFO (stcs_plugin_info_get_type())
G_DECLARE_FINAL_TYPE(STCSPluginInfo, stcs_plugin_info, STCS, PLUGIN_INFO, GObject)

STCSPluginInfo *stcs_plugin_info_new(const gchar *name, 
				     STCSPluginCategory category,
                                     const gchar *version);

const gchar *stcs_plugin_info_get_name(STCSPluginInfo *self);
STCSPluginCategory stcs_plugin_info_get_category(STCSPluginInfo *self);
const gchar *stcs_plugin_info_get_version(STCSPluginInfo *self);

typedef STCSPluginInfo * STCSPluginGetInfoFunc(void);
typedef GObject * (*STCSPluginCreateFunc)(void);

G_END_DECLS

#endif /* STCS_PLUGIN_H_INCLUDED */
