/* The event's info screen (event-info.h; spec/calendar.md §6). */
#include "event-info.h"
#include "cal-minerva.h"
#include "ns-mc.h"
#include "cal-alert-page.h"
#include "cal-colors.h"
#include "cal-color-picker.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-reminders.h"
#include "cal-settings.h"
#include "cal-window.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <neo.h>
#include <string.h>
#include <spongy-gtk.h>

typedef struct {
  GtkWidget *page, *window;
  CalStore *store;
  CalEvent *event;
  GArray *saved_reminders;      /* the reminders as the calendar has them */
  GCancellable *cancellable;

  GtkWidget *actions, *menu;
  GtkWidget *headline, *title, *when, *repeat, *where;
  GtkWidget *calendar_row, *calendar_name, *organizer_row, *organizer, *description, *url;
  GtkWidget *email_row, *email_label, *guests;
  GtkWidget *reminders_header, *reminders, *add_reminder;
} Info;

static void update_actions (Info *info);
static void fill_guests (Info *info);

static void
info_free (gpointer data)
{
  Info *info = data;

  g_cancellable_cancel (info->cancellable);
  g_clear_object (&info->cancellable);
  if (info->menu)
    gtk_overlay_remove_overlay (neo_window_get_overlay (info->window), info->menu);
  g_signal_handlers_disconnect_by_data (info->store, info);
  g_clear_pointer (&info->event, cal_event_unref);
  g_clear_pointer (&info->saved_reminders, g_array_unref);
  g_object_unref (info->store);
  g_free (info);
}

/* --- the headline's colour: a box that paints itself in the event's display colour */

#define CAL_TYPE_COLOR_BOX (cal_color_box_get_type ())
G_DECLARE_FINAL_TYPE (CalColorBox, cal_color_box, CAL, COLOR_BOX, GtkBox)

struct _CalColorBox {
  GtkBox parent;
  GdkRGBA color;
};

G_DEFINE_FINAL_TYPE (CalColorBox, cal_color_box, GTK_TYPE_BOX)

static void
cal_color_box_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalColorBox *self = CAL_COLOR_BOX (widget);

  gtk_snapshot_append_color (snapshot, &self->color,
                             &GRAPHENE_RECT_INIT (0, 0, (float) gtk_widget_get_width (widget),
                                                  (float) gtk_widget_get_height (widget)));
  GTK_WIDGET_CLASS (cal_color_box_parent_class)->snapshot (widget, snapshot);
}

static void
cal_color_box_class_init (CalColorBoxClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = cal_color_box_snapshot;
}

static void
cal_color_box_init (CalColorBox *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
}

/* --- filling it in */

static gboolean
reminders_equal (GArray *a, GArray *b)
{
  if (!a || !b || a->len != b->len)
    return a == b || (!a && b && !b->len) || (!b && a && !a->len);
  for (guint i = 0; i < a->len; i++)
    if (g_array_index (a, CalReminder, i).minutes != g_array_index (b, CalReminder, i).minutes
        || g_array_index (a, CalReminder, i).method != g_array_index (b, CalReminder, i).method)
      return FALSE;
  return TRUE;
}

static void
set_label (GtkWidget *label, const char *text)
{
  gtk_label_set_label (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
}

/* the headline's text over the event's colour: with real event colours black or white by its brightness
 * (EventInfoFragment.updateAdaptiveTextAndIconColors), else the stylesheet's */
static void
tint (GtkWidget *label, guint32 rgb, guint32 base)
{
  guint32 argb = cal_adaptive_text_color (base, rgb);
  PangoAttrList *attrs;

  if (argb == base)
    {
      gtk_label_set_attributes (GTK_LABEL (label), NULL);
      return;
    }
  attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_foreground_new (((argb >> 16) & 0xff) * 257, ((argb >> 8) & 0xff) * 257,
                                                            (argb & 0xff) * 257));
  pango_attr_list_insert (attrs, pango_attr_foreground_alpha_new ((argb >> 24) * 257));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);
}

/* mIsOrganizer: the calendar's owner is the ORGANIZER; an event without one is the owner's own (the provider
 * writes the owner as the organizer of an event made on the phone) */
static gboolean
is_organizer (const CalEvent *event, const CalCalendar *calendar)
{
  if (!event->organizer)
    return TRUE;
  return calendar && calendar->email && g_ascii_strcasecmp (calendar->email, event->organizer) == 0;
}

