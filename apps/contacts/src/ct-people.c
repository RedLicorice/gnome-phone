/* The contact list screen (ct-people.h).
 *
 * Its parts, top to bottom: the toolbar frame, which holds three bars in one place (the normal toolbar, the search
 * box and the selection bar, faded in and out over 195 ms as ActionBarAdapter does); the content (the list, or an
 * empty state), with the FAB over it; the drawer over everything. The label view (§5.1) is this screen too, as it
 * is PeopleActivity on Android: a label's members, its own toolbar colour and menu. */
#include "ct-people.h"
#include "ct-avatar.h"
#include "ct-dialogs.h"
#include "ct-list.h"
#include "ct-minerva-pages.h"
#include "ct-mnv.h"
#include "ct-removal.h"
#include "ns-mc.h"
#include "ct-page.h"
#include "ct-picker.h"
#include "ct-popup.h"
#include "ct-settings.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

typedef enum {
  VIEW_ALL,                     /* Contacts */
  VIEW_LABEL,                   /* a label's members (GroupMembersFragment) */
  VIEW_ACCOUNT,                 /* one address book (the drawer's Accounts) */
} View;

typedef struct {
  GtkWidget *page, *window;
  CtStore *store;
  GSettings *settings;
  View view;
  char *label;                  /* VIEW_LABEL: its name */
  char *account;                /* VIEW_ACCOUNT: the book's uid */

  /* the toolbar frame and its three bars */
  GtkWidget *frame, *bar, *nav, *title, *actions, *search_action, *add_member, *overflow;
  GtkWidget *search_bar, *search_entry, *search_clear;
  GtkWidget *selection_bar, *selection_close, *selection_count, *share, *delete, *selection_overflow;
  NeoAnim *search_anim, *selection_anim;
  gboolean searching, selecting, editing;
  GHashTable *selected;         /* uid set */

  /* the content */
  GtkWidget *list, *header, *header_icon, *header_text, *empty, *account_empty, *label_empty, *no_results, *fab;
  GtkWidget *back_marker;
  NeoAnim *fab_anim;
  gboolean fab_shown;
  gboolean animated_load;

  /* the drawer */
  GtkWidget *drawer, *drawer_scrim, *drawer_sheet, *drawer_items;
  NeoAnim *drawer_anim;
  double drawer_open;           /* 0 closed … 1 open */
  double drawer_drag_from;

  /* the label last deleted and its members, for the snackbar's UNDO (kept until the next deletion) */
  char *undo_label;
  GPtrArray *undo_members;
} People;

static void refresh (People *people, gboolean to_top);
static void rebuild_drawer (People *people);
static void update_toolbar (People *people);
static void open_drawer (People *people, gboolean open);
static void stop_selection (People *people);
static void stop_search (People *people);

static People *
people_of (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "ct-people");
}

/* ===== the list ================================================================================================ */

/* the contacts this view is for */
static GPtrArray *
view_entries (People *people)
{
  GPtrArray *entries = people->view == VIEW_ACCOUNT ? ct_store_dup_entries (people->store)
                                                    : ct_shown_entries (people->store, people->settings);

  for (guint i = entries->len; i > 0; i--)
    {
      CtEntry *entry = entries->pdata[i - 1];
      gboolean keep = TRUE;

      if (people->view == VIEW_LABEL)
        keep = ct_contact_has_label (entry->contact, people->label);
      else if (people->view == VIEW_ACCOUNT)
        keep = g_strcmp0 (entry->book_uid, people->account) == 0;
      if (!keep)
        g_ptr_array_remove_index_fast (entries, i - 1);
    }
  return entries;
}

static const char *
query_of (People *people)
{
  return people->searching ? gtk_editable_get_text (GTK_EDITABLE (people->search_entry)) : "";
}

/* FloatingActionButtonController.scaleIn / scaleOut: 186 ms EASE_IN (0, 0, 0.2, 1) in, 175 ms (0.4, 0, 1, 1) out */
static void
on_fab_anim (NeoAnim *anim, double value, gpointer data)
{
  People *people = data;

  neo_pressable_set_scale (NEO_PRESSABLE (people->fab), value);
}

static void
show_fab (People *people, gboolean show)
{
  double scale = neo_pressable_get_scale (NEO_PRESSABLE (people->fab));

  if (people->fab_shown == show)
    return;
  people->fab_shown = show;
  if (show)
    neo_anim_start (people->fab_anim, scale, 1, CT_FAB_SCALE_IN_MS, 0, SPONGY_LINEAR_OUT_SLOW_IN);
  else
    neo_anim_start (people->fab_anim, scale, 0, CT_FAB_SCALE_OUT_MS, 0, SPONGY_FAST_OUT_LINEAR_IN);
}

/* the FAB shows in the plain list and an account's list, not in a label's, a search or selection mode
 * (PeopleActivity.showFabWithAnimation; the label view hides it) */
static void
update_fab (People *people)
{
  show_fab (people, people->view != VIEW_LABEL && !people->searching && !people->selecting);
}

/* whether Back means something here: the shell's back gesture leaves the app unless it finds a Back button or a
 * dialog, so while Back would leave a mode or a view, a dialog-role marker says so (extension/a11y.js) */
static gboolean
has_back (People *people)
{
  return people->drawer_open > 0 || people->searching || people->selecting || people->view != VIEW_ALL;
}

static void
update_back_marker (People *people)
{
  gtk_widget_set_visible (people->back_marker, has_back (people));
}

static void
refresh (People *people, gboolean to_top)
{
  g_autoptr(GPtrArray) entries = view_entries (people);
  const char *query = query_of (people);
  gboolean search = *query != '\0';
  CtRows *rows = ct_rows_new (entries, ct_display_order (people->settings), ct_sort_order (people->settings), query,
                              people->view == VIEW_ALL);
  guint n = rows->rows->len;
  gboolean header = !search && n > 0 && people->view != VIEW_ALL;

  /* a contact that went is not selected any more */
  if (people->selected)
    {
      GHashTableIter iter;
      gpointer uid;

      g_hash_table_iter_init (&iter, people->selected);
      while (g_hash_table_iter_next (&iter, &uid, NULL))
        if (!ct_store_lookup (people->store, uid))
          g_hash_table_iter_remove (&iter);
    }

  ct_list_set_search (CT_LIST (people->list), search);
  /* the list's 7 dp at the top go under the account header, and under the empty-query search box (§2.1) */
  ct_list_set_top_padding (CT_LIST (people->list), header || (people->searching && !search) ? 0
                                                                                              : CT_LIST_PAD_TOP_DP);
  ct_list_set_rows (CT_LIST (people->list), rows, to_top);

  /* the header: "N contacts" (· the account) over a label's or an account's list */
  gtk_widget_set_visible (people->header, header);
  if (header)
    {
      CtBook *book = people->view == VIEW_ACCOUNT ? ct_store_get_book (people->store, people->account) : NULL;
      g_autofree char *text = ct_contacts_count (n, book && !book->is_local ? ct_book_label (book) : NULL);

      gtk_label_set_label (GTK_LABEL (people->header_text), text);
      /* the account's icon 16 dp in; a label is in no one account, so it has none. The text starts 64 dp in
       * either way (GroupMembersFragment :690-704). */
      gtk_widget_set_visible (people->header_icon, book != NULL);
      gtk_widget_set_margin_start (people->header_text, book ? CT_HEADER_TEXT_X_DP - CT_HEADER_ICON_X_DP - 24
                                                             : CT_HEADER_TEXT_X_DP);
      if (book)
        gtk_image_set_from_icon_name (GTK_IMAGE (people->header_icon),
                                      book->is_local ? "ct-smartphone-symbolic" : "ct-account-circle-symbolic");
    }

  /* the empty states: all contacts', an account's, a label's (§1.9, §5.1.4); a search with no results says so.
   * Before the address books have answered, the list is not empty but unknown, so it shows nothing. */
  gtk_widget_set_visible (people->empty, n == 0 && !people->searching && people->view == VIEW_ALL
                                         && ct_store_is_ready (people->store));
  gtk_widget_set_visible (people->account_empty, n == 0 && !people->searching && people->view == VIEW_ACCOUNT
                                                 && ct_store_is_ready (people->store));
  gtk_widget_set_visible (people->label_empty, n == 0 && !people->searching && people->view == VIEW_LABEL);
  gtk_widget_set_visible (people->no_results, search && n == 0);

  /* slide_and_fade_in_layout_animation: once, when the list first has rows */
  if (!people->animated_load && n > 0)
    {
      people->animated_load = TRUE;
      ct_list_animate_load (CT_LIST (people->list));
    }
  if (people->selecting)
    {
      if (g_hash_table_size (people->selected) == 0 && !people->editing)
        stop_selection (people);
      else
        update_toolbar (people);
    }
}

