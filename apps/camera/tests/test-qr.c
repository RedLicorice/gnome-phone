/* What the QR sheet makes of a code (cam_qr_classify): zxing's ResultParsers, as QrTextClassifier uses them. */
#include "cam-qr-sheet.h"

#include <string.h>

typedef struct {
  const char *text;
  CamQrKind   kind;
  const char *data, *uri;
} Case;

static const Case cases[] = {
  { "https://lineageos.org/", CAM_QR_URL, "https://lineageos.org/", "https://lineageos.org/" },
  { "www.example.com/a?b=1", CAM_QR_URL, "www.example.com/a?b=1", "http://www.example.com/a?b=1" },
  { "tel:+49301234567", CAM_QR_TEL, "+49301234567", "tel:+49301234567" },
  { "SMSTO:+4930123:hello there", CAM_QR_SMS, "+4930123", "sms:+4930123?body=hello%20there" },
  { "sms:+4930123?body=hi", CAM_QR_SMS, "+4930123", "sms:+4930123?body=hi" },
  { "mailto:a@b.de?subject=x", CAM_QR_EMAIL, "a@b.de", "mailto:a@b.de?subject=x" },
  { "MATMSG:TO:a@b.de;SUB:Hi;BODY:Text;;", CAM_QR_EMAIL, "a@b.de", "mailto:a%40b.de?subject=Hi&body=Text" },
  { "someone@example.org", CAM_QR_EMAIL, "someone@example.org", "mailto:someone@example.org" },
  { "geo:52.52,13.405", CAM_QR_GEO, "52.52, 13.405", "geo:52.52,13.405" },
  { "9783161484100", CAM_QR_ISBN, "9783161484100", "https://isbnsearch.org/isbn/9783161484100" },
  { "4006381333931", CAM_QR_PRODUCT, "4006381333931", "https://www.barcodelookup.com/4006381333931" },
  { "1M8GDM9AXKP042788", CAM_QR_VIN, "1M8GDM9AXKP042788",
    "https://www.vindecoderz.com/EN/check-lookup/1M8GDM9AXKP042788" },
  { "WIFI:T:WPA;S:Home;P:secret;;", CAM_QR_WIFI, "Home", NULL },
  { "MECARD:N:Doe,John;TEL:+4930;EMAIL:j@d.de;;", CAM_QR_CONTACT, "John Doe", NULL },
  { "BEGIN:VCARD\nVERSION:3.0\nFN:Jane Roe\nEND:VCARD", CAM_QR_CONTACT, "Jane Roe", NULL },
  { "BEGIN:VEVENT\nSUMMARY:Party\nDTSTART:20261110T200000Z\nEND:VEVENT", CAM_QR_EVENT, "Party", NULL },
  { "just some words", CAM_QR_TEXT, "just some words", NULL },
  { "4006381333932", CAM_QR_TEXT, "4006381333932", NULL },
};

/* Minerva key codes: the provider shown, whether a passphrase is needed, whether the code can be imported at all.
 * The key and the encrypted blob appear in no field of the result (checked below). */
typedef struct {
  const char *text;
  const char *provider;     /* NULL: not importable */
  gboolean    encrypted;
  const char *data;
} KeyCase;

static const KeyCase key_cases[] = {
  { "minerva-key:v1?p=openrouter&k=sk-or-SECRET1", "openrouter", FALSE, "openrouter" },
  { "  minerva-key:v1?p=openrouter&k=sk-or-SECRET1\n", "openrouter", FALSE, "openrouter" },
  { "minerva-key:v1?p=groq&e=U0VDUkVUQkxPQg", "groq", TRUE, "groq • passphrase needed" },
  { "MINERVA-KEY:V1?p=groq&k=SECRET1", "groq", FALSE, "groq" },
  { "Minerva-Key:v1?k=SECRET1&p=my-ollama", "my-ollama", FALSE, "my-ollama" },
  /* percent-encoded and odd provider ids */
  { "minerva-key:v1?p=my%20local%2Fllm&k=SECRET1", "my local/llm", FALSE, "my local/llm" },
  { "minerva-key:v1?p=caf%C3%A9&k=SECRET1", "café", FALSE, "café" },
  { "minerva-key:v1?p=a%0Ab%09c&k=SECRET1", "abc", FALSE, "abc" },
  { "minerva-key:v1?p=x&k=a%2Bb%3D%26c", "x", FALSE, "x" },
  /* malformed: nothing to import, and still nothing of the code shown */
  { "minerva-key:v1?p=openrouter", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=x&k=SECRET1&e=SECRET2", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v2?p=x&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:?p=x&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=%ZZ&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=%00&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=%FF%FE&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=%0A%0D&k=SECRET1", NULL, FALSE, "Not a code this app can import" },
  { "minerva-key:v1?p=x&k=", NULL, FALSE, "Not a code this app can import" },
};

/* a text that merely mentions the scheme is not a key code */
static const char *not_keys[] = { "my minerva-key:v1?p=x&k=SECRET1", "https://example.org/minerva-key:v1?p=x&k=S", "minerva key" };

