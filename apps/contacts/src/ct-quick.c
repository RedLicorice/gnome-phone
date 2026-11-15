/* QuickContact (ct-quick.h, spec §3).
 *
 * How it is built. Android's MultiShrinkScroller moves three things with one gesture: the transparent space above
 * the sheet, the header's height (from a square as wide as the screen down to the 56 dp toolbar), and the cards'
 * own scroll. Opened from the list it starts fully expanded, so there is no transparent space, and what is left is
 * a header that shrinks while the cards rise under it, then stays 56 dp tall while the cards scroll on beneath.
 *
 * That is one scroll view here (SpongyScrollView, the fling and the stretch of every other list), whose content
 * (QcContent) is the cards below a header-sized gap, with the header (QcHeader) kept at the top of the view and as
 * tall as the gap still showing: max_h − offset, never under min_h. The header is drawn after the cards and picked
 * before them, so the cards pass under it; a drag that starts on the header still scrolls, because the scroll
 * view takes drags in the capture phase.
 *
 * The page is translucent (Theme.QuickContact): the list stays drawn under the 0xC8 scrim, and the sheet animates
 * itself, rising from the bottom in 300 ms (linear_out_slow_in) and falling off it in 250 ms on Back.
 *
 * Left out, for want of anything on this phone to back them:
 *   - the menu's Link and View linked contacts (EDS does not aggregate contacts, ct-store.h), Create shortcut (the
 *     launcher takes no pinned shortcuts) and Set ringtone (no per-contact ringtones in the Dialer);
 *   - a phone entry's third button (call with a note, video call) and an address's Directions (no navigation app);
 *   - tapping an event (the Calendar has no "show this day" entry point) or a relation (a contact search by name);
 *   - the Palette swatch of a photo: a photo contact takes the default #607d8b, as a photo without a vibrant
 *     swatch does on Android (§3.4, the measured contacts-card);
 *   - pulling the sheet down to dismiss it, and the landscape two-panel layout. Back closes it. */
#include "ct-quick.h"
#include "ct-avatar.h"
#include "ct-dialogs.h"
#include "ct-minerva.h"
#include "ct-page.h"
#include "ct-popup.h"
#include "ct-settings.h"
#include "ct-ui.h"
#include "ct-window.h"

#include "phone-number.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

typedef struct _Quick Quick;

struct _Quick {
  GtkWidget *page, *window;
  CtStore *store;
  GSettings *settings;
  char *uid;
  gboolean profile;             /* "My info" */

  EContact *contact;            /* the card shown (a ref) */
  char *name, *phonetic;
  GdkTexture *photo;
  guint32 header_tint;          /* the header's colour behind the silhouette, and the photo's tint when collapsed */
  guint32 primary;              /* the kind icons */
  guint32 title_color;          /* the card titles */

  GtkWidget *stage, *scrim, *scroll, *content, *header, *cards;
  GtkWidget *actions, *star, *edit, *overflow;
  GtkWidget *minerva;           /* the Minerva card (ct-minerva.h, a ref kept across rebuilds), NULL without one */
  double max_h, min_h;          /* the header's heights */
  double view_h;                /* the sheet's height */
  SpongyInterp title_curve;

  double sheet;                 /* how far the sheet is below its place: 0 in place … 1 off the bottom */
  NeoAnim *sheet_anim, *scrim_anim, *expand_anim;
  gboolean closing;

  double press_x, press_y;      /* where the last press on an entry was, for its context menu */
};

static void rebuild (Quick *quick);
static void quick_close (Quick *quick);

/* the header's height now: what is left of the header-sized gap at the top of the content */
static double
header_height (Quick *quick)
{
  double offset = quick->scroll ? spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (quick->scroll)) : 0;

  return CLAMP (quick->max_h - MAX (offset, 0), quick->min_h, quick->max_h);
}

/* ===== QcHeader: the photo or the tinted silhouette, the gradients, the title, the toolbar ===================== */

#define QC_TYPE_HEADER (qc_header_get_type ())
G_DECLARE_FINAL_TYPE (QcHeader, qc_header, QC, HEADER, GtkWidget)

struct _QcHeader {
  GtkWidget parent;
  Quick *quick;
};

G_DEFINE_FINAL_TYPE (QcHeader, qc_header, GTK_TYPE_WIDGET)

static void
qc_header_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                   int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

/* the toolbar: its buttons at the end of the 56 dp bar at the top */
static void
qc_header_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  Quick *quick = QC_HEADER (widget)->quick;
  int natural;

  gtk_widget_measure (quick->actions, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &natural, NULL, NULL);
  gtk_widget_allocate (quick->actions, natural, (int) quick->min_h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) (width - natural), 0)));
}

/* the header takes touches only where it shows; under it, the cards do */
static gboolean
qc_header_contains (GtkWidget *widget, double x, double y)
{
  Quick *quick = QC_HEADER (widget)->quick;

  return x >= 0 && x < gtk_widget_get_width (widget) && y >= 0 && y < header_height (quick);
}

/* MultiShrinkScroller.updatePhotoTintAndDropShadow for a photo (§3.4): below half the maximum height the photo
 * loses its saturation, washes towards white and takes the tint by a multiply, all by r^1.5 */
static void
push_photo_tint (GtkSnapshot *snapshot, Quick *quick, double height)
{
  double intermediate = quick->max_h * CT_QC_COLOR_BLEND_START;
  double r = height >= intermediate ? 0 : (intermediate - height) / (intermediate - quick->min_h);
  double alpha = 1 - MIN (pow (r, 1.5) * 2, 1);
  double tint = MIN (pow (r, 1.5) * 3, 1);
  GdkRGBA t = ct_rgba (quick->header_tint);
  /* ColorMatrix.setSaturation's luminance weights */
  double lr = 0.213 * (1 - alpha), lg = 0.715 * (1 - alpha), lb = 0.072 * (1 - alpha);
  double k[3] = { 1 - tint + tint * t.red, 1 - tint + tint * t.green, 1 - tint + tint * t.blue };
  double s[3][3] = {
    { lr + alpha, lg, lb },
    { lr, lg + alpha, lb },
    { lr, lg, lb + alpha },
  };
  graphene_matrix_t matrix;
  graphene_vec4_t offset;
  float m[16] = { 0 };

  /* out = K · (alpha · S · in + (1 − alpha) · white): GTK's matrix takes the column for each input channel */
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++)
      m[col * 4 + row] = (float) (k[row] * alpha * s[row][col]);
  m[15] = 1;
  graphene_matrix_init_from_float (&matrix, m);
  graphene_vec4_init (&offset, (float) (k[0] * (1 - alpha)), (float) (k[1] * (1 - alpha)),
                      (float) (k[2] * (1 - alpha)), 0);
  gtk_snapshot_push_color_matrix (snapshot, &matrix, &offset);
}