static void
on_store_changed (CtStore *store, People *people)
{
  /* a label that is gone (renamed or deleted elsewhere) is left */
  if (people->view == VIEW_LABEL && !ct_store_has_label (store, people->label))
    {
      people->view = VIEW_ALL;
      g_clear_pointer (&people->label, g_free);
      update_toolbar (people);
    }
  refresh (people, FALSE);
  rebuild_drawer (people);
}

static void
on_settings_changed (GSettings *settings, const char *key, People *people)
{
  refresh (people, FALSE);
}

/* ===== search (§2) ============================================================================================= */

static void
on_search_anim (NeoAnim *anim, double value, gpointer data)
{
  People *people = data;

  gtk_widget_set_opacity (people->search_bar, value);
  gtk_widget_set_visible (people->search_bar, value > 0);
}

static void
start_search (People *people, const char *text)
{
  if (people->searching)
    return;
  people->searching = TRUE;
  stop_selection (people);
  gtk_editable_set_text (GTK_EDITABLE (people->search_entry), text ? text : "");
  gtk_editable_set_position (GTK_EDITABLE (people->search_entry), -1);
  gtk_widget_set_visible (people->search_bar, TRUE);
  neo_anim_start (people->search_anim, gtk_widget_get_opacity (people->search_bar), 1, CT_ACTIONBAR_ANIM_MS, 0,
                  SPONGY_LINEAR);
  gtk_widget_grab_focus (people->search_entry);
  update_fab (people);
  update_back_marker (people);
  refresh (people, TRUE);
}

static void
stop_search (People *people)
{
  if (!people->searching)
    return;
  people->searching = FALSE;
  neo_anim_start (people->search_anim, gtk_widget_get_opacity (people->search_bar), 0, CT_ACTIONBAR_ANIM_MS, 0,
                  SPONGY_LINEAR);
  gtk_window_set_focus (GTK_WINDOW (gtk_widget_get_root (people->page)), NULL);
  update_fab (people);
  update_back_marker (people);
  refresh (people, TRUE);
}

static void
on_search_changed (GtkEditable *entry, People *people)
{
  gtk_widget_set_visible (people->search_clear, *gtk_editable_get_text (entry) != '\0');
  if (people->searching)
    refresh (people, TRUE);
}

static void
on_search_action (NeoPressable *button, People *people)
{
  start_search (people, NULL);
}

static void
on_search_back (NeoPressable *button, People *people)
{
  stop_search (people);
}

static void
on_search_clear (NeoPressable *button, People *people)
{
  gtk_editable_set_text (GTK_EDITABLE (people->search_entry), "");
  gtk_widget_grab_focus (people->search_entry);
}

/* typing in the list starts a search with what was typed (PeopleActivity.onKeyDown) */
static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, People *people)
{
  gunichar c = gdk_keyval_to_unicode (keyval);
  char text[8] = { 0 };

  if (people->searching || people->selecting || people->drawer_open > 0 || ct_window_top (people->window) != people->page
      || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) || !c || !g_unichar_isprint (c) || g_unichar_isspace (c))
    return FALSE;
  g_unichar_to_utf8 (c, text);
  start_search (people, text);
  return TRUE;
}

/* ===== selection mode (§1.10, §5.1.5) ========================================================================= */

static void
on_selection_anim (NeoAnim *anim, double value, gpointer data)
{
  People *people = data;

  gtk_widget_set_opacity (people->selection_bar, value);
  gtk_widget_set_visible (people->selection_bar, value > 0);
}

static void
start_selection (People *people, const char *uid, gboolean editing)
{
  people->selecting = TRUE;
  people->editing = editing;
  g_hash_table_remove_all (people->selected);
  if (uid)
    g_hash_table_add (people->selected, g_strdup (uid));
  ct_list_set_selection (CT_LIST (people->list), people->selected);
  gtk_widget_set_visible (people->selection_bar, TRUE);
  neo_anim_start (people->selection_anim, gtk_widget_get_opacity (people->selection_bar), 1, CT_ACTIONBAR_ANIM_MS,
                  0, SPONGY_LINEAR);
  open_drawer (people, FALSE);
  update_fab (people);
  update_toolbar (people);
  update_back_marker (people);
}

static void
stop_selection (People *people)
{
  if (!people->selecting)
    return;
  people->selecting = FALSE;
  people->editing = FALSE;
  g_hash_table_remove_all (people->selected);
  ct_list_set_selection (CT_LIST (people->list), NULL);
  neo_anim_start (people->selection_anim, gtk_widget_get_opacity (people->selection_bar), 0, CT_ACTIONBAR_ANIM_MS,
                  0, SPONGY_LINEAR);
  update_fab (people);
  update_toolbar (people);
  update_back_marker (people);
}

static GPtrArray *
selected_uids (People *people)
{
  GPtrArray *uids = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  gpointer uid;

  g_hash_table_iter_init (&iter, people->selected);
  while (g_hash_table_iter_next (&iter, &uid, NULL))
    g_ptr_array_add (uids, g_strdup (uid));
  return uids;
}

static void
on_row_activated (CtList *list, guint index, People *people)
{
  CtEntry *entry = ct_list_get_entry (list, index);

  if (!entry)
    return;
  if (people->selecting)
    {
      /* "Remove contacts" (edit mode) has no checkboxes to toggle: a tap on a row opens it */
      if (people->editing)
        {
          ct_window_show_contact (people->window, entry->uid);
          return;
        }
      if (!g_hash_table_remove (people->selected, entry->uid))
        g_hash_table_add (people->selected, g_strdup (entry->uid));
      /* unchecking the last row ends the mode */
      if (g_hash_table_size (people->selected) == 0)
        stop_selection (people);
      else
        {
          ct_list_update_selection (list);
          update_toolbar (people);
        }
      return;
    }
  ct_window_show_contact (people->window, entry->uid);
}

static void
on_row_long_pressed (CtList *list, guint index, People *people)
{
  CtEntry *entry = ct_list_get_entry (list, index);

  /* a long press in edit mode is ignored (GroupMembersFragment :802-807) */
  if (!entry || people->editing)
    return;
  if (people->selecting)
    {
      on_row_activated (list, index, people);
      return;
    }
  stop_search (people);
  start_selection (people, entry->uid, FALSE);
}

static void
on_selection_close (NeoPressable *button, People *people)
{
  stop_selection (people);
}

/* Share: Android hands a vCard of the selection to another app through its share sheet. This phone has no share
 * sheet, so the vCard is written where the person chooses (the file chooser), from where it can go anywhere. */
static void
on_share (NeoPressable *button, People *people)
{
  g_autoptr(GPtrArray) uids = selected_uids (people);

  ct_export_contacts (people->window, uids,
                      g_dngettext (NULL, "Share contact via", "Share contacts via", uids->len));
  stop_selection (people);
}

