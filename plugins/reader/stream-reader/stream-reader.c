/*
 * starter-configurable-service - stream-reader.c
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

#include <errno.h>
#include <string.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "starter/configurable-service/reader.h"
#include "text-to-value.h"

#define LOGD(msg...) g_log("stream-reader", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log("stream-reader", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_STREAM_READER (stcs_stream_reader_get_type())
G_DECLARE_FINAL_TYPE(STCSStreamReader, stcs_stream_reader, STCS, STREAM_READER, STCSReader)

typedef enum {
  STCS_STREAM_SELECT_REGEX = 0,
  STCS_STREAM_SELECT_BYTES
} STCSStreamSelectKind;

typedef enum {
  STCS_STREAM_ON_MISSING_WARNING = 0,
  STCS_STREAM_ON_MISSING_ERROR
} STCSStreamOnMissing;

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
  STCSStreamSelectKind kind;
  STCSStreamOnMissing on_missing;
  STCSTextToValue *converter;

  /* regex */
  GRegex *regex;
  gint group;

  /* bytes */
  gsize offset;
  gsize length;
  STCSStreamByteEncoding encoding;
} STCSStreamMap;

struct _STCSStreamReader {
  STCSReader parent_instance;
  GHashTable *maps; /* id → STCSStreamMap* */
};

G_DEFINE_TYPE(STCSStreamReader, stcs_stream_reader, STCS_TYPE_READER)

typedef struct {
  STCSStreamReader *stream_reader;
  STCSReader *reader;
  const guint8 *data;
  gsize len;
  GError **error;
  gboolean ok;
} STCSStreamReaderParseCtx;

static void
stcs_stream_map_free(STCSStreamMap *map)
{
  if (map == NULL)
    return;
  if (map->regex != NULL)
    g_regex_unref(map->regex);
  if (map->converter != NULL)
    stcs_text_to_value_free(map->converter);
  g_free(map);
}

static gboolean
stcs_stream_parse_on_missing(const gchar *s, STCSStreamOnMissing *out, GError **error)
{
  if (s == NULL || *s == '\0' || g_strcmp0(s, "warning") == 0)
    {
      *out = STCS_STREAM_ON_MISSING_WARNING;
      return TRUE;
    }
  if (g_strcmp0(s, "error") == 0)
    {
      *out = STCS_STREAM_ON_MISSING_ERROR;
      return TRUE;
    }
  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported on-missing `%s' (use warning or error)", s);
  return FALSE;
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
      return 0; /* variable */
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
stcs_stream_text_to_double(const gchar *text, gdouble *out, GError **error)
{
  gchar *end = NULL;
  gdouble v;

  if (text == NULL || *text == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "empty text cannot be converted to double");
      return FALSE;
    }

  v = g_ascii_strtod(text, &end);
  if (end == text || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "text `%s' is not a number", text);
      return FALSE;
    }

  *out = v;
  return TRUE;
}

