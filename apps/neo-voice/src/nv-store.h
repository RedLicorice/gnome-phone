/* Neo Voice's data: the labels and settings (neo-voice-ui.json, a resource) and the voice service's detection log,
 * reached only through org.minerva.Voice1's Detection* methods (Minerva docs/detection-log.md). Every call is
 * asynchronous; a reply is JSON text (a record, a list, the status) or, for a clip, the WAV's bytes. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
  JsonObject      *root;    /* the whole ui file */
  JsonObject      *conf;
  JsonObject      *strings; /* the language's */
  GDBusConnection *bus;
} NvStore;

/* reply: the parsed JSON (NULL on error, then `error` says why) */
typedef void (*NvReply) (gpointer data, JsonNode *reply, const char *error);
/* clip: the WAV's bytes (NULL on error) */
typedef void (*NvClip) (gpointer data, GBytes *wav, const char *error);

NvStore    *nv_store_new   (void);
void        nv_store_free  (NvStore *self);
/* a label by dotted key ("detail.play"); the key itself when the file has none */
const char *nv_ui          (NvStore *self, const char *key);
gint64      nv_conf_int    (NvStore *self, const char *key);
double      nv_conf_double (NvStore *self, const char *key);
const char *nv_conf_str    (NvStore *self, const char *key);
/* the ordered ids of a list in the ui file ("labels_order") */
GPtrArray  *nv_ui_list     (NvStore *self, const char *key);

void nv_list   (NvStore *self, const char *filter_json, NvReply cb, gpointer data);
void nv_status (NvStore *self, NvReply cb, gpointer data);
void nv_label  (NvStore *self, const char *id, const char *label, const char *note, NvReply cb, gpointer data);
void nv_trim   (NvStore *self, const char *id, int start_ms, int end_ms, NvReply cb, gpointer data);
void nv_delete (NvStore *self, const char *id, NvReply cb, gpointer data);
void nv_clip   (NvStore *self, const char *id, NvClip cb, gpointer data);

G_END_DECLS
