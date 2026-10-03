/*
 * starter-configurable-service - log.c
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

#include <stdio.h>

#include "internals.h"

static char *instance_tag = NULL;

static void stcs_log_handler (const gchar * log_domain,
			      GLogLevelFlags log_level,
			      const gchar * message, 
			      gpointer user_data)
{
  GDateTime *now = g_date_time_new_now_local ();
  gchar *timestamp = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");

  (void) user_data;

  const gchar *level_str = "UNKNOWN";
  const gchar *color_start = "";
  const gchar *color_end = "\033[0m";

  /* Mappatura dei livelli di log GLib con etichette e colori ANSI per terminale */
  if (log_level & G_LOG_LEVEL_ERROR)
    {
      level_str = "ERROR";
      color_start = "\033[1;31m";	/* Rosso */
    }
  else if (log_level & G_LOG_LEVEL_CRITICAL)
    {
      level_str = "CRITICAL";
      color_start = "\033[1;35m";	/* Magenta */
    }
  else if (log_level & G_LOG_LEVEL_WARNING)
    {
      level_str = "WARN";
      color_start = "\033[1;33m";	/* Giallo */
    }
  else if (log_level & G_LOG_LEVEL_MESSAGE)
    {
      level_str = "MESSAGE";
      color_start = "\033[1;32m";	/* Verde */
    }
  else if (log_level & G_LOG_LEVEL_INFO)
    {
      level_str = "INFO";
      color_start = "\033[1;36m";	/* Ciano */
    }
  else if (log_level & G_LOG_LEVEL_DEBUG)
    {
      level_str = "DEBUG";
      color_start = "\033[0;37m";	/* Grigio */
    }

  /* Stampa formattata su stderr */
  g_printerr ("[%s] %s[%-8s]%s [%s:%s] %s\n",
	      timestamp,
	      color_start, level_str, color_end,
	      instance_tag,
	      log_domain, 
	      message);

  g_free (timestamp);
  g_date_time_unref (now);
}

void stcs_internals_log_init(const char *_instance_tag)
{
  g_free(instance_tag);

  instance_tag = g_strdup(_instance_tag ? _instance_tag : "unknown");

  g_log_set_default_handler(stcs_log_handler, NULL);
}

