/* MaterialDatePicker (cal-date-picker.h; spec/calendar.md §8.1). Sizes are the still's, in dp.
 *
 * The text input mode (SingleDateSelector.onCreateTextInputView, mtrl_picker_text_input_date.xml): the pencil
 * swaps the month and its days for an outlined "Date" field 16 dp under the header, 12 dp in from the sides, with
 * the locale's short date as its pattern (UtcDates.getDefaultTextInputFormat: "M/d/yy" → "MM/dd/yyyy") and that
 * pattern in the abbreviations as its placeholder ("mm/dd/yyyy", "tt.mm.jjjj"). Typing forward adds the pattern's
 * next separator (DateFormatTextWatcher.afterTextChanged); every change clears the choice (the header "Selected
 * date", OK off) until the text is as long as the pattern and parses strictly to a day from 1900 to 2100; a second
 * after typing stops, a text that does not parse shows "Invalid format." with the pattern and today as an example,
 * a day outside the range "Out of range: …", both in colorError under the field, with the error icon in it. The
 * calendar icon goes back to the days, at the month of the day typed. */
#include "cal-date-picker.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-model.h"
#include "cal-outlined-field.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <string.h>
#include <unicode/udat.h>
#include <unicode/udatpg.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>

#define CARD_WIDTH 328.0
#define CELL 40.0                       /* a day's cell; 4 dp between columns */
#define COLUMN_GAP 4.0
#define DAY_CIRCLE 36.0                 /* the selection and today's ring: the cell inset 2 dp */
#define DAY_TEXT_SIZE 16.0
#define GRID_ROWS 6
#define YEAR_FIRST 1900
#define YEAR_LAST 2100
#define SWIPE_SLOP 8.0
#define NO_DAY 0                        /* no day chosen (a day number is 1 or more) */
#define FIELD_HEIGHT 56.0               /* the M3 outlined text field */
#define FIELD_PADDING 16.0
#define FIELD_CORNER 4.0
#define FIELD_TEXT_SIZE 16.0            /* bodyLarge */
#define LABEL_SIZE 12.0                 /* bodySmall: the label on the outline */
#define LABEL_CUTOUT 4.0                /* mtrl_textinput_box_label_cutout_padding */
#define LABEL_MS 167                    /* CollapsingTextHelper's animation */
#define ERROR_ICON 24.0
#define ERROR_ICON_END 12.0
#define VALIDATION_MS 1000              /* DateFormatTextWatcher.VALIDATION_DELAY */
#define ERROR_ICON_PATH "M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-2h2v2zm0-4h-2V7h2v6z"

typedef struct {
  NeoDialog *dialog;
  CalDateChosen chosen;
  gpointer data;
  int selected;                 /* the chosen day, or NO_DAY */
  int month;                    /* the month shown: year · 12 + month − 1 */
  int week_start;
  GtkWidget *selection, *month_label, *stack, *grid, *years, *prev, *next;
  /* the text input mode */
  gboolean text_mode;
  GtkWidget *toggle, *nav, *input, *field, *error, *ok;
  char *pattern;                /* the input format ("MM/dd/yyyy") */
  char *hint;                   /* its placeholder ("mm/dd/yyyy") */
  guint validation;             /* the error a second after typing stops */
  char *pending_error;
  int last_length;
  gboolean setting_text;
} Picker;

/* --- months */

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
month_days (int month)
{
  return g_date_get_days_in_month (month % 12 + 1, month / 12);
}

/* --- the grid of days: the weekday initials, then the month's days */

#define CAL_TYPE_DAY_GRID (cal_day_grid_get_type ())
G_DECLARE_FINAL_TYPE (CalDayGrid, cal_day_grid, CAL, DAY_GRID, GtkWidget)

struct _CalDayGrid {
  GtkWidget parent;
  Picker *picker;
  double drag_x;
  gboolean swiping;
};

G_DEFINE_FINAL_TYPE (CalDayGrid, cal_day_grid, GTK_TYPE_WIDGET)

static void update (Picker *picker);

static double
column_x (int column)
{
  return column * (CELL + COLUMN_GAP);
}

