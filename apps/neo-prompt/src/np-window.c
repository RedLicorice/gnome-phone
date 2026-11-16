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
  NpEditor       *editor;
  NpHistory      *history;
  NpContext      *context;
  char           *pending_id, *pending_person;
  guint           tab;
} Win;

static void select_tab (Win *self, guint tab);
static void back (Win *self);

static void
on_saved (gpointer data)
{
  Win *self = data;

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
  gtk_label_set_text (GTK_LABEL (self->title), text);
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
        GtkWidget *row = np_row (doc->title, doc->path ? np_short_path (doc->path) : doc->source_label, "np-file-row");

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

  gtk_widget_set_visible (self->mode_source_btn, self->tab == 1 && can && rendered);
  gtk_widget_set_visible (self->mode_rendered_btn, self->tab == 1 && can && !rendered);
}

static void
update_bar (Win *self, guint tab)
{
  gtk_widget_set_visible (self->back_btn, tab != 0);
  gtk_widget_set_visible (self->save_btn, tab == 1);
  gtk_widget_set_visible (self->search_btn, tab == 1);
  gtk_widget_set_visible (self->index_btn, tab == 1);
  gtk_widget_set_visible (self->refresh_btn, tab == 2);
  update_mode_buttons (self);
  if (tab == 0)
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

  self->tab = tab;
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), names[tab]);
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
  g_object_set_data_full (G_OBJECT (window), "np-win", self, g_free);
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
  self->title = np_label (np_ui (store, "app.name"), "neo-app-bar-title");
  gtk_widget_set_hexpand (self->title, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (self->title), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (bar), self->title);
  self->actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  self->save_btn = neo_text_button_new (np_ui (store, "editor.save"));
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
      gtk_stack_add_named (GTK_STACK (self->stack), files_page (self), "files");
      gtk_stack_add_named (GTK_STACK (self->stack), np_editor_root (self->editor), "edit");
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
  gtk_box_append (GTK_BOX (box), bar);
  gtk_box_append (GTK_BOX (box), self->stack);
  gtk_box_append (GTK_BOX (box), self->nav);
  neo_window_set_content (self->win, box);
  overlay = GTK_WIDGET (neo_window_get_overlay (box));
  if (overlay)
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->snackbar);

  select_tab (self, 0);
  return window;
}
