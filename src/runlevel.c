/*
 * starter-configurable-service - runlevel.c
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

#include "internals.h"
#include "starter/configurable-service/datapoint.h"
#include "starter/configurable-service/reader.h"
#include "starter/configurable-service/writer.h"

#include <starter/input.h>
#include <starter/output.h>
#include <starter/server.h>
#include <starter/mdns.h>
#include <starter/defs.h>

#define LOGD(msg...) g_log("runlevel", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("runlevel", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("runlevel", G_LOG_LEVEL_WARNING, msg)
#define LOGE(msg...) g_log("runlevel", G_LOG_LEVEL_ERROR, msg)

typedef struct {
  STCSContext *context;
  GPtrArray *runtimes; /* STCSOutputRuntime* not owned */
} STCSReaderSyncClosure;

int stcs_internals_runlevel_up(STCSContext *context, GError **error)
{
  int r = -1;
  int target;

  target = context->runlevel.current + 1;

  LOGD("try go to level %u", target);

  if(target >= 0 && target < (int) context->runlevel.info->len)
    {
      STCSRunlevel *info;

      info = context->runlevel.info->pdata[target];

      LOGD("Using info from STCSContext %p", info);
      LOGD("Begin setup runlevel %s", info->name);

      if(info->enter)
	r = (*info->enter)(context, info->custom_data, error);
      else
	r = 0;

      if(r == 0)
	context->runlevel.current = target;
      else
	{
	  if(info->exit)
	    (*info->exit)(context, info->custom_data, NULL);
	}
    }
  else
    g_set_error(error,
		G_IO_ERROR,
		G_IO_ERROR_NOT_FOUND,
		"runlevel does not exists: %u",
		target);

  LOGD("going to level %u result: %d", target, r);

  return r;
}

int stcs_internals_runlevel_down(STCSContext *context, GError **error)
{
  int r = -1;
  int current;

  current = context->runlevel.current;

  LOGD("try leave level %u", current);

  if(current >= 0 && current < (int) context->runlevel.info->len)
    {
      STCSRunlevel *info;

      info = context->runlevel.info->pdata[current];

      LOGD("Using info from STCSContext %p", info);
      LOGD("Begin shutdown runlevel %s", info->name);

      if(info->exit)
	(*info->exit)(context, info->custom_data, error);

      context->runlevel.current = current - 1;
      r = 0;
    }
  else
    g_set_error(error,
		G_IO_ERROR,
		G_IO_ERROR_NOT_FOUND,
		"runlevel does not exists: %u",
		current);

  LOGD("left level %u result: %d (now %d)", current, r, context->runlevel.current);

  return r;
}

static int runlevel_0_enter(STCSContext *context, gpointer user_data, GError **error)
{
  (void) user_data;

  return stcs_internals_scan_plugins(context, error) ? 0 : -1;
}

static void runlevel_0_exit(STCSContext *context, gpointer user_data, GError **error)
{
  (void) user_data;
  (void) error;
  g_hash_table_remove_all(context->plugins);
}

static int runlevel_1_enter(STCSContext *context, gpointer user_data, GError **error)
{
  gboolean r = FALSE;

  (void) user_data;

  if((r = stcs_internals_config_parse(context, context->options.config_file, error)) == TRUE)
    stcs_internals_log_init(context->options.service_name);

  return r ? 0 : -1;
}

static void runlevel_1_exit(STCSContext *context, gpointer user_data, GError **error)
{
  (void) user_data;
  (void) error;

  g_ptr_array_set_size(context->bindings, 0);
  g_hash_table_remove_all(context->output_runtimes);
  g_ptr_array_set_size(context->outputs, 0);
  g_ptr_array_set_size(context->inputs, 0);
  g_hash_table_remove_all(context->writers);
  g_hash_table_remove_all(context->readers);
  g_hash_table_remove_all(context->datapoints);
  g_hash_table_remove_all(context->transports);
}

static STInput *
runlevel_find_input(STCSContext *context, const gchar *name)
{
  guint i;

  for (i = 0; i < context->inputs->len; i++)
    {
      STInput *input = g_ptr_array_index(context->inputs, i);

      if (g_strcmp0(st_input_get_name(input), name) == 0)
	return input;
    }

  return NULL;
}

static STOutput *
runlevel_find_output(STCSContext *context, const gchar *name)
{
  guint i;

  for (i = 0; i < context->outputs->len; i++)
    {
      STOutput *output = g_ptr_array_index(context->outputs, i);

      if (g_strcmp0(st_output_get_name(output), name) == 0)
	return output;
    }

  return NULL;
}

