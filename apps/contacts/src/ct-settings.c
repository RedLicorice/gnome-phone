/* Settings (ct-settings.h, spec §5.2 to §5.4).
 *
 * The rows, in preference_display_options.xml's order: My info, Accounts, Default account for new contacts,
 * Contacts to display, Sort by, Name format, Phonetic name, Import, Export, About Contacts. Each is a framework
 * preference row: 16 dp in, 16 dp above and below the text, the title 16 sp #dbdbdb and the summary 14 sp #959595.
 * About Contacts takes the screen's place under the same toolbar (a fragment of the same activity); "Contacts to
 * display" and "Customize view" are activities of their own, so pages here.
 *
 * Left out, for want of anything to back them:
 *  - Blocked numbers: the screen is the Dialer's (TelecomManager's MANAGE_BLOCKED_NUMBERS) and the Dialer has no
 *    action that opens it yet.
 *  - Import from SIM: a SIM's contacts are an address book of EDS's when EDS has one for it, and then they are
 *    in the list already; the import dialog offers only the .vcf file.
 *  - Export's "Share all contacts": there is no share sheet; the .vcf export writes the same file.
 *  - About's "Open source licenses", "Privacy policy" and "Terms of service": Google's pages, about Google's app.
 *  - Accounts opens GNOME Settings' Online Accounts (where EDS's Google and CardDAV books come from) and shows
 *    only when GNOME Settings is installed. */
#include "ct-settings.h"
#include "ct-dialogs.h"
#include "ct-page.h"
#include "ct-ui.h"
#include "ct-vcard.h"
#include "ct-window.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <string.h>

#ifndef CT_VERSION
#define CT_VERSION "1.0"
#endif

/* ===== a preference row (preference_material.xml) =============================================================== */

GtkWidget *
ct_pref_row (const char *title, const char *summary, GtkWidget **summary_label, GCallback clicked, gpointer data)
{
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = neo_pressable_new (lines);
  GtkWidget *label = ct_label (summary ? summary : "", "ct-pref-summary", 14);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  gtk_widget_add_css_class (lines, "ct-pref-lines");
  gtk_box_append (GTK_BOX (lines), ct_label (title, "ct-pref-title", 16));
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_lines (GTK_LABEL (label), 10);
  gtk_widget_set_visible (label, summary && *summary);
  gtk_box_append (GTK_BOX (lines), label);
  gtk_widget_add_css_class (row, "ct-pref-row");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  if (clicked)
    g_signal_connect (row, "clicked", clicked, data);
  else
    gtk_widget_set_can_target (row, FALSE);
  if (summary_label)
    *summary_label = label;
  return row;
}

