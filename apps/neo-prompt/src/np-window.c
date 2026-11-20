/* Neo Prompt's window: see np-window.h. */
#include "np-window.h"
#include "np-context.h"
#include "np-editor.h"
#include "np-history.h"
#include "np-ui.h"

#include <neo.h>
#include <string.h>

typedef struct {
  GtkApplication *app;
  NpStore        *store;
  GtkWindow      *win;
  GtkWidget      *stack, *nav, *title, *actions, *snackbar;
  GtkWidget      *files_list, *files_msg, *files_search;
  GtkWidget      *back_btn, *save_btn, *search_btn, *index_btn, *refresh_btn;
  GtkWidget      *mode_source_btn, *mode_rendered_btn;
  GtkWidget      *edit_btn, *cancel_btn, *sidebar_btn;
  /* the layout: portrait is the bar, the pages and the bottom bar; landscape is the bar over the rail, the file list
   * (collapsible, the choice remembered) and the page of the text beside it */
  GtkWidget      *box, *body, *side, *files_root, *empty_page;
  GtkWidget      *budget_label, *budget_bar;
  gboolean        landscape, laid_out;
  GHashTable     *tokens;        /* doc id -> its estimated tokens (GINT_TO_POINTER (n + 1)) */
  GHashTable     *rows;          /* doc id -> its row in the file list */
  guint           token_next, token_gen;
  NpEditor       *editor;
  NpHistory      *history;
  NpContext      *context;
  char           *pending_id, *pending_person;
  guint           tab;
} Win;

static void select_tab (Win *self, guint tab);
static void back (Win *self);
static void update_buttons (Win *self);
static void files_fill (Win *self);
static void tokens_restart (Win *self);

#define SIDEBAR_FLAG "sidebar_collapsed"

/* the text page is on screen: its own tab, or beside the file list in landscape */
static gboolean
in_editor (Win *self)
{
  return self->tab == 1 || (self->landscape && self->tab == 0);
}

static void
on_saved (gpointer data)
{
  Win *self = data;

  np_editor_set_editing (self->editor, FALSE);   /* a saved text is read again */
  files_fill (self);
  tokens_restart (self);
  if (self->pending_id)
    {
      g_autofree char *id = g_steal_pointer (&self->pending_id);
      g_autofree char *person = g_steal_pointer (&self->pending_person);

      np_window_open_doc (GTK_WIDGET (self->win), id, person);
    }
  np_history_refresh (self->history);
}

static void
on_editor_changed (gpointer data)
{
  Win *self = data;
  NpDoc *doc = np_editor_doc (self->editor);
  g_autofree char *text = NULL;

  text = g_strdup_printf ("%s%s", doc ? doc->title : np_ui (self->store, "editor.title"),
                          np_editor_modified (self->editor) ? " •" : "");
  if (in_editor (self))
    gtk_label_set_text (GTK_LABEL (self->title), text);
  update_buttons (self);
}

static void
do_open (Win *self, const char *id, const char *person)
{
  NpDoc *doc = np_doc_by_id (self->store, id, person);

  if (!doc)
    {
      g_autofree char *text = g_strdup_printf (np_ui (self->store, "files.unknown"), id);

      np_msg_set (self->files_msg, text, TRUE);
      select_tab (self, 0);
      return;
    }
  np_editor_open (self->editor, doc);
  np_history_set_doc (self->history, np_editor_doc (self->editor));
  on_editor_changed (self);
  select_tab (self, 1);
}

static void
pending_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = data;

  if (response == NEO_RESPONSE_OK)
    {
      if (!np_editor_modified (self->editor))
        {
          g_autofree char *id = g_steal_pointer (&self->pending_id);
          g_autofree char *person = g_steal_pointer (&self->pending_person);

          do_open (self, id, person);
        }
      else
        np_editor_save (self->editor);
    }
  else if (response == 2)
    {
      g_autofree char *id = g_steal_pointer (&self->pending_id);
      g_autofree char *person = g_steal_pointer (&self->pending_person);

      g_clear_pointer (&self->pending_person, g_free);
      do_open (self, id, person);
    }
  else
    {
      g_clear_pointer (&self->pending_id, g_free);
      g_clear_pointer (&self->pending_person, g_free);
    }
}