/* A Minerva task's lines under its title (minerva-config schedule ui texts): its state, or a run's outcome and error,
 * whether its call will be put to the owner, a task Minerva made that is changed by asking her, and the whole text */
static char *
minerva_details (Info *info)
{
  CalMinervaInfo *m = info->event->minerva;
  JsonObject *ui = cal_store_minerva_ui (info->store);
  g_autofree char *state_key = g_strdup_printf ("state_%s", m->state);
  GString *text = g_string_new (ns_json_str (ui, state_key));

  if (m->error && *m->error)
    {
      g_autofree char *line = cal_minerva_fill (ns_json_str (ui, "run_error"), "error", m->error);

      g_string_append_printf (text, "\n%s", line);
    }
  if (m->approval)
    g_string_append_printf (text, "\n%s", ns_json_str (ui, "needs_approval"));
  if (!m->past && info->event->readonly)
    g_string_append_printf (text, "\n%s", ns_json_str (ui, "read_only"));
  if (m->goal && *m->goal && g_strcmp0 (m->goal, info->event->title) != 0)
    g_string_append_printf (text, "\n\n%s", m->goal);
  return g_string_free (text, FALSE);
}

static void
show_event (Info *info)
{
  CalEvent *event = info->event;
  const CalCalendar *calendar = cal_store_find_calendar (info->store, event->calendar);
  g_autofree char *when = cal_format_event_when (event->start, event->end, event->all_day,
                                                 g_get_real_time () / G_USEC_PER_SEC);
  g_autofree char *repeat = event->recurring ? cal_format_rrule (event->rrule, event->start) : NULL;
  gboolean writable = !event->readonly;

  guint32 display = cal_event_display_color (event);
  g_autofree char *organizer = NULL;

  CAL_COLOR_BOX (info->headline)->color = cal_argb (0xff000000 | display);
  gtk_widget_queue_draw (info->headline);
  set_label (info->title, event->title && *event->title ? event->title : _("(No title)"));
  set_label (info->when, when);
  set_label (info->repeat, repeat);
  set_label (info->where, event->location);
  /* event_info_headline_color is white; the repeat rule's is white at 70 % (the stylesheet's) */
  tint (info->title, display, 0xffffffff);
  tint (info->when, display, 0xffffffff);
  tint (info->where, display, 0xffffffff);
  tint (info->repeat, display, 0xffffffff);

  /* "Calendar: <name>" (calendar_container shows when the calendar is known) */
  gtk_widget_set_visible (info->calendar_row, calendar != NULL);
  if (calendar)
    gtk_label_set_label (GTK_LABEL (info->calendar_name), calendar->name);
  if (event->minerva)
    {
      g_autofree char *details = minerva_details (info);

      set_label (info->description, details);
    }
  else
    set_label (info->description, event->description);
  set_label (info->url, event->url);

  /* "Organizer:" when someone else organizes it: their name (the organizer's ATTENDEE CN), else their address */
  if (!is_organizer (event, calendar))
    {
      for (guint i = 0; i < event->attendees->len && !organizer; i++)
        {
          const CalAttendee *guest = event->attendees->pdata[i];

          if (guest->organizer && guest->name)
            organizer = g_strdup (guest->name);
        }
      if (!organizer && !g_str_has_suffix (event->organizer, "calendar.google.com"))
        organizer = g_strdup (event->organizer);
    }
  gtk_widget_set_visible (info->organizer_row, organizer != NULL);
  gtk_label_set_label (GTK_LABEL (info->organizer), organizer ? organizer : "");

  /* Email guests (or the organizer), with a mail app to write with */
  {
    g_autofree char *to_guests = NULL;
    g_autofree char *uri = cal_can_mail () ? cal_event_mail_uri (event, calendar ? calendar->email : NULL, NULL)
                                           : NULL;
    gboolean guests = FALSE;

    for (guint i = 0; i < event->attendees->len && uri; i++)
      {
        const CalAttendee *guest = event->attendees->pdata[i];

        if (!calendar || !calendar->email || g_ascii_strcasecmp (guest->email, calendar->email) != 0)
          guests = TRUE;
      }
    gtk_widget_set_visible (info->email_row, uri != NULL);
    gtk_label_set_label (GTK_LABEL (info->email_label), guests ? _("Email guests") : _("Email organizer"));
  }
  fill_guests (info);

  /* Edit (the organizer, on a writable calendar), Delete and the colour (a writable calendar), Duplicate, Share,
   * Export */
  update_actions (info);

  /* The reminders: Etar keeps them in its own table, so even a read-only calendar's events have them. Here they are
   * the event's VALARMs, which only a writable calendar can store: a read-only one shows none, and no "Add". */
  g_clear_pointer (&info->saved_reminders, g_array_unref);
  info->saved_reminders = g_array_copy (event->reminders);
  cal_reminders_set (CAL_REMINDERS (info->reminders), event->reminders);
  /* a Minerva task has no VALARMs: a reminder is a task of its own (the editor's kind) */
  writable = writable && !event->minerva;
  gtk_widget_set_visible (info->reminders_header, writable);
  gtk_widget_set_visible (info->reminders, writable);
  gtk_widget_set_visible (info->add_reminder, writable && cal_reminders_count (CAL_REMINDERS (info->reminders))
                                                             < CAL_MAX_REMINDERS);
}