void
ct_pref_set_summary (GtkWidget *label, const char *text)
{
  gtk_label_set_label (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
}

/* ===== the Settings screen ===================================================================================== */

typedef struct {
  GtkWidget *page, *window, *title;
  CtStore *store;
  GSettings *settings;
  GtkWidget *main_rows, *about_rows;
  GtkWidget *my_info, *my_info_summary, *default_summary, *filter_summary, *sort_summary, *format_summary;
  GtkWidget *phonetic_summary, *default_row, *export_row;
} Settings;

static void
settings_free (gpointer data)
{
  Settings *s = data;

  g_signal_handlers_disconnect_by_data (s->store, s);
  g_signal_handlers_disconnect_by_data (s->settings, s);
  g_object_unref (s->store);
  g_object_unref (s->settings);
  g_free (s);
}

static GPtrArray *
writable_books (CtStore *store)
{
  GPtrArray *books = ct_store_get_books (store);
  GPtrArray *writable = g_ptr_array_new ();

  for (guint i = 0; i < books->len; i++)
    if (((CtBook *) books->pdata[i])->writable)
      g_ptr_array_add (writable, books->pdata[i]);
  return writable;
}

static void
update (Settings *s)
{
  EContact *profile = ct_store_get_profile (s->store);
  /* the default account's name only once one has been chosen (Android's is null until then) */
  g_autofree char *chosen = g_settings_get_string (s->settings, "default-account");
  CtBook *book = *chosen ? ct_store_get_book (s->store, chosen) : NULL;
  g_autofree char *filter = g_settings_get_string (s->settings, "contacts-to-display");
  g_autofree char *phonetic = g_settings_get_string (s->settings, "phonetic-name");

  if (profile)
    {
      g_autofree char *name = ct_contact_display_name (profile, ct_display_order (s->settings));

      ct_pref_set_summary (s->my_info_summary, *name ? name : _("(No name)"));
    }
  else
    ct_pref_set_summary (s->my_info_summary, _("Set up your profile"));
  gtk_widget_set_visible (s->my_info, ct_store_can_have_profile (s->store));
  ct_pref_set_summary (s->default_summary, book ? ct_book_label (book) : NULL);
  ct_pref_set_summary (s->filter_summary, g_str_equal (filter, "custom") ? _("Customized view") : _("All contacts"));
  ct_pref_set_summary (s->sort_summary, ct_sort_order (s->settings) == CT_ORDER_PRIMARY ? _("First name") : _("Last name"));
  ct_pref_set_summary (s->format_summary, ct_display_order (s->settings) == CT_ORDER_PRIMARY ? _("First name first")
                                                                                    : _("Last name first"));
  ct_pref_set_summary (s->phonetic_summary, g_str_equal (phonetic, "always") ? _("Always show") : _("Hide if empty"));
  /* Export is removed when there are no contacts to export (removeUnsupportedPreferences) */
  gtk_widget_set_visible (s->export_row, ct_store_count (s->store) > 0);
}

static void
on_store_changed (CtStore *store, Settings *s)
{
  update (s);
}

static void
on_settings_changed (GSettings *settings, const char *key, Settings *s)
{
  update (s);
}

static void
on_my_info (NeoPressable *row, Settings *s)
{
  if (ct_store_get_profile (s->store))
    ct_window_show_contact (s->window, CT_PROFILE_UID);
  else
    ct_window_edit_contact (s->window, CT_PROFILE_UID);
}

/* GNOME Settings' Online Accounts panel, through its "launch-panel" action (as GNOME Shell opens its panels) */
static void
on_accounts (NeoPressable *row, Settings *s)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());
  GVariantBuilder args;

  if (!bus)
    return;
  g_variant_builder_init (&args, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&args, "v", g_variant_new ("(sav)", "online-accounts", NULL));
  g_dbus_connection_call (bus, "org.gnome.Settings", "/org/gnome/Settings", "org.freedesktop.Application",
                          "ActivateAction",
                          g_variant_new ("(sava{sv})", "launch-panel", &args, NULL),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean
have_gnome_settings (void)
{
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new ("org.gnome.Settings.desktop");

  return info != NULL;
}

static void
on_default_chosen (int response, gpointer data)
{
  Settings *s = data;
  g_autoptr(GPtrArray) books = writable_books (s->store);

  if (response >= 0 && (guint) response < books->len)
    g_settings_set_string (s->settings, "default-account", ((CtBook *) books->pdata[response])->uid);
}

/* Android hands this to the system's account picker; here it is the same radio list over EDS's writable books */
static void
on_default_account (NeoPressable *row, Settings *s)
{
  g_autoptr(GPtrArray) books = writable_books (s->store);
  const char *current = ct_store_default_book (s->store);
  const char *labels[32];
  int selected = -1;
  guint n = MIN (books->len, 32);

  for (guint i = 0; i < n; i++)
    {
      labels[i] = ct_book_label (books->pdata[i]);
      if (g_strcmp0 (((CtBook *) books->pdata[i])->uid, current) == 0)
        selected = (int) i;
    }
  ct_radio_dialog (s->page, _("Default account for new contacts"), labels, (int) n, selected, on_default_chosen, s);
}

static void
on_filter (NeoPressable *row, Settings *s);

typedef struct {
  Settings *settings;
  const char *key;
  const char *values[2];
} Choice;

static void
on_choice (int response, gpointer data)
{
  Choice *choice = data;

  if (response == 0 || response == 1)
    g_settings_set_string (choice->settings->settings, choice->key, choice->values[response]);
  g_free (choice);
}

/* ListPreference: the title, two radio rows, a tap commits */
static void
choose (Settings *s, const char *title, const char *key, const char *value0, const char *label0,
        const char *value1, const char *label1)
{
  Choice *choice = g_new0 (Choice, 1);
  g_autofree char *current = g_settings_get_string (s->settings, key);
  const char *labels[2] = { label0, label1 };

  choice->settings = s;
  choice->key = key;
  choice->values[0] = value0;
  choice->values[1] = value1;
  ct_radio_dialog (s->page, title, labels, 2, g_str_equal (current, value0) ? 0 : 1, on_choice, choice);
}

static void
on_sort (NeoPressable *row, Settings *s)
{
  choose (s, _("Sort by"), "sort-order", "first-name", _("First name"), "last-name", _("Last name"));
}

static void
on_format (NeoPressable *row, Settings *s)
{
  choose (s, _("Name format"), "display-order", "first-name-first", _("First name first"), "last-name-first",
          _("Last name first"));
}

static void
on_phonetic (NeoPressable *row, Settings *s)
{
  choose (s, _("Phonetic name"), "phonetic-name", "always", _("Always show"), "hide-if-empty", _("Hide if empty"));
}

/* --- import (§5.4.1, §5.4.2, §5.4.4) */

typedef struct {
  GtkWidget *window;
  char *book_uid;
} ImportTarget;

static void
on_import_files (GObject *source, GAsyncResult *result, gpointer data)
{
  ImportTarget *target = data;
  g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source), result, NULL);

  for (guint i = 0; files && i < g_list_model_get_n_items (files); i++)
    {
      g_autoptr(GFile) file = g_list_model_get_item (files, i);

      g_object_set_data_full (G_OBJECT (file), "ct-book", g_strdup (target->book_uid), g_free);
      ct_import_file (target->window, file);
    }
  g_object_unref (target->window);
  g_free (target->book_uid);
  g_free (target);
}