static void
go_files (Win *self)
{
  select_tab (self, 0);
}

static void
leave_saved (gboolean ok, gpointer data)
{
  Win *self = data;

  if (ok)
    go_files (self);
}

/* "Salva / Scarta / Annulla": the one question a Back out of an edited text asks, and it asks it once */
static void
leave_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = data;

  if (response == NEO_RESPONSE_OK)
    np_editor_save_then (self->editor, leave_saved, self);
  else if (response == 2)
    {
      np_editor_discard (self->editor);
      go_files (self);
    }
}

static void
ask_leave (Win *self)
{
  NeoDialog *dialog = neo_dialog_new (np_ui (self->store, "editor.unsaved_title"));

  neo_dialog_set_message (dialog, np_ui (self->store, "editor.leave_body"));
  neo_dialog_add_button (dialog, np_ui (self->store, "save.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, np_ui (self->store, "editor.discard"), 2);
  neo_dialog_add_button (dialog, np_ui (self->store, "editor.save"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (leave_answered), self);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

/* the dialog on top of this window, when one is up */
static NeoDialog *
open_dialog (Win *self)
{
  GtkWidget *overlay = GTK_WIDGET (neo_window_get_overlay (GTK_WIDGET (self->win)));

  for (GtkWidget *child = overlay ? gtk_widget_get_first_child (overlay) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    if (NEO_IS_DIALOG (child) && gtk_widget_get_visible (child))
      return NEO_DIALOG (child);
  return NULL;
}

/* Back: the dialog that is up first (Annulla), then the diff, then the search, then the page before, and out from the
 * file list. From the editor it asks first when the text has changed, and leaves only once the text is safe. */
static void
back (Win *self)
{
  NeoDialog *dialog = open_dialog (self);

  if (dialog)
    {
      neo_dialog_respond (dialog, NEO_RESPONSE_CANCEL);
      return;
    }
  if (self->tab == 3 && np_history_back (self->history))
    return;
  if (self->tab == 1 && np_editor_search_open (self->editor))
    {
      np_editor_search (self->editor);
      return;
    }
  if (self->tab == 0)
    {
      gtk_window_close (GTK_WINDOW (self->win));
      return;
    }
  if (self->tab == 1 && np_editor_saving (self->editor))
    return;
  if (self->tab == 1 && np_editor_modified (self->editor))
    {
      ask_leave (self);
      return;
    }
  go_files (self);
}

static gboolean
on_back_action (GtkWidget *widget, GVariant *args, gpointer data)
{
  back (data);
  return TRUE;
}

static void
add_back_shortcut (Win *self)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();

  gtk_shortcut_controller_add_shortcut (
    GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      gtk_callback_action_new (on_back_action, self, NULL)));
  gtk_widget_add_controller (GTK_WIDGET (self->win), keys);
}

static void
on_back (GtkWidget *button, gpointer data)
{
  back (data);
}

void
np_window_open_doc (GtkWidget *window, const char *id, const char *person)
{
  Win *self = g_object_get_data (G_OBJECT (window), "np-win");
  NpDoc *open = np_editor_doc (self->editor);

  if (!id)
    return;
  /* the text already open: its buffer is what the user has been editing, so it is shown, not read again */
  if (open && g_strcmp0 (open->id, id) == 0 && g_strcmp0 (open->person, person) == 0)
    {
      select_tab (self, 1);
      return;
    }
  if (np_editor_modified (self->editor) && open)
    {
      NeoDialog *dialog = neo_dialog_new (np_ui (self->store, "editor.unsaved_title"));

      g_free (self->pending_id);
      g_free (self->pending_person);
      self->pending_id = g_strdup (id);
      self->pending_person = g_strdup (person);
      neo_dialog_set_message (dialog, np_ui (self->store, "editor.unsaved_body"));
      neo_dialog_add_button (dialog, np_ui (self->store, "save.cancel"), NEO_RESPONSE_CANCEL);
      neo_dialog_add_button (dialog, np_ui (self->store, "editor.discard"), 2);
      neo_dialog_add_button (dialog, np_ui (self->store, "editor.save_open"), NEO_RESPONSE_OK);
      g_signal_connect (dialog, "response", G_CALLBACK (pending_answered), self);
      neo_dialog_present (dialog, GTK_WIDGET (self->win));
      return;
    }
  do_open (self, id, person);
}

/* ---- the File tab ---- */

static void
file_row_clicked (GtkWidget *row, gpointer data)
{
  Win *self = data;

  np_window_open_doc (GTK_WIDGET (self->win), g_object_get_data (G_OBJECT (row), "np-id"), NULL);
}

/* ---- what a row says: the description, the tokens once read, a draft not saved ---- */

static gboolean
has_draft (Win *self, NpDoc *doc)
{
  g_autofree char *path = np_draft_path (self->store, doc);

  return path && g_file_test (path, G_FILE_TEST_EXISTS);
}

static char *
row_sub (Win *self, NpDoc *doc)
{
  GString *s = g_string_new (doc->description ? doc->description
                             : doc->path ? np_short_path (doc->path) : doc->source_label ? doc->source_label : "");
  gpointer known = g_hash_table_lookup (self->tokens, doc->id);

  if (known)
    {
      g_autofree char *tokens = g_strdup_printf (np_ui (self->store, "files.tokens"), GPOINTER_TO_INT (known) - 1);

      g_string_append_printf (s, "%s%s", s->len ? np_ui (self->store, "files.separator") : "", tokens);
    }
  if (has_draft (self, doc))
    g_string_append_printf (s, "%s%s", s->len ? np_ui (self->store, "files.separator") : "",
                            np_ui (self->store, "files.draft"));
  return g_string_free (s, FALSE);
}

/* the texts that make the prompt, against its budget (limits.prompt_budget_tokens): an estimate from their length */
static void
budget_update (Win *self)
{
  int total = 0, budget = self->store->conf.prompt_budget_tokens;
  gboolean pending = FALSE;
  g_autofree char *text = NULL;

  if (!self->budget_label || budget <= 0)
    return;
  for (guint i = 0; i < self->store->docs->len; i++)
    {
      NpDoc *doc = g_ptr_array_index (self->store->docs, i);
      gpointer known = doc->in_prompt ? g_hash_table_lookup (self->tokens, doc->id) : NULL;

      if (!doc->in_prompt)
        continue;
      if (known)
        total += GPOINTER_TO_INT (known) - 1;
      else
        pending = TRUE;
    }
  text = pending ? g_strdup (np_ui (self->store, "files.budget_pending"))
                 : g_strdup_printf (np_ui (self->store, "files.budget"), total, budget);
  gtk_label_set_text (GTK_LABEL (self->budget_label), text);
  gtk_level_bar_set_value (GTK_LEVEL_BAR (self->budget_bar), MIN (total, budget));
  if (!pending && total > budget)
    gtk_widget_add_css_class (self->budget_bar, "np-over");
  else
    gtk_widget_remove_css_class (self->budget_bar, "np-over");
}

typedef struct {
  Win   *self;
  guint  gen;
  char  *id;
} TokenLoad;

static void tokens_next (Win *self);

static void
tokens_loaded (NpAns *ans, gpointer data)
{
  TokenLoad *load = data;
  Win *self = load->self;

  if (load->gen == self->token_gen)
    {
      if (ans->ok)
        {
          int n = (int) ((g_utf8_strlen (ans->text ? ans->text : "", -1) + self->store->conf.chars_per_token - 1)
                         / self->store->conf.chars_per_token);
          GtkWidget *row = g_hash_table_lookup (self->rows, load->id);
          NpDoc *doc = np_doc_by_id (self->store, load->id, NULL);

          g_hash_table_insert (self->tokens, g_strdup (load->id), GINT_TO_POINTER (n + 1));
          if (row && doc)
            {
              g_autofree char *sub = row_sub (self, doc);

              np_row_set_sub (row, sub);
            }
          np_doc_free (doc);
        }
      budget_update (self);
      tokens_next (self);
    }
  g_free (load->id);
  g_free (load);
}

/* one text at a time, in the background: each read may run minerva-config */
static void
tokens_next (Win *self)
{
  while (self->token_next < self->store->docs->len)
    {
      NpDoc *doc = g_ptr_array_index (self->store->docs, self->token_next++);
      TokenLoad *load = NULL;

      if (!doc->editable || !doc->listed || doc->needs_person)
        continue;
      load = g_new0 (TokenLoad, 1);
      load->self = self;
      load->gen = self->token_gen;
      load->id = g_strdup (doc->id);
      np_doc_load (self->store, doc, G_OBJECT (self->win), NULL, tokens_loaded, load);
      return;
    }
}

static void
tokens_restart (Win *self)
{
  self->token_gen++;
  self->token_next = 0;
  g_hash_table_remove_all (self->tokens);
  budget_update (self);
  tokens_next (self);
}

static void
files_fill (Win *self)
{
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (self->files_search));
  g_autofree char *folded = needle && *needle ? g_utf8_casefold (needle, -1) : NULL;
  GHashTable *groups = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *order = g_ptr_array_new ();
  GtkWidget *card = NULL;
  const char *group = NULL;

  while (gtk_widget_get_first_child (self->files_list))
    gtk_box_remove (GTK_BOX (self->files_list), gtk_widget_get_first_child (self->files_list));
  g_hash_table_remove_all (self->rows);

  for (guint i = 0; i < self->store->docs->len; i++)
    {
      NpDoc *doc = g_ptr_array_index (self->store->docs, i);
      g_autofree char *hay = NULL;
      g_autofree char *joined = NULL;
      gboolean match = TRUE;

      if (!doc->editable || !doc->listed || doc->needs_person)
        continue;
      joined = g_strdup_printf ("%s %s %s", doc->title, doc->group, doc->path ? doc->path : "");
      hay = g_utf8_casefold (joined, -1);
      if (folded && !g_strstr_len (hay, -1, folded))
        match = FALSE;
      if (!match)
        continue;
      group = doc->group && *doc->group ? doc->group : np_ui (self->store, "files.other");
      card = g_hash_table_lookup (groups, group);
      if (!card)
        {
          GtkWidget *label = np_label (group, "np-group");
          GtkWidget *box = np_card ();

          gtk_widget_set_margin_start (label, 16);
          gtk_widget_set_margin_end (label, 16);
          gtk_box_append (GTK_BOX (self->files_list), label);
          gtk_box_append (GTK_BOX (self->files_list), box);
          card = box;
          g_hash_table_insert (groups, (gpointer) group, card);
          g_ptr_array_add (order, card);
        }
      {
        g_autofree char *sub = row_sub (self, doc);
        GtkWidget *row = np_row (doc->title, sub, "np-file-row");

        g_hash_table_insert (self->rows, g_strdup (doc->id), row);
        g_object_set_data_full (G_OBJECT (row), "np-id", g_strdup (doc->id), g_free);
        g_signal_connect (row, "clicked", G_CALLBACK (file_row_clicked), self);
        gtk_box_append (GTK_BOX (card), row);
      }
    }
  if (!order->len)
    {
      card = np_card ();
      gtk_box_append (GTK_BOX (card), np_row (np_ui (self->store, "files.empty"), NULL, "np-block-flat"));
      gtk_box_append (GTK_BOX (self->files_list), card);
    }
  g_ptr_array_unref (order);
  g_hash_table_unref (groups);
}

static void
on_files_search (GtkSearchEntry *entry, gpointer data)
{
  files_fill (data);
}

static GtkWidget *
files_page (Win *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = NULL;

  self->files_search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->files_search),
                                         np_ui (self->store, "files.search"));
  gtk_widget_set_margin_start (self->files_search, 16);
  gtk_widget_set_margin_end (self->files_search, 16);
  gtk_widget_set_margin_top (self->files_search, 8);
  g_signal_connect (self->files_search, "search-changed", G_CALLBACK (on_files_search), self);
  self->files_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->files_list, 8);
  gtk_widget_set_margin_bottom (self->files_list, 24);
  self->files_msg = np_msg ();
  gtk_widget_set_margin_start (self->files_msg, 16);
  gtk_widget_set_margin_end (self->files_msg, 16);
  scroll = np_scroller (self->files_list);
  gtk_box_append (GTK_BOX (box), self->files_search);
  if (self->store->conf.prompt_budget_tokens > 0)
    {
      self->budget_label = np_label ("", "np-sub");
      self->budget_bar = gtk_level_bar_new_for_interval (0, self->store->conf.prompt_budget_tokens);
      gtk_level_bar_set_mode (GTK_LEVEL_BAR (self->budget_bar), GTK_LEVEL_BAR_MODE_CONTINUOUS);
      gtk_widget_add_css_class (self->budget_bar, "np-budget");
      for (GtkWidget **w = (GtkWidget *[]) { self->budget_label, self->budget_bar, NULL }; *w; w++)
        {
          gtk_widget_set_margin_start (*w, 16);
          gtk_widget_set_margin_end (*w, 16);
          gtk_widget_set_margin_top (*w, 8);
          gtk_box_append (GTK_BOX (box), *w);
        }
    }
  gtk_box_append (GTK_BOX (box), self->files_msg);
  gtk_box_append (GTK_BOX (box), scroll);
  return box;
}

