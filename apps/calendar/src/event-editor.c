/* The event editor (event-editor.h; spec/calendar.md §7). */
#include "event-editor.h"
#include "cal-minerva.h"
#include "ns-mc.h"
#include "cal-material-switch.h"
#include "cal-outlined-field.h"
#include "cal-colors.h"
#include "cal-color-picker.h"
#include "cal-date-picker.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-popup.h"
#include "cal-recurrence-picker.h"
#include "cal-reminders.h"
#include "cal-time-picker.h"
#include "cal-tz-picker.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <string.h>

typedef struct {
  GtkWidget *page, *window;
  CalStore *store;
  GSettings *settings;
  CalEvent *original;           /* the event changed, or NULL for a new one */
  CalScope scope;
  CalEvent *source;             /* what the fields started from (a copy for Duplicate and Import) */

  char *calendar;
  int start_day, end_day;       /* the dates shown: an all-day event's end is its last day */
  int start_minute, end_minute; /* the times shown, minutes into the day */
  gboolean all_day;
  char *rrule;
  char *tzid;                   /* the zone the times are in (the event's, else the app's) */
  gboolean tz_changed;          /* chosen in the picker: the event takes it */
  guint32 color;                /* the event's colour: its own when own_color, else its calendar's */
  gboolean own_color;

  GtkWidget *title, *calendar_row, *calendar_name, *calendar_account, *calendar_color;
  GtkWidget *all_day_switch, *start_date, *start_time, *end_date, *end_time;
  GtkWidget *color_button, *tz_row, *tz_button, *guests;
  GtkWidget *location, *description, *url, *repeat_row, *repeat, *reminders, *add_reminder;
  GtkWidget *availability, *access;
  GtkWidget *scroll, *content;
  GtkWidget *kind;              /* the Minerva calendar's: what the task is (minerva-config schedule ui kinds) */
  GtkWidget *all_day_line;      /* All day and its switch: an EDS event's only */
  GtkWidget *guests_helper;     /* "Guests are not sent an invitation", under the guests */
  gboolean saving;
} Editor;

static void
editor_free (gpointer data)
{
  Editor *editor = data;

  g_clear_pointer (&editor->original, cal_event_unref);
  g_clear_pointer (&editor->source, cal_event_unref);
  g_free (editor->calendar);
  g_free (editor->rrule);
  g_free (editor->tzid);
  g_object_unref (editor->store);
  g_object_unref (editor->settings);
  g_free (editor);
}

/* --- times: day numbers and minutes into the day, in the event's zone (EditEventView shows the times in
 * mTimezone; choosing another zone keeps them on the clock and moves the moment) */

static GTimeZone *
editor_zone (Editor *editor)
{
  GTimeZone *zone = editor->tzid ? g_time_zone_new_identifier (editor->tzid) : NULL;

  return zone ? zone : g_time_zone_new_local ();
}

static gint64
local_time (Editor *editor, int day, int minute)
{
  GDate date;
  g_autoptr(GTimeZone) zone = editor_zone (editor);
  g_autoptr(GDateTime) time = NULL;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  time = g_date_time_new (zone, g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date),
                          minute / 60, minute % 60, 0);
  return g_date_time_to_unix (time);
}

static void
split_time (Editor *editor, gint64 time, int *day, int *minute)
{
  g_autoptr(GTimeZone) zone = editor_zone (editor);
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc (time);
  g_autoptr(GDateTime) there = g_date_time_to_timezone (utc, zone);
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, g_date_time_get_day_of_month (there), g_date_time_get_month (there),
                  g_date_time_get_year (there));
  *day = (int) g_date_get_julian (&date);
  *minute = g_date_time_get_hour (there) * 60 + g_date_time_get_minute (there);
}

static gint64
start_time (Editor *editor)
{
  return editor->all_day ? cal_day_start (editor->start_day)
                         : local_time (editor, editor->start_day, editor->start_minute);
}

static gint64
end_time (Editor *editor)
{
  return editor->all_day ? cal_day_start (editor->end_day + 1)
                         : local_time (editor, editor->end_day, editor->end_minute);
}

static char *
format_minute (int minute)
{
  GDate date;
  g_autoptr(GDateTime) time = NULL;
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);

  /* the clock time itself, whatever the zone */
  g_date_clear (&date, 1);
  g_date_set_julian (&date, today);
  time = g_date_time_new_local (g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date),
                                minute / 60, minute % 60, 0);
  return cal_format_time (g_date_time_to_unix (time));
}

/* --- showing the values */

static void
set_button_text (GtkWidget *button, const char *text)
{
  gtk_label_set_label (GTK_LABEL (neo_pressable_get_child (NEO_PRESSABLE (button))), text);
}

/* a task of Minerva's scheduler: one due time, no end, no zone of its own (the scheduler's) */
static gboolean
is_minerva (Editor *editor)
{
  return g_strcmp0 (editor->calendar, CAL_MINERVA_CALENDAR) == 0;
}

static void
show_times (Editor *editor)
{
  g_autofree char *start_date = cal_format_editor_date (editor->start_day);
  g_autofree char *end_date = cal_format_editor_date (editor->end_day);
  g_autofree char *start = format_minute (editor->start_minute);
  g_autofree char *end = format_minute (editor->end_minute);

  set_button_text (editor->start_date, start_date);
  set_button_text (editor->end_date, end_date);
  set_button_text (editor->start_time, start);
  set_button_text (editor->end_time, end);
  gtk_widget_set_visible (editor->start_time, !editor->all_day);
  gtk_widget_set_visible (editor->end_time, !editor->all_day && !is_minerva (editor));
  gtk_widget_set_visible (editor->end_date, !is_minerva (editor));
  /* the time zone: hidden with All day (an all-day event has none) */
  if (editor->tz_row)
    {
      /* the GMT part in #888888 (the TZ button's grey text), the sun of summer time in #BFBFBF */
      g_autofree char *zone = cal_tz_label_markup (editor->tzid, start_time (editor), TRUE);

      gtk_widget_set_visible (editor->tz_row, !editor->all_day && !is_minerva (editor));
      gtk_label_set_markup (GTK_LABEL (neo_pressable_get_child (NEO_PRESSABLE (editor->tz_button))), zone ? zone : "");
    }
}

static void
show_repeat (Editor *editor)
{
  g_autofree char *text = editor->rrule ? cal_format_rrule (editor->rrule, start_time (editor)) : NULL;

  /* EditEventView.populateRepeats: a rule without words is "Custom (can't customize on phone)" and the button is off,
   * as it is for one the dialog cannot show (canHandleRecurrenceRule); either stays as it is, made elsewhere */
  set_button_text (editor->repeat, text ? text : editor->rrule ? _("Custom (can't customize on phone)")
                                                               : _("One-time event"));
  gtk_widget_set_sensitive (editor->repeat, !editor->rrule
                                            || (text && cal_recurrence_picker_can_handle (editor->rrule)));
}

/* the row of @widget: its ancestor that is a child of the editor's content */
static GtkWidget *
top_row (Editor *editor, GtkWidget *widget)
{
  while (widget && gtk_widget_get_parent (widget) != editor->content)
    widget = gtk_widget_get_parent (widget);
  return widget;
}

