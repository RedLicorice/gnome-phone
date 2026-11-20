/* CalDayView: Etar's DayView, the Week and Day views (day-view.h; spec/calendar.md §3). */
#include "day-view.h"
#include "cal-colors.h"
#include "cal-format.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy.h>

/* --- dimensions, dp (V/dimens.xml; E/DayView.java:114-451). Etar scales most of them by the density itself and
 * keeps HOUR_GAP and DAY_GAP at 1 device pixel: `px` below. */
#define MULTI_DAY_HEADER_HEIGHT 48.0    /* day_header_height */
#define DAY_HEADER_BOTTOM_MARGIN 4.0    /* day_header_bottom_margin */
#define DAY_HEADER_RIGHT_MARGIN 4.0     /* the labels' left inset in their column */
#define DATE_HEADER_FONT_SIZE 16.0      /* date_header_text_size */
#define DAY_HEADER_FONT_SIZE 11.0       /* day_label_text_size */
#define HOURS_TEXT_SIZE 12.0
#define HOURS_LEFT_MARGIN 4.0
#define HOURS_RIGHT_MARGIN 4.0
#define WEEK_EVENT_TEXT_SIZE 10.0       /* week_view_event_text_size */
#define DAY_EVENT_TEXT_SIZE 12.0        /* day_view_event_text_size */
#define NEW_EVENT_HINT_FONT_SIZE 12.0
#define EVENT_TEXT_VERTICAL_MARGIN 2.0
#define EVENT_TEXT_HORIZONTAL_MARGIN 4.0
#define EVENT_RECT_TOP_MARGIN 1.0
#define STAGGERED_EVENT_SHADOW_WIDTH 4.0        /* a staggered event's shadow, dp */
#define EVENT_RECT_LEFT_MARGIN 1.0
#define ALL_DAY_EVENT_RECT_BOTTOM_MARGIN 1.0
#define SINGLE_ALLDAY_HEIGHT 34.0
#define MAX_HEIGHT_OF_ONE_ALLDAY_EVENT 34.0
#define ALLDAY_TOP_MARGIN 1.0
#define MIN_HOURS_HEIGHT 180.0
#define EXPAND_ALL_DAY_BOTTOM_MARGIN 8.0        /* all_day_bottom_margin */
#define EVENT_SQUARE_WIDTH 10.0
#define EVENT_RECT_STROKE_WIDTH 2.0
#define EVENT_LINE_PADDING 4.0
#define CURRENT_TIME_LINE_SIDE_BUFFER 4.0
#define CURRENT_TIME_LINE_TOP_OFFSET 2.0
#define NEW_EVENT_MARGIN 4.0
#define NEW_EVENT_WIDTH 2.0
#define NEW_EVENT_MAX_LENGTH 16.0
#define MIN_CELL_WIDTH_FOR_TEXT 20.0
#define DEFAULT_CELL_HEIGHT 64.0
#define MAX_CELL_HEIGHT 150.0
#define EXPAND_ICON_SIZE 26.0           /* ic_expand_small's intrinsic size */

#define ANIMATION_DURATION 400          /* the all-day area expanding, ms */
#define CLICK_DISPLAY_MS 150            /* the clicked colour before info opens: tap timeout 100 + 50 ms */
#define SWITCH_MIN_VELOCITY 2200.0      /* px/s: calculateDuration's floor, so a slow release still moves briskly */
#define TOUCH_SLOP 8.0                  /* ViewConfiguration's touch slop, dp */

typedef enum { TOUCH_NONE, TOUCH_DOWN, TOUCH_VSCROLL, TOUCH_HSCROLL, TOUCH_PINCH } TouchMode;

struct _CalDayView {
  GtkWidget parent;
  int num_days;
  int first_day;
  GPtrArray *events;            /* every event given, sorted */
  GPtrArray *timed;             /* those drawn in the grid, laid out together */
  GPtrArray *all_day;           /* those drawn in the all-day area, laid out together */
  gboolean staggered;           /* Staggered display */
  gint64 history;               /* where Minerva's run history begins (0: not shown), and what its line says */
  char *history_text;

  double cell_height;           /* one hour, dp, without the gap */
  double view_start_y;          /* the grid's scroll, dp */
  double view_start_x;          /* a horizontal drag's offset: the next or previous period shows beside */
  gboolean scrolled_once;       /* the first layout scrolls to now */
  int initial_hour;

  gboolean show_all_all_day;    /* the all-day area expanded */
  double animate_all_day_height;        /* during the expand animation, else 0 */
  NeoAnim *expand_anim;

  gboolean has_selection, selection_all_day;
  int selection_day, selection_hour;
  CalEvent *clicked;            /* flashing #8033B5E5 before its info opens */
  guint click_timeout;

  TouchMode touch;
  double down_x, down_y, start_view_y;
  SpongyVelocity velocity;
  SpongyFling fling;
  guint tick;
  NeoAnim *page_anim;
  double page_from, page_to;
  double pinch_cell_height, pinch_focus;

  double hours_text_width;      /* the widest hour label, unrounded (dp) */
  PangoLayout *hour_layouts[24];
  gboolean hours_24;
  guint minute_timer;
};

G_DEFINE_FINAL_TYPE (CalDayView, cal_day_view, GTK_TYPE_WIDGET)

enum { EVENT_ACTIVATED, CREATE_EVENT, DAY_ACTIVATED, PAGE, CELL_HEIGHT_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* --- fonts: DayView's Paint draws Roboto as asked, Regular or Bold; the Bold-text setting is TextView's and does
 * not reach a canvas (measured: the hour labels are Regular on the still) */

static PangoFontDescription *
font (double size, int weight)
{
  PangoFontDescription *description = pango_font_description_from_string ("Neo Roboto, Roboto");

  pango_font_description_set_absolute_size (description, size * PANGO_SCALE);
  pango_font_description_set_weight (description, weight);
  return description;
}

static PangoLayout *
make_layout (CalDayView *self, const char *text, double size, int weight)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), text);
  PangoFontDescription *description = font (size, weight);

  pango_layout_set_font_description (layout, description);
  pango_font_description_free (description);
  return layout;
}

/* Paint.FontMetrics bottom − top + 1, as DayView measures its event font: Roboto's bounding box is 1.327 em */
static double
event_font_height (double size, double px)
{
  return round (size * 1.3267 / px) * px + px;
}

/* --- metrics of one page (one week or day), as DayView.remeasure */

typedef struct {
  double px;
  double width, height;
  double header_height;
  double hours_width;
  double pitch;                 /* an hour with its gap */
  double min_unexpanded;        /* MIN_UNEXPANDED_ALLDAY_EVENT_HEIGHT */
  int max_all_day;              /* rows the page's all-day events need */
  int max_unexpanded_count;
  double all_day_height;
  double first_cell;            /* where the grid starts */
  double grid_height;
  double max_view_start_y;
  double event_text_size;
} Metrics;

static double
day_left (const Metrics *m, int num_days, int day)
{
  /* computeDayLeftPosition: day · (width − hours) / days + hours, in whole device pixels */
  return round ((day * (m->width - m->hours_width) / num_days + m->hours_width) / m->px) * m->px;
}