/* ---- the bar ---- */

static void
update_mode_buttons (Win *self)
{
  const char *kind = np_doc_kind (np_editor_doc (self->editor));
  gboolean rendered = np_editor_rendered (self->editor);
  /* a rendered mode is for the texts whose Markdown means something: not JSON, not plain text */
  gboolean can = g_strcmp0 (kind, "markdown") == 0 || g_strcmp0 (kind, "jinja") == 0;

  /* the choice of mode is an editing one: reading shows each kind its own way (editor.read.modes) */
  gboolean editing = in_editor (self) && np_editor_editing (self->editor);

  gtk_widget_set_visible (self->mode_source_btn, editing && can && rendered);
  gtk_widget_set_visible (self->mode_rendered_btn, editing && can && !rendered);
}

/* the text's buttons: Edit while it is read, Cancel and Save while it is edited */
static void
update_buttons (Win *self)
{
  NpDoc *doc = np_editor_doc (self->editor);
  gboolean shown = in_editor (self) && doc != NULL, editing = shown && np_editor_editing (self->editor);

  gtk_widget_set_visible (self->edit_btn, shown && !editing && doc->editable);
  gtk_widget_set_visible (self->cancel_btn, editing);
  gtk_widget_set_visible (self->save_btn, editing);
  gtk_widget_set_visible (self->search_btn, shown);
  gtk_widget_set_visible (self->index_btn, shown);
  gtk_widget_set_visible (self->sidebar_btn, self->landscape && self->tab <= 1);
  update_mode_buttons (self);
}