static void
show_row (Editor *editor, GtkWidget *widget, gboolean shown)
{
  GtkWidget *line = top_row (editor, widget);
  GtkWidget *next = line ? gtk_widget_get_next_sibling (line) : NULL;

  if (!line)
    return;
  gtk_widget_set_visible (line, shown);
  if (next && gtk_widget_has_css_class (next, "cal-edit-separator"))
    gtk_widget_set_visible (next, shown);
}

/* The Minerva calendar takes what the scheduler does: a text, when, a repeat, and what kind of task it is; the
 * place, the guests, the zone, the reminders, the colour, availability and privacy are an EDS event's only */
static void
show_minerva (Editor *editor)
{
  gboolean minerva = is_minerva (editor);
  JsonObject *ui = cal_store_minerva_ui (editor->store);
  GtkWidget *eds_only[] = { editor->location, editor->url, editor->guests, editor->tz_row, editor->reminders,
                            editor->add_reminder, editor->availability, editor->access, editor->description };

  if (!editor->content)
    return;
  for (guint i = 0; i < G_N_ELEMENTS (eds_only); i++)
    if (eds_only[i])
      show_row (editor, eds_only[i], !minerva);
  gtk_widget_set_visible (editor->color_button, !minerva);
  /* a task is due at a time: All day off (its handler shows the times again), then the line and the end hidden */
  if (minerva && editor->all_day && editor->all_day_switch)
    cal_material_switch_set_active (CAL_MATERIAL_SWITCH (editor->all_day_switch), FALSE);
  if (editor->all_day_line)
    gtk_widget_set_visible (editor->all_day_line, !minerva);
  if (editor->guests_helper)
    gtk_widget_set_visible (editor->guests_helper, !minerva);
  if (editor->start_date)
    show_times (editor);
  if (editor->kind)
    show_row (editor, editor->kind, minerva);
  gtk_entry_set_placeholder_text (GTK_ENTRY (editor->title), minerva && ui ? ns_json_str (ui, "text_hint_prompt")
                                                                          : _("Event name"));
}

/* the account is said only when it adds something: a calendar named as its account ("Minerva") is its name alone */
static gboolean
has_own_account (const CalCalendar *calendar)
{
  return calendar->account && *calendar->account && g_strcmp0 (calendar->account, calendar->name) != 0;
}

static void
show_calendar (Editor *editor)
{
  const CalCalendar *calendar = cal_store_find_calendar (editor->store, editor->calendar);
  GdkRGBA color;

  gtk_label_set_label (GTK_LABEL (editor->calendar_name), calendar ? calendar->name : "");
  gtk_label_set_label (GTK_LABEL (editor->calendar_account), calendar ? calendar->account : "");
  gtk_widget_set_visible (editor->calendar_account, calendar && has_own_account (calendar));
  /* the square shows the event's colour when it has its own, else the calendar's (setSpinnerBackgroundColor) */
  color = cal_argb (0xff000000 | (editor->own_color ? editor->color : calendar ? calendar->color : 0x3f51b5));
  g_object_set_data_full (G_OBJECT (editor->calendar_color), "cal-color", g_memdup2 (&color, sizeof color), g_free);
  gtk_widget_queue_draw (editor->calendar_color);
  show_minerva (editor);
}

/* --- the values the user changed (EditEventFragment.isModelChanged) */

static char *
entry_text (GtkWidget *entry)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
}

static char *
description_text (Editor *editor)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (editor->description));
  GtkTextIter a, b;

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  return g_strstrip (gtk_text_buffer_get_text (buffer, &a, &b, FALSE));
}

/* the guests typed (RecipientEditTextView's text): addresses split at commas, semicolons and spaces, each once */
static char **
typed_guests (Editor *editor)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (editor->guests));
  g_auto(GStrv) parts = g_regex_split_simple ("[,;\\s]+", text, 0, 0);
  g_autoptr(GPtrArray) out = g_ptr_array_new ();

  for (char **p = parts; *p; p++)
    {
      gboolean seen = FALSE;

      if (!**p || !strchr (*p, '@'))
        continue;
      for (guint i = 0; i < out->len && !seen; i++)
        seen = g_ascii_strcasecmp (out->pdata[i], *p) == 0;
      if (!seen)
        g_ptr_array_add (out, g_strdup (*p));
    }
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&out), FALSE);
}

/* a guest the field lists: not the organizer, not the calendar's owner (EditEventHelper leaves both out) */
static gboolean
listed_guest (Editor *editor, const CalAttendee *guest)
{
  const CalCalendar *calendar = cal_store_find_calendar (editor->store, editor->calendar);

  return !guest->organizer
         && !(calendar && calendar->email && g_ascii_strcasecmp (guest->email, calendar->email) == 0);
}

static CalEvent *
edited_event (Editor *editor)
{
  CalEvent *event = cal_event_new ();
  CalEvent *source = editor->source;

  event->calendar = g_strdup (editor->calendar);
  event->uid = g_strdup (editor->original ? editor->original->uid : NULL);
  event->rid = g_strdup (editor->original ? editor->original->rid : NULL);
  event->title = entry_text (editor->title);
  event->location = entry_text (editor->location);
  event->description = description_text (editor);
  event->url = entry_text (editor->url);
  event->all_day = editor->all_day;
  event->start = start_time (editor);
  event->end = MAX (end_time (editor), event->start);
  /* a rule chosen here, or All day switched under one: its end of the kind the start now is (a rule left alone
   * keeps its text, as another client wrote it) */
  event->rrule = !source || source->all_day != editor->all_day || g_strcmp0 (source->rrule, editor->rrule) != 0
                 ? cal_recurrence_fit_until (editor->rrule, event->start, editor->all_day ? NULL : editor->tzid)
                 : g_strdup (editor->rrule);
  event->recurring = editor->rrule != NULL;
  event->color = editor->color;
  event->own_color = editor->own_color;
  /* the zone: the one chosen, else the event's own (a UTC or floating time stays so) */
  event->tzid = editor->all_day ? NULL : g_strdup (editor->tz_changed || !source ? editor->tzid : source->tzid);
  event->availability = cal_spinner_get_selected (CAL_SPINNER (editor->availability)) == 1 ? CAL_AVAILABILITY_FREE
                                                                                            : CAL_AVAILABILITY_BUSY;
  event->access = (CalAccess) MAX (0, cal_spinner_get_selected (CAL_SPINNER (editor->access)));
  if (source)
    {
      event->organizer = g_strdup (source->organizer);
      event->self_status = source->self_status;
    }
  if (g_strcmp0 (editor->calendar, CAL_MINERVA_CALENDAR) == 0)
    {
      JsonArray *kinds = ns_json_arr (cal_store_minerva_ui (editor->store), "kinds");
      int k = editor->kind ? cal_spinner_get_selected (CAL_SPINNER (editor->kind)) : 0;

      event->minerva = source && source->minerva ? cal_minerva_info_copy (source->minerva)
                                                 : g_new0 (CalMinervaInfo, 1);
      if (kinds && k >= 0 && k < (int) json_array_get_length (kinds))
        {
          g_free (event->minerva->kind);
          event->minerva->kind = g_strdup (json_array_get_string_element (kinds, (guint) k));
        }
    }
  /* the guests: the organizer and the owner as they were, those still typed as they were (their answers kept),
   * the new ones not answered yet */
  {
    g_auto(GStrv) typed = typed_guests (editor);

    for (guint i = 0; source && i < source->attendees->len; i++)
      {
        const CalAttendee *guest = source->attendees->pdata[i];

        if (!listed_guest (editor, guest) || g_strv_contains ((const char *const *) typed, guest->email))
          g_ptr_array_add (event->attendees, cal_attendee_copy (guest));
      }
    for (char **t = typed; *t; t++)
      {
        gboolean known = FALSE;

        for (guint i = 0; i < event->attendees->len && !known; i++)
          known = g_ascii_strcasecmp (((CalAttendee *) event->attendees->pdata[i])->email, *t) == 0;
        if (!known)
          {
            CalAttendee *guest = g_new0 (CalAttendee, 1);

            guest->email = g_strdup (*t);
            guest->status = CAL_STATUS_INVITED;
            g_ptr_array_add (event->attendees, guest);
          }
      }
  }
  g_array_unref (event->reminders);
  event->reminders = cal_reminders_get (CAL_REMINDERS (editor->reminders));
  return event;
}

