/*
 * starter-configurable-service - process-transport.c
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
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "starter/configurable-service/transport.h"
#include "starter/configurable-service/cron.h"

#define LOGD(msg...) g_log("process-transport", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("process-transport", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("process-transport", G_LOG_LEVEL_WARNING, msg)

#define STCS_TYPE_PROCESS_TRANSPORT (stcs_process_transport_get_type())
G_DECLARE_FINAL_TYPE(STCSProcessTransport, stcs_process_transport, STCS, PROCESS_TRANSPORT, STCSTransport)

typedef enum {
  STCS_PROCESS_DIRECTION_READ = 0,
  STCS_PROCESS_DIRECTION_WRITE
} STCSProcessDirection;

typedef struct {
  uid_t uid;
  gid_t gid;
  const gchar *user; /* borrowed; valid for child_setup only */
} STCSProcessCreds;

struct _STCSProcessTransport {
  STCSTransport parent_instance;

  gchar **argv; /* NULL-terminated */
  gchar *user;
  STCSProcessCreds creds;
  gboolean creds_resolved;
  STCSProcessDirection direction;
  guint timeout_ms;
  gchar *workdir;
  STCSCron *cron;
  guint poll_source_id;
};

G_DEFINE_TYPE(STCSProcessTransport, stcs_process_transport, STCS_TYPE_TRANSPORT)

static void stcs_process_transport_schedule_next(STCSProcessTransport *self);
static void stcs_process_transport_run_read(STCSProcessTransport *self);

static gboolean
stcs_process_transport_parse_string(xmlNodePtr node, gchar **out, GError **error)
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
stcs_process_transport_parse_uint64(xmlNodePtr node, guint64 *out, GError **error)
{
  xmlChar *content;
  gchar *end = NULL;
  guint64 v;

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

  v = g_ascii_strtoull((const char *) content, &end, 10);
  if (end == (char *) content || *end != '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "element <%s> is not a valid integer",
		  (const char *) node->name);
      xmlFree(content);
      return FALSE;
    }

  *out = v;
  xmlFree(content);
  return TRUE;
}

static gboolean
stcs_process_transport_parse_direction(const gchar *mode,
				       STCSProcessDirection *out,
				       GError **error)
{
  if (mode == NULL || *mode == '\0' || g_strcmp0(mode, "read") == 0 || g_strcmp0(mode, "r") == 0)
    {
      *out = STCS_PROCESS_DIRECTION_READ;
      return TRUE;
    }

  if (g_strcmp0(mode, "write") == 0 || g_strcmp0(mode, "w") == 0)
    {
      *out = STCS_PROCESS_DIRECTION_WRITE;
      return TRUE;
    }

  g_set_error(error,
	      G_IO_ERROR,
	      G_IO_ERROR_INVALID_DATA,
	      "unsupported mode `%s' (use read or write)",
	      mode);
  return FALSE;
}

static gboolean
stcs_process_transport_resolve_user(const gchar *user, STCSProcessCreds *creds, GError **error)
{
  struct passwd pwd;
  struct passwd *result = NULL;
  char *buf;
  long bufsize;
  int rc;

  bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufsize < 0)
    bufsize = 16384;
  buf = g_malloc((gsize) bufsize);

  rc = getpwnam_r(user, &pwd, buf, (size_t) bufsize, &result);
  if (rc != 0 || result == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_FOUND,
		  "user `%s' not found", user);
      g_free(buf);
      return FALSE;
    }

  creds->uid = pwd.pw_uid;
  creds->gid = pwd.pw_gid;
  creds->user = NULL; /* filled by caller with stable pointer */
  g_free(buf);
  return TRUE;
}

static void
stcs_process_transport_child_setup(gpointer user_data)
{
  STCSProcessCreds *creds = user_data;

  if (creds->user != NULL)
    (void) initgroups(creds->user, creds->gid);

  if (setgid(creds->gid) != 0)
    _exit(126);

  if (setuid(creds->uid) != 0)
    _exit(126);
}

