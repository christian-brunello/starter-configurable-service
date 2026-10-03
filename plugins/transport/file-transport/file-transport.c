/*
 * starter-configurable-service - file-transport.c
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
#include <errno.h>
#include <string.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "starter/configurable-service/transport.h"
#include "starter/configurable-service/cron.h"

#define LOGD(msg...) g_log("file-transport", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("file-transport", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("file-transport", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_FILE_TRANSPORT (stcs_file_transport_get_type())
G_DECLARE_FINAL_TYPE(STCSFileTransport, stcs_file_transport, STCS, FILE_TRANSPORT, STCSTransport)

typedef enum {
  STCS_FILE_DIRECTION_READ = 0,
  STCS_FILE_DIRECTION_WRITE,
  STCS_FILE_DIRECTION_APPEND
} STCSFileDirection;

typedef enum {
  STCS_FILE_ACCESS_STREAM = 0,
  STCS_FILE_ACCESS_WINDOW
} STCSFileAccessMode;

typedef enum {
  STCS_FILE_REOPEN_KEEP = 0,
  STCS_FILE_REOPEN_REOPEN
} STCSFileReopenPolicy;

struct _STCSFileTransport {
  STCSTransport parent_instance;

  gchar *path;
  STCSFileDirection direction;
  STCSFileAccessMode access_mode;
  STCSFileReopenPolicy reopen;
  guint64 offset;
  gboolean has_size;
  guint64 size;
  STCSCron *cron; /* optional read poll schedule */
  guint poll_source_id;

  gchar *fopen_mode;
  FILE *fp;
};

G_DEFINE_TYPE(STCSFileTransport, stcs_file_transport, STCS_TYPE_TRANSPORT)

static void stcs_file_transport_close_file(STCSFileTransport *self);
static gboolean stcs_file_transport_open_file(STCSFileTransport *self, GError **error);
static void stcs_file_transport_fetch_and_emit(STCSFileTransport *self);
static gboolean stcs_file_transport_poll_cb(gpointer user_data);

static gboolean
stcs_file_transport_parse_string(xmlNodePtr node, gchar **out, GError **error)
{
  xmlChar *content;

  content = xmlNodeGetContent(node);
  if (content == NULL || *content == '\0')
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
stcs_file_transport_parse_uint64(xmlNodePtr node, guint64 *out, GError **error)
{
  xmlChar *content;
  gchar *end = NULL;
  unsigned long long value;

  content = xmlNodeGetContent(node);
  if (content == NULL || *content == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has empty content",
		  (const char *) node->name);
      xmlFree(content);
      return FALSE;
    }

  value = g_ascii_strtoull((const char *) content, &end, 10);
  if (end == (const char *) content || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> has invalid unsigned value: %s",
		  (const char *) node->name,
		  (const char *) content);
      xmlFree(content);
      return FALSE;
    }

  *out = (guint64) value;
  xmlFree(content);
  return TRUE;
}

static gboolean
stcs_file_transport_parse_direction(const gchar *mode,
				    STCSFileDirection *direction,
				    GError **error)
{
  if (mode == NULL || *mode == '\0' || g_strcmp0(mode, "read") == 0 || g_strcmp0(mode, "r") == 0)
    {
      *direction = STCS_FILE_DIRECTION_READ;
      return TRUE;
    }

  if (g_strcmp0(mode, "write") == 0 || g_strcmp0(mode, "w") == 0)
    {
      *direction = STCS_FILE_DIRECTION_WRITE;
      return TRUE;
    }

  if (g_strcmp0(mode, "append") == 0 || g_strcmp0(mode, "a") == 0)
    {
      *direction = STCS_FILE_DIRECTION_APPEND;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported mode `%s' (use read, write or append; read+write is not allowed)",
	      mode);
  return FALSE;
}

static gboolean
stcs_file_transport_parse_access_mode(const gchar *value,
				      STCSFileAccessMode *out,
				      GError **error)
{
  if (value == NULL || *value == '\0' || g_strcmp0(value, "stream") == 0)
    {
      *out = STCS_FILE_ACCESS_STREAM;
      return TRUE;
    }

  if (g_strcmp0(value, "window") == 0)
    {
      *out = STCS_FILE_ACCESS_WINDOW;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported access mode `%s' (use stream or window)",
	      value);
  return FALSE;
}