static void
cal_day_grid_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalDayGrid *self = CAL_DAY_GRID (widget);
  Picker *picker = self->picker;
  int scale = neo_device_scale (widget), first = month_start (picker->month), n = month_days (picker->month);
  int offset = (cal_day_of_week (first) - picker->week_start + 7) % 7;
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
  GdkRGBA text = cal_argb (CAL_ON_SURFACE), primary = cal_argb (CAL_PRIMARY), on_primary = cal_argb (CAL_ON_PRIMARY);
  double baseline, height;

  cal_text_line (DAY_TEXT_SIZE, scale, &baseline, &height);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) self->drag_x, 0));
  /* the weekday initials ("S M T W T F S"), from the locale's first day */
  for (int i = 0; i < 7; i++)
    {
      g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (cal_day_start (first - offset + i));
      g_autofree char *name = g_date_time_format (date, "%a");
      g_autofree char *initial = g_utf8_substring (name, 0, 1);
      g_autofree char *upper = g_utf8_strup (initial, -1);
      g_autoptr(PangoLayout) layout = cal_text_layout (widget, upper, DAY_TEXT_SIZE, 700);

      cal_text_draw (snapshot, layout, column_x (i) + CELL / 2, (CELL - height) / 2 + baseline, 0.5, &text);
    }
  for (int d = 0; d < n; d++)
    {
      int cell = offset + d, row = cell / 7, column = cell % 7, day = first + d;
      double x = column_x (column), y = CELL * (row + 1);
      g_autofree char *number = g_strdup_printf ("%d", d + 1);
      g_autoptr(PangoLayout) layout = cal_text_layout (widget, number, DAY_TEXT_SIZE, 700);
      const GdkRGBA *color = &text;
      graphene_rect_t circle = GRAPHENE_RECT_INIT ((float) (x + (CELL - DAY_CIRCLE) / 2),
                                                   (float) (y + (CELL - DAY_CIRCLE) / 2), DAY_CIRCLE, DAY_CIRCLE);
      GskRoundedRect round;

      gsk_rounded_rect_init_from_rect (&round, &circle, DAY_CIRCLE / 2);
      if (day == picker->selected)
        {
          /* the choice: filled colorPrimary, its number onPrimary */
          gtk_snapshot_push_rounded_clip (snapshot, &round);
          gtk_snapshot_append_color (snapshot, &primary, &circle);
          gtk_snapshot_pop (snapshot);
          color = &on_primary;
        }
      else if (day == today)
        {
          /* today: a 1 dp ring and its number in colorPrimary */
          float widths[4] = { 1, 1, 1, 1 };
          GdkRGBA colors[4] = { primary, primary, primary, primary };

          gtk_snapshot_append_border (snapshot, &round, widths, colors);
          color = &primary;
        }
      cal_text_draw (snapshot, layout, x + CELL / 2, y + (CELL - height) / 2 + baseline, 0.5, color);
    }
}

static void
cal_day_grid_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                      int *min_baseline, int *nat_baseline)
{
  *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? (int) (7 * CELL + 6 * COLUMN_GAP)
                                                          : (int) ((GRID_ROWS + 1) * CELL);
}

static void
show_month (Picker *picker, int month)
{
  picker->month = CLAMP (month, YEAR_FIRST * 12, YEAR_LAST * 12 + 11);
  update (picker);
}

static void
on_grid_released (GtkGestureClick *gesture, int n, double x, double y, CalDayGrid *self)
{
  Picker *picker = self->picker;
  int first = month_start (picker->month), offset = (cal_day_of_week (first) - picker->week_start + 7) % 7;
  int column = (int) floor (x / (CELL + COLUMN_GAP)), row = (int) floor (y / CELL) - 1, d;

  if (self->swiping || row < 0 || column < 0 || column > 6 || x - column_x (column) > CELL)
    return;
  d = row * 7 + column - offset;
  if (d < 0 || d >= month_days (picker->month))
    return;
  picker->selected = first + d;
  update (picker);
}

static void
on_grid_drag_begin (GtkGestureDrag *gesture, double x, double y, CalDayGrid *self)
{
  self->swiping = FALSE;
}

static void
on_grid_drag_update (GtkGestureDrag *gesture, double dx, double dy, CalDayGrid *self)
{
  if (!self->swiping)
    {
      if (fabs (dx) < SWIPE_SLOP || fabs (dx) < fabs (dy))
        return;
      self->swiping = TRUE;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  self->drag_x = dx;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_grid_drag_end (GtkGestureDrag *gesture, double dx, double dy, CalDayGrid *self)
{
  /* the months page (ViewPager2): a swipe past a third of the width goes to the next or previous one */
  if (self->swiping && fabs (dx) > gtk_widget_get_width (GTK_WIDGET (self)) / 3)
    show_month (self->picker, self->picker->month + (dx < 0 ? 1 : -1));
  self->drag_x = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cal_day_grid_class_init (CalDayGridClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->snapshot = cal_day_grid_snapshot;
  widget_class->measure = cal_day_grid_measure;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_GRID);
}

static void
cal_day_grid_init (CalDayGrid *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();

  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  g_signal_connect (click, "released", G_CALLBACK (on_grid_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_grid_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_grid_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_grid_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

/* --- the dialog */

static void
update (Picker *picker)
{
  /* with nothing chosen (a date being typed) the header says so and OK is off (isSelectionComplete) */
  g_autofree char *selection = picker->selected != NO_DAY ? cal_format_picker_date (picker->selected)
                                                          : g_strdup (_("Selected date"));
  g_autofree char *month = cal_format_month_year (month_start (picker->month));
  g_autofree char *current = picker->selected != NO_DAY ? g_strdup_printf (_("Current selection: %s"), selection)
                                                        : g_strdup (_("None"));

  gtk_label_set_label (GTK_LABEL (picker->selection), selection);
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->selection), GTK_ACCESSIBLE_PROPERTY_LABEL, current, -1);
  gtk_label_set_label (GTK_LABEL (picker->month_label), month);
  gtk_widget_queue_draw (picker->grid);
  if (picker->ok)
    gtk_widget_set_sensitive (picker->ok, picker->selected != NO_DAY);
}

static void
on_prev (NeoPressable *button, Picker *picker)
{
  show_month (picker, picker->month - 1);
}

static void
on_next (NeoPressable *button, Picker *picker)
{
  show_month (picker, picker->month + 1);
}

static void
on_year (NeoPressable *button, Picker *picker)
{
  int year = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "cal-year"));

  /* the year list: the same month in that year, back to the days */
  show_month (picker, year * 12 + picker->month % 12);
  gtk_stack_set_visible_child_name (GTK_STACK (picker->stack), "days");
  gtk_widget_set_visible (picker->prev, TRUE);
  gtk_widget_set_visible (picker->next, TRUE);
}

/* the year shown in view, rows of three, 52 dp each: once the list has its size (scrolled before it is laid out,
 * the view clamps the offset to the top) */
static gboolean
scroll_to_year (GtkWidget *years, GdkFrameClock *clock, gpointer data)
{
  Picker *picker = data;

  if (gtk_widget_get_height (years) <= 0)
    return G_SOURCE_CONTINUE;
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (years),
                                MAX (0, ((picker->month / 12 - YEAR_FIRST) / 3 - 1) * 52.0));
  return G_SOURCE_REMOVE;
}

