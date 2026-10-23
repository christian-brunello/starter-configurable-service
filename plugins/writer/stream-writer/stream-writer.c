/*
 * starter-configurable-service - stream-writer.c
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

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "starter/configurable-service/writer.h"
#include "value-to-text.h"

#define LOGD(msg...) g_log("stream-writer", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("stream-writer", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_STREAM_WRITER (stcs_stream_writer_get_type())
G_DECLARE_FINAL_TYPE(STCSStreamWriter, stcs_stream_writer, STCS, STREAM_WRITER, STCSWriter)

typedef enum {
  STCS_STREAM_WRITER_MODE_TEXT = 0,
  STCS_STREAM_WRITER_MODE_BINARY
} STCSStreamWriterMode;

typedef enum {
  STCS_STREAM_ENC_ASCII = 0,
  STCS_STREAM_ENC_UINT8,
  STCS_STREAM_ENC_INT8,
  STCS_STREAM_ENC_UINT16BE,
  STCS_STREAM_ENC_UINT16LE,
  STCS_STREAM_ENC_INT16BE,
  STCS_STREAM_ENC_INT16LE,
  STCS_STREAM_ENC_UINT32BE,
  STCS_STREAM_ENC_UINT32LE,
  STCS_STREAM_ENC_INT32BE,
  STCS_STREAM_ENC_INT32LE
} STCSStreamByteEncoding;

typedef struct {
  STCSValueToText *converter;
  /* BINARY maps only: */
  gsize offset;
  gsize length;
  STCSStreamByteEncoding encoding;
} STCSStreamWriterMap;

struct _STCSStreamWriter {
  STCSWriter parent_instance;

  STCSStreamWriterMode mode;
  gchar *template_text; /* TEXT: source with {{id}} */
  GByteArray *template_bytes; /* BINARY: mutable working copy */
  GHashTable *text_values; /* id → gchar* (TEXT substitutions) */
  GHashTable *maps; /* id → STCSStreamWriterMap* */
};

G_DEFINE_TYPE(STCSStreamWriter, stcs_stream_writer, STCS_TYPE_WRITER)

static void
stcs_stream_writer_map_free(STCSStreamWriterMap *map)
{
  if (map == NULL)
    return;
  if (map->converter != NULL)
    stcs_value_to_text_free(map->converter);
  g_free(map);
}