static gboolean
stcs_stream_bytes_to_double(const guint8 *data,
			    gsize len,
			    STCSStreamByteEncoding encoding,
			    STCSTextToValue *converter,
			    gdouble *out,
			    GError **error)
{
  if (encoding == STCS_STREAM_ENC_ASCII)
    {
      gchar *text;
      gboolean r;

      text = g_strndup((const gchar *) data, len);
      g_strchomp(text);
      if (converter != NULL)
	r = stcs_text_to_value_apply(converter, text, out, error);
      else
	r = stcs_stream_text_to_double(text, out, error);
      g_free(text);
      return r;
    }

  if (converter != NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "text-to-value requires encoding=ascii (or a regex map)");
      return FALSE;
    }

  if (len != stcs_stream_encoding_size(encoding))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "byte slice length %" G_GSIZE_FORMAT " does not match encoding size %" G_GSIZE_FORMAT,
		  len, stcs_stream_encoding_size(encoding));
      return FALSE;
    }

  switch (encoding)
    {
    case STCS_STREAM_ENC_UINT8:
      *out = (gdouble) data[0];
      return TRUE;
    case STCS_STREAM_ENC_INT8:
      *out = (gdouble) (gint8) data[0];
      return TRUE;
    case STCS_STREAM_ENC_UINT16BE:
      *out = (gdouble) ((guint16) data[0] << 8 | data[1]);
      return TRUE;
    case STCS_STREAM_ENC_UINT16LE:
      *out = (gdouble) ((guint16) data[1] << 8 | data[0]);
      return TRUE;
    case STCS_STREAM_ENC_INT16BE:
      *out = (gdouble) (gint16) ((guint16) data[0] << 8 | data[1]);
      return TRUE;
    case STCS_STREAM_ENC_INT16LE:
      *out = (gdouble) (gint16) ((guint16) data[1] << 8 | data[0]);
      return TRUE;
    case STCS_STREAM_ENC_UINT32BE:
      *out = (gdouble) ((guint32) data[0] << 24 | (guint32) data[1] << 16 |
			(guint32) data[2] << 8 | data[3]);
      return TRUE;
    case STCS_STREAM_ENC_UINT32LE:
      *out = (gdouble) ((guint32) data[3] << 24 | (guint32) data[2] << 16 |
			(guint32) data[1] << 8 | data[0]);
      return TRUE;
    case STCS_STREAM_ENC_INT32BE:
      *out = (gdouble) (gint32) ((guint32) data[0] << 24 | (guint32) data[1] << 16 |
				 (guint32) data[2] << 8 | data[3]);
      return TRUE;
    case STCS_STREAM_ENC_INT32LE:
      *out = (gdouble) (gint32) ((guint32) data[3] << 24 | (guint32) data[2] << 16 |
				 (guint32) data[1] << 8 | data[0]);
      return TRUE;
    default:
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unsupported encoding");
      return FALSE;
    }
}

static gboolean
stcs_stream_extract_regex(STCSStreamMap *map,
			  const guint8 *data,
			  gsize len,
			  gdouble *out,
			  GError **error)
{
  GMatchInfo *info = NULL;
  gchar *subject = NULL;
  gchar *piece = NULL;
  gboolean r = FALSE;
  gboolean matched;

  /* GRegex needs a NUL-terminated string; copy and allow embedded NULs to truncate. */
  subject = g_malloc(len + 1);
  memcpy(subject, data, len);
  subject[len] = '\0';

  matched = g_regex_match_full(map->regex, subject, (gssize) len, 0, 0, &info, error);
  if (*error != NULL)
    goto out;

  if (!matched)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "regex did not match");
      goto out;
    }

  if (map->group < 0 || map->group >= g_match_info_get_match_count(info))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "capture group %d is not available (match has %d groups)",
		  map->group, g_match_info_get_match_count(info));
      goto out;
    }

  piece = g_match_info_fetch(info, map->group);
  if (piece == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "capture group %d is empty", map->group);
      goto out;
    }

  if (map->converter != NULL)
    r = stcs_text_to_value_apply(map->converter, piece, out, error);
  else
    r = stcs_stream_text_to_double(piece, out, error);

out:
  g_free(piece);
  if (info != NULL)
    g_match_info_free(info);
  g_free(subject);
  return r;
}

static gboolean
stcs_stream_extract_bytes(STCSStreamMap *map,
			  const guint8 *data,
			  gsize len,
			  gdouble *out,
			  GError **error)
{
  if (map->offset > len || map->length > len - map->offset)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "byte range offset=%" G_GSIZE_FORMAT " length=%" G_GSIZE_FORMAT
		  " is outside payload (%" G_GSIZE_FORMAT " bytes)",
		  map->offset, map->length, len);
      return FALSE;
    }

  return stcs_stream_bytes_to_double(data + map->offset,
				     map->length,
				     map->encoding,
				     map->converter,
				     out,
				     error);
}