static void
on_month_toggle (NeoPressable *button, Picker *picker)
{
  gboolean years = g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (picker->stack)), "days");

  gtk_stack_set_visible_child_name (GTK_STACK (picker->stack), years ? "years" : "days");
  gtk_widget_set_visible (picker->prev, !years);
  gtk_widget_set_visible (picker->next, !years);
  if (years)
    gtk_widget_add_tick_callback (picker->years, scroll_to_year, picker, NULL);
}

static void
on_response (NeoDialog *dialog, int response, Picker *picker)
{
  g_clear_handle_id (&picker->validation, g_source_remove);
  if (response == NEO_RESPONSE_OK && picker->selected != NO_DAY)
    picker->chosen (picker->selected, picker->data);
}

/* The year list is as tall as the days (six rows and the weekday row) and scrolls the rest. A size request alone
 * no longer does it: NeoDialog wraps its content's natural height, and the list's is all of its 67 rows, which
 * would stretch the card to the window. Its holder asks for that one height, natural and minimum. */
static void
years_measure (GtkWidget *holder, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
               int *nat_base)
{
  if (orientation == GTK_ORIENTATION_VERTICAL)
    *min = *nat = (int) ((GRID_ROWS + 1) * CELL);
  else
    gtk_widget_measure (gtk_widget_get_first_child (holder), orientation, for_size, min, nat, NULL, NULL);
}

static void
years_allocate (GtkWidget *holder, int width, int height, int baseline)
{
  gtk_widget_allocate (gtk_widget_get_first_child (holder), width, height, baseline, NULL);
}

/* the years page: picker->years is the list, the page its holder */
static GtkWidget *
years_list (Picker *picker)
{
  GtkWidget *grid = gtk_grid_new ();
  GtkWidget *scroll = spongy_scroll_view_new ();
  int this_year = month_of (cal_day_number (g_get_real_time () / G_USEC_PER_SEC)) / 12;

  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  for (int year = YEAR_FIRST; year <= YEAR_LAST; year++)
    {
      g_autofree char *text = g_strdup_printf ("%d", year);
      GtkWidget *label = gtk_label_new (text);
      GtkWidget *button = neo_pressable_new (label);

      gtk_widget_add_css_class (button, "cal-year");
      gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
      /* YearGridAdapter: the year of the chosen day filled, this year outlined */
      if (picker->selected != NO_DAY && year == month_of (picker->selected) / 12)
        gtk_widget_add_css_class (button, "selected");
      else if (year == this_year)
        gtk_widget_add_css_class (button, "today");
      neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 18);
      g_object_set_data (G_OBJECT (button), "cal-year", GINT_TO_POINTER (year));
      g_signal_connect (button, "clicked", G_CALLBACK (on_year), picker);
      gtk_grid_attach (GTK_GRID (grid), button, (year - YEAR_FIRST) % 3, (year - YEAR_FIRST) / 3, 1, 1);
    }
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), grid);
  picker->years = scroll;
  {
    GtkWidget *holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_layout_manager (holder, gtk_custom_layout_new (NULL, years_measure, years_allocate));
    gtk_box_append (GTK_BOX (holder), scroll);
    return holder;
  }
}

/* --- the text input mode: the outlined field (TextInputLayout.OutlinedBox under Material 3) */

#define CAL_TYPE_DATE_FIELD (cal_date_field_get_type ())
G_DECLARE_FINAL_TYPE (CalDateField, cal_date_field, CAL, DATE_FIELD, GtkWidget)

struct _CalDateField {
  GtkWidget parent;
  GtkWidget *text;
  char *label;
  char *placeholder;
  gboolean error;
  double floated;               /* the label: 0 in the field at 16 sp, 1 on the outline at 12 sp */
  NeoAnim *float_anim;
  GskPath *error_icon;
};

G_DEFINE_FINAL_TYPE (CalDateField, cal_date_field, GTK_TYPE_WIDGET)

