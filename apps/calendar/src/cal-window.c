/* The Calendar's window (cal-window.h): AllInOneActivity, its toolbar, views, FAB and drawer (spec/calendar.md §2). */
#include "cal-window.h"
#include "cal-minerva.h"
#include "ns-mc.h"
#include "agenda-view.h"
#include "cal-colors.h"
#include "cal-date-picker.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-popup.h"
#include "cal-search.h"
#include "cal-settings.h"
#include "day-view.h"
#include "event-editor.h"
#include "event-info.h"
#include "month-view.h"

#include <glib/gi18n.h>
#include <neo.h>

typedef enum { VIEW_DAY, VIEW_WEEK, VIEW_MONTH, VIEW_AGENDA, N_VIEWS } ViewType;

static const char *view_names[N_VIEWS] = { "day", "week", "month", "agenda" };

#define DRAWER_WIDTH 296.0              /* measured 888 px */
#define DRAWER_MS 250                   /* DrawerLayout's settle, at most */
#define VIEW_FADE_MS 150                /* FragmentTransaction.TRANSIT_FRAGMENT_FADE */
#define PAGE_MS 300                     /* an activity opening over the main one */

typedef struct {
  GtkWidget *window;
  CalStore *store;
  GSettings *settings;

  GtkWidget *pages;                     /* GtkStack: the main screen, then the pushed ones */
  GPtrArray *pushed;
  guint page_serial;

  /* the main screen */
  GtkWidget *main;
  GtkWidget *title, *subtitle, *today_icon;
  GtkWidget *views;
  GtkWidget *view_widgets[N_VIEWS];
  GtkWidget *fab;
  GtkWidget *drawer, *drawer_scrim, *drawer_sheet, *drawer_slide, *drawer_items[N_VIEWS];
  NeoAnim *drawer_anim;
  double drawer_open;                   /* 0 closed … 1 open */
  gboolean drawer_dragging;             /* a drag on the sheet past the touch slop */
  double drawer_drag_from;
  GtkWidget *menu;                      /* the overflow menu while it shows */
  GtkWidget *toast, *toast_label;
  NeoAnim *toast_anim;
  guint toast_timeout;

  ViewType view, previous_view;
  gboolean back_to_previous;            /* Back returns to the view before (Month → a day) */
  gboolean landscape;                   /* the window wider than tall: the Month view's landscape layout */
  int selected_day;
  GCancellable *query;
  gint64 range_start;           /* the shown range's start, for the Minerva calendar's history line */
  gint64 history_said;          /* the run log's start the owner was told about, once */
  guint midnight;
} Shell;

static Shell *
shell_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (gtk_widget_get_root (window)), "cal-shell");
}

CalStore *
cal_window_get_store (GtkWidget *window)
{
  return shell_of (window)->store;
}

GSettings *
cal_window_get_settings (GtkWidget *window)
{
  return shell_of (window)->settings;
}

static int
today (void)
{
  return cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
}

static int
week_start (Shell *shell)
{
  return cal_week_start (g_settings_get_int (shell->settings, "week-start"));
}

/* the first day of the week holding @day */
static int
week_of (Shell *shell, int day)
{
  return day - ((cal_day_of_week (day) - week_start (shell) + 7) % 7);
}

/* --- the toolbar's title (CalendarToolbarHandler) */

static void
update_title (Shell *shell)
{
  g_autofree char *title = NULL, *subtitle = NULL;
  int day = shell->selected_day;

  switch (shell->view)
    {
    case VIEW_DAY:
    case VIEW_AGENDA:
      /* the date, and "Today, Saturday" under it */
      title = cal_format_full_date (day);
      subtitle = cal_format_relative_weekday (day, today ());
      break;
    case VIEW_WEEK:
      title = cal_format_month_year (cal_day_view_get_first_day (CAL_DAY_VIEW (shell->view_widgets[VIEW_WEEK])));
      /* "Show week number": "Week 46" under it (buildWeekNum, from the selected time) */
      if (g_settings_get_boolean (shell->settings, "show-week-number"))
        subtitle = cal_format_week_number (cal_week_number (cal_day_view_get_first_day (
                                                              CAL_DAY_VIEW (shell->view_widgets[VIEW_WEEK])),
                                                            week_start (shell)));
      break;
    case VIEW_MONTH:
    default:
      title = cal_format_month_year (cal_month_view_get_focus_day (CAL_MONTH_VIEW (shell->view_widgets[VIEW_MONTH])));
      break;
    }
  gtk_label_set_label (GTK_LABEL (shell->title), title);
  gtk_label_set_label (GTK_LABEL (shell->subtitle), subtitle ? subtitle : "");
  gtk_widget_set_visible (shell->subtitle, subtitle != NULL);
  gtk_widget_queue_draw (shell->today_icon);
}

/* --- the events of the view */

static void
on_events (GPtrArray *events, gpointer data)
{
  Shell *shell = data;
  GtkWidget *view;

  if (!events)
    return;             /* cancelled: a newer query is on its way */
  /* Hide declined events: the provider's query leaves out selfAttendeeStatus = DECLINED */
  if (g_settings_get_boolean (shell->settings, "hide-declined"))
    for (guint i = events->len; i-- > 0;)
      if (((CalEvent *) events->pdata[i])->self_status == CAL_STATUS_DECLINED)
        g_ptr_array_remove_index (events, i);
  /* the scheduler's run log does not reach so far back: say it once, plainly */
  {
    gint64 history = cal_store_minerva_history (shell->store);
    JsonObject *ui = cal_store_minerva_ui (shell->store);
    const CalCalendar *minerva = cal_store_find_calendar (shell->store, CAL_MINERVA_CALENDAR);

    if (ui && minerva && minerva->visible && history > 0 && shell->range_start < history
        && history != shell->history_said)
      {
        g_autofree char *date = cal_format_full_date (cal_day_number (history));
        g_autofree char *text = cal_minerva_fill (ns_json_str (ui, "history_gone"), "date", date);

        shell->history_said = history;
        cal_window_toast (shell->window, text);
      }
  }
  /* where the run history begins, in the views that can show it: a row in the agenda, a line in the day and week */
  {
    gint64 history = cal_store_minerva_history (shell->store);
    JsonObject *ui = cal_store_minerva_ui (shell->store);
    const CalCalendar *minerva = cal_store_find_calendar (shell->store, CAL_MINERVA_CALENDAR);
    gboolean shown = ui && minerva && minerva->visible && history > 0;
    g_autofree char *date = shown ? cal_format_full_date (cal_day_number (history)) : NULL;
    g_autofree char *text = shown ? cal_minerva_fill (ns_json_str (ui, "history_row"), "date", date) : NULL;

    cal_agenda_view_set_history (CAL_AGENDA_VIEW (shell->view_widgets[VIEW_AGENDA]),
                                 shown ? cal_day_number (history) : 0, text);
    cal_day_view_set_history (CAL_DAY_VIEW (shell->view_widgets[VIEW_DAY]), shown ? history : 0, text);
    cal_day_view_set_history (CAL_DAY_VIEW (shell->view_widgets[VIEW_WEEK]), shown ? history : 0, text);
  }
  view = shell->view_widgets[shell->view];
  switch (shell->view)
    {
    case VIEW_DAY:
    case VIEW_WEEK:
      cal_day_view_set_events (CAL_DAY_VIEW (view), events);
      break;
    case VIEW_MONTH:
      cal_month_view_set_events (CAL_MONTH_VIEW (view), events);
      break;
    case VIEW_AGENDA:
    default:
      cal_agenda_view_set_events (CAL_AGENDA_VIEW (view), events);
      break;
    }
}

