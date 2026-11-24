/* The contact editor (ct-editor.h, spec §4).
 *
 * The screen top to bottom: the toolbar (✕, "Create new contact" / "Edit contact", SAVE), the photo (width / 1.96),
 * the account header ("Saving to"), then the sections in RawContactEditorView's order: name, nickname, company,
 * phone, email, address, website, event, relation, note, labels; "More fields" while any of them is hidden; and the
 * read-only IM and SIP rows ("Can't update this info").
 *
 * A section is the kind's icon in a 68 dp column and its rows; a row is its fields (54 dp each, an underline 4 dp in
 * from both sides), the type spinner under them for the typed kinds, and a 56 dp column at the end with the
 * delete ✕ (or the name's chevron). Every section keeps exactly one empty row at its bottom, which is how a new
 * number or address is added (KindSectionView.updateEmptyNonNameEditors).
 *
 * Saving builds the vCard over the one that was loaded: a row that was not edited keeps its original property with
 * every parameter (ct-contact.h), so nothing the editor does not show is lost.
 *
 * Left out, for want of anything to back them: Take photo (no app here returns a camera picture) and the crop step
 * after choosing one (the picture is cut to its centre square); join suggestions, the linked-contacts picker and
 * the read-only "View only" mode (EDS does not aggregate, and every address book here is writable); typing-time
 * phone number formatting (PhoneNumberFormattingTextWatcher); deleting a read-only IM or SIP row; and the rows'
 * slide animations: rows fade in and out, the rows under them move at once. */
#include "ct-editor.h"
#include "ct-avatar.h"
#include "ct-dialogs.h"
#include "ct-minerva-pages.h"
#include "ct-mnv.h"
#include "ct-page.h"
#include "ct-popup.h"
#include "ct-ui.h"
#include "ct-window.h"
#include "ns-mc.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

typedef enum {
  S_NAME,
  S_NICKNAME,
  S_ORGANIZATION,
  S_PHONE,
  S_EMAIL,
  S_ADDRESS,
  S_WEBSITE,
  S_EVENT,
  S_RELATION,
  S_NOTE,
  S_LABELS,
  N_SECTIONS
} SectionId;

/* what each section is (§4.9) */
typedef struct {
  CtKind kind;
  const char *icon;             /* NULL: the nickname's empty icon slot */
  const char *name;             /* the icon's description and the delete button's "Delete %s" */
  const char *hints[3];
  int n_fields;
  gboolean typed;               /* a type spinner under the fields */
  gboolean single;              /* at most one row (Nickname, Organization, Note) */
  gboolean multiline;           /* Address, Note */
  gboolean shown_empty;         /* Phone and Email: shown before "More fields" even when empty */
  GtkInputPurpose purpose;
  GtkInputHints input_hints;
  /* the primary types a new row takes, the first one not used yet (RawContactModifier.getBestValidType) */
  int new_types[16];
} SectionKind;

static const SectionKind section_kinds[N_SECTIONS] = {
  [S_NAME] = { CT_KIND_NICKNAME, "ct-person-symbolic", N_("Name"), { NULL }, 0 },
  [S_NICKNAME] = { CT_KIND_NICKNAME, NULL, N_("Nickname"), { N_("Nickname") }, 1, FALSE, TRUE, FALSE, FALSE,
                   GTK_INPUT_PURPOSE_NAME, GTK_INPUT_HINT_UPPERCASE_WORDS },
  [S_ORGANIZATION] = { CT_KIND_ORGANIZATION, "ct-business-symbolic", N_("Company"),
                       { N_("Company"), N_("Department"), N_("Title") }, 3, FALSE, TRUE, FALSE, FALSE,
                       GTK_INPUT_PURPOSE_FREE_FORM, GTK_INPUT_HINT_UPPERCASE_WORDS },
  [S_PHONE] = { CT_KIND_PHONE, "ct-phone-symbolic", N_("Phone"), { N_("Phone") }, 1, TRUE, FALSE, FALSE, TRUE,
                GTK_INPUT_PURPOSE_PHONE, GTK_INPUT_HINT_NONE, { 2, 3, 1, 12, 7 } },
  [S_EMAIL] = { CT_KIND_EMAIL, "ct-email-symbolic", N_("Email"), { N_("Email") }, 1, TRUE, FALSE, FALSE, TRUE,
                GTK_INPUT_PURPOSE_EMAIL, GTK_INPUT_HINT_NONE, { 1, 2, 3 } },
  [S_ADDRESS] = { CT_KIND_ADDRESS, "ct-place-symbolic", N_("Address"), { N_("Address") }, 1, TRUE, FALSE, TRUE,
                  FALSE, GTK_INPUT_PURPOSE_FREE_FORM, GTK_INPUT_HINT_UPPERCASE_WORDS, { 1, 2, 3 } },
  [S_WEBSITE] = { CT_KIND_WEBSITE, "ct-public-symbolic", N_("Website"), { N_("Website") }, 1, FALSE, FALSE, FALSE,
                  FALSE, GTK_INPUT_PURPOSE_URL, GTK_INPUT_HINT_NONE },
  [S_EVENT] = { CT_KIND_EVENT, "ct-event-symbolic", N_("Event"), { NULL }, 0, TRUE, FALSE, FALSE, FALSE,
                GTK_INPUT_PURPOSE_FREE_FORM, GTK_INPUT_HINT_NONE, { 3, 1, 2 } },
  [S_RELATION] = { CT_KIND_RELATION, "ct-circles-ext-symbolic", N_("Relationship"), { N_("Relationship") }, 1,
                   TRUE, FALSE, FALSE, FALSE, GTK_INPUT_PURPOSE_NAME, GTK_INPUT_HINT_UPPERCASE_WORDS,
                   { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 } },
  [S_NOTE] = { CT_KIND_NOTE, "ct-insert-comment-symbolic", N_("Notes"), { N_("Notes") }, 1, FALSE, TRUE, TRUE,
               FALSE, GTK_INPUT_PURPOSE_FREE_FORM, GTK_INPUT_HINT_UPPERCASE_SENTENCES },
  [S_LABELS] = { CT_KIND_NICKNAME, "ct-label-symbolic", N_("Labels"), { NULL }, 0 },
};

typedef struct _Editor Editor;
typedef struct _Section Section;

typedef struct {
  Section *section;
  GtkWidget *widget;
  GtkWidget *fields[5];         /* the editables: GtkText, or GtkTextView for a multi-line field */
  int n_fields;
  GtkWidget *spinner, *spinner_label, *delete;
  GtkWidget *date_label;        /* an event's date button */
  int type;
  char *label;                  /* the custom type's label */
  char *date;                   /* an event's value, NULL while it has none */
  CtItem *original;             /* the row as it was read, NULL for a new one */
  gboolean was_empty;
} Row;

struct _Section {
  Editor *editor;
  SectionId id;
  const SectionKind *kind;
  GtkWidget *widget, *rows;
  GPtrArray *row_list;          /* Row, owned */
  gboolean has_data;            /* had data when loaded: shown before "More fields" */
};

struct _Editor {
  GtkWidget *page, *window;
  CtStore *store;
  GSettings *settings;
  char *uid;                    /* NULL: a new contact */
  gboolean profile, is_new, from_quick;
  EContact *original;           /* what was loaded (a duplicate), or an empty contact */
  char *book_uid;

  GtkWidget *scroll, *content, *photo, *account_header, *account_name, *account_icon, *account_expander;
  GtkWidget *save;
  Section sections[N_SECTIONS];

  /* the name section */
  GtkWidget *name_row, *name_fields[5], *chevron, *phonetic_row, *phonetic_fields[3];
  gboolean name_expanded;
  CtName *name;

  /* labels */
  GtkWidget *labels_button, *labels_text;
  GPtrArray *labels;            /* the chosen labels */

  GBytes *photo_bytes;          /* the photo to save, NULL for none */
  GdkTexture *photo_texture;
  guint32 photo_tint;

  GtkWidget *more_fields, *legacy, *channels, *channel_list;
  GPtrArray *channel_rows;      /* ChannelRow: Minerva's chat channels (people ui im_services), one row each */
  GFile *prefill_photo;         /* a new contact's picture from a prefill, read once the page is on screen */
  gboolean expanded;            /* "More fields" was tapped */
  char *initial_state;
  char *saved_name;
  gboolean saving;
};

static void update_section (Section *section);

/* one chat channel's id (an X-<CHANNEL>:<id> or IMPP:<scheme>:<id> row of the card, ct_mnv_im_form) */
typedef struct {
  char *id;                     /* the scheme: the messaging provider's id */
  char *legacy;                 /* the property an older Contacts wrote it as, "" for none */
  char *original;               /* the id the card had when the editor opened */
  GtkWidget *editable, *line;
} ChannelRow;

static void
channel_row_free (gpointer data)
{
  ChannelRow *row = data;

  g_free (row->id);
  g_free (row->legacy);
  g_free (row->original);
  g_free (row);
}
static Row *add_row (Section *section, CtItem *item, gboolean animate);
static char *editor_state (Editor *editor);
static void update_more_fields (Editor *editor);

/* ===== fields ================================================================================================== */

static char *
editable_text (GtkWidget *editable)
{
  if (GTK_IS_TEXT_VIEW (editable))
    {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (editable));
      GtkTextIter start, end;

      gtk_text_buffer_get_bounds (buffer, &start, &end);
      return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
    }
  return g_strdup (gtk_editable_get_text (GTK_EDITABLE (editable)));
}

static void
on_text_view_changed (GtkTextBuffer *buffer, GtkWidget *placeholder)
{
  gtk_widget_set_visible (placeholder, gtk_text_buffer_get_char_count (buffer) == 0);
}

/* A finger on a field that does not have the focus is the scroll view's: GtkText claims a touch the moment it
 * lands (to place the cursor or select), and a claim cancels the scroll view's drag, so a page full of fields could
 * not be scrolled by them as Android's ScrollView is. Until the field has the focus its touches stop here, after
 * the scroll view (an ancestor) has seen them in the capture phase; a touch that ends where it began focuses the
 * field with the cursor at the end of its text. Once focused, the field has its touches (cursor, selection). */
static gboolean
on_field_touch (GtkEventControllerLegacy *controller, GdkEvent *event, GtkWidget *editable)
{
  static double start_x, start_y;
  GdkEventType type = gdk_event_get_event_type (event);
  double x, y;

  if (gtk_widget_has_focus (editable) || gtk_widget_has_focus (gtk_widget_get_parent (editable))
      || (type != GDK_TOUCH_BEGIN && type != GDK_TOUCH_UPDATE && type != GDK_TOUCH_END && type != GDK_TOUCH_CANCEL))
    return GDK_EVENT_PROPAGATE;
  gdk_event_get_position (event, &x, &y);
  if (type == GDK_TOUCH_BEGIN)
    {
      start_x = x;
      start_y = y;
    }
  else if (type == GDK_TOUCH_END && hypot (x - start_x, y - start_y) < CT_TOUCH_SLOP_DP)
    {
      gtk_widget_grab_focus (editable);
      if (GTK_IS_TEXT_VIEW (editable))
        {
          GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (editable));
          GtkTextIter end;

          gtk_text_buffer_get_end_iter (buffer, &end);
          gtk_text_buffer_place_cursor (buffer, &end);
        }
      else
        gtk_editable_set_position (GTK_EDITABLE (editable), -1);
    }
  return GDK_EVENT_STOP;
}

