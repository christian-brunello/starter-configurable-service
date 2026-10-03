/*
 * starter-configurable-service - glib-compat.h
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

#ifndef STCS_GLIB_COMPAT_H_INCLUDED
#define STCS_GLIB_COMPAT_H_INCLUDED

#ifdef HAVE_CONFIG_H
#include "autoconf.h"
#endif

#include <glib.h>

G_BEGIN_DECLS

#ifndef HAVE_G_STRING_REPLACE
guint g_string_replace(GString *string,
		       const gchar *find,
		       const gchar *replace,
		       guint limit);
#endif

G_END_DECLS

#endif /* STCS_GLIB_COMPAT_H_INCLUDED */
