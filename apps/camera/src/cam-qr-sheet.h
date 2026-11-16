/* cam-qr-sheet: what a scanned code says, and what can be done with it (§15.2; QrBottomSheetDialog.kt,
 * L/qr_bottom_sheet_dialog.xml, QrTextClassifier.kt).
 *
 * A modal bottom sheet over a 32 % black scrim: the drag handle, then a 72 dp card with the action's icon, its title
 * ("Open URL", "Add contact", … or "Text") and the code's data, and a Copy button. A tap on the card runs the
 * action. An action is offered only when an app on the phone can do it (a browser for links and lookups, the
 * Contacts app for a vCard, the Calendar for an event, a handler for tel:, sms:, mailto:, geo:); otherwise the code
 * shows as Text. Left out, because nothing here can back them: Share (no share chooser on the phone), Wi-Fi
 * networks (adding one needs NetworkManager and a confirmation; shown as Text), Android's TextClassifier.
 *
 * Not from Aperture: a "minerva-key:v1?p=<provider>&k=<key>" (or &e=<encrypted key>) code, made by Minerva on the
 * computer, shows as "Minerva API key" with the provider and an Import button (cam-key-import.h); a
 * "minerva-key:v1?p=<provider>&c=<chat>&t=<token>" one, made by Minerva on the computer, pairs the phone with that
 * provider (Messaging): it shows as "Minerva pairing" with "<provider> • pairing" and the same Import button, which
 * hands the whole code to `minerva-config messaging pair` (cam-pair-run, cam-key-import.h). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CamQrDismissed) (gpointer data);

/* shows the sheet over the window `parent` is in; `dismissed` runs once it is gone (scanning goes on then) */
void cam_qr_sheet_show (GtkWidget *parent, const char *text, CamQrDismissed dismissed, gpointer data);

/* the classification alone, for tests: the action's title (NULL for plain text), the line shown, and the URI or
 * file content it hands over */
typedef enum {
  CAM_QR_TEXT,
  CAM_QR_URL,
  CAM_QR_CONTACT,
  CAM_QR_EVENT,
  CAM_QR_EMAIL,
  CAM_QR_GEO,
  CAM_QR_TEL,
  CAM_QR_SMS,
  CAM_QR_ISBN,
  CAM_QR_PRODUCT,
  CAM_QR_VIN,
  CAM_QR_WIFI,
  CAM_QR_KEY,
  CAM_QR_PAIR,            /* a provider's pairing code: the same "minerva-key:" scheme, &c=<chat>&t=<token> */
} CamQrKind;

typedef struct {
  CamQrKind kind;
  char     *data;           /* the line under the title */
  char     *uri;            /* what is opened (a URL, tel:…), or NULL */
  char     *content;        /* a vCard or iCalendar to hand over as a file, or NULL */
  const char *mime;         /* its type */
  /* CAM_QR_KEY and CAM_QR_PAIR only (the key, the encrypted blob and the pairing token are never copied out of the
   * scanned text): the provider's id as the code names it, cleaned for display; whether a passphrase unlocks the
   * key (`encrypted`, never for a pairing code); whether the code is one this app can hand to `minerva-config` at all
   * (an unknown version or a missing part is not; so is a pairing code whose provider does not pair) */
  char     *provider;
  gboolean  encrypted;
  gboolean  importable;
} CamQrResult;

/* whether a scanned text is a Minerva code ("minerva-key:", any case; a key's or a provider's pairing): such a text
 * is never logged, shown whole or copied */
gboolean cam_qr_is_key (const char *text);

/* a secret string of ours: wiped, then freed */
void cam_scrub (char *secret);

CamQrResult *cam_qr_classify     (const char *text);
void         cam_qr_result_free  (CamQrResult *result);

G_END_DECLS