/* A framework EditText: 54 dp tall, the text 4 dp in from the column and the underline under it (1 dp #737373,
 * 2 dp #33666d when focused, 8 dp above the bottom: edit_text_material's insets). @changed runs on every edit. */
static GtkWidget *
field_new (const char *hint, const char *text, gboolean multiline, GtkInputPurpose purpose, GtkInputHints hints,
           GCallback changed, gpointer data, GtkWidget **editable)
{
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *input = gtk_overlay_new ();

  gtk_widget_add_css_class (field, "ct-ed-field");
  gtk_widget_add_css_class (input, "ct-ed-input");
  if (multiline)
    {
      GtkWidget *view = gtk_text_view_new ();
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      GtkWidget *placeholder = ct_label (hint, "ct-ed-hint", 16);

      gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
      gtk_text_view_set_input_purpose (GTK_TEXT_VIEW (view), purpose);
      gtk_text_view_set_input_hints (GTK_TEXT_VIEW (view), hints);
      gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (view), FALSE);
      gtk_widget_add_css_class (view, "ct-ed-text");
      gtk_text_buffer_set_text (buffer, text ? text : "", -1);
      gtk_widget_set_can_target (placeholder, FALSE);
      gtk_widget_set_valign (placeholder, GTK_ALIGN_START);
      gtk_overlay_set_child (GTK_OVERLAY (input), view);
      gtk_overlay_add_overlay (GTK_OVERLAY (input), placeholder);
      on_text_view_changed (buffer, placeholder);
      g_signal_connect (buffer, "changed", G_CALLBACK (on_text_view_changed), placeholder);
      g_signal_connect_swapped (buffer, "changed", changed, data);
      gtk_accessible_update_property (GTK_ACCESSIBLE (view), GTK_ACCESSIBLE_PROPERTY_LABEL, hint, -1);
      *editable = view;
    }
  else
    {
      GtkWidget *entry = gtk_text_new ();

      gtk_text_set_placeholder_text (GTK_TEXT (entry), hint);
      gtk_text_set_input_purpose (GTK_TEXT (entry), purpose);
      gtk_text_set_input_hints (GTK_TEXT (entry), hints);
      gtk_editable_set_text (GTK_EDITABLE (entry), text ? text : "");
      gtk_widget_add_css_class (entry, "ct-ed-text");
      gtk_widget_set_valign (entry, GTK_ALIGN_CENTER);
      gtk_overlay_set_child (GTK_OVERLAY (input), entry);
      g_signal_connect_swapped (entry, "changed", changed, data);
      gtk_accessible_update_property (GTK_ACCESSIBLE (entry), GTK_ACCESSIBLE_PROPERTY_LABEL, hint, -1);
      *editable = entry;
    }
  gtk_widget_set_vexpand (input, TRUE);
  gtk_box_append (GTK_BOX (field), input);
  {
    GtkEventController *touch = gtk_event_controller_legacy_new ();

    gtk_event_controller_set_propagation_phase (touch, GTK_PHASE_CAPTURE);
    g_signal_connect (touch, "event", G_CALLBACK (on_field_touch), *editable);
    gtk_widget_add_controller (input, touch);
  }
  return field;
}

/* ===== rows ==================================================================================================== */

static void
row_free (Row *row)
{
  g_free (row->label);
  g_free (row->date);
  g_clear_pointer (&row->original, ct_item_free);
  g_free (row);
}

static gboolean
row_is_empty (Row *row)
{
  if (row->section->id == S_EVENT)
    return row->date == NULL;
  for (int i = 0; i < row->n_fields; i++)
    {
      g_autofree char *text = editable_text (row->fields[i]);

      if (*g_strstrip (text))
        return FALSE;
    }
  return TRUE;
}

static gboolean
row_has_focus (Row *row)
{
  for (int i = 0; i < row->n_fields; i++)
    if (gtk_widget_has_focus (row->fields[i]) || gtk_widget_has_focus (gtk_widget_get_parent (row->fields[i])))
      return TRUE;
  return FALSE;
}

static const char *
row_type_label (Row *row)
{
  return row->type == CT_TYPE_CUSTOM && row->label ? row->label
                                                   : _(ct_type_label (row->section->kind->kind, row->type, row->label));
}

/* the spinner's label: #dbdbdb, or the hint colour while the row is empty (EditTypeAdapter.getView) */
static void
update_spinner (Row *row)
{
  if (!row->spinner)
    return;
  gtk_label_set_label (GTK_LABEL (row->spinner_label), row_type_label (row));
  if (row_is_empty (row))
    gtk_widget_add_css_class (row->spinner, "empty");
  else
    gtk_widget_remove_css_class (row->spinner, "empty");
}

/* the ✕: invisible while the row is empty (LabeledEditorView :239-243) */
static void
update_delete (Row *row)
{
  if (row->delete)
    gtk_widget_set_opacity (row->delete, row_is_empty (row) ? 0 : 1),
    gtk_widget_set_can_target (row->delete, !row_is_empty (row));
}

static void
on_row_changed (Row *row)
{
  gboolean empty = row_is_empty (row);

  update_delete (row);
  update_spinner (row);
  /* FIELD_TURNED_EMPTY / _NON_EMPTY: keep one empty row at the bottom */
  if (empty != row->was_empty)
    {
      row->was_empty = empty;
      update_section (row->section);
    }
}

typedef struct {
  NeoAnim *anim;
  GtkWidget *widget;
  Row *row;
} Fade;

static void
on_fade (NeoAnim *anim, double value, gpointer data)
{
  Fade *fade = data;

  gtk_widget_set_opacity (fade->widget, value);
  if (value <= 0 && fade->row)
    {
      Section *section = fade->row->section;

      gtk_box_remove (GTK_BOX (section->rows), fade->widget);
      g_ptr_array_remove (section->row_list, fade->row);
      fade->row = NULL;
    }
}

static void
fade_free (gpointer data)
{
  Fade *fade = data;

  neo_anim_free (fade->anim);
  g_free (fade);
}

/* EditorAnimator: a row going fades out over 200 ms; a new one fades in over 200 ms after 200 ms */
static void
fade_row (Row *row, gboolean in)
{
  Fade *fade = g_object_get_data (G_OBJECT (row->widget), "ct-fade");

  if (!fade)
    {
      fade = g_new0 (Fade, 1);
      fade->widget = row->widget;
      fade->anim = neo_anim_new (row->widget, on_fade, fade);
      g_object_set_data_full (G_OBJECT (row->widget), "ct-fade", fade, fade_free);
    }
  fade->row = in ? NULL : row;
  if (in)
    {
      gtk_widget_set_opacity (row->widget, 0);
      neo_anim_start (fade->anim, 0, 1, CT_ED_ANIM_MS, CT_ED_ANIM_MS, SPONGY_ACCELERATE_DECELERATE);
    }
  else
    {
      gtk_widget_set_can_target (row->widget, FALSE);
      neo_anim_start (fade->anim, gtk_widget_get_opacity (row->widget), 0, CT_ED_ANIM_MS, 0,
                      SPONGY_ACCELERATE_DECELERATE);
    }
}

/* onDeleteRequested: the only row of a section is cleared, not removed */
static void
on_delete (NeoPressable *button, Row *row)
{
  Section *section = row->section;

  if (section->row_list->len <= 1)
    {
      for (int i = 0; i < row->n_fields; i++)
        if (GTK_IS_TEXT_VIEW (row->fields[i]))
          gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (row->fields[i])), "", -1);
        else
          gtk_editable_set_text (GTK_EDITABLE (row->fields[i]), "");
      if (section->id == S_EVENT)
        {
          g_clear_pointer (&row->date, g_free);
          gtk_label_set_label (GTK_LABEL (row->date_label), _("Date"));
          gtk_widget_add_css_class (gtk_widget_get_parent (row->date_label), "empty");
        }
      on_row_changed (row);
      return;
    }
  fade_row (row, FALSE);
  /* the row is gone as far as the contact goes (it is out of the list once faded) */
  g_clear_pointer (&row->original, ct_item_free);
  for (int i = 0; i < row->n_fields; i++)
    if (GTK_IS_TEXT_VIEW (row->fields[i]))
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (row->fields[i])), "", -1);
    else
      gtk_editable_set_text (GTK_EDITABLE (row->fields[i]), "");
  g_clear_pointer (&row->date, g_free);
  /* an event's birthday that goes leaves the empty row a Birthday again (restoreBirthday) */
  if (section->id == S_EVENT)
    for (guint i = 0; i < section->row_list->len; i++)
      {
        Row *other = section->row_list->pdata[i];

        if (other != row && !other->date)
          {
            other->type = 3;
            update_spinner (other);
          }
      }
}

/* whether another row of the section has @type (a Birthday may be there only once) */
static gboolean
type_used (Section *section, Row *except, int type)
{
  for (guint i = 0; i < section->row_list->len; i++)
    {
      Row *row = section->row_list->pdata[i];

      if (row != except && row->type == type && (row->original || !row_is_empty (row) || row->date))
        return TRUE;
    }
  return FALSE;
}

/* the first primary type no row has yet, else the last one */
static int
new_row_type (Section *section)
{
  const int *types = section->kind->new_types;
  int last = 0;

  for (int i = 0; types[i]; i++)
    {
      if (!type_used (section, NULL, types[i]))
        return types[i];
      last = types[i];
    }
  return last ? last : ct_kind_default_type (section->kind->kind);
}

typedef struct {
  Row *row;
  int types[24];
  int n;
  gboolean custom_first;
} TypeMenu;

static void
on_custom_label (const char *text, gpointer data)
{
  Row *row = data;

  if (!text)
    return;
  row->type = CT_TYPE_CUSTOM;
  g_free (row->label);
  row->label = g_strdup (text);
  update_spinner (row);
  if (row->n_fields)
    gtk_widget_grab_focus (row->fields[0]);
}

/* an event whose type needs a year (all but Birthday) gets this year when its date has none (onLabelRebuilt) */
static void
event_require_year (Row *row)
{
  int year, month, day;

  if (row->section->id != S_EVENT || row->type == 3 || !row->date || !ct_date_parse (row->date, &year, &month, &day)
      || year)
    return;
  {
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *display = NULL;

    g_free (row->date);
    row->date = ct_date_format_value (g_date_time_get_year (now), month, day);
    display = ct_date_format_display (row->date);
    gtk_label_set_label (GTK_LABEL (row->date_label), display);
  }
}