static gboolean
stcs_stream_parse_encoding(const gchar *s, STCSStreamByteEncoding *out, GError **error)
{
  if (s == NULL || *s == '\0' || g_strcmp0(s, "ascii") == 0)
    {
      *out = STCS_STREAM_ENC_ASCII;
      return TRUE;
    }
  if (g_strcmp0(s, "uint8") == 0) { *out = STCS_STREAM_ENC_UINT8; return TRUE; }
  if (g_strcmp0(s, "int8") == 0) { *out = STCS_STREAM_ENC_INT8; return TRUE; }
  if (g_strcmp0(s, "uint16be") == 0) { *out = STCS_STREAM_ENC_UINT16BE; return TRUE; }
  if (g_strcmp0(s, "uint16le") == 0) { *out = STCS_STREAM_ENC_UINT16LE; return TRUE; }
  if (g_strcmp0(s, "int16be") == 0) { *out = STCS_STREAM_ENC_INT16BE; return TRUE; }
  if (g_strcmp0(s, "int16le") == 0) { *out = STCS_STREAM_ENC_INT16LE; return TRUE; }
  if (g_strcmp0(s, "uint32be") == 0) { *out = STCS_STREAM_ENC_UINT32BE; return TRUE; }
  if (g_strcmp0(s, "uint32le") == 0) { *out = STCS_STREAM_ENC_UINT32LE; return TRUE; }
  if (g_strcmp0(s, "int32be") == 0) { *out = STCS_STREAM_ENC_INT32BE; return TRUE; }
  if (g_strcmp0(s, "int32le") == 0) { *out = STCS_STREAM_ENC_INT32LE; return TRUE; }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported encoding `%s'", s);
  return FALSE;
}

static gsize
stcs_stream_encoding_size(STCSStreamByteEncoding enc)
{
  switch (enc)
    {
    case STCS_STREAM_ENC_ASCII:
      return 0;
    case STCS_STREAM_ENC_UINT8:
    case STCS_STREAM_ENC_INT8:
      return 1;
    case STCS_STREAM_ENC_UINT16BE:
    case STCS_STREAM_ENC_UINT16LE:
    case STCS_STREAM_ENC_INT16BE:
    case STCS_STREAM_ENC_INT16LE:
      return 2;
    case STCS_STREAM_ENC_UINT32BE:
    case STCS_STREAM_ENC_UINT32LE:
    case STCS_STREAM_ENC_INT32BE:
    case STCS_STREAM_ENC_INT32LE:
      return 4;
    default:
      return 0;
    }
}

static gboolean
stcs_stream_parse_uint64_attr(const gchar *s, guint64 *out, const gchar *name, GError **error)
{
  gchar *end = NULL;
  guint64 v;

  if (s == NULL || *s == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer <map> attribute %s is empty", name);
      return FALSE;
    }

  v = g_ascii_strtoull(s, &end, 10);
  if (end == s || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer <map> attribute %s is not a valid integer", name);
      return FALSE;
    }

  *out = v;
  return TRUE;
}

static gint
stcs_stream_hex_nibble(gchar c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static gboolean
stcs_stream_decode_hex(const gchar *hex, GByteArray **out, GError **error)
{
  GByteArray *buf;
  const gchar *p = hex;
  gint hi = -1;

  buf = g_byte_array_new();
  while (*p != '\0')
    {
      gint n;

      if (g_ascii_isspace(*p))
	{
	  p++;
	  continue;
	}

      n = stcs_stream_hex_nibble(*p);
      if (n < 0)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "invalid hex digit `%c' in stream-writer <template>",
		      *p);
	  g_byte_array_unref(buf);
	  return FALSE;
	}

      if (hi < 0)
	hi = n;
      else
	{
	  guint8 b = (guint8) ((hi << 4) | n);
	  g_byte_array_append(buf, &b, 1);
	  hi = -1;
	}
      p++;
    }

  if (hi >= 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "odd number of hex digits in stream-writer <template>");
      g_byte_array_unref(buf);
      return FALSE;
    }

  *out = buf;
  return TRUE;
}

static gboolean
stcs_stream_value_to_text_default(gdouble value, gchar **out)
{
  gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_dtostr(buf, sizeof(buf), value);
  *out = g_strdup(buf);
  return TRUE;
}

