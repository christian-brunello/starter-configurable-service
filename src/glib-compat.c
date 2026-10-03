/*
 * starter-configurable-service - glib-compat.c
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

#include <string.h>
#include <glib.h>

#ifdef HAVE_CONFIG_H
#include "autoconf.h"
#endif

/* libstarter-core was built against a newer GLib that provides g_memdup2.
 * On GLib < 2.68 provide a compatible symbol; with --export-dynamic the
 * dynamic linker can resolve libstarter-core's reference from the main binary.
 */
#if !GLIB_CHECK_VERSION(2, 68, 0)
gpointer
g_memdup2(gconstpointer mem, gsize byte_size)
{
  gpointer new_mem;

  if (mem == NULL || byte_size == 0)
    return NULL;

  new_mem = g_malloc(byte_size);
  memcpy(new_mem, mem, byte_size);
  return new_mem;
}
#endif

#ifndef HAVE_G_STRING_REPLACE
/*
 * Portable stand-in for GLib >= 2.68 g_string_replace().
 * Replaces non-overlapping occurrences of @find with @replace, up to @limit
 * (0 = unlimited). Returns the number of replacements performed.
 */
guint
g_string_replace(GString *string,
		 const gchar *find,
		 const gchar *replace,
		 guint limit)
{
  gsize find_len;
  gsize replace_len;
  gsize pos = 0;
  guint n = 0;

  g_return_val_if_fail(string != NULL, 0);
  g_return_val_if_fail(find != NULL, 0);
  g_return_val_if_fail(replace != NULL, 0);

  find_len = strlen(find);
  if (find_len == 0)
    return 0;

  replace_len = strlen(replace);

  while (pos + find_len <= string->len && (limit == 0 || n < limit))
    {
      gchar *match = strstr(string->str + pos, find);

      if (match == NULL)
	break;

      pos = (gsize) (match - string->str);
      g_string_erase(string, pos, find_len);
      g_string_insert_len(string, pos, replace, replace_len);
      pos += replace_len;
      n++;
    }

  return n;
}
#endif
