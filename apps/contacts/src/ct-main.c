/* neo-contacts: the Contacts app, LineageOS's Contacts (com.android.contacts) rebuilt in GTK 4 (spec/contacts.md),
 * over evolution-data-server's address books.
 *
 * de.yesman.Contacts is D-Bus activatable. The launcher starts it at the list; a .vcf file opened with it is
 * imported after "Import contacts from vCard?". Other apps reach its screens through its actions, which they
 * activate with org.freedesktop.Application.ActivateAction on de.yesman.Contacts (/de/yesman/Contacts), or with
 * `gapplication action de.yesman.Contacts <action> <parameter>`:
 *
 *   view-contact    (s)       the card of the contact with that vCard UID, as EDS has it (ACTION_VIEW)
 *   edit-contact    (s)       the editor for that contact (ACTION_EDIT)
 *   insert          (a{sv})   the editor for a new contact with these fields filled in (ACTION_INSERT):
 *                             "name" (s), "phone" (s), "email" (s); the Dialer's "Create new contact"
 *   insert-or-edit  (a{sv})   the same fields, added to a contact chosen from the list, or to a new one through
 *                             the list's "Create new contact" row (ACTION_INSERT_OR_EDIT; "Add to a contact")
 *   pick-contact    (sss)     the list to pick one contact from (ACTION_PICK): the caller's application id, the
 *                             action of the caller's to activate with the answer, and what the contact must have
 *                             ("" anything, "phone" or "email"). The answer is that action, activated on the
 *                             caller the same way, with (ss): the contact's UID and its vCard, or ("", "") when
 *                             the picker was left without a pick.
 *   minerva-settings          the people's Minerva settings: trust, when strangers are forgotten (Settings opens
 *                             it from Minerva › Memoria › Fiducia; the desktop file's action of the same name)
 *
 * Calls and texts go out the other way, as tel: and sms: URIs to the apps that take them (ct-ui.c). */
#include "ct-dialogs.h"
#include "ct-editor.h"
#include "ct-minerva-pages.h"
#include "ct-picker.h"
#include "ct-settings.h"
#include "ct-store.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <neo.h>

#define CT_APP_ID "de.yesman.Contacts"

static CtStore *store;
static GSettings *settings;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/de/yesman/Contacts/contacts.css");
  /* over libneo's, which is at APPLICATION priority too: the later provider wins at equal priority */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  neo_init ();
  load_style ();
  settings = g_settings_new (CT_APP_ID);
  store = ct_store_new (settings);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  g_clear_object (&store);
  g_clear_object (&settings);
}

/* the one window, made on the first need */
static GtkWidget *
contacts_window (GApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  return window ? GTK_WIDGET (window) : ct_window_new (GTK_APPLICATION (app), store, settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (contacts_window (app)));
}

/* --- a .vcf file: "Import contacts from vCard?" (ImportVCardDialogFragment), then the import */

static void
on_import_confirmed (int response, gpointer data)
{
  GFile *file = data;

  if (response == CT_DIALOG_OK)
    ct_import_file (g_object_get_data (G_OBJECT (file), "ct-window"), file);
  g_object_unref (file);
}

static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
  GtkWidget *window = contacts_window (app);

  gtk_window_present (GTK_WINDOW (window));
  for (int i = 0; i < n_files; i++)
    {
      GFile *file = g_object_ref (files[i]);

      g_object_set_data (G_OBJECT (file), "ct-window", window);
      ct_confirm_dialog (ct_window_top (window), NULL, _("Import contacts from vCard?"), _("OK"),
                         on_import_confirmed, file);
    }
}

/* --- a contact another app names: when the app has just been started for it, the address books are still being
 * read, so its screen waits until they have been (or the contact would "not exist") */

typedef void (*ContactScreen) (GtkWidget *window, const char *uid);

typedef struct {
  GtkWidget *window;
  ContactScreen show;
  char *uid;
} Pending;

static void
on_store_ready (CtStore *ready, gpointer data)
{
  Pending *pending = data;

  g_signal_handlers_disconnect_by_data (ready, pending);
  pending->show (pending->window, pending->uid);
  g_object_unref (pending->window);
  g_free (pending->uid);
  g_free (pending);
}

