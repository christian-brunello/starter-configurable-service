/*
 * starter-configurable-service - http-transport.c
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
#include "starter/configurable-service/transport.h"
#include "starter/configurable-service/cron.h"
#include "http-session.h"

#define LOGD(msg...) g_log("http-transport", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("http-transport", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("http-transport", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_HTTP_TRANSPORT (stcs_http_transport_get_type())
G_DECLARE_FINAL_TYPE(STCSHttpTransport, stcs_http_transport, STCS, HTTP_TRANSPORT, STCSTransport)

typedef enum {
  STCS_HTTP_AUTH_NONE = 0,
  STCS_HTTP_AUTH_HEADER,
  STCS_HTTP_AUTH_BASIC,
  STCS_HTTP_AUTH_FORM
} STCSHttpAuthType;

typedef enum {
  STCS_HTTP_WRITE_TARGET_BODY = 0,
  STCS_HTTP_WRITE_TARGET_URL
} STCSHttpWriteTarget;

#define STCS_HTTP_PAYLOAD_PLACEHOLDER "{{payload}}"

struct _STCSHttpTransport {
  STCSTransport parent_instance;

  gchar *url;
  gchar *method;
  STCSCron *cron; /* read-mode schedule; NULL until configured */
  gchar *cookie_jar_path;
  gboolean write_mode; /* TRUE = write; FALSE = GET poll read */
  STCSHttpWriteTarget write_target; /* body (default) or url */

  GHashTable *headers; /* name → value */

  STCSHttpAuthType auth_type;
  gchar *login_url;
  gchar *login_method;
  gchar *username;
  gchar *password;
  gchar *token;
  gchar *login_body;
  gchar *login_content_type;
  GArray *relogin_statuses; /* guint */

  STCSHttpSession *session;
  gboolean authenticated;
  guint poll_source_id;          /* delay before next poll */
  GCancellable *poll_cancellable;
  gboolean poll_enabled;         /* read-mode polling armed */
  guint poll_epoch;              /* bumped on stop to drop stale callbacks */
  gboolean poll_relogin_tried;   /* one relogin attempt per poll cycle */
};

G_DEFINE_TYPE(STCSHttpTransport, stcs_http_transport, STCS_TYPE_TRANSPORT)

static void stcs_http_transport_poll_stop(STCSHttpTransport *self);
static void stcs_http_transport_poll_schedule_next(STCSHttpTransport *self, guint epoch);
static gboolean stcs_http_transport_poll_delay_cb(gpointer user_data);
static void stcs_http_transport_poll_begin(STCSHttpTransport *self);

static gboolean
stcs_http_transport_method_is_write(const gchar *method)
{
  return g_ascii_strcasecmp(method, "POST") == 0
    || g_ascii_strcasecmp(method, "PUT") == 0;
}

static gboolean
stcs_http_transport_method_is_read(const gchar *method)
{
  return g_ascii_strcasecmp(method, "GET") == 0;
}

/* Case-insensitive lookup in headers table; returns transfer-none value or NULL. */
static const gchar *
stcs_http_transport_lookup_header(GHashTable *headers, const gchar *name)
{
  GHashTableIter iter;
  gpointer key, value;

  if (headers == NULL || name == NULL)
    return NULL;

  if (g_hash_table_lookup_extended(headers, name, NULL, &value))
    return (const gchar *) value;

  g_hash_table_iter_init(&iter, headers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      if (g_ascii_strcasecmp((const gchar *) key, name) == 0)
	return (const gchar *) value;
    }

  return NULL;
}

/* application/x-www-form-urlencoded escape (works on GLib < 2.66). */
static gchar *
stcs_http_transport_form_escape(const gchar *unescaped)
{
  GString *out;
  const guchar *p;

  if (unescaped == NULL)
    return g_strdup("");

  out = g_string_new(NULL);
  for (p = (const guchar *) unescaped; *p != '\0'; p++)
    {
      guchar c = *p;

      if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
	g_string_append_c(out, (gchar) c);
      else if (c == ' ')
	g_string_append_c(out, '+');
      else
	g_string_append_printf(out, "%%%02X", c);
    }

  return g_string_free(out, FALSE);
}

static gboolean
stcs_http_transport_url_has_payload(const gchar *url)
{
  return url != NULL && strstr(url, STCS_HTTP_PAYLOAD_PLACEHOLDER) != NULL;
}

/*
 * Replace every {{payload}} in @url_template with the percent-encoded @payload.
 * Returns a newly allocated URL (transfer full).
 */
