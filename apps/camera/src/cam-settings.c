/* cam-settings: see cam-settings.h. Measured on still:camera-settings (S10e, 3 px per dp): the app bar 179 dp
 * under the status bar, the up arrow's glyph at x 61…107 px, "Settings" 36 sp bold from x 75 px with its baseline
 * 31 dp above the bar's bottom; rows with their text 24 dp in, categories in colorAccent; switches end 16 dp from
 * the edge. */
#include "cam-settings.h"
#include "cam-dialogs.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define TOOLBAR_H        64       /* ?actionBarSize */
#define SUMMARY_LINE_H   20       /* textAppearanceBodyMedium's line height, 20 sp */
#define APP_BAR_H        179      /* the large collapsing toolbar, expanded */
#define TITLE_BASELINE   31       /* above the app bar's bottom */
#define TITLE_SIZE       36       /* textAppearanceDisplaySmall */

typedef struct {
  const char *key;
  const char *title;
  const char *const *labels;
  const char *const *values;
  int         n;
  GtkWidget  *summary;
} ListRow;

struct _CamSettings {
  GtkWidget        parent;
  GSettings       *settings;
  CamSettingsRows  rows;
  GtkWidget       *toolbar, *collapsed, *stack;
  GtkWidget       *pages[2], *scrollers[2];
  GtkWidget       *invert_row;
  GPtrArray       *lists;       /* ListRow * */
  GtkWidget       *bright, *location, *leveler, *gestures, *photos, *video, *focus;
  int              page;
};

G_DEFINE_FINAL_TYPE (CamSettings, cam_settings, GTK_TYPE_WIDGET)

/* ---- a summary: wrapped text with BodyMedium's 20 sp line height ------------------------------------------------
 * A TextView with a line height puts the extra space between its lines only (StaticLayout's spacing), which is
 * Pango's layout spacing; CSS line-height on a GtkLabel would pad its first and last lines too, and so every
 * one-line row as well. */

#define CAM_TYPE_SUMMARY (cam_summary_get_type ())
G_DECLARE_FINAL_TYPE (CamSummary, cam_summary, CAM, SUMMARY, GtkWidget)

struct _CamSummary {
  GtkWidget  parent;
  char      *text;
};

G_DEFINE_FINAL_TYPE (CamSummary, cam_summary, GTK_TYPE_WIDGET)

static PangoLayout *
summary_layout (CamSummary *self, int width)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), "X");
  PangoRectangle line;

  pango_layout_get_extents (layout, NULL, &line);
  pango_layout_set_text (layout, self->text ? self->text : "", -1);
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  if (width > 0)
    pango_layout_set_width (layout, width * PANGO_SCALE);
  pango_layout_set_spacing (layout, MAX (0, SUMMARY_LINE_H * PANGO_SCALE - line.height));
  return layout;
}

static void
cam_summary_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                     int *min_base, int *nat_base)
{
  g_autoptr(PangoLayout) layout = summary_layout (CAM_SUMMARY (widget),
                                                  orientation == GTK_ORIENTATION_VERTICAL ? for_size : -1);
  PangoRectangle logical;

  pango_layout_get_pixel_extents (layout, NULL, &logical);
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *min = 0, *nat = logical.width;
  else
    *min = *nat = logical.height;
}

static GtkSizeRequestMode
cam_summary_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
cam_summary_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  g_autoptr(PangoLayout) layout = summary_layout (CAM_SUMMARY (widget), gtk_widget_get_width (widget));
  GdkRGBA color;

  gtk_widget_get_color (widget, &color);
  gtk_snapshot_append_layout (snapshot, layout, &color);
}

static void
cam_summary_finalize (GObject *object)
{
  g_free (CAM_SUMMARY (object)->text);
  G_OBJECT_CLASS (cam_summary_parent_class)->finalize (object);
}

static void
cam_summary_class_init (CamSummaryClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = cam_summary_finalize;
  GTK_WIDGET_CLASS (klass)->get_request_mode = cam_summary_get_request_mode;
  GTK_WIDGET_CLASS (klass)->measure = cam_summary_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = cam_summary_snapshot;
  gtk_widget_class_set_accessible_role (GTK_WIDGET_CLASS (klass), GTK_ACCESSIBLE_ROLE_LABEL);
}

static void
cam_summary_init (CamSummary *self)
{
}