static gboolean
text_equal (const char *a, const char *b)
{
  return g_strcmp0 (a && *a ? a : NULL, b && *b ? b : NULL) == 0;
}

static gboolean
changed (Editor *editor)
{
  g_autoptr(CalEvent) now = edited_event (editor);
  CalEvent *before = editor->source;

  if (!before)
    {
      /* a new event: anything typed counts, the times and the default reminder do not */
      return (now->title && *now->title) || (now->location && *now->location)
             || (now->description && *now->description) || (now->url && *now->url) || now->attendees->len > 0;
    }
  if (now->own_color != before->own_color || (now->own_color && now->color != before->color)
      || now->availability != before->availability || now->access != before->access
      || g_strcmp0 (now->tzid, before->tzid) != 0 || now->attendees->len != before->attendees->len)
    return TRUE;
  for (guint i = 0; i < now->attendees->len; i++)
    if (g_ascii_strcasecmp (((CalAttendee *) now->attendees->pdata[i])->email,
                            ((CalAttendee *) before->attendees->pdata[i])->email) != 0)
      return TRUE;
  if (!text_equal (now->title, before->title) || !text_equal (now->location, before->location)
      || !text_equal (now->description, before->description) || !text_equal (now->url, before->url)
      || !text_equal (now->rrule, before->rrule) || now->all_day != before->all_day || now->start != before->start
      || now->end != before->end || g_strcmp0 (now->calendar, before->calendar) != 0)
    return TRUE;
  if (now->reminders->len != before->reminders->len)
    return TRUE;
  for (guint i = 0; i < now->reminders->len; i++)
    if (g_array_index (now->reminders, CalReminder, i).minutes
        != g_array_index (before->reminders, CalReminder, i).minutes)
      return TRUE;
  return FALSE;
}

/* --- Done and Cancel */

static void
on_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  g_autoptr(GError) error = NULL;

  if (!cal_store_save_finish (CAL_STORE (source), result, &error))
    {
      g_warning ("save: %s", error->message);
      cal_window_toast (window, error->message);
    }
  g_object_unref (window);
}

static void
done (Editor *editor)
{
  g_autoptr(CalEvent) edited = edited_event (editor);
  gboolean is_new = editor->original == NULL;

  /* EditEventFragment.onActionBarItemSelected: a new event with nothing in it is not made */
  if (is_new && !(edited->title && *edited->title) && !(edited->location && *edited->location)
      && !(edited->description && *edited->description) && edited->attendees->len == 0)
    {
      cal_window_toast (editor->window, _("Empty event not created."));
      cal_window_pop (editor->window);
      return;
    }
  if (!is_new && !changed (editor))
    {
      cal_window_pop (editor->window);
      return;
    }
  if (is_new)
    g_settings_set_string (editor->settings, "last-calendar", editor->calendar);
  cal_store_save (editor->store, editor->original, edited, editor->scope, on_saved, g_object_ref (editor->window));
  cal_window_toast (editor->window, is_new ? _("Event created.") : _("Event saved."));
  cal_window_pop (editor->window);
}

static void
on_done (NeoPressable *button, Editor *editor)
{
  done (editor);
}

static void
on_discard_answer (int index, gpointer data)
{
  Editor *editor = data;

  if (index == 0)
    cal_window_pop (editor->window);
  g_object_unref (editor->page);
}

/* Back and Cancel: "Do you want to discard changes?" when something changed */
static gboolean
on_back (CalPage *page, gpointer data)
{
  Editor *editor = data;

  if (!changed (editor))
    return FALSE;
  cal_confirm_dialog (editor->page, _("Do you want to discard changes?"), _("Discard"), on_discard_answer,
                      g_object_ref (editor->page));
  return TRUE;
}

static void
on_cancel (NeoPressable *button, Editor *editor)
{
  cal_page_back (CAL_PAGE (editor->page));
}

/* --- the calendar */

static void
on_calendar_chosen (int index, gpointer data)
{
  Editor *editor = data;
  g_autoptr(GPtrArray) writable = g_ptr_array_new ();
  GPtrArray *calendars = cal_store_get_calendars (editor->store);

  for (guint i = 0; i < calendars->len; i++)
    if (!((CalCalendar *) calendars->pdata[i])->readonly)
      g_ptr_array_add (writable, calendars->pdata[i]);
  if (index < 0 || index >= (int) writable->len)
    return;
  g_free (editor->calendar);
  editor->calendar = g_strdup (((CalCalendar *) writable->pdata[index])->uid);
  show_calendar (editor);
}

static void
on_calendar (NeoPressable *row, Editor *editor)
{
  GPtrArray *calendars = cal_store_get_calendars (editor->store);
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  int selected = -1;

  /* the writable calendars, as Etar's calendars spinner lists them (name, then account) */
  for (guint i = 0; i < calendars->len; i++)
    {
      CalCalendar *calendar = calendars->pdata[i];

      if (calendar->readonly)
        continue;
      if (g_strcmp0 (calendar->uid, editor->calendar) == 0)
        selected = (int) labels->len;
      g_ptr_array_add (labels, has_own_account (calendar)
                               ? g_strdup_printf ("%s (%s)", calendar->name, calendar->account)
                               : g_strdup (calendar->name));
    }
  cal_popup_show (GTK_WIDGET (row), (const char *const *) labels->pdata, (int) labels->len, selected, 0, FALSE,
                  on_calendar_chosen, editor);
}

static void
draw_calendar_color (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  GdkRGBA *color = g_object_get_data (G_OBJECT (area), "cal-color");

  if (!color)
    return;
  gdk_cairo_set_source_rgba (cr, color);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_fill (cr);
}

/* --- dates and times (EditEventView.DateListener, TimeListener) */