static gchar *
stcs_http_transport_expand_url_payload(const gchar *url_template,
				       const guint8 *payload,
				       gsize payload_len,
				       GError **error)
{
  gchar *raw = NULL;
  gchar *escaped = NULL;
  GString *out = NULL;
  const gchar *p;
  const gchar *ph = STCS_HTTP_PAYLOAD_PLACEHOLDER;
  gsize ph_len = strlen(ph);

  if (url_template == NULL || !stcs_http_transport_url_has_payload(url_template))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "write-target=url requires %s in <url>",
		  STCS_HTTP_PAYLOAD_PLACEHOLDER);
      return NULL;
    }

  raw = g_strndup((const gchar *) payload, payload_len);
  escaped = stcs_http_transport_form_escape(raw);
  g_free(raw);

  out = g_string_new(NULL);
  p = url_template;
  while (*p != '\0')
    {
      if (strncmp(p, ph, ph_len) == 0)
	{
	  g_string_append(out, escaped);
	  p += ph_len;
	}
      else
	{
	  g_string_append_c(out, *p);
	  p++;
	}
    }

  g_free(escaped);
  return g_string_free(out, FALSE);
}

static gboolean
stcs_http_transport_parse_write_target(const gchar *text,
				       STCSHttpWriteTarget *out,
				       GError **error)
{
  if (text == NULL || *text == '\0' || g_strcmp0(text, "body") == 0)
    {
      *out = STCS_HTTP_WRITE_TARGET_BODY;
      return TRUE;
    }
  if (g_strcmp0(text, "url") == 0)
    {
      *out = STCS_HTTP_WRITE_TARGET_URL;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported write-target `%s' (use body or url)",
	      text);
  return FALSE;
}

static gboolean
stcs_http_transport_parse_string(xmlNodePtr node, gchar **out, GError **error)
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
stcs_http_transport_parse_headers(STCSHttpTransport *self,
				  xmlNodePtr node,
				  GError **error)
{
  xmlNodePtr child;

  for (child = node->children; child != NULL; child = child->next)
    {
      xmlChar *name_attr;
      xmlChar *value_attr;
      const gchar *name;
      const gchar *value;

      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (!xmlStrEqual(child->name, (const xmlChar *) "header"))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside <headers>",
		      (const char *) child->name);
	  return FALSE;
	}

      name_attr = xmlGetProp(child, (const xmlChar *) "name");
      value_attr = xmlGetProp(child, (const xmlChar *) "value");
      name = name_attr != NULL ? (const gchar *) name_attr : "";
      value = value_attr != NULL ? (const gchar *) value_attr : "";

      if (*name == '\0' || *value == '\0')
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "<header> requires non-empty name and value attributes");
	  xmlFree(name_attr);
	  xmlFree(value_attr);
	  return FALSE;
	}

      if (g_hash_table_contains(self->headers, name))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_EXISTS,
		      "duplicate HTTP header name `%s'", name);
	  xmlFree(name_attr);
	  xmlFree(value_attr);
	  return FALSE;
	}

      g_hash_table_insert(self->headers, g_strdup(name), g_strdup(value));
      xmlFree(name_attr);
      xmlFree(value_attr);
    }

  return TRUE;
}

static gboolean
stcs_http_transport_parse_relogin_statuses(STCSHttpTransport *self,
					   const gchar *text,
					   GError **error)
{
  gchar **parts;
  guint i;

  g_array_set_size(self->relogin_statuses, 0);

  parts = g_strsplit(text, ",", 0);
  for (i = 0; parts[i] != NULL; i++)
    {
      gchar *part = g_strstrip(parts[i]);
      gchar *end = NULL;
      unsigned long long value;

      if (*part == '\0')
	continue;

      value = g_ascii_strtoull(part, &end, 10);
      if (end == part || *end != '\0' || value < 100 || value > 599)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "invalid relogin-on-status value `%s'", part);
	  g_strfreev(parts);
	  return FALSE;
	}

      {
	guint status = (guint) value;
	g_array_append_val(self->relogin_statuses, status);
      }
    }

  g_strfreev(parts);
  return TRUE;
}

static gboolean
stcs_http_transport_parse_auth(STCSHttpTransport *self,
			       xmlNodePtr node,
			       GError **error)
{
  xmlChar *type_attr;
  xmlNodePtr child;
  const gchar *type;

  type_attr = xmlGetProp(node, (const xmlChar *) "type");
  if (type_attr == NULL || *type_attr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "<auth> requires a non-empty type attribute");
      xmlFree(type_attr);
      return FALSE;
    }

  type = (const gchar *) type_attr;
  if (g_strcmp0(type, "none") == 0)
    self->auth_type = STCS_HTTP_AUTH_NONE;
  else if (g_strcmp0(type, "header") == 0)
    self->auth_type = STCS_HTTP_AUTH_HEADER;
  else if (g_strcmp0(type, "basic") == 0)
    self->auth_type = STCS_HTTP_AUTH_BASIC;
  else if (g_strcmp0(type, "form") == 0)
    self->auth_type = STCS_HTTP_AUTH_FORM;
  else
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "unsupported auth type `%s' (use none|header|basic|form)",
		  type);
      xmlFree(type_attr);
      return FALSE;
    }

  xmlFree(type_attr);

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "login-url"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->login_url, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "login-method"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->login_method, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "username"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->username, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "password"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->password, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "token"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->token, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "body"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->login_body, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "content-type"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->login_content_type, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "relogin-on-status"))
	{
	  xmlChar *content = xmlNodeGetContent(child);
	  gboolean ok;

	  if (content == NULL || *content == '\0')
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "element <relogin-on-status> has empty content");
	      xmlFree(content);
	      return FALSE;
	    }

	  ok = stcs_http_transport_parse_relogin_statuses(self, (const gchar *) content, error);
	  xmlFree(content);
	  if (!ok)
	    return FALSE;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside <auth>",
		      (const char *) child->name);
	  return FALSE;
	}
    }

  if (self->auth_type == STCS_HTTP_AUTH_HEADER &&
      (self->token == NULL || *self->token == '\0'))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "auth type=header requires <token>");
      return FALSE;
    }

  if (self->auth_type == STCS_HTTP_AUTH_BASIC &&
      (self->username == NULL || self->password == NULL))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "auth type=basic requires <username> and <password>");
      return FALSE;
    }

  if (self->auth_type == STCS_HTTP_AUTH_FORM)
    {
      if (self->login_url == NULL || *self->login_url == '\0')
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "auth type=form requires <login-url>");
	  return FALSE;
	}

      if (self->login_body == NULL &&
	  (self->username == NULL || self->password == NULL))
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "auth type=form requires <body> or <username>+<password>");
	  return FALSE;
	}
    }

  return TRUE;
}