/* ACTION_OPEN_DOCUMENT, text/x-vcard, several at once: the file chooser (the portal's, where there is one) */
static void
pick_vcf_files (GtkWidget *window, const char *book_uid)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  ImportTarget *target = g_new0 (ImportTarget, 1);

  gtk_file_filter_set_name (filter, _("vCard"));
  gtk_file_filter_add_mime_type (filter, "text/vcard");
  gtk_file_filter_add_mime_type (filter, "text/x-vcard");
  gtk_file_filter_add_suffix (filter, "vcf");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  target->window = g_object_ref (window);
  target->book_uid = g_strdup (book_uid);
  gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (window), NULL, on_import_files, target);
}

static void
on_import_account (int response, gpointer data)
{
  Settings *s = data;
  g_autoptr(GPtrArray) books = writable_books (s->store);

  if (response >= 0 && (guint) response < books->len)
    pick_vcf_files (s->window, ((CtBook *) books->pdata[response])->uid);
}

static void
on_import_source (int response, gpointer data)
{
  Settings *s = data;
  g_autoptr(GPtrArray) books = writable_books (s->store);

  if (response != 0)
    return;
  /* more than one writable account: "Save imported contacts to:" first (SelectAccountDialogFragment) */
  if (books->len > 1)
    {
      const char *labels[32];
      guint n = MIN (books->len, 32);

      for (guint i = 0; i < n; i++)
        labels[i] = ct_book_label (books->pdata[i]);
      ct_list_dialog (s->page, CT_DIALOG_APPCOMPAT, _("Save imported contacts to:"), labels, (int) n, FALSE,
                      on_import_account, s);
      return;
    }
  pick_vcf_files (s->window, ct_store_default_book (s->store));
}

static void
on_import (NeoPressable *row, Settings *s)
{
  const char *labels[] = { _(".vcf file") };

  ct_list_dialog (s->page, CT_DIALOG_FRAMEWORK, _("Import contacts from"), labels, 1, TRUE, on_import_source, s);
}

/* --- export (§5.4.5) */

static void
on_export_chosen (int response, gpointer data)
{
  Settings *s = data;
  g_autoptr(GPtrArray) entries = NULL;
  g_autoptr(GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);

  if (response != 0)
    return;
  entries = ct_shown_entries (s->store, s->settings);
  for (guint i = 0; i < entries->len; i++)
    g_ptr_array_add (uids, g_strdup (((CtEntry *) entries->pdata[i])->uid));
  ct_export_contacts (s->window, uids, _("Export contacts"));
}

static void
on_export (NeoPressable *row, Settings *s)
{
  const char *labels[] = { _("Export to .vcf file") };

  ct_list_dialog (s->page, CT_DIALOG_APPCOMPAT, _("Export contacts"), labels, 1, FALSE, on_export_chosen, s);
}

/* --- About Contacts: the rows in the screen's place, the title "About Contacts" (showAboutFragment) */

