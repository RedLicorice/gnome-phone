/* cam-dialogs: see cam-dialogs.h. */
#include "cam-dialogs.h"
#include "cam-location-art.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#define RESPONSE_ITEM 100

typedef struct {
  GCallback done;
  gpointer  data;
} Answer;

static void
answer_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

/* ---- the list -------------------------------------------------------------------------------------------------- */

static void
on_list_response (NeoDialog *dialog, int response, Answer *answer)
{
  ((CamChoiceDone) answer->done) (response >= RESPONSE_ITEM ? response - RESPONSE_ITEM : -1, answer->data);
}

static void
on_item (NeoPressable *row, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, RESPONSE_ITEM + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cam-index")));
}

/* select_dialog_singlechoice_material: 48 dp tall; the radio (a 20 dp ring in a 32 dp drawable) 20 dp in, 20 dp
 * to the text (still:camera-settings-list-dialog: ring x 153…212 px, text from 292, card edge 75) */
static GtkWidget *
item_row (const char *label, int index, gboolean checked)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *mark = gtk_image_new_from_icon_name (checked ? "cam-radio-on-symbolic" : "cam-radio-off-symbolic");
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *row;

  /* the 24 dp icon (a 20 dp ring) 4 dp into the 32 dp drawable: margins in camera.css */
  gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
  gtk_widget_add_css_class (mark, "cam-radio");
  if (checked)
    gtk_widget_add_css_class (mark, "checked");
  gtk_box_append (GTK_BOX (box), mark);
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (box), text);
  row = neo_pressable_new (box);
  gtk_widget_add_css_class (row, "cam-dialog-item");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_state (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_STATE_CHECKED,
                               checked ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  g_object_set_data (G_OBJECT (row), "cam-index", GINT_TO_POINTER (index));
  return row;
}

void
cam_list_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                 CamChoiceDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();
  Answer *answer = g_new0 (Answer, 1);

  answer->done = G_CALLBACK (done);
  answer->data = data;
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *row = item_row (labels[i], i, i == selected);

      g_signal_connect (row, "clicked", G_CALLBACK (on_item), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (GTK_WIDGET (dialog), "cam-settings-dialog");
  gtk_widget_add_css_class (list, "cam-dialog-list");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_widget_add_css_class (scroll, "cam-dialog-scroll");
  neo_dialog_set_content (dialog, scroll);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_list_response), answer, answer_free, 0);
  neo_dialog_present (dialog, parent);
}

/* ---- the location prompt --------------------------------------------------------------------------------------- */

/* the header: an ImageView with a 0 dp width and wrap_content height, so the 320 × 96 vector at the card's width */
#define CAM_TYPE_LOCATION_ART (cam_location_art_get_type ())
G_DECLARE_FINAL_TYPE (CamLocationArt, cam_location_art, CAM, LOCATION_ART, GtkWidget)

struct _CamLocationArt {
  GtkWidget parent;
};

G_DEFINE_FINAL_TYPE (CamLocationArt, cam_location_art, GTK_TYPE_WIDGET)

static void
cam_location_art_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                          int *min_base, int *nat_base)
{
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *min = 0, *nat = CAM_ART_W;
  else
    *min = *nat = (int) ((for_size > 0 ? for_size : CAM_ART_W) * CAM_ART_H / (double) CAM_ART_W + 0.5);
}

static void
cam_location_art_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  int width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double scale = width / (double) CAM_ART_W;
  GdkRGBA outline = neo_color (NEO_OUTLINE_VARIANT), accent = neo_color (NEO_SECONDARY);

  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_scale (snapshot, scale, scale);
  for (guint i = 0; i < G_N_ELEMENTS (cam_location_art); i++)
    {
      const CamArtPath *part = &cam_location_art[i];
      g_autoptr(GskPath) path = gsk_path_parse (part->path);

      if (!path)
        continue;
      if (part->stroke > 0)
        {
          g_autoptr(GskStroke) stroke = gsk_stroke_new (part->stroke);

          if (part->round_cap)
            gsk_stroke_set_line_cap (stroke, GSK_LINE_CAP_ROUND);
          gtk_snapshot_append_stroke (snapshot, path, stroke, part->accent ? &accent : &outline);
        }
      else
        gtk_snapshot_append_fill (snapshot, path, GSK_FILL_RULE_WINDING, part->accent ? &accent : &outline);
    }
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