/* the shown period's events, and the periods beside it (a swipe draws them) */
static void
refresh (Shell *shell)
{
  int first, last;

  switch (shell->view)
    {
    case VIEW_DAY:
    case VIEW_WEEK:
      {
        CalDayView *view = CAL_DAY_VIEW (shell->view_widgets[shell->view]);
        int n = cal_day_view_get_num_days (view);

        first = cal_day_view_get_first_day (view) - n;
        last = cal_day_view_get_first_day (view) + 2 * n - 1;
      }
      break;
    case VIEW_MONTH:
      cal_month_view_get_range (CAL_MONTH_VIEW (shell->view_widgets[VIEW_MONTH]), &first, &last);
      break;
    case VIEW_AGENDA:
    default:
      cal_agenda_view_get_range (CAL_AGENDA_VIEW (shell->view_widgets[VIEW_AGENDA]), &first, &last);
      break;
    }
  g_cancellable_cancel (shell->query);
  g_clear_object (&shell->query);
  shell->query = g_cancellable_new ();
  shell->range_start = cal_day_start (first);
  cal_store_query (shell->store, cal_day_start (first), cal_day_start (last + 1), shell->query, on_events, shell);
}

static void
on_store_changed (CalStore *store, Shell *shell)
{
  refresh (shell);
}

/* --- views */

static void
select_drawer_item (Shell *shell)
{
  for (int i = 0; i < N_VIEWS; i++)
    if (i == (int) shell->view)
      gtk_widget_add_css_class (shell->drawer_items[i], "selected");
    else
      gtk_widget_remove_css_class (shell->drawer_items[i], "selected");
}

static void
show_view (Shell *shell, ViewType view, int day)
{
  GtkWidget *widget = shell->view_widgets[view];

  shell->view = view;
  shell->selected_day = day;
  switch (view)
    {
    case VIEW_DAY:
      cal_day_view_set_first_day (CAL_DAY_VIEW (widget), day);
      break;
    case VIEW_WEEK:
      /* DayView.setSelected: a seven-day view starts on the week's first day, a shorter one on the day itself */
      cal_day_view_set_first_day (CAL_DAY_VIEW (widget), cal_day_view_get_num_days (CAL_DAY_VIEW (widget)) == 7
                                                         ? week_of (shell, day) : day);
      break;
    case VIEW_MONTH:
      cal_month_view_go_to (CAL_MONTH_VIEW (widget), day);
      break;
    case VIEW_AGENDA:
    default:
      cal_agenda_view_go_to (CAL_AGENDA_VIEW (widget), day);
      break;
    }
  gtk_stack_set_visible_child (GTK_STACK (shell->views), widget);
  select_drawer_item (shell);
  update_title (shell);
  refresh (shell);
  /* "Previously used view" (Etar's default start view) */
  g_settings_set_string (shell->settings, "view", view_names[view]);
}

static void
switch_view (Shell *shell, ViewType view)
{
  shell->back_to_previous = FALSE;
  show_view (shell, view, shell->selected_day);
}

void
cal_window_go_to (GtkWidget *window, int day)
{
  Shell *shell = shell_of (window);

  show_view (shell, shell->view, day);
  if (shell->view == VIEW_DAY || shell->view == VIEW_WEEK)
    {
      g_autoptr(GDateTime) now = g_date_time_new_now_local ();

      if (day == today ())
        cal_day_view_scroll_to_hour (CAL_DAY_VIEW (shell->view_widgets[shell->view]), g_date_time_get_hour (now));
    }
}

static void
on_page (CalDayView *view, int delta, Shell *shell)
{
  /* the swipe moved the view already; the selected day moves with it */
  shell->selected_day += delta * cal_day_view_get_num_days (view);
  update_title (shell);
  refresh (shell);
}

static void
on_day_activated (GtkWidget *view, int day, Shell *shell)
{
  /* a date in the week's header, or a day in Month: that day's Day view; Back returns (mBackToPreviousView) */
  shell->previous_view = shell->view;
  shell->back_to_previous = shell->view == VIEW_MONTH;
  show_view (shell, VIEW_DAY, day);
}

static void
on_month_day (CalMonthView *view, int day, gboolean has_events, Shell *shell)
{
  /* MonthWeekEventsView.onTouchEvent: a day with events opens its Day view, an empty one a new event */
  if (has_events)
    on_day_activated (GTK_WIDGET (view), day, shell);
  else
    {
      g_autoptr(GDateTime) now = g_date_time_new_now_local ();

      cal_window_new_event (shell->window, cal_day_start (day) + g_date_time_get_hour (now) * 3600, FALSE);
    }
}

static void
on_month_moved (CalMonthView *view, Shell *shell)
{
  update_title (shell);
  refresh (shell);
}

static void
on_agenda_range (CalAgendaView *view, Shell *shell)
{
  refresh (shell);
}

static void
on_agenda_top_day (CalAgendaView *view, int day, Shell *shell)
{
  shell->selected_day = day;
  update_title (shell);
}

static void
on_event_activated (GtkWidget *view, CalEvent *event, Shell *shell)
{
  cal_window_show_event (shell->window, event);
}

static void
on_create_event (GtkWidget *view, gint64 start, gboolean all_day, Shell *shell)
{
  cal_window_new_event (shell->window, start, all_day);
}