/* the gradients' darkness: a photo's, fading as the tint comes; none over a silhouette */
static double
gradient_alpha (Quick *quick, double height)
{
  double intermediate = quick->max_h * CT_QC_COLOR_BLEND_START;
  double r = height >= intermediate ? 0 : (intermediate - height) / (intermediate - quick->min_h);

  return quick->photo ? 1 - MIN (pow (r, 1.5) * 2, 1) : 0;
}

/* for a silhouette: how far it has blended into the tint, 0 at the full height, 0.8 at the intermediate one */
static double
silhouette_blend (Quick *quick, double ratio)
{
  double slowing = 1 / (1 - pow (1 - CT_QC_TILE_INTERMEDIATE_ALPHA, 1 / CT_QC_TILE_EXPONENT));
  double linear = MAX (1 - (1 - ratio) / CT_QC_INTERMEDIATE_RATIO / slowing, 0);

  return 1 - pow (linear, CT_QC_TILE_EXPONENT);
}

static void
append_gradient (GtkSnapshot *snapshot, double y, double width, double height, double alpha, gboolean dark_bottom)
{
  GdkRGBA dark = ct_rgba (CT_QC_GRADIENT_END), clear = { 0, 0, 0, 0 };
  GskColorStop stops[2];

  dark.alpha *= alpha;
  stops[0] = (GskColorStop) { 0, dark_bottom ? clear : dark };
  stops[1] = (GskColorStop) { 1, dark_bottom ? dark : clear };
  gtk_snapshot_append_linear_gradient (snapshot, &GRAPHENE_RECT_INIT (0, (float) y, (float) width, (float) height),
                                       &GRAPHENE_POINT_INIT (0, (float) y),
                                       &GRAPHENE_POINT_INIT (0, (float) (y + height)), stops, 2);
}

static void
qc_header_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  Quick *quick = QC_HEADER (widget)->quick;
  double width = gtk_widget_get_width (widget);
  double height = header_height (quick);
  double range = MAX (quick->max_h - quick->min_h, 1);
  double ratio = (height - quick->min_h) / range;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, (float) width, (float) height);
  TextStyle title_style = CT_TEXT (CT_QC_TITLE_MAX_DP, CT_WEIGHT_REGULAR);
  TextStyle phonetic_style = CT_TEXT (CT_QC_PHONETIC_MAX_DP, CT_WEIGHT_REGULAR);
  g_autoptr(PangoLayout) title = NULL;
  g_autoptr(PangoLayout) phonetic = NULL;
  GdkRGBA white = ct_rgba (0xFFFFFFFFu);
  double box, block, b, scale, bottom, top, alpha;

  /* the toolbar's 4.5 dp elevation once the header is down to its least (drawn under it, over the cards) */
  if (height <= quick->min_h + 0.5)
    {
      GdkRGBA shade = { 0, 0, 0, 0.3f }, clear = { 0, 0, 0, 0 };

      gtk_snapshot_append_linear_gradient (snapshot,
                                           &GRAPHENE_RECT_INIT (0, (float) height, (float) width,
                                                                CT_QC_TOOLBAR_ELEVATION_DP),
                                           &GRAPHENE_POINT_INIT (0, (float) height),
                                           &GRAPHENE_POINT_INIT (0, (float) (height + CT_QC_TOOLBAR_ELEVATION_DP)),
                                           (GskColorStop[]) { { 0, shade }, { 1, clear } }, 2);
    }

  gtk_snapshot_push_clip (snapshot, &bounds);
  if (quick->photo)
    {
      gboolean tinted = height < quick->max_h * CT_QC_COLOR_BLEND_START;

      if (tinted)
        push_photo_tint (snapshot, quick, height);
      ct_avatar_header_snapshot (snapshot, &bounds, quick->header_tint, quick->photo);
      if (tinted)
        gtk_snapshot_pop (snapshot);
    }
  else
    {
      GdkRGBA tint = ct_rgba (quick->header_tint);

      gtk_snapshot_append_color (snapshot, &tint, &bounds);
      gtk_snapshot_push_opacity (snapshot, 1 - silhouette_blend (quick, ratio));
      ct_avatar_header_snapshot (snapshot, &bounds, quick->header_tint, NULL);
      gtk_snapshot_pop (snapshot);
    }

  /* the title (32 dp) and the phonetic name (28 dp) under it, scaled down to the toolbar title's 20 dp along
   * (0.16, 0.4, 0.2, 1), their start edge 16 dp in; full size, their bottom is 16 dp above the header's, and at
   * the least they are centred in the 56 dp bar (updateHeaderTextSizeAndMargin) */
  title = text_layout (widget, title_style, quick->name, width - 2 * CT_QC_TITLE_MARGIN_H_DP);
  box = text_box_height (title, title_style);
  block = box;
  if (quick->phonetic)
    {
      phonetic = text_layout (widget, phonetic_style, quick->phonetic, width - 2 * CT_QC_TITLE_MARGIN_H_DP);
      block += text_box_height (phonetic, phonetic_style);
    }
  b = spongy_interp_get (&quick->title_curve, (float) CLAMP (ratio, 0, 1));
  scale = (CT_QC_TITLE_PLACEHOLDER_DP + (CT_QC_TITLE_MAX_DP - CT_QC_TITLE_PLACEHOLDER_DP) * b) / CT_QC_TITLE_MAX_DP;
  bottom = (quick->min_h - block) / 2 + (CT_QC_TITLE_MAX_MARGIN_DP - (quick->min_h - block) / 2) * b;
  top = height - bottom - block;

  /* the gradients behind the title and the toolbar, over a photo */
  alpha = gradient_alpha (quick, height);
  if (alpha > 0)
    {
      double gradient = block * CT_QC_GRADIENT_TITLE_FACTOR;

      append_gradient (snapshot, height - gradient, width, gradient, alpha, TRUE);
      append_gradient (snapshot, 0, width, quick->min_h, alpha, FALSE);
    }

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (CT_QC_TITLE_MARGIN_H_DP, (float) (top + block / 2)));
  gtk_snapshot_scale (snapshot, (float) scale, (float) scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, (float) (-block / 2)));
  text_draw (snapshot, title, title_style, 0, 0, &white);
  if (phonetic)
    text_draw (snapshot, phonetic, phonetic_style, 0, box, &white);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);

  gtk_widget_snapshot_child (widget, quick->actions, snapshot);
}

static void
qc_header_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (qc_header_parent_class)->dispose (object);
}