static void
update_bar (Win *self, guint tab)
{
  gtk_widget_set_visible (self->back_btn, tab != 0);
  gtk_widget_set_visible (self->refresh_btn, tab == 2);
  update_buttons (self);
  if (in_editor (self) && np_editor_doc (self->editor))
    on_editor_changed (self);
  else if (tab == 0)
    gtk_label_set_text (GTK_LABEL (self->title), np_ui (self->store, "app.name"));
  else if (tab == 2)
    gtk_label_set_text (GTK_LABEL (self->title), np_ui (self->store, "context.title"));
  else if (tab == 3)
    gtk_label_set_text (GTK_LABEL (self->title), np_ui (self->store, "history.title"));
  else
    on_editor_changed (self);
}

static void
select_tab (Win *self, guint tab)
{
  static const char *const names[] = { "files", "edit", "context", "history" };

  const char *child = names[tab];
  gboolean collapsed = np_state_flag (self->store, SIDEBAR_FLAG, FALSE);

  if (self->landscape && tab <= 1)   /* the list is beside: the page is the text, or the hint to pick one */
    {
      child = np_editor_doc (self->editor) ? "edit" : "empty";
      if (tab == 0 && collapsed && self->tab != 0)   /* the Texts item brings the list back */
        np_state_flag_set (self->store, SIDEBAR_FLAG, collapsed = FALSE);
    }
  self->tab = tab;
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), child);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->side), self->landscape && tab <= 1 && !collapsed);
  neo_nav_bar_set_selected (NEO_NAV_BAR (self->nav), (int) tab);
  update_bar (self, tab);
  if (tab == 2)
    np_context_refresh (self->context);
  if (tab == 3)
    np_history_refresh (self->history);
}

