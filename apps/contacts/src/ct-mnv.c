/* Contacts' one door to Minerva's people archive (ct-mnv.h). */
#include "ct-mnv.h"
#include "ct-mnv-core.h"

#include "ns-mc.h"

#include <string.h>

static JsonObject *ui;          /* `people ui` once per process: the texts do not change under a running app */

gboolean
ct_mnv_available (void)
{
  return ns_minerva_available ();
}

/* ---- the data ---- */

static JsonObject *
contacts_part (void)
{
  return ui ? ns_json_obj (ui, "contacts") : NULL;
}

const char *
ct_mnv_text (const char *key)
{
  return ns_json_str (contacts_part (), key);
}

char *
ct_mnv_textf (const char *key, const char *const *pairs)
{
  return ct_mnv_format (ct_mnv_text (key), pairs);
}

int
ct_mnv_int (const char *key)
{
  JsonObject *part = contacts_part ();

  return part ? (int) json_object_get_int_member_with_default (part, key, 0) : 0;
}

double
ct_mnv_double (const char *key)
{
  JsonObject *part = contacts_part ();

  return part ? json_object_get_double_member_with_default (part, key, 0) : 0;
}

const char *
ct_mnv_map (const char *map, const char *key)
{
  return ns_json_str (ui ? ns_json_obj (ui, map) : NULL, key);
}

JsonArray *
ct_mnv_list (const char *key)
{
  return ns_json_arr (contacts_part (), key);
}

/* ---- the calls ---- */

typedef struct {
  CtMnvDone done;
  gpointer data;
} Call;

/* why an answer is no answer: the program failed, or minerva-config refused ({"ok": false, "error": …}) */
static const char *
refusal (JsonObject *root, const char *err)
{
  const char *why;

  if (err)
    return err;
  if (root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    return NULL;
  why = ns_json_str (root, "error");
  return *why ? why : ct_mnv_text ("error");
}

static void
answered (JsonObject *root, const char *err, gpointer data)
{
  Call *call = data;
  const char *why = refusal (root, err);

  if (call->done)
    call->done (why ? NULL : root, why, call->data);
  g_free (call);
}

/* `minerva-config people ARGS… --json`, @args ending at NULL */
static void
people (GObject *owner, const char *stdin_text, CtMnvDone done, gpointer data, const char *first, ...)
{
  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  Call *call = g_new0 (Call, 1);
  va_list ap;

  call->done = done;
  call->data = data;
  g_ptr_array_add (argv, (gpointer) "people");
  va_start (ap, first);
  for (const char *arg = first; arg; arg = va_arg (ap, const char *))
    g_ptr_array_add (argv, (gpointer) arg);
  va_end (ap);
  g_ptr_array_add (argv, (gpointer) "--json");
  g_ptr_array_add (argv, NULL);
  ns_mc_run (owner, (const char *const *) argv->pdata, stdin_text, answered, call);
}

typedef struct {
  CtMnvDone done;
  gpointer data;
} UiWait;

static void
ui_answered (JsonObject *root, const char *err, gpointer data)
{
  UiWait *wait = data;

  if (!err && root && ns_json_obj (root, "contacts") && !ui)
    ui = json_object_ref (root);
  if (wait->done)
    wait->done (ui ? root : NULL, ui ? NULL : (err ? err : "?"), wait->data);
  g_free (wait);
}

void
ct_mnv_ui_load (GObject *owner, CtMnvDone done, gpointer data)
{
  UiWait *wait;

  if (ui)
    {
      if (done)
        done (ui, NULL, data);
      return;
    }
  wait = g_new0 (UiWait, 1);
  wait->done = done;
  wait->data = data;
  people (owner, NULL, ui_answered, wait, "ui", NULL);
}

gboolean
ct_mnv_ui_ready (void)
{
  return ui != NULL;
}

void
ct_mnv_by_contact (GObject *owner, const char *uid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "by-contact", uid, NULL);
}

void
ct_mnv_notebook (GObject *owner, const char *pid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "notebook", pid, "get", NULL);
}

void
ct_mnv_notebook_clear (GObject *owner, const char *pid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "notebook", pid, "clear", NULL);
}

void
ct_mnv_voice_status (GObject *owner, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "voice-status", NULL);
}

void
ct_mnv_voice_enroll (GObject *owner, const char *pid, const char *consent, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "voice-enroll", pid, "--consent", consent, NULL);
}

void
ct_mnv_voice_forget (GObject *owner, const char *pid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "voice-forget", pid, NULL);
}

void
ct_mnv_voice_learn (GObject *owner, const char *pid, gboolean on, const char *consent, CtMnvDone done, gpointer data)
{
  if (on)
    people (owner, NULL, done, data, "voice-learn", pid, "on", "--consent", consent, NULL);
  else
    people (owner, NULL, done, data, "voice-learn", pid, "off", NULL);
}

void
ct_mnv_clips_forget (GObject *owner, const char *pid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "voice-clips", pid, "clear", NULL);
}

void
ct_mnv_set_kind (GObject *owner, const char *pid, const char *kind, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "kind", pid, kind, NULL);
}

void
ct_mnv_create (GObject *owner, const char *name, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "create", name, NULL);
}

void
ct_mnv_link (GObject *owner, const char *pid, const char *contact_uid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "link", pid, contact_uid, NULL);
}

void
ct_mnv_unlink (GObject *owner, const char *pid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "unlink", pid, NULL);
}

void
ct_mnv_sync_name (GObject *owner, const char *pid, const char *person_name, const char *name, CtMnvDone done,
                  gpointer data)
{
  if (!pid || !ct_mnv_name_stale (person_name, name))
    {
      if (done)
        done (NULL, NULL, data);
      return;
    }
  people (owner, NULL, done, data, "rename", pid, name, NULL);
}

void
ct_mnv_merge (GObject *owner, const char *pid, const char *into, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "merge", pid, into, NULL);
}

void
ct_mnv_forget (GObject *owner, const char *pid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "delete", pid, NULL);
}

void
ct_mnv_known (GObject *owner, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "known", NULL);
}

void
ct_mnv_rule_set (GObject *owner, const char *key, const char *value, CtMnvDone done, gpointer data)
{
  g_autofree char *pair = g_strconcat (key, "=", value, NULL);

  people (owner, NULL, done, data, "rules-set", pair, NULL);
}