/* the store changed: read the event again (it may have been edited, or be gone) */
static void
on_found (GObject *source, GAsyncResult *result, gpointer data)
{
  Info *info = data;
  g_autoptr(GError) error = NULL;
  CalEvent *event = cal_store_find_event_finish (CAL_STORE (source), result, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!event)
    {
      /* "Event not found.": it went from under us */
      cal_window_pop (info->window);
      return;
    }
  /* the reminders the user is changing stay as they are (and still differ from the calendar's) */
  {
    GArray *changing = cal_reminders_get (CAL_REMINDERS (info->reminders));
    GArray *stored = g_array_copy (event->reminders);
    gboolean changed = !reminders_equal (changing, info->saved_reminders);

    if (changed)
      {
        g_array_unref (event->reminders);
        event->reminders = g_array_ref (changing);
      }
    cal_event_unref (info->event);
    info->event = event;
    show_event (info);
    g_clear_pointer (&info->saved_reminders, g_array_unref);
    info->saved_reminders = stored;
    g_array_unref (changing);
  }
}

static void
on_store_changed (CalStore *store, Info *info)
{
  g_cancellable_cancel (info->cancellable);
  g_clear_object (&info->cancellable);
  info->cancellable = g_cancellable_new ();
  cal_store_find_event (store, info->event->calendar, info->event->uid, info->event->rid, info->event->start,
                        info->cancellable, on_found, info);
}

/* --- the reminders: saved on the way back (EventInfoFragment.saveReminders, onStop) */

static void
on_reminders_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!cal_store_save_finish (CAL_STORE (source), result, &error))
    g_warning ("reminders not saved: %s", error->message);
}

static void
save_reminders (Info *info)
{
  g_autoptr(GArray) now = cal_reminders_get (CAL_REMINDERS (info->reminders));
  CalEvent *edited;

  if (info->event->readonly || reminders_equal (now, info->saved_reminders))
    return;
  /* the reminders are the series' (Etar's Reminders rows belong to the event, not an instance) */
  edited = cal_event_copy (info->event);
  g_array_unref (edited->reminders);
  edited->reminders = g_array_ref (now);
  cal_store_save (info->store, info->event, edited, CAL_SCOPE_ALL, on_reminders_saved, NULL);
  cal_event_unref (edited);
  g_clear_pointer (&info->saved_reminders, g_array_unref);
  info->saved_reminders = g_array_copy (now);
}

static gboolean
on_back (CalPage *page, gpointer data)
{
  save_reminders (data);
  return FALSE;
}

static void
on_reminders_changed (CalReminders *reminders, Info *info)
{
  gtk_widget_set_visible (info->add_reminder, cal_reminders_count (reminders) < CAL_MAX_REMINDERS);
}

static void
on_add_reminder (NeoPressable *button, Info *info)
{
  /* EventInfoFragment.addReminder: the default reminder time, else 10 minutes */
  int minutes = g_settings_get_int (cal_window_get_settings (info->window), "default-reminder");

  cal_reminders_add (CAL_REMINDERS (info->reminders), minutes >= 0 ? minutes : 10);
}

/* --- the actions */

static void
on_edit (NeoPressable *button, Info *info)
{
  g_autoptr(CalEvent) event = cal_event_ref (info->event);
  GtkWidget *window = info->window;

  /* doEdit, then the info screen finishes: the editor takes its place */
  save_reminders (info);
  cal_window_pop (window);
  cal_window_edit_event (window, event, FALSE);
}

static void
on_duplicate (NeoPressable *button, Info *info)
{
  g_autoptr(CalEvent) event = cal_event_ref (info->event);
  GtkWidget *window = info->window;

  save_reminders (info);
  cal_window_pop (window);
  cal_window_edit_event (window, event, TRUE);
}