typedef struct {
  People *people;
  GPtrArray *uids;
} Deletion;

static void
on_deleted (const char *uid, const GError *error, gpointer data)
{
  Deletion *deletion = data;

  if (error)
    g_warning ("delete: %s", error->message);
  else
    ct_window_toast (deletion->people->window, g_dngettext (NULL, "Contact deleted", "Contacts deleted",
                                                            deletion->uids->len));
  g_ptr_array_unref (deletion->uids);
  g_free (deletion);
}

static void
on_delete_confirmed (int response, gpointer data)
{
  Deletion *deletion = data;

  if (response != CT_DIALOG_OK)
    {
      g_ptr_array_unref (deletion->uids);
      g_free (deletion);
      return;
    }
  stop_selection (deletion->people);
  ct_store_delete (deletion->people->store, deletion->uids, on_deleted, deletion);
}

/* ContactMultiDeletionInteraction: "Delete selected contacts?" (every address book here is writable, so neither
 * read-only variant applies) */
static void
on_delete (NeoPressable *button, People *people)
{
  Deletion *deletion = g_new0 (Deletion, 1);

  deletion->people = people;
  deletion->uids = selected_uids (people);
  /* with Minerva: its own dialog, the proof and the cascade (ct-removal.c) */
  if (ct_removal_active ())
    {
      stop_selection (people);
      ct_removal_delete (people->window, deletion->uids, NULL, NULL);
      g_ptr_array_unref (deletion->uids);
      g_free (deletion);
      return;
    }
  ct_confirm_dialog (people->page, NULL, _("Delete selected contacts?"), _("Delete"), on_delete_confirmed, deletion);
}

/* ===== the label view (§5.1) ================================================================================== */

static void
show_view (People *people, View view, const char *name)
{
  stop_selection (people);
  stop_search (people);
  people->view = view;
  g_free (people->label);
  g_free (people->account);
  people->label = view == VIEW_LABEL ? g_strdup (name) : NULL;
  people->account = view == VIEW_ACCOUNT ? g_strdup (name) : NULL;
  people->animated_load = FALSE;
  update_toolbar (people);
  update_fab (people);
  update_back_marker (people);
  refresh (people, TRUE);
  rebuild_drawer (people);
}

static void
on_members_added (GPtrArray *uids, gpointer data)
{
  People *people = data;

  if (!uids->len || !people->label)
    return;
  ct_store_add_to_label (people->store, uids, people->label);
  ct_window_toast (people->window, _("Added to label"));
}

/* "Add contact": the picker, without the label's members (§5.1.6) */
static void
add_members (People *people)
{
  g_autoptr(GPtrArray) members = ct_store_label_members (people->store, people->label);

  ct_window_push (people->window, ct_picker_new (people->window, _("Select"), members, TRUE, on_members_added,
                                                 people));
}

static void
on_add_member (NeoPressable *button, People *people)
{
  add_members (people);
}

static void
on_add_contacts_button (NeoPressable *button, People *people)
{
  add_members (people);
}

/* the members' (or the selection's) first email address or number each, to one mailto: or smsto: (sendToGroup).
 * Android asks which one for a person with several and no default; here it is the default, else the first. */
static void
send_to (People *people, CtKind kind)
{
  g_autoptr(GPtrArray) uids = people->selecting ? selected_uids (people)
                                                : ct_store_label_members (people->store, people->label);
  g_autoptr(GString) to = g_string_new (NULL);
  g_autofree char *uri = NULL;
  guint missing = 0;

  for (guint i = 0; i < uids->len; i++)
    {
      CtEntry *entry = ct_store_lookup (people->store, uids->pdata[i]);
      g_autoptr(GPtrArray) items = entry ? ct_contact_get_items (entry->contact, kind) : NULL;
      CtItem *chosen = NULL;

      for (guint j = 0; items && j < items->len; j++)
        if (!chosen || ((CtItem *) items->pdata[j])->primary)
          chosen = chosen && chosen->primary ? chosen : items->pdata[j];
      if (!chosen)
        {
          missing++;
          continue;
        }
      if (to->len)
        g_string_append_c (to, ',');
      g_string_append_uri_escaped (to, chosen->value, "+@", TRUE);
    }
  if (missing)
    ct_window_toast (people->window, kind == CT_KIND_EMAIL ? _("Some contacts do not have emails.")
                                                           : _("Some contacts do not have phone numbers."));
  if (!to->len)
    return;
  uri = g_strconcat (kind == CT_KIND_EMAIL ? "mailto:" : "smsto:", to->str, NULL);
  if (!ct_open_uri (people->window, uri))
    ct_window_toast (people->window, _("No app was found to handle this action."));
  if (people->selecting)
    stop_selection (people);
}

static const char *
label_exists (const char *text, gpointer data)
{
  People *people = data;

  /* the name it has already is not "another" label (a rename that changes nothing just closes) */
  if (people->label && g_str_equal (text, people->label))
    return NULL;
  return ct_store_has_label (people->store, text) ? _("That label already exists") : NULL;
}

static void
on_renamed (const char *text, gpointer data)
{
  People *people = data;

  if (!text || !people->label || g_str_equal (text, people->label))
    return;
  ct_store_rename_label (people->store, people->label, text);
  g_free (people->label);
  people->label = g_strdup (text);
  update_toolbar (people);
  ct_window_toast (people->window, _("Label updated"));
}

static void
forget_undo (People *people)
{
  g_clear_pointer (&people->undo_label, g_free);
  g_clear_pointer (&people->undo_members, g_ptr_array_unref);
}

static void
on_undo_delete (gpointer data)
{
  People *people = data;

  /* the label comes back, with its members */
  if (!people->undo_label)
    return;
  ct_store_create_label (people->store, people->undo_label);
  if (people->undo_members && people->undo_members->len)
    ct_store_add_to_label (people->store, people->undo_members, people->undo_label);
  forget_undo (people);
}

/* ContactSaveService's group deletion, then the snackbar with UNDO (§5.1.8). The snackbar cannot say when it has
 * gone without its action being taken, so the undo is kept here until the next deletion replaces it. */
static void
delete_label (People *people)
{
  forget_undo (people);
  people->undo_label = g_strdup (people->label);
  people->undo_members = ct_store_delete_label (people->store, people->label);
  show_view (people, VIEW_ALL, NULL);
  ct_window_snackbar (people->window, _("Label deleted"), _("Undo"), on_undo_delete, people);
}

static void
on_delete_label_confirmed (int response, gpointer data)
{
  if (response == CT_DIALOG_OK)
    delete_label (data);
}

static void
on_delete_label (People *people)
{
  g_autoptr(GPtrArray) members = ct_store_label_members (people->store, people->label);
  g_autofree char *message = NULL;

  /* an empty label goes at once (GroupMembersFragment :593-603) */
  if (!members->len)
    {
      g_autofree char *gone = g_strdup (people->label);

      ct_store_delete_label (people->store, gone);
      show_view (people, VIEW_ALL, NULL);
      return;
    }
  message = g_strdup_printf (_("Delete the label \"%s\"? (Contacts themselves will not be deleted.)"),
                             people->label);
  ct_confirm_dialog (people->page, NULL, message, _("OK"), on_delete_label_confirmed, people);
}

static void
on_removed_from_label (People *people)
{
  g_autoptr(GPtrArray) uids = selected_uids (people);

  ct_store_remove_from_label (people->store, uids, people->label);
  stop_selection (people);
  ct_window_toast (people->window, _("Removed from label"));
}

typedef enum {
  MENU_SEND_EMAIL,
  MENU_SEND_MESSAGE,
  MENU_REMOVE_CONTACTS,
  MENU_RENAME,
  MENU_DELETE,
  MENU_REMOVE_FROM_LABEL,
} MenuItem;

