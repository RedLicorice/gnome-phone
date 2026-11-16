/* cam-qr-sheet: see cam-qr-sheet.h. */
#include "cam-qr-sheet.h"
#include "cam-dialogs.h"
#include "cam-key-import.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <string.h>

#define SHEET_MAX_W      640      /* M3 bottom sheet's maxWidth */
#define SHEET_RADIUS     28
#define SCRIM_ALPHA      0.32f
#define ENTER_MS         250      /* BottomSheetBehavior's settle, standard curve */
#define EXIT_MS          200

/* ---- what the code says (zxing's ResultParsers, as QrTextClassifier uses them) --------------------------------- */

void
cam_qr_result_free (CamQrResult *result)
{
  if (!result)
    return;
  g_free (result->data);
  g_free (result->uri);
  g_free (result->content);
  g_free (result->provider);
  g_free (result);
}

static gboolean
has_prefix_ci (const char *text, const char *prefix)
{
  return g_ascii_strncasecmp (text, prefix, strlen (prefix)) == 0;
}

static gboolean
all_digits (const char *text, gsize len)
{
  if (strlen (text) != len)
    return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isdigit (text[i]))
      return FALSE;
  return TRUE;
}

/* the UPC/EAN check digit (ProductResultParser takes only codes of those formats) */
static gboolean
ean_valid (const char *digits)
{
  gsize len = strlen (digits);
  int sum = 0;

  for (gsize i = 0; i + 1 < len; i++)
    sum += (digits[len - 2 - i] - '0') * (i % 2 == 0 ? 3 : 1);
  return (10 - sum % 10) % 10 == digits[len - 1] - '0';
}

/* VINResultParser: 17 characters without I, O, Q, and a valid check digit */
static gboolean
vin_valid (const char *vin)
{
  static const int weights[17] = { 8, 7, 6, 5, 4, 3, 2, 10, 0, 9, 8, 7, 6, 5, 4, 3, 2 };
  int sum = 0, check;

  if (strlen (vin) != 17)
    return FALSE;
  for (int i = 0; i < 17; i++)
    {
      char c = vin[i];
      int value;

      if (g_ascii_isdigit (c))
        value = c - '0';
      else if (c >= 'A' && c <= 'H')
        value = c - 'A' + 1;
      else if (c >= 'J' && c <= 'N')
        value = c - 'J' + 1;
      else if (c == 'P')
        value = 7;
      else if (c == 'R')
        value = 9;
      else if (c >= 'S' && c <= 'Z')
        value = c - 'S' + 2;
      else
        return FALSE;
      sum += value * weights[i];
    }
  check = sum % 11;
  return vin[8] == (check == 10 ? 'X' : '0' + check);
}

/* one field of a MECARD / MATMSG / WIFI code: "KEY:value;" with \ escapes */
static char *
field (const char *text, const char *key)
{
  g_autofree char *needle = g_strconcat (key, ":", NULL);
  const char *start = text;

  while ((start = strstr (start, needle)))
    {
      /* a field starts the code (after its "MECARD:") or follows a ';' */
      if (start == text || start[-1] == ';' || start[-1] == ':')
        {
          GString *value = g_string_new (NULL);

          for (const char *p = start + strlen (needle); *p && *p != ';'; p++)
            {
              if (*p == '\\' && p[1])
                p++;
              g_string_append_c (value, *p);
            }
          return g_string_free (value, FALSE);
        }
      start += strlen (needle);
    }
  return NULL;
}

/* a vCard / iCalendar property's value, unfolded enough for one line */
static char *
property (const char *text, const char *name)
{
  g_auto(GStrv) lines = g_regex_split_simple ("\\r?\\n", text, 0, 0);

  for (int i = 0; lines[i]; i++)
    {
      const char *colon = strchr (lines[i], ':');
      g_autofree char *key = colon ? g_strndup (lines[i], colon - lines[i]) : NULL;
      char *semi = key ? strchr (key, ';') : NULL;

      if (semi)
        *semi = 0;
      if (key && !g_ascii_strcasecmp (key, name))
        return g_strdup (colon + 1);
    }
  return NULL;
}