static int
page_all_day_rows (CalDayView *self, int first_day)
{
  int rows = 0;

  for (guint i = 0; i < self->all_day->len; i++)
    {
      CalEvent *event = self->all_day->pdata[i];

      if (cal_event_first_day (event) <= first_day + self->num_days - 1 && cal_event_last_day (event) >= first_day)
        rows = MAX (rows, event->column + 1);
    }
  return rows;
}

static void
measure_page (CalDayView *self, int first_day, Metrics *m)
{
  int scale = neo_device_scale (GTK_WIDGET (self));
  double max_unexpanded_height;

  m->px = 1.0 / MAX (scale, 1);
  m->width = gtk_widget_get_width (GTK_WIDGET (self));
  m->height = gtk_widget_get_height (GTK_WIDGET (self));
  m->header_height = self->num_days > 1 ? MULTI_DAY_HEADER_HEIGHT : 0;
  /* computeMaxStringWidth rounds the widest label to a whole device pixel: 127 px measured, where a width rounded
   * up to a whole dp made it 129 */
  m->hours_width = HOURS_LEFT_MARGIN + round (self->hours_text_width * MAX (scale, 1)) * m->px + HOURS_RIGHT_MARGIN;
  m->pitch = self->cell_height + m->px;
  m->event_text_size = self->num_days > 1 ? WEEK_EVENT_TEXT_SIZE : DAY_EVENT_TEXT_SIZE;
  /* MIN_EVENT_HEIGHT: a line of event text and its margins (E/DayView.java:753) */
  m->min_unexpanded = event_font_height (m->event_text_size, m->px) + EVENT_RECT_TOP_MARGIN
                      + 2 * EVENT_TEXT_VERTICAL_MARGIN + ALL_DAY_EVENT_RECT_BOTTOM_MARGIN;

  /* shrink to fit, but at least two events */
  max_unexpanded_height = MIN (m->min_unexpanded * 4, m->height / 6);
  max_unexpanded_height = MAX (max_unexpanded_height, m->min_unexpanded * 2);
  m->max_unexpanded_count = (int) (max_unexpanded_height / m->min_unexpanded);
  m->max_all_day = page_all_day_rows (self, first_day);

  m->all_day_height = 0;
  m->first_cell = m->header_height;
  if (m->max_all_day > 0)
    {
      double most = m->height - m->header_height - MIN_HOURS_HEIGHT;

      if (m->max_all_day == 1)
        m->all_day_height = SINGLE_ALLDAY_HEIGHT;
      else if (m->max_all_day <= m->max_unexpanded_count)
        m->all_day_height = MIN (m->max_all_day * MAX_HEIGHT_OF_ONE_ALLDAY_EVENT, max_unexpanded_height);
      else if (self->animate_all_day_height > 0)
        m->all_day_height = MAX (self->animate_all_day_height, max_unexpanded_height);
      else
        {
          m->all_day_height = m->max_all_day * m->min_unexpanded;
          if (!self->show_all_all_day && m->all_day_height > max_unexpanded_height)
            m->all_day_height = m->max_unexpanded_count * m->min_unexpanded;
          else if (m->all_day_height > most)
            m->all_day_height = most;
        }
      m->first_cell = m->header_height + m->all_day_height + ALLDAY_TOP_MARGIN;
    }
  m->grid_height = m->height - m->first_cell;
  m->max_view_start_y = MAX (0, m->px + 24 * m->pitch - m->grid_height);
}

/* --- the hour labels */

static void
update_hour_labels (CalDayView *self)
{
  double widest = 0;
  const char *wide[] = { "12 AM", "12 PM", "22:00" };

  self->hours_24 = cal_format_uses_24h ();
  for (int hour = 0; hour < 24; hour++)
    {
      g_autofree char *text = self->hours_24 ? g_strdup_printf ("%02d:00", hour)
                                             : g_strdup_printf ("%d %s", hour % 12 ? hour % 12 : 12,
                                                                hour < 12 ? "AM" : "PM");

      g_clear_object (&self->hour_layouts[hour]);
      self->hour_layouts[hour] = make_layout (self, text, HOURS_TEXT_SIZE, PANGO_WEIGHT_NORMAL);
    }
  /* mHoursWidth: the margins and the widest label, the 12-hour ones included whatever the setting
   * (computeMaxStringWidth over "12 AM", "12 PM" and "22:00"; measured 127 px) */
  for (guint i = 0; i < G_N_ELEMENTS (wide); i++)
    {
      g_autoptr(PangoLayout) layout = make_layout (self, wide[i], HOURS_TEXT_SIZE, PANGO_WEIGHT_NORMAL);
      PangoRectangle logical;

      pango_layout_get_extents (layout, NULL, &logical);
      widest = MAX (widest, (double) logical.width / PANGO_SCALE);
    }
  self->hours_text_width = widest;
}

/* --- drawing */

static void
fill (GtkSnapshot *snapshot, guint32 argb, double x, double y, double w, double h)
{
  GdkRGBA rgba = cal_argb (argb);

  if (w > 0 && h > 0)
    gtk_snapshot_append_color (snapshot, &rgba, &GRAPHENE_RECT_INIT (x, y, w, h));
}

static void
draw_text (GtkSnapshot *snapshot, PangoLayout *layout, double x, double baseline, guint32 argb)
{
  GdkRGBA rgba = cal_argb (argb);

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, baseline - pango_layout_get_baseline (layout)
                                                                         / (double) PANGO_SCALE));
  gtk_snapshot_append_layout (snapshot, layout, &rgba);
  gtk_snapshot_restore (snapshot);
}

/* the event's text, DayView.getEventLayout: the title bold, a space, the place; wrapped to the rect, and only the
 * lines that fit whole (drawEventText); centred vertically in the all-day area */
static void
draw_event_text (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, CalEvent *event, double left,
                 double top, double right, double bottom, gboolean center)
{
  g_autofree char *title = g_markup_escape_text (event->title && *event->title ? event->title : _("(No title)"), -1);
  g_autofree char *place = event->location ? g_markup_escape_text (event->location, -1) : NULL;
  /* a cancelled event's title struck through (setStrikeThruText; the place too, as the paint draws both) */
  g_autofree char *markup = g_strdup_printf ("%s<b>%s</b>%s%s%s", event->cancelled ? "<s>" : "", title,
                                             place ? " " : "", place ? place : "", event->cancelled ? "</s>" : "");
  guint32 display = 0xff000000 | cal_event_display_color (event);
  /* the text: an unanswered invitation's in the event's colour, any other adaptive (white, or with real event
   * colours black or white by the colour's brightness); a declined event's alpha Etar sets is undone by the
   * setColor after it */
  guint32 text = event->self_status == CAL_STATUS_INVITED
                 ? (cal_adaptive_text_color (0xffffffff, display) & 0xff000000) | (display & 0xffffff)
                 : cal_adaptive_text_color (0xffffffff, display);
  g_autoptr(PangoLayout) layout = NULL;
  PangoLayoutIter *iter;
  double width = right - left, height = bottom - top, fits = 0;

  if (width < MIN_CELL_WIDTH_FOR_TEXT || height <= 0)
    return;
  layout = make_layout (self, NULL, m->event_text_size, PANGO_WEIGHT_NORMAL);
  pango_layout_set_markup (layout, markup, -1);
  {
    /* StaticLayout's default hyphenation is NONE: a word too long for the line breaks without a hyphen */
    PangoAttrList *attrs = pango_layout_get_attributes (layout) ? pango_attr_list_copy (pango_layout_get_attributes (layout))
                                                                : pango_attr_list_new ();

    pango_attr_list_insert (attrs, pango_attr_insert_hyphens_new (FALSE));
    pango_layout_set_attributes (layout, attrs);
    pango_attr_list_unref (attrs);
  }
  pango_layout_set_width (layout, (int) (width * PANGO_SCALE));
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  iter = pango_layout_get_iter (layout);
  do
    {
      PangoRectangle line;

      pango_layout_iter_get_line_extents (iter, NULL, &line);
      if ((line.y + line.height) / (double) PANGO_SCALE <= height)
        fits = (line.y + line.height) / (double) PANGO_SCALE;
      else
        break;
    }
  while (pango_layout_iter_next_line (iter));
  pango_layout_iter_free (iter);
  if (fits <= 0)
    return;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (left, top + (center ? (height - fits) / 2 : 0), width, fits));
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (left, top + (center ? (height - fits) / 2 : 0)));
  gtk_snapshot_append_layout (snapshot, layout, &(GdkRGBA) { ((text >> 16) & 0xff) / 255.f,
                                                             ((text >> 8) & 0xff) / 255.f, (text & 0xff) / 255.f,
                                                             (text >> 24) / 255.f });
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