typedef struct {
  People *people;
  MenuItem items[6];
  int n;
} Menu;

static void
on_menu_chosen (int index, gpointer data)
{
  Menu *menu = data;
  People *people = menu->people;

  switch (menu->items[index])
    {
    case MENU_SEND_EMAIL:
      send_to (people, CT_KIND_EMAIL);
      break;
    case MENU_SEND_MESSAGE:
      send_to (people, CT_KIND_PHONE);
      break;
    case MENU_REMOVE_CONTACTS:
      start_selection (people, NULL, TRUE);
      break;
    case MENU_RENAME:
      ct_text_dialog (people->page, _("Rename label"), people->label, _("Label name"), 40, label_exists, on_renamed,
                      people);
      break;
    case MENU_DELETE:
      on_delete_label (people);
      break;
    case MENU_REMOVE_FROM_LABEL:
      on_removed_from_label (people);
      break;
    }
}

/* view_group.xml, by GroupMembersFragment's rules (§5.1.3). Send email shows only when something takes mailto:
 * links, which this phone may not have. */
static void
on_overflow (NeoPressable *button, People *people)
{
  static Menu menu;
  const char *labels[6];
  g_autoptr(GPtrArray) members = ct_store_label_members (people->store, people->label);
  gboolean has_members = members->len > 0;

  menu.people = people;
  menu.n = 0;
  if (!people->editing && (has_members || people->selecting))
    {
      if (ct_can_open_scheme ("mailto"))
        {
          labels[menu.n] = _("Send email");
          menu.items[menu.n++] = MENU_SEND_EMAIL;
        }
      labels[menu.n] = _("Send message");
      menu.items[menu.n++] = MENU_SEND_MESSAGE;
    }
  if (!people->editing && !people->selecting && has_members)
    {
      labels[menu.n] = _("Remove contacts");
      menu.items[menu.n++] = MENU_REMOVE_CONTACTS;
    }
  if (!people->selecting)
    {
      labels[menu.n] = _("Rename label");
      menu.items[menu.n++] = MENU_RENAME;
      labels[menu.n] = _("Delete label");
      menu.items[menu.n++] = MENU_DELETE;
    }
  if (people->selecting && !people->editing)
    {
      labels[menu.n] = _("Remove from label");
      menu.items[menu.n++] = MENU_REMOVE_FROM_LABEL;
    }
  if (menu.n)
    ct_popup_show (GTK_WIDGET (button), labels, menu.n, CT_POPUP_OVER_END, 0, 0, 0, on_menu_chosen, &menu);
}

/* ===== the toolbar ============================================================================================= */

static void
update_toolbar (People *people)
{
  gboolean label = people->view == VIEW_LABEL;
  guint count = people->selected ? g_hash_table_size (people->selected) : 0;

  /* a label's toolbar is neutral grey, not the app's teal (group_primary_color, §5.1.2) */
  if (label)
    gtk_widget_add_css_class (people->frame, "label-view");
  else
    gtk_widget_remove_css_class (people->frame, "label-view");
  if (label)
    gtk_label_set_label (GTK_LABEL (people->title), people->label);
  else if (people->view == VIEW_ACCOUNT)
    {
      CtBook *book = ct_store_get_book (people->store, people->account);

      gtk_label_set_label (GTK_LABEL (people->title), book ? ct_book_label (book) : _("Contacts"));
    }
  else
    gtk_label_set_label (GTK_LABEL (people->title), _("Contacts"));
  gtk_widget_set_visible (people->search_action, !label);
  gtk_widget_set_visible (people->add_member, label);
  gtk_widget_set_visible (people->overflow, label);

  /* the selection bar: the count (none while 0, and "Remove contacts" in edit mode), share and delete in the
   * list, the label's menu in a label */
  if (people->editing)
    gtk_label_set_label (GTK_LABEL (people->selection_count), _("Remove contacts"));
  else
    {
      g_autofree char *text = g_strdup_printf ("%u", count);

      gtk_label_set_label (GTK_LABEL (people->selection_count), text);
    }
  gtk_widget_set_visible (people->selection_count, people->editing || count > 0);
  gtk_widget_set_visible (people->share, !label && count > 0);
  gtk_widget_set_visible (people->delete, !label && count > 0);
  gtk_widget_set_visible (people->selection_overflow, label && !people->editing && count > 0);
}

static void
on_nav (NeoPressable *button, People *people)
{
  open_drawer (people, TRUE);
}