static void
stcs_http_transport_poll_stop(STCSHttpTransport *self)
{
  self->poll_enabled = FALSE;
  self->poll_epoch++;

  if (self->poll_source_id != 0)
    {
      g_source_remove(self->poll_source_id);
      self->poll_source_id = 0;
    }

  if (self->poll_cancellable != NULL)
    g_cancellable_cancel(self->poll_cancellable);
}

static void
stcs_http_transport_poll_reset_cancellable(STCSHttpTransport *self)
{
  g_clear_object(&self->poll_cancellable);
  self->poll_cancellable = g_cancellable_new();
}

static void
stcs_http_transport_reset_session(STCSHttpTransport *self)
{
  stcs_http_session_free(self->session);
  self->session = NULL;
  self->authenticated = FALSE;
}

static GHashTable *
stcs_http_transport_build_request_headers(STCSHttpTransport *self)
{
  GHashTable *headers;
  GHashTableIter iter;
  gpointer key, value;

  headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  g_hash_table_iter_init(&iter, self->headers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    g_hash_table_insert(headers, g_strdup(key), g_strdup(value));

  if (self->auth_type == STCS_HTTP_AUTH_HEADER)
    {
      gchar *auth = g_strdup_printf("Bearer %s", self->token);
      g_hash_table_insert(headers, g_strdup("Authorization"), auth);
    }
  else if (self->auth_type == STCS_HTTP_AUTH_BASIC)
    {
      gchar *plain = g_strdup_printf("%s:%s", self->username, self->password);
      gchar *b64 = g_base64_encode((const guchar *) plain, strlen(plain));
      gchar *auth = g_strdup_printf("Basic %s", b64);

      g_hash_table_insert(headers, g_strdup("Authorization"), auth);
      g_free(b64);
      g_free(plain);
    }

  return headers;
}

static gboolean
stcs_http_transport_status_needs_relogin(STCSHttpTransport *self, guint status)
{
  guint i;

  for (i = 0; i < self->relogin_statuses->len; i++)
    {
      if (g_array_index(self->relogin_statuses, guint, i) == status)
	return TRUE;
    }

  return FALSE;
}

static gboolean
stcs_http_transport_do_login(STCSHttpTransport *self, GError **error)
{
  GHashTable *headers;
  STCSHttpResponse response;
  GBytes *body = NULL;
  const gchar *content_type = NULL;
  const gchar *method;
  gboolean ok = FALSE;
  gchar *owned_body = NULL;

  if (self->auth_type == STCS_HTTP_AUTH_NONE ||
      self->auth_type == STCS_HTTP_AUTH_HEADER ||
      self->auth_type == STCS_HTTP_AUTH_BASIC)
    {
      self->authenticated = TRUE;
      return TRUE;
    }

  /* form */
  method = self->login_method != NULL ? self->login_method : "POST";

  if (self->login_body != NULL)
    {
      body = g_bytes_new_static(self->login_body, strlen(self->login_body));
      content_type = self->login_content_type != NULL
	? self->login_content_type
	: "application/json";
    }
  else
    {
      gchar *user_esc = stcs_http_transport_form_escape(self->username);
      gchar *pass_esc = stcs_http_transport_form_escape(self->password);

      owned_body = g_strdup_printf("username=%s&password=%s", user_esc, pass_esc);
      g_free(user_esc);
      g_free(pass_esc);

      body = g_bytes_new_take(owned_body, strlen(owned_body));
      owned_body = NULL;
      content_type = self->login_content_type != NULL
	? self->login_content_type
	: "application/x-www-form-urlencoded";
    }

  headers = stcs_http_transport_build_request_headers(self);

  LOGI("login %s %s", method, self->login_url);

  if (!stcs_http_session_request(self->session,
				 method,
				 self->login_url,
				 headers,
				 body,
				 content_type,
				 &response,
				 error))
    {
      g_hash_table_unref(headers);
      g_bytes_unref(body);
      return FALSE;
    }

  g_hash_table_unref(headers);
  g_bytes_unref(body);

  if (response.status < 200 || response.status >= 300)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "login failed with HTTP status %u",
		  response.status);
      stcs_http_response_clear(&response);
      self->authenticated = FALSE;
      return FALSE;
    }

  self->authenticated = TRUE;
  ok = TRUE;
  stcs_http_response_clear(&response);
  return ok;
}