static gboolean
stcs_stream_value_to_bytes(gdouble value,
			   STCSStreamByteEncoding encoding,
			   gsize length,
			   STCSValueToText *converter,
			   guint8 *out_buf,
			   GError **error)
{
  if (encoding == STCS_STREAM_ENC_ASCII)
    {
      gchar *text = NULL;
      gsize n;

      if (converter != NULL)
	{
	  if (!stcs_value_to_text_apply(converter, value, &text, error))
	    return FALSE;
	}
      else
	{
	  stcs_stream_value_to_text_default(value, &text);
	}

      n = strlen(text);
      if (n > length)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "ascii value `%s' (%" G_GSIZE_FORMAT " bytes) exceeds map length %" G_GSIZE_FORMAT,
		      text, n, length);
	  g_free(text);
	  return FALSE;
	}

      memset(out_buf, 0, length);
      memcpy(out_buf, text, n);
      g_free(text);
      return TRUE;
    }

  if (converter != NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "value-to-text requires encoding=ascii (or a text template map)");
      return FALSE;
    }

  if (length != stcs_stream_encoding_size(encoding))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "byte length %" G_GSIZE_FORMAT " does not match encoding size %" G_GSIZE_FORMAT,
		  length, stcs_stream_encoding_size(encoding));
      return FALSE;
    }

  switch (encoding)
    {
    case STCS_STREAM_ENC_UINT8:
      out_buf[0] = (guint8) CLAMP(value, 0.0, 255.0);
      return TRUE;
    case STCS_STREAM_ENC_INT8:
      out_buf[0] = (guint8) (gint8) CLAMP(value, -128.0, 127.0);
      return TRUE;
    case STCS_STREAM_ENC_UINT16BE:
      {
	guint16 v = (guint16) CLAMP(value, 0.0, 65535.0);
	out_buf[0] = (guint8) (v >> 8);
	out_buf[1] = (guint8) (v & 0xff);
	return TRUE;
      }
    case STCS_STREAM_ENC_UINT16LE:
      {
	guint16 v = (guint16) CLAMP(value, 0.0, 65535.0);
	out_buf[0] = (guint8) (v & 0xff);
	out_buf[1] = (guint8) (v >> 8);
	return TRUE;
      }
    case STCS_STREAM_ENC_INT16BE:
      {
	gint16 v = (gint16) CLAMP(value, -32768.0, 32767.0);
	guint16 u = (guint16) v;
	out_buf[0] = (guint8) (u >> 8);
	out_buf[1] = (guint8) (u & 0xff);
	return TRUE;
      }
    case STCS_STREAM_ENC_INT16LE:
      {
	gint16 v = (gint16) CLAMP(value, -32768.0, 32767.0);
	guint16 u = (guint16) v;
	out_buf[0] = (guint8) (u & 0xff);
	out_buf[1] = (guint8) (u >> 8);
	return TRUE;
      }
    case STCS_STREAM_ENC_UINT32BE:
      {
	guint32 v = (guint32) CLAMP(value, 0.0, 4294967295.0);
	out_buf[0] = (guint8) (v >> 24);
	out_buf[1] = (guint8) (v >> 16);
	out_buf[2] = (guint8) (v >> 8);
	out_buf[3] = (guint8) (v & 0xff);
	return TRUE;
      }
    case STCS_STREAM_ENC_UINT32LE:
      {
	guint32 v = (guint32) CLAMP(value, 0.0, 4294967295.0);
	out_buf[0] = (guint8) (v & 0xff);
	out_buf[1] = (guint8) (v >> 8);
	out_buf[2] = (guint8) (v >> 16);
	out_buf[3] = (guint8) (v >> 24);
	return TRUE;
      }
    case STCS_STREAM_ENC_INT32BE:
      {
	gint32 v = (gint32) CLAMP(value, -2147483648.0, 2147483647.0);
	guint32 u = (guint32) v;
	out_buf[0] = (guint8) (u >> 24);
	out_buf[1] = (guint8) (u >> 16);
	out_buf[2] = (guint8) (u >> 8);
	out_buf[3] = (guint8) (u & 0xff);
	return TRUE;
      }
    case STCS_STREAM_ENC_INT32LE:
      {
	gint32 v = (gint32) CLAMP(value, -2147483648.0, 2147483647.0);
	guint32 u = (guint32) v;
	out_buf[0] = (guint8) (u & 0xff);
	out_buf[1] = (guint8) (u >> 8);
	out_buf[2] = (guint8) (u >> 16);
	out_buf[3] = (guint8) (u >> 24);
	return TRUE;
      }
    default:
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unsupported encoding");
      return FALSE;
    }
}

static GString *
stcs_stream_render_text(STCSStreamWriter *self)
{
  GString *out;
  const gchar *p;

  out = g_string_new(NULL);
  p = self->template_text;

  while (*p != '\0')
    {
      if (p[0] == '{' && p[1] == '{')
	{
	  const gchar *end = strstr(p + 2, "}}");

	  if (end != NULL)
	    {
	      gchar *id = g_strndup(p + 2, (gsize) (end - (p + 2)));
	      const gchar *val = g_hash_table_lookup(self->text_values, id);

	      if (val != NULL)
		g_string_append(out, val);
	      else
		g_string_append_len(out, p, (gssize) (end + 2 - p));

	      g_free(id);
	      p = end + 2;
	      continue;
	    }
	}

      g_string_append_c(out, *p);
      p++;
    }

  return out;
}

static gboolean
stcs_stream_writer_flush(STCSStreamWriter *self, GError **error)
{
  GBytes *bytes = NULL;
  gboolean r;

  if (self->mode == STCS_STREAM_WRITER_MODE_TEXT)
    {
      GString *rendered;
      gsize len;

      if (self->template_text == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_NOT_INITIALIZED,
		      "stream-writer has no text template");
	  return FALSE;
	}

      rendered = stcs_stream_render_text(self);
      len = rendered->len;
      bytes = g_bytes_new_take(g_string_free(rendered, FALSE), len);
    }
  else
    {
      if (self->template_bytes == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_NOT_INITIALIZED,
		      "stream-writer has no binary template");
	  return FALSE;
	}

      bytes = g_bytes_new(self->template_bytes->data, self->template_bytes->len);
    }

  r = stcs_writer_write_bytes(STCS_WRITER(self), bytes, error);
  g_bytes_unref(bytes);
  return r;
}

