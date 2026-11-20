/* CalMonthView: Etar's Month view (month-view.h; spec/calendar.md §4). It draws itself in device pixels, as
 * MonthWeekEventsView draws its canvas. */
#include "month-view.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>
#include <neo.h>
#include <spongy.h>

/* --- dimensions, dp (E/month/MonthWeekEventsView.java:76-108, L/full_month_header.xml, V/integers.xml) */
#define HEADER_ROW 16.0                 /* full_month_header: 16 dp tall … */
#define HEADER_MARGIN 10.0              /* … with 10 dp under it */
#define HEADER_TOP_PADDING 1.0          /* full_month_by_week's paddingTop */
#define DAY_NAME_SIZE 11.0              /* day_label_text_size */
#define DAY_NAME_END_MARGIN 6.0
#define WEEKS_SHOWN 6
#define MONTH_NUMBER_SIZE 20.0          /* text_size_month_number */
#define MONTH_NUMBER_TOP 3.0            /* mTopPaddingMonthNumber */
#define MONTH_NUMBER_SIDE 4.0           /* month_day_number_margin */
#define SEPARATOR_WIDTH 1.0             /* mDaySeparatorInnerWidth */
#define EVENT_TITLE_SIZE 10.0           /* text_size_event_title */
#define EXTRAS_SIZE 12.0                /* mTextSizeEvent: the "+N" */
#define EVENT_Y_OFFSET 2.0              /* mEventYOffsetPortrait */
#define EVENT_SQUARE_WIDTH 3.0
#define EVENT_SQUARE_HEIGHT 10.0
#define EVENT_LINE_PADDING 2.0
#define EVENT_RIGHT_PADDING 4.0
#define EVENT_SQUARE_BORDER 0.0         /* mEventSquareBorder */
#define EVENT_BOTTOM_PADDING 1.0        /* mEventBottomPadding: under "+N" and the last event */
#define CLICKED_ALPHA (128 / 255.0)
#define WEEK_NUMBER_SIZE 9.0            /* mTextSizeWeekNum */
#define WEEK_NUMBER_TOP 4.0             /* mTopPaddingWeekNumber */
#define WEEK_NUMBER_SIDE 12.0           /* mSidePaddingWeekNumber: the number's right edge */

/* MonthListView: below 1500 dp/s the list flings freely; above, it snaps to the start of a month, one month below
 * 2000 dp/s and one more per 500 dp/s above that, in 1000 ms */
#define MIN_VELOCITY_FOR_FLING 1500.0
#define MULTIPLE_MONTH_VELOCITY_THRESHOLD 2000.0
#define FLING_VELOCITY_DIVIDER 500.0
#define FLING_TIME 1000
#define TOUCH_SLOP 8.0

/* Display options (ViewDetailsPreferences.Preferences) */
#define MAX_LINES 7
typedef struct {
  int time;                     /* TimeVisibility: 0 none, 1 start, 2 start and end, 3 start and duration, 4 below */
  gboolean location;
  int max_lines;                /* 1 … 7 */
} Details;

struct _CalMonthView {
  GtkWidget parent;
  int week_start;
  int base;                     /* a day that starts a week: week w starts on base + 7 w */
  double top_week;              /* the scroll: the week at the top of the list, fractional */
  int focus_month;              /* year · 12 + month − 1 */
  GPtrArray *events;
  int clicked_day;              /* the pressed day, drawn in the clicked colour; 0 for none */
  gboolean show_week_number;
  Details details;
  gboolean h24;                 /* the times in 24-hour form */
  GHashTable *weeks;            /* first day → Week: the formatted weeks (the size they were formatted for below) */
  int cache_height, cache_x_width, cache_scale;

  gboolean dragging;
  double down_y, start_top;
  SpongyVelocity velocity;
  SpongyFling fling;
  guint tick;
  NeoAnim *snap;
  double snap_from, snap_to;
};

G_DEFINE_FINAL_TYPE (CalMonthView, cal_month_view, GTK_TYPE_WIDGET)

enum { DAY_ACTIVATED, MOVED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* --- days and months */

static int
month_of (int day)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  return g_date_get_year (&date) * 12 + g_date_get_month (&date) - 1;
}

static int
month_start (int month)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, 1, month % 12 + 1, month / 12);
  return g_date_get_julian (&date);
}

static int
week_of (CalMonthView *self, int day)
{
  int d = day - self->base;

  return d >= 0 ? d / 7 : -((-d + 6) / 7);
}

static int
week_first_day (CalMonthView *self, int week)
{
  return self->base + 7 * week;
}

/* --- geometry */

static int
scale_of (CalMonthView *self)
{
  return neo_device_scale (GTK_WIDGET (self));
}

/* the top of the list of weeks: the names, their margin and the 1 px line under them */
static double
list_top (CalMonthView *self)
{
  return HEADER_TOP_PADDING + HEADER_ROW + HEADER_MARGIN + 1.0 / scale_of (self);
}

/* each week is a sixth of the list, in whole device pixels (MonthByWeekAdapter: list height / 6) */
static double
row_height (CalMonthView *self)
{
  int scale = scale_of (self);
  double list = gtk_widget_get_height (GTK_WIDGET (self)) - list_top (self);

  return MAX (1.0, floor (list * scale / WEEKS_SHOWN) / scale);
}

/* the x where day column @i starts (computeDayLeftPosition: i · width / 7, in whole device pixels) */
static double
day_left (CalMonthView *self, int i)
{
  int scale = scale_of (self);

  return floor (i * gtk_widget_get_width (GTK_WIDGET (self)) * scale / 7.0) / scale;
}

/* the month in focus: the month of the middle of the week in the middle of the list */
static void
update_focus (CalMonthView *self)
{
  int week = (int) floor (self->top_week + WEEKS_SHOWN / 2.0);
  int month = month_of (week_first_day (self, week) + 3);

  if (month == self->focus_month)
    return;
  self->focus_month = month;
  g_signal_emit (self, signals[MOVED], 0);
}