static void
on_cell_height (CalDayView *view, double height, Shell *shell)
{
  g_settings_set_double (shell->settings, "cell-height", height);
  /* the other one follows (DayView keeps one preferred cell height) */
  for (int i = VIEW_DAY; i <= VIEW_WEEK; i++)
    if (shell->view_widgets[i] != GTK_WIDGET (view))
      cal_day_view_set_cell_height (CAL_DAY_VIEW (shell->view_widgets[i]), height);
}

/* --- the FAB: a new event at the selected hour, else now rounded up to the next :00 or :30 */

static void
on_fab (NeoPressable *fab, Shell *shell)
{
  gint64 start;
  gboolean all_day = FALSE;
  GtkWidget *view = shell->view_widgets[shell->view];

  if ((shell->view == VIEW_DAY || shell->view == VIEW_WEEK)
      && cal_day_view_get_selection (CAL_DAY_VIEW (view), &start, &all_day))
    cal_day_view_clear_selection (CAL_DAY_VIEW (view));
  else
    {
      g_autoptr(GDateTime) now = g_date_time_new_now_local ();
      int minute = g_date_time_get_minute (now);
      int day = shell->selected_day;

      /* AllInOneActivity's FAB: the shown day, the time now rounded up to the half hour */
      start = cal_day_start (day) + g_date_time_get_hour (now) * 3600 + (minute <= 30 ? 30 : 60) * 60;
      if (minute == 0)
        start -= 30 * 60;
    }
  cal_window_new_event (shell->window, start, all_day);
}

/* --- the drawer */

/* CalSlide: its child moved along x by a transform (a negative margin, the other way to slide it, is one GTK warns
 * about on every frame) */
#define CAL_TYPE_SLIDE (cal_slide_get_type ())
G_DECLARE_FINAL_TYPE (CalSlide, cal_slide, CAL, SLIDE, GtkWidget)

struct _CalSlide {
  GtkWidget parent;
  double offset;
};

G_DEFINE_FINAL_TYPE (CalSlide, cal_slide, GTK_TYPE_WIDGET)

static void
cal_slide_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                   int *minimum_baseline, int *natural_baseline)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);

  *minimum = *natural = 0;
  if (child)
    gtk_widget_measure (child, orientation, for_size, minimum, natural, NULL, NULL);
}

static void
cal_slide_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);

  if (child)
    gtk_widget_allocate (child, width, height, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) CAL_SLIDE (widget)->offset, 0)));
}

static void
cal_slide_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (cal_slide_parent_class)->dispose (object);
}

static void
cal_slide_class_init (CalSlideClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cal_slide_dispose;
  GTK_WIDGET_CLASS (klass)->measure = cal_slide_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = cal_slide_size_allocate;
}

static void
cal_slide_init (CalSlide *self)
{
}

static void
place_drawer (Shell *shell)
{
  gtk_widget_set_visible (shell->drawer, shell->drawer_open > 0);
  gtk_widget_set_opacity (shell->drawer_scrim, shell->drawer_open);
  /* slid in from the left */
  CAL_SLIDE (shell->drawer_slide)->offset = round (-DRAWER_WIDTH * (1 - shell->drawer_open));
  gtk_widget_queue_allocate (shell->drawer_slide);
}

static void
on_drawer_anim (NeoAnim *anim, double value, gpointer data)
{
  Shell *shell = data;

  shell->drawer_open = value;
  place_drawer (shell);
}

static void
open_drawer (Shell *shell, gboolean open)
{
  neo_anim_start (shell->drawer_anim, shell->drawer_open, open ? 1 : 0,
                  (guint) (DRAWER_MS * fabs ((open ? 1 : 0) - shell->drawer_open)), 0, SPONGY_LINEAR_OUT_SLOW_IN);
}

static void
on_drawer_item (NeoPressable *item, Shell *shell)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "view"));

  open_drawer (shell, FALSE);
  switch_view (shell, index);
}

static void
on_settings_item (NeoPressable *item, Shell *shell)
{
  open_drawer (shell, FALSE);
  cal_window_push (shell->window, cal_settings_page_new (shell->window));
}

static void
on_about_item (NeoPressable *item, Shell *shell)
{
  open_drawer (shell, FALSE);
  cal_window_push (shell->window, cal_about_page_new (shell->window));
}

static void
on_scrim_pressed (GtkGestureClick *gesture, int n, double x, double y, Shell *shell)
{
  open_drawer (shell, FALSE);
}

/* A drag on the sheet moves it once it is past the touch slop (8 dp); a tap is its item's, and must not set the
 * sheet back where it was: an item's tap closes the drawer, and letting go would open it again. */
static void
on_sheet_drag_update (GtkGestureDrag *gesture, double dx, double dy, Shell *shell)
{
  if (!shell->drawer_dragging)
    {
      if (fabs (dx) < 8)
        return;
      shell->drawer_dragging = TRUE;
      neo_anim_stop (shell->drawer_anim);
      shell->drawer_drag_from = shell->drawer_open;
    }
  shell->drawer_open = CLAMP (shell->drawer_drag_from + dx / DRAWER_WIDTH, 0, 1);
  place_drawer (shell);
}

static void
on_sheet_drag_begin (GtkGestureDrag *gesture, double x, double y, Shell *shell)
{
  shell->drawer_dragging = FALSE;
}

static void
on_sheet_drag_end (GtkGestureDrag *gesture, double dx, double dy, Shell *shell)
{
  if (shell->drawer_dragging)
    open_drawer (shell, shell->drawer_open > 0.5);
  shell->drawer_dragging = FALSE;
}

static GtkWidget *
drawer_item (Shell *shell, const char *icon, const char *label, GCallback callback, int view)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *image = gtk_image_new_from_icon_name (icon);
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *item = neo_pressable_new (row);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  gtk_widget_add_css_class (row, "cal-drawer-row");
  gtk_widget_add_css_class (image, "cal-drawer-icon");
  gtk_widget_add_css_class (text, "cal-drawer-label");
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  neo_label_set_font_padding (text, 14);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (row), image);
  gtk_box_append (GTK_BOX (row), text);
  gtk_widget_add_css_class (item, "cal-drawer-item");
  /* the item's pill: a full radius, 12 dp in from the sheet's edges (36 px measured) */
  neo_pressable_set_ripple (NEO_PRESSABLE (item), NEO_RIPPLE_BOUNDED, 24);
  g_object_set_data (G_OBJECT (item), "view", GINT_TO_POINTER (view));
  g_signal_connect (item, "clicked", callback, shell);
  return item;
}