static void
on_deleted (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!cal_store_delete_finish (CAL_STORE (source), result, &error))
    g_warning ("delete: %s", error->message);
}

typedef struct {
  GtkWidget *window;
  CalStore *store;
  CalEvent *event;
} Deleting;

static void
on_delete_answer (int index, gpointer data)
{
  Deleting *deleting = data;
  CalScope scopes[] = { CAL_SCOPE_THIS, CAL_SCOPE_THIS_AND_FUTURE, CAL_SCOPE_ALL };

  if (index >= 0)
    {
      /* a single event or a Minerva task (one task, whatever due time was opened): OK is index 0, the whole of it */
      CalScope scope = deleting->event->recurring && !deleting->event->minerva ? scopes[index] : CAL_SCOPE_ALL;

      cal_store_delete (deleting->store, deleting->event, scope, on_deleted, NULL);
      /* exitWhenDone: back to the views */
      cal_window_pop (deleting->window);
    }
  g_object_unref (deleting->store);
  cal_event_unref (deleting->event);
  g_free (deleting);
}

static void
on_delete (NeoPressable *button, Info *info)
{
  Deleting *deleting = g_new0 (Deleting, 1);

  deleting->window = info->window;
  deleting->store = g_object_ref (info->store);
  deleting->event = cal_event_ref (info->event);
  if (info->event->recurring && !info->event->minerva)
    {
      static const char *labels[] = { N_("Only this event"), N_("This and future events"), N_("All events") };
      const char *translated[] = { _(labels[0]), _(labels[1]), _(labels[2]) };
      g_autofree char *title = g_strdup_printf (_("Delete %s?"), info->event->title && *info->event->title
                                                                 ? info->event->title : _("(No title)"));

      cal_radio_dialog (info->page, title, translated, 3, on_delete_answer, deleting);
    }
  else
    cal_confirm_dialog (info->page, _("Delete this event?"), _("OK"), on_delete_answer, deleting);
}

static void
on_export_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    g_warning ("export: %s", error->message);
  g_free (data);
}

static void
on_export_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  char *ics = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (!file)
    {
      g_free (ics);
      return;
    }
  g_file_replace_contents_async (file, ics, strlen (ics), NULL, FALSE, G_FILE_CREATE_NONE, NULL, on_export_saved,
                                 ics);
}

static void
on_export (NeoPressable *button, Info *info)
{
  g_autoptr(GError) error = NULL;
  char *ics = cal_store_export_sync (info->store, info->event, &error);
  g_autoptr(GtkFileDialog) dialog = NULL;
  g_autofree char *name = NULL;

  if (!ics)
    {
      g_warning ("export: %s", error->message);
      return;
    }
  /* shareEvent(EXPORT_SAF): the system's "save as", the file named after the event */
  name = g_strdup_printf ("%s.ics", info->event->title && *info->event->title ? info->event->title : "event");
  g_strdelimit (name, "/", '_');
  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Export event"));
  gtk_file_dialog_set_initial_name (dialog, name);
  gtk_file_dialog_save (dialog, GTK_WINDOW (info->window), NULL, on_export_chosen, ics);
}

/* Share (shareEvent(INTENT)): the event as an .ics in the cache, handed to an app the user picks, as Android's
 * share sheet offers the apps that take text/calendar; the portal's "open with" chooser (OpenFile, ask) is that
 * sheet here. It shows only when an app besides this one opens text/calendar. */
static gboolean
can_share (void)
{
  GList *apps = g_app_info_get_all_for_type ("text/calendar");
  gboolean other = FALSE;

  for (GList *l = apps; l && !other; l = l->next)
    other = g_strcmp0 (g_app_info_get_id (l->data), "de.yesman.Calendar.desktop") != 0;
  g_list_free_full (apps, g_object_unref);
  return other;
}

static void
on_shared (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!gtk_file_launcher_launch_finish (GTK_FILE_LAUNCHER (source), result, &error)
      && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    g_warning ("share: %s", error->message);
}