static void
show_about (Settings *s, gboolean about)
{
  gtk_widget_set_visible (s->main_rows, !about);
  gtk_widget_set_visible (s->about_rows, about);
  gtk_label_set_label (GTK_LABEL (s->title), about ? _("About Contacts") : _("Settings"));
}

static void
on_about (NeoPressable *row, Settings *s)
{
  show_about (s, TRUE);
}

static gboolean
on_back (CtPage *page, gpointer data)
{
  Settings *s = data;

  if (!gtk_widget_get_visible (s->about_rows))
    return FALSE;
  show_about (s, FALSE);
  return TRUE;
}

GtkWidget *
ct_settings_new (GtkWidget *window)
{
  Settings *s = g_new0 (Settings, 1);
  GtkWidget *page = ct_page_new (window);
  GtkWidget *scroll = spongy_scroll_view_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *rows;

  s->page = page;
  s->window = window;
  s->store = g_object_ref (ct_window_get_store (window));
  s->settings = g_object_ref (ct_window_get_settings (window));
  g_object_set_data_full (G_OBJECT (page), "ct-settings", s, settings_free);
  gtk_widget_add_css_class (page, "ct-settings");
  ct_page_set_back_handler (CT_PAGE (page), on_back, s);
  gtk_box_append (GTK_BOX (page), ct_page_toolbar (CT_PAGE (page), NULL, "ct-arrow-back-symbolic", _("Back"),
                                                   _("Settings"), NULL, &s->title));

  rows = s->main_rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  s->my_info = ct_pref_row (_("My info"), NULL, &s->my_info_summary, G_CALLBACK (on_my_info), s);
  gtk_box_append (GTK_BOX (rows), s->my_info);
  if (have_gnome_settings ())
    gtk_box_append (GTK_BOX (rows), ct_pref_row (_("Accounts"), NULL, NULL, G_CALLBACK (on_accounts), s));
  s->default_row = ct_pref_row (_("Default account for new contacts"), NULL, &s->default_summary,
                             G_CALLBACK (on_default_account), s);
  gtk_box_append (GTK_BOX (rows), s->default_row);
  gtk_box_append (GTK_BOX (rows), ct_pref_row (_("Contacts to display"), NULL, &s->filter_summary,
                                            G_CALLBACK (on_filter), s));
  gtk_box_append (GTK_BOX (rows), ct_pref_row (_("Sort by"), NULL, &s->sort_summary, G_CALLBACK (on_sort), s));
  gtk_box_append (GTK_BOX (rows), ct_pref_row (_("Name format"), NULL, &s->format_summary, G_CALLBACK (on_format), s));
  gtk_box_append (GTK_BOX (rows), ct_pref_row (_("Phonetic name"), NULL, &s->phonetic_summary,
                                            G_CALLBACK (on_phonetic), s));
  gtk_box_append (GTK_BOX (rows), ct_pref_row (_("Import"), NULL, NULL, G_CALLBACK (on_import), s));
  s->export_row = ct_pref_row (_("Export"), NULL, NULL, G_CALLBACK (on_export), s);
  gtk_box_append (GTK_BOX (rows), s->export_row);
  gtk_box_append (GTK_BOX (rows), ct_pref_row (_("About Contacts"), NULL, NULL, G_CALLBACK (on_about), s));
  gtk_box_append (GTK_BOX (content), rows);

  s->about_rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (s->about_rows), ct_pref_row (_("Build version"), CT_VERSION, NULL, NULL, NULL));
  gtk_widget_set_visible (s->about_rows, FALSE);
  gtk_box_append (GTK_BOX (content), s->about_rows);

  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), content);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), scroll);
  gtk_widget_add_css_class (shadow, "ct-toolbar-shadow");
  gtk_widget_add_css_class (shadow, "settings");
  gtk_widget_set_valign (shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (shadow, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), shadow);
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_box_append (GTK_BOX (page), overlay);

  g_signal_connect (s->store, "changed", G_CALLBACK (on_store_changed), s);
  g_signal_connect (s->store, "books-changed", G_CALLBACK (on_store_changed), s);
  g_signal_connect (s->store, "profile-changed", G_CALLBACK (on_store_changed), s);
  g_signal_connect (s->settings, "changed", G_CALLBACK (on_settings_changed), s);
  update (s);
  return page;
}

/* ===== "Contacts to display" (AccountFilterActivity, §5.3.1) and "Customize view" (§5.3.2) ====================== */

