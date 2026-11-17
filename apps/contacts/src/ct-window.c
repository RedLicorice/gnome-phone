/* The Contacts window (ct-window.h). */
#include "ct-window.h"
#include "ct-editor.h"
#include "ct-page.h"
#include "ct-people.h"
#include "ct-popup.h"
#include "ct-quick.h"
#include "ct-ui.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

/* ===== CtPager: the pages, the top ones drawn, sliding as activities do ======================================== */

#define CT_TYPE_PAGER (ct_pager_get_type ())
G_DECLARE_FINAL_TYPE (CtPager, ct_pager, CT, PAGER, GtkWidget)

typedef struct {
  GtkWidget *page;
  double x;                     /* the slide, dp */
  double alpha;
} Placed;

struct _CtPager {
  GtkWidget parent;
  GArray *pages;                /* Placed, bottom first */
  NeoAnim *anim;
  GtkWidget *entering, *exiting;
  gboolean closing;
};

G_DEFINE_FINAL_TYPE (CtPager, ct_pager, GTK_TYPE_WIDGET)

static Placed *
placed_of (CtPager *self, GtkWidget *page)
{
  for (guint i = 0; i < self->pages->len; i++)
    if (g_array_index (self->pages, Placed, i).page == page)
      return &g_array_index (self->pages, Placed, i);
  return NULL;
}

/* the lowest page that shows: the top one, the ones under it as far down as the pages above are translucent, and
 * during a slide both the page that comes and the one that goes */
static guint
lowest_shown (CtPager *self)
{
  guint lowest = self->pages->len ? self->pages->len - 1 : 0;

  while (lowest > 0 && ct_page_get_translucent (CT_PAGE (g_array_index (self->pages, Placed, lowest).page)))
    lowest--;
  for (guint i = 0; i < lowest; i++)
    {
      GtkWidget *page = g_array_index (self->pages, Placed, i).page;

      if (page == self->entering || page == self->exiting)
        return i;
    }
  return lowest;
}

static void
update_pages (CtPager *self)
{
  guint lowest = lowest_shown (self);

  for (guint i = 0; i < self->pages->len; i++)
    {
      Placed *placed = &g_array_index (self->pages, Placed, i);
      gboolean top = i == self->pages->len - 1;

      gtk_widget_set_child_visible (placed->page, i >= lowest);
      /* only the top page takes touches, and not while it slides */
      gtk_widget_set_can_target (placed->page, top && !neo_anim_is_running (self->anim));
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
ct_pager_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
ct_pager_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtPager *self = CT_PAGER (widget);

  for (guint i = 0; i < self->pages->len; i++)
    {
      Placed *placed = &g_array_index (self->pages, Placed, i);

      if (gtk_widget_get_child_visible (placed->page))
        gtk_widget_allocate (placed->page, width, height, -1,
                             gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) round (placed->x), 0)));
    }
}

static void
ct_pager_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtPager *self = CT_PAGER (widget);

  for (guint i = 0; i < self->pages->len; i++)
    {
      Placed *placed = &g_array_index (self->pages, Placed, i);

      if (!gtk_widget_get_child_visible (placed->page) || placed->alpha <= 0)
        continue;
      gtk_snapshot_push_opacity (snapshot, placed->alpha);
      gtk_widget_snapshot_child (widget, placed->page, snapshot);
      gtk_snapshot_pop (snapshot);
    }
}

