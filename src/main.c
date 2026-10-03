/*
 * starter-configurable-service - main.c
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

#include <libxml/parser.h>

#include "internals.h"

#define LOGD(msg...) g_log("main", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log("main", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("main", G_LOG_LEVEL_WARNING, msg)
#define LOGE(msg...) g_log("main", G_LOG_LEVEL_ERROR, msg)

#define STCS_DEFAULT_PLUGIN_DIR  "/usr/local/lib/starter/configurable-service/plugins"

/* Definizione dei parametri da riga di comando */


static gboolean parse_cli (STCSContext *context, int *argc, char ***argv, GError ** error)
{
  GOptionEntry cli_entries[] = {
    { "config",     'c', 0, G_OPTION_ARG_FILENAME, &context->options.config_file,
      "Path to the service configuration file", "FILE" },
    { "config-preprocessor", 'P', 0, G_OPTION_ARG_STRING, &context->options.config_preprocessor,
      "Command to preprocess config file ({F} = path); empty disables", "CMD" },
    { "plugin-dir", 'p', 0, G_OPTION_ARG_FILENAME, &context->options.plugin_dir,
      "Path to the plugins directory", "DIR" },
    { "version",    'V', 0, G_OPTION_ARG_NONE,     &context->options.version,
      "Show software version and exit", NULL },
    { NULL }
  };

  GOptionContext *opt_context;
  gboolean r = FALSE;
  
  opt_context = g_option_context_new ("- Starter Configurable Service");
  g_option_context_add_main_entries (opt_context, cli_entries, NULL);

  r = g_option_context_parse (opt_context, argc, argv, error);

  g_option_context_free (opt_context);

  return r;
}

static void load_environment (STCSContext *context)
{
  if (context->options.plugin_dir == NULL)
    {
      const gchar *env_plugin = g_getenv ("STCS_PLUGIN_PATH");

      if (env_plugin && *env_plugin != '\0')
	{
	  context->options.plugin_dir = g_strdup (env_plugin);
	}
      else
	{
	  context->options.plugin_dir = g_strdup (STCS_DEFAULT_PLUGIN_DIR);
	}
    }

  if (context->options.config_preprocessor == NULL)
    {
      const gchar *env_prep = g_getenv ("STCS_CONFIG_PREPROCESSOR");

      if (env_prep != NULL)
	context->options.config_preprocessor = g_strdup (env_prep);
    }
}

static gboolean runlevel_tick_callback(gpointer user_data)
{
  STCSContext *context = user_data;
  GError *error = NULL;

  if(context->runlevel.target > context->runlevel.current)
    {
      int r;

      r = stcs_internals_runlevel_up(context, &error);

      if(r != 0)
	{
	  LOGE("error changing runlevel: %d %s", r, error ? error->message : "unknown error");
	  g_clear_error(&error);
	}
    }
  else if(context->runlevel.target < context->runlevel.current)
    {
      stcs_internals_runlevel_down(context, &error);

      if(error)
	{
	  LOGE("error changing runlevel: %s", error ? error->message : "unknown error");
	  g_clear_error(&error);
	}
    }
  else
    LOGD("runlevel already at target, nothing to do");

  return TRUE;
}

int main(int argc, char *argv[]) {
    GError *error = NULL;
    STCSContext context;

    LOGD("Booting new instance of starter-configurable-service...");

    stcs_internals_context_initialize(&context);

    stcs_internals_log_init(argv[0]);

    /* 1. Parsing argomenti riga di comando */
    if (!parse_cli(&context, &argc, &argv, &error)) 
      {
	LOGE("error parsing command line options: %s\n", error->message);
	g_clear_error(&error);
	exit(EXIT_FAILURE);
      }

    LOGD("command line options parsed successfully");

    if (context.options.version) 
      {
	g_print("Starter Configurable Service v0.1.0\n");
	exit(EXIT_SUCCESS);
      }

    /* 2. Caricamento opzioni da Environment (Fallback) */
    load_environment(&context);

    LOGD("environment load successfully");

    LOGD("configuration file: %s", context.options.config_file);
    LOGD("config preprocessor: %s",
	 context.options.config_preprocessor && *context.options.config_preprocessor
	 ? context.options.config_preprocessor : "(none)");
    LOGD("plugins directory: %s", context.options.plugin_dir);

    stcs_internals_runlevel_initialize(&context);

    /* 
    if(!stcs_internals_scan_plugins(&context, &error))
      {
	LOGE("error scanning plugins: %s", error ? error->message : "unknown error");
	exit(EXIT_FAILURE);
      }

    LOGI("Plugin scan completed. Registered elements: %u", g_hash_table_size(context.plugins));
*/
    /* 4. Inizializzazione LibXML2 */
    LIBXML_TEST_VERSION

    /* 5. Istanziazione Main Loop */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    /* TODO: 
     * - Inizializzare lo StcsPluginRegistry e scansionare options.plugin_dir
     * - Inizializzare il parser XML per leggere options.config_file e istanziare i componenti
     */

    g_timeout_add(5000, runlevel_tick_callback, &context);

    g_main_loop_run(loop);

    /* 6. Cleanup finale */
    xmlCleanupParser();
    g_main_loop_unref(loop);

    stcs_internals_context_finalize(&context);

    return 0;
}
