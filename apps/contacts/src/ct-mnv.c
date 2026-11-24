/* Contacts' one door to Minerva's people archive (ct-mnv.h). */
#include "ct-mnv.h"
#include "ct-mnv-core.h"

#include "ns-mc.h"

#include <string.h>

static JsonObject *ui;          /* `people ui` once per process: the texts do not change under a running app */
static JsonArray *imports;      /* the providers whose chats import (`messaging list` "import"), read again with every
                                 * ct_mnv_ui_load */
static gboolean imports_loading;

static void imports_fetch (GObject *owner, CtMnvDone done, gpointer data);

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

/* `minerva-config GROUP ACTION [OPTIONS…] --json -- [ARGS…]`: the arguments come after "--", so an id that starts
 * with '-' (a chat's, a contact's) is never read as an option; @options (NULL-terminated, or NULL) and @args are
 * borrowed */
static void
run (GObject *owner, const char *stdin_text, CtMnvDone done, gpointer data, const char *group, const char *action,
     const char *const *options, const char *const *args)
{
  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  Call *call = g_new0 (Call, 1);

  call->done = done;
  call->data = data;
  g_ptr_array_add (argv, (gpointer) group);
  g_ptr_array_add (argv, (gpointer) action);
  for (guint i = 0; options && options[i]; i++)
    g_ptr_array_add (argv, (gpointer) options[i]);
  g_ptr_array_add (argv, (gpointer) "--json");
  g_ptr_array_add (argv, (gpointer) "--");
  for (guint i = 0; args && args[i]; i++)
    g_ptr_array_add (argv, (gpointer) args[i]);
  g_ptr_array_add (argv, NULL);
  ns_mc_run (owner, (const char *const *) argv->pdata, stdin_text, answered, call);
}

/* run() with the positional arguments as a NULL-terminated variadic list */
static void
run_v (GObject *owner, CtMnvDone done, gpointer data, const char *group, const char *action,
       const char *const *options, va_list ap)
{
  g_autoptr(GPtrArray) args = g_ptr_array_new ();

  for (const char *arg = va_arg (ap, const char *); arg; arg = va_arg (ap, const char *))
    g_ptr_array_add (args, (gpointer) arg);
  g_ptr_array_add (args, NULL);
  run (owner, NULL, done, data, group, action, options, (const char *const *) args->pdata);
}

/* `minerva-config people ACTION [OPTIONS…] --json -- ARGS…`, the ARGS ending at NULL */
static void
people (GObject *owner, const char *const *options, CtMnvDone done, gpointer data, const char *action, ...)
{
  va_list ap;

  va_start (ap, action);
  run_v (owner, done, data, "people", action, options, ap);
  va_end (ap);
}

/* `minerva-config messaging ACTION [OPTIONS…] --json -- ARGS…`, the ARGS ending at NULL */
static void
messaging (GObject *owner, const char *const *options, CtMnvDone done, gpointer data, const char *action, ...)
{
  va_list ap;

  va_start (ap, action);
  run_v (owner, done, data, "messaging", action, options, ap);
  va_end (ap);
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
    {
      JsonArray *channels;

      ui = json_object_ref (root);
      /* the registry's channels name their IM rows (ct-contact.h) */
      channels = ct_mnv_list ("channel_list");
      for (guint i = 0; channels && i < json_array_get_length (channels); i++)
        {
          JsonObject *channel = json_array_get_object_element (channels, i);

          ct_contact_register_channel (ns_json_str (channel, "id"), ns_json_str (channel, "legacy_property"),
                                       ns_json_str (channel, "title"));
        }
    }
  if (wait->done)
    wait->done (ui ? root : NULL, ui ? NULL : (err ? err : "?"), wait->data);
  g_free (wait);
}

void
ct_mnv_ui_load (GObject *owner, CtMnvDone done, gpointer data)
{
  UiWait *wait;

  /* the providers that import may change under a running app (messaging.conf, a provider turned off): read again */
  if (!imports_loading)
    imports_fetch (owner, NULL, NULL);
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
  people (owner, (const char *const[]) { "--consent", consent, NULL }, done, data, "voice-enroll", pid, NULL);
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
    people (owner, (const char *const[]) { "--consent", consent, NULL }, done, data, "voice-learn", pid, "on", NULL);
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
  /* each command: its action, then its arguments */
  argv = cascade->commands->pdata[cascade->next++];
  run (cascade->owner, NULL, cascade_stepped, cascade, "people", argv[0], NULL, (const char *const *) argv + 1);
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

/* ---- the messaging providers: chats that become contacts ---- */

typedef struct {
  CtMnvDone done;
  gpointer data;
} ImportsWait;

static void
imports_answered (JsonObject *root, const char *err, gpointer data)
{
  ImportsWait *wait = data;
  JsonArray *providers = err ? NULL : ns_json_arr (root, "providers");

  imports_loading = FALSE;
  /* a list that could not be read leaves the one there was */
  if (providers)
    {
      g_clear_pointer (&imports, json_array_unref);
      imports = json_array_new ();
      for (guint i = 0; i < json_array_get_length (providers); i++)
        {
          JsonObject *p = json_array_get_object_element (providers, i);

          /* a provider that is off has no daemon to list its chats */
          if (p && *ns_json_str (p, "id") && json_object_get_boolean_member_with_default (p, "import", FALSE)
              && json_object_get_boolean_member_with_default (p, "enabled", TRUE))
            json_array_add_object_element (imports, json_object_ref (p));
        }
    }
  if (wait->done)
    wait->done (NULL, imports ? NULL : (err ? err : ct_mnv_text ("error")), wait->data);
  g_free (wait);
}

static void
imports_fetch (GObject *owner, CtMnvDone done, gpointer data)
{
  ImportsWait *wait = g_new0 (ImportsWait, 1);

  wait->done = done;
  wait->data = data;
  imports_loading = TRUE;
  messaging (owner, NULL, imports_answered, wait, "list", NULL);
}

void
ct_mnv_imports_load (GObject *owner, CtMnvDone done, gpointer data)
{
  if (imports)
    {
      if (done)
        done (NULL, NULL, data);
      return;
    }
  imports_fetch (owner, done, data);
}

JsonArray *
ct_mnv_imports (void)
{
  return imports;
}

JsonObject *
ct_mnv_import_provider (const char *id)
{
  for (guint i = 0; imports && id && i < json_array_get_length (imports); i++)
    {
      JsonObject *p = json_array_get_object_element (imports, i);

      if (!g_strcmp0 (ns_json_str (p, "id"), id))
        return p;
    }
  return NULL;
}

void
ct_mnv_user_chats (GObject *owner, const char *provider, CtMnvDone done, gpointer data)
{
  messaging (owner, (const char *const[]) { "--kind", "user", NULL }, done, data, "chats", provider, NULL);
}

void
ct_mnv_chat_contact (GObject *owner, const char *provider, const char *chat, CtMnvDone done, gpointer data)
{
  messaging (owner, NULL, done, data, "contact", provider, chat, NULL);
}