static void
on_all_day (GObject *object, GParamSpec *pspec, Editor *editor)
{
  gboolean all_day = cal_material_switch_get_active (CAL_MATERIAL_SWITCH (object));

  if (all_day == editor->all_day)
    return;
  editor->all_day = all_day;
  if (all_day)
    {
      /* an end at midnight is the day before's end */
      if (editor->end_minute == 0 && editor->end_day > editor->start_day)
        editor->end_day--;
    }
  else if (editor->end_minute == 0 && editor->end_day >= editor->start_day
           && !(editor->end_day == editor->start_day && editor->end_minute > editor->start_minute))
    {
      /* back to times: the end at midnight after the last day */
      if (editor->start_minute == 0)
        editor->end_day++;
    }
  if (editor->end_day < editor->start_day)
    editor->end_day = editor->start_day;
  show_times (editor);
}

static void
on_start_date (int day, gpointer data)
{
  Editor *editor = data;
  int shift = day - editor->start_day;

  /* the end keeps the duration */
  editor->start_day = day;
  editor->end_day += shift;
  show_times (editor);
  show_repeat (editor);
}

static void
on_end_date (int day, gpointer data)
{
  Editor *editor = data;

  /* an end before the start is the start's date */
  editor->end_day = MAX (day, editor->start_day);
  if (editor->end_day == editor->start_day && !editor->all_day && editor->end_minute < editor->start_minute)
    editor->end_minute = editor->start_minute;
  show_times (editor);
}

static void
on_start_minute (int hour, int minute, gpointer data)
{
  Editor *editor = data;
  gint64 duration = local_time (editor, editor->end_day, editor->end_minute)
                    - local_time (editor, editor->start_day, editor->start_minute);
  gint64 end;

  editor->start_minute = hour * 60 + minute;
  end = local_time (editor, editor->start_day, editor->start_minute) + duration;
  split_time (editor, end, &editor->end_day, &editor->end_minute);
  show_times (editor);
}

static void
on_end_minute (int hour, int minute, gpointer data)
{
  Editor *editor = data;

  editor->end_minute = hour * 60 + minute;
  /* an end time before the start on the same day: the next day */
  if (editor->end_day == editor->start_day && editor->end_minute < editor->start_minute)
    editor->end_day++;
  show_times (editor);
}

static int
week_start (Editor *editor)
{
  /* MaterialDatePicker starts the week on the locale's first day */
  return cal_week_start (-1);
}

/* EditEventView titles its date pickers "Go to…" as well (R.string.goto_date, :1681) */
static void
on_start_date_clicked (NeoPressable *button, Editor *editor)
{
  cal_date_picker_show (editor->page, _("Go to…"), editor->start_day, week_start (editor), on_start_date,
                        editor);
}

static void
on_end_date_clicked (NeoPressable *button, Editor *editor)
{
  cal_date_picker_show (editor->page, _("Go to…"), editor->end_day, week_start (editor), on_end_date, editor);
}

static void
on_start_time_clicked (NeoPressable *button, Editor *editor)
{
  cal_time_picker_show (editor->page, editor->start_minute / 60, editor->start_minute % 60, on_start_minute, editor);
}

static void
on_end_time_clicked (NeoPressable *button, Editor *editor)
{
  cal_time_picker_show (editor->page, editor->end_minute / 60, editor->end_minute % 60, on_end_minute, editor);
}

/* --- repeat (RecurrencePickerDialog) */

static void
on_repeat_chosen (const char *rrule, gpointer data)
{
  Editor *editor = data;

  /* onRecurrenceSet: the rule, or none ("does not repeat") */
  g_free (editor->rrule);
  editor->rrule = g_strdup (rrule);
  show_repeat (editor);
}

static void
on_repeat (NeoPressable *button, Editor *editor)
{
  /* EditEventView.onClick: the start, its zone (an all-day event's dates have none) and the rule */
  cal_recurrence_picker_show (editor->page, editor->rrule, start_time (editor), editor->all_day ? NULL : editor->tzid,
                              on_repeat_chosen, editor);
}

/* --- reminders */

static void
on_reminders_changed (CalReminders *reminders, Editor *editor)
{
  gtk_widget_set_visible (editor->add_reminder, cal_reminders_count (reminders) < CAL_MAX_REMINDERS);
}

static void
on_add_reminder (NeoPressable *button, Editor *editor)
{
  int minutes = g_settings_get_int (editor->settings, "default-reminder");

  cal_reminders_add (CAL_REMINDERS (editor->reminders), minutes >= 0 ? minutes : 10);
}

/* --- building it */

/* the event's colour (change_color → EventColorPickerDialog): Etar's palette, as for an account without one of
 * its own; "Set to default calendar color" goes back to following the calendar */
static void
on_color_chosen (guint32 rgb, gpointer data)
{
  Editor *editor = data;
  const CalCalendar *calendar = cal_store_find_calendar (editor->store, editor->calendar);

  editor->color = rgb;
  editor->own_color = !calendar || rgb != calendar->color;
  show_calendar (editor);
}

static void
on_color (NeoPressable *button, Editor *editor)
{
  const CalCalendar *calendar = cal_store_find_calendar (editor->store, editor->calendar);
  guint32 calendar_color = calendar ? calendar->color : 0x3f51b5;
  int n_colors;
  const guint32 *colors = cal_color_picker_default_colors (&n_colors);

  cal_event_color_picker_show (editor->window, colors, n_colors, editor->own_color ? editor->color : calendar_color,
                               calendar_color, on_color_chosen, editor);
}

/* the time zone (TimeZonePickerDialog): the times stay on the clock, in the zone chosen */
static void
on_tz_chosen (const char *tzid, gpointer data)
{
  Editor *editor = data;

  if (!tzid || g_strcmp0 (tzid, editor->tzid) == 0)
    return;
  g_free (editor->tzid);
  editor->tzid = g_strdup (tzid);
  editor->tz_changed = TRUE;
  show_times (editor);
}

static void
on_tz (NeoPressable *button, Editor *editor)
{
  cal_tz_picker_show (editor->window, editor->tzid, start_time (editor), on_tz_chosen, editor);
}

static GtkWidget *
separator (void)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (line, "cal-edit-separator");
  return line;
}

/* a row: the 24 dp icon 12 dp in, its content from 48 dp */
static GtkWidget *
row (const char *icon_name, GtkWidget *content)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "cal-edit-icon");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_hexpand (content, TRUE);
  gtk_box_append (GTK_BOX (box), content);
  gtk_widget_add_css_class (box, "cal-edit-row");
  return box;
}

/* an M3 outlined button: a pill, its text in colorPrimary */
static GtkWidget *
outlined_button (const char *name, GCallback callback, gpointer data)
{
  GtkWidget *label = gtk_label_new (NULL);
  GtkWidget *button = neo_pressable_new (label);

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  /* centred as a TextView with includeFontPadding is (the font's bounding box, not its line) */
  neo_label_set_font_padding (label, 14);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (button, "cal-outlined");
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 20);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, name, -1);
  gtk_widget_set_halign (button, GTK_ALIGN_START);
  g_signal_connect (button, "clicked", callback, data);
  return button;
}