static void
set_top_week (CalMonthView *self, double top_week)
{
  self->top_week = top_week;
  update_focus (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- drawing */

static void
draw_header (CalMonthView *self, GtkSnapshot *snapshot, int scale, double width)
{
  /* the 1 px View under the names (L/full_month_by_week.xml): @color/calendar_grid_line_inner_vertical_color from the
   * layout, so the plain #CCCCCC in every look (DynamicTheme only reaches colours read in code); the land-cal-month
   * still has it */
  GdkRGBA line = cal_argb (0xffcccccc);
  double baseline;

  /* the weekday names (TextViews: Regular + 300 = 700), uppercase, at the end of each column less 6 dp */
  cal_text_line (DAY_NAME_SIZE, scale, &baseline, NULL);
  for (int i = 0; i < 7; i++)
    {
      int weekday = (self->week_start + i) % 7;
      g_autofree char *name = NULL, *upper = NULL;
      g_autoptr(PangoLayout) layout = NULL;
      /* Sunday and Saturday: month_sunday_dark / month_saturday_dark */
      GdkRGBA color = cal_argb (weekday == 0 || weekday == 6 ? CAL_MONTH_WEEKEND_NAMES : CAL_MONTH_DAY_NAMES);
      /* any week's day with that weekday: the base is a week's first day */
      int day = self->base + i;

      {
        g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (cal_day_start (day));

        name = g_date_time_format (date, "%a");
      }
      upper = g_utf8_strup (name, -1);
      layout = cal_text_layout (GTK_WIDGET (self), upper, DAY_NAME_SIZE, 700);
      cal_text_draw (snapshot, layout, day_left (self, i + 1) - DAY_NAME_END_MARGIN, HEADER_TOP_PADDING + baseline, 1,
                     &color);
    }
  cal_fill_rect (snapshot, scale, 0, list_top (self) - 1.0 / scale, width, 1.0 / scale, &line);
}

/* --- events: MonthWeekEventsView.drawEvents (E/month/MonthWeekEventsView.java:700-1735). WeekEventFormatter gives
 * each day its events and each event its format (lines, days spanned); DayEventSorter orders a day's events so that
 * a bar keeps its line over its days; DayEventFormatter fits a day's events in its box (fewer lines per event, then
 * fewer events and "+N"); then each FormattedEvent draws itself. All of it in whole device pixels, as on the canvas
 * (Px), with the class and method names in the comments. Not ported: drawDNA, the busy-time strands Etar draws in
 * place of events where show_details_in_month is false (it is true in every configuration Etar ships), and the
 * TalkBack hover text (onHoverEvent; the view is a GTK_ACCESSIBLE_ROLE_GRID without per-day children). */

/* ViewDetailsPreferences.TimeVisibility */
enum { TIME_NONE, TIME_START, TIME_START_END, TIME_START_DURATION, TIME_BELOW };

/* the box's dimensions in device pixels: the dp values times the density, truncated as Etar's static ints */
typedef struct {
  int scale;
  int height;                   /* the week's (mHeight) */
  int x_width;                  /* a day's box, mWidth / mNumDays (DayBoxBoundaries.mXWidth) */
  int top;                      /* the first event's top: mEventYOffsetPortrait + mMonthNumHeight + top padding */
  int event_height, event_ascent;       /* mEventHeight, mEventAscentHeight (the title's paint) */
  int extras_height, extras_descent;    /* mExtrasHeight, mExtrasDescent (the times' and "+N"'s paint) */
  int border_space;             /* mBorderSpace: mEventSquareBorder (0) + 1 */
  int line_padding, square_width, square_height, right_padding, bottom_padding;
} Px;

static void
px_init (Px *px, int scale, double width, double height)
{
  double ascent, line;

  px->scale = scale;
  px->height = (int) lround (height * scale);
  px->x_width = (int) lround (width * scale) / 7;
  cal_paint_line (MONTH_NUMBER_SIZE, scale, NULL, &line);
  px->top = (int) (EVENT_Y_OFFSET * scale) + (int) lround (line * scale) + (int) (MONTH_NUMBER_TOP * scale);
  cal_paint_line (EVENT_TITLE_SIZE, scale, &ascent, &line);
  px->event_ascent = (int) lround (ascent * scale);
  px->event_height = (int) lround (line * scale);
  cal_paint_line (EXTRAS_SIZE, scale, NULL, &line);
  px->extras_height = (int) lround (line * scale);
  px->extras_descent = (int) floor (EXTRAS_SIZE * scale * 500 / 2048 + 0.5);
  px->border_space = (int) (EVENT_SQUARE_BORDER * scale) + 1;
  px->line_padding = (int) (EVENT_LINE_PADDING * scale);
  px->square_width = (int) (EVENT_SQUARE_WIDTH * scale);
  px->square_height = (int) (EVENT_SQUARE_HEIGHT * scale);
  px->right_padding = (int) (EVENT_RIGHT_PADDING * scale);
  px->bottom_padding = (int) (EVENT_BOTTOM_PADDING * scale);
}

/* DayBoxBoundaries: where the next event of the day goes */
typedef struct {
  int x, y, y_offset, right_edge, x_width;
} Box;

static void
box_init (Box *box, const Px *px)
{
  *box = (Box) { 1, px->top, 0, -1, px->x_width };
}

static void
box_next_day (Box *box)
{
  box->x += box->x_width;
  box->right_edge += box->x_width;
  box->y_offset = 0;
}

static int
box_y (const Box *box)
{
  return box->y + box->y_offset;
}

static int
box_right (const Box *box, int span)
{
  return span * box->x_width + box->right_edge;
}

/* the BoundariesSetters: a bar (AllDay: a 1 px border inside the fill), a timed event's square (Regular), and a
 * timed event squeezed to one line beside a bar (FixedHeightRegular: the bar's border, the square's text offset) */
typedef enum { BOUNDS_ALL_DAY, BOUNDS_REGULAR, BOUNDS_FIXED } Bounds;

static int
bounds_border (const Px *px, Bounds bounds)
{
  return bounds == BOUNDS_REGULAR ? 0 : px->border_space;
}

static int
bounds_x_padding (const Px *px, Bounds bounds)
{
  if (bounds == BOUNDS_ALL_DAY)
    return 0;
  return px->square_width + px->right_padding - bounds_border (px, bounds);
}

/* EventFormat: the event's lines and, per day of the week, how many days the bar drawn from that day covers */
typedef struct {
  int lines;
  int span[7];
  int n_days;                   /* 7; 0 for the placeholder's */
  int y_index;                  /* its line in the day, -1 not yet placed */
  gboolean partially_hidden;
} Format;

static void
format_init (Format *format, int day, int n_days)
{
  memset (format, 0, sizeof *format);
  format->n_days = n_days;
  if (day >= 0 && day < n_days)
    format->span[day] = 1;
  format->lines = 1;
  format->y_index = -1;
}

static int
format_total_span (const Format *format)
{
  int span = 0;

  for (int i = 0; i < format->n_days; i++)
    span += format->span[i];
  return span;
}

static int
format_span (const Format *format, int day)
{
  return day < format->n_days ? format->span[day] : 0;
}

/* hide: a one-day event goes; a bar is cut at @day and goes on after it */
static void
format_hide (Format *format, int day)
{
  if (format->n_days <= day)
    return;
  if (format_total_span (format) > 1)
    {
      int split = day, span;

      format->partially_hidden = TRUE;
      while (split >= 0 && format->span[split] <= 0)
        split--;
      if (split < 0)
        return;
      span = format->span[split];
      format->span[split] = day - split;
      format->span[day] = 0;
      if (format->n_days > day + 1)
        format->span[day + 1] = span - 1 - format->span[split];
    }
  else
    {
      format->lines = 0;
      format->partially_hidden = FALSE;
    }
}

static void
format_cap_lines (Format *format, int cap)
{
  format->lines = MIN (format->lines, cap);
}

static void
format_extend (Format *format, int day)
{
  for (int index = MIN (day, format->n_days - 1); index >= 0; index--)
    if (format->span[index] > 0)
      {
        format->span[index]++;
        break;
      }
}

/* a FormattedEvent (or, with no event, the NullFormattedEvent that holds a line free under a bar) */
typedef struct {
  CalEvent *event;
  Format *format;               /* shared by a bar's days */
  Bounds bounds;
  GArray *starts;               /* mTextLayout: the first byte of each line of its text; NULL until laid out */
} Formatted;

static void
formatted_free (Formatted *formatted)
{
  g_clear_pointer (&formatted->starts, g_array_unref);
  g_free (formatted);
}

static gboolean
formatted_bordered (const Px *px, const Formatted *formatted)
{
  return bounds_border (px, formatted->bounds) > 0;
}

/* isTimeInNextLine: "Start and end time below" on an event without a border */
static gboolean
time_in_next_line (const Px *px, const Formatted *formatted, const Details *details)
{
  return formatted->event && details->time == TIME_BELOW && !formatted_bordered (px, formatted);
}

/* FormattedEventBase.getHeight */
static int
formatted_height (const Px *px, const Formatted *formatted, const Details *details)
{
  int border = bounds_border (px, formatted->bounds);

  return formatted->format->lines * px->event_height + 2 * border + px->line_padding
         + (time_in_next_line (px, formatted, details) ? px->extras_height : 0);
}

/* the week, formatted: its days' events in drawing order, and what each day kept of the details */
typedef struct {
  GPtrArray *days[7];
  Details details[7];           /* the time hidden on a day it does not fit ("below" only) */
  int hidden[7];                /* "+N" */
  GPtrArray *owned;             /* the Formatted */
  GPtrArray *formats;
  Format placeholder_format;    /* DayEventSorter.virtualFormat */
  Formatted placeholder;        /* DayEventSorter.mVirtualEvent */
} Week;

static void
week_free (Week *week)
{
  for (int d = 0; d < 7; d++)
    g_clear_pointer (&week->days[d], g_ptr_array_unref);
  g_ptr_array_unref (week->owned);
  g_ptr_array_unref (week->formats);
  g_free (week);
}

/* --- the text: getBaseText, getFormattedTime */

static gboolean
on_the_hour (gint64 time)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (time);

  return g_date_time_get_minute (local) == 0 && g_date_time_get_second (local) == 0;
}

/* FORMAT_SHOW_TIME | FORMAT_ABBREV_ALL: "09:00"; on the 12-hour clock "9 AM" on the hour, else "9:30 AM" */
static char *
abbreviated_time (CalMonthView *self, gint64 time)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (time);

  if (self->h24)
    return g_date_time_format (local, "%H:%M");
  return g_date_time_format (local, on_the_hour (time) ? "%-I %p" : "%-I:%M %p");
}