static void
qc_header_class_init (QcHeaderClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = qc_header_dispose;
  GTK_WIDGET_CLASS (klass)->measure = qc_header_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = qc_header_size_allocate;
  GTK_WIDGET_CLASS (klass)->contains = qc_header_contains;
  GTK_WIDGET_CLASS (klass)->snapshot = qc_header_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-qc-header");
}

static void
qc_header_init (QcHeader *self)
{
}

/* ===== QcContent: the cards under a header-sized gap, the header pinned over them ============================== */

#define QC_TYPE_CONTENT (qc_content_get_type ())
G_DECLARE_FINAL_TYPE (QcContent, qc_content, QC, CONTENT, GtkWidget)

struct _QcContent {
  GtkWidget parent;
  Quick *quick;
};

G_DEFINE_FINAL_TYPE (QcContent, qc_content, GTK_TYPE_WIDGET)

/* the maximum height: the header is square with the screen's width, the status bar's inset included
 * (mMaximumHeaderHeight = the photo container's width); the window here starts under the status bar */
static double
max_header_height (double width)
{
  return MAX (width - CT_STATUSBAR_H_PX / (double) CT_PX_PER_DP, CT_TOOLBAR_H_DP);
}

static void
qc_content_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                    int *minimum_baseline, int *natural_baseline)
{
  Quick *quick = QC_CONTENT (widget)->quick;

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      gtk_widget_measure (quick->cards, orientation, -1, minimum, natural, NULL, NULL);
      return;
    }
  /* the cards are at least as tall as the view under the collapsed header, so the header can always shrink all
   * the way (getMaximumScrollUpwards) */
  {
    int cards_min, cards_nat;
    double view = quick->view_h;
    double gap = max_header_height (for_size > 0 ? for_size : 0);

    gtk_widget_measure (quick->cards, orientation, for_size, &cards_min, &cards_nat, NULL, NULL);
    *minimum = *natural = (int) ceil (gap + MAX (cards_nat, view - quick->min_h));
  }
}

static GtkSizeRequestMode
qc_content_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
qc_content_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  Quick *quick = QC_CONTENT (widget)->quick;
  double offset = MAX (spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (quick->scroll)), 0);

  quick->max_h = max_header_height (width);
  gtk_widget_allocate (quick->cards, width, (int) (height - quick->max_h), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (float) quick->max_h)));
  /* the header at the top of the view, wherever the content is scrolled to */
  gtk_widget_allocate (quick->header, width, (int) ceil (quick->max_h), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (float) offset)));
}

static void
qc_content_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (qc_content_parent_class)->dispose (object);
}

static void
qc_content_class_init (QcContentClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = qc_content_dispose;
  GTK_WIDGET_CLASS (klass)->measure = qc_content_measure;
  GTK_WIDGET_CLASS (klass)->get_request_mode = qc_content_get_request_mode;
  GTK_WIDGET_CLASS (klass)->size_allocate = qc_content_size_allocate;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-qc-content");
}

static void
qc_content_init (QcContent *self)
{
}

/* ===== QcStage: the scrim and the sheet sliding over it ======================================================== */

#define QC_TYPE_STAGE (qc_stage_get_type ())
G_DECLARE_FINAL_TYPE (QcStage, qc_stage, QC, STAGE, GtkWidget)

struct _QcStage {
  GtkWidget parent;
  Quick *quick;
};

G_DEFINE_FINAL_TYPE (QcStage, qc_stage, GTK_TYPE_WIDGET)

static void
qc_stage_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
qc_stage_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  Quick *quick = QC_STAGE (widget)->quick;

  /* the cards' least height follows the sheet's (qc_content_measure): measured again before it is placed */
  if (height != quick->view_h)
    {
      quick->view_h = height;
      gtk_widget_queue_resize (quick->content);
    }
  gtk_widget_allocate (quick->scrim, width, height, -1, NULL);
  gtk_widget_allocate (quick->scroll, width, height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (float) round (quick->sheet * height))));
}

static void
qc_stage_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (qc_stage_parent_class)->dispose (object);
}