static void
runlevel_on_datapoint_value_changed(STCSDatapoint *datapoint,
				    gdouble value,
				    gpointer user_data)
{
  STInput *input = ST_INPUT(user_data);
  GError *error = NULL;

  (void) datapoint;

  if (!st_input_set_val(input, value, &error))
    {
      LOGW("st_input_set_val(%s, %g) failed: %s",
	   st_input_get_name(input),
	   value,
	   error ? error->message : "unknown error");
      g_clear_error(&error);
    }
  else
    LOGD("input `%s' <- %g", st_input_get_name(input), value);
}

static gboolean
runlevel_clear_manual_override(gpointer user_data)
{
  STCSOutputRuntime *rt = user_data;
  GError *error = NULL;
  guint64 flags;

  rt->override_timer_id = 0;

  if (rt->output == NULL)
    return G_SOURCE_REMOVE;

  flags = st_output_get_flags(rt->output);
  if ((flags & ST_IO_FLAG_MANUAL_OVERRIDE) == 0)
    return G_SOURCE_REMOVE;

  if (!st_output_set_flags(rt->output, flags & ~ST_IO_FLAG_MANUAL_OVERRIDE, &error))
    {
      LOGW("failed clearing MANUAL_OVERRIDE on `%s': %s",
	   rt->output_name,
	   error ? error->message : "unknown error");
      g_clear_error(&error);
      return G_SOURCE_REMOVE;
    }

  LOGI("cleared MANUAL_OVERRIDE on output `%s' after timeout", rt->output_name);
  return G_SOURCE_REMOVE;
}

static void
runlevel_arm_override_timer(STCSOutputRuntime *rt)
{
  stcs_output_runtime_clear_override_timer(rt);

  if (rt->override_timeout_ms == 0)
    return;

  if (rt->override_timeout_ms > G_MAXUINT)
    {
      LOGW("override-timeout for `%s' clamped to G_MAXUINT ms", rt->output_name);
      rt->override_timer_id =
	g_timeout_add(G_MAXUINT, runlevel_clear_manual_override, rt);
    }
  else
    {
      rt->override_timer_id =
	g_timeout_add((guint) rt->override_timeout_ms,
		      runlevel_clear_manual_override,
		      rt);
    }
}

static void
runlevel_on_output_command_val_changed(STOutput *output, gpointer user_data)
{
  STCSOutputRuntime *rt = user_data;
  STCSDatapoint *datapoint;
  gdouble value;
  GError *error = NULL;

  if (rt->syncing > 0)
    {
      LOGD("output `%s' val-changed during read-sync — skip command",
	   st_output_get_name(output));
      return;
    }

  if (st_output_get_flags(output) & ST_IO_FLAG_MANUAL_OVERRIDE)
    {
      LOGD("output `%s' val-changed ignored (MANUAL_OVERRIDE)",
	   st_output_get_name(output));
      return;
    }

  if (rt->command_datapoint_id == NULL)
    return;

  datapoint = g_hash_table_lookup(rt->context->datapoints, rt->command_datapoint_id);
  if (datapoint == NULL)
    {
      LOGW("command datapoint `%s' missing for output `%s'",
	   rt->command_datapoint_id, rt->output_name);
      return;
    }

  value = st_output_get_val(output);
  stcs_output_runtime_note_write_pending(rt);

  if (!stcs_datapoint_push(datapoint, value, &error))
    {
      LOGW("datapoint_push(%s, %g) from output failed: %s",
	   stcs_datapoint_get_id(datapoint),
	   value,
	   error ? error->message : "unknown error");
      g_clear_error(&error);
      stcs_output_runtime_note_write_ack(rt, FALSE);
    }
  else
    LOGD("datapoint `%s' <- output %g (command, write pending)",
	 stcs_datapoint_get_id(datapoint), value);
}