static GtkWidget *
make_toolbar (People *people)
{
  GtkWidget *frame = gtk_overlay_new ();
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *search = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *selection = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *back, *end;

  people->frame = frame;
  gtk_widget_add_css_class (frame, "ct-people-toolbar");

  /* the toolbar: the drawer button (a 56 dp target), the title 72 dp in, the actions at the end (§1.2) */
  gtk_widget_add_css_class (bar, "ct-toolbar");
  people->nav = ct_icon_button ("ct-menu-symbolic", _("Open navigation drawer"), CT_TOOLBAR_NAV_W_DP,
                                CT_TOOLBAR_H_DP, CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (people->nav, "ct-toolbar-nav");
  g_signal_connect (people->nav, "clicked", G_CALLBACK (on_nav), people);
  gtk_box_append (GTK_BOX (bar), people->nav);
  people->title = ct_label (_("Contacts"), "ct-toolbar-title", CT_TOOLBAR_TITLE_SP);
  gtk_label_set_ellipsize (GTK_LABEL (people->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (people->title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (people->title, TRUE);
  gtk_box_append (GTK_BOX (bar), people->title);
  /* the action buttons: 56 dp wide (16 dp padding around the icon), 48 dp tall, centred in the bar */
  people->search_action = ct_icon_button ("ct-search-symbolic", _("Search contacts"), 56, 48, CT_RIPPLE_APPCOMPAT);
  people->add_member = ct_icon_button ("ct-person-add-symbolic", _("Add contact"), 56, 48, CT_RIPPLE_APPCOMPAT);
  people->overflow = ct_icon_button ("ct-more-vert-symbolic", _("More options"), 40, 48, CT_RIPPLE_APPCOMPAT);
  g_signal_connect (people->search_action, "clicked", G_CALLBACK (on_search_action), people);
  g_signal_connect (people->add_member, "clicked", G_CALLBACK (on_add_member), people);
  g_signal_connect (people->overflow, "clicked", G_CALLBACK (on_overflow), people);
  for (GtkWidget *w = people->search_action; w; w = w == people->search_action ? people->add_member
                                                  : w == people->add_member ? people->overflow : NULL)
    {
      gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (w, "ct-toolbar-action");
      gtk_box_append (GTK_BOX (bar), w);
    }
  gtk_overlay_set_child (GTK_OVERLAY (frame), bar);
  people->bar = bar;

  /* the search box (search_bar_expanded.xml): #1b1b1b, the back arrow, the field, the clear ✕ */
  gtk_widget_add_css_class (search, "ct-search-bar");
  back = ct_icon_button ("ct-arrow-back-symbolic", _("stop searching"), CT_SEARCH_ICON_DP, CT_TOOLBAR_H_DP,
                         CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (back, "ct-search-back");
  g_signal_connect (back, "clicked", G_CALLBACK (on_search_back), people);
  gtk_box_append (GTK_BOX (search), back);
  people->search_entry = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (people->search_entry), _("Search contacts"));
  gtk_widget_add_css_class (people->search_entry, "ct-search-entry");
  gtk_widget_set_hexpand (people->search_entry, TRUE);
  gtk_widget_set_valign (people->search_entry, GTK_ALIGN_CENTER);
  g_signal_connect (people->search_entry, "changed", G_CALLBACK (on_search_changed), people);
  gtk_box_append (GTK_BOX (search), people->search_entry);
  people->search_clear = ct_icon_button ("ct-close-symbolic", _("Clear search"), CT_SEARCH_ICON_DP,
                                         CT_TOOLBAR_H_DP, CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (people->search_clear, "ct-search-clear");
  gtk_widget_set_visible (people->search_clear, FALSE);
  g_signal_connect (people->search_clear, "clicked", G_CALLBACK (on_search_clear), people);
  gtk_box_append (GTK_BOX (search), people->search_clear);
  gtk_widget_set_visible (search, FALSE);
  gtk_widget_set_opacity (search, 0);
  gtk_overlay_add_overlay (GTK_OVERLAY (frame), search);
  people->search_bar = search;

  /* the selection bar (selection_bar.xml): #222222, ✕, the count 72 dp in, the actions */
  gtk_widget_add_css_class (selection, "ct-selection-bar");
  people->selection_close = ct_icon_button ("ct-close-symbolic", _("close"), CT_SELECTION_CLOSE_DP,
                                            CT_TOOLBAR_H_DP, CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (people->selection_close, "ct-selection-close");
  g_signal_connect (people->selection_close, "clicked", G_CALLBACK (on_selection_close), people);
  gtk_box_append (GTK_BOX (selection), people->selection_close);
  people->selection_count = ct_label ("", "ct-selection-count", CT_TOOLBAR_TITLE_SP);
  gtk_label_set_ellipsize (GTK_LABEL (people->selection_count), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (people->selection_count, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (people->selection_count, TRUE);
  gtk_box_append (GTK_BOX (selection), people->selection_count);
  end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign (end, GTK_ALIGN_END);
  people->share = ct_icon_button ("ct-share-symbolic", _("Share"), 56, 48, CT_RIPPLE_APPCOMPAT);
  people->delete = ct_icon_button ("ct-delete-symbolic", _("Delete"), 56, 48, CT_RIPPLE_APPCOMPAT);
  people->selection_overflow = ct_icon_button ("ct-more-vert-symbolic", _("More options"), 40, 48,
                                               CT_RIPPLE_APPCOMPAT);
  g_signal_connect (people->share, "clicked", G_CALLBACK (on_share), people);
  g_signal_connect (people->delete, "clicked", G_CALLBACK (on_delete), people);
  g_signal_connect (people->selection_overflow, "clicked", G_CALLBACK (on_overflow), people);
  gtk_widget_add_css_class (people->selection_overflow, "ct-selection-overflow");
  gtk_box_append (GTK_BOX (end), people->share);
  gtk_box_append (GTK_BOX (end), people->delete);
  gtk_box_append (GTK_BOX (end), people->selection_overflow);
  for (GtkWidget *w = gtk_widget_get_first_child (end); w; w = gtk_widget_get_next_sibling (w))
    {
      gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (w, "ct-selection-action");
    }
  gtk_box_append (GTK_BOX (selection), end);
  gtk_widget_set_visible (selection, FALSE);
  gtk_widget_set_opacity (selection, 0);
  gtk_overlay_add_overlay (GTK_OVERLAY (frame), selection);
  people->selection_bar = selection;
  return frame;
}

/* ===== the empty states (§1.9, §5.1.4) ========================================================================= */

static void
on_add_contact (NeoPressable *button, People *people)
{
  ct_window_new_contact (people->window, NULL);
}

/* an illustration, a line under it and a borderless button, centred on #272727; the picture's top is placed in
 * proportion to the screen's height (place_empty_pictures) */
static GtkWidget *
empty_view (const char *image, double image_width, double image_height, const char *text, const char *button_label,
            GCallback clicked, People *people)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *picture = gtk_picture_new_for_resource (image);
  GtkWidget *label = ct_label (text, "ct-empty-text", CT_EMPTY_TEXT_SP);
  GtkWidget *button = ct_text_button (button_label, "ct-empty-button");

  gtk_widget_add_css_class (box, "ct-empty");
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  gtk_widget_set_size_request (picture, (int) image_width, (int) image_height);
  gtk_widget_set_halign (picture, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target (picture, FALSE);
  gtk_box_append (GTK_BOX (box), picture);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
  g_signal_connect (button, "clicked", clicked, people);
  gtk_box_append (GTK_BOX (box), button);
  g_object_set_data (G_OBJECT (box), "ct-picture", picture);
  gtk_widget_set_visible (box, FALSE);
  return box;
}

/* The illustrations' tops, from the top of the empty view, as fractions of heightPixels (the screen: this window
 * and the status bar above it): home_empty at h / 2 − 210 dp, accounts_empty at h / 4 + 30 dp, label_empty at
 * h / 6 (DefaultContactBrowseListFragment :452-492, GroupMembersFragment :756-782) */
static void
place_empty_pictures (People *people)
{
  double screen = gtk_widget_get_height (people->page) + CT_STATUSBAR_H_PX / (double) CT_PX_PER_DP;

  gtk_widget_set_margin_top (g_object_get_data (G_OBJECT (people->empty), "ct-picture"),
                             (int) MAX (screen / 2 - CT_EMPTY_IMAGE_OFFSET_DP, 0));
  gtk_widget_set_margin_top (g_object_get_data (G_OBJECT (people->account_empty), "ct-picture"),
                             (int) (screen / 4 + CT_EMPTY_ACCOUNT_OFFSET_DP));
  gtk_widget_set_margin_top (g_object_get_data (G_OBJECT (people->label_empty), "ct-picture"),
                             (int) (screen / 6));
}

static void
on_page_resized (CtPage *page, int width, int height, People *people)
{
  place_empty_pictures (people);
}

/* ===== the drawer (§1.7) ======================================================================================= */

#define DRAWER_W CT_DRAWER_W_DP

static void
place_drawer (People *people)
{
  gtk_widget_set_visible (people->drawer, people->drawer_open > 0);
  /* the scrim darkens with the sheet's way in (DrawerLayout's scrim × the slide offset) */
  gtk_widget_set_opacity (people->drawer_scrim, people->drawer_open);
  /* the sheet slides in from the start edge: CtDrawer places it by this offset */
  g_object_set_data (G_OBJECT (people->drawer_sheet), "ct-offset",
                     GINT_TO_POINTER ((int) round (-DRAWER_W * (1 - people->drawer_open))));
  gtk_widget_queue_allocate (people->drawer);
  update_back_marker (people);
}

static void
on_drawer_anim (NeoAnim *anim, double value, gpointer data)
{
  People *people = data;

  people->drawer_open = value;
  place_drawer (people);
}

/* ViewDragHelper's settle: (fraction to go + 1) x 256 ms, at most 600, quintic out */
static void
open_drawer (People *people, gboolean open)
{
  double to = open ? 1 : 0;
  double distance = fabs (to - people->drawer_open);

  if (distance <= 0 && !neo_anim_is_running (people->drawer_anim))
    return;
  if (open)
    {
      stop_search (people);
      stop_selection (people);
      ct_popup_dismiss (people->page);
    }
  neo_anim_start (people->drawer_anim, people->drawer_open, to,
                  (guint) MIN ((distance + 1) * CT_DRAWER_SETTLE_BASE_MS, CT_DRAWER_SETTLE_MAX_MS), 0,
                  SPONGY_DECELERATE_QUINT);
}

static void
on_scrim_pressed (GtkGestureClick *gesture, int n, double x, double y, People *people)
{
  open_drawer (people, FALSE);
}

typedef enum {
  ITEM_MY_INFO,
  ITEM_CONTACTS,
  ITEM_LABEL,
  ITEM_CREATE_LABEL,
  ITEM_ACCOUNT,
  ITEM_SETTINGS,
  ITEM_MNV_LIST,
  ITEM_MINERVA,
} ItemKind;

static void
on_label_created (const char *text, gpointer data)
{
  People *people = data;

  if (!text)
    return;
  ct_store_create_label (people->store, text);
  ct_window_toast (people->window, _("Label created"));
  show_view (people, VIEW_LABEL, text);
}

static void
on_drawer_item (NeoPressable *row, People *people)
{
  ItemKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-kind"));
  const char *name = g_object_get_data (G_OBJECT (row), "ct-name");
  EContact *profile;

  open_drawer (people, FALSE);
  switch (kind)
    {
    case ITEM_MY_INFO:
      /* the profile's card, or the editor to set one up */
      profile = ct_store_get_profile (people->store);
      if (profile)
        ct_window_show_contact (people->window, CT_PROFILE_UID);
      else
        ct_window_edit_contact (people->window, CT_PROFILE_UID);
      break;
    case ITEM_CONTACTS:
      show_view (people, VIEW_ALL, NULL);
      break;
    case ITEM_LABEL:
      show_view (people, VIEW_LABEL, name);
      break;
    case ITEM_CREATE_LABEL:
      ct_text_dialog (people->page, _("Create label"), NULL, _("Label name"), 40, label_exists, on_label_created,
                      people);
      break;
    case ITEM_ACCOUNT:
      show_view (people, VIEW_ACCOUNT, name);
      break;
    case ITEM_SETTINGS:
      ct_window_push (people->window, ct_settings_new (people->window));
      break;
    case ITEM_MNV_LIST:
      ct_window_push (people->window, ct_mnv_list_new (people->window, name));
      break;
    case ITEM_MINERVA:
      ct_window_push (people->window, ct_minerva_settings_new (people->window));
      break;
    }
}

/* drawer_item / drawer_primary_item: 48 dp, the 32 dp icon box 16 dp in, the title 32 dp after it (14 sp medium
 * bold); the row of the view that shows is activated (#2c2c2c, the title #81b5bc, the icon #edf9fb) */
static GtkWidget *
drawer_item (People *people, ItemKind kind, const char *icon, const char *title, const char *name, gboolean active,
             gboolean primary)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *image = gtk_image_new_from_icon_name (icon);
  GtkWidget *label = ct_label (title, "ct-drawer-title", CT_DRAWER_TEXT_SP);
  GtkWidget *row = neo_pressable_new (box);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_DRAWER);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  gtk_widget_add_css_class (image, "ct-drawer-icon");
  gtk_box_append (GTK_BOX (box), image);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_add_css_class (row, "ct-drawer-item");
  if (primary)
    gtk_widget_add_css_class (row, "primary");
  if (active)
    gtk_widget_add_css_class (row, "active");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  g_object_set_data (G_OBJECT (row), "ct-kind", GINT_TO_POINTER (kind));
  g_object_set_data_full (G_OBJECT (row), "ct-name", g_strdup (name), g_free);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_drawer_item), people);
  return row;
}

static GtkWidget *
drawer_divider (void)
{
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (frame, "ct-drawer-divider");
  /* the 1 dp line in the middle of the 16 dp frame, by its margins (not vexpand, which would hand the drawer's
   * spare height to every divider) */
  gtk_widget_add_css_class (line, "ct-drawer-divider-line");
  gtk_box_append (GTK_BOX (frame), line);
  return frame;
}

/* drawer_header: a divider, then the heading (14 sp medium #979797, 12 dp above and below) */
static void
drawer_header (GtkWidget *items, const char *title)
{
  GtkWidget *label = ct_label (title, "ct-drawer-header", CT_DRAWER_HEADER_SP);

  gtk_box_append (GTK_BOX (items), drawer_divider ());
  gtk_box_append (GTK_BOX (items), label);
}

static GtkWidget *
my_info_item (People *people)
{
  EContact *profile = ct_store_get_profile (people->store);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = neo_pressable_new (box);
  GtkWidget *icon;
  GtkWidget *title = ct_label (_("My info"), "ct-drawer-title", CT_DRAWER_TEXT_SP);
  g_autofree char *name = profile ? ct_contact_display_name (profile, ct_display_order (people->settings)) : NULL;
  GtkWidget *summary = ct_label (!profile ? _("Set up your profile") : *name ? name : _("(No name)"),
                                 "ct-drawer-summary", CT_DRAWER_TEXT_SP);
  EContactPhoto *photo = profile ? e_contact_get (profile, E_CONTACT_PHOTO) : NULL;
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_DRAWER);

  /* the profile's photo fit in the 32 dp box, else quantum_ic_drawer_my_info_32 */
  if (photo && photo->type == E_CONTACT_PHOTO_TYPE_INLINED && photo->data.inlined.length)
    {
      g_autoptr(GBytes) bytes = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);
      g_autoptr(GdkTexture) texture = ct_avatar_photo_now (bytes);

      icon = texture ? gtk_image_new_from_paintable (GDK_PAINTABLE (texture)) : NULL;
      if (icon)
        gtk_widget_add_css_class (icon, "ct-drawer-photo");
    }
  else
    icon = NULL;
  if (photo)
    e_contact_photo_free (photo);
  if (!icon)
    icon = gtk_image_new_from_icon_name ("ct-my-info");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 32);
  gtk_widget_add_css_class (icon, "ct-drawer-my-info-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize (GTK_LABEL (summary), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (lines), title);
  gtk_box_append (GTK_BOX (lines), summary);
  gtk_widget_set_valign (lines, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (lines, TRUE);
  gtk_widget_add_css_class (lines, "ct-drawer-lines");
  gtk_box_append (GTK_BOX (box), lines);
  gtk_widget_add_css_class (row, "ct-drawer-item");
  gtk_widget_add_css_class (row, "my-info");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  g_object_set_data (G_OBJECT (row), "ct-kind", GINT_TO_POINTER (ITEM_MY_INFO));
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, _("My info"), -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_drawer_item), people);
  return row;
}

/* DrawerAdapter's order (§1.7): My info, Contacts, the labels and Create label, the accounts, Settings. Emergency
 * information and Help & feedback are not here: nothing on this phone backs them. "Accounts" lists the address
 * books when there is more than one to choose from. Every EDS address book takes categories, so "Labels" and
 * "Create label" always show. */
static void
rebuild_drawer (People *people)
{
  GtkWidget *items = people->drawer_items;
  GtkWidget *child;
  g_autoptr(GPtrArray) labels = ct_store_dup_labels (people->store);
  GPtrArray *books = ct_store_get_books (people->store);
  GtkWidget *spacer;

  while ((child = gtk_widget_get_first_child (items)))
    gtk_box_remove (GTK_BOX (items), child);
  spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (spacer, "ct-drawer-spacer");
  gtk_box_append (GTK_BOX (items), spacer);
  /* "My info" is EDS's self contact; without EDS's settings schema there is none to have */
  if (ct_store_can_have_profile (people->store))
    {
      gtk_box_append (GTK_BOX (items), my_info_item (people));
      gtk_box_append (GTK_BOX (items), drawer_divider ());
    }
  gtk_box_append (GTK_BOX (items), drawer_item (people, ITEM_CONTACTS, "ct-account-circle-symbolic", _("Contacts"),
                                                NULL, people->view == VIEW_ALL, TRUE));
  drawer_header (items, _("Labels"));
  for (guint i = 0; i < labels->len; i++)
    gtk_box_append (GTK_BOX (items),
                    drawer_item (people, ITEM_LABEL, "ct-label-symbolic", labels->pdata[i], labels->pdata[i],
                                 people->view == VIEW_LABEL && g_strcmp0 (people->label, labels->pdata[i]) == 0,
                                 FALSE));
  gtk_box_append (GTK_BOX (items), drawer_item (people, ITEM_CREATE_LABEL, "ct-add-symbolic", _("Create label"),
                                                NULL, FALSE, FALSE));
  if (books->len > 1)
    {
      drawer_header (items, _("Accounts"));
      for (guint i = 0; i < books->len; i++)
        {
          CtBook *book = books->pdata[i];

          gtk_box_append (GTK_BOX (items),
                          drawer_item (people, ITEM_ACCOUNT,
                                       book->is_local ? "ct-smartphone-symbolic" : "ct-account-circle-symbolic",
                                       ct_book_label (book), book->uid,
                                       people->view == VIEW_ACCOUNT && g_strcmp0 (people->account, book->uid) == 0,
                                       FALSE));
        }
    }
  /* Minerva's people that are not contacts, and her rules: only where minerva-config answered with its texts */
  if (ct_mnv_available () && ct_mnv_ui_ready ())
    {
      drawer_header (items, ct_mnv_text ("title"));
      JsonArray *lists = ct_mnv_list ("lists");

      for (guint i = 0; lists && i < json_array_get_length (lists); i++)
        {
          JsonObject *def = json_array_get_object_element (lists, i);

          gtk_box_append (GTK_BOX (items), drawer_item (people, ITEM_MNV_LIST, ns_json_str (def, "icon"),
                                                        ns_json_str (def, "title"), ns_json_str (def, "id"), FALSE,
                                                        FALSE));
        }
      gtk_box_append (GTK_BOX (items), drawer_item (people, ITEM_MINERVA, "preferences-system-symbolic",
                                                    ct_mnv_text ("settings_title"), NULL, FALSE, FALSE));
    }
  gtk_box_append (GTK_BOX (items), drawer_divider ());
  gtk_box_append (GTK_BOX (items), drawer_item (people, ITEM_SETTINGS, "ct-settings-symbolic", _("Settings"), NULL,
                                                FALSE, TRUE));
  spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (spacer, "ct-drawer-spacer");
  gtk_box_append (GTK_BOX (items), spacer);
}

static void
on_profile_changed (CtStore *store, People *people)
{
  rebuild_drawer (people);
}

/* `people ui` answered: the Minerva entries can show */
static void
on_mnv_ui (JsonObject *root, const char *err, gpointer data)
{
  if (!err)
    rebuild_drawer (data);
}

/* the drawer's layer: the sheet is placed by its offset (CtDrawer below), the scrim fills the rest */

#define CT_TYPE_DRAWER (ct_drawer_get_type ())
G_DECLARE_FINAL_TYPE (CtDrawer, ct_drawer, CT, DRAWER, GtkWidget)

struct _CtDrawer {
  GtkWidget parent;
};

G_DEFINE_FINAL_TYPE (CtDrawer, ct_drawer, GTK_TYPE_WIDGET)

static void
ct_drawer_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                   int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
ct_drawer_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  GtkWidget *scrim = gtk_widget_get_first_child (widget);
  GtkWidget *sheet = gtk_widget_get_next_sibling (scrim);
  int offset = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (sheet), "ct-offset"));

  gtk_widget_allocate (scrim, width, height, -1, NULL);
  gtk_widget_allocate (sheet, (int) MIN (DRAWER_W, width), height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) offset, 0)));
}

