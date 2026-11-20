/* CalAgendaView: Etar's Agenda (agenda-view.h; spec/calendar.md §5).
 *
 * Every row has a fixed height (a day's header 48 dp, an event 64 dp, each with its 1 px separator: the stills
 * measure exactly those, the text never needing more), so the list is drawn rather than built of widgets: a
 * SpongyScrollView scrolls one tall drawing of the rows near the screen, and the pinned header is drawn over it. */
#include "agenda-view.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

/* --- dimensions, dp (L/agenda_day.xml, L/agenda_item.xml, V/integers.xml; measured still:cal-agenda) */
#define DAY_MIN_HEIGHT 48.0
#define DAY_LEFT 16.0
#define DAY_LINE_TOP 4.0
#define DAY_LINE_BOTTOM -8.0
#define DAY_LINE_SIZE 12.0
#define DATE_LINE_SIZE 24.0
#define PAST_PRESENT_HEIGHT 2.0
#define ITEM_MIN_HEIGHT 64.0
#define CHIP_LEFT 16.0
#define CHIP_WIDTH 24.0
#define CHIP_TOP 8.0
#define CHIP_RIGHT 8.0
#define CHIP_HEIGHT 24.0                /* color_chip_height */
#define CHIP_ALL_DAY_HEIGHT 48.0        /* color_chip_all_day_height */
#define CHIP_BORDER_WIDTH 4             /* ColorChipView.DEF_BORDER_WIDTH, device pixels */
#define ITEM_RIGHT_MARGIN 4.0           /* agenda_item_right_margin */
#define TITLE_SIZE 18.0
#define WHEN_SIZE 14.0
#define WHERE_TOP -3.0
#define HISTORY_HEIGHT 32.0             /* the row where Minerva's run history begins */
#define HISTORY_TEXT_PADDING 8.0        /* the gap cut in its line around the text */

#define DAY_TEXT_COLOR CAL_AGENDA_DAY_TEXT
#define ITEM_TITLE_COLOR CAL_AGENDA_TITLE   /* agenda_item_standard_color */
#define ITEM_WHEN_COLOR CAL_AGENDA_WHEN     /* agenda_item_where_text_color */

/* AgendaWindowAdapter loads a window of days and more as the list nears an end */
#define WINDOW_BEFORE 30
#define WINDOW_AFTER 60
#define WINDOW_GROW 60

typedef enum { ROW_DAY, ROW_EVENT, ROW_HISTORY } RowType;

typedef struct {
  RowType type;
  int day;
  CalEvent *event;              /* ROW_EVENT: not a reference, the view's events array holds it */
  gboolean past_present;        /* ROW_DAY: the first day from today on, under the red line */
  double y, height;
} Row;

/* --- a row for assistive technologies: the list is drawn, so each row is an accessible object of its own (a
 * list item with its text and place), the list's accessible children */

#define CAL_TYPE_AGENDA_ROW_A11Y (cal_agenda_row_a11y_get_type ())
G_DECLARE_FINAL_TYPE (CalAgendaRowA11y, cal_agenda_row_a11y, CAL, AGENDA_ROW_A11Y, GObject)

struct _CalAgendaRowA11y {
  GObject parent;
  GtkATContext *context;
  GtkAccessible *list;          /* its parent: not a reference, the list holds the rows */
  CalAgendaRowA11y *next;       /* not a reference either */
  GtkAccessibleRole role;       /* GtkAccessible's "accessible-role": what GTK reads before the context is realized */
  double y, height;
};

enum { ROW_A11Y_PROP_ROLE = 1 };

static void cal_agenda_row_a11y_accessible_init (GtkAccessibleInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (CalAgendaRowA11y, cal_agenda_row_a11y, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_ACCESSIBLE, cal_agenda_row_a11y_accessible_init))

static GtkATContext *
row_a11y_get_at_context (GtkAccessible *accessible)
{
  CalAgendaRowA11y *self = CAL_AGENDA_ROW_A11Y (accessible);

  return self->context ? g_object_ref (self->context) : NULL;
}

static gboolean
row_a11y_get_platform_state (GtkAccessible *accessible, GtkAccessiblePlatformState state)
{
  return FALSE;
}

static GtkAccessible *
row_a11y_get_parent (GtkAccessible *accessible)
{
  CalAgendaRowA11y *self = CAL_AGENDA_ROW_A11Y (accessible);

  return self->list ? g_object_ref (self->list) : NULL;
}