static char *
mecard_to_vcard (const char *text, char **name_out)
{
  g_autofree char *name = field (text, "N"), *tel = field (text, "TEL"), *email = field (text, "EMAIL");
  g_autofree char *adr = field (text, "ADR"), *url = field (text, "URL"), *note = field (text, "NOTE");
  g_autofree char *org = field (text, "ORG");
  GString *card = g_string_new ("BEGIN:VCARD\r\nVERSION:3.0\r\n");
  g_autofree char *display = NULL;

  /* MECARD's N is "Last,First" */
  if (name)
    {
      g_auto(GStrv) parts = g_strsplit (name, ",", 2);

      display = parts[1] ? g_strdup_printf ("%s %s", g_strstrip (parts[1]), g_strstrip (parts[0])) : g_strdup (name);
      g_string_append_printf (card, "N:%s;%s;;;\r\nFN:%s\r\n", parts[0], parts[1] ? parts[1] : "", display);
    }
  if (org)
    g_string_append_printf (card, "ORG:%s\r\n", org);
  if (tel)
    g_string_append_printf (card, "TEL:%s\r\n", tel);
  if (email)
    g_string_append_printf (card, "EMAIL:%s\r\n", email);
  if (adr)
    g_string_append_printf (card, "ADR:;;%s;;;;\r\n", adr);
  if (url)
    g_string_append_printf (card, "URL:%s\r\n", url);
  if (note)
    g_string_append_printf (card, "NOTE:%s\r\n", note);
  g_string_append (card, "END:VCARD\r\n");
  *name_out = g_strdup (display ? display : org ? org : "");
  return g_string_free (card, FALSE);
}

static char *
escape (const char *text)
{
  return g_uri_escape_string (text ? text : "", NULL, FALSE);
}

void
cam_scrub (char *secret)
{
  if (!secret)
    return;
  explicit_bzero (secret, strlen (secret));
  g_free (secret);
}

#define KEY_SCHEME "minerva-key:"

gboolean
cam_qr_is_key (const char *text)
{
  while (*text && g_ascii_isspace (*text))
    text++;
  return has_prefix_ci (text, KEY_SCHEME);
}

/* the provider id of a key code for the sheet's line: valid UTF-8 without control characters, at most 48
 * characters; NULL when nothing is left */
static char *
key_provider (const char *decoded)
{
  GString *clean = g_string_new (NULL);
  glong chars = 0;

  if (!decoded || !g_utf8_validate (decoded, -1, NULL))
    {
      g_string_free (clean, TRUE);
      return NULL;
    }
  for (const char *p = decoded; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_iscntrl (c) || !g_unichar_isdefined (c))
        continue;
      if (chars == 48)
        {
          g_string_append (clean, "…");
          break;
        }
      g_string_append_unichar (clean, c);
      chars++;
    }
  g_strstrip (clean->str);
  if (!clean->str[0])
    {
      g_string_free (clean, TRUE);
      return NULL;
    }
  return g_string_free (clean, FALSE);
}

/* "minerva-key:v1?p=<provider>&k=<key>" or "&e=<encrypted>", or a pairing code
 * "minerva-key:v1?p=<provider>&c=<chat>&t=<token>". Only the provider is ever copied out; the key part and the
 * pairing token are looked at in place and not kept. */
static void
classify_key (CamQrResult *result, const char *text)
{
  const char *rest = text + strlen (KEY_SCHEME);
  const char *query = strchr (rest, '?');
  g_auto(GStrv) params = NULL;
  gboolean has_k = FALSE, has_e = FALSE, has_c = FALSE, has_t = FALSE;

  result->kind = CAM_QR_KEY;
  if (!query || (query - rest != 2) || g_ascii_strncasecmp (rest, "v1", 2))
    {
      result->data = g_strdup (_("Not a code this app can import"));
      return;
    }
  params = g_strsplit (query + 1, "&", -1);
  for (int i = 0; params[i]; i++)
    {
      const char *value = strchr (params[i], '=');
      gsize name_len = value ? (gsize) (value - params[i]) : 0;

      if (!value || value[1] == 0)
        continue;
      value++;
      if (name_len == 1 && params[i][0] == 'p' && !result->provider)
        {
          g_autofree char *decoded = g_uri_unescape_string (value, NULL);

          result->provider = key_provider (decoded);
        }
      else if (name_len == 1 && params[i][0] == 'k')
        has_k = TRUE;
      else if (name_len == 1 && params[i][0] == 'e')
        has_e = TRUE;
      else if (name_len == 1 && params[i][0] == 'c')
        has_c = TRUE;
      else if (name_len == 1 && params[i][0] == 't')
        has_t = TRUE;
    }
  /* the pairing code: the same scheme, a chat and a token in place of the key. Whether its provider pairs at all is
   * asked of minerva-config when the sheet opens (cam-qr-sheet_show), not here: this stays a pure reading of the text */
  if (has_c && has_t && !has_k && !has_e)
    {
      if (!result->provider)
        {
          result->data = g_strdup (_("Not a code this app can import"));
          return;
        }
      result->kind = CAM_QR_PAIR;
      result->importable = TRUE;
      result->data = g_strdup_printf (_("%s • pairing"), result->provider);
      return;
    }
  result->encrypted = has_e && !has_k;
  result->importable = result->provider && (has_k != has_e);
  if (!result->importable)
    {
      g_clear_pointer (&result->provider, g_free);
      result->encrypted = FALSE;
      result->data = g_strdup (_("Not a code this app can import"));
    }
  else if (result->encrypted)
    result->data = g_strdup_printf (_("%s • passphrase needed"), result->provider);
  else
    result->data = g_strdup (result->provider);
}

