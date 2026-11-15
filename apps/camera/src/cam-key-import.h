/* cam-key-import: hands a scanned Minerva key code to `minerva-config key import --apply --json`.
 *
 * The code ("minerva-key:v1?p=<provider>&k=<key>", or &e= for a passphrase-encrypted key) and the passphrase travel
 * as one JSON object on the program's standard input, {"payload":"…","passphrase":"…"}: never in argv, never in a
 * log. The program prints {"ok":true,"provider":"…","name":"…"} or, failing, {"ok":false,"error":"…",
 * "need_passphrase":true|false}. Which program runs is found as Neo Settings does (ns-mc.c): $MINERVA_CONFIG_BIN,
 * else ~/.local/share/minerva/bin/minerva-config. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  gboolean  ok;
  char     *provider;         /* the provider's id and display name, on success */
  char     *name;
  char     *error;            /* what went wrong, on failure (a line to show) */
  gboolean  need_passphrase;  /* failure: a (right) passphrase is needed */
} CamKeyResult;

typedef void (*CamKeyImportDone) (CamKeyResult *result, gpointer data);

/* Runs the import without blocking; `done` gets the result (freed after it returns) on the main loop.
 * `passphrase` may be NULL. The strings are copied into the program's input, which is wiped when it is sent. */
void cam_key_import_run (const char *payload, const char *passphrase, CamKeyImportDone done, gpointer data);

G_END_DECLS