static GtkAccessible *
row_a11y_get_first_child (GtkAccessible *accessible)
{
  return NULL;
}

static GtkAccessible *
row_a11y_get_next_sibling (GtkAccessible *accessible)
{
  CalAgendaRowA11y *self = CAL_AGENDA_ROW_A11Y (accessible);

  return self->next ? g_object_ref (GTK_ACCESSIBLE (self->next)) : NULL;
}

static gboolean
row_a11y_get_bounds (GtkAccessible *accessible, int *x, int *y, int *width, int *height)
{
  CalAgendaRowA11y *self = CAL_AGENDA_ROW_A11Y (accessible);

  if (!self->list)
    return FALSE;
  *x = 0;
  *y = (int) self->y;
  *width = gtk_widget_get_width (GTK_WIDGET (self->list));
  *height = (int) ceil (self->height);
  return TRUE;
}

static void
cal_agenda_row_a11y_accessible_init (GtkAccessibleInterface *iface)
{
  iface->get_at_context = row_a11y_get_at_context;
  iface->get_platform_state = row_a11y_get_platform_state;
  iface->get_accessible_parent = row_a11y_get_parent;
  iface->get_first_accessible_child = row_a11y_get_first_child;
  iface->get_next_accessible_sibling = row_a11y_get_next_sibling;
  iface->get_bounds = row_a11y_get_bounds;
}

static void
cal_agenda_row_a11y_dispose (GObject *object)
{
  g_clear_object (&CAL_AGENDA_ROW_A11Y (object)->context);
  G_OBJECT_CLASS (cal_agenda_row_a11y_parent_class)->dispose (object);
}

static void
row_a11y_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == ROW_A11Y_PROP_ROLE)
    g_value_set_enum (value, CAL_AGENDA_ROW_A11Y (object)->role);
}

static void
row_a11y_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == ROW_A11Y_PROP_ROLE)
    CAL_AGENDA_ROW_A11Y (object)->role = g_value_get_enum (value);
}

static void
cal_agenda_row_a11y_class_init (CalAgendaRowA11yClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cal_agenda_row_a11y_dispose;
  object_class->get_property = row_a11y_get_property;
  object_class->set_property = row_a11y_set_property;
  g_object_class_override_property (object_class, ROW_A11Y_PROP_ROLE, "accessible-role");
}

static void
cal_agenda_row_a11y_init (CalAgendaRowA11y *self)
{
}

static CalAgendaRowA11y *
row_a11y_new (GtkWidget *list, GtkAccessibleRole role, const char *label, double y, double height)
{
  CalAgendaRowA11y *self = g_object_new (CAL_TYPE_AGENDA_ROW_A11Y, "accessible-role", role, NULL);

  self->list = GTK_ACCESSIBLE (list);
  self->y = y;
  self->height = height;
  self->context = gtk_at_context_create (role, GTK_ACCESSIBLE (self), gtk_widget_get_display (list));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, label ? label : "", -1);
  return self;
}

/* --- the drawn list */

#define CAL_TYPE_AGENDA_LIST (cal_agenda_list_get_type ())
G_DECLARE_FINAL_TYPE (CalAgendaList, cal_agenda_list, CAL, AGENDA_LIST, GtkWidget)

struct _CalAgendaList {
  GtkWidget parent;
  CalAgendaView *view;
  NeoRipple ripple;
  int pressed;                  /* the row the ripple is on, or −1 */
  double drawn_from, drawn_to;  /* the part of the list the last snapshot drew */
  GPtrArray *a11y;              /* CalAgendaRowA11y, one per row, in order */
};

static void cal_agenda_list_accessible_init (GtkAccessibleInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (CalAgendaList, cal_agenda_list, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_ACCESSIBLE, cal_agenda_list_accessible_init))

/* the list's accessible children are its rows (it has no widget children) */
static GtkAccessible *
list_get_first_accessible_child (GtkAccessible *accessible)
{
  CalAgendaList *self = CAL_AGENDA_LIST (accessible);

  return self->a11y && self->a11y->len ? g_object_ref (GTK_ACCESSIBLE (self->a11y->pdata[0])) : NULL;
}

static void
cal_agenda_list_accessible_init (GtkAccessibleInterface *iface)
{
  iface->get_first_accessible_child = list_get_first_accessible_child;
}