static gboolean
stcs_file_transport_parse_reopen(const gchar *value,
				 STCSFileReopenPolicy *out,
				 GError **error)
{
  if (value == NULL || *value == '\0' || g_strcmp0(value, "keep") == 0)
    {
      *out = STCS_FILE_REOPEN_KEEP;
      return TRUE;
    }

  if (g_strcmp0(value, "reopen") == 0)
    {
      *out = STCS_FILE_REOPEN_REOPEN;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported reopen `%s' (use keep or reopen)",
	      value);
  return FALSE;
}

static const gchar *
stcs_file_transport_fopen_mode_for(STCSFileDirection direction, STCSFileAccessMode access_mode)
{
  switch (direction)
    {
    case STCS_FILE_DIRECTION_READ:
      return "rb";
    case STCS_FILE_DIRECTION_APPEND:
      return "ab";
    case STCS_FILE_DIRECTION_WRITE:
      /* window needs seek-in-place; stream truncates/creates */
      return access_mode == STCS_FILE_ACCESS_WINDOW ? "r+b" : "wb";
    default:
      return "rb";
    }
}

static void
stcs_file_transport_stop_poll(STCSFileTransport *self)
{
  if (self->poll_source_id != 0)
    {
      g_source_remove(self->poll_source_id);
      self->poll_source_id = 0;
    }
}

static void
stcs_file_transport_close_file(STCSFileTransport *self)
{
  if (self->fp != NULL)
    {
      fclose(self->fp);
      self->fp = NULL;
    }
}

static gboolean
stcs_file_transport_open_file(STCSFileTransport *self, GError **error)
{
  FILE *fp;

  g_return_val_if_fail(self->path != NULL, FALSE);
  g_return_val_if_fail(self->fopen_mode != NULL, FALSE);

  fp = fopen(self->path, self->fopen_mode);
  if (fp == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  g_io_error_from_errno(errno),
		  "unable to open `%s' with mode `%s': %s",
		  self->path, self->fopen_mode, g_strerror(errno));
      return FALSE;
    }

  stcs_file_transport_close_file(self);
  self->fp = fp;
  return TRUE;
}

static gboolean
stcs_file_transport_ensure_open(STCSFileTransport *self, GError **error)
{
  if (self->fp != NULL)
    return TRUE;

  return stcs_file_transport_open_file(self, error);
}

static gboolean
stcs_file_transport_prepare_io(STCSFileTransport *self, GError **error)
{
  if (self->reopen == STCS_FILE_REOPEN_REOPEN)
    stcs_file_transport_close_file(self);

  if (!stcs_file_transport_ensure_open(self, error))
    return FALSE;

  if (self->access_mode == STCS_FILE_ACCESS_WINDOW)
    {
      if (fseeko(self->fp, (off_t) self->offset, SEEK_SET) != 0)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      g_io_error_from_errno(errno),
		      "fseeko failed on %s (offset=%" G_GUINT64_FORMAT "): %s",
		      self->path, self->offset, g_strerror(errno));
	  return FALSE;
	}
    }

  return TRUE;
}