/* EventGeometry.computeEventRect for a timed event on @day (an index in the page) */
static gboolean
event_rect (CalDayView *self, const Metrics *m, CalEvent *event, int day_number, int index, graphene_rect_t *rect)
{
  int first = cal_event_first_day (event), last = cal_event_last_day (event);
  int start_minute, end_minute, start_hour, end_hour;
  double minute_height = self->cell_height / 60, left, cell_width, col_width, top, bottom;

  if (first > day_number || last < day_number)
    return FALSE;
  start_minute = first < day_number ? 0 : cal_minute_of_day (event->start);
  end_minute = last > day_number ? 24 * 60 : cal_minute_of_day (event->end);
  if (end_minute == 0 && event->end > event->start)
    end_minute = 24 * 60;       /* to midnight */
  start_hour = start_minute / 60;
  end_hour = end_minute / 60;
  /* ending on an hour line counts as the hour before, not to cross into the next cell */
  if (end_hour * 60 == end_minute)
    end_hour--;
  top = (int) (start_minute * minute_height / m->px) * m->px + start_hour * m->px;
  bottom = (int) (end_minute * minute_height / m->px) * m->px + end_hour * m->px - m->px;
  if (bottom < top + m->min_unexpanded - ALL_DAY_EVENT_RECT_BOTTOM_MARGIN)
    bottom = top + m->min_unexpanded - ALL_DAY_EVENT_RECT_BOTTOM_MARGIN;

  /* drawEvents: the day's cell, then its share for the event's column */
  left = day_left (m, self->num_days, index) + m->px;
  cell_width = day_left (m, self->num_days, index + 1) - left - m->px;
  if (event->staggered)
    {
      /* each column starts an offset further right and ends one further left than the one above it; column 0 has
       * the whole width */
      double offset, right;

      col_width = cell_width - 2 * m->px;
      offset = col_width / (event->max_columns * 2 + 4);
      left += m->px;
      right = event->column == 0 ? left + col_width
                                 : left + col_width - (event->max_columns - 1 - event->column) * offset;
      left += event->column * offset;
      *rect = GRAPHENE_RECT_INIT (left, top, right - left, bottom - top);
      return TRUE;
    }
  col_width = (cell_width - (event->max_columns + 1) * m->px) / event->max_columns;
  left += event->column * (col_width + m->px);
  *rect = GRAPHENE_RECT_INIT (left, top, col_width, bottom - top);
  return TRUE;
}

/* the top of an event's text: textTop, where its unobscured part begins (0 unless staggered) */
static double
event_text_top (CalDayView *self, CalEvent *event, const graphene_rect_t *rect)
{
  return rect->origin.y + (int) (event->text_offset / 60.0 * self->cell_height / 60);
}

/* drawEventRect's colour: the clicked colour, else the event's, faded when declined */
static guint32
event_color (CalDayView *self, CalEvent *event)
{
  if (event == self->clicked)
    return CAL_CLICKED;
  if (cal_event_faded (event))
    return cal_declined_color (cal_event_display_color (event));
  return 0xff000000 | cal_event_display_color (event);
}

/* drawEventRect: filled; an invitation not answered yet (ATTENDEE_STATUS_INVITED) as a 2 dp frame (Style.STROKE
 * on the rect moved in by half the stroke: the frame's outside is the filled rect's) */
static void
draw_event_rect (CalDayView *self, GtkSnapshot *snapshot, CalEvent *event, double left, double top, double right,
                 double bottom)
{
  guint32 color = event_color (self, event);
  double w = right - left, h = bottom - top, stroke = EVENT_RECT_STROKE_WIDTH;

  if (event->self_status != CAL_STATUS_INVITED || event == self->clicked)
    {
      fill (snapshot, color, left, top, w, h);
      return;
    }
  fill (snapshot, color, left, top, w, MIN (stroke, h));
  fill (snapshot, color, left, MAX (top, bottom - stroke), w, MIN (stroke, h));
  if (h > 2 * stroke)
    {
      fill (snapshot, color, left, top + stroke, MIN (stroke, w), h - 2 * stroke);
      fill (snapshot, color, MAX (left, right - stroke), top + stroke, MIN (stroke, w), h - 2 * stroke);
    }
}

/* the highest column a timed event of the page takes: staggered events draw column by column, the higher ones over
 * the lower (drawEvents sorts the visible ones by column), and a tap finds the highest first */
static int
max_column (CalDayView *self)
{
  int columns = 0;

  if (!self->staggered)
    return 0;
  for (guint i = 0; i < self->timed->len; i++)
    columns = MAX (columns, ((CalEvent *) self->timed->pdata[i])->column);
  return columns;
}

static void
draw_timed_event (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, CalEvent *event,
                  const graphene_rect_t *r)
{
  double top, bottom, left, right;

  /* drawEventRect: 1 dp from the top and the left */
  top = r->origin.y + EVENT_RECT_TOP_MARGIN;
  bottom = r->origin.y + r->size.height;
  left = r->origin.x + EVENT_RECT_LEFT_MARGIN;
  right = r->origin.x + r->size.width;
  if (event->staggered && event->column > 0)
    {
      /* setShadowLayer(4 dp, staggered_event_shadow_color): Skia's blur sigma 0.57735 r + 0.5 px, a CSS blur
       * radius twice the sigma */
      GskRoundedRect box;
      GdkRGBA shadow = cal_argb (CAL_STAGGERED_SHADOW);

      gsk_rounded_rect_init_from_rect (&box, &GRAPHENE_RECT_INIT (left, top, right - left, bottom - top), 0);
      gtk_snapshot_append_outset_shadow (snapshot, &box, &shadow, 0, 0, 0,
                                         2 * (0.57735 * STAGGERED_EVENT_SHADOW_WIDTH + 0.5 * m->px));
    }
  draw_event_rect (self, snapshot, event, left, top, right, bottom);
  /* getEventTextRect: the text from textTop down */
  top = event_text_top (self, event, r) + EVENT_RECT_TOP_MARGIN;
  /* setupTextRect: the text margins when there is room for them */
  if (bottom - top > 2 * EVENT_TEXT_VERTICAL_MARGIN)
    {
      top += EVENT_TEXT_VERTICAL_MARGIN;
      bottom -= EVENT_TEXT_VERTICAL_MARGIN;
    }
  if (right - left > 2 * EVENT_TEXT_HORIZONTAL_MARGIN)
    {
      left += EVENT_TEXT_HORIZONTAL_MARGIN;
      right -= EVENT_TEXT_HORIZONTAL_MARGIN;
    }
  draw_event_text (self, snapshot, m, event, left, top, right, bottom, FALSE);
}