static int
check_keys (void)
{
  int failed = 0;
  g_autofree char *long_id = g_strnfill (200, 'z');
  g_autofree char *long_text = g_strdup_printf ("minerva-key:v1?p=%s&k=SECRET1", long_id);
  CamQrResult *result;

  for (guint i = 0; i < G_N_ELEMENTS (key_cases); i++)
    {
      const KeyCase *c = &key_cases[i];

      result = cam_qr_classify (c->text);
      if (result->kind != CAM_QR_KEY || g_strcmp0 (result->provider, c->provider) || result->encrypted != c->encrypted
          || result->importable != (c->provider != NULL) || g_strcmp0 (result->data, c->data) || result->uri
          || result->content || !cam_qr_is_key (c->text)
          || (result->data && (strstr (result->data, "SECRET") || strstr (result->data, "U0VDUkVU"))))
        {
          g_printerr ("FAIL key %s: kind %d provider \"%s\" encrypted %d importable %d data \"%s\"\n", c->text,
                      result->kind, result->provider ? result->provider : "(null)", result->encrypted,
                      result->importable, result->data);
          failed++;
        }
      cam_qr_result_free (result);
    }
  for (guint i = 0; i < G_N_ELEMENTS (not_keys); i++)
    {
      result = cam_qr_classify (not_keys[i]);
      if (result->kind == CAM_QR_KEY || cam_qr_is_key (not_keys[i]))
        {
          g_printerr ("FAIL not a key: %s\n", not_keys[i]);
          failed++;
        }
      cam_qr_result_free (result);
    }
  /* a long provider id is cut for display */
  result = cam_qr_classify (long_text);
  if (!result->importable || g_utf8_strlen (result->provider, -1) != 49 || !g_str_has_suffix (result->provider, "…"))
    {
      g_printerr ("FAIL long provider: \"%s\"\n", result->provider ? result->provider : "(null)");
      failed++;
    }
  cam_qr_result_free (result);
  g_print ("%d of %d key cases right\n", (int) (G_N_ELEMENTS (key_cases) + G_N_ELEMENTS (not_keys) + 1) - failed,
           (int) (G_N_ELEMENTS (key_cases) + G_N_ELEMENTS (not_keys) + 1));
  return failed;
}

/* Minerva pairing codes: "minerva-key:v1?p=<provider>&c=<chat>&t=<token>", shown as "<provider> • pairing". The token
 * appears in no field; which providers pair is asked of minerva-config when the sheet opens, so the classifier alone
 * takes any provider the code names. */
typedef struct {
  const char *text;
  const char *provider;     /* NULL: not importable */
  const char *data;
} PairCase;

static const PairCase pair_cases[] = {
  { "minerva-key:v1?p=echo&c=@me&t=TOKEN1", "echo", "echo • pairing" },
  { "MINERVA-KEY:V1?p=chan&c=3933123&t=TOKEN1", "chan", "chan • pairing" },
  { "  minerva-key:v1?t=TOKEN1&c=@me&p=my%20local\n", "my local", "my local • pairing" },
  /* a code missing its provider, its chat or its token is neither a pairing code nor a key */
  { "minerva-key:v1?c=@me&t=TOKEN1", NULL, "Not a code this app can import" },
  { "minerva-key:v1?p=echo&t=TOKEN1", NULL, "Not a code this app can import" },
  { "minerva-key:v1?p=echo&c=@me", NULL, "Not a code this app can import" },
  { "minerva-key:v1?p=echo&c=@me&t=", NULL, "Not a code this app can import" },
  { "minerva-key:v2?p=echo&c=@me&t=TOKEN1", NULL, "Not a code this app can import" },
};

static int
check_pairs (void)
{
  int failed = 0;

  for (guint i = 0; i < G_N_ELEMENTS (pair_cases); i++)
    {
      const PairCase *c = &pair_cases[i];
      CamQrResult *result = cam_qr_classify (c->text);
      CamQrKind kind = c->provider ? CAM_QR_PAIR : CAM_QR_KEY;

      if (result->kind != kind || g_strcmp0 (result->provider, c->provider)
          || result->importable != (c->provider != NULL) || result->encrypted
          || g_strcmp0 (result->data, c->data) || result->uri || result->content
          || !cam_qr_is_key (c->text) || (result->data && strstr (result->data, "TOKEN")))
        {
          g_printerr ("FAIL pair %s: kind %d provider \"%s\" importable %d data \"%s\"\n", c->text, result->kind,
                      result->provider ? result->provider : "(null)", result->importable, result->data);
          failed++;
        }
      cam_qr_result_free (result);
    }
  g_print ("%d of %d pair cases right\n", (int) G_N_ELEMENTS (pair_cases) - failed,
           (int) G_N_ELEMENTS (pair_cases));
  return failed;
}

int
main (void)
{
  int failed = 0;

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      CamQrResult *result = cam_qr_classify (cases[i].text);

      if (result->kind != cases[i].kind || g_strcmp0 (result->data, cases[i].data)
          || g_strcmp0 (result->uri, cases[i].uri))
        {
          g_printerr ("FAIL %s: kind %d data \"%s\" uri \"%s\"\n", cases[i].text, result->kind, result->data,
                      result->uri ? result->uri : "(null)");
          failed++;
        }
      cam_qr_result_free (result);
    }
  g_print ("%d of %d cases right\n", (int) G_N_ELEMENTS (cases) - failed, (int) G_N_ELEMENTS (cases));
  failed += check_keys ();
  failed += check_pairs ();
  return failed ? 1 : 0;
}