static GtkWidget *
make_drawer (Shell *shell)
{
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sheet = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkGesture *press = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();
  static const struct { const char *icon, *label; } items[N_VIEWS] = {
    { "cal-view-day-symbolic", N_("Day") },
    { "cal-view-week-symbolic", N_("Week") },
    { "cal-view-month-symbolic", N_("Month") },
    { "cal-view-agenda-symbolic", N_("Agenda") },
  };
  GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (scrim, "neo-scrim");
  gtk_widget_set_hexpand (scrim, TRUE);
  gtk_widget_set_vexpand (scrim, TRUE);
  g_signal_connect (press, "pressed", G_CALLBACK (on_scrim_pressed), shell);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (overlay), scrim);

  gtk_widget_add_css_class (sheet, "cal-drawer");
  gtk_widget_set_size_request (sheet, (int) DRAWER_WIDTH, -1);
  gtk_widget_set_halign (sheet, GTK_ALIGN_START);
  gtk_widget_set_valign (sheet, GTK_ALIGN_FILL);
  /* to assistive technology (and the shell's back gesture) the open drawer is a dialog: Back closes it */
  gtk_accessible_update_property (GTK_ACCESSIBLE (sheet), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Calendar"), -1);
  for (int i = 0; i < N_VIEWS; i++)
    {
      shell->drawer_items[i] = drawer_item (shell, items[i].icon, _(items[i].label), G_CALLBACK (on_drawer_item), i);
      gtk_box_append (GTK_BOX (sheet), shell->drawer_items[i]);
    }
  gtk_widget_add_css_class (divider, "cal-drawer-divider");
  gtk_box_append (GTK_BOX (sheet), divider);
  gtk_box_append (GTK_BOX (sheet), drawer_item (shell, "cal-settings-symbolic", _("Settings"),
                                                G_CALLBACK (on_settings_item), -1));
  gtk_box_append (GTK_BOX (sheet), drawer_item (shell, "cal-info-symbolic", _("About Calendar"),
                                                G_CALLBACK (on_about_item), -1));
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_sheet_drag_begin), shell);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_sheet_drag_update), shell);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_sheet_drag_end), shell);
  gtk_widget_add_controller (sheet, GTK_EVENT_CONTROLLER (drag));
  shell->drawer_slide = g_object_new (CAL_TYPE_SLIDE, NULL);
  gtk_widget_set_parent (sheet, shell->drawer_slide);
  gtk_widget_set_halign (shell->drawer_slide, GTK_ALIGN_START);
  gtk_widget_set_valign (shell->drawer_slide, GTK_ALIGN_FILL);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), shell->drawer_slide);

  shell->drawer_scrim = scrim;
  shell->drawer_sheet = sheet;
  gtk_widget_set_visible (overlay, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (overlay), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Calendar"), -1);
  return overlay;
}

/* an edge swipe from the left opens it (DrawerLayout's 20 dp edge) */
static void
on_edge_drag_begin (GtkGestureDrag *gesture, double x, double y, Shell *shell)
{
  if (x > 20 || shell->pushed->len > 0)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  shell->drawer_drag_from = 0;
}

static void
on_edge_drag_update (GtkGestureDrag *gesture, double dx, double dy, Shell *shell)
{
  if (dx < 8 && shell->drawer_open == 0)
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  shell->drawer_open = CLAMP (dx / DRAWER_WIDTH, 0, 1);
  place_drawer (shell);
}

static void
on_edge_drag_end (GtkGestureDrag *gesture, double dx, double dy, Shell *shell)
{
  if (shell->drawer_open > 0)
    open_drawer (shell, shell->drawer_open > 0.3);
}

/* --- the overflow menu (all_in_one_title_bar.xml) */

static void close_menu (Shell *shell);

static void
on_date_chosen (int day, gpointer data)
{
  Shell *shell = data;

  cal_window_go_to (shell->window, day);
}

static void
go_to (Shell *shell)
{
  cal_date_picker_show (shell->window, _("Go to…"), shell->selected_day, week_start (shell), on_date_chosen, shell);
}

static void
on_menu_go_to (NeoPressable *item, Shell *shell)
{
  close_menu (shell);
  go_to (shell);
}

static void
on_refreshed (GObject *client, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!e_client_refresh_finish (E_CLIENT (client), result, &error)
      && !g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED))
    g_message ("refresh: %s", error->message);
}

static void
on_menu_refresh (NeoPressable *item, Shell *shell)
{
  GPtrArray *calendars = cal_store_get_calendars (shell->store);

  close_menu (shell);
  /* Etar's Refresh asks every account to sync (ContentResolver.requestSync); EDS's is e_client_refresh, which a
   * local calendar does not support */
  for (guint i = 0; i < calendars->len; i++)
    {
      ECalClient *client = cal_store_get_client (shell->store, ((CalCalendar *) calendars->pdata[i])->uid);

      if (client && e_client_check_refresh_supported (E_CLIENT (client)))
        e_client_refresh (E_CLIENT (client), NULL, on_refreshed, NULL);
    }
  refresh (shell);
}

static void
on_menu_view_settings (NeoPressable *item, Shell *shell)
{
  close_menu (shell);
  cal_window_push (shell->window, cal_display_options_page_new (shell->window));
}

static void
on_menu_search (NeoPressable *item, Shell *shell)
{
  close_menu (shell);
  cal_search_open (shell->window);
}

static void
on_import_chosen (GObject *dialog, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog), result, NULL);

  if (file)
    cal_window_import (window, file);
  g_object_unref (window);
}

/* Import event: Etar asks for a text/calendar document (ACTION_GET_CONTENT) and opens ImportActivity with it; here
 * the file chooser (the portal's when there is one) and then the same import as opening an .ics with the app */
static void
on_menu_import (NeoPressable *item, Shell *shell)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  close_menu (shell);
  gtk_file_filter_set_name (filter, _("Calendar files"));
  gtk_file_filter_add_mime_type (filter, "text/calendar");
  gtk_file_filter_add_suffix (filter, "ics");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, filter);
  gtk_file_dialog_open (dialog, GTK_WINDOW (shell->window), NULL, on_import_chosen, g_object_ref (shell->window));
}

static void
on_menu_scrim (GtkGestureClick *gesture, int n, double x, double y, Shell *shell)
{
  close_menu (shell);
}

static void
close_menu (Shell *shell)
{
  if (!shell->menu)
    return;
  gtk_overlay_remove_overlay (neo_window_get_overlay (shell->window), shell->menu);
  shell->menu = NULL;
}