static void
cam_summary_set_text (CamSummary *self, const char *text)
{
  if (!g_strcmp0 (self->text, text))
    return;
  g_free (self->text);
  self->text = g_strdup (text);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, text ? text : "", -1);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static GtkWidget *
cam_summary_new (const char *text)
{
  CamSummary *self = g_object_new (CAM_TYPE_SUMMARY, NULL);

  cam_summary_set_text (self, text);
  return GTK_WIDGET (self);
}

enum { SIG_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- rows ------------------------------------------------------------------------------------------------------ */

static GtkWidget *
category (GtkWidget *list, const char *title)
{
  GtkWidget *label = gtk_label_new (title);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  neo_label_set_font_padding (label, 14);
  /* PreferenceCategory (SettingsLib): 24 dp above the first, more space between groups */
  gtk_widget_set_margin_top (label, gtk_widget_get_margin_top (label) + 24);
  gtk_widget_set_margin_bottom (label, gtk_widget_get_margin_bottom (label) + 8);
  gtk_widget_set_margin_start (label, 24);
  gtk_widget_set_margin_end (label, 24);
  gtk_widget_add_css_class (label, "cam-settings-category");
  gtk_box_append (GTK_BOX (list), label);
  return label;
}

static GtkWidget *
row (GtkWidget *list, const char *title, const char *summary_text, GtkWidget **summary, GtkWidget *end)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *sub = cam_summary_new (summary_text);
  GtkWidget *pressable;

  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 16);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (column, 16);
  gtk_widget_set_margin_bottom (column, 16);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "cam-settings-title");
  neo_label_set_font_padding (label, 16);
  /* the TextView is 65 px at 3 px/dp (its font metrics rounded outwards): 21⅔ dp, which whole dp can only come
   * near; 22 drifts +1 px a row where 21 drifts -2 */
  gtk_widget_set_margin_bottom (label, gtk_widget_get_margin_bottom (label) + 1);
  gtk_box_append (GTK_BOX (column), label);
  gtk_widget_add_css_class (sub, "cam-settings-summary");
  /* neo_label_set_font_padding's margins, for a summary that is not a GtkLabel */
  gtk_widget_set_margin_top (sub, (int) round (14 * (2163 - 1900) / 2048.0));
  gtk_widget_set_margin_bottom (sub, (int) round (14 * (555 - 500) / 2048.0));
  gtk_widget_set_visible (sub, summary_text != NULL);
  gtk_box_append (GTK_BOX (column), sub);
  if (summary)
    *summary = sub;
  gtk_box_append (GTK_BOX (box), column);
  if (end)
    {
      gtk_widget_set_margin_start (end, 16);
      gtk_widget_set_valign (end, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), end);
    }
  else
    gtk_widget_set_margin_end (box, 24);
  pressable = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_BOUNDED, 0);
  gtk_widget_add_css_class (pressable, "cam-settings-row");
  gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (list), pressable);
  return pressable;
}

static void
on_switch_row (NeoPressable *pressable, NeoSwitch *toggle)
{
  neo_switch_set_active (toggle, !neo_switch_get_active (toggle));
}

static GtkWidget *
switch_row (CamSettings *self, GtkWidget *list, const char *key, const char *title, const char *summary)
{
  GtkWidget *toggle = neo_switch_new ();
  GtkWidget *pressable;

  neo_switch_set_thumb_icons (NEO_SWITCH (toggle), FALSE);
  pressable = row (list, title, summary, NULL, toggle);
  if (key)
    g_settings_bind (self->settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_switch_row), toggle);
  g_object_set_data (G_OBJECT (pressable), "cam-switch", toggle);
  return pressable;
}

static int
list_index (CamSettings *self, ListRow *list)
{
  g_autofree char *value = g_settings_get_string (self->settings, list->key);

  for (int i = 0; i < list->n; i++)
    if (g_str_equal (list->values[i], value))
      return i;
  return 0;
}

static void
show_list (CamSettings *self, ListRow *list)
{
  /* ListPreference's "%s" summary: the chosen entry */
  cam_summary_set_text (CAM_SUMMARY (list->summary), list->labels[list_index (self, list)]);
}

typedef struct {
  CamSettings *self;
  ListRow     *list;
} Pick;