CamQrResult *
cam_qr_classify (const char *raw)
{
  CamQrResult *result = g_new0 (CamQrResult, 1);
  g_autofree char *text = g_strstrip (g_strdup (raw));

  result->kind = CAM_QR_TEXT;
  if (has_prefix_ci (text, KEY_SCHEME))
    {
      classify_key (result, text);
      explicit_bzero (text, strlen (text));       /* the copy made for stripping held the key */
    }
  else if (has_prefix_ci (text, "BEGIN:VCARD"))
    {
      char *fn = property (text, "FN");

      result->kind = CAM_QR_CONTACT;
      result->data = fn ? fn : property (text, "N");
      result->content = g_strdup (text);
      result->mime = "text/vcard";
    }
  else if (has_prefix_ci (text, "MECARD:"))
    {
      result->kind = CAM_QR_CONTACT;
      result->content = mecard_to_vcard (text + 7, &result->data);
      result->mime = "text/vcard";
    }
  else if (strstr (text, "BEGIN:VEVENT"))
    {
      result->kind = CAM_QR_EVENT;
      result->data = property (text, "SUMMARY");
      result->content = strstr (text, "BEGIN:VCALENDAR") ? g_strdup (text)
                        : g_strdup_printf ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//neo-camera//EN\r\n%s\r\n"
                                           "END:VCALENDAR\r\n", text);
      result->mime = "text/calendar";
    }
  else if (has_prefix_ci (text, "mailto:"))
    {
      const char *query = strchr (text, '?');

      result->kind = CAM_QR_EMAIL;
      result->data = g_uri_unescape_segment (text + 7, query, NULL);
      result->uri = g_strconcat ("mailto:", text + 7, NULL);
    }
  else if (has_prefix_ci (text, "MATMSG:") || has_prefix_ci (text, "SMTP:"))
    {
      g_autofree char *to = NULL, *subject = NULL, *body = NULL;
      g_autofree char *to_e = NULL, *subject_e = NULL, *body_e = NULL;

      if (has_prefix_ci (text, "MATMSG:"))
        to = field (text + 7, "TO"), subject = field (text + 7, "SUB"), body = field (text + 7, "BODY");
      else
        {
          /* SMTP:to:subject:body */
          g_auto(GStrv) parts = g_strsplit (text + 5, ":", 3);

          to = g_strdup (parts[0]);
          subject = parts[0] && parts[1] ? g_strdup (parts[1]) : NULL;
          body = parts[0] && parts[1] && parts[2] ? g_strdup (parts[2]) : NULL;
        }
      result->kind = CAM_QR_EMAIL;
      result->data = g_strdup (to ? to : "");
      to_e = escape (to), subject_e = escape (subject), body_e = escape (body);
      result->uri = g_strdup_printf ("mailto:%s?subject=%s&body=%s", to_e, subject_e, body_e);
    }
  else if (has_prefix_ci (text, "geo:"))
    {
      g_autofree char *coordinates = g_strndup (text + 4, strcspn (text + 4, "?;"));
      g_auto(GStrv) parts = g_strsplit (coordinates, ",", 3);

      result->kind = CAM_QR_GEO;
      /* GeoParsedResult's display: "lat, lon" and the altitude in metres */
      result->data = parts[0] && parts[1]
                     ? (parts[2] && g_ascii_strtod (parts[2], NULL) > 0
                        ? g_strdup_printf ("%s, %s, %sm", parts[0], parts[1], parts[2])
                        : g_strdup_printf ("%s, %s", parts[0], parts[1]))
                     : g_strdup (coordinates);
      result->uri = g_strconcat ("geo:", text + 4, NULL);
    }
  else if (has_prefix_ci (text, "tel:"))
    {
      result->kind = CAM_QR_TEL;
      result->data = g_strdup (text + 4);
      result->uri = g_strconcat ("tel:", text + 4, NULL);
    }
  else if (has_prefix_ci (text, "smsto:") || has_prefix_ci (text, "sms:") || has_prefix_ci (text, "mmsto:")
           || has_prefix_ci (text, "mms:"))
    {
      const char *rest = strchr (text, ':') + 1;
      g_autofree char *number = NULL, *body = NULL, *body_e = NULL;
      const char *query = strchr (rest, '?');

      if (query)
        {
          number = g_strndup (rest, query - rest);
          if (has_prefix_ci (query + 1, "body="))
            body = g_uri_unescape_string (query + 6, NULL);
        }
      else if (has_prefix_ci (text, "smsto:") || has_prefix_ci (text, "mmsto:"))
        {
          /* SMSTO:number:body */
          const char *colon = strchr (rest, ':');

          number = colon ? g_strndup (rest, colon - rest) : g_strdup (rest);
          body = colon ? g_strdup (colon + 1) : NULL;
        }
      else
        number = g_strdup (rest);
      /* SMSParsedResult: the first of several numbers */
      if (strchr (number, ','))
        *strchr (number, ',') = 0;
      result->kind = CAM_QR_SMS;
      result->data = g_strdup (number);
      body_e = body ? escape (body) : NULL;
      result->uri = body_e ? g_strdup_printf ("sms:%s?body=%s", number, body_e) : g_strdup_printf ("sms:%s", number);
    }
  else if (has_prefix_ci (text, "WIFI:"))
    {
      result->kind = CAM_QR_WIFI;
      result->data = field (text + 5, "S");
    }
  else if (all_digits (text, 13) && (g_str_has_prefix (text, "978") || g_str_has_prefix (text, "979"))
           && ean_valid (text))
    {
      result->kind = CAM_QR_ISBN;
      result->data = g_strdup (text);
      result->uri = g_strdup_printf ("https://isbnsearch.org/isbn/%s", text);
    }
  else if ((all_digits (text, 8) || all_digits (text, 12) || all_digits (text, 13)) && ean_valid (text))
    {
      result->kind = CAM_QR_PRODUCT;
      result->data = g_strdup (text);
      result->uri = g_strdup_printf ("https://www.barcodelookup.com/%s", text);
    }
  else if (vin_valid (text))
    {
      result->kind = CAM_QR_VIN;
      result->data = g_strdup (text);
      result->uri = g_strdup_printf ("https://www.vindecoderz.com/EN/check-lookup/%s", text);
    }
  else if (has_prefix_ci (text, "http://") || has_prefix_ci (text, "https://"))
    {
      result->kind = CAM_QR_URL;
      result->data = g_strdup (text);
      result->uri = g_strdup (text);
    }
  else if (!strchr (text, ' ') && !strchr (text, '\n')
           && g_regex_match_simple ("^[a-z0-9-]+(\\.[a-z0-9-]+)*\\.[a-z]{2,}(:[0-9]+)?([/?#].*)?$", text,
                                    G_REGEX_CASELESS, 0))
    {
      /* URIParsedResult: a bare host gets http:// */
      result->kind = CAM_QR_URL;
      result->data = g_strdup (text);
      result->uri = g_strconcat ("http://", text, NULL);
    }
  else if (g_regex_match_simple ("^[^@\\s]+@[^@\\s]+\\.[a-z]{2,}$", text, G_REGEX_CASELESS, 0))
    {
      /* EmailAddressResultParser takes a bare address too */
      result->kind = CAM_QR_EMAIL;
      result->data = g_strdup (text);
      result->uri = g_strconcat ("mailto:", text, NULL);
    }
  if (!result->data)
    result->data = g_strdup (text);
  return result;
}