static GtkWidget *
menu_item (Shell *shell, const char *label, GCallback callback)
{
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *item = neo_pressable_new (text);

  gtk_label_set_xalign (GTK_LABEL (text), 0);
  /* TextView's font padding: the title sits where ListMenuItemView puts it (measured: 2 px lower than without) */
  neo_label_set_font_padding (text, 16);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (item, "cal-menu-item");
  neo_pressable_set_ripple (NEO_PRESSABLE (item), NEO_RIPPLE_BOUNDED, 0);
  g_signal_connect (item, "clicked", callback, shell);
  return item;
}

static void
on_more (NeoPressable *button, Shell *shell)
{
  GtkWidget *scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *menu = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *layer = gtk_overlay_new ();
  GtkGesture *press = gtk_gesture_click_new ();

  close_menu (shell);
  g_signal_connect (press, "pressed", G_CALLBACK (on_menu_scrim), shell);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (layer), scrim);
  gtk_widget_add_css_class (menu, "cal-menu");
  gtk_widget_add_css_class (menu, "cal-overflow-menu");
  gtk_widget_set_halign (menu, GTK_ALIGN_END);
  gtk_widget_set_valign (menu, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (menu), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  gtk_box_append (GTK_BOX (menu), menu_item (shell, _("Go to…"), G_CALLBACK (on_menu_go_to)));
  gtk_box_append (GTK_BOX (menu), menu_item (shell, _("Refresh"), G_CALLBACK (on_menu_refresh)));
  gtk_box_append (GTK_BOX (menu), menu_item (shell, _("Search"), G_CALLBACK (on_menu_search)));
  gtk_box_append (GTK_BOX (menu), menu_item (shell, _("Import event"), G_CALLBACK (on_menu_import)));
  /* updateViewSettingsVisiblility: the Month view only */
  if (shell->view == VIEW_MONTH)
    gtk_box_append (GTK_BOX (menu), menu_item (shell, _("View settings"), G_CALLBACK (on_menu_view_settings)));
  gtk_overlay_add_overlay (GTK_OVERLAY (layer), menu);
  shell->menu = layer;
  gtk_overlay_add_overlay (neo_window_get_overlay (shell->window), layer);
}

/* --- the Today button: a calendar with today's date in it (DayOfMonthDrawable) */

typedef struct {
  GtkWidget parent;
} CalTodayIcon;

typedef struct {
  GtkWidgetClass parent_class;
} CalTodayIconClass;

G_DEFINE_TYPE (CalTodayIcon, cal_today_icon, GTK_TYPE_WIDGET)

static void
cal_today_icon_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GdkRGBA color = cal_argb (CAL_ON_SURFACE_VARIANT);
  double size = 28;     /* ic_baseline_calendar_dayofmonth */
  double x = (gtk_widget_get_width (widget) - size) / 2, y = (gtk_widget_get_height (widget) - size) / 2 - 2;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *number = g_strdup_printf ("%d", g_date_time_get_day_of_month (now));
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, number);
  PangoFontDescription *font = pango_font_description_from_string ("Neo Roboto, Roboto");
  int w, h;

  /* today_icon.xml: the outline, 4 dp off the bottom; the date bold 12 sp in it, at (bottom + height + 1) / 1.75 */
  neo_snapshot_icon (snapshot, widget, "cal-today-symbolic", x, y, size, &color);
  pango_font_description_set_absolute_size (font, 12 * PANGO_SCALE);
  pango_font_description_set_weight (font, PANGO_WEIGHT_BOLD);
  pango_layout_set_font_description (layout, font);
  pango_layout_get_pixel_size (layout, &w, &h);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x + (size - w) / 2.0,
                                                          y + (size + 12 * 0.711 + 1) / 1.75
                                                            - pango_layout_get_baseline (layout) / (double) PANGO_SCALE
                                                            + 2));
  gtk_snapshot_append_layout (snapshot, layout, &color);
  gtk_snapshot_restore (snapshot);
  pango_font_description_free (font);
  g_object_unref (layout);
}

static void
cal_today_icon_class_init (CalTodayIconClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = cal_today_icon_snapshot;
}

static void
cal_today_icon_init (CalTodayIcon *self)
{
  gtk_widget_set_size_request (GTK_WIDGET (self), 48, 48);
}

static void
on_today (NeoPressable *button, Shell *shell)
{
  cal_window_go_to (shell->window, today ());
}

static void
on_nav (NeoPressable *button, Shell *shell)
{
  open_drawer (shell, TRUE);
}

static void
on_title_pressed (GtkGestureClick *gesture, int n, double x, double y, Shell *shell)
{
  go_to (shell);
}