static void
qc_stage_class_init (QcStageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = qc_stage_dispose;
  GTK_WIDGET_CLASS (klass)->measure = qc_stage_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = qc_stage_size_allocate;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-qc-stage");
  /* QuickContact has no Back button: as a dialog, the shell's back gesture sends it Escape (extension/a11y.js) */
  gtk_widget_class_set_accessible_role (GTK_WIDGET_CLASS (klass), GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
qc_stage_init (QcStage *self)
{
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

/* ===== the entries ============================================================================================= */

typedef struct {
  const char *icon;             /* the kind's icon, NULL for none (the About card) */
  gboolean icon_shown;          /* only the first entry of a kind shows it (the others keep its space) */
  const char *header, *sub, *text;
  char *uri;                    /* what a tap opens, NULL for nothing */
  const char *alt_icon;         /* the 48 dp button at the end (a number's SMS) */
  char *alt_uri, *alt_name;
  char *copy;                   /* what a long press copies */
  CtKind kind;                  /* for Set default: the kind and the row's index */
  int index;
  int n_of_kind;
  gboolean add;                 /* "Add phone number" / "Add email": grey, opens the editor */
  gboolean first_in_untitled;   /* the first entry of a card without a title: 2 dp more above */
  gboolean later_of_kind;       /* a later entry of the same kind: 6 dp above instead of 13 */
} Entry;

static void
entry_clear (Entry *entry)
{
  g_free (entry->uri);
  g_free (entry->alt_uri);
  g_free (entry->alt_name);
  g_free (entry->copy);
}

static void
open_or_toast (Quick *quick, const char *uri)
{
  if (!ct_open_uri (quick->window, uri))
    ct_window_toast (quick->window, _("No app was found to handle this action."));
}

static void
on_entry_clicked (NeoPressable *row, Quick *quick)
{
  const char *uri = g_object_get_data (G_OBJECT (row), "ct-uri");

  if (g_object_get_data (G_OBJECT (row), "ct-add"))
    ct_window_edit_contact (quick->window, quick->profile ? CT_PROFILE_UID : quick->uid);
  else if (uri)
    open_or_toast (quick, uri);
}

static void
on_alt_clicked (NeoPressable *button, Quick *quick)
{
  open_or_toast (quick, g_object_get_data (G_OBJECT (button), "ct-uri"));
}

/* "Set default" / "Clear default": the row becomes the kind's PREF one, or none is */
static void
set_default (Quick *quick, CtKind kind, int index, gboolean make_default)
{
  g_autoptr(EContact) copy = e_contact_duplicate (quick->contact);
  g_autoptr(GPtrArray) items = ct_contact_get_items (copy, kind);

  for (guint i = 0; i < items->len; i++)
    ((CtItem *) items->pdata[i])->primary = make_default && (int) i == index;
  ct_contact_set_items (copy, kind, items);
  if (quick->profile)
    ct_store_save_profile (quick->store, copy, NULL, NULL);
  else
    {
      g_autoptr(GPtrArray) contacts = g_ptr_array_new_with_free_func (g_object_unref);

      g_ptr_array_add (contacts, g_object_ref (copy));
      ct_store_modify (quick->store, contacts, NULL, NULL);
    }
}

typedef struct {
  Quick *quick;
  GtkWidget *row;
  int actions[2];
} Context;

enum { CONTEXT_COPY, CONTEXT_SET_DEFAULT, CONTEXT_CLEAR_DEFAULT };

static void
on_context_chosen (int index, gpointer data)
{
  Context *context = data;
  Quick *quick = context->quick;
  GtkWidget *row = context->row;

  switch (context->actions[index])
    {
    case CONTEXT_COPY:
      ct_copy_text (row, g_object_get_data (G_OBJECT (row), "ct-copy"));
      ct_window_toast (quick->window, _("Text copied"));
      break;
    case CONTEXT_SET_DEFAULT:
    case CONTEXT_CLEAR_DEFAULT:
      set_default (quick, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-kind")),
                   GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-index")),
                   context->actions[index] == CONTEXT_SET_DEFAULT);
      break;
    }
}

/* the context menu (§3.7) at the finger: Copy to clipboard, and for a number or an address of several of its kind,
 * Set default or Clear default ("Call via" needs several calling accounts, which this phone does not have) */
static void
on_entry_long_pressed (NeoPressable *row, Quick *quick)
{
  static Context context;
  const char *labels[2];
  int n = 0;
  int n_of_kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-n-of-kind"));

  if (!g_object_get_data (G_OBJECT (row), "ct-copy"))
    return;
  context.quick = quick;
  context.row = GTK_WIDGET (row);
  labels[n] = _("Copy to clipboard");
  context.actions[n++] = CONTEXT_COPY;
  if (n_of_kind > 1)
    {
      gboolean is_default = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-default"));

      labels[n] = is_default ? _("Clear default") : _("Set default");
      context.actions[n++] = is_default ? CONTEXT_CLEAR_DEFAULT : CONTEXT_SET_DEFAULT;
    }
  ct_popup_show (GTK_WIDGET (row), labels, n, CT_POPUP_AT, quick->press_x, quick->press_y, 0, on_context_chosen,
                 &context);
}

/* where a press began, for the context menu; it takes nothing from the row's own press */
static void
on_entry_pressed (GtkGestureClick *gesture, int n, double x, double y, Quick *quick)
{
  quick->press_x = x;
  quick->press_y = y;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
}

static GtkWidget *
entry_label (const char *text, const char *css_class, double size)
{
  GtkWidget *label = ct_label (text, css_class, size);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_hexpand (label, TRUE);
  return label;
}

/* expanding_entry_card_item.xml: the kind icon (24 dp, 12 dp down, 27 dp before the text), the header (16 sp), the
 * sub-header and the text (14 sp), the 48 dp button at the end; 20 dp in, 8 dp from the end, 13 dp above (6 for a
 * later entry of the same kind) and 16 dp below */
static GtkWidget *
entry_row (Quick *quick, const Entry *entry, gboolean card_has_icons)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = neo_pressable_new (box);
  GtkGesture *press = gtk_gesture_click_new ();
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_FRAMEWORK);
  gboolean header_only = !entry->sub && !entry->text;
  double top = entry->later_of_kind && !header_only ? CT_QC_NO_ICON_MARGIN_TOP_DP : CT_QC_ENTRY_PAD_TOP_DP;
  GtkWidget *label;

  if (entry->first_in_untitled)
    top += CT_QC_NULL_TITLE_EXTRA_DP;
  gtk_widget_add_css_class (row, "ct-qc-entry");
  if (entry->add)
    gtk_widget_add_css_class (row, "add");
  gtk_widget_set_margin_start (box, (int) CT_QC_ENTRY_PAD_START_DP);
  gtk_widget_set_margin_end (box, (int) CT_QC_ENTRY_PAD_END_DP);
  gtk_widget_set_margin_top (box, (int) round (top));
  gtk_widget_set_margin_bottom (box, (int) CT_QC_ENTRY_PAD_BOTTOM_DP + (header_only ? 2 : 0));
  if (card_has_icons)
    {
      GtkWidget *icon = ct_tinted_icon_new (entry->icon ? entry->icon : "ct-phone-symbolic", CT_QC_ICON_DP,
                                            entry->add ? CT_C_QC_NO_DATA : quick->primary);

      gtk_widget_set_valign (icon, GTK_ALIGN_START);
      gtk_widget_set_margin_top (icon, (int) CT_QC_ICON_MARGIN_TOP_DP);
      gtk_widget_set_margin_end (icon, (int) CT_QC_IMAGE_SPACING_DP);
      gtk_widget_set_opacity (icon, entry->icon_shown ? 1 : 0);
      gtk_box_append (GTK_BOX (box), icon);
    }
  label = entry_label (entry->header, "ct-qc-entry-header", CT_QC_HEADER_SP);
  /* a header-only entry sits 6 dp lower (expanding_entry_card_item's header-only margins) */
  if (header_only)
    gtk_widget_set_margin_top (label, (int) CT_QC_NO_ICON_MARGIN_TOP_DP);
  gtk_box_append (GTK_BOX (lines), label);
  if (entry->sub)
    gtk_box_append (GTK_BOX (lines), entry_label (entry->sub, "ct-qc-entry-sub", CT_QC_SUB_SP));
  if (entry->text)
    gtk_box_append (GTK_BOX (lines), entry_label (entry->text, "ct-qc-entry-text", CT_QC_SUB_SP));
  gtk_widget_set_hexpand (lines, TRUE);
  gtk_widget_set_valign (lines, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), lines);
  if (entry->alt_icon)
    {
      GtkWidget *icon = ct_tinted_icon_new (entry->alt_icon, CT_QC_ICON_DP, quick->primary);
      GtkWidget *alt = neo_pressable_new (icon);

      gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      gtk_widget_set_size_request (alt, (int) CT_QC_CLICKABLE_ICON_DP, (int) CT_QC_CLICKABLE_ICON_DP);
      gtk_widget_set_valign (alt, GTK_ALIGN_START);
      neo_pressable_set_ripple (NEO_PRESSABLE (alt), NEO_RIPPLE_CIRCLE, hypot (CT_QC_CLICKABLE_ICON_DP,
                                                                                CT_QC_CLICKABLE_ICON_DP) / 2);
      neo_pressable_set_ripple_color (NEO_PRESSABLE (alt), &ripple);
      gtk_accessible_update_property (GTK_ACCESSIBLE (alt), GTK_ACCESSIBLE_PROPERTY_LABEL, entry->alt_name, -1);
      g_object_set_data_full (G_OBJECT (alt), "ct-uri", g_strdup (entry->alt_uri), g_free);
      g_signal_connect (alt, "clicked", G_CALLBACK (on_alt_clicked), quick);
      gtk_box_append (GTK_BOX (box), alt);
    }

  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, entry->header, -1);
  g_object_set_data_full (G_OBJECT (row), "ct-uri", g_strdup (entry->uri), g_free);
  g_object_set_data_full (G_OBJECT (row), "ct-copy", g_strdup (entry->copy), g_free);
  g_object_set_data (G_OBJECT (row), "ct-add", GINT_TO_POINTER (entry->add));
  g_object_set_data (G_OBJECT (row), "ct-kind", GINT_TO_POINTER (entry->kind));
  g_object_set_data (G_OBJECT (row), "ct-index", GINT_TO_POINTER (entry->index));
  g_object_set_data (G_OBJECT (row), "ct-n-of-kind", GINT_TO_POINTER (entry->n_of_kind));
  g_signal_connect (row, "clicked", G_CALLBACK (on_entry_clicked), quick);
  g_signal_connect (row, "long-pressed", G_CALLBACK (on_entry_long_pressed), quick);
  g_signal_connect (press, "pressed", G_CALLBACK (on_entry_pressed), quick);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (press), GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (press));
  return row;
}