static void
stcs_file_transport_fetch_and_emit(STCSFileTransport *self)
{
  GError *error = NULL;
  GByteArray *array;
  guint8 buf[4096];
  size_t n;
  guint64 remaining;
  GBytes *bytes;

  if (self->direction != STCS_FILE_DIRECTION_READ)
    return;

  LOGD("fetch path=`%s' access=%s reopen=%s offset=%" G_GUINT64_FORMAT,
       self->path ? self->path : "(null)",
       self->access_mode == STCS_FILE_ACCESS_WINDOW ? "window" : "stream",
       self->reopen == STCS_FILE_REOPEN_REOPEN ? "reopen" : "keep",
       self->offset);

  if (!stcs_file_transport_prepare_io(self, &error))
    {
      LOGW("prepare_io failed: %s", error ? error->message : "unknown");
      stcs_transport_emit_data_error(STCS_TRANSPORT(self), error);
      g_clear_error(&error);
      return;
    }

  array = g_byte_array_new();
  remaining = self->has_size ? self->size : G_MAXUINT64;

  while (remaining > 0)
    {
      size_t chunk = sizeof buf;

      if ((guint64) chunk > remaining)
	chunk = (size_t) remaining;

      n = fread(buf, 1, chunk, self->fp);
      if (n == 0)
	break;

      g_byte_array_append(array, buf, n);
      remaining -= n;
    }

  if (ferror(self->fp))
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  g_io_error_from_errno(errno),
		  "fread failed on %s: %s",
		  self->path, g_strerror(errno));
      clearerr(self->fp);
      LOGW("read error: %s", error->message);
      g_byte_array_unref(array);
      stcs_transport_emit_data_error(STCS_TRANSPORT(self), error);
      g_clear_error(&error);
      return;
    }

  LOGD("read %u bytes from `%s'", array->len, self->path ? self->path : "(null)");

  if (self->access_mode == STCS_FILE_ACCESS_STREAM && array->len == 0)
    {
      LOGD("stream read empty, skip data-ready");
      g_byte_array_unref(array);
      return;
    }

  bytes = g_byte_array_free_to_bytes(array);
  LOGD("emit data-ready (%" G_GSIZE_FORMAT " bytes) handlers=%u",
       g_bytes_get_size(bytes),
       g_signal_has_handler_pending(self, g_signal_lookup("data-ready", STCS_TYPE_TRANSPORT),
				    0, FALSE) ? 1 : 0);
  stcs_transport_emit_data_ready(STCS_TRANSPORT(self), bytes);
  g_bytes_unref(bytes);
}

static void
stcs_file_transport_schedule_next(STCSFileTransport *self)
{
  if (self->cron == NULL || !stcs_cron_is_parsed(self->cron))
    return;

  if (self->poll_source_id != 0)
    {
      g_source_remove(self->poll_source_id);
      self->poll_source_id = 0;
    }

  self->poll_source_id = stcs_cron_timeout_add(self->cron,
					       stcs_file_transport_poll_cb,
					       self);
  if (self->poll_source_id == 0)
    LOGW("failed to arm next poll from cron `%s'",
	 stcs_cron_get_expression(self->cron));
  else
    LOGD("armed poll timeout id=%u cron=`%s'",
	 self->poll_source_id,
	 stcs_cron_get_expression(self->cron));
}

static gboolean
stcs_file_transport_poll_cb(gpointer user_data)
{
  STCSFileTransport *self = STCS_FILE_TRANSPORT(user_data);

  self->poll_source_id = 0;

  LOGD("poll timeout fired path=`%s' cron=`%s'",
       self->path ? self->path : "(null)",
       (self->cron != NULL && stcs_cron_is_parsed(self->cron))
         ? stcs_cron_get_expression(self->cron)
         : "(none)");
  stcs_file_transport_fetch_and_emit(self);
  stcs_file_transport_schedule_next(self);
  return G_SOURCE_REMOVE;
}