static void
draw_timed_events (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, int first_day)
{
  int columns = max_column (self);

  for (int index = 0; index < self->num_days; index++)
    for (int column = 0; column <= columns; column++)
      for (guint i = 0; i < self->timed->len; i++)
        {
          CalEvent *event = self->timed->pdata[i];
          graphene_rect_t r;

          if ((self->staggered && event->column != column) || !event_rect (self, m, event, first_day + index, index, &r))
            continue;
          draw_timed_event (self, snapshot, m, event, &r);
        }
}

/* the all-day events of a page (drawAllDayEvents), with "□ +N" for those that do not fit unexpanded; @skipped is
 * filled per day */
static void
draw_all_day_events (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, int first_day)
{
  int last_day = first_day + self->num_days - 1, skipped[7] = { 0 };
  double y = m->header_height + ALLDAY_TOP_MARGIN, clip = m->header_height + m->all_day_height + ALLDAY_TOP_MARGIN;
  double rows = m->max_all_day, cell_width = (m->width - m->hours_width) / self->num_days - m->px;
  gboolean more = FALSE, many = m->max_all_day > m->max_unexpanded_count;

  if (many && !self->show_all_all_day && self->animate_all_day_height == 0)
    {
      /* one fewer than fits, and the "+N" under them */
      rows = m->max_unexpanded_count - 1;
      clip -= m->min_unexpanded;
      more = TRUE;
    }
  else if (self->animate_all_day_height != 0)
    clip = m->header_height + self->animate_all_day_height + ALLDAY_TOP_MARGIN;

  for (guint i = 0; i < self->all_day->len; i++)
    {
      CalEvent *event = self->all_day->pdata[i];
      int start = cal_event_first_day (event), end = cal_event_last_day (event);
      double left_offset = 0, right_offset = 0, height, left, right, top, bottom;

      if (start > last_day || end < first_day)
        continue;
      if (start < first_day)
        start = first_day;
      else if (!event->all_day)
        left_offset = cal_minute_of_day (event->start) * cell_width / (24 * 60);
      if (end > last_day)
        end = last_day;
      else if (!event->all_day)
        right_offset = (24 * 60 - cal_minute_of_day (event->end)) * cell_width / (24 * 60);
      height = many ? m->min_unexpanded : m->all_day_height / MAX (rows, 1);
      height = MIN (height, MAX_HEIGHT_OF_ONE_ALLDAY_EVENT);

      left = day_left (m, self->num_days, start - first_day) + left_offset;
      right = day_left (m, self->num_days, end - first_day + 1) - m->px - right_offset;
      top = y + height * event->column;
      bottom = top + height - ALL_DAY_EVENT_RECT_BOTTOM_MARGIN;
      if (many)
        {
          if (top >= clip || (bottom > clip && more))
            {
              for (int d = start; d <= end; d++)
                skipped[d - first_day]++;
              continue;
            }
          if (bottom > clip)
            bottom = clip;
        }
      draw_event_rect (self, snapshot, event, left + EVENT_RECT_LEFT_MARGIN, top + EVENT_RECT_TOP_MARGIN, right,
                       bottom);
      /* getAllDayEventTextRect */
      top += EVENT_RECT_TOP_MARGIN;
      left += EVENT_RECT_LEFT_MARGIN;
      if (bottom - top > 2 * EVENT_TEXT_VERTICAL_MARGIN)
        {
          top += EVENT_TEXT_VERTICAL_MARGIN;
          bottom -= EVENT_TEXT_VERTICAL_MARGIN;
        }
      if (right - left > 2 * EVENT_TEXT_HORIZONTAL_MARGIN)
        {
          left += EVENT_TEXT_HORIZONTAL_MARGIN;
          right -= EVENT_TEXT_HORIZONTAL_MARGIN;
        }
      draw_event_text (self, snapshot, m, event, left, top, right, bottom, TRUE);
    }

  /* drawMoreAlldayEvents: a 10 dp square outlined 2 dp, then "+N" in the event font, #666666 */
  if (more)
    for (int d = 0; d < self->num_days; d++)
      {
        g_autofree char *text = NULL;
        g_autoptr(PangoLayout) layout = NULL;
        double x, sy;

        if (!skipped[d])
          continue;
        x = day_left (m, self->num_days, d) + EVENT_TEXT_HORIZONTAL_MARGIN;
        sy = m->all_day_height - 0.5 * m->min_unexpanded - 0.5 * EVENT_SQUARE_WIDTH + m->header_height
             + ALLDAY_TOP_MARGIN;
        fill (snapshot, CAL_MORE_EVENTS, x - 1, sy - 1, EVENT_SQUARE_WIDTH + 2, EVENT_RECT_STROKE_WIDTH);
        fill (snapshot, CAL_MORE_EVENTS, x - 1, sy + EVENT_SQUARE_WIDTH - 1, EVENT_SQUARE_WIDTH + 2,
              EVENT_RECT_STROKE_WIDTH);
        fill (snapshot, CAL_MORE_EVENTS, x - 1, sy - 1, EVENT_RECT_STROKE_WIDTH, EVENT_SQUARE_WIDTH + 2);
        fill (snapshot, CAL_MORE_EVENTS, x + EVENT_SQUARE_WIDTH - 1, sy - 1, EVENT_RECT_STROKE_WIDTH,
              EVENT_SQUARE_WIDTH + 2);
        /* month_more_events: "+%d" */
        text = g_strdup_printf (C_("more events", "+%d"), skipped[d]);
        layout = make_layout (self, text, m->event_text_size, PANGO_WEIGHT_NORMAL);
        draw_text (snapshot, layout, x + EVENT_SQUARE_WIDTH + EVENT_LINE_PADDING, sy + EVENT_SQUARE_WIDTH,
                   CAL_MORE_EVENTS);
      }
}

/* the red line at now: timeline_indicator_holo_light, 2 dp of red over a soft shadow, across the day ± 4 dp */
static void
draw_now_line (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, int index, double line_y)
{
  double left = day_left (m, self->num_days, index), right = day_left (m, self->num_days, index + 1);
  double top = line_y - CURRENT_TIME_LINE_TOP_OFFSET + 3 * m->px;

  if (self->num_days > 1)
    {
      left = MAX (left - CURRENT_TIME_LINE_SIDE_BUFFER, m->hours_width) + m->px;
      right = MIN (right + CURRENT_TIME_LINE_SIDE_BUFFER, m->width);
    }
  else
    left += m->px;
  /* the 9-patch's shadow: measured #231616 under the line, fading over ~4 dp */
  for (int i = 0; i < 4; i++)
    fill (snapshot, ((guint32) (0x60 - i * 0x18) << 24) | 0x3a0000, left, top + 5 * m->px + i * m->px * 3, right - left,
          3 * m->px);
  fill (snapshot, CAL_NOW_LINE, left, top, right - left, 5 * m->px);
}