/* activity_open_* / activity_close_* (§7.5): 450 ms of FOESI for the slides, the fades linear in their windows */
static void
on_slide (NeoAnim *anim, double t, gpointer data)
{
  CtPager *self = data;
  double ms = t * CT_ACTIVITY_ANIM_MS;
  double slide = spongy_curve (SPONGY_EMPHASIZED, (float) t) * CT_ACTIVITY_SLIDE_DP;
  Placed *entering = self->entering ? placed_of (self, self->entering) : NULL;
  Placed *exiting = self->exiting ? placed_of (self, self->exiting) : NULL;

  if (!self->closing)
    {
      if (entering)
        {
          entering->x = CT_ACTIVITY_SLIDE_DP - slide;
          entering->alpha = CLAMP ((ms - CT_ACTIVITY_OPEN_FADE_DELAY_MS) / CT_ACTIVITY_FADE_MS, 0, 1);
        }
      if (exiting)
        exiting->x = -slide;
    }
  else
    {
      if (entering)
        entering->x = slide - CT_ACTIVITY_SLIDE_DP;
      if (exiting)
        {
          exiting->x = slide;
          exiting->alpha = 1 - CLAMP ((ms - CT_ACTIVITY_CLOSE_FADE_DELAY_MS) / CT_ACTIVITY_FADE_MS, 0, 1);
        }
    }
  if (t >= 1)
    {
      GtkWidget *gone = self->closing ? self->exiting : NULL;

      if (entering)
        entering->x = 0, entering->alpha = 1;
      if (exiting)
        exiting->x = 0, exiting->alpha = 1;
      self->entering = self->exiting = NULL;
      if (gone)
        {
          for (guint i = 0; i < self->pages->len; i++)
            if (g_array_index (self->pages, Placed, i).page == gone)
              g_array_remove_index (self->pages, i);
          gtk_widget_unparent (gone);
        }
    }
  update_pages (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
ct_pager_dispose (GObject *object)
{
  CtPager *self = CT_PAGER (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  if (self->pages)
    for (guint i = 0; i < self->pages->len; i++)
      gtk_widget_unparent (g_array_index (self->pages, Placed, i).page);
  g_clear_pointer (&self->pages, g_array_unref);
  G_OBJECT_CLASS (ct_pager_parent_class)->dispose (object);
}

static void
ct_pager_class_init (CtPagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ct_pager_dispose;
  widget_class->measure = ct_pager_measure;
  widget_class->size_allocate = ct_pager_size_allocate;
  widget_class->snapshot = ct_pager_snapshot;
  gtk_widget_class_set_css_name (widget_class, "ct-pager");
}

static void
ct_pager_init (CtPager *self)
{
  self->pages = g_array_new (FALSE, TRUE, sizeof (Placed));
  self->anim = neo_anim_new (GTK_WIDGET (self), on_slide, self);
}

static GtkWidget *
pager_top (CtPager *self)
{
  return self->pages->len ? g_array_index (self->pages, Placed, self->pages->len - 1).page : NULL;
}

/* a slide still running ends where it was going before the next one starts */
static void
finish_slide (CtPager *self)
{
  if (neo_anim_is_running (self->anim))
    neo_anim_finish (self->anim);
}

static void
pager_push (CtPager *self, GtkWidget *page)
{
  Placed placed = { .page = page, .x = 0, .alpha = 1 };
  GtkWidget *below;

  finish_slide (self);
  below = pager_top (self);
  gtk_widget_set_parent (page, GTK_WIDGET (self));
  g_array_append_val (self->pages, placed);
  if (below && !ct_page_get_translucent (CT_PAGE (page)))
    {
      self->closing = FALSE;
      self->entering = page;
      self->exiting = below;
      neo_anim_start (self->anim, 0, 1, CT_ACTIVITY_ANIM_MS, 0, SPONGY_LINEAR);
    }
  update_pages (self);
}

static void
pager_pop (CtPager *self, GtkWidget *page)
{
  gboolean slide;
  guint index;

  finish_slide (self);
  for (index = 0; index < self->pages->len; index++)
    if (g_array_index (self->pages, Placed, index).page == page)
      break;
  if (index == 0 || index >= self->pages->len)
    return;                     /* not here, or the list, which never goes */
  /* the pages above it go with it, at once */
  while (self->pages->len > index + 1)
    {
      GtkWidget *above = pager_top (self);

      g_array_remove_index (self->pages, self->pages->len - 1);
      gtk_widget_unparent (above);
    }
  slide = !ct_page_get_translucent (CT_PAGE (page));
  if (slide)
    {
      self->closing = TRUE;
      self->exiting = page;
      self->entering = g_array_index (self->pages, Placed, index - 1).page;
      neo_anim_start (self->anim, 0, 1, CT_ACTIVITY_ANIM_MS, 0, SPONGY_LINEAR);
    }
  else
    {
      g_array_remove_index (self->pages, index);
      gtk_widget_unparent (page);
    }
  update_pages (self);
}

/* ===== the window ============================================================================================= */

typedef struct {
  GtkWidget *window;
  CtStore *store;
  GSettings *settings;
  CtPager *pager;
  GtkWidget *people;
  GtkWidget *toast, *toast_label;
  NeoAnim *toast_anim;
  guint toast_timeout;
  GtkWidget *snackbar;
} Shell;

static Shell *
shell_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (gtk_widget_get_root (window)), "ct-shell");
}

CtStore *
ct_window_get_store (GtkWidget *window)
{
  return shell_of (window)->store;
}

GSettings *
ct_window_get_settings (GtkWidget *window)
{
  return shell_of (window)->settings;
}

void
ct_window_push (GtkWidget *window, GtkWidget *page)
{
  Shell *shell = shell_of (window);

  ct_popup_dismiss (window);
  /* a page opens with nothing focused, as an activity does unless it asks (no keyboard coming up uninvited) */
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  pager_push (shell->pager, page);
}

void
ct_window_pop (GtkWidget *window, GtkWidget *page)
{
  Shell *shell = shell_of (window);

  ct_popup_dismiss (window);
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  pager_pop (shell->pager, page);
}

GtkWidget *
ct_window_top (GtkWidget *window)
{
  return pager_top (shell_of (window)->pager);
}

GtkWidget *
ct_window_get_people (GtkWidget *window)
{
  return shell_of (window)->people;
}

void
ct_window_show_contact (GtkWidget *window, const char *uid)
{
  GtkWidget *page = ct_quick_new (window, uid);

  if (page)
    ct_window_push (window, page);
}

void
ct_window_edit_contact (GtkWidget *window, const char *uid)
{
  ct_window_push (window, ct_editor_new (window, uid, NULL));
}

void
ct_window_new_contact (GtkWidget *window, EContact *prefill)
{
  ct_window_push (window, ct_editor_new (window, NULL, prefill));
}

/* --- the toast: SystemUI's text toast (text_toast.xml), the app's icon and the text in a pill 48 dp above the
 * bottom, 300 dp wide at most (§7.8) */

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

void
ct_window_toast_hold (GtkWidget *window, const char *text)
{
  Shell *shell = shell_of (window);

  gtk_label_set_label (GTK_LABEL (shell->toast_label), text);
  g_clear_handle_id (&shell->toast_timeout, g_source_remove);
  neo_anim_start (shell->toast_anim, neo_anim_get_value (shell->toast_anim), 1, TOAST_FADE_MS, 0, SPONGY_LINEAR);
}

void
ct_window_toast (GtkWidget *window, const char *text)
{
  Shell *shell = shell_of (window);

  ct_window_toast_hold (window, text);
  shell->toast_timeout = g_timeout_add (TOAST_FADE_MS + CT_TOAST_SHORT_MS, on_toast_timeout, shell);
}

static GtkWidget *
make_toast (Shell *shell)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("de.yesman.Contacts");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "ct-toast-icon");
  gtk_box_append (GTK_BOX (box), icon);
  shell->toast_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (shell->toast_label), TRUE);
  gtk_label_set_lines (GTK_LABEL (shell->toast_label), 2);
  gtk_label_set_ellipsize (GTK_LABEL (shell->toast_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (shell->toast_label), 30);
  gtk_label_set_xalign (GTK_LABEL (shell->toast_label), 0);
  gtk_widget_add_css_class (shell->toast_label, "ct-toast-text");
  gtk_box_append (GTK_BOX (box), shell->toast_label);
  gtk_widget_add_css_class (box, "ct-toast");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_END);
  gtk_widget_set_can_target (box, FALSE);
  gtk_widget_set_visible (box, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Message"), -1);
  shell->toast = box;
  shell->toast_anim = neo_anim_new (box, on_toast_anim, shell);
  return box;
}