static void
on_picked (int chosen, gpointer data)
{
  Pick *pick = data;

  if (chosen >= 0)
    g_settings_set_string (pick->self->settings, pick->list->key, pick->list->values[chosen]);
  g_object_unref (pick->self);
  g_free (pick);
}

static void
on_list_row (NeoPressable *pressable, CamSettings *self)
{
  Pick *pick = g_new0 (Pick, 1);

  pick->self = g_object_ref (self);
  pick->list = g_object_get_data (G_OBJECT (pressable), "cam-list");
  cam_list_dialog (GTK_WIDGET (self), pick->list->title, pick->list->labels, pick->list->n,
                   list_index (self, pick->list), on_picked, pick);
}

static GtkWidget *
list_row (CamSettings *self, GtkWidget *list, const char *key, const char *title, const char *const *labels,
          const char *const *values, int n)
{
  ListRow *data = g_new0 (ListRow, 1);
  GtkWidget *pressable;

  data->key = key;
  data->title = title;
  data->labels = labels;
  data->values = values;
  data->n = n;
  pressable = row (list, title, "", &data->summary, NULL);
  g_object_set_data (G_OBJECT (pressable), "cam-list", data);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_list_row), self);
  g_ptr_array_add (self->lists, data);
  show_list (self, data);
  return pressable;
}

/* ---- pages ----------------------------------------------------------------------------------------------------- */

static void
on_scrolled (GtkAdjustment *adjustment, CamSettings *self)
{
  /* the large title scrolls under the toolbar; once it has, the toolbar takes the title (titleCollapseMode fade)
   * and lifts (liftOnScroll) */
  gboolean collapsed = gtk_adjustment_get_value (adjustment) >= APP_BAR_H - TOOLBAR_H - TITLE_BASELINE / 2;

  gtk_widget_set_opacity (self->collapsed, collapsed ? 1 : 0);
  if (gtk_adjustment_get_value (adjustment) > 0)
    gtk_widget_add_css_class (self->toolbar, "lifted");
  else
    gtk_widget_remove_css_class (self->toolbar, "lifted");
}

static GtkWidget *
page (CamSettings *self, int index)
{
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new (_("Settings"));
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = spongy_scroll_view_new ();
  /* Roboto's ascent (1900 / 2048): the label's top above its baseline */
  int ascent = (int) (TITLE_SIZE * 1900 / 2048.0 + 0.5);

  /* the expanded part of the app bar, under the 64 dp toolbar */
  gtk_widget_set_size_request (header, -1, APP_BAR_H - TOOLBAR_H);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_valign (title, GTK_ALIGN_START);
  gtk_widget_set_margin_start (title, 24);
  gtk_widget_set_margin_top (title, APP_BAR_H - TOOLBAR_H - TITLE_BASELINE - ascent);
  gtk_widget_add_css_class (title, "cam-settings-large-title");
  gtk_box_append (GTK_BOX (header), title);
  gtk_box_append (GTK_BOX (content), header);
  gtk_box_append (GTK_BOX (content), list);
  gtk_widget_set_margin_bottom (list, 16);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroller), content);
  gtk_widget_set_vexpand (scroller, TRUE);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (scroller)), "value-changed",
                    G_CALLBACK (on_scrolled), self);
  self->scrollers[index] = scroller;
  g_object_set_data (G_OBJECT (scroller), "cam-list", list);
  return scroller;
}

static void
show_page (CamSettings *self, int index)
{
  self->page = index;
  gtk_stack_set_visible_child (GTK_STACK (self->stack), self->pages[index]);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scrollers[index]), 0);
  on_scrolled (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scrollers[index])), self);
}

static void
on_gestures (NeoPressable *pressable, CamSettings *self)
{
  show_page (self, 1);
}

static void
go_back (CamSettings *self)
{
  if (self->page > 0)
    show_page (self, 0);
  else
    g_signal_emit (self, signals[SIG_CLOSED], 0);
}

static void
on_up (NeoPressable *button, CamSettings *self)
{
  go_back (self);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CamSettings *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  go_back (self);
  return TRUE;
}