/* a card (CardView: 28 dp corners, #1b1b1b, 8 dp from the sides, 12 dp below) */
static GtkWidget *
card_new (Quick *quick, const char *title)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (card, "ct-qc-card");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
  if (title)
    {
      GtkWidget *label = ct_label (title, "ct-qc-card-title", CT_QC_CARD_TITLE_SP);
      PangoAttrList *attrs = pango_attr_list_new ();
      GdkRGBA color = ct_rgba (quick->title_color);

      /* the theme colour, which no style sheet knows */
      pango_attr_list_insert (attrs, pango_attr_foreground_new ((guint16) (color.red * 65535),
                                                                (guint16) (color.green * 65535),
                                                                (guint16) (color.blue * 65535)));
      gtk_label_set_attributes (GTK_LABEL (label), attrs);
      pango_attr_list_unref (attrs);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (card), label);
    }
  return card;
}

/* the 1 dp separator between kinds (transparent: divider_line_color_light) */
static void
card_separator (GtkWidget *card)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (line, "ct-qc-separator");
  gtk_box_append (GTK_BOX (card), line);
}

/* the rows of @kind, the default (PREF) one first, as QuickContact sorts super-primary first; each item keeps its
 * vCard index for Set default */
static GPtrArray *
sorted_items (EContact *contact, CtKind kind, GArray **indexes)
{
  GPtrArray *items = ct_contact_get_items (contact, kind);
  GPtrArray *sorted = g_ptr_array_new ();

  *indexes = g_array_new (FALSE, FALSE, sizeof (int));
  for (int pass = 0; pass < 2; pass++)
    for (guint i = 0; i < items->len; i++)
      {
        CtItem *item = items->pdata[i];
        int index = (int) i;

        if (ct_item_is_empty (item) || (pass == 0) != item->primary)
          continue;
        g_ptr_array_add (sorted, item);
        g_array_append_val (*indexes, index);
      }
  /* the sorted array borrows the items; they go with it */
  g_ptr_array_set_free_func (items, NULL);
  g_ptr_array_set_free_func (sorted, (GDestroyNotify) ct_item_free);
  for (guint i = 0; i < items->len; i++)
    if (!g_ptr_array_find (sorted, items->pdata[i], NULL))
      ct_item_free (items->pdata[i]);
  g_ptr_array_unref (items);
  return sorted;
}

static char *
escaped (const char *value)
{
  return g_uri_escape_string (value, "+@,", TRUE);
}

/* the communication card (§3.7): numbers, SIP addresses, email addresses, postal addresses, in that order */
static GtkWidget *
contact_card (Quick *quick)
{
  static const CtKind kinds[] = { CT_KIND_PHONE, CT_KIND_SIP, CT_KIND_EMAIL, CT_KIND_ADDRESS };
  static const char *const icons[] = { "ct-phone-symbolic", "ct-dialer-sip-symbolic", "ct-email-symbolic",
                                       "ct-place-symbolic" };
  GtkWidget *card = card_new (quick, NULL);
  const char *country = phone_number_locale_country ();
  gboolean first = TRUE, any = FALSE;

  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++)
    {
      GArray *indexes;
      g_autoptr(GPtrArray) items = sorted_items (quick->contact, kinds[k], &indexes);

      if (items->len && !first)
        card_separator (card);
      for (guint i = 0; i < items->len; i++)
        {
          CtItem *item = items->pdata[i];
          Entry entry = { 0 };
          g_autofree char *header = NULL;
          g_autofree char *value = NULL;
          GtkWidget *row;

          entry.icon = icons[k];
          entry.icon_shown = i == 0;
          entry.kind = kinds[k];
          entry.index = g_array_index (indexes, int, i);
          entry.n_of_kind = (int) items->len;
          entry.first_in_untitled = first;
          entry.later_of_kind = i > 0;
          entry.text = _(ct_type_label (kinds[k], item->type, item->label));
          entry.copy = g_strdup (item->value);
          value = escaped (item->value);
          switch (kinds[k])
            {
            case CT_KIND_PHONE:
              header = phone_number_format (item->value, country);
              entry.uri = g_strconcat ("tel:", value, NULL);
              entry.alt_icon = "ct-message-symbolic";
              entry.alt_uri = g_strconcat ("sms:", value, NULL);
              entry.alt_name = g_strdup_printf (_("Text %s"), header);
              break;
            case CT_KIND_SIP:
              entry.uri = g_strconcat ("sip:", value, NULL);
              break;
            case CT_KIND_EMAIL:
              entry.uri = g_strconcat ("mailto:", value, NULL);
              break;
            default:
              entry.uri = g_strconcat ("geo:0,0?q=", value, NULL);
              break;
            }
          entry.header = header ? header : item->value;
          row = entry_row (quick, &entry, TRUE);
          g_object_set_data (G_OBJECT (row), "ct-default", GINT_TO_POINTER (item->primary));
          gtk_box_append (GTK_BOX (card), row);
          entry_clear (&entry);
          first = FALSE;
          any = TRUE;
        }
      g_array_unref (indexes);
    }
  if (any)
    return card;

  /* no number and no address: "Add phone number" and "Add email", grey, into the editor (no_contact_data_card) */
  {
    Entry phone = { .icon = "ct-phone-symbolic", .icon_shown = TRUE, .header = _("Add phone number"), .add = TRUE,
                    .first_in_untitled = TRUE };
    Entry email = { .icon = "ct-email-symbolic", .icon_shown = TRUE, .header = _("Add email"), .add = TRUE };

    gtk_box_append (GTK_BOX (card), entry_row (quick, &phone, TRUE));
    card_separator (card);
    gtk_box_append (GTK_BOX (card), entry_row (quick, &email, TRUE));
  }
  return card;
}