static void
runlevel_on_feedback_datapoint_changed(STCSDatapoint *datapoint,
				       gdouble value,
				       gpointer user_data)
{
  STCSOutputRuntime *rt = user_data;
  STOutput *output = rt->output;
  gdouble current;
  GError *error = NULL;
  guint64 flags;

  (void) datapoint;

  if (output == NULL)
    return;

  if (rt->syncing == 0)
    {
      LOGW("feedback datapoint `%s' changed outside read-sync for output `%s' — ignored",
	   stcs_datapoint_get_id(datapoint), rt->output_name);
      return;
    }

  current = st_output_get_val(output);
  if (current == value)
    return;

  /* Before STServer (runlevel < 3): startup align, no MANUAL_OVERRIDE. */
  if (rt->context->runlevel.current < 3)
    {
      if (!st_output_set_val(output, value, &error))
	{
	  LOGW("startup feedback align `%s' <- %g failed: %s",
	       rt->output_name, value,
	       error ? error->message : "unknown error");
	  g_clear_error(&error);
	  return;
	}
      LOGD("output `%s' startup sync <- %g (runlevel=%d)",
	   rt->output_name, value, rt->context->runlevel.current);
      return;
    }

  /* After a command, ignore feedback until settle-delay elapses. */
  if (rt->settle_delay_ms > 0
      && rt->feedback_ignore_until_us > 0
      && g_get_monotonic_time() < rt->feedback_ignore_until_us)
    {
      LOGD("feedback mismatch on `%s' during settle (cmd=%g device=%g) — ignored",
	   rt->output_name, current, value);
      return;
    }

  if (rt->write_state == STCS_OUTPUT_WRITE_PENDING)
    {
      LOGD("feedback mismatch on `%s' while write pending (cmd=%g device=%g) — no override",
	   rt->output_name, current, value);
      return;
    }

  if (rt->write_state == STCS_OUTPUT_WRITE_FAILED)
    {
      if (!st_output_set_val(output, value, &error))
	{
	  LOGW("align after write-fail `%s' <- %g failed: %s",
	       rt->output_name, value,
	       error ? error->message : "unknown error");
	  g_clear_error(&error);
	  return;
	}
      stcs_output_runtime_note_write_ack(rt, TRUE); /* back to idle */
      LOGI("output `%s' aligned to device %g after write failure (no MANUAL_OVERRIDE)",
	   rt->output_name, value);
      return;
    }

  /* idle mismatch → external change */
  flags = st_output_get_flags(output);
  if ((flags & ST_IO_FLAG_MANUAL_OVERRIDE) == 0)
    {
      if (!st_output_set_flags(output, flags | ST_IO_FLAG_MANUAL_OVERRIDE, &error))
	{
	  LOGW("failed setting MANUAL_OVERRIDE on `%s': %s",
	       rt->output_name,
	       error ? error->message : "unknown error");
	  g_clear_error(&error);
	  return;
	}
      LOGI("MANUAL_OVERRIDE on `%s' (output=%g feedback=%g)",
	   rt->output_name, current, value);
    }

  if (!st_output_set_val(output, value, &error))
    {
      LOGW("failed aligning output `%s' to feedback %g: %s",
	   rt->output_name, value,
	   error ? error->message : "unknown error");
      g_clear_error(&error);
      return;
    }

  runlevel_arm_override_timer(rt);
}

static void
runlevel_reader_sync_hook(gboolean entering, gpointer user_data)
{
  STCSReaderSyncClosure *closure = user_data;
  guint i;

  for (i = 0; i < closure->runtimes->len; i++)
    {
      STCSOutputRuntime *rt = g_ptr_array_index(closure->runtimes, i);

      if (entering)
	stcs_output_runtime_sync_begin(rt);
      else
	stcs_output_runtime_sync_end(rt);
    }
}

static void
runlevel_writer_ack_hook(const gchar *datapoint_id,
			 gboolean success,
			 gpointer user_data)
{
  STCSContext *context = user_data;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, context->output_runtimes);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      STCSOutputRuntime *rt = value;

      if (g_strcmp0(rt->command_datapoint_id, datapoint_id) == 0)
	{
	  stcs_output_runtime_note_write_ack(rt, success);
	  LOGD("write ack output=`%s' datapoint=`%s' success=%s state=%d",
	       rt->output_name,
	       datapoint_id,
	       success ? "true" : "false",
	       (int) rt->write_state);
	  return;
	}
    }
}