static void
stcs_process_transport_close_fd(gint *fd)
{
  if (fd != NULL && *fd >= 0)
    {
      close(*fd);
      *fd = -1;
    }
}

static gboolean
stcs_process_transport_run(STCSProcessTransport *self,
			   GBytes *stdin_bytes,
			   GByteArray *stdout_out,
			   GError **error)
{
  gint stdin_fd = -1;
  gint stdout_fd = -1;
  gint stderr_fd = -1;
  GPid pid = 0;
  GSpawnFlags flags;
  gint64 deadline;
  gint64 spawn_start;
  GString *stderr_buf = NULL;
  gboolean child_exited = FALSE;
  gint wait_status = 0;
  gboolean timed_out = FALSE;
  gboolean r = FALSE;
  const guint8 *stdin_data = NULL;
  gsize stdin_len = 0;
  gsize stdin_off = 0;

  g_return_val_if_fail(self->argv != NULL && self->argv[0] != NULL, FALSE);
  g_return_val_if_fail(self->creds_resolved, FALSE);

  flags = G_SPAWN_DO_NOT_REAP_CHILD;
  if (strchr(self->argv[0], '/') == NULL)
    flags |= G_SPAWN_SEARCH_PATH;

  LOGD("spawn argv0=`%s' user=`%s' uid=%u gid=%u timeout_ms=%u workdir=`%s'",
       self->argv[0],
       self->user,
       (unsigned) self->creds.uid,
       (unsigned) self->creds.gid,
       self->timeout_ms,
       self->workdir ? self->workdir : "(inherit)");

  if (!g_spawn_async_with_pipes(self->workdir,
				self->argv,
				NULL,
				flags,
				stcs_process_transport_child_setup,
				&self->creds,
				&pid,
				&stdin_fd,
				&stdout_fd,
				&stderr_fd,
				error))
    return FALSE;

  LOGD("spawned pid=%d", (int) pid);

  if (stdin_bytes != NULL)
    stdin_data = g_bytes_get_data(stdin_bytes, &stdin_len);
  else
    stcs_process_transport_close_fd(&stdin_fd);

  {
    gint fl;
    if (stdin_fd >= 0 && (fl = fcntl(stdin_fd, F_GETFL)) >= 0)
      fcntl(stdin_fd, F_SETFL, fl | O_NONBLOCK);
    if (stdout_fd >= 0 && (fl = fcntl(stdout_fd, F_GETFL)) >= 0)
      fcntl(stdout_fd, F_SETFL, fl | O_NONBLOCK);
    if (stderr_fd >= 0 && (fl = fcntl(stderr_fd, F_GETFL)) >= 0)
      fcntl(stderr_fd, F_SETFL, fl | O_NONBLOCK);
  }

  stderr_buf = g_string_new(NULL);
  spawn_start = g_get_monotonic_time();
  deadline = spawn_start + ((gint64) self->timeout_ms * 1000);
  /* Cap each poll so we re-check waitpid often. A long poll(NULL,0,deadline)
   * after both pipes hit EOF (child closed stdio but not yet _exit) would
   * otherwise block until timeout-ms even though the child is nearly done. */
#define STCS_PROCESS_POLL_SLICE_MS 200

  while (!child_exited || stdout_fd >= 0 || stderr_fd >= 0 || stdin_fd >= 0)
    {
      struct pollfd pfds[3];
      nfds_t nfds = 0;
      gint timeout;
      gint64 remaining;
      int pr;
      pid_t wr;
      gint stdin_idx = -1;

      remaining = deadline - g_get_monotonic_time();
      if (remaining <= 0)
	{
	  timed_out = TRUE;
	  break;
	}

      /* Reap before poll so we never sleep a full slice after the child is gone. */
      if (!child_exited)
	{
	  wr = waitpid(pid, &wait_status, WNOHANG);
	  if (wr == pid)
	    {
	      child_exited = TRUE;
	      LOGD("child pid=%d exited status=0x%x after %.3fs",
		   (int) pid,
		   wait_status,
		   (g_get_monotonic_time() - spawn_start) / 1e6);
	      stcs_process_transport_close_fd(&stdin_fd);
	    }
	  else if (wr < 0 && errno != EINTR)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  g_io_error_from_errno(errno),
			  "waitpid failed: %s",
			  g_strerror(errno));
	      kill(pid, SIGKILL);
	      waitpid(pid, NULL, 0);
	      goto out_fds;
	    }
	}

      timeout = (gint) (remaining / 1000);
      if (timeout > STCS_PROCESS_POLL_SLICE_MS)
	timeout = STCS_PROCESS_POLL_SLICE_MS;
      if (timeout < 1)
	timeout = 1;

      if (stdin_fd >= 0 && stdin_off < stdin_len)
	{
	  stdin_idx = (gint) nfds;
	  pfds[nfds].fd = stdin_fd;
	  pfds[nfds].events = POLLOUT;
	  nfds++;
	}
      else if (stdin_fd >= 0 && stdin_off >= stdin_len)
	{
	  stcs_process_transport_close_fd(&stdin_fd);
	}

      if (stdout_fd >= 0)
	{
	  pfds[nfds].fd = stdout_fd;
	  pfds[nfds].events = POLLIN;
	  nfds++;
	}
      if (stderr_fd >= 0)
	{
	  pfds[nfds].fd = stderr_fd;
	  pfds[nfds].events = POLLIN;
	  nfds++;
	}

      /* nfds==0: child still running (or zombies pending) with pipes already
       * drained — sleep only one slice, then waitpid again. */
      pr = poll(nfds > 0 ? pfds : NULL, nfds, timeout);
      if (pr < 0)
	{
	  if (errno == EINTR)
	    continue;
	  g_set_error(error,
		      G_IO_ERROR,
		      g_io_error_from_errno(errno),
		      "poll failed: %s",
		      g_strerror(errno));
	  kill(pid, SIGKILL);
	  waitpid(pid, NULL, 0);
	  goto out_fds;
	}

      if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLOUT | POLLERR | POLLHUP)))
	{
	  if (pfds[stdin_idx].revents & (POLLERR | POLLHUP))
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_BROKEN_PIPE,
			  "child closed stdin early");
	      kill(pid, SIGKILL);
	      waitpid(pid, NULL, 0);
	      goto out_fds;
	    }

	  while (stdin_off < stdin_len)
	    {
	      ssize_t n = write(stdin_fd, stdin_data + stdin_off, stdin_len - stdin_off);
	      if (n < 0)
		{
		  if (errno == EINTR)
		    continue;
		  if (errno == EAGAIN || errno == EWOULDBLOCK)
		    break;
		  g_set_error(error,
			      G_IO_ERROR,
			      g_io_error_from_errno(errno),
			      "write to child stdin failed: %s",
			      g_strerror(errno));
		  kill(pid, SIGKILL);
		  waitpid(pid, NULL, 0);
		  goto out_fds;
		}
	      if (n == 0)
		break;
	      stdin_off += (gsize) n;
	    }

	  if (stdin_off >= stdin_len)
	    stcs_process_transport_close_fd(&stdin_fd);
	}

      {
	guint i;
	for (i = 0; i < nfds; i++)
	  {
	    gchar buf[4096];
	    ssize_t n;

	    if (i == (guint) stdin_idx)
	      continue;
	    if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
	      continue;

	    do
	      {
		n = read(pfds[i].fd, buf, sizeof(buf));
		if (n > 0)
		  {
		    if (pfds[i].fd == stdout_fd && stdout_out != NULL)
		      g_byte_array_append(stdout_out, (const guint8 *) buf, (guint) n);
		    else if (pfds[i].fd == stderr_fd)
		      g_string_append_len(stderr_buf, buf, n);
		  }
	      }
	    while (n > 0);

	    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
	      {
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		  LOGW("read failed on fd=%d: %s", pfds[i].fd, g_strerror(errno));
		if (pfds[i].fd == stdout_fd)
		  stcs_process_transport_close_fd(&stdout_fd);
		else if (pfds[i].fd == stderr_fd)
		  stcs_process_transport_close_fd(&stderr_fd);
	      }
	  }
      }

      if (!child_exited)
	{
	  wr = waitpid(pid, &wait_status, WNOHANG);
	  if (wr == pid)
	    {
	      child_exited = TRUE;
	      LOGD("child pid=%d exited status=0x%x after %.3fs",
		   (int) pid,
		   wait_status,
		   (g_get_monotonic_time() - spawn_start) / 1e6);
	      stcs_process_transport_close_fd(&stdin_fd);
	    }
	  else if (wr < 0 && errno != EINTR)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  g_io_error_from_errno(errno),
			  "waitpid failed: %s",
			  g_strerror(errno));
	      kill(pid, SIGKILL);
	      waitpid(pid, NULL, 0);
	      goto out_fds;
	    }
	}
    }
