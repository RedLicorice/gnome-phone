/* Neo Prompt's data: the files it may open (a registry of JSON data), the app's own configuration, and the one door
 * to Minerva (`minerva-config … --json`, run as a child, the text on stdin).
 *
 * Nothing about the files is compiled in: an entry of the registry says where a text comes from ({file} or {argv})
 * and how it goes back ({argv} with the text on stdin, or {file}: an atomic write). `glob` entries stand for a family
 * of files (the event templates, the skills), one document each, saved in place.
 *
 * Paths come from ~/.config/minerva/neo-prompt.json (NEO_PROMPT_CONF overrides it), the labels from
 * ~/.config/minerva/neo-prompt-ui.json. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
  char     *id;
  char     *group;
  char     *title;
  char     *kind;            /* markdown, jinja, json, text: the highlighter and the local check */
  char     *path;            /* the file a {file} source read, expanded; NULL for an argv source */
  char     *source_label;    /* what the registry calls the source, for a row or a block */
  char     *text_key;        /* the member of an argv answer that carries the text ("text") */
  char    **open_argv;       /* NULL-terminated; the text is in "text" of the answer */
  char    **save_argv;       /* NULL-terminated; NULL: write `path` (an atomic replace) */
  char    **validate_argv;   /* NULL-terminated; NULL: no check but the local one */
  char    **empty_argv;      /* NULL-terminated; for empty text (a notebook is cleared, not set) */
  char     *person;          /* the person this document was opened for (a notebook), or NULL */
  gboolean  needs_person;    /* listed only once a person is known (the notebook) */
  gboolean  editable;
  gboolean  history;
  gboolean  listed;          /* in the File tab */
} NpDoc;

typedef struct {
  char    *conf_path;
  char    *minerva_config;
  char    *registry;
  char    *ui_path;
  char    *history_dir;
  char    *draft_dir;
  char    *agents_file;
  char    *opencode_json;
  char    *skills_dir;
  char    *people_root;
  char    *event_templates;
  char    *input_tags;
  int      history_max;
  int      chars_per_token;
  int      cache_min_tokens;
  int      timeout_s;
  JsonObject *block_freq;    /* block id -> "static" | "session" | "turn" (the cache indicator) */
  JsonArray  *block_order;   /* the order of the blocks, as the provider's cache prefix sees them */
} NpConf;

typedef struct {
  NpConf     conf;
  GPtrArray *docs;           /* NpDoc*, the registry expanded (globs included) */
  JsonObject *ui;
} NpStore;

typedef struct {
  gboolean    ok;
  char       *error;         /* the refusal, verbatim */
  char       *text;          /* "text" of the answer, when it has one */
  JsonObject *json;          /* the whole answer (a ref), or NULL */
} NpAns;

typedef void (*NpRunDone) (NpAns *ans, gpointer data);

NpStore    *np_store_new       (const char *conf_path);
void        np_store_free      (NpStore *self);

/* a label of the ui file, by dotted path ("files.empty"); the path itself when missing */
const char *np_ui              (NpStore *self, const char *key);
char       *np_path_expand     (const char *path);
const char *np_doc_kind        (NpDoc *doc);

GPtrArray  *np_docs            (NpStore *self);          /* the openable ones (editable) */
NpDoc      *np_doc_by_id       (NpStore *self, const char *id, const char *person);
void        np_doc_free        (NpDoc *doc);

/* the text of a document now, as its source has it (a file, or `open_argv` run) */
void        np_doc_load        (NpStore *self, NpDoc *doc, GObject *owner, GCancellable *cancel,
                                NpRunDone done, gpointer data);
/* run a minerva-config command (or any argv) with the text on stdin; the answer is JSON when it is */
void        np_run             (NpStore *self, const char *const *argv, const char *stdin_text, GObject *owner,
                                GCancellable *cancel, NpRunDone done, gpointer data);

void        np_ans_clear       (NpAns *ans);
void        np_argv_free       (char **argv);

/* the document's history: the snapshots of what a save is about to overwrite */
char       *np_history_dir     (NpStore *self, NpDoc *doc);
GPtrArray  *np_history_list    (NpStore *self, NpDoc *doc);   /* snapshot file names, newest first */
gboolean    np_history_push    (NpStore *self, NpDoc *doc, const char *text, char **error);
char       *np_history_read    (NpStore *self, NpDoc *doc, const char *name);
void        np_history_trim    (NpStore *self, NpDoc *doc);

/* the draft: the buffer, kept while it is dirty, offered back when the app opens the document again */
char       *np_draft_path      (NpStore *self, NpDoc *doc);
gboolean    np_draft_write     (NpStore *self, NpDoc *doc, const char *text);
void        np_draft_drop      (NpStore *self, NpDoc *doc);

char       *np_checksum        (const char *text);

G_END_DECLS