/* the app that would do it, or NULL: then the code is just Text */
static GAppInfo *
handler (CamQrResult *result)
{
  g_autofree char *scheme = NULL;

  switch (result->kind)
    {
    case CAM_QR_CONTACT:
    case CAM_QR_EVENT:
      return g_app_info_get_default_for_type (result->mime, FALSE);
    case CAM_QR_TEXT:
    case CAM_QR_WIFI:
    case CAM_QR_KEY:            /* imported by this app itself (cam-key-import) */
    case CAM_QR_PAIR:           /* paired by this app itself (cam-key-import) */
      return NULL;
    default:
      scheme = result->uri ? g_uri_parse_scheme (result->uri) : NULL;
      return scheme ? g_app_info_get_default_for_uri_scheme (scheme) : NULL;
    }
}

static const char *
action_title (CamQrKind kind)
{
  switch (kind)
    {
    case CAM_QR_URL: return _("Open URL");
    case CAM_QR_CONTACT: return _("Add contact");
    case CAM_QR_EVENT: return _("Add event to calendar");
    case CAM_QR_EMAIL: return _("Send a new email");
    case CAM_QR_GEO: return _("Open this location");
    case CAM_QR_TEL: return _("Call phone number");
    case CAM_QR_SMS: return _("Send a new SMS");
    case CAM_QR_ISBN: return _("Lookup this ISBN");
    case CAM_QR_PRODUCT: return _("Lookup product");
    case CAM_QR_VIN: return _("Lookup VIN");
    case CAM_QR_KEY: return _("Minerva API key");
    case CAM_QR_PAIR: return _("Minerva pairing");
    default: return _("Text");
    }
}