struct _CalAgendaView {
  GtkWidget parent;
  GtkWidget *scroll;
  CalAgendaList *list;
  GPtrArray *events;
  GArray *rows;
  double content_height;
  int first, last;              /* the days loaded */
  int history_day;              /* the day Minerva's run history begins on (0: none to show), and what its row says */
  char *history_text;
  int top_day;
  /* growing the window: what it waits for, and the ends where a growth found nothing more (no more asking there
   * until the next go_to) */
  gboolean growing_before, growing_after, done_before, done_after;
  int first_row_day, last_row_day;

  /* where the list should be once it is laid out again: a row (its day, and its event or NULL for the header) and
   * how far its top is from the screen's */
  gboolean anchor_pending;
  gboolean loaded;              /* the events of the window have come (go_to waits for them to place the list) */
  int anchor_day;
  char *anchor_uid;
  gint64 anchor_start;
  double anchor_offset;
};

G_DEFINE_FINAL_TYPE (CalAgendaView, cal_agenda_view, GTK_TYPE_WIDGET)

enum { EVENT_ACTIVATED, RANGE_CHANGED, TOP_DAY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static int
today (void)
{
  return cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
}

static double
offset_of (CalAgendaView *self)
{
  return spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (self->scroll));
}

/* --- drawing a row */

static char *
day_text (int day)
{
  g_autofree char *text = cal_format_relative_weekday (day, today ());

  /* Utils.getDayOfWeekString, shown in capitals ("TODAY, SATURDAY") */
  return g_utf8_strup (text, -1);
}

static char *
date_text (int day)
{
  g_autoptr(GDateTime) a = g_date_time_new_from_unix_local (cal_day_start (day));
  g_autoptr(GDateTime) b = g_date_time_new_now_local ();

  /* FORMAT_SHOW_DATE: "November 7", with the year when it is not this one */
  if (g_date_time_get_year (a) == g_date_time_get_year (b))
    return cal_format_month_day (day);
  return cal_format_full_date (day);
}

static void
draw_day (GtkWidget *widget, GtkSnapshot *snapshot, int scale, const Row *row, double y, double width)
{
  double px = 1.0 / scale, top, day_baseline, day_height, date_baseline;
  GdkRGBA separator = cal_argb (row->past_present ? CAL_AGENDA_PAST_PRESENT : CAL_AGENDA_SEPARATOR);
  GdkRGBA text = cal_argb (DAY_TEXT_COLOR);
  g_autofree char *day = day_text (row->day), *date = date_text (row->day);
  g_autoptr(PangoLayout) day_layout = cal_text_layout (widget, day, DAY_LINE_SIZE, 700);
  g_autoptr(PangoLayout) date_layout = cal_text_layout (widget, date, DATE_LINE_SIZE, 700);

  /* the 1 px separator, or the 2 dp red past/present line in its place */
  top = row->past_present ? PAST_PRESENT_HEIGHT : px;
  cal_fill_rect (snapshot, scale, 0, y, width, top, &separator);
  cal_text_line (DAY_LINE_SIZE, scale, &day_baseline, &day_height);
  cal_text_line (DATE_LINE_SIZE, scale, &date_baseline, NULL);
  cal_text_ellipsize (day_layout, width - DAY_LEFT);
  cal_text_ellipsize (date_layout, width - DAY_LEFT);
  cal_text_draw (snapshot, day_layout, DAY_LEFT, y + top + DAY_LINE_TOP + day_baseline, 0, &text);
  cal_text_draw (snapshot, date_layout, DAY_LEFT,
                 y + top + DAY_LINE_TOP + day_height + DAY_LINE_BOTTOM + date_baseline, 0, &text);
}

/* where Minerva's run history begins: a line across the row, the text in a gap at its middle */
static void
draw_history (GtkWidget *widget, GtkSnapshot *snapshot, int scale, const char *text, double y, double width)
{
  double px = 1.0 / scale, baseline, height, text_width, mid = y + HISTORY_HEIGHT / 2;
  GdkRGBA line = cal_argb (CAL_AGENDA_SEPARATOR), color = cal_argb (ITEM_WHEN_COLOR);
  g_autoptr(PangoLayout) layout = cal_text_layout (widget, text ? text : "", WHEN_SIZE, 700);
  int w;

  cal_text_ellipsize (layout, width - 2 * (DAY_LEFT + HISTORY_TEXT_PADDING));
  pango_layout_get_pixel_size (layout, &w, NULL);
  text_width = w + 2 * HISTORY_TEXT_PADDING;
  cal_fill_rect (snapshot, scale, DAY_LEFT, mid, (width - text_width) / 2 - DAY_LEFT, px, &line);
  cal_fill_rect (snapshot, scale, (width + text_width) / 2, mid, (width - text_width) / 2 - DAY_LEFT, px, &line);
  cal_text_line (WHEN_SIZE, scale, &baseline, &height);
  cal_text_draw (snapshot, layout, width / 2, mid - height / 2 + baseline, 0.5, &color);
}