static gboolean
stcs_stream_extract(STCSStreamMap *map,
		    const guint8 *data,
		    gsize len,
		    gdouble *out,
		    GError **error)
{
  if (map->kind == STCS_STREAM_SELECT_REGEX)
    return stcs_stream_extract_regex(map, data, len, out, error);
  return stcs_stream_extract_bytes(map, data, len, out, error);
}

static void
stcs_stream_reader_parse_mapping(const gchar *key,
				 STCSDatapoint *datapoint,
				 gpointer user_data)
{
  STCSStreamReaderParseCtx *ctx = user_data;
  STCSStreamMap *map;
  gdouble value = 0.0;
  GError *local_error = NULL;

  (void) datapoint;

  if (!ctx->ok)
    return;

  map = g_hash_table_lookup(ctx->stream_reader->maps, key);
  if (map == NULL)
    {
      LOGW("map id=`%s' has no selector state", key);
      return;
    }

  if (!stcs_stream_extract(map, ctx->data, ctx->len, &value, &local_error))
    {
      gboolean is_missing = (local_error != NULL &&
			     local_error->domain == G_IO_ERROR &&
			     local_error->code == G_IO_ERROR_NOT_FOUND);

      if (is_missing && map->on_missing == STCS_STREAM_ON_MISSING_WARNING)
	{
	  LOGW("map id=`%s': %s", key, local_error->message);
	  g_clear_error(&local_error);
	  return;
	}

      LOGW("map id=`%s': %s", key, local_error ? local_error->message : "unknown error");
      if (ctx->error != NULL && *ctx->error == NULL && local_error != NULL)
	g_propagate_error(ctx->error, local_error);
      else
	g_clear_error(&local_error);
      ctx->ok = FALSE;
      return;
    }

  if (!stcs_reader_push_value(ctx->reader, key, value, ctx->error))
    {
      ctx->ok = FALSE;
      return;
    }

  LOGD("pushed id=`%s' value=%g", key, value);
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
		  "stream-reader <map> attribute %s is empty", name);
      return FALSE;
    }

  v = g_ascii_strtoull(s, &end, 10);
  if (end == s || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-reader <map> attribute %s is not a valid integer", name);
      return FALSE;
    }

  *out = v;
  return TRUE;
}