static gboolean
stcs_file_transport_configure(STCSTransport *transport, xmlNodePtr node, GError **error)
{
  STCSFileTransport *self = STCS_FILE_TRANSPORT(transport);
  xmlNodePtr child;
  gchar *path = NULL;
  gchar *mode_str = NULL;
  gchar *reopen_str = NULL;
  gchar *read_mode_str = NULL;
  gchar *write_mode_str = NULL;
  STCSFileDirection direction = STCS_FILE_DIRECTION_READ;
  STCSFileAccessMode access_mode = STCS_FILE_ACCESS_WINDOW;
  STCSFileReopenPolicy reopen = STCS_FILE_REOPEN_KEEP;
  guint64 offset = 0;
  gboolean has_size = FALSE;
  guint64 size = 0;
  gchar *poll_cron = NULL;
  gboolean saw_read_mode = FALSE;
  gboolean saw_write_mode = FALSE;
  gboolean r = FALSE;

  g_return_val_if_fail(node != NULL, FALSE);

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "path"))
	{
	  if (!stcs_file_transport_parse_string(child, &path, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "mode"))
	{
	  if (!stcs_file_transport_parse_string(child, &mode_str, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "reopen"))
	{
	  if (!stcs_file_transport_parse_string(child, &reopen_str, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "read-mode"))
	{
	  if (!stcs_file_transport_parse_string(child, &read_mode_str, error))
	    goto out;
	  saw_read_mode = TRUE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "write-mode"))
	{
	  if (!stcs_file_transport_parse_string(child, &write_mode_str, error))
	    goto out;
	  saw_write_mode = TRUE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "offset"))
	{
	  if (!stcs_file_transport_parse_uint64(child, &offset, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "size"))
	{
	  if (!stcs_file_transport_parse_uint64(child, &size, error))
	    goto out;
	  has_size = TRUE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "poll-cron"))
	{
	  if (!stcs_file_transport_parse_string(child, &poll_cron, error))
	    goto out;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside file-transport config",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (path == NULL || *path == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "file-transport requires a <path> element");
      goto out;
    }

  if (!stcs_file_transport_parse_direction(mode_str, &direction, error))
    goto out;

  if (!stcs_file_transport_parse_reopen(reopen_str, &reopen, error))
    goto out;

  if (direction == STCS_FILE_DIRECTION_READ)
    {
      if (saw_write_mode)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "file-transport mode=read cannot use <write-mode>");
	  goto out;
	}

      access_mode = STCS_FILE_ACCESS_WINDOW;
      if (saw_read_mode
	  && !stcs_file_transport_parse_access_mode(read_mode_str, &access_mode, error))
	goto out;
    }
  else
    {
      if (saw_read_mode)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "file-transport mode=%s cannot use <read-mode>",
		      mode_str ? mode_str : "write");
	  goto out;
	}

      if (poll_cron != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "file-transport mode=%s cannot use <poll-cron>",
		      direction == STCS_FILE_DIRECTION_APPEND ? "append" : "write");
	  goto out;
	}

      access_mode = STCS_FILE_ACCESS_STREAM;
      if (saw_write_mode
	  && !stcs_file_transport_parse_access_mode(write_mode_str, &access_mode, error))
	goto out;

      if (direction == STCS_FILE_DIRECTION_APPEND
	  && access_mode == STCS_FILE_ACCESS_WINDOW)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "file-transport mode=append cannot use write-mode=window");
	  goto out;
	}
    }

  if (access_mode == STCS_FILE_ACCESS_STREAM && offset != 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "file-transport access-mode=stream cannot use a non-zero <offset>");
      goto out;
    }

  if (poll_cron != NULL && *poll_cron != '\0')
    {
      if (self->cron == NULL)
	self->cron = stcs_cron_new();
      if (!stcs_cron_parse(self->cron, poll_cron, error))
	goto out;
    }
  else
    {
      g_clear_object(&self->cron);
    }

  stcs_file_transport_stop_poll(self);
  stcs_file_transport_close_file(self);

  g_free(self->path);
  g_free(self->fopen_mode);

  self->path = path;
  path = NULL;
  self->direction = direction;
  self->access_mode = access_mode;
  self->reopen = reopen;
  self->offset = offset;
  self->has_size = has_size;
  self->size = size;
  self->fopen_mode = g_strdup(stcs_file_transport_fopen_mode_for(direction, access_mode));

  if (reopen == STCS_FILE_REOPEN_KEEP)
    {
      if (!stcs_file_transport_open_file(self, error))
	goto out;
    }

  r = TRUE;

  if (direction == STCS_FILE_DIRECTION_READ)
    {
      if (self->cron != NULL && stcs_cron_is_parsed(self->cron))
	LOGI("reading `%s' (poll-cron `%s')",
	     self->path, stcs_cron_get_expression(self->cron));
      else
	LOGI("reading `%s'", self->path);

      LOGD("configure read path=`%s' access=%s reopen=%s offset=%" G_GUINT64_FORMAT
	   " poll-cron=%s — initial fetch",
	   self->path,
	   self->access_mode == STCS_FILE_ACCESS_WINDOW ? "window" : "stream",
	   self->reopen == STCS_FILE_REOPEN_REOPEN ? "reopen" : "keep",
	   self->offset,
	   (self->cron != NULL && stcs_cron_is_parsed(self->cron))
	     ? stcs_cron_get_expression(self->cron)
	     : "(none)");
      stcs_file_transport_fetch_and_emit(self);
      stcs_file_transport_schedule_next(self);
    }
  else
    {
      LOGI("writing `%s' (mode=%s)",
	   self->path,
	   direction == STCS_FILE_DIRECTION_APPEND ? "append" : "write");
      LOGD("configure write path=`%s' access=%s reopen=%s",
	   self->path,
	   self->access_mode == STCS_FILE_ACCESS_WINDOW ? "window" : "stream",
	   self->reopen == STCS_FILE_REOPEN_REOPEN ? "reopen" : "keep");
    }