/* where Minerva's run history begins: a line across its day at its time, the text over the line's start */
static void
draw_history_line (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, int first_day)
{
  int index = self->history ? cal_day_number (self->history) - first_day : -1, minute;
  double left, right, y;

  if (index < 0 || index >= self->num_days)
    return;
  minute = cal_minute_of_day (self->history);
  y = (minute / 60) * m->pitch + (minute % 60) * self->cell_height / 60 + m->px;
  left = day_left (m, self->num_days, index) + m->px;
  right = day_left (m, self->num_days, index + 1);
  fill (snapshot, CAL_HOUR_LABEL, left, y, right - left, 2 * m->px);
  if (self->history_text && right - left >= MIN_CELL_WIDTH_FOR_TEXT)
    {
      g_autoptr(PangoLayout) layout = make_layout (self, self->history_text, m->event_text_size, PANGO_WEIGHT_NORMAL);

      pango_layout_set_width (layout, (int) ((right - left - 2 * EVENT_TEXT_HORIZONTAL_MARGIN) * PANGO_SCALE));
      pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
      draw_text (snapshot, layout, left + EVENT_TEXT_HORIZONTAL_MARGIN, y - EVENT_TEXT_VERTICAL_MARGIN, CAL_HOUR_LABEL);
    }
}

static void
draw_header (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m, int first_day, int today)
{
  for (int index = 0; index < self->num_days; index++)
    {
      int day = first_day + index, weekday = cal_day_of_week (day);
      g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (cal_day_start (day));
      g_autofree char *number = g_strdup_printf ("%d", g_date_time_get_day_of_month (date));
      g_autofree char *name = cal_format_weekday_short (day);
      g_autoptr(PangoLayout) number_layout = make_layout (self, number, DATE_HEADER_FONT_SIZE,
                                                          day == today ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
      g_autoptr(PangoLayout) name_layout = make_layout (self, name, DAY_HEADER_FONT_SIZE, PANGO_WEIGHT_NORMAL);
      double x = day_left (m, self->num_days, index) + DAY_HEADER_RIGHT_MARGIN;
      double y = m->header_height - DAY_HEADER_BOTTOM_MARGIN;
      /* today's in the today colour; Saturday and Sunday in theirs (measured #FAFAFA), the rest #B2FFFFFF */
      guint32 color = day == today ? CAL_WEEK_TODAY
                      : weekday == 0 || weekday == 6 ? CAL_WEEKEND_LABEL : CAL_DAY_LABEL;

      draw_text (snapshot, number_layout, x, y, color);
      draw_text (snapshot, name_layout, x, y - DATE_HEADER_FONT_SIZE, color);
    }
}

static void
draw_hours (CalDayView *self, GtkSnapshot *snapshot, const Metrics *m)
{
  int first_hour = (int) ((self->view_start_y + self->cell_height + m->px - 1e-6) / m->pitch);
  double first_hour_offset = first_hour * m->pitch - self->view_start_y;
  double ascent = pango_layout_get_baseline (self->hour_layouts[0]) / (double) PANGO_SCALE;
  int hour = MAX (first_hour, 0);

  /* drawHours: from the first whole hour; in the day view without an all-day area, that one's label is left out
   * while the top edge would cut it */
  if (first_hour_offset < ascent / 2 && m->all_day_height == 0 && self->num_days == 1)
    hour++;
  for (; hour < 24; hour++)
    draw_text (snapshot, self->hour_layouts[hour], HOURS_LEFT_MARGIN, hour * m->pitch + ascent / 2 - m->px,
               CAL_HOUR_LABEL);
}

static void
draw_page (CalDayView *self, GtkSnapshot *snapshot, int first_day)
{
  Metrics m;
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC), today_index;
  double now_y;

  measure_page (self, first_day, &m);
  today_index = today - first_day;
  {
    gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    int minute = cal_minute_of_day (now);

    now_y = (minute / 60) * m.pitch + (minute % 60) * self->cell_height / 60 + m.px;
  }

  /* the view's own background: the past (day_activity's background under the grid) */
  fill (snapshot, CAL_PAST_BG, 0, 0, m.width, m.height);
  /* the hours column, its whole height */
  fill (snapshot, CAL_HOUR_BG, 0, 0, m.hours_width, m.height);

  /* --- the scrolled grid */
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, m.first_cell, m.width, m.grid_height));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, m.first_cell - self->view_start_y));

  /* drawBgColors: later than now is the future colour */
  if (today_index >= 0 && today_index < self->num_days)
    {
      double left = self->num_days == 1 ? m.hours_width : day_left (&m, self->num_days, today_index) + m.px;
      double right = self->num_days == 1 ? m.width : day_left (&m, self->num_days, today_index + 1);

      fill (snapshot, CAL_FUTURE_BG, left, now_y, right - left, 24 * m.pitch - now_y + m.px);
      if (today_index + 1 < self->num_days)
        fill (snapshot, CAL_FUTURE_BG, day_left (&m, self->num_days, today_index + 1) + m.px, 0,
              m.width - day_left (&m, self->num_days, today_index + 1) - m.px, 24 * m.pitch + m.px);
    }
  else if (today_index < 0)
    fill (snapshot, CAL_FUTURE_BG, m.hours_width + m.px, 0, m.width - m.hours_width - m.px, 24 * m.pitch + m.px);

  /* drawGridBackground: a line at every hour, one between days */
  for (int hour = 0; hour <= 24; hour++)
    fill (snapshot, CAL_GRID_LINE, m.hours_width, hour * m.pitch, m.width - m.hours_width, m.px);
  for (int day = 0; day < self->num_days; day++)
    fill (snapshot, CAL_GRID_LINE, day_left (&m, self->num_days, day), 0, m.px, 24 * m.pitch + m.px);

  /* drawSelectedRect */
  if (self->has_selection && !self->selection_all_day
      && self->selection_day >= first_day && self->selection_day < first_day + self->num_days)
    {
      int index = self->selection_day - first_day;
      double left = day_left (&m, self->num_days, index) + m.px;
      double right = day_left (&m, self->num_days, index + 1) + m.px - m.px;
      double top = self->selection_hour * m.pitch + m.px;

      fill (snapshot, CAL_GRID_SELECTED, left, top, right - left, self->cell_height);
      if (self->num_days > 1)
        {
          double width = right - left, length = MIN (MIN (self->cell_height, width) - 2 * NEW_EVENT_MARGIN,
                                                     NEW_EVENT_MAX_LENGTH);
          double mid_x = left + width / 2, mid_y = top + self->cell_height / 2;

          fill (snapshot, CAL_NEW_EVENT_HINT, mid_x - length / 2, mid_y - NEW_EVENT_WIDTH / 2, length, NEW_EVENT_WIDTH);
          fill (snapshot, CAL_NEW_EVENT_HINT, mid_x - NEW_EVENT_WIDTH / 2, mid_y - length / 2, NEW_EVENT_WIDTH, length);
        }
      else
        {
          g_autoptr(PangoLayout) hint = make_layout (self, _("+ New event"), NEW_EVENT_HINT_FONT_SIZE,
                                                     PANGO_WEIGHT_BOLD);

          draw_text (snapshot, hint, left + EVENT_TEXT_HORIZONTAL_MARGIN,
                     top + pango_layout_get_baseline (hint) / (double) PANGO_SCALE + EVENT_TEXT_VERTICAL_MARGIN,
                     CAL_NEW_EVENT_HINT);
        }
    }

  draw_history_line (self, snapshot, &m, first_day);
  draw_timed_events (self, snapshot, &m, first_day);
  if (today_index >= 0 && today_index < self->num_days)
    draw_now_line (self, snapshot, &m, today_index, now_y);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);

  /* --- the hour labels: they scroll with the grid and pass under the header only (measured: a label straddles
   * the line under the all-day area) */
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, m.header_height, m.hours_width, m.height));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, m.first_cell - self->view_start_y));
  draw_hours (self, snapshot, &m);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);

  /* --- drawAfterScroll: the header, the all-day area, the line above the grid */
  fill (snapshot, CAL_HOUR_BG, 0, 0, m.width, m.header_height);
  if (m.max_all_day > 0)
    {
      double stop = m.header_height + m.all_day_height + ALLDAY_TOP_MARGIN;

      fill (snapshot, CAL_GRID_LINE, m.hours_width, m.header_height, m.width - m.hours_width, m.px);
      for (int day = 0; day <= self->num_days; day++)
        fill (snapshot, CAL_GRID_LINE, day_left (&m, self->num_days, day), m.header_height, m.px,
              stop - m.header_height);
      gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, m.header_height, m.width, stop - m.header_height));
      draw_all_day_events (self, snapshot, &m, first_day);
      gtk_snapshot_pop (snapshot);
      /* drawUpperLeftCorner: the expand chevron while not all fit */
      if (m.max_all_day > m.max_unexpanded_count)
        {
          GdkRGBA color = cal_argb (CAL_EXPAND_ICON);
          double x = MAX ((m.hours_width - EXPAND_ICON_SIZE) / 2, EVENT_TEXT_HORIZONTAL_MARGIN);

          neo_snapshot_icon (snapshot, GTK_WIDGET (self),
                             self->show_all_all_day ? "cal-collapse-symbolic" : "cal-expand-symbolic", x,
                             m.first_cell - EXPAND_ALL_DAY_BOTTOM_MARGIN - EXPAND_ICON_SIZE, EXPAND_ICON_SIZE,
                             &color);
        }
    }
  /* drawScrollLine */
  fill (snapshot, CAL_GRID_LINE, m.hours_width, m.first_cell - m.px, m.width - m.hours_width, m.px);
  if (self->num_days > 1)
    draw_header (self, snapshot, &m, first_day, today);
}