static void
when_ready (GtkWidget *window, ContactScreen show, const char *uid)
{
  Pending *pending;

  if (ct_store_is_ready (store))
    {
      show (window, uid);
      return;
    }
  pending = g_new0 (Pending, 1);
  pending->window = g_object_ref (window);
  pending->show = show;
  pending->uid = g_strdup (uid);
  g_signal_connect (store, "ready", G_CALLBACK (on_store_ready), pending);
}

/* ContactEditorActivity with a contact that is not there: "Failed to open editor." and nothing more */
static void
edit_contact (GtkWidget *window, const char *uid)
{
  if (!g_str_equal (uid, CT_PROFILE_UID) && !ct_store_lookup (store, uid))
    ct_window_toast (window, _("Failed to open editor."));
  else
    ct_window_edit_contact (window, uid);
}

/* --- the actions */

static void
on_view_contact (GSimpleAction *action, GVariant *parameter, gpointer app)
{
  GtkWidget *window = contacts_window (app);

  gtk_window_present (GTK_WINDOW (window));
  when_ready (window, ct_window_show_contact, g_variant_get_string (parameter, NULL));
}

static void
on_edit_contact (GSimpleAction *action, GVariant *parameter, gpointer app)
{
  GtkWidget *window = contacts_window (app);

  gtk_window_present (GTK_WINDOW (window));
  when_ready (window, edit_contact, g_variant_get_string (parameter, NULL));
}

/* the Minerva settings of the people (trust, when strangers are forgotten): Settings › Minerva › Memoria opens them */
static void
on_minerva_settings (GSimpleAction *action, GVariant *parameter, gpointer app)
{
  GtkWidget *window = contacts_window (app);

  gtk_window_present (GTK_WINDOW (window));
  ct_window_push (window, ct_minerva_settings_new (window));
}

/* the fields another app sends: a name, a number, an email address (Intents.Insert.NAME, PHONE, EMAIL) */
static EContact *
prefill_from (GVariant *fields)
{
  EContact *contact = e_contact_new ();
  const char *name = NULL, *phone = NULL, *email = NULL;

  g_variant_lookup (fields, "name", "&s", &name);
  g_variant_lookup (fields, "phone", "&s", &phone);
  g_variant_lookup (fields, "email", "&s", &email);
  if (name && *name)
    {
      /* one string: given and family as e_contact_name_from_string splits it */
      EContactName *parsed = e_contact_name_from_string (name);
      g_autoptr(CtName) split = ct_name_new ();

      g_free (split->given);
      g_free (split->family);
      split->given = g_strdup (parsed->given ? parsed->given : "");
      split->family = g_strdup (parsed->family ? parsed->family : "");
      ct_contact_set_name (contact, split);
      e_contact_name_free (parsed);
    }
  for (int k = 0; k < 2; k++)
    {
      const char *value = k == 0 ? phone : email;
      CtKind kind = k == 0 ? CT_KIND_PHONE : CT_KIND_EMAIL;
      g_autoptr(GPtrArray) items = NULL;

      if (!value || !*value)
        continue;
      items = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);
      g_ptr_array_add (items, ct_item_new (kind, ct_kind_default_type (kind), value));
      ct_contact_set_items (contact, kind, items);
    }
  return contact;
}

static void
on_insert (GSimpleAction *action, GVariant *parameter, gpointer app)
{
  GtkWidget *window = contacts_window (app);
  g_autoptr(EContact) prefill = prefill_from (parameter);

  gtk_window_present (GTK_WINDOW (window));
  ct_window_new_contact (window, prefill);
}

static void
on_insert_picked (GPtrArray *uids, gpointer data)
{
  EContact *prefill = data;
  GtkWidget *window = g_object_get_data (G_OBJECT (prefill), "ct-window");

  if (uids->len)
    ct_window_push (window, ct_editor_new (window, uids->pdata[0], prefill));
}

static void
on_insert_new (gpointer data)
{
  EContact *prefill = data;

  ct_window_new_contact (g_object_get_data (G_OBJECT (prefill), "ct-window"), prefill);
}