static gboolean
stcs_http_transport_ensure_session(STCSHttpTransport *self, GError **error)
{
  if (self->session != NULL)
    return TRUE;

  self->session = stcs_http_session_new(self->cookie_jar_path, error);
  if (self->session == NULL)
    return FALSE;

  self->authenticated = FALSE;
  return TRUE;
}

static gboolean
stcs_http_transport_ensure_auth(STCSHttpTransport *self, GError **error)
{
  if (self->authenticated)
    return TRUE;

  return stcs_http_transport_do_login(self, error);
}

typedef struct {
  STCSHttpTransport *self; /* strong ref while cycle is in flight */
  guint epoch;
} STCSHttpPollCycle;

static STCSHttpPollCycle *
stcs_http_poll_cycle_new(STCSHttpTransport *self)
{
  STCSHttpPollCycle *cycle;

  cycle = g_new0(STCSHttpPollCycle, 1);
  cycle->self = g_object_ref(self);
  cycle->epoch = self->poll_epoch;
  return cycle;
}

static void
stcs_http_poll_cycle_free(STCSHttpPollCycle *cycle)
{
  if (cycle == NULL)
    return;

  g_clear_object(&cycle->self);
  g_free(cycle);
}

static gboolean
stcs_http_transport_poll_alive(STCSHttpTransport *self, guint epoch)
{
  return self->poll_enabled
    && self->poll_epoch == epoch
    && self->poll_cancellable != NULL
    && !g_cancellable_is_cancelled(self->poll_cancellable);
}

