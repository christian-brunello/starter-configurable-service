/*
 * libstarter-configurable-service - date-text-to-value.c
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

#define _GNU_SOURCE

#include "date-text-to-value.h"

#include <string.h>
#include <time.h>

#define LOGD(msg...) g_log("date-text-to-value", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("date-text-to-value", G_LOG_LEVEL_WARNING, msg)

typedef enum {
  STCS_DATE_VALUE_UNIX = 0,
  STCS_DATE_VALUE_UNIX_MS,
  STCS_DATE_VALUE_DAY_MINUTES,
  STCS_DATE_VALUE_DAY_SECONDS,
  STCS_DATE_VALUE_DAY_MS
} STCSDateValueKind;

typedef struct {
  STCSTextToValue parent;
  gchar *format;
  gboolean format_is_iso8601;
  gchar *timezone_id;
  gchar *local_timezone_id;
  GTimeZone *timezone;
  GTimeZone *local_timezone;
  STCSDateValueKind value_kind;
} STCSDateTextToValue;

static const gchar *
stcs_date_value_kind_name(STCSDateValueKind kind)
{
  switch (kind)
    {
    case STCS_DATE_VALUE_UNIX: return "unix";
    case STCS_DATE_VALUE_UNIX_MS: return "unix-ms";
    case STCS_DATE_VALUE_DAY_MINUTES: return "day-minutes";
    case STCS_DATE_VALUE_DAY_SECONDS: return "day-seconds";
    case STCS_DATE_VALUE_DAY_MS: return "day-ms";
    default: return "?";
    }
}

static gboolean
stcs_date_parse_child_string(xmlNodePtr node, gchar **out, GError **error)
{
  xmlChar *content;

  content = xmlNodeGetContent(node);
  if (content == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has empty content",
		  (const char *) node->name);
      return FALSE;
    }

  g_strstrip((gchar *) content);
  if (*content == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has empty content",
		  (const char *) node->name);
      xmlFree(content);
      return FALSE;
    }

  g_free(*out);
  *out = g_strdup((const char *) content);
  xmlFree(content);
  return TRUE;
}

static gboolean
stcs_date_parse_value_kind(const gchar *text, STCSDateValueKind *out, GError **error)
{
  if (g_strcmp0(text, "unix") == 0)
    {
      *out = STCS_DATE_VALUE_UNIX;
      return TRUE;
    }
  if (g_strcmp0(text, "unix-ms") == 0)
    {
      *out = STCS_DATE_VALUE_UNIX_MS;
      return TRUE;
    }
  if (g_strcmp0(text, "day-minutes") == 0)
    {
      *out = STCS_DATE_VALUE_DAY_MINUTES;
      return TRUE;
    }
  if (g_strcmp0(text, "day-seconds") == 0)
    {
      *out = STCS_DATE_VALUE_DAY_SECONDS;
      return TRUE;
    }
  if (g_strcmp0(text, "day-ms") == 0)
    {
      *out = STCS_DATE_VALUE_DAY_MS;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported date value kind `%s' "
	      "(expected unix, unix-ms, day-minutes, day-seconds, day-ms)",
	      text);
  return FALSE;
}

static GTimeZone *
stcs_date_load_timezone(const gchar *id, GError **error)
{
  GTimeZone *tz;

  if (id == NULL || *id == '\0')
    return g_time_zone_new_local();

  if (g_ascii_strcasecmp(id, "UTC") == 0 || g_ascii_strcasecmp(id, "GMT") == 0)
    return g_time_zone_new_utc();

  tz = g_time_zone_new_identifier(id);
  if (tz == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "unknown timezone `%s'", id);
      return NULL;
    }

  return tz;
}

static gdouble
stcs_date_day_offset(GDateTime *local_dt, STCSDateValueKind kind)
{
  gint hour = g_date_time_get_hour(local_dt);
  gint minute = g_date_time_get_minute(local_dt);
  gint second = g_date_time_get_second(local_dt);
  gint usec = g_date_time_get_microsecond(local_dt);

  switch (kind)
    {
    case STCS_DATE_VALUE_DAY_MINUTES:
      return (gdouble) (hour * 60 + minute);
    case STCS_DATE_VALUE_DAY_SECONDS:
      return (gdouble) (hour * 3600 + minute * 60 + second);
    case STCS_DATE_VALUE_DAY_MS:
      return (gdouble) (hour * 3600 + minute * 60 + second) * 1000.0
	+ (gdouble) usec / 1000.0;
    default:
      return 0.0;
    }
}

static GDateTime *
stcs_date_parse_text(STCSDateTextToValue *self, const gchar *text, GError **error)
{
  GDateTime *dt = NULL;

  if (self->format_is_iso8601)
    {
      LOGD("parse iso8601 text=`%s' timezone=`%s'", text, self->timezone_id);
      dt = g_date_time_new_from_iso8601(text, self->timezone);
      if (dt == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "failed to parse ISO-8601 date `%s'", text);
	}
      else
	{
	  LOGD("parsed iso8601 -> %04d-%02d-%02d %02d:%02d:%02d tz=`%s'",
	       g_date_time_get_year(dt),
	       g_date_time_get_month(dt),
	       g_date_time_get_day_of_month(dt),
	       g_date_time_get_hour(dt),
	       g_date_time_get_minute(dt),
	       g_date_time_get_second(dt),
	       self->timezone_id);
	}
      return dt;
    }

  {
    GDateTime *now;
    struct tm tm;
    const gchar *end;
    gchar *stripped = NULL;

    LOGD("parse format=`%s' text=`%s' timezone=`%s'",
	 self->format, text, self->timezone_id);

    memset(&tm, 0, sizeof(tm));
    now = g_date_time_new_now(self->timezone);
    if (now == NULL)
      {
	g_set_error(error,
		    G_IO_ERROR,
		    G_IO_ERROR_FAILED,
		    "unable to get current time in timezone");
	return NULL;
      }

    tm.tm_year = g_date_time_get_year(now) - 1900;
    tm.tm_mon = g_date_time_get_month(now) - 1;
    tm.tm_mday = g_date_time_get_day_of_month(now);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    g_date_time_unref(now);

    stripped = g_strdup(text);
    g_strstrip(stripped);

    end = strptime(stripped, self->format, &tm);
    if (end == NULL)
      {
	g_set_error(error,
		    G_IO_ERROR,
		    G_IO_ERROR_INVALID_DATA,
		    "failed to parse date `%s' with format `%s'",
		    text, self->format);
	g_free(stripped);
	return NULL;
      }

    while (*end != '\0' && g_ascii_isspace(*end))
      end++;
    if (*end != '\0')
      {
	g_set_error(error,
		    G_IO_ERROR,
		    G_IO_ERROR_INVALID_DATA,
		    "trailing junk after date `%s'", text);
	g_free(stripped);
	return NULL;
      }
    g_free(stripped);

    dt = g_date_time_new(self->timezone,
			 tm.tm_year + 1900,
			 tm.tm_mon + 1,
			 tm.tm_mday,
			 tm.tm_hour,
			 tm.tm_min,
			 (gdouble) tm.tm_sec);
    if (dt == NULL)
      {
	g_set_error(error,
		    G_IO_ERROR,
		    G_IO_ERROR_INVALID_DATA,
		    "invalid date components from `%s'", text);
      }
    else
      {
	LOGD("parsed format -> %04d-%02d-%02d %02d:%02d:%02d tz=`%s'",
	     g_date_time_get_year(dt),
	     g_date_time_get_month(dt),
	     g_date_time_get_day_of_month(dt),
	     g_date_time_get_hour(dt),
	     g_date_time_get_minute(dt),
	     g_date_time_get_second(dt),
	     self->timezone_id);
      }
    return dt;
  }
}

static STCSTextToValue *
stcs_date_text_to_value_create(void)
{
  return (STCSTextToValue *) g_new0(STCSDateTextToValue, 1);
}

static void
stcs_date_text_to_value_destroy(STCSTextToValue *transform)
{
  STCSDateTextToValue *self = (STCSDateTextToValue *) transform;

  g_free(self->format);
  g_free(self->timezone_id);
  g_free(self->local_timezone_id);
  g_clear_pointer(&self->timezone, g_time_zone_unref);
  g_clear_pointer(&self->local_timezone, g_time_zone_unref);
  g_free(self);
}

static gboolean
stcs_date_text_to_value_configure(STCSTextToValue *transform,
				  xmlNodePtr node,
				  GError **error)
{
  STCSDateTextToValue *self = (STCSDateTextToValue *) transform;
  xmlNodePtr child;
  gchar *format = NULL;
  gchar *timezone = NULL;
  gchar *local_timezone = NULL;
  gchar *value = NULL;
  gboolean value_set = FALSE;
  gboolean r = FALSE;

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "format"))
	{
	  if (!stcs_date_parse_child_string(child, &format, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "timezone"))
	{
	  if (!stcs_date_parse_child_string(child, &timezone, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "local-timezone"))
	{
	  if (!stcs_date_parse_child_string(child, &local_timezone, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "value"))
	{
	  if (!stcs_date_parse_child_string(child, &value, error))
	    goto out;
	  if (!stcs_date_parse_value_kind(value, &self->value_kind, error))
	    goto out;
	  value_set = TRUE;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside date text-to-value",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (format == NULL || *format == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "date text-to-value requires a <format> element");
      goto out;
    }

  if (!value_set)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "date text-to-value requires a <value> element");
      goto out;
    }

  self->format = g_strdup(format);
  self->format_is_iso8601 = (g_ascii_strcasecmp(format, "iso8601") == 0);

  self->local_timezone = stcs_date_load_timezone(local_timezone, error);
  if (self->local_timezone == NULL)
    goto out;
  self->local_timezone_id = g_strdup(local_timezone != NULL
				     ? local_timezone
				     : g_time_zone_get_identifier(self->local_timezone));

  if (timezone != NULL)
    {
      self->timezone = stcs_date_load_timezone(timezone, error);
      if (self->timezone == NULL)
	goto out;
      self->timezone_id = g_strdup(timezone);
    }
  else
    {
      self->timezone = g_time_zone_ref(self->local_timezone);
      self->timezone_id = g_strdup(self->local_timezone_id);
    }

  LOGD("configured format=`%s' timezone=`%s' local-timezone=`%s' value=`%s'",
       self->format,
       self->timezone_id,
       self->local_timezone_id,
       stcs_date_value_kind_name(self->value_kind));

  r = TRUE;

out:
  g_free(format);
  g_free(timezone);
  g_free(local_timezone);
  g_free(value);
  return r;
}

static gboolean
stcs_date_text_to_value_to_value(STCSTextToValue *transform,
				 const gchar *text,
				 gdouble *out,
				 GError **error)
{
  STCSDateTextToValue *self = (STCSDateTextToValue *) transform;
  GDateTime *parsed = NULL;
  GDateTime *local = NULL;
  gboolean r = FALSE;

  LOGD("to_value text=`%s' value-kind=`%s'",
       text, stcs_date_value_kind_name(self->value_kind));

  parsed = stcs_date_parse_text(self, text, error);
  if (parsed == NULL)
    {
      LOGW("parse failed for text=`%s'", text);
      return FALSE;
    }

  switch (self->value_kind)
    {
    case STCS_DATE_VALUE_UNIX:
      *out = (gdouble) g_date_time_to_unix(parsed);
      r = TRUE;
      break;

    case STCS_DATE_VALUE_UNIX_MS:
      *out = (gdouble) g_date_time_to_unix(parsed) * 1000.0
	+ (gdouble) g_date_time_get_microsecond(parsed) / 1000.0;
      r = TRUE;
      break;

    case STCS_DATE_VALUE_DAY_MINUTES:
    case STCS_DATE_VALUE_DAY_SECONDS:
    case STCS_DATE_VALUE_DAY_MS:
      local = g_date_time_to_timezone(parsed, self->local_timezone);
      if (local == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_FAILED,
		      "failed to convert date to local-timezone");
	  break;
	}
      LOGD("local wall time %02d:%02d:%02d in `%s'",
	   g_date_time_get_hour(local),
	   g_date_time_get_minute(local),
	   g_date_time_get_second(local),
	   self->local_timezone_id);
      *out = stcs_date_day_offset(local, self->value_kind);
      r = TRUE;
      break;
    }

  if (r)
    LOGD("to_value result=%g (%s)", *out, stcs_date_value_kind_name(self->value_kind));
  else
    LOGW("to_value failed for text=`%s'", text);

  if (local != NULL)
    g_date_time_unref(local);
  g_date_time_unref(parsed);
  return r;
}

const STCSTextToValueClass stcs_date_text_to_value_class = {
  .type = "date",
  .create = stcs_date_text_to_value_create,
  .destroy = stcs_date_text_to_value_destroy,
  .configure = stcs_date_text_to_value_configure,
  .to_value = stcs_date_text_to_value_to_value,
};
