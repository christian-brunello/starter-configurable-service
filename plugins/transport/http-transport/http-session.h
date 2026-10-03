/*
 * starter-configurable-service - http-session.h
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

#ifndef STCS_HTTP_SESSION_H_INCLUDED
#define STCS_HTTP_SESSION_H_INCLUDED

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _STCSHttpSession STCSHttpSession;

typedef struct _STCSHttpResponse {
  guint status;
  GBytes *body; /* transfer full; may be NULL */
} STCSHttpResponse;

/*
 * cookie_jar_path: optional path for SoupCookieJarText (shared across transports).
 * NULL → in-memory jar only.
 */
STCSHttpSession *stcs_http_session_new(const gchar *cookie_jar_path, GError **error);
void stcs_http_session_free(STCSHttpSession *session);

/*
 * headers: optional GHashTable of gchar* name → gchar* value (transfer none).
 * body/content_type: optional request body.
 * On success fills @out (caller must stcs_http_response_clear).
 */
gboolean stcs_http_session_request(STCSHttpSession *session,
				   const gchar *method,
				   const gchar *url,
				   GHashTable *headers,
				   GBytes *body,
				   const gchar *content_type,
				   STCSHttpResponse *out,
				   GError **error);

/*
 * Async variant. Completes on the thread-default main context.
 * headers/body are consumed into the message before returning (caller may free).
 */
void stcs_http_session_request_async(STCSHttpSession *session,
				     const gchar *method,
				     const gchar *url,
				     GHashTable *headers,
				     GBytes *body,
				     const gchar *content_type,
				     GCancellable *cancellable,
				     GAsyncReadyCallback callback,
				     gpointer user_data);

gboolean stcs_http_session_request_finish(STCSHttpSession *session,
					  GAsyncResult *result,
					  STCSHttpResponse *out,
					  GError **error);

void stcs_http_response_clear(STCSHttpResponse *response);

G_END_DECLS

#endif /* STCS_HTTP_SESSION_H_INCLUDED */