static void
on_type_chosen (int index, gpointer data)
{
  TypeMenu *menu = data;
  Row *row = menu->row;
  int type = menu->types[index];

  if (index == 0 && menu->custom_first)
    return;
  if (type == CT_TYPE_CUSTOM)
    {
      ct_text_dialog (row->section->editor->page, _("Custom label name"), NULL, _("Label name"), 0, NULL,
                      on_custom_label, row);
      return;
    }
  row->type = type;
  g_clear_pointer (&row->label, g_free);
  update_spinner (row);
  event_require_year (row);
}

/* the spinner's list (getValidTypes): the custom label first if the row has one, then every type of the Device
 * account's list, less a Birthday another row has */
static void
on_spinner (NeoPressable *spinner, Row *row)
{
  static TypeMenu menu;
  const CtType *types = ct_kind_types (row->section->kind->kind);
  const char *labels[24];

  menu.row = row;
  menu.n = 0;
  menu.custom_first = row->type == CT_TYPE_CUSTOM && row->label;
  if (menu.custom_first)
    {
      labels[menu.n] = row->label;
      menu.types[menu.n++] = CT_TYPE_CUSTOM;
    }
  for (int i = 0; types && types[i].label && menu.n < 23; i++)
    {
      if (!types[i].editor && types[i].type != row->type)
        continue;
      if (row->section->id == S_EVENT && types[i].type == 3 && type_used (row->section, row, 3))
        continue;
      labels[menu.n] = _(types[i].label);
      menu.types[menu.n++] = types[i].type;
    }
  ct_popup_show (GTK_WIDGET (spinner), labels, menu.n, CT_POPUP_SPINNER, 0, 0, 0, on_type_chosen, &menu);
}

/* Widget.Material.Spinner.Underlined (§4.11): the label, the ▾ 12 dp from the end, the underline; as wide as the
 * widest label + 56 dp */
static GtkWidget *
spinner_new (Row *row)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *spinner = neo_pressable_new (box);
  GtkWidget *caret = ct_tinted_icon_new ("ct-drop-down-symbolic", 24, CT_C_ED_UNDERLINE);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);
  const CtType *types = ct_kind_types (row->section->kind->kind);
  double widest = 0;

  for (int i = 0; types && types[i].label && i < 15; i++)
    if (types[i].editor)
      {
        g_autoptr(PangoLayout) layout = text_layout (row->section->editor->page, CT_TEXT (16, CT_WEIGHT_REGULAR),
                                                     _(types[i].label), 0);

        widest = MAX (widest, text_width (layout));
      }
  row->spinner_label = ct_label ("", "ct-ed-spinner-text", 16);
  gtk_label_set_ellipsize (GTK_LABEL (row->spinner_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (row->spinner_label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (row->spinner_label, TRUE);
  gtk_box_append (GTK_BOX (box), row->spinner_label);
  gtk_widget_set_valign (caret, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), caret);
  gtk_widget_add_css_class (spinner, "ct-ed-spinner");
  gtk_widget_set_size_request (spinner, (int) ceil (widest + CT_ED_SPINNER_EXTRA_DP), (int) CT_ED_MIN_LINE_DP);
  gtk_widget_set_halign (spinner, GTK_ALIGN_START);
  neo_pressable_set_ripple (NEO_PRESSABLE (spinner), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (spinner), &ripple);
  g_signal_connect (spinner, "clicked", G_CALLBACK (on_spinner), row);
  return spinner;
}

static void
on_date_chosen (int year, int month, int day, gpointer data)
{
  Row *row = data;
  g_autofree char *display = NULL;

  g_free (row->date);
  row->date = ct_date_format_value (year, month, day);
  display = ct_date_format_display (row->date);
  gtk_label_set_label (GTK_LABEL (row->date_label), display);
  gtk_widget_remove_css_class (gtk_widget_get_parent (row->date_label), "empty");
  event_require_year (row);
  on_row_changed (row);
}

static void
on_date_button (NeoPressable *button, Row *row)
{
  int year = 0, month = 0, day = 0;

  if (row->date)
    ct_date_parse (row->date, &year, &month, &day);
  ct_date_dialog (row->section->editor->page, year, month, day, row->type == 3, on_date_chosen, row);
}

/* SpinnerButtonStyle: the date (or "Date" in the hint colour) and the ▾ at the far end of the column */
static GtkWidget *
date_button_new (Row *row)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *button = neo_pressable_new (box);
  GtkWidget *caret = ct_tinted_icon_new ("ct-drop-down-symbolic", 24, CT_C_ED_UNDERLINE);
  g_autofree char *display = row->date ? ct_date_format_display (row->date) : NULL;
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  row->date_label = ct_label (display ? display : _("Date"), "ct-ed-spinner-text", 16);
  gtk_widget_set_valign (row->date_label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (row->date_label, TRUE);
  gtk_box_append (GTK_BOX (box), row->date_label);
  gtk_widget_set_valign (caret, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), caret);
  gtk_widget_add_css_class (button, "ct-ed-spinner");
  gtk_widget_add_css_class (button, "date");
  if (!row->date)
    gtk_widget_add_css_class (box, "empty");
  gtk_widget_set_size_request (button, -1, (int) CT_ED_MIN_LINE_DP);
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (button), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Date"), -1);
  g_signal_connect (button, "clicked", G_CALLBACK (on_date_button), row);
  return button;
}

/* the 56 dp end column's ✕ (#61000000 SRC_ATOP: #9e9e9e), centred on the whole row */
static GtkWidget *
delete_button_new (Row *row)
{
  GtkWidget *button = ct_icon_button ("ct-close-symbolic", _("delete"), CT_ED_END_COLUMN_DP, CT_ED_MIN_LINE_DP,
                                      CT_RIPPLE_APPCOMPAT);
  g_autofree char *name = row->section->kind->typed
                            ? g_strdup_printf (_("Delete %1$s %2$s"), row_type_label (row), _(row->section->kind->name))
                            : g_strdup_printf (_("Delete %s"), _(row->section->kind->name));

  gtk_widget_add_css_class (button, "ct-ed-delete");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect (button, "clicked", G_CALLBACK (on_delete), row);
  return button;
}

/* one row of a section: its fields (or the date button), the spinner, the ✕ */
static Row *
add_row (Section *section, CtItem *item, gboolean animate)
{
  const SectionKind *kind = section->kind;
  Row *row = g_new0 (Row, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  row->section = section;
  row->widget = box;
  row->original = item;
  row->type = item ? item->type : new_row_type (section);
  row->label = item && item->label ? g_strdup (item->label) : NULL;
  gtk_widget_add_css_class (box, "ct-ed-row");
  gtk_widget_set_hexpand (column, TRUE);
  if (section->id == S_EVENT)
    {
      row->date = item && item->value && *item->value ? g_strdup (item->value) : NULL;
      gtk_box_append (GTK_BOX (column), date_button_new (row));
    }
  else
    {
      /* an organization's company, department and title are value, value3 and value2 */
      const char *values[3] = { item ? item->value : NULL, item ? item->value3 : NULL, item ? item->value2 : NULL };

      row->n_fields = kind->n_fields;
      for (int i = 0; i < kind->n_fields; i++)
        gtk_box_append (GTK_BOX (column), field_new (_(kind->hints[i]), values[i], kind->multiline, kind->purpose,
                                                     kind->input_hints, G_CALLBACK (on_row_changed), row,
                                                     &row->fields[i]));
    }
  if (kind->typed)
    {
      row->spinner = spinner_new (row);
      gtk_box_append (GTK_BOX (column), row->spinner);
    }
  gtk_box_append (GTK_BOX (box), column);
  row->delete = delete_button_new (row);
  gtk_box_append (GTK_BOX (box), row->delete);
  g_ptr_array_add (section->row_list, row);
  gtk_box_append (GTK_BOX (section->rows), box);
  row->was_empty = row_is_empty (row);
  update_delete (row);
  update_spinner (row);
  if (animate)
    fade_row (row, TRUE);
  return row;
}

/* KindSectionView.updateEmptyNonNameEditors: exactly one empty row at the bottom (none more once a single-row kind
 * has its row); empty rows without the focus go when there are more of them */
static void
update_empty_rows (Section *section)
{
  guint empty = 0;
  Row *last = section->row_list->len ? section->row_list->pdata[section->row_list->len - 1] : NULL;

  for (guint i = 0; i < section->row_list->len; i++)
    if (row_is_empty (section->row_list->pdata[i]))
      empty++;
  if (empty > 1)
    for (guint i = section->row_list->len; i > 0 && empty > 1; i--)
      {
        Row *row = section->row_list->pdata[i - 1];
        Fade *fade = g_object_get_data (G_OBJECT (row->widget), "ct-fade");

        if (row_is_empty (row) && !row_has_focus (row) && !(fade && fade->row))
          {
            fade_row (row, FALSE);
            empty--;
          }
      }
  if (!empty && !(section->kind->single && section->row_list->len >= 1))
    add_row (section, NULL, gtk_widget_get_mapped (section->rows));
  else if (!last)
    add_row (section, NULL, FALSE);
}

/* a section shows when it has data, when it shows empty (Phone, Email), or after "More fields" */
static gboolean
section_shown (Section *section)
{
  Editor *editor = section->editor;

  if (section->id == S_NAME)
    return TRUE;
  if (section->id == S_LABELS)
    return editor->expanded;
  return editor->expanded || section->kind->shown_empty || section->has_data;
}

static void
update_section (Section *section)
{
  if (section->rows && section->id != S_NAME && section->id != S_LABELS)
    update_empty_rows (section);
  gtk_widget_set_visible (section->widget, section_shown (section));
  update_more_fields (section->editor);
}

/* item_kind_section: the kind's icon (24 dp, 16 dp in, 14 dp down, ≈#e5e5e5) in a 68 dp column, then the rows */
static GtkWidget *
section_new (Editor *editor, SectionId id)
{
  Section *section = &editor->sections[id];
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon_column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  section->editor = editor;
  section->id = id;
  section->kind = &section_kinds[id];
  section->row_list = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);
  section->widget = box;
  gtk_widget_add_css_class (box, "ct-ed-section");
  gtk_widget_set_size_request (icon_column, (int) CT_ED_KIND_ICON_COL_DP, -1);
  if (section->kind->icon)
    {
      GtkWidget *icon = ct_tinted_icon_new (section->kind->icon, 24, CT_ED_ICON_COLOR);

      gtk_widget_set_halign (icon, GTK_ALIGN_START);
      gtk_widget_set_margin_start (icon, (int) CT_ED_KIND_ICON_START_DP);
      gtk_widget_set_margin_top (icon, (int) CT_ED_KIND_ICON_TOP_DP);
      gtk_accessible_update_property (GTK_ACCESSIBLE (icon), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      _(section->kind->name), -1);
      gtk_box_append (GTK_BOX (icon_column), icon);
    }
  gtk_box_append (GTK_BOX (box), icon_column);
  section->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (section->rows, TRUE);
  gtk_box_append (GTK_BOX (box), section->rows);
  return box;
}