static char *
formatted_time (CalMonthView *self, const CalEvent *event, const Details *details)
{
  GString *time = g_string_new (NULL);

  if (details->time != TIME_NONE)
    {
      g_autofree char *start = abbreviated_time (self, event->start);

      g_string_append (time, start);
    }
  if (details->time == TIME_START_END || details->time == TIME_BELOW)
    {
      g_autofree char *end = abbreviated_time (self, event->end);

      g_string_append (time, " – ");
      if (cal_event_first_day (event) != cal_event_last_day (event))
        {
          /* FORMAT_SHOW_DATE | FORMAT_ABBREV_ALL: "Nov 9", with the year when it is not this one */
          g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (event->end);
          g_autoptr(GDateTime) now = g_date_time_new_now_local ();
          g_autofree char *date = cal_format_skeleton (local, g_date_time_get_year (local) == g_date_time_get_year (now)
                                                              ? "MMMd" : "yMMMd");

          g_string_append_printf (time, "%s, ", date);
        }
      g_string_append (time, end);
    }
  if (details->time == TIME_START_DURATION)
    {
      /* DateUtils.formatElapsedTime: "1:30:00", or "45:00" under an hour */
      gint64 seconds = MAX (0, event->end - event->start);

      if (time->len)
        g_string_append_c (time, ' ');
      if (seconds >= 3600)
        g_string_append_printf (time, "[%d:%02d:%02d]", (int) (seconds / 3600), (int) (seconds / 60 % 60),
                                (int) (seconds % 60));
      else
        g_string_append_printf (time, "[%02d:%02d]", (int) (seconds / 60), (int) (seconds % 60));
    }
  return g_string_free (time, FALSE);
}

/* getBaseText: the time inline unless it goes below (never an all-day event's), the title, "@ location" on a line of
 * its own */
static char *
base_text (CalMonthView *self, const Px *px, const Formatted *formatted, const Details *details)
{
  const CalEvent *event = formatted->event;
  GString *text = g_string_new (NULL);

  if (details->time != TIME_NONE && !time_in_next_line (px, formatted, details) && !event->all_day)
    {
      g_autofree char *time = formatted_time (self, event, details);

      g_string_append_printf (text, "%s ", time);
    }
  g_string_append (text, event->title && *event->title ? event->title : _("(No title)"));
  if (details->location && event->location && *event->location)
    g_string_append_printf (text, "\n@ %s", event->location);
  return g_string_free (text, FALSE);
}

/* getAvailableSpaceForText: from the text's left (after the square, or the bar's border) to the box's right less the
 * border, over @span days */
static int
text_width (const Px *px, Bounds bounds, int span)
{
  int border = bounds_border (px, bounds);

  return span * px->x_width - 2 - 2 * border - bounds_x_padding (px, bounds);
}

/* preFormatText: a DynamicLayout of the text in the title's paint, broken into lines at the width */
static void
pre_format (CalMonthView *self, const Px *px, Formatted *formatted, const Details *details, int span)
{
  g_autofree char *text = base_text (self, px, formatted, details);
  g_autoptr(PangoLayout) layout = cal_text_layout (GTK_WIDGET (self), text, EVENT_TITLE_SIZE, 700);
  GSList *lines;

  pango_layout_set_width (layout, (int) (MAX (1, text_width (px, formatted->bounds, span)) * PANGO_SCALE
                                         / (double) px->scale));
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  if (formatted->starts)
    g_array_set_size (formatted->starts, 0);
  else
    formatted->starts = g_array_new (FALSE, FALSE, sizeof (int));
  for (lines = pango_layout_get_lines_readonly (layout); lines; lines = lines->next)
    g_array_append_val (formatted->starts, ((PangoLayoutLine *) lines->data)->start_index);
}

/* FormattedEvent.initialPreFormatText: a one-day event takes as many lines as its text needs, up to the maximum */
static void
initial_pre_format (CalMonthView *self, const Px *px, Formatted *formatted, const Details *details)
{
  int span;

  if (formatted->starts)
    return;
  span = format_total_span (formatted->format);
  pre_format (self, px, formatted, details, span);
  if (span == 1 && formatted->format->lines != 0)
    formatted->format->lines = MIN ((int) formatted->starts->len, details->max_lines);
}

/* --- WeekEventFormatter */

static Formatted *
week_add (Week *week, CalEvent *event, Format *format, Bounds bounds)
{
  Formatted *formatted = g_new0 (Formatted, 1);

  formatted->event = event;
  formatted->format = format;
  formatted->bounds = bounds;
  g_ptr_array_add (week->owned, formatted);
  return formatted;
}

/* getFormatByEvent */
static Format *
format_on_day (Week *week, CalEvent *event, int day)
{
  if (day < 0)
    return NULL;
  for (guint i = 0; i < week->days[day]->len; i++)
    if (((Formatted *) week->days[day]->pdata[i])->event == event)
      return ((Formatted *) week->days[day]->pdata[i])->format;
  return NULL;
}