static void
draw_event (GtkWidget *widget, GtkSnapshot *snapshot, int scale, const Row *row, double y, double width)
{
  CalEvent *event = row->event;
  double px = 1.0 / scale, title_baseline, title_height, when_baseline, when_height, block, text_top;
  double text_x = CHIP_LEFT + CHIP_WIDTH + CHIP_RIGHT, text_width = width - text_x - ITEM_RIGHT_MARGIN;
  /* AgendaAdapter.bindView: a declined event's text in the declined colours and its chip faded (DRAW_FADED); an
   * unanswered invitation's chip a frame (DRAW_BORDER) */
  gboolean declined = cal_event_faded (event);   /* a declined invitation, or a run of Minerva's that is history */
  guint32 chip_rgb = cal_event_display_color (event);
  GdkRGBA separator = cal_argb (CAL_AGENDA_SEPARATOR);
  GdkRGBA chip = cal_argb (declined ? cal_declined_color (chip_rgb) : 0xff000000 | chip_rgb);
  GdkRGBA title_color = cal_argb (declined ? CAL_AGENDA_DECLINED_TITLE : ITEM_TITLE_COLOR);
  GdkRGBA when_color = cal_argb (declined ? CAL_AGENDA_DECLINED_WHEN : ITEM_WHEN_COLOR);
  double chip_height = cal_event_draws_as_all_day (event) ? CHIP_ALL_DAY_HEIGHT : CHIP_HEIGHT;
  gboolean has_where = event->location && *event->location;
  /* AgendaByDayAdapter: an event in the past or already started has its title in the regular face */
  gboolean started = event->all_day ? row->day <= today () : event->start <= g_get_real_time () / G_USEC_PER_SEC;
  g_autofree char *when = cal_format_agenda_when (event->start, event->end, event->all_day);
  g_autoptr(PangoLayout) title = cal_text_layout (widget, event->title && *event->title ? event->title
                                                                                         : _("(No title)"),
                                                  TITLE_SIZE, started ? 700 : 900);
  g_autoptr(PangoLayout) when_layout = cal_text_layout (widget, when, WHEN_SIZE, 700);

  if (event->cancelled)
    cal_text_strike (title);

  cal_fill_rect (snapshot, scale, 0, y, width, px, &separator);
  y += px;
  if (event->self_status == CAL_STATUS_INVITED)
    {
      /* ColorChipView.DRAW_BORDER: 4 px lines (DEF_BORDER_WIDTH, unscaled) inside its width and height less 1 px */
      double border = CHIP_BORDER_WIDTH * px, w = CHIP_WIDTH - px, h = chip_height - px;

      cal_fill_rect (snapshot, scale, CHIP_LEFT, y + CHIP_TOP, w, border, &chip);
      cal_fill_rect (snapshot, scale, CHIP_LEFT, y + CHIP_TOP + h - border, w, border, &chip);
      cal_fill_rect (snapshot, scale, CHIP_LEFT, y + CHIP_TOP, border, h, &chip);
      cal_fill_rect (snapshot, scale, CHIP_LEFT + w - border, y + CHIP_TOP, border, h, &chip);
    }
  else
    /* the colour chip, 24 dp wide, 48 dp tall for an all-day event and 24 dp for a timed one (DRAW_FULL) */
    cal_fill_rect (snapshot, scale, CHIP_LEFT, y + CHIP_TOP, CHIP_WIDTH, chip_height, &chip);
  /* the title, when and where, one line each, centred in the row */
  cal_text_line (TITLE_SIZE, scale, &title_baseline, &title_height);
  cal_text_line (WHEN_SIZE, scale, &when_baseline, &when_height);
  block = title_height + when_height + (has_where ? WHERE_TOP + when_height : 0);
  text_top = y + floor ((row->height - px - block) * scale / 2) / scale;
  cal_text_ellipsize (title, text_width);
  cal_text_ellipsize (when_layout, text_width);
  cal_text_draw (snapshot, title, text_x, text_top + title_baseline, 0, &title_color);
  cal_text_draw (snapshot, when_layout, text_x, text_top + title_height + when_baseline, 0, &when_color);
  if (has_where)
    {
      g_autoptr(PangoLayout) where = cal_text_layout (widget, event->location, WHEN_SIZE, 700);

      cal_text_ellipsize (where, text_width);
      cal_text_draw (snapshot, where, text_x, text_top + title_height + when_height + WHERE_TOP + when_baseline, 0,
                     &when_color);
    }
}