#undef STCS_PROCESS_POLL_SLICE_MS

  if (timed_out)
    {
      LOGW("timeout after %u ms, killing pid=%d", self->timeout_ms, (int) pid);
      kill(pid, SIGKILL);
      waitpid(pid, &wait_status, 0);
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_TIMED_OUT,
		  "process `%s' timed out after %u ms",
		  self->argv[0], self->timeout_ms);
      goto out_fds;
    }

  if (!child_exited)
    {
      pid_t wr2 = waitpid(pid, &wait_status, 0);
      if (wr2 == pid)
	child_exited = TRUE;
    }

  if (stderr_buf->len > 0)
    {
      g_strchomp(stderr_buf->str);
      LOGW("process `%s' stderr: %s", self->argv[0], stderr_buf->str);
    }

  if (WIFSIGNALED(wait_status))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "process `%s' killed by signal %d",
		  self->argv[0], WTERMSIG(wait_status));
      goto out_fds;
    }

  if (!WIFEXITED(wait_status))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "process `%s' ended abnormally",
		  self->argv[0]);
      goto out_fds;
    }

  if (WEXITSTATUS(wait_status) != 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "process `%s' exited with status %d",
		  self->argv[0], WEXITSTATUS(wait_status));
      goto out_fds;
    }

  LOGD("process `%s' ok stdout_bytes=%u",
       self->argv[0],
       stdout_out != NULL ? stdout_out->len : 0);
  r = TRUE;