static gboolean
stcs_http_transport_poll_should_abort(STCSHttpTransport *self,
				      guint epoch,
				      GError *error)
{
  if (!stcs_http_transport_poll_alive(self, epoch))
    return TRUE;

  if (error != NULL &&
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return TRUE;

  return FALSE;
}

static void
stcs_http_transport_poll_schedule_next(STCSHttpTransport *self, guint epoch)
{
  if (!stcs_http_transport_poll_alive(self, epoch))
    return;

  if (self->cron == NULL || !stcs_cron_is_parsed(self->cron))
    {
      LOGW("poll schedule skipped: cron not configured");
      return;
    }

  if (self->poll_source_id != 0)
    {
      g_source_remove(self->poll_source_id);
      self->poll_source_id = 0;
    }

  self->poll_source_id = stcs_cron_timeout_add(self->cron,
					       stcs_http_transport_poll_delay_cb,
					       self);
  if (self->poll_source_id == 0)
    LOGW("failed to arm next poll from cron `%s'",
	 stcs_cron_get_expression(self->cron));
}

static gboolean
stcs_http_transport_poll_delay_cb(gpointer user_data)
{
  STCSHttpTransport *self = user_data;

  self->poll_source_id = 0;

  if (!self->poll_enabled)
    return G_SOURCE_REMOVE;

  stcs_http_transport_poll_begin(self);
  return G_SOURCE_REMOVE;
}

static void
stcs_http_transport_poll_finish_cycle(STCSHttpPollCycle *cycle,
				      GBytes *data,
				      GError *error)
{
  STCSHttpTransport *self = cycle->self;

  if (stcs_http_transport_poll_alive(self, cycle->epoch))
    {
      if (error != NULL)
	{
	  LOGW("poll failed: %s", error->message);
	  stcs_transport_emit_data_error(STCS_TRANSPORT(self), error);
	}
      else if (data != NULL)
	{
	  LOGD("emit %zu bytes from %s",
	       g_bytes_get_size(data),
	       self->url ? self->url : "(null)");
	  stcs_transport_emit_data_ready(STCS_TRANSPORT(self), data);
	}

      stcs_http_transport_poll_schedule_next(self, cycle->epoch);
    }

  stcs_http_poll_cycle_free(cycle);
}

static void stcs_http_transport_poll_on_login(GObject *source,
					      GAsyncResult *result,
					      gpointer user_data);
static void stcs_http_transport_poll_on_exchange(GObject *source,
						 GAsyncResult *result,
						 gpointer user_data);
static void stcs_http_transport_poll_request(STCSHttpPollCycle *cycle);
static void stcs_http_transport_poll_login_async(STCSHttpPollCycle *cycle);

static void
stcs_http_transport_poll_login_async(STCSHttpPollCycle *cycle)
{
  STCSHttpTransport *self = cycle->self;
  GHashTable *headers;
  GBytes *body = NULL;
  const gchar *content_type = NULL;
  const gchar *method;
  gchar *owned_body = NULL;

  if (!stcs_http_transport_poll_alive(self, cycle->epoch))
    {
      stcs_http_poll_cycle_free(cycle);
      return;
    }

  if (self->auth_type == STCS_HTTP_AUTH_NONE ||
      self->auth_type == STCS_HTTP_AUTH_HEADER ||
      self->auth_type == STCS_HTTP_AUTH_BASIC)
    {
      self->authenticated = TRUE;
      stcs_http_transport_poll_request(cycle);
      return;
    }

  method = self->login_method != NULL ? self->login_method : "POST";

  if (self->login_body != NULL)
    {
      body = g_bytes_new_static(self->login_body, strlen(self->login_body));
      content_type = self->login_content_type != NULL
	? self->login_content_type
	: "application/json";
    }
  else
    {
      gchar *user_esc = stcs_http_transport_form_escape(self->username);
      gchar *pass_esc = stcs_http_transport_form_escape(self->password);

      owned_body = g_strdup_printf("username=%s&password=%s", user_esc, pass_esc);
      g_free(user_esc);
      g_free(pass_esc);

      body = g_bytes_new_take(owned_body, strlen(owned_body));
      content_type = self->login_content_type != NULL
	? self->login_content_type
	: "application/x-www-form-urlencoded";
    }

  headers = stcs_http_transport_build_request_headers(self);

  LOGI("login %s %s", method, self->login_url);

  stcs_http_session_request_async(self->session,
				  method,
				  self->login_url,
				  headers,
				  body,
				  content_type,
				  self->poll_cancellable,
				  stcs_http_transport_poll_on_login,
				  cycle);

  g_hash_table_unref(headers);
  g_bytes_unref(body);
}

static void
stcs_http_transport_poll_on_login(GObject *source,
				  GAsyncResult *result,
				  gpointer user_data)
{
  STCSHttpPollCycle *cycle = user_data;
  STCSHttpTransport *self = cycle->self;
  STCSHttpResponse response;
  GError *error = NULL;

  (void) source;

  memset(&response, 0, sizeof(response));

  if (!stcs_http_session_request_finish(self->session, result, &response, &error))
    {
      if (stcs_http_transport_poll_should_abort(self, cycle->epoch, error))
	{
	  g_clear_error(&error);
	  stcs_http_poll_cycle_free(cycle);
	  return;
	}

      self->authenticated = FALSE;
      stcs_http_transport_poll_finish_cycle(cycle, NULL, error);
      g_clear_error(&error);
      return;
    }

  if (!stcs_http_transport_poll_alive(self, cycle->epoch))
    {
      stcs_http_response_clear(&response);
      stcs_http_poll_cycle_free(cycle);
      return;
    }

  if (response.status < 200 || response.status >= 300)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "login failed with HTTP status %u",
		  response.status);
      self->authenticated = FALSE;
      stcs_http_response_clear(&response);
      stcs_http_transport_poll_finish_cycle(cycle, NULL, error);
      g_clear_error(&error);
      return;
    }

  self->authenticated = TRUE;
  stcs_http_response_clear(&response);
  stcs_http_transport_poll_request(cycle);
}

static void
stcs_http_transport_poll_request(STCSHttpPollCycle *cycle)
{
  STCSHttpTransport *self = cycle->self;
  GHashTable *headers;

  if (!stcs_http_transport_poll_alive(self, cycle->epoch))
    {
      stcs_http_poll_cycle_free(cycle);
      return;
    }

  headers = stcs_http_transport_build_request_headers(self);

  stcs_http_session_request_async(self->session,
				  self->method,
				  self->url,
				  headers,
				  NULL,
				  NULL,
				  self->poll_cancellable,
				  stcs_http_transport_poll_on_exchange,
				  cycle);

  g_hash_table_unref(headers);
}

static void
stcs_http_transport_poll_on_exchange(GObject *source,
				     GAsyncResult *result,
				     gpointer user_data)
{
  STCSHttpPollCycle *cycle = user_data;
  STCSHttpTransport *self = cycle->self;
  STCSHttpResponse response;
  GError *error = NULL;
  GBytes *body;

  (void) source;

  memset(&response, 0, sizeof(response));

  if (!stcs_http_session_request_finish(self->session, result, &response, &error))
    {
      if (stcs_http_transport_poll_should_abort(self, cycle->epoch, error))
	{
	  g_clear_error(&error);
	  stcs_http_poll_cycle_free(cycle);
	  return;
	}

      stcs_http_transport_poll_finish_cycle(cycle, NULL, error);
      g_clear_error(&error);
      return;
    }

  if (!stcs_http_transport_poll_alive(self, cycle->epoch))
    {
      stcs_http_response_clear(&response);
      stcs_http_poll_cycle_free(cycle);
      return;
    }

  if (stcs_http_transport_status_needs_relogin(self, response.status) &&
      self->auth_type == STCS_HTTP_AUTH_FORM &&
      !self->poll_relogin_tried)
    {
      LOGI("HTTP %u → re-login", response.status);
      stcs_http_response_clear(&response);
      self->authenticated = FALSE;
      self->poll_relogin_tried = TRUE;
      stcs_http_transport_poll_login_async(cycle);
      return;
    }

  if (response.status < 200 || response.status >= 300)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "HTTP status %u from %s",
		  response.status, self->url);
      stcs_http_response_clear(&response);
      stcs_http_transport_poll_finish_cycle(cycle, NULL, error);
      g_clear_error(&error);
      return;
    }

  body = response.body;
  response.body = NULL;
  stcs_http_response_clear(&response);

  if (body == NULL)
    body = g_bytes_new_static("", 0);

  stcs_http_transport_poll_finish_cycle(cycle, body, NULL);
  g_bytes_unref(body);
}