/* an M3 outlined text field (a TextInputLayout's OutlinedBox) with @hint as its label, its text in *@text_widget
 * (a GtkText, or a GtkTextView for lines that wrap). The padding is 15 dp on every side: EditEventView gives every
 * field it edits the location's. Guests (RecipientEditTextView) and Location (AutoCompleteTextView) are 18 sp, the
 * others (EditText under the box's theme overlay) bodyLarge, 16 sp with 0.5 sp letter spacing: the stills' boxes
 * are 163 and 155 px tall. */
static GtkWidget *
text_field (const char *hint, const char *text, gboolean body, gboolean wrap, GtkWidget **text_widget)
{
  GtkWidget *child;

  if (wrap)
    {
      child = gtk_text_view_new ();
      gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (child), GTK_WRAP_WORD_CHAR);
      gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (child), FALSE);
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (child)), text ? text : "", -1);
    }
  else
    {
      child = gtk_text_new ();
      gtk_editable_set_text (GTK_EDITABLE (child), text ? text : "");
    }
  gtk_widget_add_css_class (child, body ? "cal-field-body" : "cal-field-text");
  *text_widget = child;
  return cal_outlined_field_new (child, hint, body ? 16 : 18, body ? 0.5 : 0, 15);
}

/* a field's row: the icon 14 dp over the box's bottom, the box from 48 dp to 24 dp from the end */
static GtkWidget *
field_row (const char *icon_name, GtkWidget *field)
{
  GtkWidget *line = row (icon_name, field);
  GtkWidget *icon = gtk_widget_get_first_child (line);

  gtk_widget_set_valign (icon, GTK_ALIGN_END);
  gtk_widget_add_css_class (icon, "cal-edit-field-icon");
  gtk_widget_add_css_class (field, "cal-edit-field");
  return line;
}

static GtkWidget *
action_button (const char *icon_name, const char *label, GCallback callback, Editor *editor)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *button;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 36);     /* ic_menu_cancel, ic_menu_done: 36 dp */
  gtk_widget_add_css_class (icon, "cal-edit-action-icon");
  gtk_widget_add_css_class (text, "cal-edit-action-text");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  button = neo_pressable_new (box);
  gtk_widget_set_hexpand (button, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 0);
  g_signal_connect (button, "clicked", callback, editor);
  return button;
}