void
ct_window_snackbar (GtkWidget *window, const char *text, const char *action, NeoSnackbarAction callback,
                    gpointer data)
{
  neo_snackbar_show (NEO_SNACKBAR (shell_of (window)->snackbar), text, action, callback, data, NEO_SNACKBAR_LONG);
}

NeoSnackbar *
ct_window_get_snackbar (GtkWidget *window)
{
  return NEO_SNACKBAR (shell_of (window)->snackbar);
}

/* --- Back: a menu first, then the top page (which may ask, or leave search or a drawer first), then the window */

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  Shell *shell = data;
  GtkWidget *top = pager_top (shell->pager);

  if (ct_popup_dismiss (shell->window))
    return TRUE;
  if (top != shell->people)
    ct_page_back (CT_PAGE (top));
  else if (!ct_people_back (shell->people))
    gtk_window_close (GTK_WINDOW (shell->window));
  return TRUE;
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  g_clear_handle_id (&shell->toast_timeout, g_source_remove);
  g_clear_pointer (&shell->toast_anim, neo_anim_free);
  g_object_unref (shell->store);
  g_object_unref (shell->settings);
  g_free (shell);
}

GtkWidget *
ct_window_new (GtkApplication *app, CtStore *store, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = neo_window_new (app);
  GtkWidget *root = gtk_overlay_new ();
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();

  shell->window = window;
  shell->store = g_object_ref (store);
  shell->settings = g_object_ref (settings);
  g_object_set_data_full (G_OBJECT (window), "ct-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Contacts"));
  gtk_widget_add_css_class (window, "ct-window");

  shell->pager = g_object_new (CT_TYPE_PAGER, NULL);
  gtk_overlay_set_child (GTK_OVERLAY (root), GTK_WIDGET (shell->pager));
  /* the snackbar is edge to edge: the legacy design_layout_snackbar on a phone (§7.8) */
  shell->snackbar = neo_snackbar_new ();
  neo_snackbar_set_margin (NEO_SNACKBAR (shell->snackbar), 0);
  gtk_widget_add_css_class (shell->snackbar, "ct-snackbar");
  gtk_overlay_add_overlay (GTK_OVERLAY (root), shell->snackbar);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), make_toast (shell));
  neo_window_set_content (GTK_WINDOW (window), root);

  shell->people = ct_people_new (window);
  pager_push (shell->pager, shell->people);

  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);
  return window;
}