/* a filter row: the text 16 dp in (#e2e3e3), a radio or a checkbox at the end 16 dp in */
static GtkWidget *
filter_row (const char *text, GtkWidget *mark, GCallback clicked, gpointer data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *row = neo_pressable_new (box);
  GtkWidget *label = ct_label (text, "ct-filter-text", 16);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_FRAMEWORK);

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_set_valign (mark, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), mark);
  gtk_widget_add_css_class (row, "ct-filter-row");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  g_signal_connect (row, "clicked", clicked, data);
  return row;
}

static GtkWidget *
radio_new (gboolean on)
{
  GtkWidget *radio = gtk_image_new_from_icon_name (on ? "ct-radio-on-symbolic" : "ct-radio-off-symbolic");

  gtk_image_set_pixel_size (GTK_IMAGE (radio), 24);
  gtk_widget_add_css_class (radio, on ? "ct-radio-on" : "ct-radio-off");
  gtk_widget_add_css_class (radio, "ct-filter-radio");
  return radio;
}

typedef struct {
  GtkWidget *page, *window;
  CtStore *store;
  GSettings *settings;
  GtkWidget *rows;
  GHashTable *shown;            /* book uid → book uid: the books ticked */
  char *initial;
} Customize;

static void
customize_free (gpointer data)
{
  Customize *c = data;

  g_hash_table_unref (c->shown);
  g_object_unref (c->store);
  g_object_unref (c->settings);
  g_free (c->initial);
  g_free (c);
}

/* the ticked books, sorted, as one string: whether anything changed */
static char *
customize_state (Customize *c)
{
  g_autoptr(GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (c->shown);
  GString *state = g_string_new (NULL);

  g_ptr_array_sort_values (keys, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < keys->len; i++)
    g_string_append_printf (state, "%s\n", (char *) keys->pdata[i]);
  return g_string_free (state, FALSE);
}

static void
on_book_toggled (NeoPressable *row, Customize *c)
{
  const char *uid = g_object_get_data (G_OBJECT (row), "ct-book");
  GtkWidget *check = g_object_get_data (G_OBJECT (row), "ct-check");

  if (!g_hash_table_remove (c->shown, uid))
    g_hash_table_add (c->shown, g_strdup (uid));
  ct_check_set_active (check, g_hash_table_contains (c->shown, uid));
}

/* Save: the unticked books are hidden, the filter is "custom" (and the filter screen closes with it) */
static void
on_customize_save (NeoPressable *button, Customize *c)
{
  GPtrArray *books = ct_store_get_books (c->store);
  g_autoptr(GStrvBuilder) hidden = g_strv_builder_new ();
  g_auto(GStrv) strv = NULL;
  GtkWidget *under;

  for (guint i = 0; i < books->len; i++)
    if (!g_hash_table_contains (c->shown, ((CtBook *) books->pdata[i])->uid))
      g_strv_builder_add (hidden, ((CtBook *) books->pdata[i])->uid);
  strv = g_strv_builder_end (hidden);
  g_settings_set_strv (c->settings, "hidden-books", (const char *const *) strv);
  g_settings_set_string (c->settings, "contacts-to-display", "custom");
  under = g_object_get_data (G_OBJECT (c->page), "ct-filter-page");
  ct_window_pop (c->window, under ? under : c->page);
}

static void
on_discard (int response, gpointer data)
{
  Customize *c = data;

  if (response == CT_DIALOG_OK)
    ct_window_pop (c->window, c->page);
}

static gboolean
on_customize_back (CtPage *page, gpointer data)
{
  Customize *c = data;
  g_autofree char *now = customize_state (c);

  if (g_str_equal (now, c->initial))
    return FALSE;
  ct_confirm_dialog (c->page, NULL, _("Discard customizations?"), _("OK"), on_discard, c);
  return TRUE;
}

/* one row per address book with its checkbox: EDS has no groups inside a book, so a book is shown or not as a
 * whole (an account's "All contacts" row of Android's) */
static GtkWidget *
customize_new (GtkWidget *window, GtkWidget *filter_page)
{
  Customize *c = g_new0 (Customize, 1);
  GtkWidget *page = ct_page_new (window);
  GtkWidget *scroll = spongy_scroll_view_new ();
  GtkWidget *actions, *save;
  GPtrArray *books;
  g_auto(GStrv) hidden = NULL;

  c->page = page;
  c->window = window;
  c->store = g_object_ref (ct_window_get_store (window));
  c->settings = g_object_ref (ct_window_get_settings (window));
  c->shown = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full (G_OBJECT (page), "ct-customize", c, customize_free);
  g_object_set_data (G_OBJECT (page), "ct-filter-page", filter_page);
  ct_page_set_back_handler (CT_PAGE (page), on_customize_back, c);
  gtk_widget_add_css_class (page, "ct-filter");
  gtk_box_append (GTK_BOX (page), ct_page_toolbar (CT_PAGE (page), NULL, "ct-arrow-back-symbolic", _("Back"),
                                                   _("Customize view"), &actions, NULL));
  save = ct_text_button (_("Save"), "ct-toolbar-text-action");
  gtk_widget_set_valign (save, GTK_ALIGN_CENTER);
  g_signal_connect (save, "clicked", G_CALLBACK (on_customize_save), c);
  gtk_box_append (GTK_BOX (actions), save);

  c->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  hidden = g_settings_get_strv (c->settings, "hidden-books");
  books = ct_store_get_books (c->store);
  for (guint i = 0; i < books->len; i++)
    {
      CtBook *book = books->pdata[i];
      gboolean shown = !g_strv_contains ((const char *const *) hidden, book->uid);
      GtkWidget *check = ct_check_new (CT_C_CHECK_ON, CT_C_CHECK_OFF);
      GtkWidget *row = filter_row (ct_book_label (book), check, G_CALLBACK (on_book_toggled), c);

      ct_check_set_active (check, shown);
      if (shown)
        g_hash_table_add (c->shown, g_strdup (book->uid));
      g_object_set_data_full (G_OBJECT (row), "ct-book", g_strdup (book->uid), g_free);
      g_object_set_data (G_OBJECT (row), "ct-check", check);
      gtk_widget_add_css_class (row, "custom");
      gtk_box_append (GTK_BOX (c->rows), row);
    }
  c->initial = customize_state (c);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), c->rows);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  return page;
}