void
np_window_present_tab (GtkWidget *window, int tab)
{
  Win *self = g_object_get_data (G_OBJECT (window), "np-win");

  select_tab (self, (guint) CLAMP (tab, 0, 3));
}

/* ---- the callbacks of the tabs ---- */

static void
on_nav (NeoNavBar *bar, int index, gpointer data)
{
  select_tab (data, (guint) index);
}

static void
on_save (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_editor_save (self->editor);
}

static void
on_search (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_editor_search (self->editor);
}

static void
on_index (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_editor_index (self->editor, button);
}

static void
on_edit (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_editor_set_editing (self->editor, TRUE);
}

static void
cancel_answered (NeoDialog *dialog, int response, gpointer data)
{
  Win *self = data;

  if (response == 2)
    np_editor_discard (self->editor);   /* the text as its source has it, read again */
}

/* Cancel: back to reading; edits are thrown away only after a question */
static void
on_cancel (GtkWidget *button, gpointer data)
{
  Win *self = data;
  NeoDialog *dialog = NULL;

  if (!np_editor_modified (self->editor))
    {
      np_editor_set_editing (self->editor, FALSE);
      return;
    }
  dialog = neo_dialog_new (np_ui (self->store, "editor.cancel_title"));
  neo_dialog_set_message (dialog, np_ui (self->store, "editor.cancel_body"));
  neo_dialog_add_button (dialog, np_ui (self->store, "editor.keep_editing"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, np_ui (self->store, "editor.discard"), 2);
  g_signal_connect (dialog, "response", G_CALLBACK (cancel_answered), self);
  neo_dialog_present (dialog, GTK_WIDGET (self->win));
}

static void
on_sidebar (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_state_flag_set (self->store, SIDEBAR_FLAG, !np_state_flag (self->store, SIDEBAR_FLAG, FALSE));
  select_tab (self, self->tab);
}

/* ---- the layout, again on every rotation ---- */

static void
relayout (Win *self, gboolean landscape)
{
  GtkWidget *parent = NULL;

  if (self->laid_out && self->landscape == landscape)
    return;
  self->laid_out = TRUE;
  self->landscape = landscape;
  g_object_ref (self->files_root);
  parent = gtk_widget_get_parent (self->files_root);
  if (parent && GTK_IS_REVEALER (parent))
    gtk_revealer_set_child (GTK_REVEALER (parent), NULL);
  else if (parent)
    gtk_stack_remove (GTK_STACK (self->stack), self->files_root);
  if (landscape)
    gtk_revealer_set_child (GTK_REVEALER (self->side), self->files_root);
  else
    gtk_stack_add_named (GTK_STACK (self->stack), self->files_root, "files");
  g_object_unref (self->files_root);

  g_object_ref (self->nav);
  if (gtk_widget_get_parent (self->nav))
    gtk_box_remove (GTK_BOX (gtk_widget_get_parent (self->nav)), self->nav);
  neo_nav_bar_set_rail (NEO_NAV_BAR (self->nav), landscape);
  if (landscape)
    gtk_box_prepend (GTK_BOX (self->body), self->nav);
  else
    gtk_box_append (GTK_BOX (self->box), self->nav);
  g_object_unref (self->nav);
  gtk_widget_set_visible (self->side, landscape);
  select_tab (self, self->tab);
}

static void
on_surface_layout (GdkSurface *surface, int width, int height, gpointer data)
{
  Win *self = data;
  double aspect = self->store->conf.landscape_aspect;

  relayout (self, aspect > 0 && height > 0 && width >= height * aspect);
}

static void
on_realize (GtkWidget *window, gpointer data)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));

  g_signal_connect (surface, "layout", G_CALLBACK (on_surface_layout), data);
}