static void
cal_agenda_list_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalAgendaList *self = CAL_AGENDA_LIST (widget);
  CalAgendaView *view = self->view;
  int scale = neo_device_scale (widget);
  double width = gtk_widget_get_width (widget), page = gtk_widget_get_height (GTK_WIDGET (view));
  double offset = offset_of (view);
  GdkRGBA ripple = cal_ripple_color ();

  /* the rows a screen above and below what shows: scrolling within them needs no new drawing */
  self->drawn_from = offset - page;
  self->drawn_to = offset + 2 * page;
  for (guint i = 0; i < view->rows->len; i++)
    {
      const Row *row = &g_array_index (view->rows, Row, i);

      if (row->y + row->height < self->drawn_from || row->y > self->drawn_to)
        continue;
      if (row->type == ROW_DAY)
        draw_day (widget, snapshot, scale, row, row->y, width);
      else if (row->type == ROW_HISTORY)
        draw_history (widget, snapshot, scale, view->history_text, row->y, width);
      else
        draw_event (widget, snapshot, scale, row, row->y, width);
      if ((int) i == self->pressed && neo_ripple_is_active (&self->ripple))
        {
          graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, (float) row->y, (float) width, (float) row->height);

          gtk_snapshot_push_clip (snapshot, &bounds);
          neo_ripple_snapshot (&self->ripple, snapshot, &bounds, &ripple);
          gtk_snapshot_pop (snapshot);
        }
    }
}

static void
cal_agenda_list_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                         int *min_baseline, int *nat_baseline)
{
  CalAgendaList *self = CAL_AGENDA_LIST (widget);

  *min = *nat = orientation == GTK_ORIENTATION_VERTICAL ? (int) ceil (self->view->content_height) : 0;
}

static int
row_at (CalAgendaView *self, double y)
{
  for (guint i = 0; i < self->rows->len; i++)
    {
      const Row *row = &g_array_index (self->rows, Row, i);

      if (y >= row->y && y < row->y + row->height)
        return (int) i;
    }
  return -1;
}

static void
on_list_pressed (GtkGestureClick *gesture, int n, double x, double y, CalAgendaList *self)
{
  int index = row_at (self->view, y);

  /* the pinned header covers the top: a press there is on it, not on the row under it */
  if (index < 0 || g_array_index (self->view->rows, Row, index).type != ROW_EVENT
      || y - offset_of (self->view) < DAY_MIN_HEIGHT + PAST_PRESENT_HEIGHT)
    return;
  self->pressed = index;
  neo_ripple_press (&self->ripple, x, y - g_array_index (self->view->rows, Row, index).y);
}

static void
on_list_released (GtkGestureClick *gesture, int n, double x, double y, CalAgendaList *self)
{
  int index = self->pressed;

  if (index < 0)
    return;
  neo_ripple_release (&self->ripple);
  if (row_at (self->view, y) == index)
    g_signal_emit (self->view, signals[EVENT_ACTIVATED], 0, g_array_index (self->view->rows, Row, index).event);
}

static void
on_list_cancel (GtkGesture *gesture, GdkEventSequence *sequence, CalAgendaList *self)
{
  neo_ripple_release (&self->ripple);
}

static void
cal_agenda_list_dispose (GObject *object)
{
  neo_ripple_clear (&CAL_AGENDA_LIST (object)->ripple);
  g_clear_pointer (&CAL_AGENDA_LIST (object)->a11y, g_ptr_array_unref);
  G_OBJECT_CLASS (cal_agenda_list_parent_class)->dispose (object);
}

static void
cal_agenda_list_class_init (CalAgendaListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_agenda_list_dispose;
  widget_class->snapshot = cal_agenda_list_snapshot;
  widget_class->measure = cal_agenda_list_measure;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_LIST);
}