out:
  g_free(path);
  g_free(mode_str);
  g_free(reopen_str);
  g_free(read_mode_str);
  g_free(write_mode_str);
  g_free(poll_cron);
  return r;
}

static void
stcs_file_transport_start_write(STCSTransport *transport,
				guint64 op_id,
				GBytes *data)
{
  STCSFileTransport *self = STCS_FILE_TRANSPORT(transport);
  GError *error = NULL;
  gconstpointer buffer;
  gsize count = 0;
  gsize to_write;
  size_t n;

  buffer = g_bytes_get_data(data, &count);
  to_write = count;

  if (self->direction == STCS_FILE_DIRECTION_READ)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_SUPPORTED,
		  "file-transport path `%s' is configured for read only",
		  self->path ? self->path : "(unset)");
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  if (count == 0)
    {
      stcs_transport_finish_write(transport, op_id, 0);
      return;
    }

  if (self->access_mode == STCS_FILE_ACCESS_WINDOW && self->has_size && count > self->size)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_ARGUMENT,
		  "write of %zu bytes exceeds configured size %" G_GUINT64_FORMAT " on %s",
		  count, self->size, self->path);
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  if (!stcs_file_transport_prepare_io(self, &error))
    {
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  n = fwrite(buffer, 1, to_write, self->fp);
  if (n < to_write)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  g_io_error_from_errno(errno),
		  "fwrite failed on %s: %s",
		  self->path, g_strerror(errno));
      clearerr(self->fp);
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  if (fflush(self->fp) != 0)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  g_io_error_from_errno(errno),
		  "fflush failed on %s: %s",
		  self->path, g_strerror(errno));
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  stcs_transport_finish_write(transport, op_id, n);
}

static void
stcs_file_transport_dispose(GObject *object)
{
  STCSFileTransport *self = STCS_FILE_TRANSPORT(object);

  stcs_file_transport_stop_poll(self);
  stcs_file_transport_close_file(self);
  g_clear_object(&self->cron);

  G_OBJECT_CLASS(stcs_file_transport_parent_class)->dispose(object);
}

static void
stcs_file_transport_finalize(GObject *object)
{
  STCSFileTransport *self = STCS_FILE_TRANSPORT(object);

  g_free(self->path);
  g_free(self->fopen_mode);

  G_OBJECT_CLASS(stcs_file_transport_parent_class)->finalize(object);
}

static void
stcs_file_transport_class_init(STCSFileTransportClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSTransportClass *transport_class = STCS_TRANSPORT_CLASS(klass);

  object_class->dispose = stcs_file_transport_dispose;
  object_class->finalize = stcs_file_transport_finalize;

  transport_class->configure = stcs_file_transport_configure;
  transport_class->start_write = stcs_file_transport_start_write;
}

static void
stcs_file_transport_init(STCSFileTransport *self)
{
  self->path = NULL;
  self->direction = STCS_FILE_DIRECTION_READ;
  self->access_mode = STCS_FILE_ACCESS_WINDOW;
  self->reopen = STCS_FILE_REOPEN_KEEP;
  self->offset = 0;
  self->has_size = FALSE;
  self->size = 0;
  self->cron = NULL;
  self->poll_source_id = 0;
  self->fopen_mode = NULL;
  self->fp = NULL;
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("file-transport", STCS_PLUGIN_CATEGORY_TRANSPORT, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_FILE_TRANSPORT, NULL);
}