static gboolean
stcs_stream_writer_configure_map(STCSStreamWriter *self,
				 xmlNodePtr node,
				 STCSDatapointLookupFunc lookup_datapoint,
				 gpointer lookup_data,
				 GError **error)
{
  xmlChar *id_attr = NULL;
  xmlChar *datapoint_attr = NULL;
  xmlChar *offset_attr = NULL;
  xmlChar *length_attr = NULL;
  xmlChar *encoding_attr = NULL;
  const gchar *id;
  const gchar *datapoint_id;
  STCSDatapoint *datapoint;
  STCSStreamWriterMap *map = NULL;
  STCSValueToText *converter = NULL;
  xmlNodePtr child;
  gboolean has_offset;
  gboolean has_length;
  gboolean r = FALSE;

  id_attr = xmlGetProp(node, (const xmlChar *) "id");
  datapoint_attr = xmlGetProp(node, (const xmlChar *) "datapoint");
  offset_attr = xmlGetProp(node, (const xmlChar *) "offset");
  length_attr = xmlGetProp(node, (const xmlChar *) "length");
  encoding_attr = xmlGetProp(node, (const xmlChar *) "encoding");

  if (id_attr == NULL || *id_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer <map> requires a non-empty id attribute");
      goto out;
    }

  if (datapoint_attr == NULL || *datapoint_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer <map> requires a non-empty datapoint attribute");
      goto out;
    }

  id = (const gchar *) id_attr;
  datapoint_id = (const gchar *) datapoint_attr;

  if (g_hash_table_contains(self->maps, id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer duplicate map id=`%s'", id);
      goto out;
    }

  datapoint = lookup_datapoint(datapoint_id, lookup_data);
  if (datapoint == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "datapoint `%s' not found (stream-writer id=%s)",
		  datapoint_id, id);
      goto out;
    }

  has_offset = (offset_attr != NULL);
  has_length = (length_attr != NULL);

  if (self->mode == STCS_STREAM_WRITER_MODE_TEXT)
    {
      if (has_offset || has_length || encoding_attr != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-writer text template maps cannot use offset/length/encoding (id=%s)",
		      id);
	  goto out;
	}
    }
  else
    {
      if (!has_offset || !has_length)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-writer binary maps require offset= and length= (id=%s)",
		      id);
	  goto out;
	}
    }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "value-to-text"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside stream-writer <map>, expected <value-to-text>",
		      (const char *) child->name);
	  goto out;
	}

      if (converter != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-writer <map id=\"%s\"> allows at most one <value-to-text>",
		      id);
	  goto out;
	}

      converter = stcs_value_to_text_new_from_node(child, error);
      if (converter == NULL)
	goto out;
    }

  map = g_new0(STCSStreamWriterMap, 1);
  map->converter = converter;
  converter = NULL;

  if (self->mode == STCS_STREAM_WRITER_MODE_BINARY)
    {
      guint64 offset = 0;
      guint64 length = 0;

      if (!stcs_stream_parse_uint64_attr((const gchar *) offset_attr, &offset, "offset", error))
	goto out;
      if (!stcs_stream_parse_uint64_attr((const gchar *) length_attr, &length, "length", error))
	goto out;
      if (length == 0)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-writer <map id=\"%s\"> length must be > 0", id);
	  goto out;
	}

      map->offset = (gsize) offset;
      map->length = (gsize) length;
      if (!stcs_stream_parse_encoding((const gchar *) encoding_attr, &map->encoding, error))
	goto out;

      if (map->offset > self->template_bytes->len ||
	  map->length > self->template_bytes->len - map->offset)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-writer <map id=\"%s\"> byte range outside template (%u bytes)",
		      id, self->template_bytes->len);
	  goto out;
	}

      if (map->encoding != STCS_STREAM_ENC_ASCII)
	{
	  gsize need = stcs_stream_encoding_size(map->encoding);
	  if (map->length != need)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "stream-writer <map id=\"%s\"> length=%" G_GSIZE_FORMAT
			  " does not match encoding size %" G_GSIZE_FORMAT,
			  id, map->length, need);
	      goto out;
	    }
	  if (map->converter != NULL)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "stream-writer <map id=\"%s\"> value-to-text requires encoding=ascii",
			  id);
	      goto out;
	    }
	}
    }

  stcs_writer_add_mapping(STCS_WRITER(self), id, datapoint);
  g_hash_table_insert(self->maps, g_strdup(id), map);

  if (self->mode == STCS_STREAM_WRITER_MODE_TEXT)
    LOGD("map id=`%s' → datapoint=`%s' placeholder={{%s}}%s",
	 id, datapoint_id, id,
	 map->converter != NULL ? " (value-to-text)" : "");
  else
    LOGD("map id=`%s' → datapoint=`%s' offset=%" G_GSIZE_FORMAT
	 " length=%" G_GSIZE_FORMAT " encoding=%s%s",
	 id, datapoint_id, map->offset, map->length,
	 encoding_attr ? (const char *) encoding_attr : "ascii",
	 map->converter != NULL ? " (value-to-text)" : "");

  map = NULL;
  r = TRUE;

