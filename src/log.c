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
#include <string.h>

#include "internals.h"

static char *instance_tag = NULL;

/*
 * Match GLib's default writer: DEBUG and INFO are silent unless the
 * domain (or "all") appears in G_MESSAGES_DEBUG. Without this filter the
 * custom handler floods journald on every poll/datapoint push.
 */
static gboolean
stcs_log_debug_enabled(const gchar *log_domain)
{
  const gchar *domains;
  const gchar *p;
  gsize domain_len;

  domains = g_getenv("G_MESSAGES_DEBUG");
  if (domains == NULL || *domains == '\0')
    return FALSE;

  if (strcmp(domains, "all") == 0)
    return TRUE;

  if (log_domain == NULL || *log_domain == '\0')
    return FALSE;

  domain_len = strlen(log_domain);
  p = domains;
  while (*p != '\0')
    {
      while (*p == ' ')
	p++;
      if (*p == '\0')
	break;
      if (strncmp(p, log_domain, domain_len) == 0 &&
	  (p[domain_len] == '\0' || p[domain_len] == ' '))
	return TRUE;
      while (*p != '\0' && *p != ' ')
	p++;
    }

  return FALSE;
}

static void
stcs_log_handler(const gchar *log_domain,
		 GLogLevelFlags log_level,
		 const gchar *message,
		 gpointer user_data)
{
  GDateTime *now = NULL;
  gchar *timestamp = NULL;
  const gchar *level_str = "UNKNOWN";
  const gchar *color_start = "";
  const gchar *color_end = "\033[0m";

  (void) user_data;

  if ((log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO)) != 0 &&
      !stcs_log_debug_enabled(log_domain))
    return;

  now = g_date_time_new_now_local();
  timestamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");

  if (log_level & G_LOG_LEVEL_ERROR)
    {
      level_str = "ERROR";
      color_start = "\033[1;31m";
    }
  else if (log_level & G_LOG_LEVEL_CRITICAL)
    {
      level_str = "CRITICAL";
      color_start = "\033[1;35m";
    }
  else if (log_level & G_LOG_LEVEL_WARNING)
    {
      level_str = "WARN";
      color_start = "\033[1;33m";
    }
  else if (log_level & G_LOG_LEVEL_MESSAGE)
    {
      level_str = "MESSAGE";
      color_start = "\033[1;32m";
    }
  else if (log_level & G_LOG_LEVEL_INFO)
    {
      level_str = "INFO";
      color_start = "\033[1;36m";
    }
  else if (log_level & G_LOG_LEVEL_DEBUG)
    {
      level_str = "DEBUG";
      color_start = "\033[0;37m";
    }

  g_printerr("[%s] %s[%-8s]%s [%s:%s] %s\n",
	     timestamp,
	     color_start, level_str, color_end,
	     instance_tag,
	     log_domain != NULL ? log_domain : "?",
	     message);

  g_free(timestamp);
  g_date_time_unref(now);
}

void
stcs_internals_log_init(const char *_instance_tag)
{
  g_free(instance_tag);

  instance_tag = g_strdup(_instance_tag ? _instance_tag : "unknown");

  g_log_set_default_handler(stcs_log_handler, NULL);
}