static void
cal_date_field_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                        int *min_baseline, int *nat_baseline)
{
  *min = *nat = orientation == GTK_ORIENTATION_VERTICAL ? (int) FIELD_HEIGHT : 0;
}

static void
cal_date_field_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalDateField *self = CAL_DATE_FIELD (widget);
  /* the end icon's 48 dp take the text's end padding while the error shows */
  int end = self->error ? (int) (ERROR_ICON + 2 * ERROR_ICON_END) : (int) FIELD_PADDING, text_height, ignored;

  gtk_widget_measure (self->text, GTK_ORIENTATION_VERTICAL, -1, &ignored, &text_height, NULL, NULL);
  gtk_widget_size_allocate (self->text,
                            &(GtkAllocation) { (int) FIELD_PADDING, (height - text_height) / 2,
                                               MAX (0, width - (int) FIELD_PADDING - end), text_height }, -1);
}

static void
cal_date_field_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalDateField *self = CAL_DATE_FIELD (widget);
  double width = gtk_widget_get_width (widget), f = self->floated;
  int scale = neo_device_scale (widget);
  gboolean focused = gtk_widget_has_focus (self->text);
  double stroke = focused ? 2 : 1, inset = stroke / 2, size = FIELD_TEXT_SIZE + (LABEL_SIZE - FIELD_TEXT_SIZE) * f;
  double baseline, line, label_width = 0;
  g_autoptr(PangoLayout) label = cal_text_layout (widget, self->label, size, 700);
  g_autoptr(PangoLayout) collapsed = cal_text_layout (widget, self->label, LABEL_SIZE, 700);
  g_autoptr(GskPath) path = NULL;
  g_autoptr(GskStroke) pen = gsk_stroke_new ((float) stroke);
  GdkRGBA error, outline, label_color;
  PangoRectangle logical;

  /* the error colour is the field's CSS colour (.cal-date-field.error) */
  gtk_widget_get_color (widget, &error);
  outline = self->error ? error : cal_argb (focused ? CAL_PRIMARY : CAL_OUTLINE);
  label_color = self->error ? error : cal_argb (focused ? CAL_PRIMARY : CAL_ON_SURFACE_VARIANT);

  pango_layout_get_extents (collapsed, NULL, &logical);
  if (f > 0)
    label_width = logical.width / (double) PANGO_SCALE;
  path = cal_outline_path (inset, inset, width - stroke, FIELD_HEIGHT - stroke, FIELD_CORNER,
                       f > 0 ? FIELD_PADDING - LABEL_CUTOUT : 0, f > 0 ? FIELD_PADDING + label_width + LABEL_CUTOUT : 0);
  gtk_snapshot_append_stroke (snapshot, path, pen, &outline);

  /* the label, from the middle of the field to the middle of the top edge */
  cal_text_line (size, scale, &baseline, &line);
  cal_text_draw (snapshot, label, FIELD_PADDING, (FIELD_HEIGHT / 2) * (1 - f) - line / 2 + baseline, 0, &label_color);

  if (self->error)
    {
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) (width - ERROR_ICON_END - ERROR_ICON),
                                                              (float) ((FIELD_HEIGHT - ERROR_ICON) / 2)));
      gtk_snapshot_append_fill (snapshot, self->error_icon, GSK_FILL_RULE_WINDING, &error);
      gtk_snapshot_restore (snapshot);
    }
  gtk_widget_snapshot_child (widget, self->text, snapshot);
}