/* prepareFormattedEventDay: a bar continues the previous day's format; a timed event over midnight shows on its
 * first day of the week only */
static void
prepare_day (CalMonthView *self, Week *week, int first_day, int d)
{
  int day = first_day + d;

  week->days[d] = g_ptr_array_new ();
  for (guint i = 0; i < self->events->len; i++)
    {
      CalEvent *event = self->events->pdata[i];
      gboolean bar = cal_event_draws_as_all_day (event);
      Format *last;

      if (cal_event_first_day (event) > day || cal_event_last_day (event) < day)
        continue;
      last = format_on_day (week, event, d - 1);
      if (last && bar)
        {
          format_extend (last, d);
          g_ptr_array_add (week->days[d], week_add (week, event, last, BOUNDS_ALL_DAY));
        }
      else if (!last)
        {
          Format *format = g_new (Format, 1);

          format_init (format, d, 7);
          g_ptr_array_add (week->formats, format);
          g_ptr_array_add (week->days[d], week_add (week, event, format, bar ? BOUNDS_ALL_DAY : BOUNDS_REGULAR));
        }
    }
}

/* --- DayEventSorter: a bar keeps its line (y-index) from the day before; the others fill the free lines, the
 * longest bars first; a line left free under a bar holds a placeholder */

static void
sorted_add_remaining (GPtrArray *remaining, Formatted *formatted)
{
  int span = format_total_span (formatted->format);

  if (span > 1)
    for (guint i = 0; i < remaining->len; i++)
      if (format_total_span (((Formatted *) remaining->pdata[i])->format) < span)
        {
          g_ptr_array_insert (remaining, (int) i, formatted);
          return;
        }
  g_ptr_array_add (remaining, formatted);
}

/* getNextIndex: one line at a time above the last placed bar, then by the lines of the event placed */
static int
next_index (Formatted **indexed, int index, int min_items)
{
  if (index < min_items)
    return index + 1;
  return index + MAX (1, indexed[index]->format->lines);
}

static GPtrArray *
sort_day (Week *week, const Px *px, GPtrArray *events)
{
  GPtrArray *sorted = g_ptr_array_new ();
  g_autoptr(GPtrArray) remaining = g_ptr_array_new ();
  g_autofree Formatted **indexed = NULL;
  int min_items = -1, height = 0, size, index = 0;

  if (events->len == 0)
    return sorted;
  for (guint i = 0; i < events->len; i++)
    {
      Formatted *formatted = events->pdata[i];

      height += formatted->format->lines;
      min_items = MAX (min_items, formatted->format->y_index);
    }
  size = MAX (min_items + 1, height);
  indexed = g_new0 (Formatted *, size);
  for (guint i = 0; i < events->len; i++)
    {
      Formatted *formatted = events->pdata[i];

      if (formatted->format->y_index != -1)
        indexed[formatted->format->y_index] = formatted;
      else
        sorted_add_remaining (remaining, formatted);
    }
  for (guint i = 0; i < remaining->len; i++)
    {
      Formatted *formatted = remaining->pdata[i];

      if (formatted->format->lines <= 0)
        continue;
      while (index < size)
        {
          if (!indexed[index])
            {
              formatted->format->y_index = index;
              if (index < min_items)
                {
                  /* beside a bar's line: one line, with the bar's border */
                  format_cap_lines (formatted->format, 1);
                  if (!formatted_bordered (px, formatted))
                    formatted->bounds = BOUNDS_FIXED;
                }
              indexed[index] = formatted;
              index = next_index (indexed, index, min_items);
              break;
            }
          index = next_index (indexed, index, min_items);
        }
    }
  for (int i = index; i < min_items; i++)
    if (!indexed[i])
      indexed[i] = &week->placeholder;
  for (int i = 0; i < size; i++)
    if (indexed[i])
      g_ptr_array_add (sorted, indexed[i]);
  return sorted;
}

/* --- DayEventFormatter: if a day's events are taller than its box, they lose lines (the tallest first) while one line
 * each still fits; else they are cut to one line each and the last ones go, "+N" taking their place */

typedef struct {
  GPtrArray *events;
  int day;
  Details details;
  int full_day_events, visible, max_lines;
  GPtrArray *by_height[MAX_LINES + 1];  /* the events by their lines; [0] the hidden ones */
  int n_heights;
} DayFormatter;

static void
day_init (DayFormatter *f, const Px *px, GPtrArray *events, int day, const Details *details)
{
  memset (f, 0, sizeof *f);
  f->events = events;
  f->day = day;
  f->details = *details;
  f->max_lines = details->max_lines;
  f->n_heights = f->max_lines + 1;
  for (int i = 0; i < f->n_heights; i++)
    f->by_height[i] = g_ptr_array_new ();
  for (guint i = 0; i < events->len; i++)
    {
      Formatted *formatted = events->pdata[i];
      int lines = CLAMP (formatted->format->lines, 0, f->max_lines);

      if (lines > 0)
        {
          f->visible++;
          if (formatted_bordered (px, formatted))
            f->full_day_events++;
        }
      g_ptr_array_add (f->by_height[lines], formatted);
    }
}

static void
day_clear (DayFormatter *f)
{
  for (int i = 0; i < f->n_heights; i++)
    g_ptr_array_unref (f->by_height[i]);
}

static gboolean
more_lines_shown (const DayFormatter *f)
{
  return f->by_height[0]->len > 0;
}

/* getOverheadHeight: "+N", the bars' borders, the gaps between events */
static int
overhead_height (const DayFormatter *f, const Px *px)
{
  return (more_lines_shown (f) ? px->extras_height : 0) + f->full_day_events * px->border_space * 2
         + (f->visible - 1) * px->line_padding;
}

static int
time_ranges_height (const DayFormatter *f, const Px *px)
{
  return f->details.time == TIME_BELOW ? px->extras_height * (f->visible - f->full_day_events) : 0;
}

static int
total_event_lines (const DayFormatter *f)
{
  int lines = 0;

  for (int i = 1; i < f->n_heights; i++)
    lines += i * (int) f->by_height[i]->len;
  return lines;
}

static int
events_height (const DayFormatter *f, const Px *px)
{
  return overhead_height (f, px) + total_event_lines (f) * px->event_height + time_ranges_height (f, px);
}

static int
max_number_of_lines (const DayFormatter *f, const Px *px, int available)
{
  return (available - overhead_height (f, px) - time_ranges_height (f, px)) / px->event_height;
}

static int
highest_events (const DayFormatter *f)
{
  return f->by_height[f->max_lines]->len;
}

/* reduceNumberOfLines: the tallest events lose a line */
static void
reduce_number_of_lines (DayFormatter *f)
{
  int index = f->max_lines;

  if (f->max_lines <= 0)
    return;
  f->max_lines--;
  for (guint i = 0; i < f->by_height[index]->len; i++)
    {
      Formatted *formatted = f->by_height[index]->pdata[i];

      format_cap_lines (formatted->format, f->max_lines);
      g_ptr_array_add (f->by_height[index - 1], formatted);
    }
  g_ptr_array_set_size (f->by_height[index], 0);
}

