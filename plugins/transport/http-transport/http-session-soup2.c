/*
 * starter-configurable-service - http-session-soup2.c
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

#include <libsoup/soup.h>

#include "http-session.h"

struct _STCSHttpSession {
  SoupSession *session;
  SoupCookieJar *jar;
};

typedef struct {
  STCSHttpSession *session;
  SoupMessage *msg;
  gulong cancel_id;
  GCancellable *cancellable;
} STCSHttpRequestData;

static void
stcs_http_response_free_heap(STCSHttpResponse *response)
{
  stcs_http_response_clear(response);
  g_free(response);
}

void
stcs_http_response_clear(STCSHttpResponse *response)
{
  if (response == NULL)
    return;

  g_clear_pointer(&response->body, g_bytes_unref);
  response->status = 0;
}

STCSHttpSession *
stcs_http_session_new(const gchar *cookie_jar_path, GError **error)
{
  STCSHttpSession *self;
  SoupCookieJar *jar;

  self = g_new0(STCSHttpSession, 1);
  self->session = soup_session_new();

  if (cookie_jar_path != NULL && *cookie_jar_path != '\0')
    jar = soup_cookie_jar_text_new(cookie_jar_path, FALSE);
  else
    jar = soup_cookie_jar_new();

  if (jar == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "unable to create cookie jar%s%s",
		  cookie_jar_path ? " at " : "",
		  cookie_jar_path ? cookie_jar_path : "");
      g_clear_object(&self->session);
      g_free(self);
      return NULL;
    }

  soup_session_add_feature(self->session, SOUP_SESSION_FEATURE(jar));
  self->jar = jar;

  return self;
}

void
stcs_http_session_free(STCSHttpSession *session)
{
  if (session == NULL)
    return;

  g_clear_object(&session->jar);
  g_clear_object(&session->session);
  g_free(session);
}

static SoupMessage *
stcs_http_session_build_message(const gchar *method,
				const gchar *url,
				GHashTable *headers,
				GBytes *body,
				const gchar *content_type,
				GError **error)
{
  SoupMessage *msg;

  msg = soup_message_new(method, url);
  if (msg == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_ARGUMENT,
		  "invalid HTTP URL or method: %s %s", method, url);
      return NULL;
    }

  if (headers != NULL)
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init(&iter, headers);
      while (g_hash_table_iter_next(&iter, &key, &value))
	soup_message_headers_replace(msg->request_headers,
				     (const gchar *) key,
				     (const gchar *) value);
    }

  if (body != NULL)
    {
      gsize len = 0;
      gconstpointer data = g_bytes_get_data(body, &len);

      soup_message_set_request(msg,
			       content_type != NULL ? content_type : "application/octet-stream",
			       SOUP_MEMORY_COPY,
			       data,
			       len);
    }

  return msg;
}

gboolean
stcs_http_session_request(STCSHttpSession *session,
			  const gchar *method,
			  const gchar *url,
			  GHashTable *headers,
			  GBytes *body,
			  const gchar *content_type,
			  STCSHttpResponse *out,
			  GError **error)
{
  SoupMessage *msg;
  guint status;

  g_return_val_if_fail(session != NULL, FALSE);
  g_return_val_if_fail(method != NULL, FALSE);
  g_return_val_if_fail(url != NULL, FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  memset(out, 0, sizeof(*out));

  msg = stcs_http_session_build_message(method, url, headers, body, content_type, error);
  if (msg == NULL)
    return FALSE;

  status = soup_session_send_message(session->session, msg);

  out->status = status;
  if (msg->response_body != NULL && msg->response_body->data != NULL)
    out->body = g_bytes_new(msg->response_body->data, msg->response_body->length);
  else
    out->body = g_bytes_new_static("", 0);

  g_object_unref(msg);

  /* Soup 2 returns status even on HTTP error pages; treat transport failure as 0. */
  if (status == 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "HTTP request failed for %s %s", method, url);
      stcs_http_response_clear(out);
      return FALSE;
    }

  return TRUE;
}

static void
stcs_http_request_data_disconnect_cancel(STCSHttpRequestData *req)
{
  if (req->cancellable != NULL && req->cancel_id != 0)
    {
      g_cancellable_disconnect(req->cancellable, req->cancel_id);
      req->cancel_id = 0;
    }

  g_clear_object(&req->cancellable);
}