static void
about_entry (Quick *quick, GtkWidget *card, gboolean *first, const char *header, const char *sub, const char *text,
             char *uri, const char *copy)
{
  Entry entry = { 0 };

  if (!*first)
    card_separator (card);
  entry.header = header;
  entry.sub = sub;
  entry.text = text;
  entry.uri = uri;
  entry.copy = g_strdup (copy);
  entry.kind = CT_N_KINDS;
  gtk_box_append (GTK_BOX (card), entry_row (quick, &entry, FALSE));
  entry_clear (&entry);
  *first = FALSE;
}

/* the About card (§3.7): nickname, website, organization, events, relations, IM, note, without icons; its title
 * "About" and the given name. NULL when there is nothing to put in it. */
static GtkWidget *
about_card (Quick *quick)
{
  g_autoptr(CtName) name = ct_contact_get_name (quick->contact);
  g_autofree char *title = *name->given ? g_strdup_printf ("%s %s", _("About"), name->given) : g_strdup (_("About"));
  GtkWidget *card = card_new (quick, title);
  gboolean first = TRUE;
  g_autoptr(GPtrArray) items = NULL;

  card_separator (card);
  items = ct_contact_get_items (quick->contact, CT_KIND_NICKNAME);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];

      about_entry (quick, card, &first, _("Nickname"), item->value, NULL, NULL, item->value);
    }
  g_ptr_array_unref (items);

  items = ct_contact_get_items (quick->contact, CT_KIND_WEBSITE);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];
      char *uri = strstr (item->value, "://") ? g_strdup (item->value) : g_strconcat ("http://", item->value, NULL);

      about_entry (quick, card, &first, _("Website"), item->value, NULL, uri, item->value);
    }
  g_ptr_array_unref (items);

  /* organization_entry_two_field / _all_field: "Company • Title", "Company • Department • Title" */
  items = ct_contact_get_items (quick->contact, CT_KIND_ORGANIZATION);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];
      g_autoptr(GString) text = g_string_new (NULL);
      const char *parts[] = { item->value, item->value3, item->value2 };

      for (guint p = 0; p < G_N_ELEMENTS (parts); p++)
        if (parts[p] && *parts[p])
          g_string_append_printf (text, "%s%s", text->len ? " • " : "", parts[p]);
      if (text->len)
        about_entry (quick, card, &first, _("Organization"), NULL, text->str, NULL, text->str);
    }
  g_ptr_array_unref (items);

  items = ct_contact_get_items (quick->contact, CT_KIND_EVENT);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];
      g_autofree char *date = ct_date_format_display (item->value);

      about_entry (quick, card, &first, _("Event"), _(ct_type_label (CT_KIND_EVENT, item->type, item->label)),
                   date ? date : item->value, NULL, date ? date : item->value);
    }
  g_ptr_array_unref (items);

  items = ct_contact_get_items (quick->contact, CT_KIND_RELATION);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];

      about_entry (quick, card, &first, _("Relation"), item->value,
                   _(ct_type_label (CT_KIND_RELATION, item->type, item->label)), NULL, item->value);
    }
  g_ptr_array_unref (items);

  items = ct_contact_get_items (quick->contact, CT_KIND_IM);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];

      about_entry (quick, card, &first, _("IM"), item->value2 && *item->value2 ? item->value2 : NULL, item->value,
                   NULL, item->value);
    }
  g_ptr_array_unref (items);

  items = ct_contact_get_items (quick->contact, CT_KIND_NOTE);
  for (guint i = 0; i < items->len; i++)
    {
      CtItem *item = items->pdata[i];

      about_entry (quick, card, &first, _("Note"), item->value, NULL, NULL, item->value);
    }

  if (first)
    {
      g_object_ref_sink (card);
      g_object_unref (card);
      return NULL;
    }
  return card;
}

/* ===== the toolbar ============================================================================================= */

static void
update_star (Quick *quick)
{
  gboolean starred = ct_contact_is_starred (quick->contact);
  GtkWidget *image = neo_pressable_get_child (NEO_PRESSABLE (quick->star));

  gtk_image_set_from_icon_name (GTK_IMAGE (image), starred ? "ct-star-symbolic" : "ct-star-border-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (quick->star), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  starred ? _("Remove from favorites") : _("Add to favorites"), -1);
  gtk_widget_set_visible (quick->star, !quick->profile);
}

static void
on_star (NeoPressable *button, Quick *quick)
{
  ct_store_set_starred (quick->store, quick->uid, !ct_contact_is_starred (quick->contact));
}

static void
on_edit (NeoPressable *button, Quick *quick)
{
  ct_window_edit_contact (quick->window, quick->profile ? CT_PROFILE_UID : quick->uid);
}

static void
on_deleted (const char *uid, const GError *error, gpointer data)
{
  Quick *quick = data;

  if (error)
    {
      g_warning ("delete: %s", error->message);
      return;
    }
  {
    g_autofree char *text = g_strdup_printf (_("%s deleted"), quick->name);

    ct_window_toast (quick->window, text);
  }
  ct_window_pop (quick->window, quick->page);
}

static void
on_delete_confirmed (int response, gpointer data)
{
  Quick *quick = data;
  g_autoptr(GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);

  if (response != CT_DIALOG_OK)
    return;
  g_ptr_array_add (uids, g_strdup (quick->uid));
  ct_store_delete (quick->store, uids, on_deleted, quick);
}

static void
on_menu_chosen (int index, gpointer data)
{
  Quick *quick = data;

  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (quick->overflow), "ct-menu-delete")) && index == 0)
    ct_confirm_dialog (quick->page, NULL, _("Delete this contact?"), _("Delete"), on_delete_confirmed, quick);
  else
    {
      g_autoptr(GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);

      g_ptr_array_add (uids, g_strdup (quick->profile ? CT_PROFILE_UID : quick->uid));
      ct_export_contacts (quick->window, uids, g_dngettext (NULL, "Share contact via", "Share contacts via", 1));
    }
}