static void
on_share (NeoPressable *button, Info *info)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *ics = cal_store_export_sync (info->store, info->event, &error);
  g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "de.yesman.Calendar", "share", NULL);
  g_autofree char *name = NULL, *path = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GtkFileLauncher) launcher = NULL;

  if (!ics)
    {
      g_warning ("share: %s", error->message);
      return;
    }
  name = g_strdup_printf ("%s.ics", info->event->title && *info->event->title ? info->event->title : "event");
  g_strdelimit (name, "/", '_');
  path = g_build_filename (dir, name, NULL);
  if (g_mkdir_with_parents (dir, 0700) != 0 || !g_file_set_contents (path, ics, -1, &error))
    {
      g_warning ("share: %s", error ? error->message : g_strerror (errno));
      return;
    }
  file = g_file_new_for_path (path);
  launcher = gtk_file_launcher_new (file);
  gtk_file_launcher_set_always_ask (launcher, TRUE);
  gtk_file_launcher_launch (launcher, GTK_WINDOW (info->window), NULL, on_shared, NULL);
}

/* Choose event color (EventColorPickerDialog): Etar's palette for an account without one of its own, the event's
 * COLOR written to the whole series as saveEventColor writes the event row; "Set to default calendar color" takes
 * the event's own colour away */
static void
on_color_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!cal_store_save_finish (CAL_STORE (source), result, &error))
    g_warning ("colour not saved: %s", error->message);
}

static void
on_color_chosen (guint32 rgb, gpointer data)
{
  Info *info = data;
  const CalCalendar *calendar = cal_store_find_calendar (info->store, info->event->calendar);
  gboolean own = !calendar || rgb != calendar->color;
  CalEvent *edited;

  if (rgb == info->event->color && own == info->event->own_color)
    return;
  edited = cal_event_copy (info->event);
  edited->color = rgb;
  edited->own_color = own;
  cal_store_save (info->store, info->event, edited, CAL_SCOPE_ALL, on_color_saved, NULL);
  cal_event_unref (edited);
}

static void
on_change_color (NeoPressable *button, Info *info)
{
  const CalCalendar *calendar = cal_store_find_calendar (info->store, info->event->calendar);
  int n_colors;
  const guint32 *colors = cal_color_picker_default_colors (&n_colors);

  cal_event_color_picker_show (info->window, colors, n_colors, info->event->color,
                               calendar ? calendar->color : info->event->color, on_color_chosen, info);
}

/* Email guests (QuickResponseActivity): a quick response (sorted, as Etar lists them) or "Write your own…", then
 * the mail app with it, as from a reminder's notification */
static void
on_email (NeoPressable *button, Info *info)
{
  const CalCalendar *calendar = cal_store_find_calendar (info->store, info->event->calendar);

  cal_alert_email_guests (info->window, cal_window_get_settings (info->window), info->event,
                          calendar ? calendar->email : NULL);
}

/* --- the guests (AttendeesView, not editable): under "Yes", "No", "Maybe" and "No response" with their counts,
 * each a name (else the address) and the default contact picture, faded for one who has not responded. A guest's
 * own photo from the address book is left out: the app reads no contacts. */

static GtkWidget *
guest_row (const CalAttendee *guest)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *name = gtk_label_new (guest->name ? guest->name : guest->email);
  GtkWidget *badge = gtk_picture_new_for_resource ("/de/yesman/Calendar/contact-picture.png");
  GtkWidget *separator = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (row, "cal-guest");
  gtk_widget_add_css_class (line, "cal-guest-line");
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (name), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_widget_add_css_class (name, "cal-guest-name");
  gtk_box_append (GTK_BOX (line), name);
  gtk_picture_set_content_fit (GTK_PICTURE (badge), GTK_CONTENT_FIT_FILL);
  gtk_picture_set_can_shrink (GTK_PICTURE (badge), TRUE);
  gtk_widget_set_size_request (badge, 48, 48);
  gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
  /* noresponse_attendee_photo_alpha_level 64; a declined guest's is greyscale, as the picture already is */
  gtk_widget_set_opacity (badge, guest->status == CAL_STATUS_NONE ? 64 / 255.0 : 1);
  gtk_box_append (GTK_BOX (line), badge);
  gtk_box_append (GTK_BOX (row), line);
  gtk_widget_add_css_class (separator, "cal-guest-separator");
  gtk_widget_set_visible (separator, FALSE);
  gtk_box_append (GTK_BOX (row), separator);
  return row;
}