static void
cal_agenda_list_init (CalAgendaList *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->pressed = -1;
  self->a11y = g_ptr_array_new_with_free_func (g_object_unref);
  neo_ripple_init (&self->ripple, GTK_WIDGET (self));
  g_signal_connect (click, "pressed", G_CALLBACK (on_list_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_list_released), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_list_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

/* --- the rows */

static double
day_row_height (CalAgendaView *self, gboolean past_present)
{
  int scale = neo_device_scale (GTK_WIDGET (self));

  return (past_present ? PAST_PRESENT_HEIGHT : 1.0 / scale) + DAY_MIN_HEIGHT;
}

static double
event_row_height (CalAgendaView *self)
{
  return 1.0 / neo_device_scale (GTK_WIDGET (self)) + ITEM_MIN_HEIGHT;
}

/* the rows as assistive technologies read them: a day ("Today, Friday, November 20"), an event (its title, when
 * and where), the history's row (its text) */
static void
build_a11y (CalAgendaView *self)
{
  GPtrArray *a11y = self->list->a11y;

  g_ptr_array_set_size (a11y, 0);
  for (guint i = 0; i < self->rows->len; i++)
    {
      const Row *row = &g_array_index (self->rows, Row, i);
      g_autofree char *label = NULL;
      CalAgendaRowA11y *item;

      if (row->type == ROW_DAY)
        {
          g_autofree char *day = day_text (row->day), *date = date_text (row->day);

          label = g_strdup_printf ("%s, %s", day, date);
        }
      else if (row->type == ROW_HISTORY)
        label = g_strdup (self->history_text);
      else
        {
          CalEvent *event = row->event;
          g_autofree char *when = cal_format_agenda_when (event->start, event->end, event->all_day);
          const char *title = event->title && *event->title ? event->title : _("(No title)");

          label = event->location && *event->location ? g_strdup_printf ("%s, %s, %s", title, when, event->location)
                                                       : g_strdup_printf ("%s, %s", title, when);
        }
      item = row_a11y_new (GTK_WIDGET (self->list), GTK_ACCESSIBLE_ROLE_LIST_ITEM, label, row->y, row->height);
      if (a11y->len)
        CAL_AGENDA_ROW_A11Y (a11y->pdata[a11y->len - 1])->next = item;
      g_ptr_array_add (a11y, item);
    }
}

static void
build_rows (CalAgendaView *self)
{
  int now = today ();
  gboolean past_present_done = FALSE;
  double y = 0;
  /* the events of each day: those that cover it (a long event under each of its days), in the events' order */
  g_autoptr(GHashTable) days = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) g_ptr_array_unref);
  g_autoptr(GList) keys = NULL;

  g_array_set_size (self->rows, 0);
  for (guint i = 0; i < self->events->len; i++)
    {
      CalEvent *event = self->events->pdata[i];
      int a = MAX (cal_event_first_day (event), self->first), b = MIN (cal_event_last_day (event), self->last);

      for (int day = a; day <= b; day++)
        {
          GPtrArray *list = g_hash_table_lookup (days, GINT_TO_POINTER (day));

          if (!list)
            {
              list = g_ptr_array_new ();
              g_hash_table_insert (days, GINT_TO_POINTER (day), list);
            }
          g_ptr_array_add (list, event);
        }
    }
  /* today has its header even without events */
  if (now >= self->first && now <= self->last && !g_hash_table_contains (days, GINT_TO_POINTER (now)))
    g_hash_table_insert (days, GINT_TO_POINTER (now), g_ptr_array_new ());
  for (int day = self->first; day <= self->last; day++)
    {
      GPtrArray *list = g_hash_table_lookup (days, GINT_TO_POINTER (day));
      Row header = { ROW_DAY, day, NULL, FALSE, 0, 0 };

      /* the history's first day, with or without events: what is above it is no longer kept */
      if (day == self->history_day && day > self->first)
        {
          Row history = { ROW_HISTORY, day, NULL, FALSE, y, HISTORY_HEIGHT };

          y += history.height;
          g_array_append_val (self->rows, history);
        }
      if (!list)
        continue;
      if (!past_present_done && day >= now)
        header.past_present = past_present_done = TRUE;
      header.y = y;
      header.height = day_row_height (self, header.past_present);
      y += header.height;
      g_array_append_val (self->rows, header);
      for (guint i = 0; i < list->len; i++)
        {
          Row row = { ROW_EVENT, day, list->pdata[i], FALSE, y, event_row_height (self) };

          y += row.height;
          g_array_append_val (self->rows, row);
        }
    }
  self->content_height = y;
  build_a11y (self);
}