/* quickcontact.xml (§3.8): Delete (not for "My info") and Share; the rest is left out (the top comment) */
static void
on_overflow (NeoPressable *button, Quick *quick)
{
  const char *labels[2];
  int n = 0;

  if (!quick->profile)
    labels[n++] = _("Delete");
  labels[n++] = _("Share");
  g_object_set_data (G_OBJECT (quick->overflow), "ct-menu-delete", GINT_TO_POINTER (!quick->profile));
  ct_popup_show (GTK_WIDGET (button), labels, n, CT_POPUP_OVER_END, 0, 0, 0, on_menu_chosen, quick);
}

/* the star and the pencil 48 dp, ⋮ 40 dp, white, with the #a0ffffff ripple of a dark action bar */
static GtkWidget *
make_actions (Quick *quick)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  quick->star = ct_icon_button ("ct-star-border-symbolic", _("Add to favorites"), 48, CT_TOOLBAR_H_DP,
                                CT_RIPPLE_QC_TOOLBAR);
  quick->edit = ct_icon_button ("ct-create-symbolic", _("Edit"), 48, CT_TOOLBAR_H_DP, CT_RIPPLE_QC_TOOLBAR);
  quick->overflow = ct_icon_button ("ct-more-vert-symbolic", _("More options"), 40, CT_TOOLBAR_H_DP,
                                    CT_RIPPLE_QC_TOOLBAR);
  g_signal_connect (quick->star, "clicked", G_CALLBACK (on_star), quick);
  g_signal_connect (quick->edit, "clicked", G_CALLBACK (on_edit), quick);
  g_signal_connect (quick->overflow, "clicked", G_CALLBACK (on_overflow), quick);
  gtk_box_append (GTK_BOX (box), quick->star);
  gtk_box_append (GTK_BOX (box), quick->edit);
  gtk_box_append (GTK_BOX (box), quick->overflow);
  gtk_widget_add_css_class (box, "ct-qc-actions");
  return box;
}

/* ===== the contact ============================================================================================= */

/* the card's contact now: the profile, or the entry of @uid */
static EContact *
current_contact (Quick *quick, GBytes **photo)
{
  EContact *contact;

  *photo = NULL;
  if (quick->profile)
    {
      EContactPhoto *picture;

      contact = ct_store_get_profile (quick->store);
      picture = contact ? e_contact_get (contact, E_CONTACT_PHOTO) : NULL;
      if (picture && picture->type == E_CONTACT_PHOTO_TYPE_INLINED && picture->data.inlined.length)
        *photo = g_bytes_new (picture->data.inlined.data, picture->data.inlined.length);
      if (picture)
        e_contact_photo_free (picture);
      return contact;
    }
  else
    {
      CtEntry *entry = ct_store_lookup (quick->store, quick->uid);

      if (!entry)
        return NULL;
      if (entry->photo)
        *photo = g_bytes_ref (entry->photo);
      return entry->contact;
    }
}

static void
rebuild (Quick *quick)
{
  GBytes *photo_bytes = NULL;
  EContact *contact = current_contact (quick, &photo_bytes);
  CtNameOrder order = ct_display_order (quick->settings);
  GtkWidget *child, *about;
  int index;

  if (!contact)
    return;
  g_set_object (&quick->contact, contact);
  g_free (quick->name);
  g_free (quick->phonetic);
  quick->name = ct_contact_display_name (contact, order);
  if (!*quick->name)
    {
      g_free (quick->name);
      quick->name = g_strdup (_("(No name)"));
    }
  quick->phonetic = ct_contact_phonetic_name (contact);
  if (quick->phonetic && !*quick->phonetic)
    g_clear_pointer (&quick->phonetic, g_free);
  g_clear_object (&quick->photo);
  if (photo_bytes)
    {
      quick->photo = ct_avatar_photo_now (photo_bytes);
      g_bytes_unref (photo_bytes);
    }

  /* the theme colour: the tile's through the hue mapping, or the default for a photo (the top comment) */
  index = quick->photo ? -1 : ct_avatar_color_index (quick->profile ? CT_PROFILE_UID : quick->uid);
  if (quick->photo)
    {
      quick->header_tint = CT_C_QC_DEFAULT_TINT;
      quick->primary = CT_C_QC_DEFAULT_TINT;
      quick->title_color = CT_C_QC_DEFAULT_TITLE;
    }
  else
    {
      quick->header_tint = ct_avatar_header_color (quick->profile ? CT_PROFILE_UID : quick->uid);
      quick->primary = index < 0 ? 0xFF4A6366u : CT_QC_PRIMARY[index];
      quick->title_color = index < 0 ? 0xFF96B1B4u : CT_QC_TITLE[index];
    }

  while ((child = gtk_widget_get_first_child (quick->cards)))
    gtk_box_remove (GTK_BOX (quick->cards), child);
  gtk_box_append (GTK_BOX (quick->cards), contact_card (quick));
  about = about_card (quick);
  if (about)
    gtk_box_append (GTK_BOX (quick->cards), about);
  if (quick->minerva)
    {
      ct_minerva_card_update (quick->minerva, quick->name, quick->title_color);
      gtk_box_append (GTK_BOX (quick->cards), quick->minerva);
    }
  update_star (quick);
  gtk_accessible_update_property (GTK_ACCESSIBLE (quick->stage), GTK_ACCESSIBLE_PROPERTY_LABEL, quick->name, -1);
  gtk_widget_queue_resize (quick->content);
  gtk_widget_queue_draw (quick->header);
}

static void
on_store_changed (CtStore *store, Quick *quick)
{
  if (quick->closing)
    return;
  if (!quick->profile && !ct_store_lookup (store, quick->uid))
    {
      /* deleted (here or elsewhere): there is nothing left to show */
      ct_window_pop (quick->window, quick->page);
      return;
    }
  rebuild (quick);
}

/* ===== coming and going ======================================================================================== */

static void
on_sheet (NeoAnim *anim, double value, gpointer data)
{
  Quick *quick = data;

  quick->sheet = value;
  /* going, the scrim fades as the sheet uncovers it (alpha × (1 − transparent height / height)) */
  if (quick->closing)
    gtk_widget_set_opacity (quick->scrim, 1 - value);
  gtk_widget_queue_allocate (quick->stage);
  if (quick->closing && value >= 1)
    ct_window_pop (quick->window, quick->page);
}

static void
on_scrim (NeoAnim *anim, double value, gpointer data)
{
  Quick *quick = data;

  gtk_widget_set_opacity (quick->scrim, value);
}