static void
fill_guests (Info *info)
{
  static const CalStatus groups[] = { CAL_STATUS_ACCEPTED, CAL_STATUS_DECLINED, CAL_STATUS_TENTATIVE, CAL_STATUS_NONE };
  const char *labels[] = { _("Yes"), _("No"), _("Maybe"), _("No response") };
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (info->guests)))
    gtk_box_remove (GTK_BOX (info->guests), child);
  for (guint g = 0; g < G_N_ELEMENTS (groups); g++)
    {
      GtkWidget *divider = NULL, *last = NULL;
      int count = 0;

      for (guint i = 0; i < info->event->attendees->len; i++)
        {
          const CalAttendee *guest = info->event->attendees->pdata[i];
          CalStatus status = guest->status;
          GtkWidget *row;

          /* "No response" takes every guest who has not said yes, no or maybe */
          if (status != CAL_STATUS_ACCEPTED && status != CAL_STATUS_DECLINED && status != CAL_STATUS_TENTATIVE)
            status = CAL_STATUS_NONE;
          if (status != groups[g])
            continue;
          if (!divider)
            {
              divider = gtk_label_new (NULL);
              gtk_label_set_xalign (GTK_LABEL (divider), 0);
              gtk_widget_add_css_class (divider, "cal-list-separator");
              gtk_widget_add_css_class (divider, "cal-guest-divider");
              gtk_box_append (GTK_BOX (info->guests), divider);
            }
          /* a separator under the guest before, within the group */
          if (last)
            gtk_widget_set_visible (gtk_widget_get_last_child (last), TRUE);
          row = guest_row (guest);
          gtk_box_append (GTK_BOX (info->guests), row);
          last = row;
          count++;
        }
      if (divider)
        {
          g_autofree char *text = g_strdup_printf ("%s (%d)", labels[g], count);

          gtk_label_set_label (GTK_LABEL (divider), text);
        }
    }
  gtk_widget_set_visible (info->guests, gtk_widget_get_first_child (info->guests) != NULL);
}

/* --- the toolbar's actions (event_info_title_bar.xml, all showAsAction ifRoom): as many as ActionBarPolicy allows
 * for the window's width (3 on a phone held upright, 5 from 600 dp), one fewer and "More options" when the rest go
 * into the overflow menu */

typedef struct {
  const char *icon, *label;
  void (*activate) (NeoPressable *button, Info *info);
} Action;

static void
close_menu (Info *info)
{
  if (!info->menu)
    return;
  gtk_overlay_remove_overlay (neo_window_get_overlay (info->window), info->menu);
  info->menu = NULL;
}

static void
on_menu_scrim (GtkGestureClick *gesture, int n, double x, double y, Info *info)
{
  close_menu (info);
}

static void
on_menu_item (NeoPressable *item, Info *info)
{
  void (*activate) (NeoPressable *, Info *) = g_object_get_data (G_OBJECT (item), "cal-activate");

  close_menu (info);
  activate (item, info);
}

static void
on_more (NeoPressable *button, Info *info)
{
  const Action *rest = g_object_get_data (G_OBJECT (button), "cal-actions");
  int n_rest = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "cal-n-actions"));
  GtkWidget *scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *menu = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *layer = gtk_overlay_new ();
  GtkGesture *press = gtk_gesture_click_new ();

  close_menu (info);
  g_signal_connect (press, "pressed", G_CALLBACK (on_menu_scrim), info);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (layer), scrim);
  gtk_widget_add_css_class (menu, "cal-menu");
  gtk_widget_add_css_class (menu, "cal-overflow-menu");
  gtk_widget_set_halign (menu, GTK_ALIGN_END);
  gtk_widget_set_valign (menu, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (menu), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  for (int i = 0; i < n_rest; i++)
    {
      GtkWidget *text = gtk_label_new (rest[i].label);
      GtkWidget *item = neo_pressable_new (text);

      gtk_label_set_xalign (GTK_LABEL (text), 0);
      neo_label_set_font_padding (text, 16);
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (item, "cal-menu-item");
      neo_pressable_set_ripple (NEO_PRESSABLE (item), NEO_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (item), "cal-activate", rest[i].activate);
      g_signal_connect (item, "clicked", G_CALLBACK (on_menu_item), info);
      gtk_box_append (GTK_BOX (menu), item);
    }
  gtk_overlay_add_overlay (GTK_OVERLAY (layer), menu);
  info->menu = layer;
  gtk_overlay_add_overlay (neo_window_get_overlay (info->window), layer);
}

static int
max_action_buttons (Info *info)
{
  int width = gtk_widget_get_width (info->window), height = gtk_widget_get_height (info->window);

  if (width > 600 || MIN (width, height) > 600)
    return 5;
  if (width >= 500 || (width > 640 && height > 480) || (width > 480 && height > 640))
    return 4;
  return width >= 360 || width == 0 ? 3 : 2;
}

