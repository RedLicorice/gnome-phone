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

const ImForm *
ct_mnv_im_form (void)
{
  static ImForm form;
  static GStrv reads;
  JsonArray *list;
  GStrvBuilder *builder;

  if (reads || !ui)
    return &form;
  builder = g_strv_builder_new ();
  list = ct_mnv_list ("im_read_fields");
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    g_strv_builder_add (builder, json_array_get_string_element (list, i));
  reads = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  form.write_field = ct_mnv_text ("im_write_field");
  form.read_fields = (const char *const *) reads;
  return &form;
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

void
ct_mnv_set_owner (GObject *owner, const char *pid, const char *contact_uid, CtMnvDone done, gpointer data)
{
  if (contact_uid && *contact_uid)
    people (owner, NULL, done, data, "owner", pid, contact_uid, NULL);
  else
    people (owner, NULL, done, data, "owner", pid, NULL);
}

void
ct_mnv_rename (GObject *owner, const char *pid, const char *name, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "rename", pid, name, NULL);
}

/* ---- the memory service: through minerva-config, which asks the platform's approval (ct-mnv.h) ---- */

gboolean
ct_mnv_memory_configured (void)
{
  JsonObject *part = contacts_part ();

  return part && json_object_get_boolean_member_with_default (part, "memory_ops", FALSE);
}

void
ct_mnv_contact_approve_delete (GObject *owner, const char *uid, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "contact-approve", "delete", uid, NULL);
}

void
ct_mnv_contact_approve_merge (GObject *owner, const char *from, const char *to, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "contact-approve", "merge", from, to, NULL);
}

void
ct_mnv_contact_commit (GObject *owner, const char *intent, CtMnvDone done, gpointer data)
{
  people (owner, NULL, done, data, "contact-commit", intent, NULL);
}

/* ---- the people archive follows the contact ---- */

typedef struct {
  GObject *owner;
  GPtrArray *commands;          /* NULL-terminated argument vectors after "people", run one after the other */
  guint next;
  CtMnvDone done;
  gpointer data;
  char *from, *to;
  JsonObject *known;
} Cascade;

static void cascade_run (Cascade *cascade);

static void
cascade_free (Cascade *cascade)
{
  g_clear_pointer (&cascade->commands, g_ptr_array_unref);
  g_clear_pointer (&cascade->known, json_object_unref);
  g_free (cascade->from);
  g_free (cascade->to);
  g_free (cascade);
}

static void
cascade_stepped (JsonObject *root, const char *err, gpointer data)
{
  Cascade *cascade = data;

  if (err)
    {
      cascade->done (NULL, err, cascade->data);
      cascade_free (cascade);
      return;
    }
  cascade_run (cascade);
}

static void
cascade_run (Cascade *cascade)
{
  gchar **argv;

  if (cascade->next >= cascade->commands->len)
    {
      cascade->done (NULL, NULL, cascade->data);
      cascade_free (cascade);
      return;
    }
  argv = cascade->commands->pdata[cascade->next++];
  {
    g_autoptr (GPtrArray) full = g_ptr_array_new ();
    Call *call = g_new0 (Call, 1);

    call->done = cascade_stepped;
    call->data = cascade;
    g_ptr_array_add (full, (gpointer) "people");
    for (guint i = 0; argv[i]; i++)
      g_ptr_array_add (full, argv[i]);
    g_ptr_array_add (full, (gpointer) "--json");
    g_ptr_array_add (full, NULL);
    ns_mc_run (cascade->owner, (const char *const *) full->pdata, NULL, answered, call);
  }
}

static void
add_command (Cascade *cascade, const char *first, ...)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  va_list ap;

  va_start (ap, first);
  for (const char *arg = first; arg; arg = va_arg (ap, const char *))
    g_ptr_array_add (argv, g_strdup (arg));
  va_end (ap);
  g_ptr_array_add (argv, NULL);
  g_ptr_array_add (cascade->commands, g_strdupv ((gchar **) argv->pdata));
  for (guint i = 0; i + 1 < argv->len; i++)
    g_free (argv->pdata[i]);
}

/* the animals and objects of @from, owned by @to now ("" for nobody) */
static void
repoint_owned (Cascade *cascade, const char *to)
{
  g_autoptr (GPtrArray) owned = ct_mnv_owned_by (cascade->known, cascade->from);

  for (guint i = 0; i < owned->len; i++)
    {
      const char *pid = ns_json_str (owned->pdata[i], "id");

      if (to && *to)
        add_command (cascade, "owner", pid, to, NULL);
      else
        add_command (cascade, "owner", pid, NULL);
    }
}

static Cascade *
cascade_new (GObject *owner, const char *from, const char *to, JsonObject *known, CtMnvDone done, gpointer data)
{
  Cascade *cascade = g_new0 (Cascade, 1);

  cascade->owner = owner;
  cascade->commands = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  cascade->done = done;
  cascade->data = data;
  cascade->from = g_strdup (from);
  cascade->to = g_strdup (to);
  cascade->known = known ? json_object_ref (known) : NULL;
  return cascade;
}

/* the person of a contact in the answer of `people known`, NULL when she has none */
static JsonObject *
person_of_contact (JsonObject *known, const char *uid)
{
  JsonArray *people = ns_json_arr (known, "people");

  for (guint i = 0; uid && *uid && people && i < json_array_get_length (people); i++)
    {
      JsonObject *person = json_array_get_object_element (people, i);

      if (person && !g_strcmp0 (ns_json_str (person, "contact_uid"), uid))
        return person;
    }
  return NULL;
}

void
ct_mnv_archive_contact_deleted (GObject *owner, const char *uid, JsonObject *known, CtMnvDone done, gpointer data)
{
  Cascade *cascade = cascade_new (owner, uid, NULL, known, done, data);
  JsonObject *person = person_of_contact (known, uid);

  repoint_owned (cascade, NULL);
  if (person)
    add_command (cascade, "delete", ns_json_str (person, "id"), NULL);
  cascade_run (cascade);
}

void
ct_mnv_archive_contact_merged (GObject *owner, const char *from, const char *to, JsonObject *known, CtMnvDone done,
                               gpointer data)
{
  Cascade *cascade = cascade_new (owner, from, to, known, done, data);
  JsonObject *gone = person_of_contact (known, from), *stays = person_of_contact (known, to);

  repoint_owned (cascade, to);
  if (gone && stays)
    {
      /* two entries linked to two contacts cannot be merged: the one that goes is let go of its contact first */
      add_command (cascade, "unlink", ns_json_str (gone, "id"), NULL);
      add_command (cascade, "merge", ns_json_str (gone, "id"), ns_json_str (stays, "id"), NULL);
    }
  else if (gone)
    add_command (cascade, "link", ns_json_str (gone, "id"), to, NULL);
  cascade_run (cascade);
}