/* reduceHeightOfEvents: the last @count of the tallest lose a line */
static void
reduce_height_of_events (DayFormatter *f, int count)
{
  GPtrArray *highest = f->by_height[f->max_lines];
  int keep = CLAMP ((int) highest->len - count, 0, (int) highest->len), cap = f->max_lines - 1;

  if (cap < 0)
    return;
  for (guint i = keep; i < highest->len; i++)
    {
      Formatted *formatted = highest->pdata[i];

      format_cap_lines (formatted->format, cap);
      g_ptr_array_add (f->by_height[cap], formatted);
    }
  g_ptr_array_set_size (highest, keep);
}

/* fitAllItemsOnScrean */
static void
fit_all (DayFormatter *f, const Px *px, int available)
{
  int max = max_number_of_lines (f, px, available), lines = total_event_lines (f);

  while (max < lines - highest_events (f) && f->max_lines > 0)
    {
      lines -= highest_events (f);
      reduce_number_of_lines (f);
    }
  reduce_height_of_events (f, lines - max);
}

/* reduceNumberOfEventsToFit: one line each, then the last events hidden until the rest and "+N" fit. As in Etar a
 * placeholder line (no event) counts as one of the "+N" when it has to go too. */
static void
reduce_to_fit (DayFormatter *f, const Px *px, int available)
{
  int height;

  for (int i = 2; i <= f->max_lines; i++)
    {
      for (guint j = 0; j < f->by_height[i]->len; j++)
        {
          Formatted *formatted = f->by_height[i]->pdata[j];

          format_cap_lines (formatted->format, 1);
          g_ptr_array_add (f->by_height[1], formatted);
        }
      g_ptr_array_set_size (f->by_height[i], 0);
    }
  f->max_lines = 1;
  height = events_height (f, px);
  if (!more_lines_shown (f))
    height += px->extras_height;
  for (int i = (int) f->events->len - 1; i >= 0 && height > available; i--)
    {
      Formatted *formatted = f->events->pdata[i];

      if (formatted->format->lines == 0)
        continue;
      height -= formatted_height (px, formatted, &f->details);
      format_hide (formatted->format, f->day);
      f->visible--;
      g_ptr_array_add (f->by_height[0], formatted);
    }
}

/* formatDay */
static void
format_day (DayFormatter *f, const Px *px, int available)
{
  /* hideTimeRangeIfNeeded: the times below go first */
  if (f->details.time == TIME_BELOW && max_number_of_lines (f, px, available) < f->visible)
    f->details.time = TIME_NONE;
  if (events_height (f, px) <= available)
    return;
  if (overhead_height (f, px) + f->visible * px->event_height <= available)
    fit_all (f, px, available);
  else
    reduce_to_fit (f, px, available);
}

/* prepareFormattedEvents */
static Week *
week_format (CalMonthView *self, const Px *px, int first_day)
{
  Week *week = g_new0 (Week, 1);
  int available = px->height - px->top - px->bottom_padding;     /* DayBoxBoundaries.getAvailableYSpace */

  week->owned = g_ptr_array_new_with_free_func ((GDestroyNotify) formatted_free);
  week->formats = g_ptr_array_new_with_free_func (g_free);
  format_init (&week->placeholder_format, 0, 0);
  week->placeholder.format = &week->placeholder_format;
  week->placeholder.bounds = BOUNDS_FIXED;
  for (int d = 0; d < 7; d++)
    prepare_day (self, week, first_day, d);
  for (int d = 0; d < 7; d++)
    for (guint i = 0; i < week->days[d]->len; i++)
      initial_pre_format (self, px, week->days[d]->pdata[i], &self->details);
  for (int d = 0; d < 7; d++)
    {
      GPtrArray *sorted = sort_day (week, px, week->days[d]);

      g_ptr_array_unref (week->days[d]);
      week->days[d] = sorted;
    }
  for (int d = 0; d < 7; d++)
    {
      DayFormatter f;

      day_init (&f, px, week->days[d], d, &self->details);
      format_day (&f, px, available);
      week->details[d] = f.details;
      week->hidden[d] = f.by_height[0]->len;
      day_clear (&f);
    }
  return week;
}

/* --- FormattedEvent.draw */

static void
fill_px (GtkSnapshot *snapshot, const Px *px, int left, int top, int right, int bottom, guint32 argb)
{
  GdkRGBA color = cal_argb (argb);
  double s = px->scale;

  cal_fill_rect (snapshot, px->scale, left / s, top / s, (right - left) / s, (bottom - top) / s, &color);
}

/* drawEventRectangle: filled (Style.FILL_AND_STROKE at stroke width 0 is a fill), or for an invitation not yet
 * answered a hairline frame (Style.STROKE at width 0: the pixels of both edges) */
static void
draw_rectangle (GtkSnapshot *snapshot, const Px *px, const Box *box, const Formatted *formatted, int day)
{
  const CalEvent *event = formatted->event;
  int span = format_span (formatted->format, day), lines = formatted->format->lines;
  int left, right, top, bottom;
  guint32 rgb = cal_event_display_color (event);
  guint32 color = 0xff000000 | (cal_event_faded (event) ? cal_declined_color (rgb) : rgb);

  left = box->x;
  if (formatted->bounds == BOUNDS_ALL_DAY)
    {
      /* AllDayBoundariesSetter.setRectangle */
      right = box_right (box, span);
      top = box_y (box);
      bottom = box_y (box) + px->event_height * lines + px->border_space * 2;
    }
  else
    {
      /* RegularBoundariesSetter.setRectangle: the square beside the first line, as tall as the lines */
      right = box->x + px->square_width;
      top = box_y (box) + px->event_ascent - px->square_height;
      bottom = box_y (box) + px->event_ascent + (lines - 1) * px->event_height;
    }
  if (event->self_status == CAL_STATUS_INVITED)
    {
      fill_px (snapshot, px, left, top, right + 1, top + 1, color);
      fill_px (snapshot, px, left, bottom, right + 1, bottom + 1, color);
      fill_px (snapshot, px, left, top, left + 1, bottom + 1, color);
      fill_px (snapshot, px, right, top, right + 1, bottom + 1, color);
    }
  else
    fill_px (snapshot, px, left, top, right, bottom, color);
}

/* getTextPaint */
static guint32
text_color (const Formatted *formatted)
{
  const CalEvent *event = formatted->event;
  guint32 rgb = cal_event_display_color (event);
  gboolean bar = cal_event_draws_as_all_day (event);

  if (event->self_status != CAL_STATUS_INVITED && bar)
    /* on the bar: white, or black or white by the bar's brightness with real event colours */
    return cal_adaptive_text_color (0xffffffff, rgb);
  if (cal_event_faded (event))
    return CAL_AGENDA_DECLINED_TITLE;
  if (bar)
    /* an invitation's frame: the text in the frame's colour */
    return 0xff000000 | rgb;
  return CAL_MONTH_EVENT_TEXT;
}