static void
on_filter_all (NeoPressable *row, GtkWidget *page)
{
  GtkWidget *window = ct_page_get_window (CT_PAGE (page));

  g_settings_set_string (ct_window_get_settings (window), "contacts-to-display", "all");
  ct_window_pop (window, page);
}

static void
on_filter_customize (NeoPressable *row, GtkWidget *page)
{
  GtkWidget *window = ct_page_get_window (CT_PAGE (page));

  ct_window_push (window, customize_new (window, page));
}

static void
on_filter (NeoPressable *row, Settings *s)
{
  GtkWidget *page = ct_page_new (s->window);
  GtkWidget *rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autofree char *filter = g_settings_get_string (s->settings, "contacts-to-display");
  gboolean custom = g_str_equal (filter, "custom");
  GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (page, "ct-filter");
  gtk_box_append (GTK_BOX (page), ct_page_toolbar (CT_PAGE (page), NULL, "ct-arrow-back-symbolic", _("Back"),
                                                   _("Contacts to display"), NULL, NULL));
  gtk_box_append (GTK_BOX (rows), filter_row (_("All contacts"), radio_new (!custom), G_CALLBACK (on_filter_all),
                                              page));
  gtk_box_append (GTK_BOX (rows), filter_row (_("Customize"), radio_new (custom), G_CALLBACK (on_filter_customize),
                                              page));
  gtk_box_append (GTK_BOX (page), rows);
  gtk_widget_set_vexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (page), spacer);
  /* the list's bottom divider (contact_list_filter.xml) */
  gtk_widget_add_css_class (divider, "ct-filter-divider");
  gtk_box_append (GTK_BOX (page), divider);
  ct_window_push (s->window, page);
}

/* ===== .vcf import and export ================================================================================== */

typedef struct {
  GtkWidget *window;
  char *name;
} Job;

static void
job_free (Job *job)
{
  g_object_unref (job->window);
  g_free (job->name);
  g_free (job);
}

/* "Finished importing vCard %s": a notification on Android, and here (GNotification, to whatever shows them) */
static void
notify (const char *id, const char *title)
{
  GApplication *app = g_application_get_default ();
  g_autoptr(GNotification) notification = g_notification_new (title);

  if (!app)
    return;
  g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_LOW);
  g_application_send_notification (app, id, notification);
}

static void
on_imported (const char *count, const GError *error, gpointer data)
{
  Job *job = data;

  if (error)
    ct_window_toast (job->window, _("Couldn't import vCard."));
  else
    {
      g_autofree char *title = g_strdup_printf (_("Finished importing vCard %s"), job->name);

      notify ("import", title);
    }
  job_free (job);
}