static GtkWidget *
title_bar (Editor *editor)
{
  GtkWidget *bar = gtk_center_box_new ();
  GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *back = neo_icon_button_new ("cal-back-symbolic");

  /* edit_event_custom_actionbar.xml: "✕ CANCEL" and "✓ DONE" in two equal halves (weightSum 2) of the toolbar
   * after its 16 dp content inset, the divider between (measured: at x 562, the halves centred at 305 and 822) */
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (bar),
                                   action_button ("cal-cancel-symbolic", _("CANCEL"), G_CALLBACK (on_cancel), editor));
  gtk_widget_add_css_class (divider, "cal-edit-action-divider");
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (bar), divider);
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (bar),
                                 action_button ("cal-done-symbolic", _("DONE"), G_CALLBACK (on_done), editor));
  gtk_widget_add_css_class (bar, "cal-edit-bar");
  gtk_overlay_set_child (GTK_OVERLAY (overlay), bar);
  /* An invisible "Back" for the shell's back gesture, which presses the button of that name in the top band (Etar's
   * Back here is Cancel's: ask before throwing changes away). Not drawn: the bar has no arrow. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (back), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  gtk_widget_set_opacity (back, 0);
  gtk_widget_set_halign (back, GTK_ALIGN_START);
  gtk_widget_set_valign (back, GTK_ALIGN_START);
  gtk_widget_set_can_target (back, FALSE);
  g_signal_connect (back, "clicked", G_CALLBACK (on_cancel), editor);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), back);
  return overlay;
}

static void
build (Editor *editor)
{
  GtkWidget *page = editor->page, *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *box, *label, *calendar_text, *dates, *line, *field;
  CalEvent *source = editor->source;
  gboolean new_event = editor->original == NULL;

  gtk_box_append (GTK_BOX (page), title_bar (editor));

  /* the title: "Event name", 20 sp, from 48 dp */
  editor->title = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (editor->title), _("Event name"));
  gtk_editable_set_text (GTK_EDITABLE (editor->title), source && source->title ? source->title : "");
  gtk_widget_add_css_class (editor->title, "cal-edit-title");
  gtk_box_append (GTK_BOX (content), editor->title);
  gtk_box_append (GTK_BOX (content), separator ());
  editor->content = content;

  /* the Minerva calendar's kind of task: a prompt to Minerva or a reminder (its texts from minerva-config) */
  {
    JsonObject *ui = cal_store_minerva_ui (editor->store);
    JsonArray *kinds = ui ? ns_json_arr (ui, "kinds") : NULL;
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
    int selected = 0;

    for (guint i = 0; kinds && i < json_array_get_length (kinds); i++)
      {
        const char *kind = json_array_get_string_element (kinds, i);
        g_autofree char *key = g_strdup_printf ("kind_%s", kind);

        g_ptr_array_add (labels, g_strdup (ns_json_str (ui, key)));
        if (source && source->minerva && g_strcmp0 (source->minerva->kind, kind) == 0)
          selected = (int) i;
      }
    if (labels->len)
      {
        editor->kind = cal_spinner_new ();
        cal_spinner_set_items (CAL_SPINNER (editor->kind), (const char *const *) labels->pdata, (int) labels->len);
        cal_spinner_set_selected (CAL_SPINNER (editor->kind), selected);
        line = row ("cal-notify-symbolic", editor->kind);
        gtk_widget_add_css_class (line, "cal-edit-spinner-row");
        gtk_box_append (GTK_BOX (content), line);
        gtk_box_append (GTK_BOX (content), separator ());
      }
  }

  /* the calendar: chosen for a new event; an existing one stays in its calendar (Etar shows it as text) */
  calendar_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  editor->calendar_name = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (editor->calendar_name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (editor->calendar_name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (editor->calendar_name, "cal-edit-calendar-name");
  editor->calendar_account = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (editor->calendar_account), 0);
  gtk_label_set_ellipsize (GTK_LABEL (editor->calendar_account), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (editor->calendar_account, "cal-edit-calendar-account");
  /* textAppearanceMedium over textAppearanceSmall: 24.3 dp and 19 dp with the font padding, 43 dp in all */
  neo_label_set_font_padding (editor->calendar_name, 18);
  neo_label_set_font_padding (editor->calendar_account, 14);
  gtk_box_append (GTK_BOX (calendar_text), editor->calendar_name);
  gtk_box_append (GTK_BOX (calendar_text), editor->calendar_account);
  {
    GtkWidget *icon_box = gtk_overlay_new ();
    GtkWidget *icon = gtk_image_new_from_icon_name ("cal-calendar-symbolic");

    gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
    gtk_widget_add_css_class (icon, "cal-edit-icon");
    gtk_overlay_set_child (GTK_OVERLAY (icon_box), icon);
    /* calendar_selector_group_background: the calendar's colour in a 12 dp square at the icon's corner */
    editor->calendar_color = gtk_drawing_area_new ();
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (editor->calendar_color), draw_calendar_color, NULL, NULL);
    gtk_widget_set_size_request (editor->calendar_color, 12, 12);
    gtk_widget_set_halign (editor->calendar_color, GTK_ALIGN_END);
    gtk_widget_set_valign (editor->calendar_color, GTK_ALIGN_END);
    gtk_widget_add_css_class (editor->calendar_color, "cal-edit-calendar-color");
    gtk_overlay_add_overlay (GTK_OVERLAY (icon_box), editor->calendar_color);
    gtk_widget_set_valign (icon_box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (icon_box, "cal-edit-calendar-icon");
    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (box), icon_box);
    gtk_widget_set_hexpand (calendar_text, TRUE);
    gtk_box_append (GTK_BOX (box), calendar_text);
    gtk_widget_add_css_class (box, "cal-edit-row");
    gtk_widget_add_css_class (box, "cal-edit-calendar");
  }
  {
    /* change_color: outline_color_lens drawn twice its size (scaleX/Y 2) in a 48 dp button, 20 dp from the end;
     * a writable calendar takes an event colour (the editor only opens on one) */
    GtkWidget *lens = gtk_image_new_from_icon_name ("cal-color-symbolic");

    gtk_image_set_pixel_size (GTK_IMAGE (lens), 48);
    editor->color_button = neo_pressable_new (lens);
    gtk_widget_add_css_class (editor->color_button, "cal-edit-color");
    gtk_widget_set_valign (editor->color_button, GTK_ALIGN_CENTER);
    neo_pressable_set_ripple (NEO_PRESSABLE (editor->color_button), NEO_RIPPLE_CIRCLE, 24);
    gtk_accessible_update_property (GTK_ACCESSIBLE (editor->color_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("Choose event color"), -1);
    g_signal_connect (editor->color_button, "clicked", G_CALLBACK (on_color), editor);
  }
  if (new_event)
    {
      editor->calendar_row = neo_pressable_new (box);
      neo_pressable_set_ripple (NEO_PRESSABLE (editor->calendar_row), NEO_RIPPLE_BOUNDED, 0);
      gtk_accessible_update_property (GTK_ACCESSIBLE (editor->calendar_row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      _("Calendar"), -1);
      g_signal_connect (editor->calendar_row, "clicked", G_CALLBACK (on_calendar), editor);
    }
  else
    editor->calendar_row = box;
  {
    /* the colour button sits over the row's end, outside the calendar's pressable area */
    GtkWidget *overlay = gtk_overlay_new ();

    gtk_overlay_set_child (GTK_OVERLAY (overlay), editor->calendar_row);
    gtk_widget_set_halign (editor->color_button, GTK_ALIGN_END);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), editor->color_button);
    gtk_box_append (GTK_BOX (content), overlay);
  }
  gtk_box_append (GTK_BOX (content), separator ());

  /* All day, and the dates and times */
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  label = gtk_label_new (_("All day"));
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "cal-edit-label");
  gtk_box_append (GTK_BOX (box), label);
  editor->all_day_switch = cal_material_switch_new ();
  cal_material_switch_set_active (CAL_MATERIAL_SWITCH (editor->all_day_switch), editor->all_day);
  gtk_accessible_update_property (GTK_ACCESSIBLE (editor->all_day_switch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("All day event"), -1);
  g_signal_connect (editor->all_day_switch, "notify::active", G_CALLBACK (on_all_day), editor);
  {
    /* The row is the icon's 24 dp line (16 dp under the separator, the buttons 8 dp under it); the 32 dp switch
     * overhangs it, centred a dp under the line's middle, 24 dp from the end. An overlay, so it adds no height. */
    GtkWidget *overlay = gtk_overlay_new ();

    neo_label_set_font_padding (label, 18);
    line = row ("cal-schedule-symbolic", box);
    gtk_widget_add_css_class (line, "cal-edit-all-day");
    gtk_overlay_set_child (GTK_OVERLAY (overlay), line);
    gtk_widget_set_halign (editor->all_day_switch, GTK_ALIGN_END);
    gtk_widget_set_valign (editor->all_day_switch, GTK_ALIGN_START);
    gtk_widget_add_css_class (editor->all_day_switch, "cal-edit-all-day-switch");
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), editor->all_day_switch);
    gtk_box_append (GTK_BOX (content), overlay);
    editor->all_day_line = overlay;
  }

  dates = gtk_grid_new ();
  gtk_widget_add_css_class (dates, "cal-edit-dates");
  editor->start_date = outlined_button (_("Start date"), G_CALLBACK (on_start_date_clicked), editor);
  editor->start_time = outlined_button (_("Start time"), G_CALLBACK (on_start_time_clicked), editor);
  editor->end_date = outlined_button (_("End date"), G_CALLBACK (on_end_date_clicked), editor);
  editor->end_time = outlined_button (_("End time"), G_CALLBACK (on_end_time_clicked), editor);
  gtk_grid_attach (GTK_GRID (dates), editor->start_date, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dates), editor->start_time, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dates), editor->end_date, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dates), editor->end_time, 1, 1, 1, 1);
  gtk_box_append (GTK_BOX (content), dates);

  /* the time zone: an outlined button over the row's width, the name and its GMT offset (timezone_button) */
  editor->tz_button = outlined_button (_("Time zone"), G_CALLBACK (on_tz), editor);
  {
    GtkWidget *tz_label = neo_pressable_get_child (NEO_PRESSABLE (editor->tz_button));

    /* its lines are spaced by the stylesheet's line height instead (lineSpacingExtra) */
    gtk_widget_set_margin_top (tz_label, 0);
    gtk_widget_set_margin_bottom (tz_label, 0);
    gtk_label_set_ellipsize (GTK_LABEL (tz_label), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_wrap (GTK_LABEL (tz_label), TRUE);
    gtk_label_set_xalign (GTK_LABEL (tz_label), 0);
  }
  gtk_widget_set_halign (editor->tz_button, GTK_ALIGN_FILL);
  gtk_widget_add_css_class (editor->tz_button, "cal-edit-tz");
  editor->tz_row = row ("cal-public-symbolic", editor->tz_button);
  gtk_box_append (GTK_BOX (content), editor->tz_row);
  /* view2, 8 dp under the button (view7, the response and organizer rows above it are a new event's GONE ones) */
  line = separator ();
  gtk_widget_add_css_class (line, "cal-edit-separator-tz");
  gtk_box_append (GTK_BOX (content), line);

  /* guests (RecipientEditTextView, "Guests"): their addresses are kept with the event. Nothing invites them: the
   * guests go in with SCHEDULE-AGENT=CLIENT and no ORGANIZER, so no server mails them either, and the field says
   * so under it. */
  {
    GString *text = g_string_new (NULL);
    GtkWidget *helper = gtk_label_new (_("Guests are not sent an invitation"));

    for (guint i = 0; source && i < source->attendees->len; i++)
      {
        const CalAttendee *guest = source->attendees->pdata[i];

        if (listed_guest (editor, guest))
          g_string_append_printf (text, "%s%s", text->len ? ", " : "", guest->email);
      }
    field = text_field (_("Guests"), text->str, FALSE, FALSE, &editor->guests);
    g_string_free (text, TRUE);
    gtk_text_set_input_purpose (GTK_TEXT (editor->guests), GTK_INPUT_PURPOSE_EMAIL);
    line = field_row ("cal-person-add-symbolic", field);
    /* add_attendees_row: 10 dp under the separator's top */
    gtk_widget_add_css_class (line, "cal-edit-guests");
    gtk_box_append (GTK_BOX (content), line);
    /* the helper text under the box (IndicatorViewController): at the text's start, 4 dp under the box */
    gtk_label_set_xalign (GTK_LABEL (helper), 0);
    gtk_label_set_wrap (GTK_LABEL (helper), TRUE);
    neo_label_set_font_padding (helper, 12);
    gtk_widget_add_css_class (helper, "cal-text-helper");
    gtk_box_append (GTK_BOX (content), helper);
    editor->guests_helper = helper;
  }

  /* location, description, URL (one under the other, no space between the boxes' layouts) */
  field = text_field (_("Location"), source ? source->location : NULL, FALSE, FALSE, &editor->location);
  gtk_box_append (GTK_BOX (content), field_row ("cal-place-symbolic", field));
  field = text_field (_("Description"), source ? source->description : NULL, TRUE, TRUE, &editor->description);
  gtk_box_append (GTK_BOX (content), field_row ("cal-notes-symbolic", field));
  field = text_field (_("URL"), source ? source->url : NULL, TRUE, FALSE, &editor->url);
  gtk_text_set_input_purpose (GTK_TEXT (editor->url), GTK_INPUT_PURPOSE_URL);
  line = field_row ("cal-link-symbolic", field);
  /* view3 12 dp under it */
  gtk_widget_add_css_class (line, "cal-edit-url");
  gtk_box_append (GTK_BOX (content), line);
  gtk_box_append (GTK_BOX (content), separator ());

  /* repeat: not for one occurrence of a series (it is an exception, it repeats nothing) */
  editor->repeat = outlined_button (_("Repeat event"), G_CALLBACK (on_repeat), editor);
  gtk_widget_set_hexpand (editor->repeat, TRUE);
  gtk_widget_set_halign (editor->repeat, GTK_ALIGN_FILL);
  editor->repeat_row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  /* rrule_icon 20 dp under view3 and over view4, the button (marginEnd 24 dp) centred on it */
  gtk_widget_add_css_class (editor->repeat, "cal-edit-repeat-button");
  line = row ("cal-repeat-symbolic", editor->repeat);
  gtk_widget_add_css_class (line, "cal-edit-repeat");
  gtk_box_append (GTK_BOX (editor->repeat_row), line);
  gtk_box_append (GTK_BOX (editor->repeat_row), separator ());
  gtk_widget_set_visible (editor->repeat_row, !(editor->original && editor->scope == CAL_SCOPE_THIS
                                                && editor->original->recurring));
  gtk_box_append (GTK_BOX (content), editor->repeat_row);

  /* reminders */
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  editor->reminders = cal_reminders_new ();
  g_signal_connect (editor->reminders, "changed", G_CALLBACK (on_reminders_changed), editor);
  gtk_box_append (GTK_BOX (box), editor->reminders);
  label = gtk_label_new (_("Add reminder"));
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  editor->add_reminder = neo_pressable_new (label);
  gtk_widget_add_css_class (editor->add_reminder, "cal-add-reminder");
  neo_pressable_set_ripple (NEO_PRESSABLE (editor->add_reminder), NEO_RIPPLE_BOUNDED, 4);
  g_signal_connect (editor->add_reminder, "clicked", G_CALLBACK (on_add_reminder), editor);
  gtk_box_append (GTK_BOX (box), editor->add_reminder);
  line = row ("cal-notifications-symbolic", box);
  gtk_widget_add_css_class (line, "cal-edit-reminders");
  gtk_box_append (GTK_BOX (content), line);
  gtk_box_append (GTK_BOX (content), separator ());

  /* Show me as (availability: TRANSP): Busy, Available; Tentative is left out as a calendar's
   * ALLOWED_AVAILABILITY leaves it out, iCalendar's TRANSP having only those two */
  {
    const char *labels[] = { _("Busy"), _("Available") };

    editor->availability = cal_spinner_new ();
    cal_spinner_set_items (CAL_SPINNER (editor->availability), labels, G_N_ELEMENTS (labels));
    cal_spinner_set_selected (CAL_SPINNER (editor->availability),
                              source && source->availability == CAL_AVAILABILITY_FREE ? 1 : 0);
    gtk_accessible_update_property (GTK_ACCESSIBLE (editor->availability), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("Show me as"), -1);
    line = row ("cal-business-symbolic", editor->availability);
    gtk_widget_add_css_class (line, "cal-edit-spinner-row");
    gtk_box_append (GTK_BOX (content), line);
    gtk_box_append (GTK_BOX (content), separator ());
  }
  /* Privacy (CLASS): Default (none), Confidential, Private, Public */
  {
    const char *labels[] = { _("Default"), _("Confidential"), _("Private"), _("Public") };

    editor->access = cal_spinner_new ();
    cal_spinner_set_items (CAL_SPINNER (editor->access), labels, G_N_ELEMENTS (labels));
    cal_spinner_set_selected (CAL_SPINNER (editor->access), source ? (int) source->access : 0);
    gtk_accessible_update_property (GTK_ACCESSIBLE (editor->access), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Privacy"),
                                    -1);
    line = row ("cal-lock-symbolic", editor->access);
    gtk_widget_add_css_class (line, "cal-edit-spinner-row");
    gtk_widget_add_css_class (line, "cal-edit-last");
    gtk_box_append (GTK_BOX (content), line);
  }

  editor->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (editor->scroll), content);
  gtk_widget_set_vexpand (editor->scroll, TRUE);
  gtk_box_append (GTK_BOX (page), editor->scroll);
}