static void
draw_line (CalMonthView *self, GtkSnapshot *snapshot, const Px *px, const char *text, double size, int weight,
           int x, int baseline, int ellipsize_width, gboolean strike, guint32 argb)
{
  g_autoptr(PangoLayout) layout = cal_text_layout (GTK_WIDGET (self), text, size, weight);
  GdkRGBA color = cal_argb (argb);

  if (ellipsize_width >= 0)
    cal_text_ellipsize (layout, ellipsize_width / (double) px->scale);
  if (strike)
    cal_text_strike (layout);
  cal_text_draw (snapshot, layout, x / (double) px->scale, baseline / (double) px->scale, 0, &color);
}

/* a line as canvas.drawText draws it: a line break inside it takes no room */
static char *
one_line (const char *text, gsize length)
{
  GString *line = g_string_new_len (text, (gssize) length);

  for (gsize i = 0; i < line->len;)
    if (line->str[i] == '\n' || line->str[i] == '\r')
      g_string_erase (line, (gssize) i, 1);
    else
      i++;
  return g_string_free (line, FALSE);
}

/* drawText: each line of the layout, the last one with the rest of the text, ellipsized */
static void
draw_text (CalMonthView *self, GtkSnapshot *snapshot, const Px *px, Box *box, Formatted *formatted,
           const Details *details, int day)
{
  g_autofree char *text = base_text (self, px, formatted, details);
  int lines = formatted->format->lines, span = format_span (formatted->format, day);
  int border = bounds_border (px, formatted->bounds);
  int x = box->x + border + bounds_x_padding (px, formatted->bounds);
  int width = box_right (box, span) - border - x;
  gsize length = strlen (text);
  guint32 color = text_color (formatted);

  if (formatted->format->partially_hidden)
    pre_format (self, px, formatted, details, span);
  for (int i = 0; i < lines; i++)
    {
      gsize start = (guint) i < formatted->starts->len ? (gsize) g_array_index (formatted->starts, int, i) : length;
      gsize end = (guint) i + 1 < formatted->starts->len ? (gsize) g_array_index (formatted->starts, int, i + 1)
                                                         : length;
      g_autofree char *line = NULL;

      start = MIN (start, length);
      if (i == lines - 1)
        end = length;
      line = one_line (text + start, MAX (start, MIN (end, length)) - start);
      draw_line (self, snapshot, px, line, EVENT_TITLE_SIZE, 700, x, box_y (box) + px->event_ascent,
                 i == lines - 1 ? width : -1, formatted->event->cancelled, color);
      box->y_offset += px->event_height;
    }
}

/* drawTimes: under the title in the times' paint (12 sp Regular), cut to one day's width */
static void
draw_times (CalMonthView *self, GtkSnapshot *snapshot, const Px *px, Box *box, const Formatted *formatted,
            const Details *details)
{
  g_autofree char *time = formatted_time (self, formatted->event, details);
  int border = bounds_border (px, formatted->bounds);
  int x = box->x + border + bounds_x_padding (px, formatted->bounds);

  draw_line (self, snapshot, px, time, EXTRAS_SIZE, 400, x, box_y (box) + px->event_ascent,
             box_right (box, 1) - border - x, FALSE,
             cal_event_faded (formatted->event) ? CAL_AGENDA_DECLINED_WHEN : CAL_MONTH_EVENT_EXTRA);
  box->y_offset += px->extras_height;
}

/* DayEventFormatter.drawDay: each event drawn on the day its bar starts, passed over (its height kept) on the others;
 * "+N" at the bottom */
static void
draw_day (CalMonthView *self, GtkSnapshot *snapshot, const Px *px, Box *box, Week *week, int day)
{
  const Details *details = &week->details[day];

  for (guint i = 0; i < week->days[day]->len; i++)
    {
      Formatted *formatted = week->days[day]->pdata[i];
      int border = bounds_border (px, formatted->bounds);

      if (formatted->format->lines <= 0)
        continue;
      if (format_span (formatted->format, day) <= 0 || !formatted->event)
        {
          /* skip */
          box->y_offset += border + formatted->format->lines * px->event_height
                           + (time_in_next_line (px, formatted, details) ? px->extras_height : 0)
                           + px->line_padding + border;
          continue;
        }
      draw_rectangle (snapshot, px, box, formatted, day);
      box->y_offset += border;
      draw_text (self, snapshot, px, box, formatted, details, day);
      if (time_in_next_line (px, formatted, details))
        draw_times (self, snapshot, px, box, formatted, details);
      box->y_offset += px->line_padding + border;
    }
  if (week->hidden[day] > 0)
    {
      /* drawMoreEvents: "+N", 12 sp fake bold, in month_event_extra_color */
      int n = week->hidden[day];
      g_autofree char *text = g_strdup_printf (g_dngettext (NULL, "+%d", "+%d", n), n);

      draw_line (self, snapshot, px, text, EXTRAS_SIZE, 700, box->x,
                 px->height - (px->extras_descent + px->bottom_padding), -1, FALSE, CAL_MONTH_EVENT_EXTRA);
    }
}

/* the formatted weeks, kept until the events, the details or the size change */
static Week *
week_for (CalMonthView *self, const Px *px, int first_day)
{
  Week *week;

  if (px->height != self->cache_height || px->x_width != self->cache_x_width || px->scale != self->cache_scale
      || g_hash_table_size (self->weeks) > 4 * WEEKS_SHOWN)
    {
      g_hash_table_remove_all (self->weeks);
      self->cache_height = px->height;
      self->cache_x_width = px->x_width;
      self->cache_scale = px->scale;
    }
  week = g_hash_table_lookup (self->weeks, GINT_TO_POINTER (first_day));
  if (!week)
    {
      week = week_format (self, px, first_day);
      g_hash_table_insert (self->weeks, GINT_TO_POINTER (first_day), week);
    }
  return week;
}

/* drawEvents */
static void
draw_events (CalMonthView *self, GtkSnapshot *snapshot, int scale, int first_day, double height)
{
  Px px;
  Box box;
  Week *week;

  px_init (&px, scale, gtk_widget_get_width (GTK_WIDGET (self)), height);
  week = week_for (self, &px, first_day);
  box_init (&box, &px);
  for (int d = 0; d < 7; d++)
    {
      draw_day (self, snapshot, &px, &box, week, d);
      box_next_day (&box);
    }
}