/* the y of the anchor row: the header of its day (or the first day after it), or its event under that day */
static double
anchor_y (CalAgendaView *self)
{
  const Row *found = NULL;

  for (guint i = 0; i < self->rows->len; i++)
    {
      const Row *row = &g_array_index (self->rows, Row, i);

      if (row->day < self->anchor_day)
        continue;
      if (!found)
        found = row;
      if (row->day > self->anchor_day || !self->anchor_uid)
        break;
      if (row->type == ROW_EVENT && g_strcmp0 (row->event->uid, self->anchor_uid) == 0
          && row->event->start == self->anchor_start)
        {
          found = row;
          break;
        }
    }
  return found ? found->y : self->content_height;
}

static void
set_anchor (CalAgendaView *self, int day, CalEvent *event, double offset)
{
  self->anchor_pending = TRUE;
  self->anchor_day = day;
  g_free (self->anchor_uid);
  self->anchor_uid = event ? g_strdup (event->uid) : NULL;
  self->anchor_start = event ? event->start : 0;
  self->anchor_offset = offset;
}

/* the row at the top of the screen, as the anchor to keep through a reload */
static void
keep_top (CalAgendaView *self)
{
  double offset = offset_of (self);
  int index = row_at (self, offset);
  const Row *row;

  if (self->anchor_pending || index < 0)
    return;
  row = &g_array_index (self->rows, Row, index);
  set_anchor (self, row->day, row->type == ROW_EVENT ? row->event : NULL, row->y - offset);
}

/* --- the pinned header and the day at the top */

static int
section_of (CalAgendaView *self, double y, int *next_header)
{
  int section = -1;

  *next_header = -1;
  for (guint i = 0; i < self->rows->len; i++)
    {
      const Row *row = &g_array_index (self->rows, Row, i);

      if (row->type != ROW_DAY)
        continue;
      if (row->y <= y)
        section = (int) i;
      else
        {
          *next_header = (int) i;
          break;
        }
    }
  return section;
}

static void
cal_agenda_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalAgendaView *self = CAL_AGENDA_VIEW (widget);
  double offset = offset_of (self), width = gtk_widget_get_width (widget);
  int scale = neo_device_scale (widget), next, section;
  GdkRGBA surface = cal_argb (CAL_SURFACE);

  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, (float) width,
                                                                      (float) gtk_widget_get_height (widget)));
  gtk_widget_snapshot_child (widget, self->scroll, snapshot);
  /* StickyHeaderListView: the header of the day at the top, over the list, pushed up by the next one */
  section = section_of (self, offset, &next);
  if (section >= 0 && offset > 0)
    {
      const Row *row = &g_array_index (self->rows, Row, section);
      double y = 0;

      if (next >= 0)
        y = MIN (0, g_array_index (self->rows, Row, next).y - offset - row->height);
      gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) (y + row->height)));
      gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, (float) y, (float) width,
                                                                          (float) row->height));
      draw_day (widget, snapshot, scale, row, y, width);
      gtk_snapshot_pop (snapshot);
    }
}

static void
check_window (CalAgendaView *self)
{
  double offset = offset_of (self), page = gtk_widget_get_height (GTK_WIDGET (self));
  gboolean grow = FALSE;

  if (!self->rows->len || page <= 0 || self->anchor_pending || self->growing_before || self->growing_after)
    return;
  /* AgendaWindowAdapter: near an end, the window takes in more days that way */
  if (offset < page && !self->done_before)
    {
      self->first -= WINDOW_GROW;
      self->growing_before = grow = TRUE;
    }
  if (offset + 2 * page > self->content_height && !self->done_after)
    {
      self->last += WINDOW_GROW;
      self->growing_after = grow = TRUE;
    }
  if (grow)
    g_signal_emit (self, signals[RANGE_CHANGED], 0);
}

static void
on_offset (GtkAdjustment *adjustment, CalAgendaView *self)
{
  double offset = offset_of (self);
  int next, section = section_of (self, offset, &next);
  int day = section >= 0 ? g_array_index (self->rows, Row, section).day : self->top_day;

  if (offset < self->list->drawn_from || offset + gtk_widget_get_height (GTK_WIDGET (self)) > self->list->drawn_to)
    gtk_widget_queue_draw (GTK_WIDGET (self->list));
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (day != self->top_day)
    {
      self->top_day = day;
      g_signal_emit (self, signals[TOP_DAY_CHANGED], 0, day);
    }
  check_window (self);
}