static GtkWidget *
make_toolbar (Shell *shell)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *nav = neo_icon_button_new ("cal-menu-symbolic");
  GtkWidget *titles = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *today_button, *more;
  GtkGesture *press = gtk_gesture_click_new ();

  gtk_widget_add_css_class (bar, "cal-toolbar");
  gtk_accessible_update_property (GTK_ACCESSIBLE (nav), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Open navigation drawer"), -1);
  gtk_widget_add_css_class (nav, "cal-toolbar-nav");
  g_signal_connect (nav, "clicked", G_CALLBACK (on_nav), shell);
  gtk_box_append (GTK_BOX (bar), nav);

  shell->title = gtk_label_new (NULL);
  shell->subtitle = gtk_label_new (NULL);
  gtk_widget_add_css_class (shell->title, "cal-toolbar-title");
  gtk_widget_add_css_class (shell->subtitle, "cal-toolbar-subtitle");
  gtk_label_set_xalign (GTK_LABEL (shell->title), 0);
  gtk_label_set_xalign (GTK_LABEL (shell->subtitle), 0);
  gtk_label_set_ellipsize (GTK_LABEL (shell->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize (GTK_LABEL (shell->subtitle), PANGO_ELLIPSIZE_END);
  /* TextView's font padding: the two lines 88 and 65 px tall, as measured (still:cal-agenda) */
  neo_label_set_font_padding (shell->title, 22);
  neo_label_set_font_padding (shell->subtitle, 16);
  gtk_box_append (GTK_BOX (titles), shell->title);
  gtk_box_append (GTK_BOX (titles), shell->subtitle);
  gtk_widget_set_hexpand (titles, TRUE);
  gtk_widget_set_valign (titles, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (titles, "cal-toolbar-titles");
  g_signal_connect (press, "released", G_CALLBACK (on_title_pressed), shell);
  gtk_widget_add_controller (titles, GTK_EVENT_CONTROLLER (press));
  gtk_box_append (GTK_BOX (bar), titles);

  shell->today_icon = g_object_new (cal_today_icon_get_type (), NULL);
  today_button = neo_pressable_new (shell->today_icon);
  neo_pressable_set_ripple (NEO_PRESSABLE (today_button), NEO_RIPPLE_CIRCLE, 20);
  gtk_accessible_update_property (GTK_ACCESSIBLE (today_button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Today"), -1);
  g_signal_connect (today_button, "clicked", G_CALLBACK (on_today), shell);
  gtk_box_append (GTK_BOX (bar), today_button);

  more = neo_icon_button_new ("cal-more-symbolic");
  gtk_widget_add_css_class (more, "cal-toolbar-more");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  g_signal_connect (more, "clicked", G_CALLBACK (on_more), shell);
  gtk_box_append (GTK_BOX (bar), more);
  return bar;
}

/* --- midnight: today moves */

static gboolean on_midnight (gpointer data);

static void
schedule_midnight (Shell *shell)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 next = cal_day_start (cal_day_number (now) + 1);

  shell->midnight = g_timeout_add_seconds ((guint) MAX (1, next - now + 1), on_midnight, shell);
}

static gboolean
on_midnight (gpointer data)
{
  Shell *shell = data;

  shell->midnight = 0;
  update_title (shell);
  gtk_widget_queue_draw (shell->views);
  schedule_midnight (shell);
  return G_SOURCE_REMOVE;
}

/* --- pages */

static void
on_page_transition_done (GObject *stack, GParamSpec *pspec, Shell *shell)
{
  /* a popped page goes once it has faded out */
  if (gtk_stack_get_transition_running (GTK_STACK (stack)))
    return;
  for (GtkWidget *child = gtk_widget_get_first_child (shell->pages), *next; child; child = next)
    {
      next = gtk_widget_get_next_sibling (child);
      if (child != shell->main && g_object_get_data (G_OBJECT (child), "cal-popped"))
        gtk_stack_remove (GTK_STACK (shell->pages), child);
    }
}

void
cal_window_push (GtkWidget *window, GtkWidget *page)
{
  Shell *shell = shell_of (window);
  g_autofree char *name = g_strdup_printf ("page-%u", ++shell->page_serial);

  close_menu (shell);
  /* A page opens with nothing focused, as an Android activity does unless it asks: with the focus still in the
   * stack, GtkStack would hand it to the new page's first entry, which selects its text and calls up the keyboard. */
  gtk_window_set_focus (GTK_WINDOW (window), NULL);
  gtk_stack_add_named (GTK_STACK (shell->pages), page, name);
  g_ptr_array_add (shell->pushed, page);
  gtk_stack_set_visible_child (GTK_STACK (shell->pages), page);
}

void
cal_window_pop (GtkWidget *window)
{
  Shell *shell = shell_of (window);
  GtkWidget *page;

  if (!shell->pushed->len)
    return;
  page = g_ptr_array_steal_index (shell->pushed, shell->pushed->len - 1);
  g_object_set_data (G_OBJECT (page), "cal-popped", GINT_TO_POINTER (TRUE));
  gtk_stack_set_visible_child (GTK_STACK (shell->pages),
                               shell->pushed->len ? shell->pushed->pdata[shell->pushed->len - 1] : shell->main);
  if (!gtk_stack_get_transition_running (GTK_STACK (shell->pages)))
    on_page_transition_done (G_OBJECT (shell->pages), NULL, shell);
}

void
cal_window_show_event (GtkWidget *window, CalEvent *event)
{
  cal_window_push (window, event_info_new (window, event));
}

void
cal_window_new_event (GtkWidget *window, gint64 start, gboolean all_day)
{
  cal_window_push (window, event_editor_new (window, NULL, start, all_day, FALSE));
}

void
cal_window_edit_event (GtkWidget *window, CalEvent *event, gboolean duplicate)
{
  cal_window_push (window, event_editor_new (window, event, event->start, event->all_day, duplicate));
}

static void
on_reminder_event (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(CalEvent) event = cal_store_find_event_finish (CAL_STORE (source), result, &error);

  if (event)
    cal_window_show_event (window, event);
  else
    g_message ("reminder: %s", error->message);
  g_object_unref (window);
}

typedef struct {
  GtkWidget *window;
  char *calendar, *uid, *rid;
  gint64 start;
  gulong handler;
} Pending;

static void
pending_free (Pending *pending)
{
  g_free (pending->calendar);
  g_free (pending->uid);
  g_free (pending->rid);
  g_object_unref (pending->window);
  g_free (pending);
}

static gboolean
try_open (Pending *pending)
{
  Shell *shell = shell_of (pending->window);

  if (!cal_store_get_client (shell->store, pending->calendar) && !cal_store_is_ready (shell->store))
    return FALSE;
  cal_store_find_event (shell->store, pending->calendar, pending->uid, pending->rid, pending->start, NULL,
                        on_reminder_event, g_object_ref (pending->window));
  return TRUE;
}

static void
on_store_ready (CalStore *store, Pending *pending)
{
  if (!try_open (pending))
    return;
  g_signal_handler_disconnect (store, pending->handler);
  pending_free (pending);
}

void
cal_window_open_reminder (GtkWidget *window, const char *calendar, const char *uid, const char *rid, gint64 start)
{
  Shell *shell = shell_of (window);
  Pending *pending = g_new0 (Pending, 1);

  /* the views at that day, under the info (a notification tap opens the event in the app) */
  cal_window_go_to (window, cal_day_number (start));
  pending->window = g_object_ref (window);
  pending->calendar = g_strdup (calendar);
  pending->uid = g_strdup (uid);
  pending->rid = rid && *rid ? g_strdup (rid) : NULL;
  pending->start = start;
  if (try_open (pending))
    {
      pending_free (pending);
      return;
    }
  /* the app was just started: once the calendar is open */
  pending->handler = g_signal_connect (shell->store, "changed", G_CALLBACK (on_store_ready), pending);
}

void
cal_window_import (GtkWidget *window, GFile *file)
{
  event_editor_import (window, file);
}

/* --- toasts: "Event created.", "Event saved." (Toast.LENGTH_SHORT) */

#define TOAST_MS 2000                   /* Toast.LENGTH_SHORT */
#define TOAST_FADE_MS 250

static void
on_toast_anim (NeoAnim *anim, double value, gpointer data)
{
  Shell *shell = data;

  gtk_widget_set_opacity (shell->toast, value);
  gtk_widget_set_visible (shell->toast, value > 0);
}

static gboolean
on_toast_timeout (gpointer data)
{
  Shell *shell = data;

  shell->toast_timeout = 0;
  neo_anim_start (shell->toast_anim, neo_anim_get_value (shell->toast_anim), 0, TOAST_FADE_MS, 0, SPONGY_LINEAR);
  return G_SOURCE_REMOVE;
}

/* SystemUI's text toast (text_toast.xml): the app's icon and the text in a 28 dp pill, 48 dp above the bottom
 * (toast_y_offset), 300 dp wide at most. It outlives the page that showed it, so it belongs to the window. */
void
cal_window_toast (GtkWidget *window, const char *text)
{
  Shell *shell = shell_of (window);

  gtk_label_set_label (GTK_LABEL (shell->toast_label), text);
  g_clear_handle_id (&shell->toast_timeout, g_source_remove);
  neo_anim_start (shell->toast_anim, neo_anim_get_value (shell->toast_anim), 1, TOAST_FADE_MS, 0, SPONGY_LINEAR);
  shell->toast_timeout = g_timeout_add (TOAST_FADE_MS + TOAST_MS, on_toast_timeout, shell);
}

static GtkWidget *
make_toast (Shell *shell)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("de.yesman.Calendar");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "cal-toast-icon");
  gtk_box_append (GTK_BOX (box), icon);
  shell->toast_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (shell->toast_label), TRUE);
  gtk_label_set_lines (GTK_LABEL (shell->toast_label), 2);
  gtk_label_set_ellipsize (GTK_LABEL (shell->toast_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (shell->toast_label), 30);
  gtk_label_set_xalign (GTK_LABEL (shell->toast_label), 0);
  gtk_widget_add_css_class (shell->toast_label, "cal-toast-text");
  gtk_box_append (GTK_BOX (box), shell->toast_label);
  gtk_widget_add_css_class (box, "cal-toast");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_END);
  gtk_widget_set_can_target (box, FALSE);
  gtk_widget_set_visible (box, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Message"), -1);
  shell->toast = box;
  shell->toast_anim = neo_anim_new (box, on_toast_anim, shell);
  return box;
}

/* --- Back */

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  Shell *shell = data;

  if (cal_popup_dismiss (shell->window))
    return TRUE;
  if (shell->menu)
    close_menu (shell);
  else if (shell->drawer_open > 0)
    open_drawer (shell, FALSE);
  else if (shell->pushed->len)
    /* a page may ask first (the editor's "discard changes?") */
    cal_page_back (CAL_PAGE (shell->pushed->pdata[shell->pushed->len - 1]));
  else if (shell->back_to_previous)
    {
      shell->back_to_previous = FALSE;
      show_view (shell, shell->previous_view, shell->selected_day);
    }
  else
    gtk_window_close (GTK_WINDOW (shell->window));
  return TRUE;
}

/* --- the window */

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  g_clear_handle_id (&shell->midnight, g_source_remove);
  g_clear_handle_id (&shell->toast_timeout, g_source_remove);
  g_clear_pointer (&shell->toast_anim, neo_anim_free);
  g_cancellable_cancel (shell->query);
  g_clear_object (&shell->query);
  g_signal_handlers_disconnect_by_data (shell->store, shell);
  g_clear_pointer (&shell->drawer_anim, neo_anim_free);
  g_ptr_array_unref (shell->pushed);
  g_object_unref (shell->store);
  g_object_unref (shell->settings);
  g_free (shell);
}