static void
on_changed (GSettings *settings, const char *key, CamSettings *self)
{
  for (guint i = 0; i < self->lists->len; i++)
    {
      ListRow *list = self->lists->pdata[i];

      if (g_str_equal (list->key, key))
        show_list (self, list);
    }
  if (g_str_equal (key, "volume-buttons-action"))
    {
      g_autofree char *action = g_settings_get_string (settings, key);

      /* "Invert volume buttons" means something only for Zoom */
      gtk_widget_set_sensitive (self->invert_row, g_str_equal (action, "zoom"));
    }
  if (g_str_equal (key, "save-location"))
    {
      g_autofree char *value = g_settings_get_string (settings, key);
      NeoSwitch *toggle = g_object_get_data (G_OBJECT (self->location), "cam-switch");

      if (neo_switch_get_active (toggle) != g_str_equal (value, "on"))
        neo_switch_set_active (toggle, g_str_equal (value, "on"));
    }
}

static void
on_location_switch (NeoSwitch *toggle, GParamSpec *pspec, CamSettings *self)
{
  g_autofree char *value = g_settings_get_string (self->settings, "save-location");
  gboolean on = neo_switch_get_active (toggle);

  /* a three-way key (unset asks once): the switch writes on or off; the window asks GeoClue and turns it back off
   * if it refuses */
  if (on != g_str_equal (value, "on"))
    g_settings_set_string (self->settings, "save-location", on ? "on" : "off");
}

static void
cam_settings_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                      int *min_base, int *nat_base)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);

  gtk_widget_measure (child, orientation, for_size, min, nat, NULL, NULL);
}

static void
cam_settings_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (gtk_widget_get_first_child (widget), width, height, -1, NULL);
}

static void
cam_settings_dispose (GObject *object)
{
  CamSettings *self = CAM_SETTINGS (object);
  GtkWidget *child;

  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->lists, g_ptr_array_unref);
  G_OBJECT_CLASS (cam_settings_parent_class)->dispose (object);
}

static void
cam_settings_class_init (CamSettingsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_settings_dispose;
  widget_class->measure = cam_settings_measure;
  widget_class->size_allocate = cam_settings_size_allocate;
  signals[SIG_CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                      G_TYPE_NONE, 0);
}