static gboolean
stcs_stream_reader_configure_map(STCSStreamReader *self,
				 xmlNodePtr node,
				 STCSDatapointLookupFunc lookup_datapoint,
				 gpointer lookup_data,
				 GError **error)
{
  xmlChar *id_attr = NULL;
  xmlChar *datapoint_attr = NULL;
  xmlChar *match_attr = NULL;
  xmlChar *group_attr = NULL;
  xmlChar *offset_attr = NULL;
  xmlChar *length_attr = NULL;
  xmlChar *encoding_attr = NULL;
  xmlChar *on_missing_attr = NULL;
  const gchar *id;
  const gchar *datapoint_id;
  STCSDatapoint *datapoint;
  STCSStreamMap *map = NULL;
  STCSTextToValue *converter = NULL;
  xmlNodePtr child;
  gboolean has_match;
  gboolean has_offset;
  gboolean has_length;
  gboolean r = FALSE;

  id_attr = xmlGetProp(node, (const xmlChar *) "id");
  datapoint_attr = xmlGetProp(node, (const xmlChar *) "datapoint");
  match_attr = xmlGetProp(node, (const xmlChar *) "match");
  group_attr = xmlGetProp(node, (const xmlChar *) "group");
  offset_attr = xmlGetProp(node, (const xmlChar *) "offset");
  length_attr = xmlGetProp(node, (const xmlChar *) "length");
  encoding_attr = xmlGetProp(node, (const xmlChar *) "encoding");
  on_missing_attr = xmlGetProp(node, (const xmlChar *) "on-missing");

  if (id_attr == NULL || *id_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-reader <map> requires a non-empty id attribute");
      goto out;
    }

  if (datapoint_attr == NULL || *datapoint_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-reader <map> requires a non-empty datapoint attribute");
      goto out;
    }

  id = (const gchar *) id_attr;
  datapoint_id = (const gchar *) datapoint_attr;

  if (g_hash_table_contains(self->maps, id))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-reader duplicate map id=`%s'", id);
      goto out;
    }

  datapoint = lookup_datapoint(datapoint_id, lookup_data);
  if (datapoint == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "datapoint `%s' not found (stream-reader id=%s)",
		  datapoint_id, id);
      goto out;
    }

  has_match = (match_attr != NULL && *match_attr != '\0');
  has_offset = (offset_attr != NULL);
  has_length = (length_attr != NULL);

  if (has_match && (has_offset || has_length))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-reader <map id=\"%s\"> cannot mix match with offset/length",
		  id);
      goto out;
    }

  if (!has_match && !(has_offset && has_length))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "stream-reader <map id=\"%s\"> requires either match= or offset=+length=",
		  id);
      goto out;
    }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "text-to-value"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside stream-reader <map>, expected <text-to-value>",
		      (const char *) child->name);
	  goto out;
	}

      if (converter != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-reader <map id=\"%s\"> allows at most one <text-to-value>",
		      id);
	  goto out;
	}

      converter = stcs_text_to_value_new_from_node(child, error);
      if (converter == NULL)
	goto out;
    }

  map = g_new0(STCSStreamMap, 1);
  if (!stcs_stream_parse_on_missing((const gchar *) on_missing_attr, &map->on_missing, error))
    goto out;

  if (has_match)
    {
      GError *regex_error = NULL;

      map->kind = STCS_STREAM_SELECT_REGEX;
      map->group = 0;
      if (group_attr != NULL && *group_attr != '\0')
	{
	  guint64 g = 0;
	  if (!stcs_stream_parse_uint64_attr((const gchar *) group_attr, &g, "group", error))
	    goto out;
	  if (g > (guint64) G_MAXINT)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "stream-reader <map id=\"%s\"> group is too large", id);
	      goto out;
	    }
	  map->group = (gint) g;
	}

      map->regex = g_regex_new((const gchar *) match_attr,
			       G_REGEX_RAW | G_REGEX_OPTIMIZE,
			       0,
			       &regex_error);
      if (map->regex == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-reader <map id=\"%s\"> invalid regex: %s",
		      id, regex_error ? regex_error->message : "unknown");
	  g_clear_error(&regex_error);
	  goto out;
	}
    }
  else
    {
      guint64 offset = 0;
      guint64 length = 0;

      map->kind = STCS_STREAM_SELECT_BYTES;
      if (!stcs_stream_parse_uint64_attr((const gchar *) offset_attr, &offset, "offset", error))
	goto out;
      if (!stcs_stream_parse_uint64_attr((const gchar *) length_attr, &length, "length", error))
	goto out;
      if (length == 0)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "stream-reader <map id=\"%s\"> length must be > 0", id);
	  goto out;
	}
      map->offset = (gsize) offset;
      map->length = (gsize) length;
      if (!stcs_stream_parse_encoding((const gchar *) encoding_attr, &map->encoding, error))
	goto out;

      if (map->encoding != STCS_STREAM_ENC_ASCII)
	{
	  gsize need = stcs_stream_encoding_size(map->encoding);
	  if (map->length != need)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "stream-reader <map id=\"%s\"> length=%" G_GSIZE_FORMAT
			  " does not match encoding size %" G_GSIZE_FORMAT,
			  id, map->length, need);
	      goto out;
	    }
	  if (converter != NULL)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "stream-reader <map id=\"%s\"> text-to-value requires encoding=ascii",
			  id);
	      goto out;
	    }
	}
    }

  map->converter = converter;
  converter = NULL;

  stcs_reader_add_mapping(STCS_READER(self), id, datapoint);
  g_hash_table_insert(self->maps, g_strdup(id), map);

  if (has_match)
    LOGD("map id=`%s' → datapoint=`%s' regex group=%d on-missing=%s%s",
	 id, datapoint_id, map->group,
	 map->on_missing == STCS_STREAM_ON_MISSING_ERROR ? "error" : "warning",
	 map->converter != NULL ? " (text-to-value)" : "");
  else
    LOGD("map id=`%s' → datapoint=`%s' offset=%" G_GSIZE_FORMAT
	 " length=%" G_GSIZE_FORMAT " encoding=%s on-missing=%s%s",
	 id, datapoint_id, map->offset, map->length,
	 encoding_attr ? (const char *) encoding_attr : "ascii",
	 map->on_missing == STCS_STREAM_ON_MISSING_ERROR ? "error" : "warning",
	 map->converter != NULL ? " (text-to-value)" : "");

  map = NULL;
  r = TRUE;