out_fds:
  stcs_process_transport_close_fd(&stdin_fd);
  stcs_process_transport_close_fd(&stdout_fd);
  stcs_process_transport_close_fd(&stderr_fd);
  if (stderr_buf != NULL)
    g_string_free(stderr_buf, TRUE);
  return r;
}

static void
stcs_process_transport_run_read(STCSProcessTransport *self)
{
  GByteArray *stdout_buf;
  GBytes *bytes;
  GError *error = NULL;

  stdout_buf = g_byte_array_new();
  if (!stcs_process_transport_run(self, NULL, stdout_buf, &error))
    {
      LOGW("read run failed: %s", error ? error->message : "unknown");
      g_byte_array_unref(stdout_buf);
      stcs_transport_emit_data_error(STCS_TRANSPORT(self), error);
      g_clear_error(&error);
      return;
    }

  bytes = g_byte_array_free_to_bytes(stdout_buf);
  LOGD("emit data-ready (%" G_GSIZE_FORMAT " bytes)", g_bytes_get_size(bytes));
  stcs_transport_emit_data_ready(STCS_TRANSPORT(self), bytes);
  g_bytes_unref(bytes);
}

static void
stcs_process_transport_stop_poll(STCSProcessTransport *self)
{
  if (self->poll_source_id != 0)
    {
      g_source_remove(self->poll_source_id);
      self->poll_source_id = 0;
    }
}

