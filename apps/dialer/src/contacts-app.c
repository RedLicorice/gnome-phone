/* The Contacts app: see contacts-app.h. */
#include "contacts-app.h"
#include "app-action.h"

#include <glib/gi18n.h>
#include <string.h>

#define CONTACTS_APP_ID "de.yesman.Contacts"
#define OTHER_APP_ID    "org.gnome.Contacts"    /* another Mobian phone's: it opens at its list */

gboolean
contacts_app_installed (void)
{
  return app_action_installed (CONTACTS_APP_ID);
}

/* the app's action @action with @parameter (or none), the app started by the bus if it is not running */
static void
activate (GtkWidget *from, const char *action, GVariant *parameter)
{
  app_action_activate (from, CONTACTS_APP_ID, action, parameter);
}

/* the fields of a new contact: the destination as its phone number, or its e-mail address */
static GVariant *
fields (const char *destination)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (destination && *destination)
    g_variant_builder_add (&builder, "{sv}", strchr (destination, '@') ? "email" : "phone",
                           g_variant_new_string (destination));
  return g_variant_builder_end (&builder);
}

/* launched from its desktop file, at its list */
static void
launch (GtkWidget *from, const char *app_id)
{
  g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);
  g_autoptr(GdkAppLaunchContext) context =
    gdk_display_get_app_launch_context (from ? gtk_widget_get_display (from) : gdk_display_get_default ());
  g_autoptr(GError) error = NULL;

  if (info && !g_app_info_launch (G_APP_INFO (info), NULL, G_APP_LAUNCH_CONTEXT (context), &error))
    g_warning ("contacts: %s", error->message);
}

/* no Contacts app of ours: another phone's, at its list, else the Dialer's toast */
static void
open_other (GtkWidget *from, NeoSnackbar *snackbar)
{
  if (app_action_installed (OTHER_APP_ID))
    launch (from, OTHER_APP_ID);
  else if (snackbar)
    neo_snackbar_show (snackbar, _("No contacts app available"), NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

void
contacts_app_open (GtkWidget *from, NeoSnackbar *snackbar)
{
  if (contacts_app_installed ())
    launch (from, CONTACTS_APP_ID);
  else
    open_other (from, snackbar);
}

void
contacts_app_view (GtkWidget *from, const char *uid, NeoSnackbar *snackbar)
{
  if (!contacts_app_installed () || !uid || !*uid)
    {
      open_other (from, snackbar);
      return;
    }
  activate (from, "view-contact", g_variant_new_string (uid));
}

void
contacts_app_insert (GtkWidget *from, const char *destination)
{
  activate (from, "insert", fields (destination));
}

void
contacts_app_insert_or_edit (GtkWidget *from, const char *destination)
{
  activate (from, "insert-or-edit", fields (destination));
}

void
contacts_app_import_chat (GtkWidget *from, const char *provider, const char *chat)
{
  activate (from, "import-chat", g_variant_new ("(ss)", provider, chat));
}
