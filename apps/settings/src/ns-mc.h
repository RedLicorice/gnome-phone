/* `minerva-config` (Minerva's repository, config/) as Neo Settings' one door to the phone agent's configuration: the
 * Minerva page, Accounts > API keys and Battery > Max charge all run it through here. */
#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

/* whether `minerva-config` is installed (MINERVA_CONFIG_BIN overrides the path) */
gboolean ns_minerva_available (void);

/* `root` is the JSON object the program printed (valid during the call), NULL with `err` when it failed */
typedef void (*NsMcDone) (JsonObject *root, const char *err, gpointer data);

/* Runs `minerva-config ARGS` without blocking. `stdin_text` (or NULL) goes to its standard input: how a key
 * travels, so it is in no argv and no process list. `owner` is kept alive until the answer comes. */
void ns_mc_run (GObject *owner, const char *const *args, const char *stdin_text, NsMcDone done, gpointer data);

/* members of a JSON object, "" / NULL when absent or of another type */
const char *ns_json_str (JsonObject *o, const char *member);
JsonObject *ns_json_obj (JsonObject *o, const char *member);
JsonArray  *ns_json_arr (JsonObject *o, const char *member);