/* scrollUpForEntranceAnimation: from below the bottom straight to fully expanded, 300 ms linear_out_slow_in; the
 * scrim from 0 to 0xC8 in config_shortAnimTime */
static void
on_map (GtkWidget *stage, Quick *quick)
{
  if (quick->closing || quick->sheet == 0)
    return;
  neo_anim_start (quick->sheet_anim, 1, 0, CT_QC_ENTRANCE_MS, 0, SPONGY_LINEAR_OUT_SLOW_IN);
  neo_anim_start (quick->scrim_anim, 0, 1, CT_QC_SCRIM_FADE_MS, 0, SPONGY_LINEAR);
}

/* scrollOffBottom: 250 ms, accelerating from the drag's speed (none, for Back: t²), then finish() without an
 * animation */
static void
quick_close (Quick *quick)
{
  if (quick->closing)
    return;
  quick->closing = TRUE;
  neo_anim_stop (quick->scrim_anim);
  gtk_widget_set_can_target (quick->stage, FALSE);
  neo_anim_start (quick->sheet_anim, quick->sheet, 1, CT_QC_EXIT_MS, 0, SPONGY_ACCELERATE);
}

static gboolean
on_back (CtPage *page, gpointer data)
{
  quick_close (data);
  return TRUE;
}

/* a tap on the header below its full height: back to the full height and the cards' top (expandHeader, 300 ms) */
static void
on_expand (NeoAnim *anim, double value, gpointer data)
{
  Quick *quick = data;

  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (quick->scroll), value);
}

static void
on_header_released (GtkGestureClick *gesture, int n, double x, double y, Quick *quick)
{
  double offset = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (quick->scroll));

  if (header_height (quick) >= quick->max_h - 0.5 || offset <= 0)
    return;
  neo_anim_start (quick->expand_anim, offset, 0, CT_QC_EXPAND_HEADER_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

static void
on_scrolled (GtkAdjustment *adjustment, Quick *quick)
{
  gtk_widget_queue_allocate (quick->content);
  gtk_widget_queue_draw (quick->header);
}

static void
quick_free (gpointer data)
{
  Quick *quick = data;

  g_signal_handlers_disconnect_by_data (quick->store, quick);
  g_clear_pointer (&quick->sheet_anim, neo_anim_free);
  g_clear_pointer (&quick->scrim_anim, neo_anim_free);
  g_clear_pointer (&quick->expand_anim, neo_anim_free);
  g_clear_object (&quick->contact);
  g_clear_object (&quick->photo);
  g_clear_object (&quick->minerva);
  g_free (quick->name);
  g_free (quick->phonetic);
  g_free (quick->uid);
  g_object_unref (quick->store);
  g_object_unref (quick->settings);
  g_free (quick);
}

GtkWidget *
ct_quick_new (GtkWidget *window, const char *uid)
{
  CtStore *store = ct_window_get_store (window);
  gboolean profile = g_strcmp0 (uid, CT_PROFILE_UID) == 0;
  Quick *quick;
  GtkGesture *tap;

  if (profile ? !ct_store_get_profile (store) : !ct_store_lookup (store, uid))
    {
      ct_window_toast (window, _("The contact doesn't exist."));
      return NULL;
    }
  quick = g_new0 (Quick, 1);
  quick->window = window;
  quick->store = g_object_ref (store);
  quick->settings = g_object_ref (ct_window_get_settings (window));
  quick->uid = g_strdup (uid);
  quick->profile = profile;
  quick->min_h = CT_TOOLBAR_H_DP;
  quick->max_h = max_header_height (360);
  quick->sheet = 1;
  spongy_interp_cubic (&quick->title_curve, CT_QC_TITLE_CURVE_X1, CT_QC_TITLE_CURVE_Y1, CT_QC_TITLE_CURVE_X2,
                       CT_QC_TITLE_CURVE_Y2);

  quick->page = ct_page_new (window);
  ct_page_set_translucent (CT_PAGE (quick->page), TRUE);
  ct_page_set_back_handler (CT_PAGE (quick->page), on_back, quick);
  gtk_widget_add_css_class (quick->page, "ct-quick");
  g_object_set_data_full (G_OBJECT (quick->page), "ct-quick", quick, quick_free);

  quick->stage = g_object_new (QC_TYPE_STAGE, NULL);
  QC_STAGE (quick->stage)->quick = quick;
  gtk_widget_set_vexpand (quick->stage, TRUE);
  quick->scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (quick->scrim, "ct-qc-scrim");
  gtk_widget_set_opacity (quick->scrim, 0);
  gtk_widget_set_parent (quick->scrim, quick->stage);

  quick->scroll = spongy_scroll_view_new ();
  gtk_widget_add_css_class (quick->scroll, "ct-qc-sheet");
  quick->content = g_object_new (QC_TYPE_CONTENT, NULL);
  QC_CONTENT (quick->content)->quick = quick;
  quick->cards = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (quick->cards, "ct-qc-cards");
  gtk_widget_set_parent (quick->cards, quick->content);
  quick->header = g_object_new (QC_TYPE_HEADER, NULL);
  QC_HEADER (quick->header)->quick = quick;
  quick->actions = make_actions (quick);
  gtk_widget_set_parent (quick->actions, quick->header);
  /* the header after the cards: drawn over them, picked before them */
  gtk_widget_set_parent (quick->header, quick->content);
  tap = gtk_gesture_click_new ();
  g_signal_connect (tap, "released", G_CALLBACK (on_header_released), quick);
  gtk_widget_add_controller (quick->header, GTK_EVENT_CONTROLLER (tap));
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (quick->scroll), quick->content);
  gtk_widget_set_parent (quick->scroll, quick->stage);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (quick->scroll)), "value-changed",
                    G_CALLBACK (on_scrolled), quick);
  gtk_box_append (GTK_BOX (quick->page), quick->stage);

  quick->sheet_anim = neo_anim_new (quick->stage, on_sheet, quick);
  quick->scrim_anim = neo_anim_new (quick->stage, on_scrim, quick);
  quick->expand_anim = neo_anim_new (quick->stage, on_expand, quick);
  g_signal_connect_after (quick->stage, "map", G_CALLBACK (on_map), quick);

  g_signal_connect (store, "changed", G_CALLBACK (on_store_changed), quick);
  if (profile)
    g_signal_connect (store, "profile-changed", G_CALLBACK (on_store_changed), quick);
  /* not for "My info": Minerva's owner is a person of her own, not a contact's */
  quick->minerva = profile ? NULL : ct_minerva_card_new (window, uid);
  if (quick->minerva)
    g_object_ref_sink (quick->minerva);
  rebuild (quick);
  return quick->page;
}