static void
win_free (Win *self)
{
  g_clear_pointer (&self->tokens, g_hash_table_unref);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  g_free (self);
}

static void
on_refresh (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_context_refresh (self->context);
}

static void
on_mode (GtkWidget *button, gpointer data)
{
  Win *self = data;

  np_editor_set_rendered (self->editor, !np_editor_rendered (self->editor));
  update_mode_buttons (self);
}

static void
on_context_open (gpointer data, const char *id, const char *person)
{
  Win *self = data;

  np_window_open_doc (GTK_WIDGET (self->win), id, person);
}

static char *
editor_text (gpointer data)
{
  Win *self = data;

  return np_editor_text (self->editor);
}

static void
on_restore (gpointer data, const char *text)
{
  Win *self = data;

  np_editor_put_text (self->editor, text);
  select_tab (self, 1);
}

/* ---- construction ---- */

static GtkWidget *
missing_config (Win *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *label = NULL;
  g_autofree char *text = g_strdup_printf (np_ui (self->store, "app.missing_conf"), self->store->conf.conf_path);

  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 64);
  label = np_label (text, "np-sub");
  gtk_box_append (GTK_BOX (box), np_label (np_ui (self->store, "app.name"), "np-title"));
  gtk_box_append (GTK_BOX (box), label);
  return box;
}