/* ===== the name ================================================================================================ */

static void
on_name_changed (Editor *editor)
{
}

/* the name's fields: First and Last name, or with the chevron open Prefix, First, Middle, Last, Suffix (given
 * before family: config_editor_field_order_primary) */
static void
build_name_fields (Editor *editor)
{
  static const char *const hints[5] = { N_("Name prefix"), N_("First name"), N_("Middle name"), N_("Last name"),
                                        N_("Name suffix") };
  GtkWidget *column = g_object_get_data (G_OBJECT (editor->name_row), "ct-column");
  char *values[5];
  GtkWidget *child;

  /* what was typed stays, the fields are made again */
  values[0] = editor->name->prefix;
  values[1] = editor->name->given;
  values[2] = editor->name->middle;
  values[3] = editor->name->family;
  values[4] = editor->name->suffix;
  for (int i = 0; i < 5; i++)
    if (editor->name_fields[i])
      {
        char **slot = i == 0 ? &editor->name->prefix : i == 1 ? &editor->name->given : i == 2 ? &editor->name->middle
                    : i == 3 ? &editor->name->family : &editor->name->suffix;

        g_free (*slot);
        *slot = editable_text (editor->name_fields[i]);
        values[i] = *slot;
      }
  while ((child = gtk_widget_get_first_child (column)))
    gtk_box_remove (GTK_BOX (column), child);
  for (int i = 0; i < 5; i++)
    {
      editor->name_fields[i] = NULL;
      /* the optional ones only when open, or when they have something (mHideOptional && empty) */
      if ((i == 0 || i == 2 || i == 4) && !editor->name_expanded && !*values[i])
        continue;
      gtk_box_append (GTK_BOX (column), field_new (_(hints[i]), values[i], FALSE, GTK_INPUT_PURPOSE_NAME,
                                                   GTK_INPUT_HINT_UPPERCASE_WORDS, G_CALLBACK (on_name_changed),
                                                   editor, &editor->name_fields[i]));
    }
}

static void
update_chevron (Editor *editor)
{
  GtkWidget *image = neo_pressable_get_child (NEO_PRESSABLE (editor->chevron));
  gboolean can_hide = !*editor->name->prefix || !*editor->name->middle || !*editor->name->suffix;

  gtk_image_set_from_icon_name (GTK_IMAGE (image), editor->name_expanded ? "ct-expand-less-symbolic"
                                                                         : "ct-expand-more-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (editor->chevron), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  editor->name_expanded ? _("Collapse name fields") : _("Show more name fields"), -1);
  /* INVISIBLE, not gone, when nothing can hide */
  gtk_widget_set_opacity (editor->chevron, can_hide || editor->name_expanded ? 1 : 0);
}

static void
on_chevron (NeoPressable *button, Editor *editor)
{
  editor->name_expanded = !editor->name_expanded;
  build_name_fields (editor);
  update_chevron (editor);
}

/* the phonetic name: last, middle, first (fixed order), under the name; shown when it has something, when Settings
 * says "Show", or after "More fields" */
static void
update_phonetic (Editor *editor)
{
  g_autofree char *setting = g_settings_get_string (editor->settings, "phonetic-name");
  gboolean has = *editor->name->phonetic_family || *editor->name->phonetic_middle || *editor->name->phonetic_given;

  gtk_widget_set_visible (editor->phonetic_row, has || editor->expanded || g_str_equal (setting, "always"));
}

static GtkWidget *
name_section (Editor *editor)
{
  GtkWidget *section = section_new (editor, S_NAME);
  GtkWidget *rows = editor->sections[S_NAME].rows;
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *phonetic = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *phonetic_column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  static const char *const phonetic_hints[3] = { N_("Phonetic last name"), N_("Phonetic middle name"),
                                                 N_("Phonetic first name") };
  const char *phonetic_values[3] = { editor->name->phonetic_family, editor->name->phonetic_middle,
                                     editor->name->phonetic_given };

  gtk_widget_add_css_class (row, "ct-ed-row");
  gtk_widget_set_hexpand (column, TRUE);
  g_object_set_data (G_OBJECT (row), "ct-column", column);
  gtk_box_append (GTK_BOX (row), column);
  editor->name_row = row;
  /* the chevron: 56 dp, beside the first field (gravity top), #eaeaea */
  editor->chevron = ct_icon_button ("ct-expand-more-symbolic", _("Show more name fields"), CT_ED_END_COLUMN_DP,
                                    CT_ED_MIN_LINE_DP, CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (editor->chevron, "ct-ed-chevron");
  gtk_widget_set_valign (editor->chevron, GTK_ALIGN_START);
  g_signal_connect (editor->chevron, "clicked", G_CALLBACK (on_chevron), editor);
  gtk_box_append (GTK_BOX (row), editor->chevron);
  gtk_box_append (GTK_BOX (rows), row);
  build_name_fields (editor);
  update_chevron (editor);

  gtk_widget_add_css_class (phonetic, "ct-ed-row");
  gtk_widget_set_hexpand (phonetic_column, TRUE);
  for (int i = 0; i < 3; i++)
    gtk_box_append (GTK_BOX (phonetic_column),
                    field_new (_(phonetic_hints[i]), phonetic_values[i], FALSE, GTK_INPUT_PURPOSE_NAME,
                               GTK_INPUT_HINT_NONE, G_CALLBACK (on_name_changed), editor,
                               &editor->phonetic_fields[i]));
  gtk_box_append (GTK_BOX (phonetic), phonetic_column);
  /* no chevron and no ✕: an empty 56 dp */
  gtk_widget_set_size_request (spacer, (int) CT_ED_END_COLUMN_DP, -1);
  gtk_box_append (GTK_BOX (phonetic), spacer);
  gtk_box_append (GTK_BOX (rows), phonetic);
  editor->phonetic_row = phonetic;
  update_phonetic (editor);
  return section;
}

/* ===== labels (§4.17) ========================================================================================== */

static void
update_labels_button (Editor *editor)
{
  g_autoptr(GString) text = g_string_new (NULL);

  for (guint i = 0; i < editor->labels->len; i++)
    g_string_append_printf (text, "%s%s", i ? ", " : "", (char *) editor->labels->pdata[i]);
  gtk_label_set_label (GTK_LABEL (editor->labels_text), text->len ? text->str : _("Label"));
  if (text->len)
    gtk_widget_remove_css_class (editor->labels_button, "empty");
  else
    gtk_widget_add_css_class (editor->labels_button, "empty");
}

typedef struct {
  Editor *editor;
  GPtrArray *names;
} LabelsMenu;

static void
on_label_toggled (int index, gboolean checked, gpointer data)
{
  LabelsMenu *menu = data;
  Editor *editor = menu->editor;
  const char *name = menu->names->pdata[index];
  guint at;

  if (checked && !g_ptr_array_find_with_equal_func (editor->labels, name, g_str_equal, NULL))
    g_ptr_array_add (editor->labels, g_strdup (name));
  else if (!checked && g_ptr_array_find_with_equal_func (editor->labels, name, g_str_equal, &at))
    g_ptr_array_remove_index (editor->labels, at);
  update_labels_button (editor);
}

static const char *
label_exists (const char *text, gpointer data)
{
  Editor *editor = data;

  return ct_store_has_label (editor->store, text) ? _("That label already exists") : NULL;
}

static void
on_label_created (const char *text, gpointer data)
{
  Editor *editor = data;

  if (!text)
    return;
  /* the label is made now (kept in Settings until a contact has it) and checked */
  ct_store_create_label (editor->store, text);
  g_ptr_array_add (editor->labels, g_strdup (text));
  update_labels_button (editor);
}

static void
on_labels_chosen (int index, gpointer data)
{
  LabelsMenu *menu = data;

  ct_text_dialog (menu->editor->page, _("Create label"), NULL, _("Label name"), 40, label_exists, on_label_created,
                  menu->editor);
}

static void
on_labels_button (NeoPressable *button, Editor *editor)
{
  static LabelsMenu menu;
  const char *labels[64];
  gboolean checked[64];
  guint n;

  if (menu.names)
    g_ptr_array_unref (menu.names);
  menu.editor = editor;
  menu.names = ct_store_dup_labels (editor->store);
  n = MIN (menu.names->len, 62);
  for (guint i = 0; i < n; i++)
    {
      labels[i] = menu.names->pdata[i];
      checked[i] = g_ptr_array_find_with_equal_func (editor->labels, labels[i], g_str_equal, NULL);
    }
  labels[n] = _("Create new…");
  checked[n] = FALSE;
  ct_popup_show_checks (GTK_WIDGET (button), labels, (int) n + 1, checked, (int) n, on_label_toggled,
                        on_labels_chosen, &menu);
}

static GtkWidget *
labels_section (Editor *editor)
{
  GtkWidget *section = section_new (editor, S_LABELS);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *caret = ct_tinted_icon_new ("ct-drop-down-symbolic", 24, CT_C_ED_UNDERLINE);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  editor->labels_text = ct_label ("", "ct-ed-spinner-text", 16);
  gtk_label_set_ellipsize (GTK_LABEL (editor->labels_text), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (editor->labels_text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (editor->labels_text, TRUE);
  gtk_box_append (GTK_BOX (box), editor->labels_text);
  gtk_widget_set_valign (caret, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), caret);
  editor->labels_button = neo_pressable_new (box);
  gtk_widget_add_css_class (editor->labels_button, "ct-ed-spinner");
  gtk_widget_add_css_class (editor->labels_button, "labels");
  gtk_widget_set_size_request (editor->labels_button, -1, (int) CT_ED_MIN_LINE_DP);
  neo_pressable_set_ripple (NEO_PRESSABLE (editor->labels_button), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (editor->labels_button), &ripple);
  g_signal_connect (editor->labels_button, "clicked", G_CALLBACK (on_labels_button), editor);
  gtk_box_append (GTK_BOX (editor->sections[S_LABELS].rows), editor->labels_button);
  update_labels_button (editor);
  return section;
}

/* ===== the photo (§4.6) ======================================================================================== */

#define ED_TYPE_PHOTO (ed_photo_get_type ())
G_DECLARE_FINAL_TYPE (EdPhoto, ed_photo, ED, PHOTO, GtkWidget)

struct _EdPhoto {
  GtkWidget parent;
  Editor *editor;
};

G_DEFINE_FINAL_TYPE (EdPhoto, ed_photo, GTK_TYPE_WIDGET)

static void
ed_photo_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  /* as wide as the screen, width / 1.96 tall (editor_portrait_photo_ratio) */
  if (orientation == GTK_ORIENTATION_VERTICAL)
    *minimum = *natural = (int) round ((for_size > 0 ? for_size : 360) / CT_ED_PHOTO_RATIO);
  else
    *minimum = *natural = 0;
}

static GtkSizeRequestMode
ed_photo_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
ed_photo_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  Editor *editor = ED_PHOTO (widget)->editor;
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, (float) width, (float) height);
  GdkRGBA white = ct_rgba (0xFFFFFFFFu);

  ct_avatar_header_snapshot (snapshot, &bounds, editor->photo_tint, editor->photo_texture);
  /* the camera badge: 24 dp, white, 16 dp from the bottom and the end */
  neo_snapshot_icon (snapshot, widget, "ct-camera-alt-symbolic", width - CT_ED_PHOTO_BADGE_MARGIN_DP - 24,
                     height - CT_ED_PHOTO_BADGE_MARGIN_DP - 24, 24, &white);
}