static void
runlevel_2_disconnect_bindings(STCSContext *context)
{
  guint i;
  GHashTableIter iter;
  gpointer key, value;

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *binding = g_ptr_array_index(context->bindings, i);

      if (binding->handler_id == 0)
	continue;

      if (binding->kind == STCS_BINDING_KIND_INPUT
	  || (binding->kind == STCS_BINDING_KIND_OUTPUT
	      && binding->role == STCS_BINDING_ROLE_FEEDBACK))
	{
	  STCSDatapoint *datapoint =
	    g_hash_table_lookup(context->datapoints, binding->datapoint_id);

	  if (datapoint != NULL)
	    g_signal_handler_disconnect(datapoint, binding->handler_id);
	}
      else
	{
	  STOutput *output = runlevel_find_output(context, binding->peer_name);

	  if (output != NULL)
	    g_signal_handler_disconnect(output, binding->handler_id);
	}

      LOGD("disconnected bind datapoint=%s %s=%s handler=%lu",
	   binding->datapoint_id,
	   binding->kind == STCS_BINDING_KIND_INPUT ? "input" : "output",
	   binding->peer_name,
	   (unsigned long) binding->handler_id);
      binding->handler_id = 0;
    }

  g_hash_table_iter_init(&iter, context->readers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      STCSReader *reader = value;
      STCSReaderSyncClosure *closure;

      closure = g_object_get_data(G_OBJECT(reader), "stcs-sync-closure");
      stcs_reader_set_sync_hook(reader, NULL, NULL);
      if (closure != NULL)
	{
	  g_ptr_array_unref(closure->runtimes);
	  g_free(closure);
	  g_object_set_data(G_OBJECT(reader), "stcs-sync-closure", NULL);
	}
    }

  g_hash_table_iter_init(&iter, context->writers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    stcs_writer_set_ack_hook(STCS_WRITER(value), NULL, NULL);

  g_hash_table_iter_init(&iter, context->output_runtimes);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      STCSOutputRuntime *rt = value;

      stcs_output_runtime_clear_override_timer(rt);
      g_clear_pointer(&rt->command_datapoint_id, g_free);
      rt->syncing = 0;
      rt->write_state = STCS_OUTPUT_WRITE_IDLE;
      rt->settle_delay_ms = 0;
      rt->feedback_ignore_until_us = 0;
    }
}

typedef struct {
  const gchar *id;
  gboolean found;
} STCSReaderMapsDatapointCtx;

static void
runlevel_reader_maps_datapoint_cb(const gchar *key,
				  STCSDatapoint *dp,
				  gpointer user_data)
{
  STCSReaderMapsDatapointCtx *ctx = user_data;

  (void) key;
  if (g_strcmp0(stcs_datapoint_get_id(dp), ctx->id) == 0)
    ctx->found = TRUE;
}

static gboolean
runlevel_reader_maps_datapoint(STCSReader *reader, const gchar *datapoint_id)
{
  STCSReaderMapsDatapointCtx ctx = { datapoint_id, FALSE };

  stcs_reader_foreach_mapping(reader, runlevel_reader_maps_datapoint_cb, &ctx);
  return ctx.found;
}