static void
stcs_http_transport_poll_begin(STCSHttpTransport *self)
{
  STCSHttpPollCycle *cycle;
  GError *error = NULL;

  if (!self->poll_enabled)
    return;

  self->poll_relogin_tried = FALSE;
  cycle = stcs_http_poll_cycle_new(self);

  if (!stcs_http_transport_ensure_session(self, &error))
    {
      stcs_http_transport_poll_finish_cycle(cycle, NULL, error);
      g_clear_error(&error);
      return;
    }

  if (!self->authenticated)
    stcs_http_transport_poll_login_async(cycle);
  else
    stcs_http_transport_poll_request(cycle);
}

static void
stcs_http_transport_poll_start(STCSHttpTransport *self)
{
  stcs_http_transport_poll_stop(self);
  stcs_http_transport_poll_reset_cancellable(self);
  self->poll_enabled = TRUE;
  stcs_http_transport_poll_begin(self);
}

static gboolean
stcs_http_transport_configure(STCSTransport *transport,
			      xmlNodePtr node,
			      GError **error)
{
  STCSHttpTransport *self = STCS_HTTP_TRANSPORT(transport);
  xmlNodePtr child;
  gboolean saw_auth = FALSE;
  gchar *poll_cron = NULL;

  g_return_val_if_fail(node != NULL, FALSE);

  stcs_http_transport_poll_stop(self);
  stcs_http_transport_reset_session(self);

  g_clear_pointer(&self->url, g_free);
  g_clear_pointer(&self->method, g_free);
  g_clear_pointer(&self->cookie_jar_path, g_free);
  g_clear_pointer(&self->login_url, g_free);
  g_clear_pointer(&self->login_method, g_free);
  g_clear_pointer(&self->username, g_free);
  g_clear_pointer(&self->password, g_free);
  g_clear_pointer(&self->token, g_free);
  g_clear_pointer(&self->login_body, g_free);
  g_clear_pointer(&self->login_content_type, g_free);
  g_hash_table_remove_all(self->headers);
  g_array_set_size(self->relogin_statuses, 0);

  self->method = g_strdup("GET");
  self->auth_type = STCS_HTTP_AUTH_NONE;
  self->write_mode = FALSE;
  self->write_target = STCS_HTTP_WRITE_TARGET_BODY;

  {
    guint s401 = 401, s403 = 403;
    g_array_append_val(self->relogin_statuses, s401);
    g_array_append_val(self->relogin_statuses, s403);
  }

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "url"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->url, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "headers"))
	{
	  if (!stcs_http_transport_parse_headers(self, child, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "method"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->method, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "write-target"))
	{
	  gchar *target = NULL;

	  if (!stcs_http_transport_parse_string(child, &target, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	  if (!stcs_http_transport_parse_write_target(target, &self->write_target, error))
	    {
	      g_free(target);
	      g_free(poll_cron);
	      return FALSE;
	    }
	  g_free(target);
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "poll-cron"))
	{
	  if (!stcs_http_transport_parse_string(child, &poll_cron, error))
	    return FALSE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "cookie-jar"))
	{
	  if (!stcs_http_transport_parse_string(child, &self->cookie_jar_path, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "auth"))
	{
	  if (saw_auth)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "duplicate <auth> element");
	      g_free(poll_cron);
	      return FALSE;
	    }
	  saw_auth = TRUE;
	  if (!stcs_http_transport_parse_auth(self, child, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside http-transport config",
		      (const char *) child->name);
	  g_free(poll_cron);
	  return FALSE;
	}
    }

  if (self->url == NULL || *self->url == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "http-transport requires a <url> element");
      g_free(poll_cron);
      return FALSE;
    }

  if (stcs_http_transport_method_is_write(self->method))
    {
      /* POST / PUT: always write; poll-cron forbidden. */
      self->write_mode = TRUE;
      if (poll_cron != NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "http-transport method=%s is write-only and cannot use <poll-cron>",
		      self->method);
	  g_free(poll_cron);
	  return FALSE;
	}
    }
  else if (stcs_http_transport_method_is_read(self->method))
    {
      if (poll_cron != NULL && *poll_cron != '\0')
	{
	  /* GET + poll-cron = read; write-target is ignored. */
	  self->write_mode = FALSE;
	}
      else if (self->write_target == STCS_HTTP_WRITE_TARGET_URL)
	{
	  /* GET + write-target=url = write (Shelly-style). */
	  self->write_mode = TRUE;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "http-transport method=GET requires <poll-cron> (read) "
		      "or <write-target>url</write-target> (write)");
	  g_free(poll_cron);
	  return FALSE;
	}

      if (!self->write_mode)
	{
	  if (self->cron == NULL)
	    self->cron = stcs_cron_new();

	  if (!stcs_cron_parse(self->cron, poll_cron, error))
	    {
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
    }
  else
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "http-transport method `%s' not supported (use GET, POST, or PUT)",
		  self->method);
      g_free(poll_cron);
      return FALSE;
    }

  if (self->write_mode)
    {
      if (self->write_target == STCS_HTTP_WRITE_TARGET_URL)
	{
	  if (!stcs_http_transport_url_has_payload(self->url))
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "http-transport write-target=url requires %s in <url>",
			  STCS_HTTP_PAYLOAD_PLACEHOLDER);
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
      else
	{
	  if (stcs_http_transport_method_is_read(self->method))
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "http-transport method=GET cannot use write-target=body");
	      g_free(poll_cron);
	      return FALSE;
	    }
	  if (stcs_http_transport_url_has_payload(self->url))
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_INVALID_DATA,
			  "http-transport write-target=body must not contain %s in <url>",
			  STCS_HTTP_PAYLOAD_PLACEHOLDER);
	      g_free(poll_cron);
	      return FALSE;
	    }
	}
    }
  else if (stcs_http_transport_url_has_payload(self->url))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "http-transport read URL must not contain %s",
		  STCS_HTTP_PAYLOAD_PLACEHOLDER);
      g_free(poll_cron);
      return FALSE;
    }

  g_free(poll_cron);

  if (!stcs_http_transport_ensure_session(self, error))
    return FALSE;

  LOGI("configured url=%s method=%s mode=%s write-target=%s poll-cron=%s auth=%d cookie-jar=%s headers=%u",
       self->url,
       self->method,
       self->write_mode ? "write" : "read",
       self->write_target == STCS_HTTP_WRITE_TARGET_URL ? "url" : "body",
       (!self->write_mode && self->cron != NULL)
         ? stcs_cron_get_expression(self->cron)
         : "(none)",
       (int) self->auth_type,
       self->cookie_jar_path ? self->cookie_jar_path : "(memory)",
       g_hash_table_size(self->headers));

  if (!self->write_mode)
    stcs_http_transport_poll_start(self);

  return TRUE;
}