/* the cursor at the title's end, nothing selected (an EditText's focus selects nothing; GtkText's selects all) */
static void
focus_title (GtkWidget *title)
{
  gtk_widget_grab_focus (title);
  gtk_editable_set_position (GTK_EDITABLE (title), -1);
}

static GtkWidget *
editor_page (GtkWidget *window, CalEvent *original, CalEvent *source, gint64 start, gboolean all_day,
             CalScope scope)
{
  Editor *editor = g_new0 (Editor, 1);
  GtkWidget *page = cal_page_new (window);
  GSettings *settings = cal_window_get_settings (window);

  editor->page = page;
  editor->window = window;
  editor->store = g_object_ref (cal_window_get_store (window));
  editor->settings = g_object_ref (settings);
  editor->original = original ? cal_event_ref (original) : NULL;
  editor->source = source ? cal_event_ref (source) : NULL;
  editor->scope = scope;
  g_object_set_data_full (G_OBJECT (page), "cal-editor", editor, editor_free);
  cal_page_set_back_handler (CAL_PAGE (page), on_back, editor);
  gtk_widget_add_css_class (page, "cal-editor");

  /* the zone: the event's, else the app's (the home time zone when it is used: Utils.getTimeZone) */
  if (source && source->tzid && !source->all_day)
    editor->tzid = g_strdup (source->tzid);
  else if (g_settings_get_boolean (settings, "home-tz-enabled"))
    {
      g_autofree char *home = g_settings_get_string (settings, "home-tz");

      editor->tzid = g_strdup (*home ? home : cal_phone_zone ());
    }
  else
    editor->tzid = g_strdup (cal_phone_zone ());
  if (source)
    {
      editor->color = source->color;
      editor->own_color = source->own_color;
    }
  if (source)
    {
      const CalCalendar *calendar = cal_store_find_calendar (editor->store, source->calendar);

      editor->all_day = source->all_day;
      split_time (editor, source->start, &editor->start_day, &editor->start_minute);
      split_time (editor, source->end, &editor->end_day, &editor->end_minute);
      if (source->all_day)
        editor->end_day = MAX (editor->start_day, cal_event_last_day (source));
      editor->rrule = g_strdup (source->rrule);
      /* a copy goes to a calendar it can be written to */
      editor->calendar = g_strdup (calendar && (!calendar->readonly || original) ? calendar->uid : NULL);
    }
  else
    {
      /* resetToDefaultDuration: the default length from the start */
      gint64 end = start + (all_day ? 24 * 3600 : g_settings_get_int (settings, "default-event-duration") * 60);

      editor->all_day = all_day;
      split_time (editor, start, &editor->start_day, &editor->start_minute);
      split_time (editor, end, &editor->end_day, &editor->end_minute);
      if (all_day)
        editor->end_day = editor->start_day;
    }
  if (!editor->calendar)
    {
      g_autofree char *last = g_settings_get_string (settings, "last-calendar");
      const CalCalendar *calendar = cal_store_default_calendar (editor->store, last);

      editor->calendar = g_strdup (calendar ? calendar->uid : NULL);
    }

  build (editor);
  if (source)
    cal_reminders_set (CAL_REMINDERS (editor->reminders), source->reminders);
  else
    {
      /* a new event: one reminder at the default time ("None" is −1) */
      int minutes = g_settings_get_int (settings, "default-reminder");

      if (minutes >= 0)
        {
          g_autoptr(GArray) reminders = g_array_new (FALSE, FALSE, sizeof (CalReminder));
          CalReminder reminder = { minutes, CAL_METHOD_NOTIFICATION };

          g_array_append_val (reminders, reminder);
          cal_reminders_set (CAL_REMINDERS (editor->reminders), reminders);
        }
    }
  on_reminders_changed (CAL_REMINDERS (editor->reminders), editor);
  show_calendar (editor);
  show_times (editor);
  show_repeat (editor);
  /* Etar focuses the title of a new event only (an existing one opens with nothing focused: cal_window_push) */
  if (!original)
    g_signal_connect_object (page, "map", G_CALLBACK (focus_title), editor->title, G_CONNECT_SWAPPED);
  return page;
}