static void
ct_drawer_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (ct_drawer_parent_class)->dispose (object);
}

static void
ct_drawer_class_init (CtDrawerClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ct_drawer_dispose;
  GTK_WIDGET_CLASS (klass)->measure = ct_drawer_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = ct_drawer_size_allocate;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-drawer");
  /* an open drawer is a dialog to the shell's back gesture, which then sends Escape (extension/a11y.js) */
  gtk_widget_class_set_accessible_role (GTK_WIDGET_CLASS (klass), GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
ct_drawer_init (CtDrawer *self)
{
}

/* the sheet follows a drag, once past the slop; let go, it settles open past 30 %, else closed */
static void
on_sheet_drag_begin (GtkGestureDrag *gesture, double x, double y, People *people)
{
  people->drawer_drag_from = people->drawer_open;
}

static void
on_sheet_drag_update (GtkGestureDrag *gesture, double dx, double dy, People *people)
{
  if (fabs (dx) < CT_TOUCH_SLOP_DP && gtk_gesture_get_sequence_state (GTK_GESTURE (gesture),
                                                                      gtk_gesture_single_get_current_sequence (
                                                                        GTK_GESTURE_SINGLE (gesture)))
                                      != GTK_EVENT_SEQUENCE_CLAIMED)
    return;
  if (fabs (dy) > fabs (dx) && fabs (dx) < CT_TOUCH_SLOP_DP)
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  neo_anim_stop (people->drawer_anim);
  people->drawer_open = CLAMP (people->drawer_drag_from + dx / DRAWER_W, 0, 1);
  place_drawer (people);
}

static void
on_sheet_drag_end (GtkGestureDrag *gesture, double dx, double dy, People *people)
{
  if (people->drawer_open > 0 && people->drawer_open < 1)
    open_drawer (people, people->drawer_open > 0.5);
}

static GtkWidget *
make_drawer (People *people)
{
  GtkWidget *layer = g_object_new (CT_TYPE_DRAWER, NULL);
  GtkWidget *scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sheet = spongy_scroll_view_new ();
  GtkWidget *items = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkGesture *press = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();

  gtk_widget_add_css_class (scrim, "ct-drawer-scrim");
  g_signal_connect (press, "pressed", G_CALLBACK (on_scrim_pressed), people);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_widget_set_parent (scrim, layer);
  gtk_widget_add_css_class (sheet, "ct-drawer-sheet");
  gtk_widget_add_css_class (items, "ct-drawer-items");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (sheet), items);
  gtk_widget_set_parent (sheet, layer);
  /* the drag in the capture phase, so that a horizontal drag starting on a row closes the drawer; it claims only a
   * horizontal one, which leaves taps and the vertical scroll to the rows and the list */
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_sheet_drag_begin), people);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_sheet_drag_update), people);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_sheet_drag_end), people);
  gtk_widget_add_controller (layer, GTK_EVENT_CONTROLLER (drag));
  gtk_accessible_update_property (GTK_ACCESSIBLE (layer), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Contacts"), -1);
  gtk_widget_set_visible (layer, FALSE);
  people->drawer_scrim = scrim;
  people->drawer_sheet = sheet;
  people->drawer_items = items;
  return layer;
}