static void
on_float (NeoAnim *anim, double value, gpointer data)
{
  CAL_DATE_FIELD (data)->floated = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

/* the label floats with the focus or a text; the placeholder shows only then, under it (TextInputLayout) */
static void
field_restate (CalDateField *self, gboolean animate)
{
  gboolean focused = gtk_widget_has_focus (self->text);
  gboolean up = focused || *gtk_editable_get_text (GTK_EDITABLE (self->text));

  gtk_text_set_placeholder_text (GTK_TEXT (self->text), focused ? self->placeholder : NULL);
  if (animate && (up ? 1.0 : 0.0) != self->floated)
    neo_anim_start (self->float_anim, self->floated, up ? 1 : 0, LABEL_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  else if (!animate)
    {
      neo_anim_stop (self->float_anim);
      self->floated = up ? 1 : 0;
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_field_focus (GtkEventControllerFocus *focus, CalDateField *self)
{
  field_restate (self, TRUE);
}

static void
cal_date_field_dispose (GObject *object)
{
  CalDateField *self = CAL_DATE_FIELD (object);

  g_clear_pointer (&self->text, gtk_widget_unparent);
  g_clear_pointer (&self->float_anim, neo_anim_free);
  g_clear_pointer (&self->error_icon, gsk_path_unref);
  g_clear_pointer (&self->label, g_free);
  g_clear_pointer (&self->placeholder, g_free);
  G_OBJECT_CLASS (cal_date_field_parent_class)->dispose (object);
}

static void
cal_date_field_class_init (CalDateFieldClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_date_field_dispose;
  widget_class->measure = cal_date_field_measure;
  widget_class->size_allocate = cal_date_field_allocate;
  widget_class->snapshot = cal_date_field_snapshot;
}

static void
cal_date_field_init (CalDateField *self)
{
  GtkEventController *focus = gtk_event_controller_focus_new ();

  gtk_widget_add_css_class (GTK_WIDGET (self), "cal-date-field");
  self->label = g_strdup (_("Date"));
  self->error_icon = gsk_path_parse (ERROR_ICON_PATH);
  self->float_anim = neo_anim_new (GTK_WIDGET (self), on_float, self);
  self->text = gtk_text_new ();
  /* inputType date: the digits and the separators */
  gtk_text_set_input_purpose (GTK_TEXT (self->text), GTK_INPUT_PURPOSE_NUMBER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->text), GTK_ACCESSIBLE_PROPERTY_LABEL, self->label, -1);
  gtk_widget_set_parent (self->text, GTK_WIDGET (self));
  g_signal_connect (focus, "enter", G_CALLBACK (on_field_focus), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_field_focus), self);
  gtk_widget_add_controller (self->text, focus);
}

static void
field_set_error (CalDateField *self, gboolean error)
{
  if (self->error == error)
    return;
  self->error = error;
  if (error)
    gtk_widget_add_css_class (GTK_WIDGET (self), "error");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "error");
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- the input format (UtcDates) */

static char *
utf8 (const UChar *text, int32_t length)
{
  UErrorCode status = U_ZERO_ERROR;
  int32_t size = 0;
  char *out;

  u_strToUTF8 (NULL, 0, &size, text, length, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE (status))
    return NULL;
  status = U_ZERO_ERROR;
  out = g_malloc (size + 1);
  u_strToUTF8 (out, size + 1, NULL, text, length, &status);
  if (U_FAILURE (status))
    g_clear_pointer (&out, g_free);
  return out;
}

static char *
regex_replace (const char *text, const char *pattern, const char *replacement)
{
  g_autoptr(GRegex) regex = g_regex_new (pattern, 0, 0, NULL);

  return g_regex_replace_literal (regex, text, -1, 0, replacement, 0, NULL);
}

/* getDefaultTextInputFormat → getDatePatternAsInputFormat: the locale's SHORT date as digits only */
static char *
input_pattern (void)
{
  UErrorCode status = U_ZERO_ERROR;
  UDateFormat *format = udat_open (UDAT_NONE, UDAT_SHORT, cal_format_date_locale (), NULL, 0, NULL, 0, &status);
  UChar pattern[64];
  int32_t length;
  g_autofree char *text = NULL;
  char *steps[6];

  if (U_FAILURE (status))
    return g_strdup ("MM/dd/yyyy");
  length = udat_toPattern (format, FALSE, pattern, G_N_ELEMENTS (pattern), &status);
  udat_close (format);
  text = U_SUCCESS (status) ? utf8 (pattern, length) : NULL;
  if (!text)
    return g_strdup ("MM/dd/yyyy");
  steps[0] = regex_replace (text, "[^dMy/\\-.]", "");
  steps[1] = regex_replace (steps[0], "d{1,2}", "dd");
  steps[2] = regex_replace (steps[1], "M{1,2}", "MM");
  steps[3] = regex_replace (steps[2], "y{1,4}", "yyyy");
  steps[4] = regex_replace (steps[3], "\\.$", "");
  steps[5] = regex_replace (steps[4], "My", "M/y");
  for (int i = 0; i < 5; i++)
    g_free (steps[i]);
  return steps[5];
}

/* getDefaultTextInputHint: the pattern in the abbreviations mtrl_picker_text_input_{day,month,year}_abbr */
static char *
input_hint (const char *pattern)
{
  GString *hint = g_string_new (NULL);

  for (const char *p = pattern; *p; p++)
    if (*p == 'd')
      g_string_append (hint, C_("date input: day", "d"));
    else if (*p == 'M')
      g_string_append (hint, C_("date input: month", "m"));
    else if (*p == 'y')
      g_string_append (hint, C_("date input: year", "y"));
    else
      g_string_append_c (hint, *p);
  return g_string_free (hint, FALSE);
}

static UDateFormat *
pattern_format (const char *pattern)
{
  UErrorCode status = U_ZERO_ERROR;
  UChar text[64], utc[] = { 'U', 'T', 'C', 0 };
  UDateFormat *format;

  if (strlen (pattern) >= G_N_ELEMENTS (text))
    return NULL;
  u_uastrcpy (text, pattern);
  format = udat_open (UDAT_PATTERN, UDAT_PATTERN, cal_format_date_locale (), utc, -1, text, -1, &status);
  if (U_FAILURE (status))
    return NULL;
  udat_setLenient (format, FALSE);
  return format;
}

/* a day number as text in @format (UTC midnight) */
static char *
format_day (UDateFormat *format, int day)
{
  GDate date, epoch;
  UChar text[64];
  UErrorCode status = U_ZERO_ERROR;
  int32_t length;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  g_date_clear (&epoch, 1);
  g_date_set_dmy (&epoch, 1, 1, 1970);
  length = udat_format (format, (UDate) g_date_days_between (&epoch, &date) * 86400000.0, text,
                        G_N_ELEMENTS (text), NULL, &status);
  return U_SUCCESS (status) ? utf8 (text, length) : NULL;
}

/* DateStrings.getYearMonthDay's skeleton yMMMd, for a day before year 1 that the header's format cannot show */
static char *
format_far_day (UDate ms)
{
  UErrorCode status = U_ZERO_ERROR;
  UDateTimePatternGenerator *generator = udatpg_open (cal_format_date_locale (), &status);
  UChar skeleton[] = { 'y', 'M', 'M', 'M', 'd', 0 }, pattern[64], text[64], utc[] = { 'U', 'T', 'C', 0 };
  UDateFormat *format;
  int32_t length;
  char *out = NULL;

  if (U_FAILURE (status))
    return NULL;
  length = udatpg_getBestPattern (generator, skeleton, -1, pattern, G_N_ELEMENTS (pattern), &status);
  udatpg_close (generator);
  if (U_FAILURE (status))
    return NULL;
  format = udat_open (UDAT_PATTERN, UDAT_PATTERN, cal_format_date_locale (), utc, -1, pattern, length, &status);
  if (U_FAILURE (status))
    return NULL;
  length = udat_format (format, ms, text, G_N_ELEMENTS (text), NULL, &status);
  if (U_SUCCESS (status))
    out = utf8 (text, length);
  udat_close (format);
  return out;
}

/* sanitizeDateString: no line break inside a date */
static char *
no_break (const char *text)
{
  g_auto(GStrv) parts = g_strsplit (text ? text : "", " ", -1);

  return g_strjoinv ("\u00a0", parts);
}

static void
set_error (Picker *picker, const char *error)
{
  gtk_label_set_label (GTK_LABEL (picker->error), error ? error : "");
  gtk_widget_set_visible (picker->error, error != NULL);
  field_set_error (CAL_DATE_FIELD (picker->field), error != NULL);
}

static gboolean
on_validation (gpointer data)
{
  Picker *picker = data;

  picker->validation = 0;
  set_error (picker, picker->pending_error);
  return G_SOURCE_REMOVE;
}

/* DateFormatTextWatcher.onTextChanged: the choice cleared, then the text parsed once it is as long as the hint */
static void
validate (Picker *picker, const char *text)
{
  UDateFormat *format;
  UErrorCode status = U_ZERO_ERROR;
  UChar input[64];
  int32_t position = 0;
  UDate ms;
  GDate first, last, epoch, day;

  g_clear_handle_id (&picker->validation, g_source_remove);
  g_clear_pointer (&picker->pending_error, g_free);
  set_error (picker, NULL);
  picker->selected = NO_DAY;
  update (picker);
  if (!*text || g_utf8_strlen (text, -1) < g_utf8_strlen (picker->hint, -1) || strlen (text) >= G_N_ELEMENTS (input))
    return;
  format = pattern_format (picker->pattern);
  if (!format)
    return;
  u_uastrcpy (input, text);
  ms = udat_parse (format, input, -1, &position, &status);
  if (U_FAILURE (status))
    {
      g_autofree char *today = NULL, *use = NULL, *example = NULL, *hint = no_break (picker->hint);
      g_autofree char *use_line = NULL, *example_line = NULL;

      today = format_day (format, cal_day_number (g_get_real_time () / G_USEC_PER_SEC));
      example = no_break (today);
      use_line = g_strdup_printf (_("Use: %s"), hint);
      example_line = g_strdup_printf (_("Example: %s"), example);
      picker->pending_error = g_strconcat (_("Invalid format."), "\n", use_line, "\n", example_line, NULL);
    }
  else
    {
      gint64 days = (gint64) floor (ms / 86400000.0), first_days, last_days;

      g_date_clear (&epoch, 1);
      g_date_set_dmy (&epoch, 1, 1, 1970);
      g_date_clear (&first, 1);
      g_date_set_dmy (&first, 1, 1, YEAR_FIRST);
      g_date_clear (&last, 1);
      g_date_set_dmy (&last, 31, 12, YEAR_LAST);
      first_days = g_date_days_between (&epoch, &first);
      last_days = g_date_days_between (&epoch, &last);
      if (days >= first_days && days <= last_days)
        {
          /* CalendarConstraints' default 1900 … 2100, the calendar's years: the day is chosen */
          day = first;
          g_date_add_days (&day, (guint) (days - first_days));
          picker->selected = (int) g_date_get_julian (&day);
          picker->month = month_of (picker->selected);
          update (picker);
        }
      else
        {
          /* the day as the header would show it; ICU's yMMMd for one GDate cannot hold (before year 1) */
          gint64 julian = (gint64) g_date_get_julian (&epoch) + days;
          g_autofree char *far = julian >= 1 && julian <= G_MAXINT32 / 2
                                 ? cal_format_picker_date ((int) julian) : format_far_day ((UDate) days * 86400000.0);
          g_autofree char *date = no_break (far);

          picker->pending_error = g_strdup_printf (_("Out of range: %s"), date);
        }
    }
  udat_close (format);
  if (picker->pending_error)
    picker->validation = g_timeout_add (VALIDATION_MS, on_validation, picker);
}

static void
on_date_changed (GtkEditable *editable, Picker *picker)
{
  const char *text = gtk_editable_get_text (editable);
  int length = (int) g_utf8_strlen (text, -1), hint_length = (int) g_utf8_strlen (picker->hint, -1);

  if (picker->setting_text)
    return;
  /* afterTextChanged: typing forward, the pattern's next character when it is a separator */
  if (length > 0 && length < hint_length && length > picker->last_length)
    {
      gunichar next = g_utf8_get_char (g_utf8_offset_to_pointer (picker->hint, length));

      if (!g_unichar_isalnum (next))
        {
          char separator[7] = { 0 };
          g_autofree char *longer = NULL;

          g_unichar_to_utf8 (next, separator);
          longer = g_strconcat (text, separator, NULL);
          picker->setting_text = TRUE;
          gtk_editable_set_text (editable, longer);
          gtk_editable_set_position (editable, -1);
          picker->setting_text = FALSE;
          text = gtk_editable_get_text (editable);
          length++;
        }
    }
  picker->last_length = length;
  field_restate (CAL_DATE_FIELD (picker->field), TRUE);
  validate (picker, text);
}

/* DateKeyListener: digits and the separators a date may have */
static void
on_date_insert (GtkEditable *editable, const char *text, int length, int *position, Picker *picker)
{
  for (const char *p = text; length < 0 ? *p : p < text + length; p++)
    if (!g_ascii_isdigit (*p) && !strchr ("/-.", *p))
      {
        g_signal_stop_emission_by_name (editable, "insert-text");
        return;
      }
}

/* mtrl_picker_text_input_date: the field 16 dp under the header, 12 dp in from the sides, the error under it */
static GtkWidget *
text_input (Picker *picker)
{
  GtkWidget *pane = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  CalDateField *field = g_object_new (CAL_TYPE_DATE_FIELD, NULL);

  gtk_widget_add_css_class (pane, "cal-date-input");
  field->placeholder = g_strdup (picker->hint);
  picker->field = GTK_WIDGET (field);
  g_signal_connect (field->text, "changed", G_CALLBACK (on_date_changed), picker);
  g_signal_connect (field->text, "insert-text", G_CALLBACK (on_date_insert), picker);
  gtk_box_append (GTK_BOX (pane), picker->field);
  picker->error = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (picker->error), 0);
  gtk_label_set_wrap (GTK_LABEL (picker->error), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (picker->error), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (picker->error, "cal-date-error");
  gtk_widget_set_visible (picker->error, FALSE);
  gtk_box_append (GTK_BOX (pane), picker->error);
  gtk_widget_set_visible (pane, FALSE);
  return pane;
}

/* the pencil and the calendar icon (MaterialDatePicker.toggleInputMode) */
static void
on_mode_toggle (NeoPressable *button, Picker *picker)
{
  GtkWidget *image = neo_pressable_get_child (button);

  picker->text_mode = !picker->text_mode;
  gtk_widget_set_visible (picker->nav, !picker->text_mode);
  gtk_widget_set_visible (picker->stack, !picker->text_mode);
  gtk_widget_set_visible (picker->input, picker->text_mode);
  gtk_image_set_from_icon_name (GTK_IMAGE (image), picker->text_mode ? "cal-calendar-symbolic" : "cal-pencil-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  picker->text_mode ? _("Switch to calendar input mode")
                                                    : _("Switch to text input mode"), -1);
  if (picker->text_mode)
    {
      CalDateField *field = CAL_DATE_FIELD (picker->field);
      g_autofree char *text = NULL;
      UDateFormat *format = pattern_format (picker->pattern);

      /* the chosen day in the input format, the cursor after it, the keyboard up */
      if (format && picker->selected != NO_DAY)
        text = format_day (format, picker->selected);
      if (format)
        udat_close (format);
      g_clear_handle_id (&picker->validation, g_source_remove);
      set_error (picker, NULL);
      picker->setting_text = TRUE;
      gtk_editable_set_text (GTK_EDITABLE (field->text), text ? text : "");
      picker->setting_text = FALSE;
      picker->last_length = (int) g_utf8_strlen (text ? text : "", -1);
      gtk_widget_grab_focus (field->text);
      gtk_editable_set_position (GTK_EDITABLE (field->text), -1);
      field_restate (field, FALSE);
    }
  else
    {
      g_clear_handle_id (&picker->validation, g_source_remove);
      gtk_stack_set_visible_child_name (GTK_STACK (picker->stack), "days");
      gtk_widget_set_visible (picker->prev, TRUE);
      gtk_widget_set_visible (picker->next, TRUE);
      if (picker->selected != NO_DAY)
        picker->month = month_of (picker->selected);
      gtk_widget_grab_focus (GTK_WIDGET (button));
      update (picker);
    }
}

static void
picker_free (gpointer data, GClosure *closure)
{
  Picker *picker = data;

  g_clear_handle_id (&picker->validation, g_source_remove);
  g_free (picker->pending_error);
  g_free (picker->pattern);
  g_free (picker->hint);
  g_free (picker);
}

void
cal_date_picker_show (GtkWidget *parent, const char *title, int day, int week_start, CalDateChosen chosen,
                      gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  NeoDialog *dialog = neo_dialog_new (NULL);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title_label = gtk_label_new (title);
  GtkWidget *selection_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *toggle_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *toggle, *arrow;
  CalDayGrid *grid;

  picker->dialog = dialog;
  picker->chosen = chosen;
  picker->data = data;
  picker->selected = day;
  picker->month = month_of (day);
  picker->week_start = week_start;
  picker->pattern = input_pattern ();
  picker->hint = input_hint (picker->pattern);

  /* the header: the title, then the chosen date large, the pencil at its end (mtrl_picker_header_toggle: 48 dp,
   * 12 dp from the card's end, 8 dp over the header's end) */
  gtk_widget_add_css_class (header, "cal-date-header");
  gtk_label_set_xalign (GTK_LABEL (title_label), 0);
  gtk_widget_add_css_class (title_label, "cal-date-title");
  gtk_box_append (GTK_BOX (header), title_label);
  picker->selection = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (picker->selection), 0);
  gtk_widget_set_hexpand (picker->selection, TRUE);
  gtk_widget_set_valign (picker->selection, GTK_ALIGN_END);
  gtk_widget_set_vexpand (selection_row, TRUE);
  gtk_widget_add_css_class (picker->selection, "cal-date-selection");
  gtk_box_append (GTK_BOX (selection_row), picker->selection);
  picker->toggle = neo_icon_button_new ("cal-pencil-symbolic");
  gtk_widget_add_css_class (picker->toggle, "cal-date-toggle");
  gtk_widget_set_valign (picker->toggle, GTK_ALIGN_END);
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->toggle), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Switch to text input mode"), -1);
  g_signal_connect (picker->toggle, "clicked", G_CALLBACK (on_mode_toggle), picker);
  gtk_box_append (GTK_BOX (selection_row), picker->toggle);
  gtk_box_append (GTK_BOX (header), selection_row);
  /* no divider under it: MaterialDatePicker draws one in its fullscreen mode only (none on still:cal-dlg-date) */
  gtk_box_append (GTK_BOX (content), header);

  /* the month: "November 2026 ▾" (the year list), ‹ › */
  picker->nav = nav;
  gtk_widget_add_css_class (nav, "cal-date-nav");
  picker->month_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (picker->month_label, "cal-date-month");
  arrow = gtk_image_new_from_icon_name ("cal-drop-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (arrow), 24);
  gtk_box_append (GTK_BOX (toggle_box), picker->month_label);
  gtk_box_append (GTK_BOX (toggle_box), arrow);
  toggle = neo_pressable_new (toggle_box);
  gtk_widget_add_css_class (toggle, "cal-date-month-toggle");
  neo_pressable_set_ripple (NEO_PRESSABLE (toggle), NEO_RIPPLE_BOUNDED, 24);
  gtk_accessible_update_property (GTK_ACCESSIBLE (toggle), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  _("Switch to year view"), -1);
  g_signal_connect (toggle, "clicked", G_CALLBACK (on_month_toggle), picker);
  gtk_widget_set_hexpand (toggle, FALSE);
  gtk_box_append (GTK_BOX (nav), toggle);
  {
    GtkWidget *space = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_widget_set_hexpand (space, TRUE);
    gtk_box_append (GTK_BOX (nav), space);
  }
  picker->prev = neo_icon_button_new ("cal-chevron-left-symbolic");
  gtk_widget_add_css_class (picker->prev, "cal-date-arrow");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->prev), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Change to previous month"), -1);
  g_signal_connect (picker->prev, "clicked", G_CALLBACK (on_prev), picker);
  gtk_box_append (GTK_BOX (nav), picker->prev);
  picker->next = neo_icon_button_new ("cal-chevron-right-symbolic");
  gtk_widget_add_css_class (picker->next, "cal-date-arrow");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->next), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Change to next month"), -1);
  g_signal_connect (picker->next, "clicked", G_CALLBACK (on_next), picker);
  gtk_box_append (GTK_BOX (nav), picker->next);
  gtk_box_append (GTK_BOX (content), nav);

  grid = g_object_new (CAL_TYPE_DAY_GRID, NULL);
  grid->picker = picker;
  picker->grid = GTK_WIDGET (grid);
  gtk_widget_set_halign (picker->grid, GTK_ALIGN_CENTER);
  picker->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (picker->stack), picker->grid, "days");
  gtk_stack_add_named (GTK_STACK (picker->stack), years_list (picker), "years");
  gtk_widget_add_css_class (picker->stack, "cal-date-days");
  gtk_box_append (GTK_BOX (content), picker->stack);
  picker->input = text_input (picker);
  gtk_box_append (GTK_BOX (content), picker->input);

  gtk_widget_set_size_request (content, (int) CARD_WIDTH, -1);
  gtk_widget_add_css_class (content, "cal-date-picker");
  neo_dialog_set_flush (dialog, TRUE);
  neo_dialog_set_content (dialog, content);
  /* the card is the content's parent: its own class takes the M3 picker's colour and button band */
  gtk_widget_add_css_class (gtk_widget_get_parent (content), "cal-date-card");
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  picker->ok = neo_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  update (picker);
  neo_dialog_present (dialog, parent);
}