GtkWidget *
np_window_new (GtkApplication *app, NpStore *store)
{
  Win *self = g_new0 (Win, 1);
  GtkWidget *window = NULL, *bar = NULL, *box = NULL, *overlay = NULL;

  self->app = app;
  self->store = store;
  window = neo_window_new (app);
  gtk_widget_add_css_class (window, "np");
  self->win = GTK_WINDOW (window);
  g_object_set_data_full (G_OBJECT (window), "np-win", self, (GDestroyNotify) win_free);
  self->tokens = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_signal_connect (window, "realize", G_CALLBACK (on_realize), self);
  add_back_shortcut (self);

  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (bar, "neo-app-bar");
  /* Back is named as the shell's Back gesture looks for it: the desktop's own translation of "Back", which is the
   * name libadwaita and GTK give their back buttons, so the gesture finds this one in any locale */
  self->back_btn = neo_icon_button_new (np_ui (store, "icons.back"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  g_dgettext ("gtk40", "Back"), -1);
  g_signal_connect (self->back_btn, "clicked", G_CALLBACK (on_back), self);
  gtk_box_append (GTK_BOX (bar), self->back_btn);
  self->sidebar_btn = neo_icon_button_new (np_ui (store, "icons.sidebar"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->sidebar_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  np_ui (store, "files.sidebar_hint"), -1);
  g_signal_connect (self->sidebar_btn, "clicked", G_CALLBACK (on_sidebar), self);
  gtk_box_append (GTK_BOX (bar), self->sidebar_btn);
  self->title = np_label (np_ui (store, "app.name"), "neo-app-bar-title");
  gtk_widget_set_hexpand (self->title, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (self->title), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (bar), self->title);
  self->actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  self->save_btn = neo_text_button_new (np_ui (store, "editor.save"));
  self->edit_btn = neo_text_button_new (np_ui (store, "editor.edit"));
  self->cancel_btn = neo_text_button_new (np_ui (store, "editor.cancel"));
  g_signal_connect (self->edit_btn, "clicked", G_CALLBACK (on_edit), self);
  g_signal_connect (self->cancel_btn, "clicked", G_CALLBACK (on_cancel), self);
  self->mode_source_btn = neo_icon_button_new (np_ui (store, "icons.mode_source"));
  self->mode_rendered_btn = neo_icon_button_new (np_ui (store, "icons.mode_rendered"));
  self->search_btn = neo_icon_button_new (np_ui (store, "icons.search"));
  self->index_btn = neo_icon_button_new (np_ui (store, "icons.outline"));
  self->refresh_btn = neo_icon_button_new (np_ui (store, "icons.refresh"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->mode_source_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  np_ui (store, "editor.mode_source_hint"), -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->mode_rendered_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  np_ui (store, "editor.mode_rendered_hint"), -1);
  g_signal_connect (self->save_btn, "clicked", G_CALLBACK (on_save), self);
  g_signal_connect (self->mode_source_btn, "clicked", G_CALLBACK (on_mode), self);
  g_signal_connect (self->mode_rendered_btn, "clicked", G_CALLBACK (on_mode), self);
  g_signal_connect (self->search_btn, "clicked", G_CALLBACK (on_search), self);
  g_signal_connect (self->index_btn, "clicked", G_CALLBACK (on_index), self);
  g_signal_connect (self->refresh_btn, "clicked", G_CALLBACK (on_refresh), self);
  gtk_box_append (GTK_BOX (self->actions), self->edit_btn);
  gtk_box_append (GTK_BOX (self->actions), self->cancel_btn);
  gtk_box_append (GTK_BOX (self->actions), self->save_btn);
  gtk_box_append (GTK_BOX (self->actions), self->mode_source_btn);
  gtk_box_append (GTK_BOX (self->actions), self->mode_rendered_btn);
  gtk_box_append (GTK_BOX (self->actions), self->search_btn);
  gtk_box_append (GTK_BOX (self->actions), self->index_btn);
  gtk_box_append (GTK_BOX (self->actions), self->refresh_btn);
  gtk_box_append (GTK_BOX (bar), self->actions);

  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);
  self->snackbar = neo_snackbar_new ();
  self->editor = np_editor_new (store, self->win, self->snackbar);
  self->context = np_context_new (store, self->win, self->snackbar);
  self->history = np_history_new (store, self->win, self->snackbar);
  np_context_set_open_cb (self->context, on_context_open, self);
  np_history_set_current_cb (self->history, editor_text, self);
  np_history_set_restore_cb (self->history, on_restore, self);
  np_editor_set_saved_cb (self->editor, on_saved, self);
  np_editor_set_changed_cb (self->editor, on_editor_changed, self);

  if (store->conf.registry && store->ui)
    {
      self->files_root = files_page (self);
      gtk_stack_add_named (GTK_STACK (self->stack), self->files_root, "files");
      gtk_stack_add_named (GTK_STACK (self->stack), np_editor_root (self->editor), "edit");
      self->empty_page = np_label (np_ui (store, "files.choose"), "np-sub");
      gtk_widget_set_valign (self->empty_page, GTK_ALIGN_CENTER);
      gtk_stack_add_named (GTK_STACK (self->stack), self->empty_page, "empty");
      gtk_stack_add_named (GTK_STACK (self->stack), np_context_root (self->context), "context");
      gtk_stack_add_named (GTK_STACK (self->stack), np_history_root (self->history), "history");
      files_fill (self);
    }
  else
    gtk_stack_add_named (GTK_STACK (self->stack), missing_config (self), "files");

  self->nav = neo_nav_bar_new ();
  neo_nav_bar_append (NEO_NAV_BAR (self->nav), np_ui (store, "icons.files"), np_ui (store, "tabs.files"));
  neo_nav_bar_append (NEO_NAV_BAR (self->nav), np_ui (store, "icons.edit"), np_ui (store, "tabs.edit"));
  neo_nav_bar_append (NEO_NAV_BAR (self->nav), np_ui (store, "icons.context"), np_ui (store, "tabs.context"));
  neo_nav_bar_append (NEO_NAV_BAR (self->nav), np_ui (store, "icons.history"), np_ui (store, "tabs.history"));
  g_signal_connect (self->nav, "selected", G_CALLBACK (on_nav), self);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->box = box;
  self->body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand (self->body, TRUE);
  self->side = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (self->side), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
  gtk_widget_set_size_request (self->side, store->conf.sidebar_width, -1);
  gtk_widget_add_css_class (self->side, "np-side");
  gtk_widget_set_visible (self->side, FALSE);
  gtk_widget_set_hexpand (self->stack, TRUE);
  gtk_box_append (GTK_BOX (self->body), self->side);
  gtk_box_append (GTK_BOX (self->body), self->stack);
  gtk_box_append (GTK_BOX (box), bar);
  gtk_box_append (GTK_BOX (box), self->body);
  gtk_box_append (GTK_BOX (box), self->nav);
  neo_window_set_content (self->win, box);
  overlay = GTK_WIDGET (neo_window_get_overlay (box));
  if (overlay)
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->snackbar);

  if (!self->files_root)   /* no configuration: the one page is the notice */
    self->files_root = gtk_stack_get_child_by_name (GTK_STACK (self->stack), "files");
  select_tab (self, 0);
  tokens_restart (self);
  return window;
}