static int runlevel_2_enter(STCSContext *context, gpointer user_data, GError **error)
{
  guint i;
  GHashTableIter iter;
  gpointer key, value;

  (void) user_data;

  LOGI("wiring I/O bindings (%u)", context->bindings->len);

  /* Resolve command datapoint ids onto output runtimes. */
  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *binding = g_ptr_array_index(context->bindings, i);
      STCSOutputRuntime *rt;

      if (binding->kind != STCS_BINDING_KIND_OUTPUT
	  || binding->role != STCS_BINDING_ROLE_COMMAND)
	continue;

      rt = stcs_context_lookup_output_runtime(context, binding->peer_name);
      if (rt == NULL)
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		      "missing runtime for output `%s'", binding->peer_name);
	  return -1;
	}

      g_free(rt->command_datapoint_id);
      rt->command_datapoint_id = g_strdup(binding->datapoint_id);
    }

  for (i = 0; i < context->bindings->len; i++)
    {
      STCSBinding *binding = g_ptr_array_index(context->bindings, i);
      STCSDatapoint *datapoint;

      datapoint = g_hash_table_lookup(context->datapoints, binding->datapoint_id);
      if (datapoint == NULL)
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_NOT_FOUND,
		      "bind references missing datapoint `%s'",
		      binding->datapoint_id);
	  runlevel_2_disconnect_bindings(context);
	  return -1;
	}

      if (binding->kind == STCS_BINDING_KIND_INPUT)
	{
	  STInput *input;
	  gdouble current = 0.0;

	  input = runlevel_find_input(context, binding->peer_name);
	  if (input == NULL)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_NOT_FOUND,
			  "bind references missing input `%s'",
			  binding->peer_name);
	      runlevel_2_disconnect_bindings(context);
	      return -1;
	    }

	  binding->handler_id =
	    g_signal_connect(datapoint, "value-changed",
			     G_CALLBACK(runlevel_on_datapoint_value_changed),
			     input);

	  LOGI("wired datapoint `%s' → input `%s' (handler=%lu)",
	       binding->datapoint_id,
	       binding->peer_name,
	       (unsigned long) binding->handler_id);

	  if (stcs_datapoint_get_value(datapoint, &current))
	    runlevel_on_datapoint_value_changed(datapoint, current, input);
	}
      else if (binding->role == STCS_BINDING_ROLE_FEEDBACK)
	{
	  STCSOutputRuntime *rt;

	  rt = stcs_context_lookup_output_runtime(context, binding->peer_name);
	  if (rt == NULL || rt->output == NULL)
	    {
	      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			  "bind feedback references missing output `%s'",
			  binding->peer_name);
	      runlevel_2_disconnect_bindings(context);
	      return -1;
	    }

	  rt->settle_delay_ms = binding->settle_delay_ms;
	  rt->feedback_ignore_until_us = 0;

	  binding->handler_id =
	    g_signal_connect(datapoint, "value-changed",
			     G_CALLBACK(runlevel_on_feedback_datapoint_changed),
			     rt);

	  LOGI("wired feedback datapoint `%s' → output `%s' settle_delay_ms=%" G_GUINT64_FORMAT " (handler=%lu)",
	       binding->datapoint_id,
	       binding->peer_name,
	       rt->settle_delay_ms,
	       (unsigned long) binding->handler_id);
	}
      else
	{
	  STCSOutputRuntime *rt;

	  rt = stcs_context_lookup_output_runtime(context, binding->peer_name);
	  if (rt == NULL || rt->output == NULL)
	    {
	      g_set_error(error,
			  G_IO_ERROR,
			  G_IO_ERROR_NOT_FOUND,
			  "bind references missing output `%s'",
			  binding->peer_name);
	      runlevel_2_disconnect_bindings(context);
	      return -1;
	    }

	  binding->handler_id =
	    g_signal_connect(rt->output, "val-changed",
			     G_CALLBACK(runlevel_on_output_command_val_changed),
			     rt);

	  LOGI("wired output `%s' → datapoint `%s' command (handler=%lu)",
	       binding->peer_name,
	       binding->datapoint_id,
	       (unsigned long) binding->handler_id);

	  runlevel_on_output_command_val_changed(rt->output, rt);
	}
    }

  /* Reader sync hooks: outputs whose feedback datapoint is mapped by the reader. */
  g_hash_table_iter_init(&iter, context->readers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      STCSReader *reader = value;
      STCSReaderSyncClosure *closure;
      guint j;

      closure = g_new0(STCSReaderSyncClosure, 1);
      closure->context = context;
      closure->runtimes = g_ptr_array_new();

      for (j = 0; j < context->bindings->len; j++)
	{
	  STCSBinding *binding = g_ptr_array_index(context->bindings, j);
	  STCSOutputRuntime *rt;

	  if (binding->kind != STCS_BINDING_KIND_OUTPUT
	      || binding->role != STCS_BINDING_ROLE_FEEDBACK)
	    continue;

	  if (!runlevel_reader_maps_datapoint(reader, binding->datapoint_id))
	    continue;

	  rt = stcs_context_lookup_output_runtime(context, binding->peer_name);
	  if (rt != NULL)
	    g_ptr_array_add(closure->runtimes, rt);
	}

      if (closure->runtimes->len == 0)
	{
	  g_ptr_array_unref(closure->runtimes);
	  g_free(closure);
	  continue;
	}

      g_object_set_data(G_OBJECT(reader), "stcs-sync-closure", closure);
      stcs_reader_set_sync_hook(reader, runlevel_reader_sync_hook, closure);
      LOGI("reader `%s': sync hook for %u feedback output(s)",
	   (const gchar *) key, closure->runtimes->len);
    }

  g_hash_table_iter_init(&iter, context->writers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    stcs_writer_set_ack_hook(STCS_WRITER(value), runlevel_writer_ack_hook, context);

  return 0;
}

static void runlevel_2_exit(STCSContext *context, gpointer user_data, GError **error)
{
  (void) user_data;
  (void) error;

  LOGI("unwiring I/O bindings");
  runlevel_2_disconnect_bindings(context);
}