static void
update_actions (Info *info)
{
  const CalCalendar *calendar = cal_store_find_calendar (info->store, info->event->calendar);
  gboolean writable = !info->event->readonly;
  Action all[6], *rest;
  int n = 0, shown;
  GtkWidget *child;

  if (info->event->minerva)
    {
      /* a task of the scheduler: changed or cancelled there, nothing else (no file, no colour of its own) */
      if (writable)
        {
          all[n++] = (Action) { "cal-edit-symbolic", _("Edit"), on_edit };
          all[n++] = (Action) { "cal-delete-symbolic", _("Delete"), on_delete };
        }
      goto minerva_actions;
    }
  if (writable && is_organizer (info->event, calendar))
    all[n++] = (Action) { "cal-edit-symbolic", _("Edit"), on_edit };
  if (writable)
    all[n++] = (Action) { "cal-delete-symbolic", _("Delete"), on_delete };
  all[n++] = (Action) { "cal-duplicate-symbolic", _("Duplicate"), on_duplicate };
  if (can_share ())
    all[n++] = (Action) { "cal-share-symbolic", _("Share"), on_share };
  all[n++] = (Action) { "cal-export-symbolic", _("Export event"), on_export };
  if (writable)
    all[n++] = (Action) { "cal-color-symbolic", _("Choose event color"), on_change_color };

minerva_actions:
  shown = max_action_buttons (info);
  if (n > shown)
    shown--;
  while ((child = gtk_widget_get_first_child (info->actions)))
    gtk_box_remove (GTK_BOX (info->actions), child);
  for (int i = 0; i < n && i < shown; i++)
    {
      GtkWidget *button = cal_page_action (all[i].icon, all[i].label);

      g_signal_connect (button, "clicked", G_CALLBACK (all[i].activate), info);
      gtk_box_append (GTK_BOX (info->actions), button);
    }
  if (n > shown)
    {
      GtkWidget *more = cal_page_action ("cal-more-symbolic", _("More options"));

      /* the ActionMenuView's overflow button, as the main toolbar's: 40 dp, not an action's 48 */
      gtk_widget_remove_css_class (more, "cal-action");
      gtk_widget_add_css_class (more, "cal-toolbar-more");

      rest = g_memdup2 (all + shown, sizeof (Action) * (n - shown));
      g_object_set_data_full (G_OBJECT (more), "cal-actions", rest, g_free);
      g_object_set_data (G_OBJECT (more), "cal-n-actions", GINT_TO_POINTER (n - shown));
      g_signal_connect (more, "clicked", G_CALLBACK (on_more), info);
      gtk_box_append (GTK_BOX (info->actions), more);
    }
}

/* --- building it */

static GtkWidget *
text (const char *css_class, gboolean wrap)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), wrap);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (label), FALSE);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