static void
ed_photo_class_init (EdPhotoClass *klass)
{
  GTK_WIDGET_CLASS (klass)->measure = ed_photo_measure;
  GTK_WIDGET_CLASS (klass)->get_request_mode = ed_photo_get_request_mode;
  GTK_WIDGET_CLASS (klass)->snapshot = ed_photo_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-ed-photo");
}

static void
ed_photo_init (EdPhoto *self)
{
}

static void
set_photo (Editor *editor, GBytes *bytes)
{
  g_clear_pointer (&editor->photo_bytes, g_bytes_unref);
  g_clear_object (&editor->photo_texture);
  if (bytes)
    {
      editor->photo_bytes = g_bytes_ref (bytes);
      editor->photo_texture = ct_avatar_photo_now (bytes);
    }
  gtk_accessible_update_property (GTK_ACCESSIBLE (gtk_widget_get_parent (editor->photo)),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  bytes ? _("Change contact photo") : _("Add contact photo"), -1);
  gtk_widget_queue_draw (editor->photo);
}

/* a picture file as the contact's photo, cut to its centre square and made at most 720 px (the crop step is left
 * out); FALSE when it cannot be read, or is larger than Minerva's data allows (people ui photo_max_bytes: a file is
 * not decoded whole into memory for a 720 px square). The page must be on screen (its renderer draws the square). */
static gboolean
photo_from_file (Editor *editor, GFile *file)
{
  g_autoptr(GdkTexture) texture = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GskRenderNode) node = NULL;
  g_autoptr(GdkTexture) square = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GFileInfo) info = NULL;
  GtkSnapshot *snapshot;
  GskRenderer *renderer;
  int w, h, side, size;
  double max_bytes = ct_mnv_double ("photo_max_bytes");

  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (!info)
    {
      g_warning ("photo: %s", error->message);
      return FALSE;
    }
  if (max_bytes > 0 && (double) g_file_info_get_size (info) > max_bytes)
    {
      g_warning ("photo: %" G_GOFFSET_FORMAT " bytes, more than photo_max_bytes", g_file_info_get_size (info));
      return FALSE;
    }
  texture = gdk_texture_new_from_file (file, &error);
  if (!texture)
    {
      g_warning ("photo: %s", error->message);
      return FALSE;
    }
  w = gdk_texture_get_width (texture);
  h = gdk_texture_get_height (texture);
  side = MIN (w, h);
  size = MIN (side, 720);
  snapshot = gtk_snapshot_new ();
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, size, size));
  gtk_snapshot_append_texture (snapshot, texture,
                               &GRAPHENE_RECT_INIT ((float) (-(w - side) / 2.0 * size / side),
                                                    (float) (-(h - side) / 2.0 * size / side),
                                                    (float) w * size / side, (float) h * size / side));
  gtk_snapshot_pop (snapshot);
  node = gtk_snapshot_free_to_node (snapshot);
  renderer = gtk_native_get_renderer (gtk_widget_get_native (editor->page));
  square = gsk_renderer_render_texture (renderer, node, &GRAPHENE_RECT_INIT (0, 0, size, size));
  bytes = gdk_texture_save_to_png_bytes (square);
  set_photo (editor, bytes);
  return TRUE;
}

/* the chosen picture */
static void
on_photo_file (GObject *source, GAsyncResult *result, gpointer data)
{
  Editor *editor = data;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);

  if (file && !photo_from_file (editor, file))
    ct_window_toast (editor->window, _("Couldn't save contact photo changes"));
}

static void
choose_photo (Editor *editor)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_filter_set_name (filter, _("Images"));
  gtk_file_filter_add_mime_type (filter, "image/*");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_title (dialog, _("Choose photo"));
  gtk_file_dialog_open (dialog, GTK_WINDOW (gtk_widget_get_root (editor->page)), NULL, on_photo_file, editor);
}

static void
on_photo_action (int response, gpointer data)
{
  Editor *editor = data;

  if (response < 0)
    return;
  /* the rows: "Remove photo" when there is one, then "Choose photo" */
  if (editor->photo_bytes && response == 0)
    set_photo (editor, NULL);
  else
    choose_photo (editor);
}

/* PhotoSourceDialogFragment: "Change photo", Remove photo / Choose photo (Take photo is left out) */
static void
on_photo_tapped (NeoPressable *button, Editor *editor)
{
  const char *labels[2];
  int n = 0;

  if (editor->photo_bytes)
    labels[n++] = _("Remove photo");
  labels[n++] = editor->photo_bytes ? _("Select new photo") : _("Choose photo");
  ct_list_dialog (editor->page, CT_DIALOG_APPCOMPAT, _("Change photo"), labels, n, TRUE, on_photo_action, editor);
}

static GtkWidget *
photo_area (Editor *editor)
{
  GtkWidget *photo = g_object_new (ED_TYPE_PHOTO, NULL);
  GtkWidget *button = neo_pressable_new (photo);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_QC_TOOLBAR);

  ED_PHOTO (photo)->editor = editor;
  editor->photo = photo;
  gtk_widget_add_css_class (button, "ct-ed-photo-button");
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (button), &ripple);
  g_signal_connect (button, "clicked", G_CALLBACK (on_photo_tapped), editor);
  return button;
}

/* ===== the account header (§4.7) =============================================================================== */

static GPtrArray *writable_books (Editor *editor);

/* the chooser: a new contact, not the profile, and more than one writable address book (§4.7.2) */
static gboolean
has_chooser (Editor *editor)
{
  g_autoptr(GPtrArray) books = writable_books (editor);

  return editor->is_new && !editor->profile && books->len > 1;
}

static void
update_account (Editor *editor)
{
  CtBook *book;
  g_autofree char *description = NULL;
  const char *name;
  gboolean chooser = has_chooser (editor);

  /* Opened by another app (the call log's "Create new contact") the editor can come before the address books have
   * been listed, with no default book to take yet: it takes it once they are, or it saved into nothing */
  if (!editor->book_uid && !editor->profile)
    editor->book_uid = g_strdup (ct_store_default_book (editor->store));
  book = ct_store_get_book (editor->store, editor->book_uid);
  name = editor->profile ? _("My local profile") : book ? ct_book_label (book) : _("Device");
  gtk_label_set_label (GTK_LABEL (editor->account_name), name);
  gtk_widget_set_visible (editor->account_expander, chooser);
  gtk_widget_set_can_target (editor->account_header, chooser);
  ct_tinted_icon_set (editor->account_icon, !book || book->is_local ? "ct-smartphone-symbolic"
                                                                    : "ct-account-circle-symbolic",
                      CT_C_ED_ACCOUNT_ICON);
  description = g_strdup_printf ("%s\n%s", _("Saving to"), name);
  gtk_accessible_update_property (GTK_ACCESSIBLE (editor->account_header), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  description, -1);
}

static GPtrArray *
writable_books (Editor *editor)
{
  GPtrArray *books = ct_store_get_books (editor->store);
  GPtrArray *writable = g_ptr_array_new ();

  for (guint i = 0; i < books->len; i++)
    if (((CtBook *) books->pdata[i])->writable)
      g_ptr_array_add (writable, books->pdata[i]);
  return writable;
}

static void
on_account_chosen (int index, gpointer data)
{
  Editor *editor = data;
  g_autoptr(GPtrArray) books = writable_books (editor);

  if (index < 0 || (guint) index >= books->len)
    return;
  g_free (editor->book_uid);
  editor->book_uid = g_strdup (((CtBook *) books->pdata[index])->uid);
  update_account (editor);
}

/* the account chooser: a new contact, not the profile, and more than one writable address book (§4.7.2) */
static void
on_account_header (NeoPressable *header, Editor *editor)
{
  g_autoptr(GPtrArray) books = writable_books (editor);
  const char *labels[16];
  guint n = MIN (books->len, 16);

  for (guint i = 0; i < n; i++)
    labels[i] = ct_book_label (books->pdata[i]);
  ct_popup_show (GTK_WIDGET (header), labels, (int) n, CT_POPUP_BELOW, 0, 0, gtk_widget_get_width (GTK_WIDGET (header)),
                 on_account_chosen, editor);
}

static GtkWidget *
account_header (Editor *editor)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = neo_pressable_new (box);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  editor->account_icon = ct_tinted_icon_new ("ct-smartphone-symbolic", 24, CT_C_ED_ACCOUNT_ICON);
  gtk_widget_set_valign (editor->account_icon, GTK_ALIGN_START);
  gtk_widget_add_css_class (editor->account_icon, "ct-ed-account-icon");
  gtk_box_append (GTK_BOX (box), editor->account_icon);
  gtk_box_append (GTK_BOX (lines), ct_label (_("Saving to"), "ct-ed-account-type", 16));
  editor->account_name = ct_label ("", "ct-ed-account-name", 14);
  gtk_label_set_ellipsize (GTK_LABEL (editor->account_name), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (lines), editor->account_name);
  gtk_widget_set_hexpand (lines, TRUE);
  gtk_widget_add_css_class (lines, "ct-ed-account-lines");
  gtk_box_append (GTK_BOX (box), lines);
  editor->account_expander = ct_tinted_icon_new ("ct-expand-more-symbolic", 24, CT_C_ED_CHEVRON);
  gtk_widget_add_css_class (editor->account_expander, "ct-ed-account-expander");
  gtk_widget_set_valign (editor->account_expander, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), editor->account_expander);
  gtk_widget_add_css_class (header, "ct-ed-account");
  neo_pressable_set_ripple (NEO_PRESSABLE (header), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (header), &ripple);
  g_signal_connect (header, "clicked", G_CALLBACK (on_account_header), editor);
  editor->account_header = header;
  update_account (editor);
  g_signal_connect_swapped (editor->store, "books-changed", G_CALLBACK (update_account), editor);
  return header;
}

/* ===== "More fields" and the read-only section ================================================================= */

static void
update_more_fields (Editor *editor)
{
  gboolean hidden = FALSE;

  if (!editor->more_fields)
    return;
  for (int i = 0; i < N_SECTIONS; i++)
    if (editor->sections[i].widget && !section_shown (&editor->sections[i]))
      hidden = TRUE;
  gtk_widget_set_visible (editor->more_fields, hidden);
}

/* showAllFields: every section shows with an empty row, and the button goes */
static void
on_more_fields (NeoPressable *button, Editor *editor)
{
  editor->expanded = TRUE;
  for (int i = 0; i < N_SECTIONS; i++)
    if (editor->sections[i].widget)
      update_section (&editor->sections[i]);
  update_phonetic (editor);
  gtk_widget_set_visible (editor->more_fields, FALSE);
}

