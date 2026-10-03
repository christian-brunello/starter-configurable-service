/*
 * starter-configurable-service - formula-transform.c
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
#include <math.h>

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "plugin.h"
#include "starter/configurable-service/transform.h"
#include "tinyexpr.h"

#define STCS_FORMULA_INPUT_VAR "VALUE"

#define STCS_TYPE_FORMULA_TRANSFORM (stcs_formula_transform_get_type())
G_DECLARE_FINAL_TYPE(STCSFormulaTransform, stcs_formula_transform, STCS, FORMULA_TRANSFORM, STCSTransform)

struct _STCSFormulaTransform {
  STCSTransform parent_instance;
  gchar *expr_src;
  gchar *expr_compiled_src;
  double input_value;
  te_variable vars[1];
  te_expr *expr;
};

G_DEFINE_TYPE(STCSFormulaTransform, stcs_formula_transform, STCS_TYPE_TRANSFORM)

static gboolean
stcs_formula_transform_parse_string(xmlNodePtr node, gchar **out, GError **error)
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

static gchar *
stcs_formula_transform_rewrite_expr(const gchar *src)
{
  GString *out;
  const gchar *p;

  out = g_string_new(NULL);
  for (p = src; *p != '\0'; p++)
    {
      if (*p == '#')
	g_string_append(out, STCS_FORMULA_INPUT_VAR);
      else
	g_string_append_c(out, *p);
    }

  return g_string_free(out, FALSE);
}

static void
stcs_formula_transform_clear_expr(STCSFormulaTransform *self)
{
  if (self->expr != NULL)
    {
      te_free(self->expr);
      self->expr = NULL;
    }

  g_clear_pointer(&self->expr_compiled_src, g_free);
}

static gboolean
stcs_formula_transform_configure(STCSTransform *transform, xmlNodePtr node, GError **error)
{
  STCSFormulaTransform *self = STCS_FORMULA_TRANSFORM(transform);
  xmlNodePtr child;
  gchar *expr = NULL;
  gchar *rewritten = NULL;
  int te_error = 0;
  gboolean r = FALSE;

  g_return_val_if_fail(node != NULL, FALSE);

  for (child = node->children; child != NULL; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(child->name, (const xmlChar *) "expr"))
	{
	  if (!stcs_formula_transform_parse_string(child, &expr, error))
	    goto out;
	}
      else
	{
	  g_set_error(error,
		      G_IO_ERROR,
		      G_IO_ERROR_INVALID_DATA,
		      "unexpected element <%s> inside formula-transform config",
		      (const char *) child->name);
	  goto out;
	}
    }

  if (expr == NULL || *expr == '\0')
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "formula-transform requires a non-empty <expr> element");
      goto out;
    }

  rewritten = stcs_formula_transform_rewrite_expr(expr);
  stcs_formula_transform_clear_expr(self);

  self->vars[0].name = STCS_FORMULA_INPUT_VAR;
  self->vars[0].address = &self->input_value;
  self->vars[0].type = TE_VARIABLE;
  self->vars[0].context = NULL;

  self->expr = te_compile(rewritten, self->vars, 1, &te_error);
  if (self->expr == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "invalid formula `%s' (rewritten: `%s', error near offset %d)",
		  expr, rewritten, te_error);
      goto out;
    }

  g_free(self->expr_src);
  self->expr_src = expr;
  expr = NULL;
  self->expr_compiled_src = rewritten;
  rewritten = NULL;
  r = TRUE;

out:
  g_free(expr);
  g_free(rewritten);
  return r;
}

static gboolean
stcs_formula_transform_process(STCSTransform *transform, gdouble in, gdouble *out, GError **error)
{
  STCSFormulaTransform *self = STCS_FORMULA_TRANSFORM(transform);
  double result;

  if (self->expr == NULL)
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_NOT_INITIALIZED,
		  "formula-transform is not configured");
      return FALSE;
    }

  self->input_value = in;
  result = te_eval(self->expr);

  if (isnan(result) || isinf(result))
    {
      g_set_error(error,
		  G_IO_ERROR,
		  G_IO_ERROR_INVALID_DATA,
		  "formula `%s' produced a non-finite result for input %g",
		  self->expr_src ? self->expr_src : "?",
		  in);
      return FALSE;
    }

  *out = result;
  return TRUE;
}

static void
stcs_formula_transform_dispose(GObject *object)
{
  STCSFormulaTransform *self = STCS_FORMULA_TRANSFORM(object);

  stcs_formula_transform_clear_expr(self);

  G_OBJECT_CLASS(stcs_formula_transform_parent_class)->dispose(object);
}

static void
stcs_formula_transform_finalize(GObject *object)
{
  STCSFormulaTransform *self = STCS_FORMULA_TRANSFORM(object);

  g_free(self->expr_src);

  G_OBJECT_CLASS(stcs_formula_transform_parent_class)->finalize(object);
}

static void
stcs_formula_transform_class_init(STCSFormulaTransformClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCSTransformClass *transform_class = STCS_TRANSFORM_CLASS(klass);

  object_class->dispose = stcs_formula_transform_dispose;
  object_class->finalize = stcs_formula_transform_finalize;

  transform_class->configure = stcs_formula_transform_configure;
  transform_class->process = stcs_formula_transform_process;
}

static void
stcs_formula_transform_init(STCSFormulaTransform *self)
{
  self->expr_src = NULL;
  self->expr_compiled_src = NULL;
  self->input_value = 0.0;
  memset(self->vars, 0, sizeof self->vars);
  self->expr = NULL;
}

G_MODULE_EXPORT STCSPluginInfo *
stcs_plugin_get_info(void)
{
  return stcs_plugin_info_new("formula-transform", STCS_PLUGIN_CATEGORY_TRANSFORM, "1.0.0");
}

G_MODULE_EXPORT GObject *
stcs_plugin_create(void)
{
  return g_object_new(STCS_TYPE_FORMULA_TRANSFORM, NULL);
}