/* --- entry points */

typedef struct {
  GtkWidget *window, *placeholder;
  CalEvent *event;
} Which;

static void
on_which (int index, gpointer data)
{
  Which *which = data;
  /* EditEventFragment: modify_event, modify_all_following, modify_all */
  CalScope scopes[] = { CAL_SCOPE_THIS, CAL_SCOPE_THIS_AND_FUTURE, CAL_SCOPE_ALL };
  GtkWidget *window = which->window;

  /* the editor stood behind the question: Cancel closes it, a choice fills it in */
  cal_window_pop (window);
  if (index >= 0)
    cal_window_push (window, editor_page (window, which->event, which->event, which->event->start,
                                          which->event->all_day, scopes[index]));
  cal_event_unref (which->event);
  g_object_unref (which->placeholder);
  g_free (which);
}

static GtkWidget *
ask_which (GtkWidget *window, CalEvent *event)
{
  static const char *labels[] = { N_("Change only this event"), N_("Change this and all future events"),
                                  N_("Change all events in the series") };
  const char *translated[] = { _(labels[0]), _(labels[1]), _(labels[2]) };
  Which *which = g_new0 (Which, 1);
  GtkWidget *placeholder = cal_page_new (window);

  /* displayEditWhichDialog: asked before the editor shows anything */
  which->window = window;
  which->placeholder = g_object_ref (placeholder);
  which->event = cal_event_ref (event);
  cal_choice_dialog (window, _("Details"), translated, 3, on_which, which);
  return placeholder;
}

GtkWidget *
event_editor_new (GtkWidget *window, CalEvent *event, gint64 start, gboolean all_day, gboolean duplicate)
{
  if (!event)
    return editor_page (window, NULL, NULL, start, all_day, CAL_SCOPE_ALL);
  if (duplicate)
    return editor_page (window, NULL, event, event->start, event->all_day, CAL_SCOPE_ALL);
  /* a Minerva task is one task, whatever due time was opened: no "this or all" */
  if (event->recurring && !event->minerva)
    return ask_which (window, event);
  return editor_page (window, event, event, event->start, event->all_day, CAL_SCOPE_ALL);
}

void
event_editor_import (GtkWidget *window, GFile *file)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;
  g_autoptr(ICalComponent) root = NULL;
  ICalComponent *vevent = NULL;
  CalCalendar dummy = { NULL, NULL, NULL, 0x3f51b5, FALSE, TRUE, TRUE };

  if (!g_file_load_contents (file, NULL, &contents, NULL, NULL, &error))
    {
      cal_window_toast (window, error->message);
      return;
    }
  root = i_cal_parser_parse_string (contents);
  if (root && i_cal_component_isa (root) == I_CAL_VEVENT_COMPONENT)
    vevent = g_object_ref (root);
  else if (root)
    vevent = i_cal_component_get_first_component (root, I_CAL_VEVENT_COMPONENT);
  if (!vevent)
    {
      /* ImportActivity's "no events" */
      cal_window_toast (window, _("Invalid iCalendar file"));
      return;
    }
  {
    g_autoptr(ECalComponent) comp = e_cal_component_new_from_icalcomponent (vevent);
    ECalComponentDateTime *dtstart = comp ? e_cal_component_get_dtstart (comp) : NULL;
    ECalComponentDateTime *dtend = comp ? e_cal_component_get_dtend (comp) : NULL;
    g_autoptr(CalEvent) event = NULL;

    if (!dtstart)
      {
        g_clear_pointer (&dtend, e_cal_component_datetime_free);
        cal_window_toast (window, _("Invalid iCalendar file"));
        return;
      }
    event = cal_event_from_component (comp, &dummy, e_cal_component_datetime_get_value (dtstart),
                                      dtend ? e_cal_component_datetime_get_value (dtend)
                                            : e_cal_component_datetime_get_value (dtstart));
    e_cal_component_datetime_free (dtstart);
    g_clear_pointer (&dtend, e_cal_component_datetime_free);
    /* a new event in the default calendar, with the file's contents */
    g_clear_pointer (&event->calendar, g_free);
    cal_window_push (window, editor_page (window, NULL, event, event->start, event->all_day, CAL_SCOPE_ALL));
  }
}