static void
cam_settings_init (CamSettings *self)
{
  self->lists = g_ptr_array_new_with_free_func (g_free);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

static void
build (CamSettings *self)
{
  static const char *const mirror_values[] = { "off", "on", "front" };
  static const char *const action_values[] = { "shutter", "focus", "mic-mute", "zoom", "default", "nothing" };
  static const char *const af_values[] = { "auto", "tap", "manual" };
  static const char *mirror_labels[3], *action_labels[6], *af_labels[3];
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_overlay_new ();
  GtkWidget *up = neo_icon_button_new ("cam-back-symbolic");
  GtkWidget *list;
  GtkEventController *keys = gtk_event_controller_key_new ();

  mirror_labels[0] = C_("mirror", "Off");
  mirror_labels[1] = C_("mirror", "On");
  mirror_labels[2] = _("Front facing camera only");
  af_labels[0] = _("Automatic");
  af_labels[1] = _("Tap to focus only");
  af_labels[2] = _("Manual");
  action_labels[0] = _("Shutter");
  action_labels[1] = _("Focus");
  action_labels[2] = _("Microphone mute");
  action_labels[3] = _("Zoom");
  /* Android's default: the system's volume change */
  action_labels[4] = _("System default");
  action_labels[5] = _("Nothing");

  gtk_widget_add_css_class (box, "cam-settings");
  /* the toolbar: Navigate up 4 dp in, and the collapsed title */
  self->toolbar = bar;
  gtk_widget_set_size_request (bar, -1, TOOLBAR_H);
  gtk_widget_add_css_class (bar, "cam-settings-toolbar");
  gtk_widget_set_halign (up, GTK_ALIGN_START);
  gtk_widget_set_valign (up, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (up, 4);
  gtk_widget_set_tooltip_text (up, _("Navigate up"));
  /* the shell's back gesture presses a "Back" button at the window's top over the accessibility bus */
  gtk_accessible_update_property (GTK_ACCESSIBLE (up), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  g_signal_connect (up, "clicked", G_CALLBACK (on_up), self);
  self->collapsed = gtk_label_new (_("Settings"));
  gtk_widget_add_css_class (self->collapsed, "cam-settings-collapsed-title");
  gtk_widget_set_halign (self->collapsed, GTK_ALIGN_START);
  gtk_widget_set_valign (self->collapsed, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (self->collapsed, 72);
  gtk_widget_set_opacity (self->collapsed, 0);
  gtk_widget_set_can_target (self->collapsed, FALSE);
  gtk_overlay_set_child (GTK_OVERLAY (bar), gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_overlay_add_overlay (GTK_OVERLAY (bar), self->collapsed);
  gtk_overlay_add_overlay (GTK_OVERLAY (bar), up);
  gtk_box_append (GTK_BOX (box), bar);

  self->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (self->stack), 150);
  gtk_widget_set_vexpand (self->stack, TRUE);
  gtk_box_append (GTK_BOX (box), self->stack);

  /* the root page */
  self->pages[0] = page (self, 0);
  gtk_stack_add_child (GTK_STACK (self->stack), self->pages[0]);
  list = g_object_get_data (G_OBJECT (self->pages[0]), "cam-list");
  category (list, _("General"));
  self->bright = switch_row (self, list, "bright-screen", _("Bright screen"), _("Locks brightness to 100%"));
  self->location = switch_row (self, list, NULL, _("Save location data"), _("Include GPS location in metadata"));
  {
    g_autofree char *value = g_settings_get_string (self->settings, "save-location");
    NeoSwitch *toggle = g_object_get_data (G_OBJECT (self->location), "cam-switch");

    neo_switch_set_active (toggle, g_str_equal (value, "on"));
    g_signal_connect (toggle, "notify::active", G_CALLBACK (on_location_switch), self);
  }
  switch_row (self, list, "shutter-sound", _("Shutter sound"), _("Play a sound on capture"));
  self->leveler = switch_row (self, list, "leveler", _("Leveler"), _("Indicator that shows device orientation"));
  self->gestures = row (list, _("Gestures"), NULL, NULL, NULL);
  g_signal_connect (self->gestures, "clicked", G_CALLBACK (on_gestures), self);
  self->photos = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (list), self->photos);
  category (self->photos, _("Photos"));
  switch_row (self, self->photos, "photo-ffc-mirror", _("Mirror front camera"),
              _("Save front camera photos as they appear in the preview"));
  self->focus = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (list), self->focus);
  category (self->focus, C_("settings", "Focus"));
  list_row (self, self->focus, "af-mode", _("Autofocus"), af_labels, af_values, 3);
  self->video = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (list), self->video);
  category (self->video, C_("settings", "Video"));
  list_row (self, self->video, "video-mirror-mode", _("Enable video mirroring"), mirror_labels, mirror_values, 3);

  /* Gestures: one category per key the phone has; a GNOME phone has the volume keys */
  self->pages[1] = page (self, 1);
  gtk_stack_add_child (GTK_STACK (self->stack), self->pages[1]);
  list = g_object_get_data (G_OBJECT (self->pages[1]), "cam-list");
  category (list, _("Volume buttons"));
  list_row (self, list, "volume-buttons-action", _("Volume buttons action"), action_labels, action_values, 6);
  self->invert_row = switch_row (self, list, "volume-buttons-invert", _("Invert volume buttons"),
                                 _("Invert volume up and down behavior"));
  on_changed (self->settings, "volume-buttons-action", self);
  g_signal_connect (self->settings, "changed", G_CALLBACK (on_changed), self);

  gtk_widget_set_parent (box, GTK_WIDGET (self));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

GtkWidget *
cam_settings_new (GSettings *settings, CamSettingsRows rows)
{
  CamSettings *self = g_object_new (CAM_TYPE_SETTINGS, NULL);

  self->settings = g_object_ref (settings);
  build (self);
  cam_settings_set_rows (self, rows);
  return GTK_WIDGET (self);
}

void
cam_settings_set_rows (CamSettings *self, CamSettingsRows rows)
{
  self->rows = rows;
  gtk_widget_set_visible (self->bright, rows & CAM_SETTINGS_BRIGHT);
  gtk_widget_set_visible (self->location, rows & CAM_SETTINGS_LOCATION);
  gtk_widget_set_visible (self->leveler, rows & CAM_SETTINGS_LEVELER);
  gtk_widget_set_visible (self->gestures, rows & CAM_SETTINGS_GESTURES);
  gtk_widget_set_visible (self->photos, rows & CAM_SETTINGS_FRONT);
  gtk_widget_set_visible (self->video, rows & CAM_SETTINGS_VIDEO);
  gtk_widget_set_visible (self->focus, rows & CAM_SETTINGS_FOCUS);
}

void
cam_settings_reset (CamSettings *self)
{
  show_page (self, 0);
}