static gboolean
stcs_process_transport_poll_cb(gpointer user_data)
{
  STCSProcessTransport *self = STCS_PROCESS_TRANSPORT(user_data);

  self->poll_source_id = 0;
  LOGD("poll timeout fired cron=`%s'",
       (self->cron != NULL && stcs_cron_is_parsed(self->cron))
	 ? stcs_cron_get_expression(self->cron)
	 : "(none)");
  stcs_process_transport_run_read(self);
  stcs_process_transport_schedule_next(self);
  return G_SOURCE_REMOVE;
}

static void
stcs_process_transport_schedule_next(STCSProcessTransport *self)
{
  if (self->cron == NULL || !stcs_cron_is_parsed(self->cron))
    return;

  stcs_process_transport_stop_poll(self);
  self->poll_source_id = stcs_cron_timeout_add(self->cron,
					       stcs_process_transport_poll_cb,
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
stcs_process_transport_configure(STCSTransport *transport, xmlNodePtr node, GError **error)
{
  STCSProcessTransport *self = STCS_PROCESS_TRANSPORT(transport);
  xmlNodePtr child;
  gchar *command = NULL;
  GPtrArray *args;
  gchar *mode_str = NULL;
  gchar *user = NULL;
  gchar *workdir = NULL;
  gchar *poll_cron = NULL;
  guint64 timeout_ms = 0;
  gboolean saw_timeout = FALSE;
  STCSProcessDirection direction = STCS_PROCESS_DIRECTION_READ;
  STCSProcessCreds creds;
  gboolean r = FALSE;

  args = g_ptr_array_new_with_free_func(g_free);
  memset(&creds, 0, sizeof(creds));

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "command"))
	{
	  if (!stcs_process_transport_parse_string(child, &command, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "arg"))
	{
	  gchar *arg = NULL;
	  if (!stcs_process_transport_parse_string(child, &arg, error))
	    goto out;
	  g_ptr_array_add(args, arg);
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "mode"))
	{
	  if (!stcs_process_transport_parse_string(child, &mode_str, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "user"))
	{
	  if (!stcs_process_transport_parse_string(child, &user, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "timeout-ms"))
	{
	  if (!stcs_process_transport_parse_uint64(child, &timeout_ms, error))
	    goto out;
	  saw_timeout = TRUE;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "workdir"))
	{
	  if (!stcs_process_transport_parse_string(child, &workdir, error))
	    goto out;
	}
      else if (xmlStrEqual(child->name, (const xmlChar *) "poll-cron"))
	{
	  if (!stcs_process_transport_parse_string(child, &poll_cron, error))
	    goto out;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside process-transport config",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (command == NULL || *command == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "process-transport requires a <command> element");
      goto out;
    }

  if (user == NULL || *user == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "process-transport requires a <user> element");
      goto out;
    }

  if (!saw_timeout || timeout_ms == 0)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "process-transport requires <timeout-ms> > 0");
      goto out;
    }

  if (!stcs_process_transport_parse_direction(mode_str, &direction, error))
    goto out;

  if (direction == STCS_PROCESS_DIRECTION_WRITE && poll_cron != NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "process-transport mode=write cannot use <poll-cron>");
      goto out;
    }

  if (!stcs_process_transport_resolve_user(user, &creds, error))
    goto out;

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

  stcs_process_transport_stop_poll(self);
  g_strfreev(self->argv);
  g_free(self->user);
  g_free(self->workdir);

  {
    GPtrArray *argv = g_ptr_array_new();
    guint i;

    g_ptr_array_add(argv, g_strdup(command));
    for (i = 0; i < args->len; i++)
      g_ptr_array_add(argv, g_strdup((const gchar *) g_ptr_array_index(args, i)));
    g_ptr_array_add(argv, NULL);
    self->argv = (gchar **) g_ptr_array_free(argv, FALSE);
  }

  self->user = g_strdup(user);
  self->creds = creds;
  self->creds.user = self->user;
  self->creds_resolved = TRUE;
  self->direction = direction;
  self->timeout_ms = (guint) MIN(timeout_ms, (guint64) G_MAXUINT);
  self->workdir = workdir;
  workdir = NULL;

  r = TRUE;

  if (direction == STCS_PROCESS_DIRECTION_READ)
    {
      if (self->cron != NULL && stcs_cron_is_parsed(self->cron))
	LOGI("process-transport read command=`%s' user=`%s' poll-cron=`%s' timeout-ms=%u",
	     self->argv[0], self->user, stcs_cron_get_expression(self->cron), self->timeout_ms);
      else
	LOGI("process-transport read command=`%s' user=`%s' timeout-ms=%u",
	     self->argv[0], self->user, self->timeout_ms);

      stcs_process_transport_run_read(self);
      stcs_process_transport_schedule_next(self);
    }
  else
    {
      LOGI("process-transport write command=`%s' user=`%s' timeout-ms=%u",
	   self->argv[0], self->user, self->timeout_ms);
    }