out:
  if (map != NULL)
    stcs_stream_writer_map_free(map);
  if (converter != NULL)
    stcs_value_to_text_free(converter);
  if (id_attr != NULL)
    xmlFree(id_attr);
  if (datapoint_attr != NULL)
    xmlFree(datapoint_attr);
  if (offset_attr != NULL)
    xmlFree(offset_attr);
  if (length_attr != NULL)
    xmlFree(length_attr);
  if (encoding_attr != NULL)
    xmlFree(encoding_attr);
  return r;
}

static gboolean
stcs_stream_writer_parse_template(STCSStreamWriter *self, xmlNodePtr node, GError **error)
{
  xmlChar *encoding_attr = NULL;
  xmlChar *content = NULL;
  const gchar *encoding = "text";
  gboolean r = FALSE;

  encoding_attr = xmlGetProp(node, (const xmlChar *) "encoding");
  if (encoding_attr != NULL && *encoding_attr != '\0')
    encoding = (const gchar *) encoding_attr;

  content = xmlNodeGetContent(node);
  if (content == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer <template> has empty content");
      goto out;
    }

  if (g_strcmp0(encoding, "text") == 0)
    {
      self->mode = STCS_STREAM_WRITER_MODE_TEXT;
      g_free(self->template_text);
      self->template_text = g_strdup((const char *) content);
      g_clear_pointer(&self->template_bytes, g_byte_array_unref);
      LOGD("loaded text template (%" G_GSIZE_FORMAT " bytes)",
	   (gsize) strlen(self->template_text));
    }
  else if (g_strcmp0(encoding, "hex") == 0)
    {
      GByteArray *bytes = NULL;

      self->mode = STCS_STREAM_WRITER_MODE_BINARY;
      if (!stcs_stream_decode_hex((const gchar *) content, &bytes, error))
	goto out;
      g_clear_pointer(&self->template_bytes, g_byte_array_unref);
      self->template_bytes = bytes;
      g_clear_pointer(&self->template_text, g_free);
      LOGD("loaded hex template (%u bytes)", self->template_bytes->len);
    }
  else
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer <template> encoding `%s' unsupported (use text or hex)",
		  encoding);
      goto out;
    }

  r = TRUE;

out:
  if (encoding_attr != NULL)
    xmlFree(encoding_attr);
  if (content != NULL)
    xmlFree(content);
  return r;
}

static gboolean
stcs_stream_writer_configure(STCSWriter *writer,
			     xmlNodePtr node,
			     STCSDatapointLookupFunc lookup_datapoint,
			     gpointer lookup_data,
			     GError **error)
{
  STCSStreamWriter *self = STCS_STREAM_WRITER(writer);
  xmlNodePtr child;
  gboolean saw_template = FALSE;

  g_return_val_if_fail(lookup_datapoint != NULL, FALSE);

  g_hash_table_remove_all(self->maps);
  g_hash_table_remove_all(self->text_values);
  g_clear_pointer(&self->template_text, g_free);
  g_clear_pointer(&self->template_bytes, g_byte_array_unref);
  self->mode = STCS_STREAM_WRITER_MODE_TEXT;

  /* First pass: template must be known before maps (binary range checks). */
  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "template"))
	{
	  if (saw_template)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "stream-writer allows only one <template>");
	      return FALSE;
	    }
	  if (!stcs_stream_writer_parse_template(self, child, error))
	    return FALSE;
	  saw_template = TRUE;
	}
    }

  if (!saw_template)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-writer requires a <template> element");
      return FALSE;
    }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "template"))
	continue;

      /* Parsed by config.c on the writer base class. */
      if (xmlStrEqual(child->name, (const xmlChar *) "write-attempts") ||
	  xmlStrEqual(child->name, (const xmlChar *) "write-retry-backoff"))
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "map"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside stream-writer, expected <map>",
		      (const char *) child->name);
	  return FALSE;
	}

      if (!stcs_stream_writer_configure_map(self, child, lookup_datapoint, lookup_data, error))
	return FALSE;
    }

  return TRUE;
}

static gboolean
stcs_stream_writer_write_value(STCSWriter *writer,
			       const gchar *key,
			       gdouble value,
			       GError **error)
{
  STCSStreamWriter *self = STCS_STREAM_WRITER(writer);
  STCSStreamWriterMap *map;
  gboolean r = FALSE;

  map = g_hash_table_lookup(self->maps, key);
  if (map == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "stream-writer unknown map id=`%s'", key);
      return FALSE;
    }

  if (self->mode == STCS_STREAM_WRITER_MODE_TEXT)
    {
      gchar *text = NULL;

      if (map->converter != NULL)
	{
	  if (!stcs_value_to_text_apply(map->converter, value, &text, error))
	    return FALSE;
	}
      else
	{
	  stcs_stream_value_to_text_default(value, &text);
	}

      g_hash_table_insert(self->text_values, g_strdup(key), text);
      LOGD("updated placeholder id=`%s' text=`%s' — flushing", key, text);
    }
  else
    {
      guint8 *patch;

      patch = g_malloc0(map->length);
      if (!stcs_stream_value_to_bytes(value,
				      map->encoding,
				      map->length,
				      map->converter,
				      patch,
				      error))
	{
	  g_free(patch);
	  return FALSE;
	}

      memcpy(self->template_bytes->data + map->offset, patch, map->length);
      g_free(patch);
      LOGD("patched id=`%s' offset=%" G_GSIZE_FORMAT " length=%" G_GSIZE_FORMAT " — flushing",
	   key, map->offset, map->length);
    }

  r = stcs_stream_writer_flush(self, error);
  return r;
}