typedef struct {
  STCSHttpTransport *self;
  guint64 op_id;
  GBytes *body; /* request body; NULL/empty when write-target=url */
  gchar *url; /* resolved request URL (may differ from template) */
  gchar *content_type;
  gsize nbytes_reported; /* bytes counted toward write-complete */
  gboolean relogin_tried;
} STCSHttpWriteOp;

static void stcs_http_transport_write_send(STCSHttpWriteOp *op);

static void
stcs_http_write_op_free(STCSHttpWriteOp *op)
{
  if (op == NULL)
    return;

  g_clear_object(&op->self);
  g_clear_pointer(&op->body, g_bytes_unref);
  g_free(op->url);
  g_free(op->content_type);
  g_free(op);
}

static void
stcs_http_transport_on_write_done(GObject *source,
				  GAsyncResult *result,
				  gpointer user_data)
{
  STCSHttpWriteOp *op = user_data;
  STCSHttpTransport *self = op->self;
  STCSHttpResponse response;
  GError *error = NULL;

  (void) source;

  memset(&response, 0, sizeof(response));

  if (!stcs_http_session_request_finish(self->session, result, &response, &error))
    {
      stcs_transport_finish_write_error(STCS_TRANSPORT(self), op->op_id, error);
      g_clear_error(&error);
      stcs_http_write_op_free(op);
      return;
    }

  if (stcs_http_transport_status_needs_relogin(self, response.status) &&
      self->auth_type == STCS_HTTP_AUTH_FORM &&
      !op->relogin_tried)
    {
      LOGI("HTTP %u on write → re-login", response.status);
      stcs_http_response_clear(&response);
      self->authenticated = FALSE;
      op->relogin_tried = TRUE;

      if (!stcs_http_transport_do_login(self, &error))
	{
	  stcs_transport_finish_write_error(STCS_TRANSPORT(self), op->op_id, error);
	  g_clear_error(&error);
	  stcs_http_write_op_free(op);
	  return;
	}

      stcs_http_transport_write_send(op);
      return;
    }

  if (response.status < 200 || response.status >= 300)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "HTTP %s %s failed with status %u",
		  self->method,
		  op->url ? op->url : self->url,
		  response.status);
      stcs_http_response_clear(&response);
      stcs_transport_finish_write_error(STCS_TRANSPORT(self), op->op_id, error);
      g_clear_error(&error);
      stcs_http_write_op_free(op);
      return;
    }

  LOGD("wrote %" G_GSIZE_FORMAT " bytes via %s %s (status %u) op_id=%" G_GUINT64_FORMAT,
       op->nbytes_reported, self->method,
       op->url ? op->url : self->url,
       response.status, op->op_id);
  stcs_http_response_clear(&response);
  stcs_transport_finish_write(STCS_TRANSPORT(self), op->op_id, op->nbytes_reported);
  stcs_http_write_op_free(op);
}