static void
draw_week (CalMonthView *self, GtkSnapshot *snapshot, int scale, int first_day, double y0, double height,
           int today)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), px = 1.0 / scale;
  GdkRGBA line = cal_argb (CAL_MONTH_GRID_LINE);
  double number_ascent;

  cal_paint_line (MONTH_NUMBER_SIZE, scale, &number_ascent, NULL);

  gtk_snapshot_save (snapshot);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, (float) y0, (float) width, (float) height));
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, (float) y0));

  /* drawBackground: the month in focus #212121; other months alternate black and the list's #141414 by the parity
   * of their month (mOddMonth); today month_today_bgcolor over it, or colorPrimary at alpha 72 when the Theme
   * setting is exactly "light" (not "system" in a light look) */
  for (int i = 0; i < 7; i++)
    {
      int day = first_day + i, month = month_of (day);
      double x = day_left (self, i), w = day_left (self, i + 1) - x;

      if (month == self->focus_month)
        {
          GdkRGBA bg = cal_argb (CAL_MONTH_FOCUS_BG);

          cal_fill_rect (snapshot, scale, x, SEPARATOR_WIDTH, w, height - SEPARATOR_WIDTH, &bg);
        }
      else if ((month % 12) % 2 == 0)
        {
          GdkRGBA bg = cal_argb (CAL_MONTH_OTHER_BG);

          cal_fill_rect (snapshot, scale, x, SEPARATOR_WIDTH, w, height - SEPARATOR_WIDTH, &bg);
        }
      if (day == today)
        {
          GdkRGBA bg = cal_argb (cal_theme_light ? (CAL_PRIMARY & 0xffffff) | (72u << 24) : CAL_MONTH_TODAY_BG);

          cal_fill_rect (snapshot, scale, x, SEPARATOR_WIDTH, w, height - SEPARATOR_WIDTH, &bg);
        }
    }

  /* drawWeekNums: the day numbers, 20 sp Regular (bold for today), right-aligned 4 dp in from the column's end */
  for (int i = 0; i < 7; i++)
    {
      int day = first_day + i;
      GDate date;
      g_autofree char *number = NULL;
      g_autoptr(PangoLayout) layout = NULL;
      GdkRGBA color = cal_argb (month_of (day) == self->focus_month || day == today ? CAL_MONTH_NUMBER
                                                                                    : CAL_MONTH_OTHER_NUMBER);

      g_date_clear (&date, 1);
      g_date_set_julian (&date, day);
      number = g_strdup_printf ("%d", g_date_get_day (&date));
      layout = cal_text_layout (GTK_WIDGET (self), number, MONTH_NUMBER_SIZE, day == today ? 700 : 400);
      cal_text_draw (snapshot, layout, day_left (self, i + 1) - MONTH_NUMBER_SIDE, number_ascent + MONTH_NUMBER_TOP, 1,
                     &color);
    }

  /* drawWeekNums with mShowWeekNum: the week's number, 9 sp, right-aligned 12 dp in, over the first day (the days
   * keep their columns); SimpleWeekView numbers a week by its Monday */
  if (self->show_week_number)
    {
      double ascent, line_height;
      int monday = first_day + (1 - self->week_start + 7) % 7;
      g_autoptr(GDateTime) time = g_date_time_new_from_unix_local (cal_day_start (monday) + 12 * 3600);
      g_autofree char *number = g_strdup_printf ("%d", g_date_time_get_week_of_year (time));
      g_autoptr(PangoLayout) layout = cal_text_layout (GTK_WIDGET (self), number, WEEK_NUMBER_SIZE, 400);
      GdkRGBA color = cal_argb (CAL_MONTH_WEEK_NUMBER);

      cal_paint_line (WEEK_NUMBER_SIZE, scale, &ascent, &line_height);
      cal_text_draw (snapshot, layout, WEEK_NUMBER_SIDE, ascent + WEEK_NUMBER_TOP, 1, &color);
    }

  /* drawDaySeparators: 1 dp lines, the horizontal one along the top, centred on y 0 and x of each column (the
   * first only half shows) */
  cal_fill_rect (snapshot, scale, 0, 0, width, ceil (SEPARATOR_WIDTH * scale / 2.0) * px, &line);
  for (int i = 0; i < 7; i++)
    {
      double x = day_left (self, i);
      double half = floor (SEPARATOR_WIDTH * scale / 2.0) * px;

      cal_fill_rect (snapshot, scale, MAX (0, x - half), 0, i == 0 ? ceil (SEPARATOR_WIDTH * scale / 2.0) * px
                                                                   : SEPARATOR_WIDTH, height, &line);
    }

  /* drawEvents */
  draw_events (self, snapshot, scale, first_day, height);

  /* drawClick: the tapped day in #33B5E5 at half alpha */
  if (self->clicked_day >= first_day && self->clicked_day < first_day + 7)
    {
      int i = self->clicked_day - first_day;
      GdkRGBA clicked = cal_argb (0xff33b5e5);

      clicked.alpha = CLICKED_ALPHA;
      cal_fill_rect (snapshot, scale, day_left (self, i), SEPARATOR_WIDTH, day_left (self, i + 1) - day_left (self, i),
                     height - SEPARATOR_WIDTH, &clicked);
    }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
cal_month_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalMonthView *self = CAL_MONTH_VIEW (widget);
  int scale = scale_of (self);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double top = list_top (self), row = row_height (self);
  GdkRGBA bg = cal_argb (CAL_FUTURE_BG);         /* month_bgcolor_dark: bg_dark */
  GdkRGBA surface = cal_argb (CAL_SURFACE);
  int first_week = (int) floor (self->top_week);
  double offset = (self->top_week - first_week) * row;
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);

  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) top));
  gtk_snapshot_append_color (snapshot, &bg, &GRAPHENE_RECT_INIT (0, (float) top, (float) width,
                                                                 (float) (height - top)));
  draw_header (self, snapshot, scale, width);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, (float) top, (float) width, (float) (height - top)));
  for (int w = 0; top - offset + w * row < height; w++)
    {
      /* each week on whole device pixels, as the list lays its children out */
      double y = round ((top - offset + w * row) * scale) / scale;

      draw_week (self, snapshot, scale, week_first_day (self, first_week + w), y, row, today);
    }
  gtk_snapshot_pop (snapshot);
}

/* --- scrolling */

static double
viscous_fluid (double x)
{
  /* Scroller.ViscousFluidInterpolator, the curve of AbsListView.smoothScrollBy */
  x *= 8;
  if (x < 1)
    return x - (1 - exp (-x));
  return 0.36787944117 + (1 - exp (1 - x)) * (1 - 0.36787944117);
}

static void
on_snap (NeoAnim *anim, double value, gpointer data)
{
  CalMonthView *self = data;
  double t = viscous_fluid (value) / viscous_fluid (1);

  set_top_week (self, self->snap_from + (self->snap_to - self->snap_from) * t);
}