out:
  g_free(command);
  g_free(mode_str);
  g_free(user);
  g_free(workdir);
  g_free(poll_cron);
  g_ptr_array_free(args, TRUE);
  return r;
}

static void
stcs_process_transport_start_write(STCSTransport *transport,
				   guint64 op_id,
				   GBytes *data)
{
  STCSProcessTransport *self = STCS_PROCESS_TRANSPORT(transport);
  GError *error = NULL;

  if (self->direction != STCS_PROCESS_DIRECTION_WRITE)
    {
      g_set_error(&error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_SUPPORTED,
		  "process-transport command `%s' is configured for read only",
		  self->argv != NULL && self->argv[0] != NULL ? self->argv[0] : "(unset)");
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  if (!stcs_process_transport_run(self, data, NULL, &error))
    {
      stcs_transport_finish_write_error(transport, op_id, error);
      g_clear_error(&error);
      return;
    }

  stcs_transport_finish_write(transport, op_id, g_bytes_get_size(data));
}

static void
stcs_process_transport_dispose(GObject *object)
{
  STCSProcessTransport *self = STCS_PROCESS_TRANSPORT(object);

  stcs_process_transport_stop_poll(self);
  g_clear_object(&self->cron);

  G_OBJECT_CLASS(stcs_process_transport_parent_class)->dispose(object);
}

static void
stcs_process_transport_finalize(GObject *object)
{
  STCSProcessTransport *self = STCS_PROCESS_TRANSPORT(object);

  g_strfreev(self->argv);
  g_free(self->user);
  g_free(self->workdir);

  G_OBJECT_CLASS(stcs_process_transport_parent_class)->finalize(object);
}

static void
stcs_process_transport_class_init(STCSProcessTransportClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSTransportClass *transport_class = STCS_TRANSPORT_CLASS(klass);

  object_class->dispose = stcs_process_transport_dispose;
  object_class->finalize = stcs_process_transport_finalize;

  transport_class->configure = stcs_process_transport_configure;
  transport_class->start_write = stcs_process_transport_start_write;
}

static void
stcs_process_transport_init(STCSProcessTransport *self)
{
  self->argv = NULL;
  self->user = NULL;
  self->creds_resolved = FALSE;
  self->direction = STCS_PROCESS_DIRECTION_READ;
  self->timeout_ms = 0;
  self->workdir = NULL;
  self->cron = NULL;
  self->poll_source_id = 0;
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("process-transport", STCS_PLUGIN_CATEGORY_TRANSPORT, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_PROCESS_TRANSPORT, NULL);
}
