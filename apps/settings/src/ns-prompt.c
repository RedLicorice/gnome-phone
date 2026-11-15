/* Minerva > Prompt: the two long texts of the page, the system prompt and the state template, are edited in Neo
 * Prompt (neo-prompt, the same page on the phone's own bar): here only their rows and Reset, the variables of the
 * state and a preview of the block as it renders now. Every label comes from `prompt_ui` of `minerva-config status`
 * (config/minerva_config/prompt_ui.json); the texts themselves are read and written by Neo Prompt through
 * `minerva-config prompt …` and `minerva-config state …`, which refuse a template that does not render. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

typedef struct {
  GObject    *owner;       /* the page: kept alive by the running jobs */
  JsonObject *ui;
  GtkWidget  *sys_msg, *state_msg, *vars_g, *preview;
} Pr;

static void
pr_free (Pr *p)
{
  json_object_unref (p->ui);
  g_free (p);
}

static const char *
ui_str (Pr *p, const char *section, const char *key)
{
  return str_of (obj_of (p->ui, section), key);
}

static void
msg_set (GtkWidget *label, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
  if (error)
    gtk_widget_add_css_class (label, "ns-danger");
  else
    gtk_widget_remove_css_class (label, "ns-danger");
}

static GtkWidget *
msg_new (GtkWidget *group)
{
  GtkWidget *l = gtk_label_new ("");
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_set_visible (l, FALSE);
  ns_row_widget (group, NULL, l);
  return l;
}

/* ---- loading ---- */

static void
vars_fill (Pr *p, JsonArray *vars)
{
  ns_group_clear (p->vars_g);
  for (guint i = 0; vars && i < json_array_get_length (vars); i++) {
    JsonObject *v = json_array_get_object_element (vars, i);
    g_autofree char *title = g_strdup_printf ("{{ %s }}", str_of (v, "name"));
    g_autofree char *sub = g_strdup_printf ("%s • %s", *str_of (v, "value") ? str_of (v, "value") : "—", str_of (v, "description"));
    ns_row (p->vars_g, NULL, NULL, title, sub);
  }
}

static void
state_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pr *p = data;
  if (err || !root) {
    msg_set (p->state_msg, err ? err : "?", TRUE);
    return;
  }
  vars_fill (p, arr_of (root, "variables"));
  gtk_label_set_text (GTK_LABEL (p->preview), str_of (root, "text"));
  msg_set (p->state_msg, str_of (root, "error"), TRUE);
}

static void
state_load (Pr *p)
{
  const char *const args[] = { "state", "show", "--json", NULL };
  ns_mc_run (p->owner, args, NULL, state_loaded, p);
}

/* ---- the buttons: Reset only; editing is Neo Prompt's ---- */

typedef struct {
  Pr         *p;
  const char *const *args;   /* static argv */
  GtkWidget  *msg;
  const char *section, *done_key;
  gboolean    refresh_state, ask;
} Act;

static void
act_done (JsonObject *root, const char *err, gpointer data)
{
  Act *a = data;
  if (err) {
    msg_set (a->msg, err, TRUE);
    return;
  }
  if (!json_object_get_boolean_member_with_default (root, "ok", FALSE)) {
    msg_set (a->msg, str_of (root, "error"), TRUE);   /* refused: nothing was written */
    return;
  }
  msg_set (a->msg, ui_str (a->p, a->section, a->done_key), FALSE);
  if (a->refresh_state)
    state_load (a->p);
}

static void
act_run (Act *a)
{
  ns_mc_run (a->p->owner, a->args, NULL, act_done, a);
}

static void
act_confirmed (gboolean yes, gpointer data)
{
  if (yes)
    act_run (data);
}

static void
act_tapped (GtkWidget *row, gpointer data)
{
  Act *a = data;
  if (a->ask)
    ns_confirm (row, ui_str (a->p, a->section, "reset"), ui_str (a->p, a->section, "reset_body"),
                ui_str (a->p, a->section, "reset"), TRUE, act_confirmed, a);
  else
    act_run (a);
}

static void
button (Pr *p, GtkWidget *group, const char *label, gboolean destructive, Act a)
{
  Act *copy = g_memdup2 (&a, sizeof a);
  GtkWidget *row = ns_row_button (group, label, destructive, act_tapped, copy);
  g_object_set_data_full (G_OBJECT (row), "pr-act", copy, g_free);
}

/* ---- the rows that open Neo Prompt ---- */

static void
open_system (GtkWidget *row, gpointer data)
{
  ns_prompt_open ("system-prompt", NULL);
}

static void
open_state (GtkWidget *row, gpointer data)
{
  ns_prompt_open ("state-template", NULL);
}

/* the section in front of the page's groups: its own cards, below the wake word and reply ones */
void
ns_prompt_build (GtkWidget *content, GObject *owner, JsonObject *ui)
{
  Pr *p = g_new0 (Pr, 1);
  p->owner = owner;
  p->ui = json_object_ref (ui);
  g_object_set_data_full (owner, "pr", p, (GDestroyNotify) pr_free);

  static const char *const sys_reset[] = { "prompt", "reset", "--json", NULL };
  static const char *const st_reset[] = { "state", "reset", "--json", NULL };

  GtkWidget *g = ns_group (content, ui_str (p, "system", "title"));
  GtkWidget *row = ns_row (g, "document-edit-symbolic", "violet", ui_str (p, "system", "edit"),
                           ui_str (p, "system", "edit_sub"));
  p->sys_msg = msg_new (g);
  ns_row_on_activate (row, open_system, NULL);
  button (p, g, ui_str (p, "system", "reset"), TRUE,
          (Act){ p, sys_reset, p->sys_msg, "system", "restored", FALSE, TRUE });
  ns_note (content, ui_str (p, "system", "note"));

  g = ns_group (content, ui_str (p, "state", "title"));
  row = ns_row (g, "document-edit-symbolic", "violet", ui_str (p, "state", "edit"),
                ui_str (p, "state", "edit_sub"));
  p->state_msg = msg_new (g);
  ns_row_on_activate (row, open_state, NULL);
  ns_note (content, ui_str (p, "state", "note"));

  p->vars_g = ns_group (content, ui_str (p, "state", "variables"));
  ns_note (content, ui_str (p, "state", "variables_note"));

  g = ns_group (content, ui_str (p, "state", "preview_title"));
  p->preview = gtk_label_new (ui_str (p, "state", "preview_empty"));
  gtk_label_set_wrap (GTK_LABEL (p->preview), TRUE);
  gtk_label_set_xalign (GTK_LABEL (p->preview), 0);
  ns_row_widget (g, NULL, p->preview);
  button (p, g, ui_str (p, "state", "reset"), TRUE,
          (Act){ p, st_reset, p->state_msg, "state", "restored", TRUE, TRUE });

  state_load (p);
}