static void
cal_day_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalDayView *self = CAL_DAY_VIEW (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);

  /* day_activity.xml: the FrameLayout around the ViewSwitcher has paddingTop 1dip on day_past_background_color, a
   * 1 dp strip above the header (measured: #2E2D32 at y 308…310, the view from 311). It is calendar.css's 1 px
   * padding on cal-day-view, painted here. */
  fill (snapshot, CAL_PAST_BG, 0, -1, width, 1);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, width, height));
  /* a horizontal drag: this period moved aside, the next or previous one beside it (DayView.onDraw's nextView) */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-self->view_start_x, 0));
  draw_page (self, snapshot, self->first_day);
  gtk_snapshot_restore (snapshot);
  if (self->view_start_x != 0)
    {
      int side = self->view_start_x > 0 ? 1 : -1;

      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (side * width - self->view_start_x, 0));
      draw_page (self, snapshot, self->first_day + side * self->num_days);
      gtk_snapshot_restore (snapshot);
    }
  gtk_snapshot_pop (snapshot);
}

/* --- scrolling */

static void
clamp_scroll (CalDayView *self)
{
  Metrics m;

  measure_page (self, self->first_day, &m);
  self->view_start_y = CLAMP (self->view_start_y, 0, m.max_view_start_y);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalDayView *self = CAL_DAY_VIEW (widget);
  double density = neo_device_scale (widget);
  gboolean running = spongy_fling_update (&self->fling, gdk_frame_clock_get_frame_time (clock) / 1000.0);

  /* the fling runs in device pixels, as OverScroller */
  self->view_start_y = self->fling.position / density;
  clamp_scroll (self);
  gtk_widget_queue_draw (widget);
  if (!running)
    {
      self->tick = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
stop_fling (CalDayView *self)
{
  if (self->tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
  self->tick = 0;
}

static void
start_fling (CalDayView *self, double velocity)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
  double density = neo_device_scale (GTK_WIDGET (self));
  Metrics m;

  if (!clock)
    return;
  measure_page (self, self->first_day, &m);
  spongy_fling_start (&self->fling, self->view_start_y * density, velocity * density, 0,
                      m.max_view_start_y * density, gdk_frame_clock_get_frame_time (clock) / 1000.0);
  if (!self->tick)
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
}

void
cal_day_view_scroll_to_hour (CalDayView *self, int hour)
{
  Metrics m;
  int num_hours, first_hour;

  if (gtk_widget_get_height (GTK_WIDGET (self)) <= 0)
    {
      /* not laid out yet: on the first allocation */
      self->scrolled_once = FALSE;
      self->initial_hour = hour;
      return;
    }
  measure_page (self, self->first_day, &m);
  num_hours = (int) (m.grid_height / m.pitch);
  first_hour = hour - num_hours / 5;
  if (first_hour < 0)
    first_hour = 0;
  else if (first_hour + num_hours > 24)
    first_hour = 24 - num_hours;
  self->view_start_y = first_hour * m.pitch;
  clamp_scroll (self);
  self->scrolled_once = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- paging */

static void
on_page_anim (NeoAnim *anim, double value, gpointer data)
{
  CalDayView *self = data;

  self->view_start_x = value;
  if (value == self->page_to && !neo_anim_is_running (anim))
    {
      double width = gtk_widget_get_width (GTK_WIDGET (self));

      self->view_start_x = 0;
      if (self->page_to != 0)
        {
          int delta = self->page_to > 0 ? 1 : -1;

          self->first_day += delta * self->num_days;
          clamp_scroll (self);
          g_signal_emit (self, signals[PAGE], 0, delta);
        }
      (void) width;
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* switchViews: the rest of the way to the next period (or back) in 6 × the time the release speed would take,
 * along (t − 1)^5 + 1 (DayView.ScrollInterpolator, calculateDuration) */
static void
settle_page (CalDayView *self, double velocity)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), target, distance, duration;
  double density = neo_device_scale (GTK_WIDGET (self));

  if (fabs (self->view_start_x) > width / 7 || fabs (velocity) > SWITCH_MIN_VELOCITY / density * 2)
    target = (self->view_start_x != 0 ? (self->view_start_x > 0 ? 1 : -1) : (velocity < 0 ? 1 : -1)) * width;
  else
    target = 0;
  distance = fabs (target - self->view_start_x) * density;
  velocity = MAX (fabs (velocity) * density, SWITCH_MIN_VELOCITY);
  duration = 6 * round (1000 * distance / velocity);
  self->page_from = self->view_start_x;
  self->page_to = target;
  if (duration < 16)
    {
      on_page_anim (self->page_anim, target, self);
      return;
    }
  neo_anim_start (self->page_anim, self->view_start_x, target, (guint) MIN (duration, 1000), 0,
                  SPONGY_DECELERATE_QUINT);
}

/* --- touch */

static double
event_time_ms (GtkGesture *gesture)
{
  return gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (gesture));
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, CalDayView *self)
{
  if (self->touch == TOUCH_PINCH)
    return;
  stop_fling (self);
  if (neo_anim_is_running (self->page_anim))
    neo_anim_finish (self->page_anim);
  self->touch = TOUCH_DOWN;
  self->down_x = x;
  self->down_y = y;
  self->start_view_y = self->view_start_y;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), x, y);
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, CalDayView *self)
{
  spongy_velocity_add (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), self->down_x + dx, self->down_y + dy);
  if (self->touch == TOUCH_DOWN)
    {
      if (fabs (dx) < TOUCH_SLOP && fabs (dy) < TOUCH_SLOP)
        return;
      /* DayView.onScroll: the first move decides, sideways pages, else the hours scroll */
      self->touch = fabs (dx) > fabs (dy) ? TOUCH_HSCROLL : TOUCH_VSCROLL;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      self->has_selection = FALSE;
    }
  if (self->touch == TOUCH_VSCROLL)
    {
      self->view_start_y = self->start_view_y - dy;
      clamp_scroll (self);
    }
  else if (self->touch == TOUCH_HSCROLL)
    self->view_start_x = -dx;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, CalDayView *self)
{
  float vx = 0, vy = 0;

  spongy_velocity_get (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), &vx, &vy);
  if (self->touch == TOUCH_VSCROLL && fabs (vy) > 50)
    start_fling (self, -vy);
  else if (self->touch == TOUCH_HSCROLL)
    settle_page (self, vx);
  if (self->touch != TOUCH_PINCH)
    self->touch = TOUCH_NONE;
}