/* an edge swipe from the start opens it (DrawerLayout's 20 dp edge), only on the list itself */
static void
on_edge_drag_begin (GtkGestureDrag *gesture, double x, double y, People *people)
{
  if (x > CT_DRAWER_EDGE_DP || people->searching || people->selecting
      || ct_window_top (people->window) != people->page)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  people->drawer_drag_from = 0;
}

static void
on_edge_drag_update (GtkGestureDrag *gesture, double dx, double dy, People *people)
{
  if (dx < CT_TOUCH_SLOP_DP && people->drawer_open == 0)
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  people->drawer_open = CLAMP (dx / DRAWER_W, 0, 1);
  place_drawer (people);
}

static void
on_edge_drag_end (GtkGestureDrag *gesture, double dx, double dy, People *people)
{
  if (people->drawer_open > 0)
    open_drawer (people, people->drawer_open > 0.3);
}

/* ===== the page ================================================================================================ */

gboolean
ct_people_back (GtkWidget *page)
{
  People *people = people_of (page);

  if (people->drawer_open > 0)
    open_drawer (people, FALSE);
  else if (people->selecting)
    stop_selection (people);
  else if (people->searching)
    stop_search (people);
  else if (people->view != VIEW_ALL)
    show_view (people, VIEW_ALL, NULL);
  else
    return FALSE;
  return TRUE;
}

void
ct_people_reset (GtkWidget *page)
{
  People *people = people_of (page);

  if (people->drawer_open > 0)
    {
      neo_anim_stop (people->drawer_anim);
      people->drawer_open = 0;
      place_drawer (people);
    }
  if (people->view != VIEW_ALL || people->searching || people->selecting)
    show_view (people, VIEW_ALL, NULL);
}

static void
on_fab (NeoPressable *fab, People *people)
{
  ct_window_new_contact (people->window, NULL);
}

