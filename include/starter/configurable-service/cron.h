/*
 * libstarter-configurable-service - cron.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef STCS_CRON_H_INCLUDED
#define STCS_CRON_H_INCLUDED

#include <glib-object.h>

G_BEGIN_DECLS

#define STCS_TYPE_CRON (stcs_cron_get_type())
G_DECLARE_FINAL_TYPE(STCSCron, stcs_cron, STCS, CRON, GObject)

STCSCron *stcs_cron_new(void);

/*
 * Parse a crontab-like expression into @self.
 *
 * Fields (whitespace-separated):
 *   ms sec min hour day-of-month month [day-of-week]
 *
 * Ranges:
 *   ms              0-999
 *   sec / min       0-59
 *   hour            0-23
 *   day-of-month    1-31
 *   month           1-12
 *   day-of-week     0-7 (0 and 7 = Sunday; optional, default = *)
 *
 * Supports "*", lists (comma), ranges (a-b) and steps (a-b/n or star/n).
 * On failure the previous schedule is left unchanged.
 */
gboolean stcs_cron_parse(STCSCron *self, const gchar *expr, GError **error);

gboolean stcs_cron_is_parsed(STCSCron *self);
const gchar *stcs_cron_get_expression(STCSCron *self);

/*
 * Milliseconds until the next fire after "now" (wall clock, exclusive).
 * Returns -1 if not parsed.
 */
gint64 stcs_cron_ms_until_next(STCSCron *self);

/*
 * One-shot wrapper around g_timeout_add using stcs_cron_ms_until_next().
 * Returns 0 if not parsed or delay cannot be computed.
 */
guint stcs_cron_timeout_add(STCSCron *self, GSourceFunc func, gpointer data);

G_END_DECLS

#endif /* STCS_CRON_H_INCLUDED */