static void
on_legacy_long_pressed (NeoPressable *row, Editor *editor)
{
  ct_copy_text (GTK_WIDGET (row), g_object_get_data (G_OBJECT (row), "ct-copy"));
  ct_window_toast (editor->window, _("Text copied"));
}

static gboolean
channel_row_of (Editor *editor, const char *value)
{
  for (guint i = 0; editor->channel_rows && value && *value && i < editor->channel_rows->len; i++)
    if (g_strcmp0 (((ChannelRow *) editor->channel_rows->pdata[i])->original, value) == 0)
      return TRUE;
  return FALSE;
}

static void
on_channel_changed (Editor *editor)
{
}

/* the legacy property of a channel, from the Minerva section's list (people ui channel_list), "" for none */
static const char *
channel_legacy (const char *id)
{
  JsonArray *list = ct_mnv_list ("channel_list");

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *c = json_array_get_object_element (list, i);

      if (g_strcmp0 (ns_json_str (c, "id"), id) == 0)
        return ns_json_str (c, "legacy_property");
    }
  return "";
}

/* Minerva's chat channels (the messaging providers that are enabled, people ui im_services), one row each, written
 * as the memory service writes them (ct_mnv_im_form); only where Minerva's texts are loaded and say how. On a signed
 * card a change is checked at saving. */
static GtkWidget *
channels_section (Editor *editor, EContact *prefill)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon_column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  JsonArray *services = ct_mnv_ui_ready () && im_form_usable (ct_mnv_im_form ()) ? ct_mnv_list ("im_services") : NULL;
  GtkWidget *icon = ct_tinted_icon_new ("ct-message-symbolic", 24, CT_ED_ICON_COLOR);

  editor->channel_rows = g_ptr_array_new_with_free_func (channel_row_free);
  gtk_widget_add_css_class (box, "ct-ed-section");
  gtk_widget_set_size_request (icon_column, (int) CT_ED_KIND_ICON_COL_DP, -1);
  gtk_widget_set_halign (icon, GTK_ALIGN_START);
  gtk_widget_set_margin_start (icon, (int) CT_ED_KIND_ICON_START_DP);
  gtk_widget_set_margin_top (icon, (int) CT_ED_KIND_ICON_TOP_DP);
  gtk_box_append (GTK_BOX (icon_column), icon);
  gtk_box_append (GTK_BOX (box), icon_column);
  gtk_widget_set_hexpand (rows, TRUE);
  gtk_box_append (GTK_BOX (box), rows);
  for (guint i = 0; services && i < json_array_get_length (services); i++)
    {
      JsonObject *service = json_array_get_object_element (services, i);
      ChannelRow *row = g_new0 (ChannelRow, 1);
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *field, *title;
      g_autofree char *initial = NULL;

      row->id = g_strdup (ns_json_str (service, "id"));
      row->legacy = g_strdup (channel_legacy (row->id));
      row->original = ct_contact_get_channel (editor->original, row->id, *row->legacy ? row->legacy : NULL,
                                              ct_mnv_im_form ());
      /* a new contact's id from its prefill (an imported chat's), saved as a typed one would be */
      initial = editor->is_new && prefill ? ct_contact_get_channel (prefill, row->id, NULL, ct_mnv_im_form ())
                                          : g_strdup (row->original);
      field = field_new (ns_json_str (service, "title"), initial, FALSE, GTK_INPUT_PURPOSE_FREE_FORM,
                         GTK_INPUT_HINT_NONE, G_CALLBACK (on_channel_changed), editor, &row->editable);
      gtk_widget_set_hexpand (field, TRUE);
      gtk_widget_add_css_class (line, "ct-ed-row");
      gtk_box_append (GTK_BOX (line), field);
      /* the service's name stays beside the id once the hint is gone, as the read-only IM rows show it */
      title = ct_label (ns_json_str (service, "title"), "ct-ed-legacy-text", 16);
      gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
      gtk_widget_set_size_request (title, (int) CT_ED_TYPE_LABEL_W_DP, -1);
      gtk_widget_set_valign (title, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (line), title);
      gtk_box_append (GTK_BOX (rows), line);
      row->line = line;
      g_ptr_array_add (editor->channel_rows, row);
    }
  editor->channel_list = rows;
  gtk_widget_set_visible (box, editor->channel_rows->len > 0);
  return box;
}

/* "Collega <channel>": the chat picked from the provider's is the channel row's id. Nothing is written before Save,
 * whose signed-card check covers it as it covers a typed id. */
typedef struct {
  Editor *editor;
  ChannelRow *row;
} Link;

static void
on_link_picked (const char *provider, const char *chat, gpointer data)
{
  Link *link = data;

  gtk_editable_set_text (GTK_EDITABLE (link->row->editable), chat);
}

static void
on_link (NeoPressable *button, Link *link)
{
  ct_window_push (link->editor->window,
                  ct_mnv_chats_new (link->editor->window, link->row->id, TRUE, on_link_picked, link, NULL));
}

/* under each channel whose provider imports chats, its "Collega <provider>" (an existing contact's editor) */
static void
on_imports (JsonObject *root, const char *err, gpointer data)
{
  Editor *editor = data;

  if (err)
    return;
  for (guint i = 0; i < editor->channel_rows->len; i++)
    {
      ChannelRow *row = editor->channel_rows->pdata[i];
      JsonObject *provider = ct_mnv_import_provider (row->id);
      g_autofree char *text = NULL;
      GtkWidget *button;
      Link *link;

      if (!provider)
        continue;
      text = ct_mnv_textf ("import_link", (const char *const[]) { "provider", ns_json_str (provider, "label"), NULL });
      button = ct_text_button (text, "ct-ed-link");
      gtk_widget_set_halign (button, GTK_ALIGN_START);
      link = g_new0 (Link, 1);
      link->editor = editor;
      link->row = row;
      g_object_set_data_full (G_OBJECT (button), "ct-link", link, g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_link), link);
      gtk_box_insert_child_after (GTK_BOX (editor->channel_list), button, row->line);
    }
}

/* "Can't update this info": the IM and SIP rows, which the editor shows but cannot change (§4.18) */
static GtkWidget *
legacy_section (Editor *editor)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autofree char *title = g_utf8_strup (_("Can’t update this info"), -1);
  static const CtKind kinds[] = { CT_KIND_IM, CT_KIND_SIP };
  static const char *const icons[] = { "ct-message-symbolic", "ct-dialer-sip-symbolic" };
  gboolean any = FALSE;

  gtk_widget_add_css_class (box, "ct-ed-legacy");
  gtk_box_append (GTK_BOX (box), ct_label (title, "ct-ed-legacy-title", 11));
  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++)
    {
      g_autoptr(GPtrArray) items = ct_contact_get_items (editor->original, kinds[k]);

      for (guint i = 0; i < items->len; i++)
        {
          CtItem *item = items->pdata[i];
          GtkWidget *line;

          /* a chat channel's id is edited in the channels' rows above, not shown again here */
          if (kinds[k] == CT_KIND_IM && channel_row_of (editor, item->value))
            continue;
          line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
          GtkWidget *row = neo_pressable_new (line);
          GtkWidget *icon = ct_tinted_icon_new (icons[k], 24, CT_ED_ICON_COLOR);
          GtkWidget *value = ct_label (item->value, "ct-ed-legacy-text", 16);
          GtkWidget *type = ct_label (kinds[k] == CT_KIND_IM && item->value2 ? item->value2
                                                                             : _(ct_type_label (kinds[k], item->type,
                                                                                                item->label)),
                                      "ct-ed-legacy-text", 16);

          gtk_widget_set_opacity (icon, i == 0 ? 0.38 : 0);
          gtk_widget_add_css_class (icon, "ct-ed-legacy-icon");
          gtk_box_append (GTK_BOX (line), icon);
          gtk_label_set_ellipsize (GTK_LABEL (value), PANGO_ELLIPSIZE_END);
          gtk_widget_set_hexpand (value, TRUE);
          gtk_box_append (GTK_BOX (line), value);
          gtk_label_set_ellipsize (GTK_LABEL (type), PANGO_ELLIPSIZE_END);
          gtk_widget_set_size_request (type, (int) CT_ED_TYPE_LABEL_W_DP, -1);
          gtk_box_append (GTK_BOX (line), type);
          gtk_widget_add_css_class (row, "ct-ed-legacy-row");
          g_object_set_data_full (G_OBJECT (row), "ct-copy", g_strdup (item->value), g_free);
          g_signal_connect (row, "long-pressed", G_CALLBACK (on_legacy_long_pressed), editor);
          gtk_box_append (GTK_BOX (box), row);
          any = TRUE;
        }
    }
  gtk_widget_set_visible (box, any);
  return box;
}

/* ===== loading, saving, leaving ================================================================================ */

/* what the screen says now, as one string: comparing it with the loaded one tells whether anything changed
 * (RawContactModifier.hasChanges); empty rows, which do not count, leave nothing in it */
static char *
editor_state (Editor *editor)
{
  GString *state = g_string_new (NULL);

  for (int i = 0; i < 5; i++)
    if (editor->name_fields[i])
      {
        g_autofree char *text = editable_text (editor->name_fields[i]);

        g_string_append_printf (state, "n%d=%s\n", i, g_strstrip (text));
      }
    else
      {
        const char *values[5] = { editor->name->prefix, editor->name->given, editor->name->middle,
                                  editor->name->family, editor->name->suffix };

        g_string_append_printf (state, "n%d=%s\n", i, values[i]);
      }
  for (int i = 0; i < 3; i++)
    {
      g_autofree char *text = editable_text (editor->phonetic_fields[i]);

      g_string_append_printf (state, "p%d=%s\n", i, g_strstrip (text));
    }
  for (int s = S_NICKNAME; s < S_LABELS; s++)
    {
      Section *section = &editor->sections[s];

      for (guint r = 0; r < section->row_list->len; r++)
        {
          Row *row = section->row_list->pdata[r];

          if (row_is_empty (row))
            continue;
          g_string_append_printf (state, "%d:%d:%s:%s", s, row->type, row->label ? row->label : "",
                                  row->date ? row->date : "");
          for (int f = 0; f < row->n_fields; f++)
            {
              g_autofree char *text = editable_text (row->fields[f]);

              g_string_append_printf (state, "|%s", g_strstrip (text));
            }
          g_string_append_c (state, '\n');
        }
    }
  for (guint i = 0; editor->channel_rows && i < editor->channel_rows->len; i++)
    {
      ChannelRow *row = editor->channel_rows->pdata[i];
      g_autofree char *text = editable_text (row->editable);

      g_string_append_printf (state, "c:%s=%s\n", row->id, g_strstrip (text));
    }
  for (guint i = 0; i < editor->labels->len; i++)
    g_string_append_printf (state, "l=%s\n", (char *) editor->labels->pdata[i]);
  g_string_append_printf (state, "photo=%p\n", (void *) editor->photo_bytes);
  return g_string_free (state, FALSE);
}