/* the scroll view has laid the new rows out: back to the anchor */
static void
on_layout (GtkAdjustment *adjustment, CalAgendaView *self)
{
  if (!self->anchor_pending || !self->loaded || !self->rows->len)
    return;
  self->anchor_pending = FALSE;
  gtk_adjustment_set_value (adjustment, anchor_y (self) - self->anchor_offset);
  gtk_widget_queue_draw (GTK_WIDGET (self->list));
}

/* --- GtkWidget */

static void
cal_agenda_view_dispose (GObject *object)
{
  CalAgendaView *self = CAL_AGENDA_VIEW (object);

  g_clear_pointer (&self->scroll, gtk_widget_unparent);
  g_clear_pointer (&self->events, g_ptr_array_unref);
  g_clear_pointer (&self->rows, g_array_unref);
  g_clear_pointer (&self->anchor_uid, g_free);
  g_clear_pointer (&self->history_text, g_free);
  G_OBJECT_CLASS (cal_agenda_view_parent_class)->dispose (object);
}

static void
cal_agenda_view_class_init (CalAgendaViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_agenda_view_dispose;
  widget_class->snapshot = cal_agenda_view_snapshot;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  signals[EVENT_ACTIVATED] = g_signal_new ("event-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[RANGE_CHANGED] = g_signal_new ("range-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[TOP_DAY_CHANGED] = g_signal_new ("top-day-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
cal_agenda_view_init (CalAgendaView *self)
{
  GtkAdjustment *adjustment;

  self->events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  self->rows = g_array_new (FALSE, FALSE, sizeof (Row));
  self->list = g_object_new (CAL_TYPE_AGENDA_LIST, NULL);
  self->list->view = self;
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), GTK_WIDGET (self->list));
  gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));
  adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));
  g_signal_connect (adjustment, "value-changed", G_CALLBACK (on_offset), self);
  g_signal_connect (adjustment, "changed", G_CALLBACK (on_layout), self);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
cal_agenda_view_new (void)
{
  CalAgendaView *self = g_object_new (CAL_TYPE_AGENDA_VIEW, NULL);

  cal_agenda_view_go_to (self, today ());
  return GTK_WIDGET (self);
}

void
cal_agenda_view_go_to (CalAgendaView *self, int day)
{
  self->first = day - WINDOW_BEFORE;
  self->last = day + WINDOW_AFTER;
  self->top_day = day;
  set_anchor (self, day, NULL, 0);
  self->loaded = FALSE;
  self->growing_before = self->growing_after = self->done_before = self->done_after = FALSE;
  /* the old rows go: what is on screen belongs to the old day until the events come */
  g_ptr_array_set_size (self->events, 0);
  build_rows (self);
  gtk_widget_queue_resize (GTK_WIDGET (self->list));
}

void
cal_agenda_view_get_range (CalAgendaView *self, int *first, int *last)
{
  *first = self->first;
  *last = self->last;
}

void
cal_agenda_view_set_history (CalAgendaView *self, int day, const char *text)
{
  if (self->history_day == day && g_strcmp0 (self->history_text, text) == 0)
    return;
  self->history_day = day;
  g_free (self->history_text);
  self->history_text = g_strdup (text);
}

void
cal_agenda_view_set_events (CalAgendaView *self, GPtrArray *events)
{
  double height = self->content_height;

  if (self->loaded)
    keep_top (self);
  self->loaded = TRUE;
  g_ptr_array_set_size (self->events, 0);
  for (guint i = 0; events && i < events->len; i++)
    g_ptr_array_add (self->events, cal_event_ref (events->pdata[i]));
  build_rows (self);
  if (self->rows->len)
    {
      int first = g_array_index (self->rows, Row, 0).day, last = g_array_index (self->rows, Row, self->rows->len - 1).day;

      if (self->growing_before && first == self->first_row_day)
        self->done_before = TRUE;
      if (self->growing_after && last == self->last_row_day)
        self->done_after = TRUE;
      self->first_row_day = first;
      self->last_row_day = last;
    }
  self->growing_before = self->growing_after = FALSE;
  self->list->pressed = -1;
  gtk_widget_queue_resize (GTK_WIDGET (self->list));
  /* the same height: the scroll view's adjustment will not change, place the list now */
  if (height == self->content_height && gtk_widget_get_height (GTK_WIDGET (self)) > 0)
    on_layout (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll)), self);
  gtk_widget_queue_draw (GTK_WIDGET (self->list));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