static void
apply_home_zone (Shell *shell)
{
  g_autofree char *tzid = g_settings_get_string (shell->settings, "home-tz");

  cal_set_home_zone (g_settings_get_boolean (shell->settings, "home-tz-enabled") ? tzid : NULL);
  if (shell->store)
    cal_store_zone_changed (shell->store);
  gtk_widget_queue_draw (shell->window);
}

/* Display options (ViewDetailsPreferences.getPreferences): the Month view's layout for the orientation */
static void
apply_month_details (Shell *shell)
{
  const char *side = shell->landscape ? "landscape" : "portrait";
  g_autofree char *lines = g_strconcat ("month-lines-", side, NULL);
  g_autofree char *time = g_strconcat ("month-time-", side, NULL);
  g_autofree char *location = g_strconcat ("month-location-", side, NULL);

  if (shell->view_widgets[VIEW_MONTH])
    cal_month_view_set_details (CAL_MONTH_VIEW (shell->view_widgets[VIEW_MONTH]),
                                g_settings_get_int (shell->settings, lines), g_settings_get_int (shell->settings, time),
                                g_settings_get_boolean (shell->settings, location));
}

/* the window turned: the other orientation's Display options */
static void
on_surface_layout (GdkSurface *surface, int width, int height, Shell *shell)
{
  gboolean landscape = width > height;

  if (landscape == shell->landscape)
    return;
  shell->landscape = landscape;
  apply_month_details (shell);
}

static void
on_window_realize (GtkWidget *window, Shell *shell)
{
  g_signal_connect (gtk_native_get_surface (GTK_NATIVE (window)), "layout", G_CALLBACK (on_surface_layout), shell);
}

static void
on_settings_changed (GSettings *settings, const char *key, Shell *shell)
{
  if (g_str_has_prefix (key, "month-lines-") || g_str_has_prefix (key, "month-time-")
      || g_str_has_prefix (key, "month-location-"))
    {
      apply_month_details (shell);
      return;
    }
  if (g_str_equal (key, "week-start"))
    {
      cal_month_view_set_week_start (CAL_MONTH_VIEW (shell->view_widgets[VIEW_MONTH]), week_start (shell));
      show_view (shell, shell->view, shell->selected_day);
    }
  else if (g_str_equal (key, "real-event-colors"))
    {
      cal_set_real_event_colors (g_settings_get_boolean (settings, key));
      refresh (shell);
    }
  else if (g_str_equal (key, "days-per-week"))
    {
      cal_day_view_set_num_days (CAL_DAY_VIEW (shell->view_widgets[VIEW_WEEK]), g_settings_get_int (settings, key));
      if (shell->view == VIEW_WEEK)
        show_view (shell, shell->view, shell->selected_day);
    }
  else if (g_str_equal (key, "show-week-number"))
    {
      cal_month_view_set_show_week_number (CAL_MONTH_VIEW (shell->view_widgets[VIEW_MONTH]),
                                           g_settings_get_boolean (settings, key));
      update_title (shell);
    }
  else if (g_str_equal (key, "hide-declined"))
    refresh (shell);
  else if (g_str_equal (key, "staggered"))
    {
      cal_day_view_set_staggered (CAL_DAY_VIEW (shell->view_widgets[VIEW_DAY]), g_settings_get_boolean (settings, key));
      cal_day_view_set_staggered (CAL_DAY_VIEW (shell->view_widgets[VIEW_WEEK]),
                                  g_settings_get_boolean (settings, key));
    }
  else if (g_str_equal (key, "home-tz-enabled") || g_str_equal (key, "home-tz"))
    {
      /* Utils.setTimeZone: every time the app shows is in the new zone */
      apply_home_zone (shell);
      show_view (shell, shell->view, today ());
    }
}