static const char *
action_icon (CamQrKind kind)
{
  switch (kind)
    {
    case CAM_QR_URL: return "cam-open-in-browser-symbolic";
    case CAM_QR_CONTACT: return "cam-contact-phone-symbolic";
    case CAM_QR_EVENT: return "cam-calendar-add-on-symbolic";
    case CAM_QR_EMAIL: return "cam-email-symbolic";
    case CAM_QR_GEO: return "cam-location-on-symbolic";
    case CAM_QR_TEL: return "cam-phone-symbolic";
    case CAM_QR_SMS: return "cam-sms-symbolic";
    case CAM_QR_ISBN: return "cam-book-symbolic";
    case CAM_QR_PRODUCT: return "cam-shopping-cart-symbolic";
    case CAM_QR_VIN: return "cam-directions-car-symbolic";
    case CAM_QR_KEY: return "cam-lock-symbolic";
    case CAM_QR_PAIR: return "cam-contact-phone-symbolic";
    default: return NULL;
    }
}

/* ---- the sheet ------------------------------------------------------------------------------------------------- */

#define CAM_TYPE_QR_SHEET (cam_qr_sheet_get_type ())
G_DECLARE_FINAL_TYPE (CamQrSheet, cam_qr_sheet, CAM, QR_SHEET, GtkWidget)

struct _CamQrSheet {
  GtkWidget       parent;
  GtkWidget      *sheet;
  CamQrResult    *result;
  GAppInfo       *app;
  double          shown;          /* 0 … 1: the scrim's fade and the sheet's rise */
  double          drag;           /* dp the finger has pulled the sheet down */
  NeoAnim        *anim;
  gboolean        closing;
  CamQrDismissed  dismissed;
  gpointer        data;
  /* a key code: its widgets to update, the scanned text (wiped with the sheet), and whether a run is going */
  GtkWidget      *title, *line, *import;
  char           *secret;
  gboolean        busy;
};

G_DEFINE_FINAL_TYPE (CamQrSheet, cam_qr_sheet, GTK_TYPE_WIDGET)

static void
cam_qr_sheet_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                      int *min_base, int *nat_base)
{
  *min = *nat = 0;
}

static void
sheet_size (CamQrSheet *self, int width, int *w, int *h)
{
  *w = MIN (width, SHEET_MAX_W);
  gtk_widget_measure (self->sheet, GTK_ORIENTATION_VERTICAL, *w, NULL, h, NULL, NULL);
}

static void
cam_qr_sheet_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CamQrSheet *self = CAM_QR_SHEET (widget);
  int w, h;

  sheet_size (self, width, &w, &h);
  gtk_widget_allocate (self->sheet, w, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - w) / 2.0, height - h)));
}

static void
cam_qr_sheet_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamQrSheet *self = CAM_QR_SHEET (widget);
  GdkRGBA scrim = { 0, 0, 0, SCRIM_ALPHA * self->shown };
  int h = gtk_widget_get_height (self->sheet);

  gtk_snapshot_append_color (snapshot, &scrim, &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                                    gtk_widget_get_height (widget)));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, (1 - self->shown) * h + self->drag));
  gtk_widget_snapshot_child (widget, self->sheet, snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  CamQrSheet *self = data;

  self->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !neo_anim_is_running (anim))
    {
      GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));
      CamQrDismissed dismissed = self->dismissed;
      gpointer dismissed_data = self->data;

      self->dismissed = NULL;
      if (overlay)
        gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
      if (dismissed)
        dismissed (dismissed_data);
    }
}

