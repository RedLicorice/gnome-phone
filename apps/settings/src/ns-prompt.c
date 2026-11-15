/* Minerva > Prompt: the two text editors of the page, the system prompt and the state template. Every label comes
 * from `prompt_ui` of `minerva-config status` (data: config/minerva_config/prompt_ui.json); the texts themselves are
 * read and written by `minerva-config prompt …` and `minerva-config state …`, which refuse a template that does not
 * render: the page shows that message under the editor and writes nothing. */
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
  GtkWidget  *sys_view, *state_view;
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

static GtkWidget *
editor_new (void)
{
  GtkWidget *view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (view, "ns-editor");
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), view);
  gtk_widget_add_css_class (sw, "ns-editor-box");
  g_object_set_data (G_OBJECT (sw), "pr-view", view);
  return sw;
}

static GtkWidget *
view_of (GtkWidget *sw)
{
  return g_object_get_data (G_OBJECT (sw), "pr-view");
}

static char *
text_get (GtkWidget *sw)
{
  GtkTextBuffer *b = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view_of (sw)));
  GtkTextIter a, z;
  gtk_text_buffer_get_bounds (b, &a, &z);
  return gtk_text_buffer_get_text (b, &a, &z, FALSE);
}

static void
text_set (GtkWidget *sw, const char *text)
{
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view_of (sw))), text ? text : "", -1);
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
  gtk_label_set_selectable (GTK_LABEL (l), TRUE);
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
  text_set (p->state_view, str_of (root, "template"));
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

static void
sys_loaded (JsonObject *root, const char *err, gpointer data)
{
  Pr *p = data;
  if (err || !root) {
    msg_set (p->sys_msg, err ? err : "?", TRUE);
    return;
  }
  text_set (p->sys_view, str_of (root, "text"));
}

static void
sys_load (Pr *p)
{
  const char *const args[] = { "prompt", "get", "--json", NULL };
  ns_mc_run (p->owner, args, NULL, sys_loaded, p);
}

/* ---- the buttons: one handler per action, the target and the answer text in the closure ---- */

typedef struct {
  Pr         *p;
  const char *const *args;   /* static argv */
  GtkWidget  *view;          /* its text goes to stdin, or NULL */
  GtkWidget  *msg;
  const char *section, *done_key;
  gboolean    refresh_state, refresh_sys, ask;
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
  if (a->args[1] && !g_strcmp0 (a->args[1], "preview"))
    gtk_label_set_text (GTK_LABEL (a->p->preview), str_of (root, "text"));   /* the rendered block */
  msg_set (a->msg, ui_str (a->p, a->section, a->done_key), FALSE);
  if (a->refresh_state)
    state_load (a->p);
  if (a->refresh_sys)
    sys_load (a->p);
}

static void
act_run (Act *a)
{
  g_autofree char *text = a->view ? text_get (a->view) : NULL;
  ns_mc_run (a->p->owner, a->args, text, act_done, a);
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

/* the section in front of the page's groups: its own cards, below the wake word and reply ones */
void
ns_prompt_build (GtkWidget *content, GObject *owner, JsonObject *ui)
{
  Pr *p = g_new0 (Pr, 1);
  p->owner = owner;
  p->ui = json_object_ref (ui);
  g_object_set_data_full (owner, "pr", p, (GDestroyNotify) pr_free);

  static const char *const sys_save[] = { "prompt", "set", "--json", NULL };
  static const char *const sys_reset[] = { "prompt", "reset", "--json", NULL };
  static const char *const st_preview[] = { "state", "preview", "--json", NULL };
  static const char *const st_save[] = { "state", "save", "--json", NULL };
  static const char *const st_reset[] = { "state", "reset", "--json", NULL };

  GtkWidget *g = ns_group (content, ui_str (p, "system", "title"));
  p->sys_view = editor_new ();
  ns_row_widget (g, ui_str (p, "system", "label"), p->sys_view);
  p->sys_msg = msg_new (g);
  button (p, g, ui_str (p, "system", "save"), FALSE,
          (Act){ p, sys_save, p->sys_view, p->sys_msg, "system", "saved", FALSE, TRUE, FALSE });
  button (p, g, ui_str (p, "system", "reset"), TRUE,
          (Act){ p, sys_reset, NULL, p->sys_msg, "system", "restored", FALSE, TRUE, TRUE });
  ns_note (content, ui_str (p, "system", "note"));

  g = ns_group (content, ui_str (p, "state", "title"));
  p->state_view = editor_new ();
  ns_row_widget (g, ui_str (p, "state", "label"), p->state_view);
  p->state_msg = msg_new (g);
  ns_note (content, ui_str (p, "state", "note"));

  p->vars_g = ns_group (content, ui_str (p, "state", "variables"));
  ns_note (content, ui_str (p, "state", "variables_note"));

  g = ns_group (content, ui_str (p, "state", "preview_title"));
  p->preview = gtk_label_new (ui_str (p, "state", "preview_empty"));
  gtk_label_set_wrap (GTK_LABEL (p->preview), TRUE);
  gtk_label_set_xalign (GTK_LABEL (p->preview), 0);
  gtk_label_set_selectable (GTK_LABEL (p->preview), TRUE);
  ns_row_widget (g, NULL, p->preview);
  /* Anteprima fills the preview label itself: its own handler, the answer is text, not a message */
  button (p, g, ui_str (p, "state", "preview"), FALSE,
          (Act){ p, st_preview, p->state_view, p->state_msg, "state", "", FALSE, FALSE, FALSE });
  button (p, g, ui_str (p, "state", "save"), FALSE,
          (Act){ p, st_save, p->state_view, p->state_msg, "state", "saved", TRUE, FALSE, FALSE });
  button (p, g, ui_str (p, "state", "reset"), TRUE,
          (Act){ p, st_reset, NULL, p->state_msg, "state", "restored", TRUE, FALSE, TRUE });

  sys_load (p);
  state_load (p);
}
