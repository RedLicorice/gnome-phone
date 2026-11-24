/* `minerva-config` (Minerva's repository, config/) as Neo Settings' one door to the phone agent's configuration: the
 * Minerva page, Accounts > API keys and Battery > Max charge all run it through here. Needs only GIO and json-glib,
 * so Neo Contacts builds this file too (its Minerva parts, apps/contacts/src/ct-mnv.c). */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

/* whether `minerva-config` is installed (in MINERVA_BIN_DIR; MINERVA_CONFIG_BIN overrides the path) */
gboolean ns_minerva_available (void);

/* whether another Minerva program (minerva-perm…) is installed: the same directory, an env var of its own name
 * overrides the path ("minerva-perm" -> MINERVA_PERM_BIN) */
gboolean ns_mc_prog_available (const char *prog);

/* `root` is the JSON object the program printed (valid during the call), NULL with `err` when it failed */
typedef void (*NsMcDone) (JsonObject *root, const char *err, gpointer data);

/* Runs `minerva-config ARGS` without blocking. `stdin_text` (or NULL) goes to its standard input: how a key
 * travels, so it is in no argv and no process list. `owner` is kept alive until the answer comes. */
void ns_mc_run (GObject *owner, const char *const *args, const char *stdin_text, NsMcDone done, gpointer data);

/* The same for another program of that directory, such as `minerva-perm` (MINERVA_PERM_BIN overrides its path) */
void ns_mc_run_bin (GObject *owner, const char *prog, const char *const *args, const char *stdin_text,
                    NsMcDone done, gpointer data);

/* A program that prints a JSON array (`minerva-perm history --json`, `minerva-config models --json`) */
typedef void (*NsMcListDone) (JsonArray *list, const char *err, gpointer data);
void ns_mc_run_list (GObject *owner, const char *prog, const char *const *args, NsMcListDone done, gpointer data);

/* members of a JSON object, "" / NULL when absent or of another type */
const char *ns_json_str (JsonObject *o, const char *member);
JsonObject *ns_json_obj (JsonObject *o, const char *member);
JsonArray  *ns_json_arr (JsonObject *o, const char *member);

/* Opens Neo Prompt on one of its documents (`id` of its registry; `person` for a notebook), through
 * neo-prompt://open/<id>[?person=<id>] and GApplication's Open. FALSE when Neo Prompt is not installed. */
gboolean ns_prompt_open (const char *id, const char *person);
gboolean ns_prompt_installed (void);