out:
  if (map != NULL)
    stcs_stream_map_free(map);
  if (converter != NULL)
    stcs_text_to_value_free(converter);
  if (id_attr != NULL)
    xmlFree(id_attr);
  if (datapoint_attr != NULL)
    xmlFree(datapoint_attr);
  if (match_attr != NULL)
    xmlFree(match_attr);
  if (group_attr != NULL)
    xmlFree(group_attr);
  if (offset_attr != NULL)
    xmlFree(offset_attr);
  if (length_attr != NULL)
    xmlFree(length_attr);
  if (encoding_attr != NULL)
    xmlFree(encoding_attr);
  if (on_missing_attr != NULL)
    xmlFree(on_missing_attr);
  return r;
}

static gboolean
stcs_stream_reader_configure(STCSReader *reader,
			     xmlNodePtr node,
			     STCSDatapointLookupFunc lookup_datapoint,
			     gpointer lookup_data,
			     GError **error)
{
  STCSStreamReader *self = STCS_STREAM_READER(reader);
  xmlNodePtr child;

  g_return_val_if_fail(lookup_datapoint != NULL, FALSE);

  g_hash_table_remove_all(self->maps);

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "map"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside stream-reader, expected <map>",
		      (const char *) child->name);
	  return FALSE;
	}

      if (!stcs_stream_reader_configure_map(self, child, lookup_datapoint, lookup_data, error))
	return FALSE;
    }

  return TRUE;
}

static gboolean
stcs_stream_reader_parse(STCSReader *reader, GBytes *data, GError **error)
{
  STCSStreamReader *self = STCS_STREAM_READER(reader);
  STCSStreamReaderParseCtx ctx;
  gsize len = 0;
  const guint8 *buf;

  g_return_val_if_fail(data != NULL, FALSE);

  buf = g_bytes_get_data(data, &len);
  if (buf == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "null stream payload");
      return FALSE;
    }

  ctx.stream_reader = self;
  ctx.reader = reader;
  ctx.data = buf;
  ctx.len = len;
  ctx.error = error;
  ctx.ok = TRUE;

  stcs_reader_foreach_mapping(reader, stcs_stream_reader_parse_mapping, &ctx);
  return ctx.ok;
}

static void
stcs_stream_reader_dispose(GObject *object)
{
  STCSStreamReader *self = STCS_STREAM_READER(object);

  g_clear_pointer(&self->maps, g_hash_table_unref);

  G_OBJECT_CLASS(stcs_stream_reader_parent_class)->dispose(object);
}

static void
stcs_stream_reader_class_init(STCSStreamReaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSReaderClass *reader_class = STCS_READER_CLASS(klass);

  object_class->dispose = stcs_stream_reader_dispose;

  reader_class->configure = stcs_stream_reader_configure;
  reader_class->parse = stcs_stream_reader_parse;
}

static void
stcs_stream_reader_init(STCSStreamReader *self)
{
  self->maps = g_hash_table_new_full(g_str_hash,
				     g_str_equal,
				     g_free,
				     (GDestroyNotify) stcs_stream_map_free);
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("stream-reader", STCS_PLUGIN_CATEGORY_READER, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_STREAM_READER, NULL);
}