static void
dismiss (CamQrSheet *self)
{
  int h = gtk_widget_get_height (self->sheet);

  if (self->closing)
    return;
  self->closing = TRUE;
  /* from wherever a drag left it */
  if (h > 0 && self->drag > 0)
    {
      self->shown = MAX (0, self->shown - self->drag / h);
      self->drag = 0;
    }
  neo_anim_start (self->anim, self->shown, 0, EXIT_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static void
on_scrim_pressed (GtkGestureClick *click, int n, double x, double y, CamQrSheet *self)
{
  graphene_rect_t bounds;

  if (gtk_widget_compute_bounds (self->sheet, GTK_WIDGET (self), &bounds)
      && graphene_rect_contains_point (&bounds, &GRAPHENE_POINT_INIT (x, y)))
    return;
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  dismiss (self);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CamQrSheet *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  dismiss (self);
  return TRUE;
}

/* the sheet follows a finger down, and goes when let go past half its height (BottomSheetBehavior's hide) */
static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CamQrSheet *self)
{
  if (self->closing)
    return;
  self->drag = MAX (0, dy);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, CamQrSheet *self)
{
  if (self->closing)
    return;
  if (self->drag > gtk_widget_get_height (self->sheet) / 2.0)
    dismiss (self);
  else
    {
      self->drag = 0;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
toast (CamQrSheet *self, const char *text)
{
  /* the window's own toast (cam-window keeps one on the overlay) */
  GtkWidget *window = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);

  if (window)
    gtk_widget_activate_action (window, "win.toast", "s", text);
}

/* ---- a key code: Import ---------------------------------------------------------------------------------------- */

static void ask_passphrase (CamQrSheet *self, const char *message);

static void
show_line (CamQrSheet *self, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (self->line), text);
  if (error)
    gtk_widget_add_css_class (self->line, "cam-qr-error");
  else
    gtk_widget_remove_css_class (self->line, "cam-qr-error");
}

static void
on_import_done (CamKeyResult *key, gpointer data)
{
  CamQrSheet *self = data;
  g_autofree char *text = NULL;

  self->busy = FALSE;
  if (key->ok)
    {
      text = g_strdup_printf (_("Key for %s saved"), key->name ? key->name : self->result->provider);
      gtk_label_set_text (GTK_LABEL (self->title), _("Key saved"));
      show_line (self, text, FALSE);
      gtk_widget_set_visible (self->import, FALSE);
      cam_scrub (g_steal_pointer (&self->secret));
      toast (self, text);
    }
  else if (key->need_passphrase)
    {
      show_line (self, self->result->data, FALSE);
      gtk_widget_set_sensitive (self->import, TRUE);
      ask_passphrase (self, key->error);
    }
  else
    {
      show_line (self, key->error ? key->error : _("The key could not be saved"), TRUE);
      gtk_widget_set_sensitive (self->import, TRUE);
      toast (self, _("The key could not be saved"));
    }
  g_object_unref (self);
}

static void
run_import (CamQrSheet *self, const char *passphrase)
{
  self->busy = TRUE;
  gtk_widget_set_sensitive (self->import, FALSE);
  show_line (self, _("Saving…"), FALSE);
  cam_key_import_run (self->secret, passphrase, on_import_done, g_object_ref (self));
}

static void
on_pair_done (CamKeyResult *key, gpointer data)
{
  CamQrSheet *self = data;
  g_autofree char *text = NULL;

  self->busy = FALSE;
  if (key->ok)
    {
      text = g_strdup (key->message && *key->message ? key->message : _("Paired"));
      gtk_label_set_text (GTK_LABEL (self->title), _("Paired"));
      show_line (self, text, FALSE);
      gtk_widget_set_visible (self->import, FALSE);
      cam_scrub (g_steal_pointer (&self->secret));
      toast (self, text);
    }
  else
    {
      text = g_strdup (key->error && *key->error ? key->error : _("The code could not be used"));
      show_line (self, text, TRUE);
      gtk_widget_set_sensitive (self->import, TRUE);
      toast (self, text);
    }
  g_object_unref (self);
}

static void
run_pair (CamQrSheet *self)
{
  self->busy = TRUE;
  gtk_widget_set_sensitive (self->import, FALSE);
  show_line (self, _("Pairing…"), FALSE);
  cam_pair_run (self->result->provider, self->secret, on_pair_done, g_object_ref (self));
}

static void
on_passphrase (char *passphrase, gpointer data)
{
  CamQrSheet *self = data;

  if (passphrase && *passphrase && self->secret)
    run_import (self, passphrase);
  cam_scrub (passphrase);
  g_object_unref (self);
}

static void
ask_passphrase (CamQrSheet *self, const char *message)
{
  g_autofree char *text = NULL;

  if (!message || !*message)
    {
      text = g_strdup_printf (_("The %s key is protected. Enter the passphrase it was saved with."),
                              self->result->provider);
      message = text;
    }
  cam_passphrase_dialog (GTK_WIDGET (self), message, on_passphrase, g_object_ref (self));
}

static void
start_import (CamQrSheet *self)
{
  if (self->busy || !self->secret || !self->result->importable)
    return;
  if (self->result->kind == CAM_QR_PAIR)
    run_pair (self);
  else if (self->result->encrypted)
    ask_passphrase (self, NULL);
  else
    run_import (self, NULL);
}

static void
on_import (NeoPressable *button, CamQrSheet *self)
{
  start_import (self);
}

static void
on_card (NeoPressable *card, CamQrSheet *self)
{
  g_autoptr(GdkAppLaunchContext) context = NULL;
  g_autoptr(GError) error = NULL;
  gboolean ok = FALSE;

  if (self->result->kind == CAM_QR_KEY || self->result->kind == CAM_QR_PAIR)
    {
      start_import (self);
      return;
    }
  if (!self->app)
    return;
  context = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (self)));
  if (self->result->content)
    {
      /* a vCard or an event goes to its app as a file, as an ACTION_INSERT with its fields would */
      g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "neo-camera", NULL);
      g_autofree char *path = g_build_filename (dir, self->result->kind == CAM_QR_CONTACT ? "scanned.vcf"
                                                                                          : "scanned.ics", NULL);
      g_autoptr(GFile) file = g_file_new_for_path (path);
      GList files = { file, NULL, NULL };

      g_mkdir_with_parents (dir, 0700);
      ok = g_file_set_contents (path, self->result->content, -1, &error)
           && g_app_info_launch (self->app, &files, G_APP_LAUNCH_CONTEXT (context), &error);
    }
  else
    ok = g_app_info_launch_default_for_uri (self->result->uri, G_APP_LAUNCH_CONTEXT (context), &error);
  if (!ok)
    {
      g_message ("scan: %s", error ? error->message : "no app");
      toast (self, _("No app available to handle this action"));
    }
}