GtkWidget *
event_info_new (GtkWidget *window, CalEvent *event)
{
  Info *info = g_new0 (Info, 1);
  GtkWidget *page = cal_page_new (window);
  GtkWidget *scroll, *content, *body, *label, *add_text, *icon, *email;

  info->page = page;
  info->window = window;
  info->store = g_object_ref (cal_window_get_store (window));
  info->event = cal_event_ref (event);
  g_object_set_data_full (G_OBJECT (page), "cal-info", info, info_free);
  cal_page_set_back_handler (CAL_PAGE (page), on_back, info);

  /* MN/event_info_title_bar.xml (update_actions fills it) */
  gtk_box_append (GTK_BOX (page), cal_page_toolbar (CAL_PAGE (page), NULL, &info->actions));

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* L/event_info_headline.xml */
  info->headline = g_object_new (CAL_TYPE_COLOR_BOX, NULL);
  gtk_widget_add_css_class (info->headline, "cal-info-headline");
  info->title = text ("cal-info-title", TRUE);
  info->when = text ("cal-info-when", TRUE);
  info->repeat = text ("cal-info-repeat", TRUE);
  info->where = text ("cal-info-where", TRUE);
  /* TextView's font padding (still:cal-event: the title 97 px, the time 57 px) */
  neo_label_set_font_padding (info->title, 24);
  neo_label_set_font_padding (info->when, 14);
  neo_label_set_font_padding (info->repeat, 14);
  neo_label_set_font_padding (info->where, 14);
  gtk_box_append (GTK_BOX (info->headline), info->title);
  gtk_box_append (GTK_BOX (info->headline), info->when);
  gtk_box_append (GTK_BOX (info->headline), info->repeat);
  gtk_box_append (GTK_BOX (info->headline), info->where);
  gtk_box_append (GTK_BOX (content), info->headline);

  /* L/event_info.xml: the calendar, the description, the URL */
  body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (body, "cal-info-body");
  info->calendar_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  label = text ("cal-info-small", FALSE);
  gtk_label_set_label (GTK_LABEL (label), _("Calendar:"));
  neo_label_set_font_padding (label, 14);
  gtk_box_append (GTK_BOX (info->calendar_row), label);
  info->calendar_name = text ("cal-info-small", TRUE);
  gtk_widget_add_css_class (info->calendar_name, "cal-info-calendar-name");
  gtk_widget_set_hexpand (info->calendar_name, TRUE);
  neo_label_set_font_padding (info->calendar_name, 14);
  gtk_box_append (GTK_BOX (info->calendar_row), info->calendar_name);
  gtk_box_append (GTK_BOX (body), info->calendar_row);
  /* "Organizer:" and the name, one line */
  info->organizer_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  label = text ("cal-info-small", FALSE);
  gtk_label_set_label (GTK_LABEL (label), _("Organizer:"));
  neo_label_set_font_padding (label, 14);
  gtk_box_append (GTK_BOX (info->organizer_row), label);
  info->organizer = text ("cal-info-small", FALSE);
  gtk_label_set_ellipsize (GTK_LABEL (info->organizer), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (info->organizer, "cal-info-calendar-name");
  gtk_widget_set_hexpand (info->organizer, TRUE);
  neo_label_set_font_padding (info->organizer, 14);
  gtk_box_append (GTK_BOX (info->organizer_row), info->organizer);
  gtk_box_append (GTK_BOX (body), info->organizer_row);
  info->description = text ("cal-info-small", TRUE);
  gtk_widget_add_css_class (info->description, "cal-info-description");
  gtk_box_append (GTK_BOX (body), info->description);
  info->url = text ("cal-info-small", TRUE);
  gtk_widget_add_css_class (info->url, "cal-info-description");
  gtk_box_append (GTK_BOX (body), info->url);
  gtk_box_append (GTK_BOX (content), body);

  /* EMAIL GUESTS: a divider and a 50 dp button, the mail icon 8 dp before 16 sp text */
  info->email_row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  label = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (label, "cal-guest-separator");
  gtk_box_append (GTK_BOX (info->email_row), label);
  email = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  icon = gtk_image_new_from_icon_name ("cal-email-symbolic");
  gtk_box_append (GTK_BOX (email), icon);
  info->email_label = gtk_label_new (NULL);
  gtk_box_append (GTK_BOX (email), info->email_label);
  email = neo_pressable_new (email);
  gtk_widget_add_css_class (email, "cal-email-guests");
  neo_pressable_set_ripple (NEO_PRESSABLE (email), NEO_RIPPLE_BOUNDED, 0);
  g_signal_connect (email, "clicked", G_CALLBACK (on_email), info);
  gtk_box_append (GTK_BOX (info->email_row), email);
  gtk_box_append (GTK_BOX (content), info->email_row);

  /* GUEST LIST */
  info->guests = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), info->guests);

  /* REMINDERS (listSeparatorTextViewStyle), the rows, "Add reminder" */
  info->reminders_header = text ("cal-list-separator", FALSE);
  gtk_label_set_label (GTK_LABEL (info->reminders_header), _("REMINDERS"));
  gtk_box_append (GTK_BOX (content), info->reminders_header);
  info->reminders = cal_reminders_new ();
  gtk_widget_add_css_class (info->reminders, "cal-info-reminders");
  g_signal_connect (info->reminders, "changed", G_CALLBACK (on_reminders_changed), info);
  gtk_box_append (GTK_BOX (content), info->reminders);
  add_text = gtk_label_new (_("Add reminder"));
  gtk_label_set_xalign (GTK_LABEL (add_text), 0);
  info->add_reminder = neo_pressable_new (add_text);
  gtk_widget_add_css_class (info->add_reminder, "cal-add-reminder");
  neo_pressable_set_ripple (NEO_PRESSABLE (info->add_reminder), NEO_RIPPLE_BOUNDED, 4);
  g_signal_connect (info->add_reminder, "clicked", G_CALLBACK (on_add_reminder), info);
  gtk_box_append (GTK_BOX (content), info->add_reminder);

  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), content);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);

  g_signal_connect (info->store, "changed", G_CALLBACK (on_store_changed), info);
  show_event (info);
  return page;
}