static GtkSizeRequestMode
height_for_width (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
cam_location_art_class_init (CamLocationArtClass *klass)
{
  GTK_WIDGET_CLASS (klass)->get_request_mode = height_for_width;
  GTK_WIDGET_CLASS (klass)->measure = cam_location_art_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = cam_location_art_snapshot;
}

static void
cam_location_art_init (CamLocationArt *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

static void
on_location_response (NeoDialog *dialog, int response, Answer *answer)
{
  ((CamLocationDone) answer->done) (response == NEO_RESPONSE_OK, answer->data);
}

static void
on_later (NeoPressable *button, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, NEO_RESPONSE_CANCEL);
}

static void
on_turn_on (NeoPressable *button, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, NEO_RESPONSE_OK);
}

void
cam_location_dialog (GtkWidget *parent, CamLocationDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (NULL);
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *art = g_object_new (CAM_TYPE_LOCATION_ART, NULL);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new (_("Add photo and video locations"));
  GtkWidget *body = gtk_label_new (_("Map your memories by including location metadata with each capture."));
  GtkWidget *buttons = gtk_center_box_new ();
  GtkWidget *later = neo_text_button_new (_("Later"));
  GtkWidget *turn_on = neo_text_button_new (_("Turn on"));
  Answer *answer = g_new0 (Answer, 1);

  /* the layout's 320 dp, the background a 16 dp-radius rect in colorSurface (camera.css) */
  gtk_widget_set_size_request (card, 320, -1);
  gtk_box_append (GTK_BOX (card), art);
  gtk_widget_set_margin_start (texts, 24);
  gtk_widget_set_margin_end (texts, 24);
  gtk_widget_set_margin_top (texts, 24);
  gtk_widget_set_margin_bottom (texts, 24);
  /* the labels wrap to the card, never widen it: its natural width is the layout's 320 dp */
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (title), 1);
  gtk_label_set_justify (GTK_LABEL (title), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (title, "cam-location-title");
  neo_label_set_font_padding (title, 22);
  gtk_box_append (GTK_BOX (texts), title);
  gtk_label_set_wrap (GTK_LABEL (body), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (body), 1);
  gtk_label_set_justify (GTK_LABEL (body), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (body, "cam-location-body");
  neo_label_set_font_padding (body, 14);
  gtk_widget_set_margin_top (body, gtk_widget_get_margin_top (body) + 16);
  gtk_box_append (GTK_BOX (texts), body);
  gtk_widget_set_margin_top (buttons, 24);
  gtk_widget_add_css_class (later, "cam-location-button");
  gtk_widget_add_css_class (turn_on, "cam-location-button");
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (buttons), later);
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (buttons), turn_on);
  gtk_box_append (GTK_BOX (texts), buttons);
  gtk_box_append (GTK_BOX (card), texts);
  g_signal_connect (later, "clicked", G_CALLBACK (on_later), dialog);
  g_signal_connect (turn_on, "clicked", G_CALLBACK (on_turn_on), dialog);

  gtk_widget_add_css_class (GTK_WIDGET (dialog), "cam-location");
  neo_dialog_set_flush (dialog, TRUE);
  neo_dialog_set_content (dialog, card);
  answer->done = G_CALLBACK (done);
  answer->data = data;
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_location_response), answer, answer_free, 0);
  neo_dialog_present (dialog, parent);
}

/* ---- the passphrase -------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget         *entry;
  CamPassphraseDone  done;
  gpointer           data;
} PassphraseAsk;

static void
on_passphrase_response (NeoDialog *dialog, int response, PassphraseAsk *ask)
{
  /* GtkPasswordEntry keeps its text in secure memory; this copy is the caller's to wipe */
  char *passphrase = response == NEO_RESPONSE_OK ? g_strdup (gtk_editable_get_text (GTK_EDITABLE (ask->entry))) : NULL;

  ask->done (passphrase, ask->data);
}

static void
on_passphrase_activate (GtkPasswordEntry *entry, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, NEO_RESPONSE_OK);
}

void
cam_passphrase_dialog (GtkWidget *parent, const char *message, CamPassphraseDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (_("Passphrase"));
  GtkWidget *entry = gtk_password_entry_new ();
  PassphraseAsk *ask = g_new0 (PassphraseAsk, 1);

  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (entry), TRUE);
  gtk_widget_add_css_class (entry, "neo-entry");
  gtk_widget_set_hexpand (entry, TRUE);
  g_signal_connect (entry, "activate", G_CALLBACK (on_passphrase_activate), dialog);
  neo_dialog_set_message (dialog, message);
  neo_dialog_set_content (dialog, entry);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("Import"), NEO_RESPONSE_OK);
  gtk_widget_add_css_class (GTK_WIDGET (dialog), "cam-settings-dialog");
  ask->entry = entry;
  ask->done = done;
  ask->data = data;
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_passphrase_response), ask, answer_free, 0);
  neo_dialog_present (dialog, parent);
}