static void
on_copy (NeoPressable *button, CamQrSheet *self)
{
  /* the code's text, as scanned */
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)),
                          g_object_get_data (G_OBJECT (self), "cam-qr-text"));
}

static void
cam_qr_sheet_dispose (GObject *object)
{
  CamQrSheet *self = CAM_QR_SHEET (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->sheet, gtk_widget_unparent);
  g_clear_pointer (&self->result, cam_qr_result_free);
  g_clear_pointer (&self->secret, cam_scrub);
  g_clear_object (&self->app);
  G_OBJECT_CLASS (cam_qr_sheet_parent_class)->dispose (object);
}

static void
cam_qr_sheet_class_init (CamQrSheetClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_qr_sheet_dispose;
  widget_class->measure = cam_qr_sheet_measure;
  widget_class->size_allocate = cam_qr_sheet_size_allocate;
  widget_class->snapshot = cam_qr_sheet_snapshot;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
cam_qr_sheet_init (CamQrSheet *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_scrim_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

static GtkWidget *
build_sheet (CamQrSheet *self)
{
  GtkWidget *sheet = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *handle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gboolean is_key = self->result->kind == CAM_QR_KEY || self->result->kind == CAM_QR_PAIR;
  GtkWidget *title = gtk_label_new (self->app || is_key ? action_title (self->result->kind) : _("Text"));
  GtkWidget *data = gtk_label_new (self->result->data);
  GtkWidget *copy = neo_icon_button_new ("cam-content-copy-symbolic");
  GtkWidget *card;
  GtkGesture *drag = gtk_gesture_drag_new ();
  const char *icon_name = self->app || is_key ? action_icon (self->result->kind) : NULL;

  self->title = title;
  self->line = data;
  gtk_widget_add_css_class (sheet, "cam-qr-sheet");
  /* the drag handle: 32 × 4 dp, 8 dp from the top */
  gtk_widget_set_size_request (handle, 32, 4);
  gtk_widget_set_halign (handle, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (handle, 8);
  gtk_widget_add_css_class (handle, "cam-qr-handle");
  gtk_box_append (GTK_BOX (sheet), handle);

  if (icon_name)
    {
      GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

      gtk_image_set_pixel_size (GTK_IMAGE (icon), 32);
      gtk_widget_set_margin_start (icon, 16);
      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (icon, "cam-qr-icon");
      gtk_box_append (GTK_BOX (row), icon);
    }
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (texts, 16);
  gtk_widget_set_margin_end (texts, 16);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (title, "cam-qr-title");
  gtk_box_append (GTK_BOX (texts), title);
  gtk_label_set_xalign (GTK_LABEL (data), 0);
  gtk_label_set_ellipsize (GTK_LABEL (data), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (data), TRUE);
  gtk_widget_add_css_class (data, "cam-qr-data");
  gtk_box_append (GTK_BOX (texts), data);
  gtk_box_append (GTK_BOX (row), texts);
  if (is_key)
    {
      /* a key and a pairing code are never copied: they hold the secret. Import takes the copy button's place */
      if (self->result->importable)
        {
          self->import = neo_text_button_new (_("Import"));
          gtk_widget_set_valign (self->import, GTK_ALIGN_CENTER);
          gtk_widget_set_margin_end (self->import, 8);
          gtk_widget_add_css_class (self->import, "cam-qr-import");
          g_signal_connect (self->import, "clicked", G_CALLBACK (on_import), self);
          gtk_box_append (GTK_BOX (row), self->import);
        }
      else
        gtk_widget_set_margin_end (texts, 16);
      g_object_ref_sink (copy);
      g_object_unref (copy);
    }
  else
    {
      /* the copy button: 24 dp icon with 5 dp padding, 16 dp from the card's end */
      gtk_widget_set_size_request (copy, 34, 34);
      gtk_widget_set_valign (copy, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_end (copy, 16);
      gtk_widget_add_css_class (copy, "cam-qr-copy");
      gtk_widget_set_tooltip_text (copy, _("Copy to clipboard"));
      neo_pressable_set_ripple (NEO_PRESSABLE (copy), NEO_RIPPLE_CIRCLE, 20);
      g_signal_connect (copy, "clicked", G_CALLBACK (on_copy), self);
      gtk_box_append (GTK_BOX (row), copy);
    }
  card = neo_pressable_new (row);
  neo_pressable_set_ripple (NEO_PRESSABLE (card), NEO_RIPPLE_BOUNDED, 12);
  gtk_widget_set_size_request (card, -1, 72);
  gtk_widget_set_margin_top (card, 12);
  gtk_widget_set_margin_start (card, 16);
  gtk_widget_set_margin_end (card, 16);
  /* the actions grid under it is empty: every code here has one action at most, the card's */
  gtk_widget_set_margin_bottom (card, 16);
  gtk_widget_add_css_class (card, "cam-qr-card");
  g_signal_connect (card, "clicked", G_CALLBACK (on_card), self);
  gtk_box_append (GTK_BOX (sheet), card);

  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_BUBBLE);
  gtk_widget_add_controller (sheet, GTK_EVENT_CONTROLLER (drag));
  return sheet;
}

void
cam_qr_sheet_show (GtkWidget *parent, const char *text, CamQrDismissed dismissed, gpointer data)
{
  GtkOverlay *overlay = neo_window_get_overlay (parent);
  CamQrSheet *self;

  g_return_if_fail (overlay != NULL);
  self = g_object_new (CAM_TYPE_QR_SHEET, NULL);
  self->result = cam_qr_classify (text);
  /* a pairing code for a provider that does not pair is no code here; the list is asked of minerva-config, whose own
   * answer decides (and a call that fails leaves it to the pair command to refuse) */
  if (self->result->kind == CAM_QR_PAIR && !cam_pair_allowed (self->result->provider))
    {
      g_clear_pointer (&self->result->provider, g_free);
      g_free (self->result->data);
      self->result->data = g_strdup (_("Not a code this app can import"));
      self->result->importable = FALSE;
    }
  self->app = handler (self->result);
  self->dismissed = dismissed;
  self->data = data;
  if (self->result->kind == CAM_QR_KEY || self->result->kind == CAM_QR_PAIR)
    self->secret = g_strdup (text);     /* held for the import, wiped with the sheet; not offered to Copy */
  else
    g_object_set_data_full (G_OBJECT (self), "cam-qr-text", g_strdup (text), g_free);
  self->sheet = build_sheet (self);
  gtk_widget_set_parent (self->sheet, GTK_WIDGET (self));
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  neo_anim_start (self->anim, 0, 1, ENTER_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}