static void
on_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  Job *job = data;
  g_autofree char *text = NULL;
  gsize length = 0;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) cards = NULL;
  g_autoptr(GPtrArray) contacts = g_ptr_array_new_with_free_func (g_object_unref);
  CtStore *store = ct_window_get_store (job->window);
  const char *book = g_object_get_data (source, "ct-book");

  if (!g_file_load_contents_finish (G_FILE (source), result, &text, &length, NULL, &error))
    {
      g_warning ("import: %s", error->message);
      ct_window_toast (job->window, _("Couldn't read vCard data"));
      job_free (job);
      return;
    }
  cards = ct_vcard_parse (text, (gssize) length);
  for (guint i = 0; i < cards->len; i++)
    g_ptr_array_add (contacts, ct_vcard_for_import (cards->pdata[i]));
  if (!contacts->len)
    {
      /* nothing in the file is a vCard */
      ct_window_toast (job->window, _("Couldn't import vCard."));
      job_free (job);
      return;
    }
  ct_store_import (store, book ? book : ct_store_default_book (store), contacts, on_imported, job);
}

void
ct_import_file (GtkWidget *window, GFile *file)
{
  Job *job = g_new0 (Job, 1);
  g_autofree char *message = NULL;

  job->window = g_object_ref (window);
  job->name = g_file_get_basename (file);
  /* vcard_import_will_start_message, Toast.LENGTH_LONG */
  message = g_strdup_printf (_("%s will be imported shortly."), job->name);
  ct_window_toast (window, message);
  g_file_load_contents_async (file, NULL, on_loaded, job);
}

typedef struct {
  GtkWidget *window;
  char *text;
} Export;

static void
on_written (GObject *source, GAsyncResult *result, gpointer data)
{
  Export *export = data;
  g_autoptr(GError) error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    {
      g_warning ("export: %s", error->message);
      ct_window_toast (export->window, _("Couldn't export"));
    }
  else
    {
      g_autofree char *name = g_file_get_basename (G_FILE (source));
      g_autofree char *title = g_strdup_printf (_("Finished exporting %s."), name);

      notify ("export", title);
    }
  g_object_unref (export->window);
  g_free (export->text);
  g_free (export);
}

static void
on_export_file (GObject *source, GAsyncResult *result, gpointer data)
{
  Export *export = data;
  g_autoptr(GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, NULL);

  if (!file)
    {
      g_object_unref (export->window);
      g_free (export->text);
      g_free (export);
      return;
    }
  g_file_replace_contents_async (file, export->text, strlen (export->text), NULL, FALSE, G_FILE_CREATE_NONE, NULL,
                                 on_written, export);
}

/* ACTION_CREATE_DOCUMENT: the file chooser saves the vCards; "<name>.vcf" for one, contacts.vcf for several */
void
ct_export_contacts (GtkWidget *window, GPtrArray *uids, const char *title)
{
  CtStore *store = ct_window_get_store (window);
  GSettings *settings = ct_window_get_settings (window);
  g_autoptr(GPtrArray) contacts = g_ptr_array_new ();
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autofree char *name = NULL;
  g_autofree char *file_name = NULL;
  Export *export;

  for (guint i = 0; uids && i < uids->len; i++)
    {
      EContact *contact = NULL;

      if (g_str_equal (uids->pdata[i], CT_PROFILE_UID))
        contact = ct_store_get_profile (store);
      else
        {
          CtEntry *entry = ct_store_lookup (store, uids->pdata[i]);

          contact = entry ? entry->contact : NULL;
        }
      if (contact)
        g_ptr_array_add (contacts, contact);
    }
  if (!contacts->len)
    {
      ct_window_toast (window, _("There are no contacts to share."));
      return;
    }
  if (contacts->len == 1)
    name = ct_contact_display_name (contacts->pdata[0], ct_display_order (settings));
  file_name = ct_vcard_file_name (name);
  export = g_new0 (Export, 1);
  export->window = g_object_ref (window);
  export->text = ct_vcard_export (contacts);
  gtk_file_dialog_set_title (dialog, title);
  gtk_file_dialog_set_initial_name (dialog, file_name);
  gtk_file_dialog_save (dialog, GTK_WINDOW (window), NULL, on_export_file, export);
}