/* ContactSelectionActivity for INSERT_OR_EDIT: "Choose a contact" with "Create new contact" first */
static void
on_insert_or_edit (GSimpleAction *action, GVariant *parameter, gpointer app)
{
  GtkWidget *window = contacts_window (app);
  EContact *prefill = prefill_from (parameter);
  GtkWidget *picker = ct_picker_new (window, _("Choose a contact"), NULL, FALSE, on_insert_picked, prefill);

  g_object_set_data (G_OBJECT (prefill), "ct-window", window);
  /* the prefill lives as long as the picker */
  g_object_set_data_full (G_OBJECT (picker), "ct-prefill", prefill, g_object_unref);
  ct_picker_offer_new (picker, on_insert_new, prefill);
  gtk_window_present (GTK_WINDOW (window));
  ct_window_push (window, picker);
}

/* --- pick-contact: the answer goes back as an action of the caller's */

typedef struct {
  char *app_id, *action;
  gboolean answered;
} PickRequest;

static void
answer (PickRequest *request, const char *uid, const char *vcard)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());
  g_autofree char *path = NULL;
  GVariantBuilder parameters;

  request->answered = TRUE;
  if (!bus || !g_application_id_is_valid (request->app_id))
    return;
  /* an application's object path is its id with the dots as slashes (GApplication's convention) */
  path = g_strconcat ("/", request->app_id, NULL);
  g_strdelimit (path, ".", '/');
  g_strdelimit (path, "-", '_');
  g_variant_builder_init (&parameters, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&parameters, "v", g_variant_new ("(ss)", uid, vcard));
  g_dbus_connection_call (bus, request->app_id, path, "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sava{sv})", request->action, &parameters, NULL), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
pick_request_free (gpointer data)
{
  PickRequest *request = data;

  /* the picker went without a pick (Back, the ✕): RESULT_CANCELED */
  if (!request->answered)
    answer (request, "", "");
  g_free (request->app_id);
  g_free (request->action);
  g_free (request);
}

static void
on_contact_picked (GPtrArray *uids, gpointer data)
{
  PickRequest *request = data;
  CtEntry *entry = uids->len ? ct_store_lookup (store, uids->pdata[0]) : NULL;
  g_autofree char *vcard = entry ? e_vcard_to_string (E_VCARD (entry->contact), EVC_FORMAT_VCARD_30) : NULL;

  if (entry)
    answer (request, entry->uid, vcard);
}

static void
on_pick_contact (GSimpleAction *action, GVariant *parameter, gpointer app)
{
  GtkWidget *window = contacts_window (app);
  PickRequest *request = g_new0 (PickRequest, 1);
  const char *need;
  GtkWidget *picker;

  g_variant_get (parameter, "(ss&s)", &request->app_id, &request->action, &need);
  picker = ct_picker_new (window, _("Choose a contact"), NULL, FALSE, on_contact_picked, request);
  g_object_set_data_full (G_OBJECT (picker), "ct-request", request, pick_request_free);
  /* Phone.CONTENT_TYPE / Email.CONTENT_TYPE: only the contacts that have one (the store may still be loading,
   * so the picker checks each contact as it comes) */
  if (g_str_equal (need, "phone"))
    ct_picker_require (picker, CT_KIND_PHONE);
  else if (g_str_equal (need, "email"))
    ct_picker_require (picker, CT_KIND_EMAIL);
  gtk_window_present (GTK_WINDOW (window));
  ct_window_push (window, picker);
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;
  static const GActionEntry actions[] = {
    { .name = "view-contact", .activate = on_view_contact, .parameter_type = "s" },
    { .name = "edit-contact", .activate = on_edit_contact, .parameter_type = "s" },
    { .name = "insert", .activate = on_insert, .parameter_type = "a{sv}" },
    { .name = "insert-or-edit", .activate = on_insert_or_edit, .parameter_type = "a{sv}" },
    { .name = "pick-contact", .activate = on_pick_contact, .parameter_type = "(sss)" },
    { .name = "minerva-settings", .activate = on_minerva_settings },
  };

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = gtk_application_new (CT_APP_ID, G_APPLICATION_HANDLES_OPEN);
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/yesman/Contacts");
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