static gboolean
has_changes (Editor *editor)
{
  g_autofree char *now = editor_state (editor);

  return g_strcmp0 (now, editor->initial_state) != 0;
}

/* the vCard to save: the loaded one with the screen's values written over it */
static EContact *
build_contact (Editor *editor)
{
  EContact *contact = e_contact_duplicate (editor->original);
  g_autoptr(CtName) name = ct_name_new ();
  char **name_slots[5] = { &name->prefix, &name->given, &name->middle, &name->family, &name->suffix };
  const char *kept[5] = { editor->name->prefix, editor->name->given, editor->name->middle, editor->name->family,
                          editor->name->suffix };
  char **phonetic_slots[3] = { &name->phonetic_family, &name->phonetic_middle, &name->phonetic_given };

  for (int i = 0; i < 5; i++)
    {
      g_free (*name_slots[i]);
      *name_slots[i] = editor->name_fields[i] ? g_strstrip (editable_text (editor->name_fields[i]))
                                              : g_strdup (kept[i]);
    }
  for (int i = 0; i < 3; i++)
    {
      g_free (*phonetic_slots[i]);
      *phonetic_slots[i] = g_strstrip (editable_text (editor->phonetic_fields[i]));
    }
  ct_contact_set_name (contact, name);

  for (int s = S_NICKNAME; s < S_LABELS; s++)
    {
      Section *section = &editor->sections[s];
      g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);

      for (guint r = 0; r < section->row_list->len; r++)
        {
          Row *row = section->row_list->pdata[r];
          CtItem *item;
          char *values[3] = { NULL };

          if (row_is_empty (row))
            continue;
          for (int f = 0; f < row->n_fields; f++)
            values[f] = g_strstrip (editable_text (row->fields[f]));
          /* an edited row starts from its original (its parameters and its PREF stay) */
          item = row->original ? ct_item_copy (row->original) : ct_item_new (section->kind->kind, row->type, NULL);
          item->type = row->type;
          g_free (item->label);
          item->label = row->type == CT_TYPE_CUSTOM ? g_strdup (row->label) : NULL;
          g_free (item->value);
          if (s == S_EVENT)
            item->value = g_strdup (row->date);
          else
            item->value = values[0] ? values[0] : g_strdup ("");
          if (s == S_ORGANIZATION)
            {
              g_free (item->value3);
              g_free (item->value2);
              item->value3 = values[1];
              item->value2 = values[2];
            }
          g_ptr_array_add (items, item);
        }
      ct_contact_set_items (contact, section->kind->kind, items);
    }

  for (guint i = 0; editor->channel_rows && i < editor->channel_rows->len; i++)
    {
      ChannelRow *row = editor->channel_rows->pdata[i];
      g_autofree char *text = editable_text (row->editable);

      ct_contact_apply_channel (contact, row->id, *row->legacy ? row->legacy : NULL, ct_mnv_im_form (), row->original,
                                text);
    }
  ct_contact_set_labels (contact, editor->labels);
  if (editor->photo_bytes)
    {
      EContactPhoto photo = { .type = E_CONTACT_PHOTO_TYPE_INLINED };
      gsize length;
      gconstpointer data = g_bytes_get_data (editor->photo_bytes, &length);

      photo.data.inlined.mime_type = (char *) "image/png";
      photo.data.inlined.data = (guchar *) data;
      photo.data.inlined.length = length;
      e_contact_set (contact, E_CONTACT_PHOTO, &photo);
    }
  else
    e_contact_set (contact, E_CONTACT_PHOTO, NULL);
  return contact;
}

/* A contact EDS has just added reaches the store when its view says so, a moment after the add returns: the card
 * opens then (or not at all after 3 s). The editor is gone by then, so this keeps what it needs. */
typedef struct {
  GtkWidget *window;
  CtStore *store;
  char *uid;
  gulong handler;
  guint timeout;
} CardWaiter;

static gboolean
contact_is_there (CtStore *store, const char *uid)
{
  return g_str_equal (uid, CT_PROFILE_UID) ? ct_store_get_profile (store) != NULL : ct_store_lookup (store, uid) != NULL;
}

static void
card_waiter_free (CardWaiter *waiter)
{
  g_clear_signal_handler (&waiter->handler, waiter->store);
  g_clear_handle_id (&waiter->timeout, g_source_remove);
  g_object_unref (waiter->window);
  g_object_unref (waiter->store);
  g_free (waiter->uid);
  g_free (waiter);
}

static void
on_store_has_card (CtStore *store, CardWaiter *waiter)
{
  if (!contact_is_there (store, waiter->uid))
    return;
  ct_window_show_contact (waiter->window, waiter->uid);
  card_waiter_free (waiter);
}

static gboolean
on_card_timeout (gpointer data)
{
  CardWaiter *waiter = data;

  waiter->timeout = 0;
  card_waiter_free (waiter);
  return G_SOURCE_REMOVE;
}

static void
show_card (GtkWidget *window, CtStore *store, const char *uid)
{
  CardWaiter *waiter;

  if (contact_is_there (store, uid))
    {
      ct_window_show_contact (window, uid);
      return;
    }
  waiter = g_new0 (CardWaiter, 1);
  waiter->window = g_object_ref (window);
  waiter->store = g_object_ref (store);
  waiter->uid = g_strdup (uid);
  waiter->handler = g_signal_connect (store, g_str_equal (uid, CT_PROFILE_UID) ? "profile-changed" : "changed",
                                      G_CALLBACK (on_store_has_card), waiter);
  waiter->timeout = g_timeout_add_seconds (3, on_card_timeout, waiter);
}

static void
close_editor (Editor *editor, const char *saved_uid)
{
  GtkWidget *window = editor->window;
  CtStore *store = editor->store;
  gboolean from_quick = editor->from_quick;
  g_autofree char *uid = g_strdup (saved_uid);

  g_object_ref (store);
  ct_window_pop (window, editor->page);
  /* the saved contact's card, unless the editor was opened from it (it is under the editor, and up to date) */
  if (uid && !from_quick)
    show_card (window, store, uid);
  g_object_unref (store);
}

static void
on_saved (const char *uid, const GError *error, gpointer data)
{
  Editor *editor = data;
  g_autofree char *name = NULL;
  g_autofree char *text = NULL;

  if (error)
    {
      editor->saving = FALSE;
      gtk_widget_set_sensitive (editor->content, TRUE);
      gtk_widget_set_sensitive (editor->save, TRUE);
      ct_window_toast (editor->window, _("Couldn't save contact changes"));
      return;
    }
  name = g_strdup (editor->saved_name);
  /* contactSavedNamedToast "%s saved", or "Contact saved" without a name */
  text = name && *name ? g_strdup_printf (_("%s saved"), name) : g_strdup (_("Contact saved"));
  ct_window_toast (editor->window, text);
  close_editor (editor, uid);
}

static void
write_contact (Editor *editor, EContact *contact)
{
  /* the name for the toast, from what is saved (a new contact is not in the store yet when the add returns) */
  g_free (editor->saved_name);
  editor->saved_name = ct_contact_display_name (contact, ct_display_order (editor->settings));
  if (editor->profile)
    {
      ct_store_save_profile (editor->store, contact, on_saved, editor);
      return;
    }
  if (!editor->book_uid)
    editor->book_uid = g_strdup (ct_store_default_book (editor->store));
  ct_store_save (editor->store, editor->book_uid, contact, editor->is_new, on_saved, editor);
}

/* the save stopped before anything was written: the editor is the user's again */
static void
save_stopped (Editor *editor)
{
  editor->saving = FALSE;
  gtk_widget_set_sensitive (editor->content, TRUE);
  gtk_widget_set_sensitive (editor->save, TRUE);
}

typedef struct {
  Editor *editor;
  EContact *contact;
} Pending;

static void
pending_free (Pending *pending)
{
  g_object_unref (pending->contact);
  g_free (pending);
}

static void
on_signed_confirmed (int response, gpointer data)
{
  Pending *pending = data;

  if (response == CT_DIALOG_OK)
    write_contact (pending->editor, pending->contact);
  else
    save_stopped (pending->editor);
  pending_free (pending);
}

static void
on_owner_refused (int response, gpointer data)
{
  Pending *pending = data;

  save_stopped (pending->editor);
  pending_free (pending);
}

/* the card's role for Minerva: a change to what its signature covers breaks it (docs/contacts-roles.md, "Editing a
 * signed field"); the owner's card is never broken from the phone, a signed role's only after the user confirms */
static void
on_signed_role (JsonObject *root, const char *err, gpointer data)
{
  Pending *pending = data;
  Editor *editor = pending->editor;
  const char *state = err ? "" : ns_json_str (ns_json_obj (root, "role"), "state");

  if (g_strcmp0 (state, "owner") == 0)
    ct_confirm_dialog (editor->window, ct_mnv_text ("signed_heading"), ct_mnv_text ("signed_owner_block"),
                       ct_mnv_text ("signed_owner_ok"), on_owner_refused, pending);
  else if (g_strcmp0 (state, "signed") == 0)
    ct_confirm_dialog (editor->window, ct_mnv_text ("signed_heading"), ct_mnv_text ("signed_role_confirm"),
                       ct_mnv_text ("signed_role_ok"), on_signed_confirmed, pending);
  else
    {
      write_contact (editor, pending->contact);
      pending_free (pending);
    }
}

/* whether the save changes what a signature over the card covers (people ui unsigned_fields: the token service's) */
static gboolean
signed_fields_change (Editor *editor, EContact *contact)
{
  JsonArray *list = ct_mnv_list ("unsigned_fields");
  g_autoptr(GPtrArray) fields = g_ptr_array_new ();
  g_autofree char *before = NULL, *after = NULL;

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    g_ptr_array_add (fields, (gpointer) json_array_get_string_element (list, i));
  g_ptr_array_add (fields, NULL);
  before = ct_contact_signed_text (editor->original, (const char *const *) fields->pdata);
  after = ct_contact_signed_text (contact, (const char *const *) fields->pdata);
  return g_strcmp0 (before, after) != 0;
}

static void
save (Editor *editor)
{
  g_autoptr(EContact) contact = NULL;

  if (editor->saving)
    return;
  /* nothing changed: an existing contact goes back to its card, a new empty one just closes */
  if (!has_changes (editor))
    {
      close_editor (editor, editor->is_new ? NULL : editor->profile ? CT_PROFILE_UID : editor->uid);
      return;
    }
  editor->saving = TRUE;
  gtk_widget_set_sensitive (editor->content, FALSE);
  gtk_widget_set_sensitive (editor->save, FALSE);
  gtk_window_set_focus (GTK_WINDOW (gtk_widget_get_root (editor->page)), NULL);
  contact = build_contact (editor);
  if (!editor->is_new && !editor->profile && editor->uid && ct_mnv_ui_ready () && signed_fields_change (editor, contact))
    {
      Pending *pending = g_new0 (Pending, 1);

      pending->editor = editor;
      pending->contact = g_object_ref (contact);
      ct_mnv_by_contact (G_OBJECT (editor->page), editor->uid, on_signed_role, pending);
      return;
    }
  write_contact (editor, contact);
}