static int runlevel_3_enter(STCSContext *context, gpointer user_data, GError **error)
{
  const gchar *name;
  STServer *server = NULL;

  (void) user_data;

  name = context->options.service_name;
  if (name == NULL || *name == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "service-name is required to start STServer");
      return -1;
    }

  if (context->server != NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "STServer is already running");
      return -1;
    }

  LOGI("creating STServer name=%s version=%u.%u.%u inputs=%u outputs=%u",
       name,
       context->options.service_version.major,
       context->options.service_version.minor,
       context->options.service_version.micro,
       context->inputs->len,
       context->outputs->len);

  server = st_server_new(name,
			 &context->options.service_version,
			 context->inputs,
			 context->outputs);
  if (server == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "st_server_new failed for `%s'", name);
      return -1;
    }

  if (!st_server_start(server, 0, error))
    {
      g_object_unref(server);
      return -1;
    }

  context->server = server;

  LOGI("STServer started on port %u", (unsigned) st_server_get_port(server));
  return 0;
}

static void runlevel_3_exit(STCSContext *context, gpointer user_data, GError **error)
{
  (void) user_data;
  (void) error;

  if (context->server == NULL)
    return;

  LOGI("stopping STServer (port %u)",
       (unsigned) st_server_get_port(context->server));
  g_clear_object(&context->server);
}

static int runlevel_4_enter(STCSContext *context, gpointer user_data, GError **error)
{
  const gchar *name;
  guint16 port;
  STMDNS *mdns = NULL;

  (void) user_data;

  if (context->server == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "STServer is required before publishing mDNS");
      return -1;
    }

  name = context->options.service_name;
  if (name == NULL || *name == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "service-name is required to publish mDNS");
      return -1;
    }

  if (context->mdns != NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_EXISTS,
		  "mDNS is already published");
      return -1;
    }

  port = st_server_get_port(context->server);

  LOGI("publishing mDNS name=%s type=_dbus._tcp port=%u",
       name, (unsigned) port);

  mdns = st_mdns_new_with_options(ST_MDNS_OPTION_NO_DISCOVERY);
  if (mdns == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "st_mdns_new_with_options failed");
      return -1;
    }

  if (!st_mdns_publish_service(mdns, name, "_dbus._tcp", port))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_FAILED,
		  "st_mdns_publish_service failed for `%s' on port %u",
		  name, (unsigned) port);
      g_object_unref(mdns);
      return -1;
    }

  context->mdns = mdns;
  LOGI("mDNS published for `%s' on port %u", name, (unsigned) port);
  return 0;
}

static void runlevel_4_exit(STCSContext *context, gpointer user_data, GError **error)
{
  (void) user_data;
  (void) error;

  if (context->mdns == NULL)
    return;

  LOGI("unpublishing mDNS");
  g_clear_object(&context->mdns);
}

static void stcs_runlevel_destroy(gpointer p)
{
  STCSRunlevel *self = p;

  g_free(self->name);
  g_free(self);
}

void stcs_internals_runlevel_initialize(STCSContext *context)
{
  STCSRunlevel *info;

  context->runlevel.info = g_ptr_array_new_full(0, stcs_runlevel_destroy);

  info = g_malloc0(sizeof(STCSRunlevel));
  info->name = g_strdup("0 - Plugins scanned");
  info->enter = runlevel_0_enter;
  info->exit = runlevel_0_exit;
  g_ptr_array_add(context->runlevel.info, info);

  info = g_malloc0(sizeof(STCSRunlevel));
  info->name = g_strdup("1 - Configuration parsed");
  info->enter = runlevel_1_enter;
  info->exit = runlevel_1_exit;
  g_ptr_array_add(context->runlevel.info, info);

  info = g_malloc0(sizeof(STCSRunlevel));
  info->name = g_strdup("2 - I/O bindings wired");
  info->enter = runlevel_2_enter;
  info->exit = runlevel_2_exit;
  g_ptr_array_add(context->runlevel.info, info);

  info = g_malloc0(sizeof(STCSRunlevel));
  info->name = g_strdup("3 - STServer started");
  info->enter = runlevel_3_enter;
  info->exit = runlevel_3_exit;
  g_ptr_array_add(context->runlevel.info, info);

  info = g_malloc0(sizeof(STCSRunlevel));
  info->name = g_strdup("4 - mDNS published");
  info->enter = runlevel_4_enter;
  info->exit = runlevel_4_exit;
  g_ptr_array_add(context->runlevel.info, info);

  context->runlevel.target = 4;
  context->runlevel.current = -1;
}