static void
on_zoom_begin (GtkGesture *gesture, GdkEventSequence *sequence, CalDayView *self)
{
  double x, y;
  Metrics m;

  stop_fling (self);
  self->touch = TOUCH_PINCH;
  self->pinch_cell_height = self->cell_height;
  gtk_gesture_get_bounding_box_center (gesture, &x, &y);
  measure_page (self, self->first_day, &m);
  /* the hour under the fingers stays under them */
  self->pinch_focus = (y - m.first_cell + self->view_start_y) / m.pitch;
}

static void
on_zoom_changed (GtkGestureZoom *gesture, double scale, CalDayView *self)
{
  double x, y;
  Metrics m;

  measure_page (self, self->first_day, &m);
  /* the shortest: the 24 hours just fill the grid; the tallest MAX_CELL_HEIGHT */
  self->cell_height = CLAMP (self->pinch_cell_height * scale, m.grid_height / 24 - m.px, MAX_CELL_HEIGHT);
  gtk_gesture_get_bounding_box_center (GTK_GESTURE (gesture), &x, &y);
  measure_page (self, self->first_day, &m);
  self->view_start_y = self->pinch_focus * m.pitch - (y - m.first_cell);
  clamp_scroll (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_zoom_end (GtkGesture *gesture, GdkEventSequence *sequence, CalDayView *self)
{
  self->touch = TOUCH_NONE;
  g_signal_emit (self, signals[CELL_HEIGHT_CHANGED], 0, self->cell_height);
}

static CalEvent *
event_at (CalDayView *self, const Metrics *m, double x, double y)
{
  if (y >= m->first_cell)
    {
      double grid_y = y - m->first_cell + self->view_start_y;

      /* staggered: the highest column first, the one drawn on top */
      for (int column = max_column (self); column >= 0; column--)
        for (int index = 0; index < self->num_days; index++)
          for (guint i = 0; i < self->timed->len; i++)
            {
              CalEvent *event = self->timed->pdata[i];
              graphene_rect_t r;

              if ((!self->staggered || event->column == column)
                  && event_rect (self, m, event, self->first_day + index, index, &r)
                  && graphene_rect_contains_point (&r, &GRAPHENE_POINT_INIT (x, grid_y)))
                return event;
            }
      return NULL;
    }
  if (y >= m->header_height && m->max_all_day > 0)
    {
      double height = m->max_all_day > m->max_unexpanded_count ? m->min_unexpanded
                      : m->all_day_height / MAX (m->max_all_day, 1);
      int row = (int) ((y - m->header_height - ALLDAY_TOP_MARGIN) / MIN (height, MAX_HEIGHT_OF_ONE_ALLDAY_EVENT));
      int day = self->first_day + (int) ((x - m->hours_width) / ((m->width - m->hours_width) / self->num_days));

      for (guint i = 0; i < self->all_day->len; i++)
        {
          CalEvent *event = self->all_day->pdata[i];

          if (event->column == row && cal_event_first_day (event) <= day && cal_event_last_day (event) >= day)
            return event;
        }
    }
  return NULL;
}

static gboolean
on_click_timeout (gpointer data)
{
  CalDayView *self = data;
  g_autoptr(CalEvent) event = g_steal_pointer (&self->clicked);

  self->click_timeout = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[EVENT_ACTIVATED], 0, event);
  return G_SOURCE_REMOVE;
}

static void
on_expand_anim (NeoAnim *anim, double value, gpointer data)
{
  CalDayView *self = data;

  self->animate_all_day_height = neo_anim_is_running (anim) ? value : 0;
  clamp_scroll (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* doSingleTapUp */
static void
on_released (GtkGestureClick *gesture, int n_press, double x, double y, CalDayView *self)
{
  Metrics m;
  CalEvent *event;
  int index, day, hour;

  if (self->touch == TOUCH_VSCROLL || self->touch == TOUCH_HSCROLL || self->touch == TOUCH_PINCH)
    return;
  measure_page (self, self->first_day, &m);
  index = (int) ((x - m.hours_width) / ((m.width - m.hours_width) / self->num_days));
  index = CLAMP (index, 0, self->num_days - 1);
  day = self->first_day + index;

  /* the week's header: that day in the Day view */
  if (self->num_days > 1 && y < m.header_height)
    {
      if (x >= m.hours_width)
        g_signal_emit (self, signals[DAY_ACTIVATED], 0, day);
      return;
    }
  /* the expand chevron: the all-day area grows to show them all, or back */
  if (x < m.hours_width && y >= m.header_height && y < m.first_cell && m.max_all_day > m.max_unexpanded_count)
    {
      double from = m.all_day_height, to;

      self->show_all_all_day = !self->show_all_all_day;
      measure_page (self, self->first_day, &m);
      to = m.all_day_height;
      neo_anim_start (self->expand_anim, from, to, ANIMATION_DURATION, 0, SPONGY_LINEAR);
      return;
    }
  if (x < m.hours_width)
    return;

  event = event_at (self, &m, x, y);
  if (event)
    {
      /* the clicked colour, then the info */
      self->has_selection = FALSE;
      g_clear_handle_id (&self->click_timeout, g_source_remove);
      g_clear_pointer (&self->clicked, cal_event_unref);
      self->clicked = cal_event_ref (event);
      self->click_timeout = g_timeout_add (CLICK_DISPLAY_MS, on_click_timeout, self);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (y < m.first_cell)
    hour = -1;          /* the all-day area */
  else
    hour = CLAMP ((int) ((y - m.first_cell + self->view_start_y) / m.pitch), 0, 23);
  /* the selected slot tapped again: a new event there (Etar's "+") */
  if (self->has_selection && self->selection_day == day
      && (hour < 0 ? self->selection_all_day : !self->selection_all_day && self->selection_hour == hour))
    {
      self->has_selection = FALSE;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      g_signal_emit (self, signals[CREATE_EVENT], 0, cal_day_start (day) + MAX (hour, 0) * 3600, hour < 0);
      return;
    }
  self->has_selection = TRUE;
  self->selection_day = day;
  self->selection_all_day = hour < 0;
  self->selection_hour = MAX (hour, 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, CalDayView *self)
{
  /* a wheel: an hour a notch; a touchpad: as it moves */
  GdkScrollUnit unit = gtk_event_controller_scroll_get_unit (controller);
  Metrics m;

  measure_page (self, self->first_day, &m);
  self->view_start_y += unit == GDK_SCROLL_UNIT_WHEEL ? dy * m.pitch : dy;
  clamp_scroll (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return TRUE;
}

/* --- now moves: the line, the backgrounds, today */

static gboolean
on_minute (gpointer data)
{
  CalDayView *self = data;

  gtk_widget_queue_draw (GTK_WIDGET (self));
  return G_SOURCE_CONTINUE;
}

/* --- GtkWidget */

static void
cal_day_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalDayView *self = CAL_DAY_VIEW (widget);

  if (!self->scrolled_once)
    cal_day_view_scroll_to_hour (self, self->initial_hour);
  else
    clamp_scroll (self);
}

static void
cal_day_view_css_changed (GtkWidget *widget, GtkCssStyleChange *change)
{
  GTK_WIDGET_CLASS (cal_day_view_parent_class)->css_changed (widget, change);
  update_hour_labels (CAL_DAY_VIEW (widget));
}

static void
cal_day_view_dispose (GObject *object)
{
  CalDayView *self = CAL_DAY_VIEW (object);

  stop_fling (self);
  g_clear_handle_id (&self->click_timeout, g_source_remove);
  g_clear_handle_id (&self->minute_timer, g_source_remove);
  g_clear_pointer (&self->clicked, cal_event_unref);
  g_clear_pointer (&self->expand_anim, neo_anim_free);
  g_clear_pointer (&self->page_anim, neo_anim_free);
  for (int i = 0; i < 24; i++)
    g_clear_object (&self->hour_layouts[i]);
  g_clear_pointer (&self->events, g_ptr_array_unref);
  g_clear_pointer (&self->timed, g_ptr_array_unref);
  g_clear_pointer (&self->all_day, g_ptr_array_unref);
  g_clear_pointer (&self->history_text, g_free);
  G_OBJECT_CLASS (cal_day_view_parent_class)->dispose (object);
}

static void
cal_day_view_class_init (CalDayViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_day_view_dispose;
  widget_class->snapshot = cal_day_view_snapshot;
  widget_class->size_allocate = cal_day_view_size_allocate;
  widget_class->css_changed = cal_day_view_css_changed;
  gtk_widget_class_set_css_name (widget_class, "cal-day-view");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_GRID);

  signals[EVENT_ACTIVATED] = g_signal_new ("event-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[CREATE_EVENT] = g_signal_new ("create-event", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_BOOLEAN);
  signals[DAY_ACTIVATED] = g_signal_new ("day-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[PAGE] = g_signal_new ("page", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                G_TYPE_NONE, 1, G_TYPE_INT);
  signals[CELL_HEIGHT_CHANGED] = g_signal_new ("cell-height-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                               NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static void
cal_day_view_init (CalDayView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *zoom = gtk_gesture_zoom_new ();
  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL
                                                                | GTK_EVENT_CONTROLLER_SCROLL_KINETIC);

  self->num_days = 7;
  self->cell_height = DEFAULT_CELL_HEIGHT;
  self->events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  self->timed = g_ptr_array_new ();
  self->all_day = g_ptr_array_new ();
  {
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();

    self->initial_hour = g_date_time_get_hour (now);
  }
  self->expand_anim = neo_anim_new (GTK_WIDGET (self), on_expand_anim, self);
  self->page_anim = neo_anim_new (GTK_WIDGET (self), on_page_anim, self);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);

  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (zoom, "begin", G_CALLBACK (on_zoom_begin), self);
  g_signal_connect (zoom, "scale-changed", G_CALLBACK (on_zoom_changed), self);
  g_signal_connect (zoom, "end", G_CALLBACK (on_zoom_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (zoom));
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);

  self->minute_timer = g_timeout_add_seconds (30, on_minute, self);
}

GtkWidget *
cal_day_view_new (int num_days)
{
  CalDayView *self = g_object_new (CAL_TYPE_DAY_VIEW, NULL);

  self->num_days = num_days;
  self->first_day = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
  update_hour_labels (self);
  return GTK_WIDGET (self);
}

int
cal_day_view_get_num_days (CalDayView *self)
{
  return self->num_days;
}

void
cal_day_view_set_num_days (CalDayView *self, int num_days)
{
  num_days = CLAMP (num_days, 1, 7);
  if (num_days == self->num_days)
    return;
  self->num_days = num_days;
  gtk_widget_queue_resize (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_day_view_set_first_day (CalDayView *self, int first_day)
{
  if (neo_anim_is_running (self->page_anim))
    neo_anim_stop (self->page_anim);
  self->view_start_x = 0;
  self->first_day = first_day;
  self->has_selection = FALSE;
  clamp_scroll (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

int
cal_day_view_get_first_day (CalDayView *self)
{
  return self->first_day;
}

/* Event.computePositions: an event shorter than the minimum height takes that height's time in the layout */
static void
layout_events (CalDayView *self)
{
  Metrics m;
  gint64 min_seconds;

  measure_page (self, self->first_day, &m);
  min_seconds = (gint64) (m.min_unexpanded * 60 / (self->cell_height / 60));
  if (self->staggered)
    cal_layout_events_staggered (self->timed, min_seconds);
  else
    cal_layout_events (self->timed, FALSE, min_seconds);
  cal_layout_events (self->all_day, TRUE, 0);
}

void
cal_day_view_set_staggered (CalDayView *self, gboolean staggered)
{
  if (self->staggered == !!staggered)
    return;
  self->staggered = !!staggered;
  layout_events (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_day_view_set_history (CalDayView *self, gint64 history, const char *text)
{
  self->history = history;
  g_free (self->history_text);
  self->history_text = g_strdup (text);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_day_view_set_events (CalDayView *self, GPtrArray *events)
{
  g_ptr_array_set_size (self->events, 0);
  g_ptr_array_set_size (self->timed, 0);
  g_ptr_array_set_size (self->all_day, 0);
  for (guint i = 0; events && i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];

      g_ptr_array_add (self->events, cal_event_ref (event));
      g_ptr_array_add (cal_event_draws_as_all_day (event) ? self->all_day : self->timed, event);
    }
  layout_events (self);
  clamp_scroll (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_day_view_set_cell_height (CalDayView *self, double cell_height)
{
  self->cell_height = CLAMP (cell_height, 10, MAX_CELL_HEIGHT);
  clamp_scroll (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
cal_day_view_get_selection (CalDayView *self, gint64 *start, gboolean *all_day)
{
  if (!self->has_selection)
    return FALSE;
  *start = cal_day_start (self->selection_day) + (self->selection_all_day ? 0 : self->selection_hour * 3600);
  *all_day = self->selection_all_day;
  return TRUE;
}

void
cal_day_view_clear_selection (CalDayView *self)
{
  self->has_selection = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