static void
stcs_http_transport_write_send(STCSHttpWriteOp *op)
{
  STCSHttpTransport *self = op->self;
  GHashTable *headers;
  const gchar *url;

  url = op->url != NULL ? op->url : self->url;
  headers = stcs_http_transport_build_request_headers(self);
  stcs_http_session_request_async(self->session,
				  self->method,
				  url,
				  headers,
				  op->body,
				  op->content_type,
				  NULL,
				  stcs_http_transport_on_write_done,
				  op);
  g_hash_table_unref(headers);
}

static void
stcs_http_transport_start_write(STCSTransport *transport,
				guint64 op_id,
				GBytes *data)
{
  STCSHttpTransport *self = STCS_HTTP_TRANSPORT(transport);
  STCSHttpWriteOp *op;
  GError *error = NULL;
  const gchar *content_type = NULL;
  const guint8 *payload;
  gsize payload_len = 0;

  if (!self->write_mode)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_SUPPORTED,
		  "http-transport method=%s is read-only",
		  self->method ? self->method : "GET");
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  if (self->url == NULL)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_INITIALIZED,
		  "http-transport is not configured");
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  if (!stcs_http_transport_ensure_session(self, &error) ||
      !stcs_http_transport_ensure_auth(self, &error))
    {
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  payload = g_bytes_get_data(data, &payload_len);

  op = g_new0(STCSHttpWriteOp, 1);
  op->self = g_object_ref(self);
  op->op_id = op_id;
  op->nbytes_reported = payload_len;
  op->relogin_tried = FALSE;

  if (self->write_target == STCS_HTTP_WRITE_TARGET_URL)
    {
      op->url = stcs_http_transport_expand_url_payload(self->url,
						       payload,
						       payload_len,
						       &error);
      if (op->url == NULL)
	{
	  stcs_transport_finish_write_error(transport, op_id, error);
	  g_clear_error(&error);
	  stcs_http_write_op_free(op);
	  return;
	}
      op->body = NULL;
      op->content_type = NULL;
      LOGD("write-target=url → %s %s", self->method, op->url);
    }
  else
    {
      content_type = stcs_http_transport_lookup_header(self->headers, "Content-Type");
      if (content_type == NULL)
	content_type = "application/json";

      op->url = g_strdup(self->url);
      op->body = g_bytes_ref(data);
      op->content_type = g_strdup(content_type);
    }

  stcs_http_transport_write_send(op);
}

static void
stcs_http_transport_dispose(GObject *object)
{
  STCSHttpTransport *self = STCS_HTTP_TRANSPORT(object);

  stcs_http_transport_poll_stop(self);
  g_clear_object(&self->poll_cancellable);
  g_clear_object(&self->cron);
  stcs_http_transport_reset_session(self);

  G_OBJECT_CLASS(stcs_http_transport_parent_class)->dispose(object);
}

static void
stcs_http_transport_finalize(GObject *object)
{
  STCSHttpTransport *self = STCS_HTTP_TRANSPORT(object);

  g_free(self->url);
  g_free(self->method);
  g_free(self->cookie_jar_path);
  g_free(self->login_url);
  g_free(self->login_method);
  g_free(self->username);
  g_free(self->password);
  g_free(self->token);
  g_free(self->login_body);
  g_free(self->login_content_type);
  g_clear_pointer(&self->headers, g_hash_table_unref);
  g_clear_pointer(&self->relogin_statuses, g_array_unref);

  G_OBJECT_CLASS(stcs_http_transport_parent_class)->finalize(object);
}

static void
stcs_http_transport_class_init(STCSHttpTransportClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSTransportClass *transport_class = STCS_TRANSPORT_CLASS(klass);

  object_class->dispose = stcs_http_transport_dispose;
  object_class->finalize = stcs_http_transport_finalize;

  transport_class->configure = stcs_http_transport_configure;
  transport_class->start_write = stcs_http_transport_start_write;
}

static void
stcs_http_transport_init(STCSHttpTransport *self)
{
  self->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->relogin_statuses = g_array_new(FALSE, FALSE, sizeof(guint));
  self->method = NULL;
  self->auth_type = STCS_HTTP_AUTH_NONE;
  self->write_mode = FALSE;
  self->write_target = STCS_HTTP_WRITE_TARGET_BODY;
  self->cron = NULL;
  self->poll_source_id = 0;
  self->poll_cancellable = NULL;
  self->poll_enabled = FALSE;
  self->poll_epoch = 0;
  self->poll_relogin_tried = FALSE;
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("http-transport", STCS_PLUGIN_CATEGORY_TRANSPORT, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_HTTP_TRANSPORT, NULL);
}