static void
stcs_http_request_data_free(STCSHttpRequestData *req)
{
  if (req == NULL)
    return;

  stcs_http_request_data_disconnect_cancel(req);
  /* msg is owned by SoupSession after queue_message until callback. */
  g_free(req);
}

static void
stcs_http_session_on_cancelled(GCancellable *cancellable,
			       gpointer user_data)
{
  STCSHttpRequestData *req = user_data;

  (void) cancellable;

  if (req->session != NULL && req->msg != NULL)
    soup_session_cancel_message(req->session->session, req->msg, SOUP_STATUS_CANCELLED);
}

static void
stcs_http_session_queue_message_cb(SoupSession *soup_session,
				   SoupMessage *msg,
				   gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  STCSHttpRequestData *req = g_task_get_task_data(task);
  STCSHttpResponse *response;
  guint status;

  (void) soup_session;

  stcs_http_request_data_disconnect_cancel(req);

  if (g_task_return_error_if_cancelled(task))
    {
      g_object_unref(task);
      return;
    }

  status = msg->status_code;

  if (status == 0 || status == SOUP_STATUS_CANCELLED)
    {
      g_task_return_new_error(task,
			      G_IO_ERROR,
			      status == SOUP_STATUS_CANCELLED
			        ? G_IO_ERROR_CANCELLED
			        : G_IO_ERROR_FAILED,
			      "HTTP request failed (status %u)",
			      status);
      g_object_unref(task);
      return;
    }

  response = g_new0(STCSHttpResponse, 1);
  response->status = status;
  if (msg->response_body != NULL && msg->response_body->data != NULL)
    response->body = g_bytes_new(msg->response_body->data, msg->response_body->length);
  else
    response->body = g_bytes_new_static("", 0);

  g_task_return_pointer(task, response, (GDestroyNotify) stcs_http_response_free_heap);
  g_object_unref(task);
}

void
stcs_http_session_request_async(STCSHttpSession *session,
				const gchar *method,
				const gchar *url,
				GHashTable *headers,
				GBytes *body,
				const gchar *content_type,
				GCancellable *cancellable,
				GAsyncReadyCallback callback,
				gpointer user_data)
{
  GTask *task;
  STCSHttpRequestData *req;
  SoupMessage *msg;
  GError *local_error = NULL;

  g_return_if_fail(session != NULL);
  g_return_if_fail(method != NULL);
  g_return_if_fail(url != NULL);
  g_return_if_fail(callback != NULL);

  task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, stcs_http_session_request_async);

  if (cancellable != NULL && g_cancellable_set_error_if_cancelled(cancellable, &local_error))
    {
      g_task_return_error(task, local_error);
      g_object_unref(task);
      return;
    }

  msg = stcs_http_session_build_message(method, url, headers, body, content_type, &local_error);
  if (msg == NULL)
    {
      g_task_return_error(task, local_error);
      g_object_unref(task);
      return;
    }

  req = g_new0(STCSHttpRequestData, 1);
  req->session = session;
  req->msg = msg;
  if (cancellable != NULL)
    {
      req->cancellable = g_object_ref(cancellable);
      req->cancel_id = g_cancellable_connect(cancellable,
					     G_CALLBACK(stcs_http_session_on_cancelled),
					     req,
					     NULL);
    }

  g_task_set_task_data(task, req, (GDestroyNotify) stcs_http_request_data_free);

  /* Steals the floating/full ref on msg until the callback runs. */
  soup_session_queue_message(session->session,
			     msg,
			     stcs_http_session_queue_message_cb,
			     task);
}

gboolean
stcs_http_session_request_finish(STCSHttpSession *session,
				 GAsyncResult *result,
				 STCSHttpResponse *out,
				 GError **error)
{
  STCSHttpResponse *response;

  g_return_val_if_fail(session != NULL, FALSE);
  g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(result, stcs_http_session_request_async), FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  memset(out, 0, sizeof(*out));

  response = g_task_propagate_pointer(G_TASK(result), error);
  if (response == NULL)
    return FALSE;

  out->status = response->status;
  out->body = response->body;
  response->body = NULL;
  g_free(response);
  return TRUE;
}