GtkWidget *
cal_window_new (GtkApplication *app, CalStore *store, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = neo_window_new (app);
  GtkWidget *root = gtk_overlay_new ();
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *main = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *image = gtk_image_new_from_icon_name ("cal-add-symbolic");
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  GtkGesture *edge = gtk_gesture_drag_new ();
  g_autofree char *default_view = g_settings_get_string (settings, "default-view");
  /* "Default view": "Previously used view" is the one shown last (AllInOneActivity's KEY_START_VIEW) */
  g_autofree char *view_name = g_str_equal (default_view, "previous") ? g_settings_get_string (settings, "view")
                                                                      : g_strdup (default_view);
  GdkRGBA fab_fill = cal_argb (CAL_PRIMARY);
  ViewType view = VIEW_WEEK;

  cal_set_real_event_colors (g_settings_get_boolean (settings, "real-event-colors"));
  shell->window = window;
  shell->settings = g_object_ref (settings);
  shell->store = g_object_ref (store);
  /* before anything is shown: the store reads floating and all-day times in the home zone too */
  apply_home_zone (shell);
  shell->pushed = g_ptr_array_new ();
  shell->drawer_anim = neo_anim_new (window, on_drawer_anim, shell);
  g_object_set_data_full (G_OBJECT (window), "cal-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Calendar"));
  gtk_widget_add_css_class (window, "cal-window");

  /* the views */
  shell->views = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->views), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (shell->views), VIEW_FADE_MS);
  gtk_widget_set_vexpand (shell->views, TRUE);
  for (int i = 0; i < N_VIEWS; i++)
    {
      GtkWidget *widget;

      if (i == VIEW_DAY || i == VIEW_WEEK)
        {
          widget = cal_day_view_new (i == VIEW_DAY ? 1 : g_settings_get_int (settings, "days-per-week"));
          cal_day_view_set_cell_height (CAL_DAY_VIEW (widget), g_settings_get_double (settings, "cell-height"));
          cal_day_view_set_staggered (CAL_DAY_VIEW (widget), g_settings_get_boolean (settings, "staggered"));
          g_signal_connect (widget, "page", G_CALLBACK (on_page), shell);
          g_signal_connect (widget, "day-activated", G_CALLBACK (on_day_activated), shell);
          g_signal_connect (widget, "create-event", G_CALLBACK (on_create_event), shell);
          g_signal_connect (widget, "cell-height-changed", G_CALLBACK (on_cell_height), shell);
        }
      else if (i == VIEW_MONTH)
        {
          widget = cal_month_view_new ();
          cal_month_view_set_week_start (CAL_MONTH_VIEW (widget), week_start (shell));
          cal_month_view_set_show_week_number (CAL_MONTH_VIEW (widget),
                                               g_settings_get_boolean (settings, "show-week-number"));
          g_signal_connect (widget, "day-activated", G_CALLBACK (on_month_day), shell);
          g_signal_connect (window, "realize", G_CALLBACK (on_window_realize), shell);
          g_signal_connect (widget, "moved", G_CALLBACK (on_month_moved), shell);
        }
      else
        {
          widget = cal_agenda_view_new ();
          g_signal_connect (widget, "range-changed", G_CALLBACK (on_agenda_range), shell);
          g_signal_connect (widget, "top-day-changed", G_CALLBACK (on_agenda_top_day), shell);
        }
      /* the month view has no events to tap: a tap on a day opens the day (MonthWeekEventsView) */
      if (i != VIEW_MONTH)
        g_signal_connect (widget, "event-activated", G_CALLBACK (on_event_activated), shell);
      shell->view_widgets[i] = widget;
      gtk_stack_add_named (GTK_STACK (shell->views), widget, view_names[i]);
      if (view_name && g_str_equal (view_name, view_names[i]))
        view = i;
    }
  apply_month_details (shell);

  gtk_box_append (GTK_BOX (main), make_toolbar (shell));
  gtk_box_append (GTK_BOX (main), shell->views);
  gtk_overlay_set_child (GTK_OVERLAY (content), main);

  /* the FAB: an M3 FAB, a 16 dp corner (measured a rounded square), colorAccent, the white plus */
  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  shell->fab = neo_pressable_new (image);
  gtk_widget_add_css_class (shell->fab, "cal-fab");
  neo_pressable_set_fill (NEO_PRESSABLE (shell->fab), &fab_fill, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (shell->fab), NEO_RIPPLE_BOUNDED, 16);
  gtk_widget_set_halign (shell->fab, GTK_ALIGN_END);
  gtk_widget_set_valign (shell->fab, GTK_ALIGN_END);
  gtk_accessible_update_property (GTK_ACCESSIBLE (shell->fab), GTK_ACCESSIBLE_PROPERTY_LABEL, _("New event"), -1);
  g_signal_connect (shell->fab, "clicked", G_CALLBACK (on_fab), shell);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), shell->fab);

  shell->drawer = make_drawer (shell);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), shell->drawer);
  g_signal_connect (edge, "drag-begin", G_CALLBACK (on_edge_drag_begin), shell);
  g_signal_connect (edge, "drag-update", G_CALLBACK (on_edge_drag_update), shell);
  g_signal_connect (edge, "drag-end", G_CALLBACK (on_edge_drag_end), shell);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (edge), GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (main, GTK_EVENT_CONTROLLER (edge));

  shell->main = content;
  shell->pages = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (shell->pages), PAGE_MS);
  gtk_stack_add_named (GTK_STACK (shell->pages), content, "main");
  g_signal_connect (shell->pages, "notify::transition-running", G_CALLBACK (on_page_transition_done), shell);
  gtk_overlay_set_child (GTK_OVERLAY (root), shell->pages);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), make_toast (shell));
  neo_window_set_content (GTK_WINDOW (window), root);

  /* Escape is Back (a keyboard's; the shell's gesture presses a page's "Back" button, or sends Escape over the
   * drawer, which is a dialog to it) */
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);

  g_signal_connect (store, "changed", G_CALLBACK (on_store_changed), shell);
  g_signal_connect (store, "calendars-changed", G_CALLBACK (on_store_changed), shell);
  g_signal_connect (settings, "changed", G_CALLBACK (on_settings_changed), shell);
  schedule_midnight (shell);
  show_view (shell, view, today ());
  return window;
}