static void
stcs_stream_writer_dispose(GObject *object)
{
  STCSStreamWriter *self = STCS_STREAM_WRITER(object);

  g_clear_pointer(&self->maps, g_hash_table_unref);
  g_clear_pointer(&self->text_values, g_hash_table_unref);
  g_clear_pointer(&self->template_text, g_free);
  g_clear_pointer(&self->template_bytes, g_byte_array_unref);

  G_OBJECT_CLASS(stcs_stream_writer_parent_class)->dispose(object);
}

static void
stcs_stream_writer_class_init(STCSStreamWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSWriterClass *writer_class = STCS_WRITER_CLASS(klass);

  object_class->dispose = stcs_stream_writer_dispose;

  writer_class->configure = stcs_stream_writer_configure;
  writer_class->write_value = stcs_stream_writer_write_value;
}

static void
stcs_stream_writer_init(STCSStreamWriter *self)
{
  self->mode = STCS_STREAM_WRITER_MODE_TEXT;
  self->template_text = NULL;
  self->template_bytes = NULL;
  self->text_values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->maps = g_hash_table_new_full(g_str_hash,
				     g_str_equal,
				     g_free,
				     (GDestroyNotify) stcs_stream_writer_map_free);
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("stream-writer", STCS_PLUGIN_CATEGORY_WRITER, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_STREAM_WRITER, NULL);
}
