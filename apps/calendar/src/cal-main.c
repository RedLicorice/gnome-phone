/* neo-calendar: the Calendar app, Etar rebuilt in GTK 4 over evolution-data-server (spec/calendar.md).
 *
 * de.yesman.Calendar, D-Bus activatable: the launcher starts it; the reminder service (neo-calendar-reminders)
 * opens an event's info with the "open-event" action when its notification is tapped, the reminders page with
 * "show-alerts" (Pop-up notification), the Snooze delay dialog with "snooze-delay" (Ask for reminder snooze delay)
 * and the quick responses with "email-guests" (a notification's Email guests), all in cal-alert-page.c; an .ics
 * file opened with it goes into the editor as a new event (Etar's ImportActivity).
 *
 * The reminders are not here: the reminder service notifies for them, and wakes the phone for them, whether or not
 * this app runs. */
#include "cal-alert-page.h"
#include "cal-colors.h"
#include "cal-store.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <neo.h>

#define APP_ID "de.yesman.Calendar"

static GSettings *settings;
static GSettings *interface;            /* org.gnome.desktop.interface, where installed: "System default" */
static CalStore *store;

static void
load_style (void)
{
  /* the app's sheet, then the pickers', the search screen's and the account screens' over it */
  static const char *sheets[] = { "calendar.css", "calendar-pickers.css", "calendar-search.css",
                                  "calendar-accounts.css", "calendar-recurrence.css", "calendar-alerts.css" };

  for (guint i = 0; i < G_N_ELEMENTS (sheets); i++)
    {
      GtkCssProvider *provider = gtk_css_provider_new ();
      g_autofree char *path = g_strconcat ("/de/yesman/Calendar/", sheets[i], NULL);

      gtk_css_provider_load_from_resource (provider, path);
      /* over libneo's, which is at APPLICATION priority too: the later provider wins at equal priority */
      gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
      g_object_unref (provider);
    }
}

/* Theme (DynamicTheme.applyTheme): light, dark, or the system's, read where GNOME keeps it (color-scheme
 * "prefer-dark"; "default" and "prefer-light" are light, as Android's day mode); Pure black night mode makes a dark
 * look the black one. Etar recreates its activities on a change; here every window is redrawn in the new colours. */
static void
apply_look (GApplication *app)
{
  g_autofree char *theme = g_settings_get_string (settings, "theme");
  g_autofree char *scheme = interface ? g_settings_get_string (interface, "color-scheme") : NULL;
  gboolean dark = g_str_equal (theme, "dark")
                  || (g_str_equal (theme, "system") && g_strcmp0 (scheme, "prefer-dark") == 0);
  CalLook look = !dark ? CAL_LOOK_LIGHT : g_settings_get_boolean (settings, "pure-black") ? CAL_LOOK_BLACK
                                                                                            : CAL_LOOK_DARK;
  gboolean light = g_str_equal (theme, "light");
  static gboolean applied;

  if (applied && look == cal_palette_look () && light == cal_theme_light)
    return;
  applied = TRUE;
  cal_theme_light = light;
  cal_palette_set (look);
  for (GList *l = gtk_application_get_windows (GTK_APPLICATION (app)); l; l = l->next)
    cal_palette_restyle (l->data);
}

static void
on_look_changed (GSettings *source, const char *key, gpointer app)
{
  apply_look (app);
}

static void
on_startup (GApplication *app, gpointer data)
{
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = schemas ? g_settings_schema_source_lookup (schemas,
                                                                                 "org.gnome.desktop.interface", TRUE)
                                              : NULL;

  neo_init ();
  load_style ();
  if (schema && g_settings_schema_has_key (schema, "color-scheme"))
    {
      interface = g_settings_new ("org.gnome.desktop.interface");
      g_signal_connect (interface, "changed::color-scheme", G_CALLBACK (on_look_changed), app);
    }
  g_signal_connect (settings, "changed::theme", G_CALLBACK (on_look_changed), app);
  g_signal_connect (settings, "changed::pure-black", G_CALLBACK (on_look_changed), app);
  apply_look (app);
  store = cal_store_new (FALSE);
  cal_store_enable_minerva (store);   /* the harness scheduler as the "Minerva" calendar, when minerva-config is here */
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  g_clear_object (&store);
  g_clear_object (&interface);
}

static GtkWidget *
calendar_window (GtkApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  return window ? GTK_WIDGET (window) : cal_window_new (app, store, settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (calendar_window (GTK_APPLICATION (app))));
}

/* an .ics file (MimeType=text/calendar): its first event in the editor */
static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (app));

  gtk_window_present (GTK_WINDOW (window));
  if (n_files > 0)
    cal_window_import (window, files[0]);
}

/* the reminder service's "open this event": (calendar uid, event uid, recurrence id or "", start) */
static void
on_open_event (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (data));
  const char *calendar, *uid, *rid;
  gint64 start;

  g_variant_get (parameter, "(&s&s&sx)", &calendar, &uid, &rid, &start);
  gtk_window_present (GTK_WINDOW (window));
  cal_window_open_reminder (window, calendar, uid, rid, start);
}

/* the reminder service's AlertActivity (Pop-up notification): the reminders page over the window */
static void
on_show_alerts (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (data));

  gtk_window_present (GTK_WINDOW (window));
  cal_alert_page_show (window);
}

/* its SnoozeDelayActivity: (occurrence key, the default snooze delay in minutes) */
static void
on_snooze_delay (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (data));
  const char *occurrence;
  int minutes;

  g_variant_get (parameter, "(&si)", &occurrence, &minutes);
  gtk_window_present (GTK_WINDOW (window));
  cal_alert_snooze_delay (window, occurrence, minutes);
}

/* its QuickResponseActivity: (title, organizer, the calendar owner's address, [(address, name, status)]), what
 * the mail to the guests is made of */
static void
on_email_guests (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (data));
  g_autoptr(CalEvent) event = cal_event_new ();
  g_autoptr(GVariantIter) guests = NULL;
  const char *title, *organizer, *me, *email, *name;
  guint32 status;

  g_variant_get (parameter, "(&s&s&sa(ssu))", &title, &organizer, &me, &guests);
  event->title = g_strdup (title);
  event->organizer = *organizer ? g_strdup (organizer) : NULL;
  while (g_variant_iter_next (guests, "(&s&su)", &email, &name, &status))
    {
      CalAttendee *guest = g_new0 (CalAttendee, 1);

      guest->email = g_strdup (email);
      guest->name = *name ? g_strdup (name) : NULL;
      guest->status = status;
      g_ptr_array_add (event->attendees, guest);
    }
  gtk_window_present (GTK_WINDOW (window));
  cal_alert_email_guests (window, settings, event, *me ? me : NULL);
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;
  static const GActionEntry actions[] = {
    { .name = "open-event", .activate = on_open_event, .parameter_type = "(sssx)" },
    { .name = "show-alerts", .activate = on_show_alerts },
    { .name = "snooze-delay", .activate = on_snooze_delay, .parameter_type = "(si)" },
    { .name = "email-guests", .activate = on_email_guests, .parameter_type = "(sssa(ssu))" },
  };
  int status;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  settings = g_settings_new (APP_ID);
  app = gtk_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/yesman/Calendar");
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (settings);
  return status;
}