static void
scroll_to_week (CalMonthView *self, double week, guint duration)
{
  if (!duration || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      neo_anim_stop (self->snap);
      set_top_week (self, week);
      return;
    }
  self->snap_from = self->top_week;
  self->snap_to = week;
  neo_anim_start (self->snap, 0, 1, duration, 0, SPONGY_LINEAR);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalMonthView *self = CAL_MONTH_VIEW (widget);
  double unit = row_height (self) * scale_of (self);
  gboolean running = spongy_fling_update (&self->fling, gdk_frame_clock_get_frame_time (clock) / 1000.0);

  set_top_week (self, self->fling.position / unit);
  if (!running)
    {
      self->tick = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
stop_motion (CalMonthView *self)
{
  if (self->tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
  self->tick = 0;
  neo_anim_stop (self->snap);
}

/* MonthListView.doFling: a fast fling goes to the first week of the next (or this, scrolling back) month, or
 * further the faster it is */
static void
fling (CalMonthView *self, double velocity)
{
  int scale = scale_of (self);
  double speed = velocity;      /* dp/s, positive toward later weeks */
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));

  if (fabs (speed) > MIN_VELOCITY_FOR_FLING)
    {
      int jump, upper_right = week_first_day (self, (int) floor (self->top_week)) + 6;
      /* Android's velocityY is negative toward later weeks */
      double v = -speed;

      if (fabs (v) < MULTIPLE_MONTH_VELOCITY_THRESHOLD)
        jump = v < 0 ? 1 : 0;
      else if (v < 0)
        jump = 1 - (int) ((v + MULTIPLE_MONTH_VELOCITY_THRESHOLD) / FLING_VELOCITY_DIVIDER);
      else
        jump = -(int) ((v - MULTIPLE_MONTH_VELOCITY_THRESHOLD) / FLING_VELOCITY_DIVIDER);
      scroll_to_week (self, week_of (self, month_start (month_of (upper_right) + jump)), FLING_TIME);
      return;
    }
  if (!clock || fabs (speed) < 50)
    return;
  {
    double unit = row_height (self) * scale;

    spongy_fling_start (&self->fling, (float) (self->top_week * unit), (float) (speed * scale), -INFINITY, INFINITY,
                        gdk_frame_clock_get_frame_time (clock) / 1000.0);
    if (!self->tick)
      self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
  }
}

static double
event_time_ms (GtkGesture *gesture)
{
  return gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (gesture));
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, CalMonthView *self)
{
  stop_motion (self);
  self->dragging = FALSE;
  self->down_y = y;
  self->start_top = self->top_week;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), x, y);
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, CalMonthView *self)
{
  spongy_velocity_add (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), dx, self->down_y + dy);
  if (!self->dragging)
    {
      if (fabs (dy) < TOUCH_SLOP)
        return;
      self->dragging = TRUE;
      self->clicked_day = 0;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  set_top_week (self, self->start_top - dy / row_height (self));
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, CalMonthView *self)
{
  float vx = 0, vy = 0;

  if (!self->dragging)
    return;
  self->dragging = FALSE;
  spongy_velocity_get (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), &vx, &vy);
  fling (self, -vy);
}

static int
day_at (CalMonthView *self, double x, double y)
{
  double top = list_top (self), row = row_height (self);
  int week, column = 0;

  if (y < top)
    return 0;
  week = (int) floor (self->top_week + (y - top) / row);
  while (column < 6 && x >= day_left (self, column + 1))
    column++;
  return week_first_day (self, week) + column;
}

static gboolean
day_has_events (CalMonthView *self, int day)
{
  for (guint i = 0; i < self->events->len; i++)
    {
      CalEvent *event = self->events->pdata[i];

      if (cal_event_first_day (event) <= day && day <= cal_event_last_day (event))
        return TRUE;
    }
  return FALSE;
}

static void
on_pressed (GtkGestureClick *gesture, int n, double x, double y, CalMonthView *self)
{
  self->clicked_day = day_at (self, x, y);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_released (GtkGestureClick *gesture, int n, double x, double y, CalMonthView *self)
{
  int day = day_at (self, x, y);

  self->clicked_day = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (day && !self->dragging)
    g_signal_emit (self, signals[DAY_ACTIVATED], 0, day, day_has_events (self, day));
}

static void
on_click_cancel (GtkGesture *gesture, GdkEventSequence *sequence, CalMonthView *self)
{
  self->clicked_day = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, CalMonthView *self)
{
  /* a wheel: a week per notch; a touchpad: as it moves */
  if (gtk_event_controller_scroll_get_unit (controller) == GDK_SCROLL_UNIT_WHEEL)
    set_top_week (self, round (self->top_week) + dy);
  else
    set_top_week (self, self->top_week + dy / row_height (self));
  return TRUE;
}

/* --- GtkWidget */

static void
cal_month_view_dispose (GObject *object)
{
  CalMonthView *self = CAL_MONTH_VIEW (object);

  stop_motion (self);
  g_clear_pointer (&self->snap, neo_anim_free);
  g_clear_pointer (&self->weeks, g_hash_table_unref);
  g_clear_pointer (&self->events, g_ptr_array_unref);
  G_OBJECT_CLASS (cal_month_view_parent_class)->dispose (object);
}

static void
cal_month_view_class_init (CalMonthViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_month_view_dispose;
  widget_class->snapshot = cal_month_view_snapshot;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_GRID);
  signals[DAY_ACTIVATED] = g_signal_new ("day-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_BOOLEAN);
  signals[MOVED] = g_signal_new ("moved", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}

static void
cal_month_view_init (CalMonthView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

  self->events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  self->weeks = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) week_free);
  /* a phone's Display options: 7 lines, no time, no location (show_time_in_month false) */
  self->details = (Details) { TIME_NONE, FALSE, MAX_LINES };
  self->h24 = cal_format_uses_24h ();
  self->snap = neo_anim_new (GTK_WIDGET (self), on_snap, self);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_click_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);
}

GtkWidget *
cal_month_view_new (void)
{
  CalMonthView *self = g_object_new (CAL_TYPE_MONTH_VIEW, NULL);

  cal_month_view_set_week_start (self, 0);
  cal_month_view_go_to (self, cal_day_number (g_get_real_time () / G_USEC_PER_SEC));
  return GTK_WIDGET (self);
}

void
cal_month_view_set_show_week_number (CalMonthView *self, gboolean show)
{
  self->show_week_number = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_month_view_set_week_start (CalMonthView *self, int week_start)
{
  int focus = self->focus_month ? month_start (self->focus_month) : 0;

  self->week_start = week_start;
  /* a week's first day near the start of GDate's days (day 0 is not one: GDate's Julian days begin at 1) */
  self->base = 8 - ((cal_day_of_week (8) - week_start + 7) % 7);
  if (focus)
    cal_month_view_go_to (self, focus);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_month_view_go_to (CalMonthView *self, int day)
{
  int month = month_of (day);

  stop_motion (self);
  /* LIST_TOP_OFFSET −1: the week of the 1st at the top */
  self->top_week = week_of (self, month_start (month));
  if (month != self->focus_month)
    {
      self->focus_month = month;
      g_signal_emit (self, signals[MOVED], 0);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

int
cal_month_view_get_focus_day (CalMonthView *self)
{
  return month_start (self->focus_month);
}

void
cal_month_view_get_range (CalMonthView *self, int *first, int *last)
{
  int start = week_first_day (self, week_of (self, month_start (self->focus_month)));

  /* the six weeks of the month and as many again above and below, so a scroll finds them loaded */
  *first = start - 7 * WEEKS_SHOWN;
  *last = start + 7 * 2 * WEEKS_SHOWN - 1;
}

void
cal_month_view_set_events (CalMonthView *self, GPtrArray *events)
{
  /* the formatted weeks point at the events */
  g_hash_table_remove_all (self->weeks);
  g_ptr_array_set_size (self->events, 0);
  for (guint i = 0; events && i < events->len; i++)
    g_ptr_array_add (self->events, cal_event_ref (events->pdata[i]));
  /* each day's events in the loader's order: begin ASC, end DESC, title ASC */
  g_ptr_array_sort (self->events, cal_event_compare);
  self->h24 = cal_format_uses_24h ();
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_month_view_set_details (CalMonthView *self, int max_lines, int time, gboolean location)
{
  Details details = { CLAMP (time, TIME_NONE, TIME_BELOW), !!location, CLAMP (max_lines, 1, MAX_LINES) };

  if (memcmp (&details, &self->details, sizeof details) == 0)
    return;
  self->details = details;
  g_hash_table_remove_all (self->weeks);
  self->h24 = cal_format_uses_24h ();
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
