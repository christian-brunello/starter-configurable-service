/*
 * starter-configurable-service - internals.c
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
#include "plugin.h"
#include "plugin-entry.h"

#include <string.h>
#include <gmodule.h>
#include <gio/gio.h>

#include <starter/output.h>

#define LOGD(msg...) g_log("plugin", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("plugin", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("plugin", G_LOG_LEVEL_WARNING, msg)
#define LOGE(msg...) g_log("plugin", G_LOG_LEVEL_ERROR, msg)

void
stcs_binding_free(STCSBinding *binding)
{
  if (binding == NULL)
    return;

  g_free(binding->handler_data);
  binding->handler_data = NULL;
  g_free(binding->datapoint_id);
  g_free(binding->peer_name);
  g_free(binding);
}

static void
stcs_binding_free_gdestroy(gpointer p)
{
  stcs_binding_free(p);
}

static void
stcs_output_runtime_free_gdestroy(gpointer p)
{
  stcs_output_runtime_free(p);
}

void
stcs_output_runtime_clear_override_timer(STCSOutputRuntime *rt)
{
  if (rt == NULL)
    return;

  if (rt->override_timer_id != 0)
    {
      g_source_remove(rt->override_timer_id);
      rt->override_timer_id = 0;
    }
}

void
stcs_output_runtime_free(STCSOutputRuntime *rt)
{
  if (rt == NULL)
    return;

  stcs_output_runtime_clear_override_timer(rt);
  g_free(rt->output_name);
  g_free(rt->command_datapoint_id);
  g_free(rt);
}

STCSOutputRuntime *
stcs_output_runtime_new(STCSContext *context,
			STOutput *output,
			guint64 override_timeout_ms)
{
  STCSOutputRuntime *rt;

  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(output != NULL, NULL);

  rt = g_new0(STCSOutputRuntime, 1);
  rt->context = context;
  rt->output = output;
  rt->output_name = g_strdup(st_output_get_name(output));
  rt->override_timeout_ms = override_timeout_ms;
  rt->write_state = STCS_OUTPUT_WRITE_IDLE;
  return rt;
}

STCSOutputRuntime *
stcs_context_lookup_output_runtime(STCSContext *context, const gchar *output_name)
{
  g_return_val_if_fail(context != NULL, NULL);
  g_return_val_if_fail(output_name != NULL, NULL);

  return g_hash_table_lookup(context->output_runtimes, output_name);
}

void
stcs_output_runtime_sync_begin(STCSOutputRuntime *rt)
{
  g_return_if_fail(rt != NULL);
  rt->syncing++;
}

void
stcs_output_runtime_sync_end(STCSOutputRuntime *rt)
{
  g_return_if_fail(rt != NULL);
  if (rt->syncing > 0)
    rt->syncing--;
}

void
stcs_output_runtime_note_write_pending(STCSOutputRuntime *rt)
{
  g_return_if_fail(rt != NULL);
  rt->write_state = STCS_OUTPUT_WRITE_PENDING;
  stcs_output_runtime_arm_settle(rt);
}

void
stcs_output_runtime_note_write_ack(STCSOutputRuntime *rt, gboolean success)
{
  g_return_if_fail(rt != NULL);

  if (success)
    rt->write_state = STCS_OUTPUT_WRITE_IDLE;
  else
    rt->write_state = STCS_OUTPUT_WRITE_FAILED;
}

void
stcs_output_runtime_arm_settle(STCSOutputRuntime *rt)
{
  g_return_if_fail(rt != NULL);

  if (rt->settle_delay_ms == 0)
    {
      rt->feedback_ignore_until_us = 0;
      return;
    }

  rt->feedback_ignore_until_us =
    g_get_monotonic_time() + (gint64) rt->settle_delay_ms * 1000;
}

void stcs_internals_context_initialize(STCSContext *context)
{
  memset(context, 0x00, sizeof(STCSContext));

  context->options.service_version.major = 0;
  context->options.service_version.minor = 0;
  context->options.service_version.micro = 0;
  context->plugins = g_hash_table_new_full(g_str_hash,
					   g_str_equal,
					   g_free,
					   g_object_unref);
  context->transports = g_hash_table_new_full(g_str_hash,
					      g_str_equal,
					      g_free,
					      g_object_unref);
  context->datapoints = g_hash_table_new_full(g_str_hash,
					      g_str_equal,
					      g_free,
					      g_object_unref);
  context->readers = g_hash_table_new_full(g_str_hash,
					   g_str_equal,
					   g_free,
					   g_object_unref);
  context->writers = g_hash_table_new_full(g_str_hash,
					   g_str_equal,
					   g_free,
					   g_object_unref);
  context->inputs = g_ptr_array_new_with_free_func(g_object_unref);
  context->outputs = g_ptr_array_new_with_free_func(g_object_unref);
  context->bindings = g_ptr_array_new_with_free_func(stcs_binding_free_gdestroy);
  context->output_runtimes = g_hash_table_new_full(g_str_hash,
						   g_str_equal,
						   g_free,
						   stcs_output_runtime_free_gdestroy);
}

void stcs_internals_context_finalize(STCSContext *context)
{
  g_free(context->options.config_file);
  g_free(context->options.config_preprocessor);
  g_free(context->options.plugin_dir);
  g_free(context->options.service_name);

  g_clear_object(&context->mdns);
  g_clear_object(&context->server);
  g_hash_table_unref(context->output_runtimes);
  g_ptr_array_unref(context->bindings);
  g_ptr_array_unref(context->outputs);
  g_ptr_array_unref(context->inputs);
  g_hash_table_unref(context->writers);
  g_hash_table_unref(context->readers);
  g_hash_table_unref(context->datapoints);
  g_hash_table_unref(context->transports);
  g_hash_table_unref(context->plugins);
  g_ptr_array_unref(context->runlevel.info);
}

gboolean stcs_internals_scan_plugins(STCSContext *context, GError **error)
{
  gboolean r = FALSE;
  GDir *dir = NULL;
  const char *filename = NULL;
  GModule *module = NULL;

  LOGD("scan plugins in directory %s", context->options.plugin_dir);

  if((dir = g_dir_open (context->options.plugin_dir, 0, error)) == NULL)
    goto out;

  while ((filename = g_dir_read_name (dir)) != NULL)
    {
      STCSPluginGetInfoFunc *func = NULL;
      gchar *full_path;

      if (!g_str_has_suffix (filename, "." G_MODULE_SUFFIX))
	{
	  LOGD("skipping `%s'", filename);
	  continue;
	}

      LOGD("managing `%s'", filename);

      full_path = g_build_filename (context->options.plugin_dir, filename, NULL);

      if((module = g_module_open (full_path, G_MODULE_BIND_LOCAL)) == NULL)
	{
	  g_set_error(error, G_IO_ERROR,
		      G_IO_ERROR_FAILED,
		      "unable to open module %s", full_path);

	  g_free(full_path);
	  goto out;
	}

      if(g_module_symbol(module, "stcs_plugin_get_info", (gpointer *) &func) && func != NULL)
	{
	  STCSPluginInfo *info = func ();

	  if (info != NULL)
	    {
	      const gchar *name = stcs_plugin_info_get_name(info);
	      STCSPluginEntry *entry;

	      if (name == NULL || *name == '\0')
		{
		  g_set_error(error,
			      G_IO_ERROR,
			      G_IO_ERROR_INVALID_DATA,
			      "plugin at %s has empty name", full_path);
		  g_object_unref(info);
		  g_module_close(module);
		  g_free(full_path);
		  goto out;
		}

	      if (stcs_plugin_entry_lookup(context->plugins, name) != NULL)
		{
		  g_set_error(error,
			      G_IO_ERROR,
			      G_IO_ERROR_EXISTS,
			      "duplicate plugin name `%s' (module %s)",
			      name, full_path);
		  g_object_unref(info);
		  g_module_close(module);
		  g_free(full_path);
		  goto out;
		}

	      entry = stcs_plugin_entry_new(full_path, info);
	      g_object_unref(info);

	      LOGI("found plugin %s: Name: %s, Category: %d (%s), Version: %s",
		   stcs_plugin_entry_get_path(entry),
		   stcs_plugin_info_get_name(stcs_plugin_entry_get_info(entry)),
		   stcs_plugin_info_get_category(stcs_plugin_entry_get_info(entry)),
		   stcs_plugin_info_category_to_string(stcs_plugin_info_get_category(stcs_plugin_entry_get_info(entry))),
		   stcs_plugin_info_get_version(stcs_plugin_entry_get_info(entry)));

	      g_hash_table_insert(context->plugins,
				  g_strdup(name),
				  entry);
	    }
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_FAILED,
		      "unable to lookup symbol `stcs_plugin_get_info' from module at %s", full_path);
	  g_module_close(module);
	  g_free(full_path);
	  goto out;
	}

      g_module_close(module);
      g_free (full_path);
    }

  r = TRUE;

  LOGD("done scanning plugins in directory %s. Registered: %u",
       context->options.plugin_dir,
       g_hash_table_size(context->plugins));

out:

  if(dir)
    g_dir_close (dir);

  return r;
}
