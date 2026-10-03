/*
 * libstarter-configurable-service - value-to-date-text.c
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

#include "value-to-date-text.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define LOGD(msg...) g_log("value-to-date-text", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("value-to-date-text", G_LOG_LEVEL_WARNING, msg)

typedef enum {
  STCS_DATE_VALUE_UNIX = 0,
  STCS_DATE_VALUE_UNIX_MS,
  STCS_DATE_VALUE_DAY_MINUTES,
  STCS_DATE_VALUE_DAY_SECONDS,
  STCS_DATE_VALUE_DAY_MS
} STCSDateValueKind;

typedef struct {
  STCSValueToText parent;
  gchar *format;
  gboolean format_is_iso8601;
  gchar *timezone_id;
  gchar *local_timezone_id;
  GTimeZone *timezone;
  GTimeZone *local_timezone;
  STCSDateValueKind value_kind;
} STCSValueToDateText;

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

static GDateTime *
stcs_date_midnight_today(GTimeZone *local_tz, GError **error)
{
  GDateTime *now;
  GDateTime *midnight;

  now = g_date_time_new_now(local_tz);
  if (now == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "unable to get current time in local-timezone");
      return NULL;
    }

  midnight = g_date_time_new(local_tz,
			     g_date_time_get_year(now),
			     g_date_time_get_month(now),
			     g_date_time_get_day_of_month(now),
			     0, 0, 0.0);
  g_date_time_unref(now);

  if (midnight == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "unable to build local midnight");
    }
  else
    {
      LOGD("anchor midnight %04d-%02d-%02d in local-timezone",
	   g_date_time_get_year(midnight),
	   g_date_time_get_month(midnight),
	   g_date_time_get_day_of_month(midnight));
    }

  return midnight;
}

static GDateTime *
stcs_date_from_value(STCSValueToDateText *self, gdouble value, GError **error)
{
  GDateTime *dt = NULL;
  GDateTime *midnight = NULL;
  gint64 unix_s;
  gint usec;

  LOGD("from_value %g kind=`%s'", value, stcs_date_value_kind_name(self->value_kind));

  switch (self->value_kind)
    {
    case STCS_DATE_VALUE_UNIX:
      unix_s = (gint64) value;
      dt = g_date_time_new_from_unix_utc(unix_s);
      break;

    case STCS_DATE_VALUE_UNIX_MS:
      {
	gdouble seconds = value / 1000.0;
	unix_s = (gint64) floor(seconds);
	usec = (gint) llround((seconds - (gdouble) unix_s) * G_USEC_PER_SEC);
	if (usec < 0)
	  {
	    unix_s -= 1;
	    usec += G_USEC_PER_SEC;
	  }
	dt = g_date_time_new_from_unix_utc(unix_s);
	if (dt != NULL && usec != 0)
	  {
	    GDateTime *adjusted = g_date_time_add(dt, usec);
	    g_date_time_unref(dt);
	    dt = adjusted;
	  }
      }
      break;

    case STCS_DATE_VALUE_DAY_MINUTES:
      midnight = stcs_date_midnight_today(self->local_timezone, error);
      if (midnight == NULL)
	return NULL;
      dt = g_date_time_add_minutes(midnight, (gint) value);
      break;

    case STCS_DATE_VALUE_DAY_SECONDS:
      midnight = stcs_date_midnight_today(self->local_timezone, error);
      if (midnight == NULL)
	return NULL;
      dt = g_date_time_add_seconds(midnight, value);
      break;

    case STCS_DATE_VALUE_DAY_MS:
      midnight = stcs_date_midnight_today(self->local_timezone, error);
      if (midnight == NULL)
	return NULL;
      dt = g_date_time_add(midnight, (gint64) llround(value * 1000.0));
      break;
    }

  if (midnight != NULL)
    g_date_time_unref(midnight);

  if (dt == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "unable to build date-time from value %g", value);
      LOGW("from_value failed for %g (%s)",
	   value, stcs_date_value_kind_name(self->value_kind));
    }
  else
    {
      LOGD("from_value built %04d-%02d-%02d %02d:%02d:%02d",
	   g_date_time_get_year(dt),
	   g_date_time_get_month(dt),
	   g_date_time_get_day_of_month(dt),
	   g_date_time_get_hour(dt),
	   g_date_time_get_minute(dt),
	   g_date_time_get_second(dt));
    }

  return dt;
}

static gchar *
stcs_date_format_text(STCSValueToDateText *self, GDateTime *dt, GError **error)
{
  GDateTime *in_tz;
  gchar *text = NULL;

  in_tz = g_date_time_to_timezone(dt, self->timezone);
  if (in_tz == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "failed to convert date to output timezone");
      return NULL;
    }

  if (self->format_is_iso8601)
    text = g_date_time_format_iso8601(in_tz);
  else
    text = g_date_time_format(in_tz, self->format);

  g_date_time_unref(in_tz);

  if (text == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "failed to format date with `%s'", self->format);
      LOGW("format failed with `%s'", self->format);
    }
  else
    {
      LOGD("formatted with `%s' timezone=`%s' -> `%s'",
	   self->format, self->timezone_id, text);
    }

  return text;
}

static STCSValueToText *
stcs_value_to_date_text_create(void)
{
  return (STCSValueToText *) g_new0(STCSValueToDateText, 1);
}

static void
stcs_value_to_date_text_destroy(STCSValueToText *transform)
{
  STCSValueToDateText *self = (STCSValueToDateText *) transform;

  g_free(self->format);
  g_free(self->timezone_id);
  g_free(self->local_timezone_id);
  g_clear_pointer(&self->timezone, g_time_zone_unref);
  g_clear_pointer(&self->local_timezone, g_time_zone_unref);
  g_free(self);
}

static gboolean
stcs_value_to_date_text_configure(STCSValueToText *transform,
				  xmlNodePtr node,
				  GError **error)
{
  STCSValueToDateText *self = (STCSValueToDateText *) transform;
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
		      "unexpected element <%s> inside date value-to-text",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (format == NULL || *format == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "date value-to-text requires a <format> element");
      goto out;
    }

  if (!value_set)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "date value-to-text requires a <value> element");
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
stcs_value_to_date_text_to_text(STCSValueToText *transform,
				gdouble value,
				gchar **out,
				GError **error)
{
  STCSValueToDateText *self = (STCSValueToDateText *) transform;
  GDateTime *dt;
  gchar *text;

  LOGD("to_text value=%g kind=`%s'",
       value, stcs_date_value_kind_name(self->value_kind));

  dt = stcs_date_from_value(self, value, error);
  if (dt == NULL)
    return FALSE;

  text = stcs_date_format_text(self, dt, error);
  g_date_time_unref(dt);
  if (text == NULL)
    return FALSE;

  LOGD("to_text result=`%s'", text);
  *out = text;
  return TRUE;
}

const STCSValueToTextClass stcs_value_to_date_text_class = {
  .type = "date",
  .create = stcs_value_to_date_text_create,
  .destroy = stcs_value_to_date_text_destroy,
  .configure = stcs_value_to_date_text_configure,
  .to_text = stcs_value_to_date_text_to_text,
};
