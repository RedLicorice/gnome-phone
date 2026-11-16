/* cam-key-import: hands a scanned Minerva key code to `minerva-config key import --apply --json`, and a scanned
 * pairing code to `minerva-config messaging pair <provider> --json`.
 *
 * A key code ("minerva-key:v1?p=<provider>&k=<key>", or &e= for a passphrase-encrypted key) and the passphrase travel
 * as one JSON object on the program's standard input, {"payload":"…","passphrase":"…"}: never in argv, never in a
 * log. The program prints {"ok":true,"provider":"…","name":"…"} or, failing, {"ok":false,"error":"…",
 * "need_passphrase":true|false}. A pairing code ("minerva-key:v1?p=<provider>&c=<chat>&t=<token>") travels as
 * {"payload":"…"} the same way; the program prints {"ok":true,"provider":"…","message":"…"} or
 * {"ok":false,"error":"…"}. Which program runs is found as Neo Settings does (ns-mc.c): $MINERVA_CONFIG_BIN, else
 * ~/.local/share/minerva/bin/minerva-config. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  gboolean  ok;
  char     *provider;         /* the provider's id and display name, on success */
  char     *name;
  char     *message;          /* a pairing's line for the sheet, on success (NULL when it gave none) */
  char     *error;            /* what went wrong, on failure (a line to show) */
  gboolean  need_passphrase;  /* failure: a (right) passphrase is needed */
} CamKeyResult;

typedef void (*CamKeyImportDone) (CamKeyResult *result, gpointer data);

/* Runs the import without blocking; `done` gets the result (freed after it returns) on the main loop.
 * `passphrase` may be NULL. The strings are copied into the program's input, which is wiped when it is sent. */
void cam_key_import_run (const char *payload, const char *passphrase, CamKeyImportDone done, gpointer data);

/* Pairs with `provider` from the scanned code, without blocking, as cam_key_import_run does: the code goes on the
 * program's standard input, never in argv. `done` gets `message` on success, `error` on failure. */
void cam_pair_run (const char *provider, const char *payload, CamKeyImportDone done, gpointer data);

/* Whether `provider` pairs at all, from `minerva-config messaging list --json` (no list of provider ids here). A call
 * that fails accepts the code: the pair command then refuses it, with its own words. Blocking, with a short timeout. */
gboolean cam_pair_allowed (const char *provider);

G_END_DECLS
