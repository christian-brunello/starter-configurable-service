/*
 * starter-configurable-service - internals.h
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

#ifndef STCS_INTERNALS_H_INCLUDED
#define STCS_INTERNALS_H_INCLUDED

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <starter/version.h>
#include <starter/server.h>
#include <starter/mdns.h>
#include <starter/output.h>

typedef struct _STCSRunlevel STCSRunlevel;
typedef struct _STCSContext STCSContext;
typedef struct _STCSBinding STCSBinding;
typedef struct _STCSOutputRuntime STCSOutputRuntime;

typedef int STCSRunlevelEnterFunc(STCSContext *, gpointer, GError **);
typedef void STCSRunlevelExitFunc(STCSContext *, gpointer, GError **);

typedef enum {
  STCS_BINDING_KIND_INPUT = 0,
  STCS_BINDING_KIND_OUTPUT
} STCSBindingKind;

/* For OUTPUT bindings: command (default) vs feedback (device → STOutput). */
typedef enum {
  STCS_BINDING_ROLE_COMMAND = 0,
  STCS_BINDING_ROLE_FEEDBACK
} STCSBindingRole;

typedef enum {
  STCS_OUTPUT_WRITE_IDLE = 0,
  STCS_OUTPUT_WRITE_PENDING,
  STCS_OUTPUT_WRITE_FAILED
} STCSOutputWriteState;

struct _STCSBinding {
  STCSBindingKind kind;
  STCSBindingRole role; /* meaningful for OUTPUT bindings */
  gchar *datapoint_id;
  gchar *peer_name; /* STInput or STOutput name */
  guint64 settle_delay_ms; /* feedback only; default 5000; 0 = no settle */
  gulong handler_id; /* set while wired in runlevel 2 */
  gpointer handler_data; /* owned; freed when unwired / binding freed */
};

/* Per-output runtime for feedback sync + write ack + override timeout. */
struct _STCSOutputRuntime {
  STCSContext *context; /* not owned */
  gchar *output_name;
  STOutput *output; /* not owned; points into context->outputs */
  guint64 override_timeout_ms; /* 0 = never auto-clear */
  guint64 settle_delay_ms; /* from feedback bind; 0 = no settle window */
  gint64 feedback_ignore_until_us; /* monotonic; ignore fb mismatch until */
  guint syncing; /* >0 while a reader parse that feeds this output is active */
  STCSOutputWriteState write_state;
  guint override_timer_id;
  gchar *command_datapoint_id; /* datapoint that drives writes; may be NULL */
};

struct _STCSRunlevel {
    char *name;
    STCSRunlevelEnterFunc *enter;
    STCSRunlevelExitFunc *exit;
    gpointer custom_data;
};

struct _STCSContext {
    struct {
	GPtrArray *info;
	int target;
	int current;
    } runlevel;
    struct {
	gchar    *config_file;
	gchar    *config_preprocessor; /* CLI/env; empty/NULL = no preprocess */
	gchar    *plugin_dir;
	gchar    *service_name;
	STVersion service_version;
	gboolean  verbose;
	gboolean  version; /* CLI --version */
    } options;
    GHashTable *plugins;
    GHashTable *transports;
    GHashTable *datapoints;
    GHashTable *readers;
    GHashTable *writers;
    GPtrArray  *inputs;   /* STInput* owned, for STServer registration */
    GPtrArray  *outputs;  /* STOutput* owned, for STServer registration */
    GPtrArray  *bindings; /* STCSBinding* owned */
    /* output name → STCSOutputRuntime* (owned). Created at config for each output. */
    GHashTable *output_runtimes;
    STServer   *server;   /* owned while runlevel >= 3 */
    STMDNS     *mdns;     /* owned while runlevel >= 4 */
};

void stcs_internals_context_initialize(STCSContext *context);
void stcs_internals_context_finalize(STCSContext *context);

void stcs_internals_runlevel_initialize(STCSContext *context);
int stcs_internals_runlevel_up(STCSContext *context, GError **error);
int stcs_internals_runlevel_down(STCSContext *context, GError **error);

void stcs_internals_log_init(const char *instance_tag);
gboolean stcs_internals_scan_plugins(STCSContext *context, GError **error);

gboolean stcs_internals_config_parse(STCSContext *context, const gchar*path, GError **error);

void stcs_binding_free(STCSBinding *binding);

STCSOutputRuntime *stcs_output_runtime_new(STCSContext *context,
					   STOutput *output,
					   guint64 override_timeout_ms);
void stcs_output_runtime_free(STCSOutputRuntime *rt);
STCSOutputRuntime *stcs_context_lookup_output_runtime(STCSContext *context,
						     const gchar *output_name);

void stcs_output_runtime_sync_begin(STCSOutputRuntime *rt);
void stcs_output_runtime_sync_end(STCSOutputRuntime *rt);
void stcs_output_runtime_note_write_pending(STCSOutputRuntime *rt);
void stcs_output_runtime_note_write_ack(STCSOutputRuntime *rt, gboolean success);
void stcs_output_runtime_arm_settle(STCSOutputRuntime *rt);
void stcs_output_runtime_clear_override_timer(STCSOutputRuntime *rt);

#endif /* STCS_INTERNALS_H_INCLUDED */