static void
on_save (NeoPressable *button, Editor *editor)
{
  save (editor);
}

static void
on_discard (int response, gpointer data)
{
  Editor *editor = data;

  if (response == CT_DIALOG_OK)
    close_editor (editor, NULL);
}

/* ✕, Back: away at once without changes, else "Discard changes?" (CancelEditDialogFragment) */
static gboolean
on_back (CtPage *page, gpointer data)
{
  Editor *editor = data;

  if (editor->saving)
    return TRUE;
  if (!has_changes (editor))
    return FALSE;
  ct_confirm_dialog (editor->page, NULL, _("Discard changes?"), _("Discard"), on_discard, editor);
  return TRUE;
}

/* a new contact: First name has the focus and the keyboard comes up */
static void
on_map (GtkWidget *widget, Editor *editor)
{
  /* a prefill's picture (an imported chat's); one that cannot be read leaves the photo empty, as it was */
  if (editor->prefill_photo)
    {
      g_autoptr(GFile) file = g_steal_pointer (&editor->prefill_photo);

      photo_from_file (editor, file);
    }
  if (editor->is_new && editor->name_fields[1])
    gtk_widget_grab_focus (editor->name_fields[1]);
}

static void
editor_free (gpointer data)
{
  Editor *editor = data;

  for (int i = 0; i < N_SECTIONS; i++)
    g_clear_pointer (&editor->sections[i].row_list, g_ptr_array_unref);
  g_clear_pointer (&editor->name, ct_name_free);
  g_clear_pointer (&editor->labels, g_ptr_array_unref);
  g_clear_pointer (&editor->channel_rows, g_ptr_array_unref);
  g_clear_pointer (&editor->photo_bytes, g_bytes_unref);
  g_clear_object (&editor->photo_texture);
  g_clear_object (&editor->prefill_photo);
  g_clear_object (&editor->original);
  g_free (editor->uid);
  g_free (editor->book_uid);
  g_free (editor->initial_state);
  g_free (editor->saved_name);
  g_signal_handlers_disconnect_by_data (editor->store, editor);
  g_object_unref (editor->store);
  g_object_unref (editor->settings);
  g_free (editor);
}

/* the rows of each section from @contact (and @extra's, appended: the number another app is adding) */
static void
load_rows (Editor *editor, EContact *contact, EContact *extra)
{
  for (int s = S_NICKNAME; s < S_LABELS; s++)
    {
      Section *section = &editor->sections[s];
      g_autoptr(GPtrArray) items = ct_contact_get_items (contact, section->kind->kind);
      g_autoptr(GPtrArray) more = extra ? ct_contact_get_items (extra, section->kind->kind) : NULL;

      for (guint i = 0; i < items->len; i++)
        if (!ct_item_is_empty (items->pdata[i]))
          {
            add_row (section, ct_item_copy (items->pdata[i]), FALSE);
            section->has_data = TRUE;
          }
      for (guint i = 0; more && i < more->len; i++)
        if (!ct_item_is_empty (more->pdata[i]))
          {
            /* a new row, not the other card's property */
            CtItem *item = ct_item_copy (more->pdata[i]);
            Row *row;

            item->source = NULL;
            row = add_row (section, NULL, FALSE);
            row->type = item->type;
            row->label = g_strdup (item->label);
            if (row->n_fields)
              gtk_editable_set_text (GTK_EDITABLE (row->fields[0]), item->value);
            ct_item_free (item);
            section->has_data = TRUE;
          }
    }
}

GtkWidget *
ct_editor_new (GtkWidget *window, const char *uid, EContact *prefill)
{
  Editor *editor = g_new0 (Editor, 1);
  GtkWidget *page = ct_page_new (window);
  GtkWidget *root = g_object_new (GTK_TYPE_BOX, "orientation", GTK_ORIENTATION_VERTICAL,
                                  "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  GtkWidget *toolbar, *actions, *title, *shadow, *frame;
  GtkWidget *top = ct_window_top (window);
  GtkWidget *more_label;
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);
  EContact *loaded = NULL;
  GBytes *photo = NULL;
  g_autoptr(GPtrArray) labels = NULL;

  editor->page = page;
  editor->window = window;
  editor->store = g_object_ref (ct_window_get_store (window));
  editor->settings = g_object_ref (ct_window_get_settings (window));
  editor->profile = g_strcmp0 (uid, CT_PROFILE_UID) == 0;
  editor->from_quick = top && g_object_get_data (G_OBJECT (top), "ct-quick");
  if (editor->profile)
    loaded = ct_store_get_profile (editor->store);
  else if (uid)
    {
      CtEntry *entry = ct_store_lookup (editor->store, uid);

      if (entry)
        {
          loaded = entry->contact;
          editor->book_uid = g_strdup (entry->book_uid);
          if (entry->photo)
            photo = entry->photo;
        }
      else
        ct_window_toast (window, _("Failed to open editor."));
    }
  editor->is_new = loaded == NULL;
  editor->uid = !editor->is_new && !editor->profile ? g_strdup (uid) : NULL;
  editor->original = loaded ? e_contact_duplicate (loaded) : e_contact_new ();
  if (!editor->book_uid)
    editor->book_uid = g_strdup (ct_store_default_book (editor->store));
  if (editor->profile && loaded)
    {
      EContactPhoto *picture = e_contact_get (loaded, E_CONTACT_PHOTO);

      if (picture && picture->type == E_CONTACT_PHOTO_TYPE_INLINED && picture->data.inlined.length)
        photo = g_bytes_new (picture->data.inlined.data, picture->data.inlined.length);
      if (picture)
        e_contact_photo_free (picture);
    }
  /* a new contact's picture from its prefill: inlined, or a file (an imported chat's) read once on screen */
  if (editor->is_new && prefill)
    {
      EContactPhoto *picture = e_contact_get (prefill, E_CONTACT_PHOTO);

      if (picture && picture->type == E_CONTACT_PHOTO_TYPE_INLINED && picture->data.inlined.length)
        photo = g_bytes_new (picture->data.inlined.data, picture->data.inlined.length);
      else if (picture && picture->type == E_CONTACT_PHOTO_TYPE_URI && picture->data.uri && *picture->data.uri)
        editor->prefill_photo = g_file_new_for_uri (picture->data.uri);
      if (picture)
        e_contact_photo_free (picture);
    }
  /* the photo's background: the card's own colour (it was opened from QuickContact) or #607d8b */
  editor->photo_tint = !editor->is_new ? ct_avatar_header_color (editor->profile ? CT_PROFILE_UID : uid)
                                       : CT_C_QC_DEFAULT_TINT;
  editor->name = ct_contact_get_name (prefill && editor->is_new ? prefill : editor->original);
  labels = ct_contact_dup_labels (editor->original);
  editor->labels = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < labels->len; i++)
    g_ptr_array_add (editor->labels, g_strdup (labels->pdata[i]));

  g_object_set_data_full (G_OBJECT (page), "ct-editor", editor, editor_free);
  ct_page_set_back_handler (CT_PAGE (page), on_back, editor);
  gtk_widget_add_css_class (page, "ct-editor");

  /* the toolbar: ✕ "Cancel", the title 72 dp in, SAVE flush with the end (§4.5) */
  toolbar = ct_page_toolbar (CT_PAGE (page), "ct-editor-toolbar", "ct-close-symbolic", _("Cancel"),
                             editor->is_new ? _("Create new contact") : _("Edit contact"), &actions, &title);
  editor->save = ct_text_button (_("Save"), "ct-editor-save");
  gtk_widget_set_valign (editor->save, GTK_ALIGN_CENTER);
  g_signal_connect (editor->save, "clicked", G_CALLBACK (on_save), editor);
  gtk_box_append (GTK_BOX (actions), editor->save);
  gtk_box_append (GTK_BOX (root), toolbar);

  editor->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (editor->content, "ct-ed-content");
  gtk_box_append (GTK_BOX (editor->content), photo_area (editor));
  set_photo (editor, photo);
  if (photo && (editor->profile || (editor->is_new && prefill)))
    g_bytes_unref (photo);
  gtk_box_append (GTK_BOX (editor->content), account_header (editor));

  gtk_box_append (GTK_BOX (editor->content), name_section (editor));
  for (int s = S_NICKNAME; s < S_LABELS; s++)
    gtk_box_append (GTK_BOX (editor->content), section_new (editor, s));
  gtk_box_append (GTK_BOX (editor->content), labels_section (editor));
  editor->channels = channels_section (editor, prefill);
  gtk_box_append (GTK_BOX (editor->content), editor->channels);
  if (!editor->is_new && !editor->profile && editor->channel_rows->len)
    ct_mnv_imports_load (G_OBJECT (page), on_imports, editor);
  load_rows (editor, editor->is_new && prefill ? prefill : editor->original,
             !editor->is_new ? prefill : NULL);

  /* "More fields": 54 dp, 72 dp in, 16 sp #81b5bc */
  more_label = ct_label (_("More fields"), "ct-ed-more-text", 16);
  gtk_widget_set_valign (more_label, GTK_ALIGN_CENTER);
  editor->more_fields = neo_pressable_new (more_label);
  gtk_widget_add_css_class (editor->more_fields, "ct-ed-more");
  neo_pressable_set_ripple (NEO_PRESSABLE (editor->more_fields), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (editor->more_fields), &ripple);
  g_signal_connect (editor->more_fields, "clicked", G_CALLBACK (on_more_fields), editor);
  gtk_box_append (GTK_BOX (editor->content), editor->more_fields);
  editor->legacy = legacy_section (editor);
  gtk_box_append (GTK_BOX (editor->content), editor->legacy);
  for (int s = 0; s < N_SECTIONS; s++)
    update_section (&editor->sections[s]);

  editor->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (editor->scroll), editor->content);
  gtk_widget_set_vexpand (editor->scroll, TRUE);
  frame = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (frame), editor->scroll);
  /* the toolbar's 4 dp elevation over the content */
  shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (shadow, "ct-ed-toolbar-shadow");
  gtk_widget_set_valign (shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (shadow, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (frame), shadow);
  gtk_widget_set_vexpand (frame, TRUE);
  gtk_box_append (GTK_BOX (root), frame);
  gtk_widget_set_vexpand (root, TRUE);
  gtk_box_append (GTK_BOX (page), root);

  /* the screen as loaded; a prefilled new contact starts changed (the number from the Dialer is worth keeping) */
  {
    char *state = editor_state (editor);

    if (editor->is_new && prefill)
      {
        g_free (state);
        state = g_strdup ("");
      }
    editor->initial_state = state;
  }
  g_signal_connect_after (page, "map", G_CALLBACK (on_map), editor);
  return page;
}