static void
people_free (gpointer data)
{
  People *people = data;

  g_signal_handlers_disconnect_by_data (people->store, people);
  g_signal_handlers_disconnect_by_data (people->settings, people);
  g_clear_pointer (&people->search_anim, neo_anim_free);
  g_clear_pointer (&people->selection_anim, neo_anim_free);
  g_clear_pointer (&people->fab_anim, neo_anim_free);
  g_clear_pointer (&people->drawer_anim, neo_anim_free);
  g_clear_pointer (&people->selected, g_hash_table_unref);
  forget_undo (people);
  g_free (people->label);
  g_free (people->account);
  g_object_unref (people->store);
  g_object_unref (people->settings);
  g_free (people);
}

GtkWidget *
ct_people_new (GtkWidget *window)
{
  People *people = g_new0 (People, 1);
  GtkWidget *page = ct_page_new (window);
  GtkWidget *root = gtk_overlay_new ();
  GtkWidget *main = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *plus = gtk_image_new_from_icon_name ("ct-add-symbolic");
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkGesture *edge = gtk_gesture_drag_new ();
  GdkRGBA fab_fill = ct_rgba (CT_C_FAB), fab_ripple = ct_rgba (CT_RIPPLE_FAB);
  GtkWidget *header_box;

  people->page = page;
  people->window = window;
  people->store = g_object_ref (ct_window_get_store (window));
  people->settings = g_object_ref (ct_window_get_settings (window));
  people->selected = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  people->search_anim = neo_anim_new (page, on_search_anim, people);
  people->selection_anim = neo_anim_new (page, on_selection_anim, people);
  people->drawer_anim = neo_anim_new (page, on_drawer_anim, people);
  g_object_set_data_full (G_OBJECT (page), "ct-people", people, people_free);
  gtk_widget_add_css_class (page, "ct-people");

  gtk_box_append (GTK_BOX (main), make_toolbar (people));

  /* the account / label header: "N contacts", 48 dp (account_filter_header.xml) */
  header_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (header_box, "ct-list-header");
  people->header_icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (people->header_icon), 24);
  gtk_widget_add_css_class (people->header_icon, "ct-list-header-icon");
  gtk_box_append (GTK_BOX (header_box), people->header_icon);
  people->header_text = ct_label ("", "ct-list-header-text", 14);
  gtk_label_set_ellipsize (GTK_LABEL (people->header_text), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (people->header_text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (people->header_text, TRUE);
  gtk_box_append (GTK_BOX (header_box), people->header_text);
  gtk_widget_set_visible (header_box, FALSE);
  people->header = header_box;
  gtk_box_append (GTK_BOX (column), header_box);

  people->list = ct_list_new ();
  gtk_widget_set_vexpand (people->list, TRUE);
  g_signal_connect (people->list, "row-activated", G_CALLBACK (on_row_activated), people);
  g_signal_connect (people->list, "row-long-pressed", G_CALLBACK (on_row_long_pressed), people);
  gtk_box_append (GTK_BOX (column), people->list);
  gtk_overlay_set_child (GTK_OVERLAY (content), column);

  /* the pictures' sizes: home_empty 150 dp tall (a square), accounts_empty 96 dp, label_empty 150 dp tall and as
   * wide as its 696 x 540 aspect makes it */
  people->empty = empty_view ("/de/yesman/Contacts/images/home_empty.png", CT_EMPTY_IMAGE_H_DP, CT_EMPTY_IMAGE_H_DP,
                              _("Your contacts list is empty"), _("Add contact"), G_CALLBACK (on_add_contact), people);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), people->empty);
  people->account_empty = empty_view ("/de/yesman/Contacts/images/accounts_empty.png", CT_EMPTY_ACCOUNT_IMAGE_DP,
                                      CT_EMPTY_ACCOUNT_IMAGE_DP, _("No contacts in this account"), _("Add contact"),
                                      G_CALLBACK (on_add_contact), people);
  gtk_widget_add_css_class (people->account_empty, "account");
  gtk_overlay_add_overlay (GTK_OVERLAY (content), people->account_empty);
  people->label_empty = empty_view ("/de/yesman/Contacts/images/label_empty.png", CT_EMPTY_IMAGE_H_DP * 696 / 540,
                                    CT_EMPTY_IMAGE_H_DP, _("No contacts with this label"), _("Add contacts"),
                                    G_CALLBACK (on_add_contacts_button), people);
  gtk_widget_add_css_class (people->label_empty, "label");
  gtk_overlay_add_overlay (GTK_OVERLAY (content), people->label_empty);
  people->no_results = ct_label (_("No contacts"), "ct-no-results", CT_SEARCH_NO_RESULTS_SP);
  gtk_widget_set_halign (people->no_results, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (people->no_results, GTK_ALIGN_START);
  gtk_widget_set_can_target (people->no_results, FALSE);
  gtk_widget_set_visible (people->no_results, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), people->no_results);

  /* the toolbar's 2 dp shadow, over the top of the content */
  gtk_widget_add_css_class (shadow, "ct-toolbar-shadow");
  gtk_widget_set_valign (shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (shadow, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), shadow);

  /* the FAB: 56 dp, #00383e, the white plus, 16 dp from the end and the bottom (§1.6) */
  gtk_image_set_pixel_size (GTK_IMAGE (plus), 24);
  people->fab = neo_pressable_new (plus);
  gtk_widget_add_css_class (people->fab, "ct-fab");
  neo_pressable_set_fill (NEO_PRESSABLE (people->fab), &fab_fill, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (people->fab), NEO_RIPPLE_CIRCLE, CT_FAB_DP / 2);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (people->fab), &fab_ripple);
  gtk_widget_set_halign (people->fab, GTK_ALIGN_END);
  gtk_widget_set_valign (people->fab, GTK_ALIGN_END);
  gtk_accessible_update_property (GTK_ACCESSIBLE (people->fab), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Create new contact"), -1);
  g_signal_connect (people->fab, "clicked", G_CALLBACK (on_fab), people);
  people->fab_anim = neo_anim_new (people->fab, on_fab_anim, people);
  people->fab_shown = TRUE;
  gtk_overlay_add_overlay (GTK_OVERLAY (content), people->fab);
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (main), content);
  gtk_overlay_set_child (GTK_OVERLAY (root), main);

  /* the marker the back gesture sees while Back has something to leave (has_back) */
  people->back_marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (people->back_marker), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Contacts"), -1);
  gtk_widget_set_can_target (people->back_marker, FALSE);
  gtk_widget_set_visible (people->back_marker, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), people->back_marker);

  people->drawer = make_drawer (people);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), people->drawer);
  gtk_widget_set_vexpand (root, TRUE);
  gtk_box_append (GTK_BOX (page), root);

  g_signal_connect (edge, "drag-begin", G_CALLBACK (on_edge_drag_begin), people);
  g_signal_connect (edge, "drag-update", G_CALLBACK (on_edge_drag_update), people);
  g_signal_connect (edge, "drag-end", G_CALLBACK (on_edge_drag_end), people);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (edge), GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (main, GTK_EVENT_CONTROLLER (edge));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), people);
  gtk_widget_add_controller (page, keys);
  g_signal_connect (page, "resized", G_CALLBACK (on_page_resized), people);

  g_signal_connect (people->store, "changed", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->store, "books-changed", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->store, "ready", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->store, "profile-changed", G_CALLBACK (on_profile_changed), people);
  g_signal_connect (people->settings, "changed", G_CALLBACK (on_settings_changed), people);
  update_toolbar (people);
  rebuild_drawer (people);
  place_drawer (people);
  refresh (people, TRUE);
  if (ct_mnv_available ())
    ct_mnv_ui_load (G_OBJECT (page), on_mnv_ui, people);
  return page;
}
