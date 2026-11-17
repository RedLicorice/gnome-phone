/* Deleting and merging contacts, with Minerva (ct-removal.h). */
#include "ct-removal.h"
#include "ct-contact.h"
#include "ct-dialogs.h"
#include "ct-mnv-core.h"
#include "ct-mnv.h"
#include "ct-picker.h"
#include "ct-proof.h"
#include "ct-store.h"
#include "ct-ui.h"
#include "ct-window.h"
#include "ns-mc.h"

#include <glib/gi18n.h>

gboolean
ct_removal_active (void)
{
  return ct_mnv_available () && ct_mnv_ui_ready ();
}

/* ---- shared ---- */

static char *
name_of (GtkWidget *window, const char *uid)
{
  CtEntry *entry = ct_store_lookup (ct_window_get_store (window), uid);
  g_autofree char *name = entry ? ct_contact_display_name (entry->contact, ct_display_order (ct_window_get_settings (window)))
                                : NULL;

  return name && *name ? g_steal_pointer (&name) : g_strdup (uid);
}

static void
toast (GtkWidget *window, const char *text)
{
  if (text && *text)
    ct_window_toast (window, text);
}

/* a failure of a step: what the service or the archive said, in the data's words */
static void
fail (GtkWidget *window, const char *key, const char *why)
{
  g_autofree char *text = ct_mnv_textf (key, (const char *const[]) { "why", why ? why : "", NULL });

  toast (window, text);
}

static CtProofMethod
method_of (const char *key)
{
  return !g_strcmp0 (ct_mnv_text (key), "pin") ? CT_PROOF_PIN : CT_PROOF_FINGER;
}

/* the proof, asked on the shell's sheet with @reason (the user's language) and the data's method and timing */
static void
ask_proof (GtkWidget *window, const char *method_key, const char *reason, CtProofDone done, gpointer data)
{
  CtProofRequest request = { method_of (method_key), reason, ct_mnv_int ("proof_seconds"), ct_mnv_int ("proof_tries"),
                             ct_mnv_text ("proof_wrong") };

  ct_proof_ask (G_OBJECT (window), &request, done, data);
}

/* the owner's name typed, for the owner's contact: the second, stronger confirmation */
typedef struct {
  char *name;
} Typed;

static const char *
typed_check (const char *text, gpointer data)
{
  Typed *typed = data;
  g_autofree char *plain = g_strstrip (g_strdup (text));

  return g_strcmp0 (plain, typed->name) ? ct_mnv_text ("owner_mismatch") : NULL;
}

/* ---- deleting ---- */

typedef struct {
  GtkWidget *window;
  GPtrArray *uids;              /* strings */
  guint next, deleted;
  JsonObject *known;
  const char *owner_uid;        /* borrowed from known */
  char *approver;
  gboolean memory;
  Typed typed;
  CtRemovalDone done;
  gpointer data;
} Del;

static void
del_finish (Del *del)
{
  if (del->deleted)
    {
      g_autofree char *n = g_strdup_printf ("%u", del->deleted);

      if (del->deleted == 1)
        toast (del->window, ct_mnv_text ("contact_deleted_done"));
      else
        {
          g_autofree char *text = ct_mnv_textf ("contacts_deleted_done", (const char *const[]) { "n", n, NULL });

          toast (del->window, text);
        }
    }
  if (del->done)
    del->done (del->deleted, del->data);
  g_object_unref (del->window);
  g_ptr_array_unref (del->uids);
  g_clear_pointer (&del->known, json_object_unref);
  g_free (del->approver);
  g_free (del->typed.name);
  g_free (del);
}

static void del_next (Del *del);

static void
del_stored (const char *uid, const GError *error, gpointer data)
{
  Del *del = data;

  if (error)
    {
      fail (del->window, "delete_failed", error->message);
      del_finish (del);
      return;
    }
  del->deleted++;
  del_next (del);
}

static void
del_archived (JsonObject *root, const char *err, gpointer data)
{
  Del *del = data;
  g_autoptr (GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);

  if (err)
    {
      fail (del->window, "archive_failed", err);
      del_finish (del);
      return;
    }
  g_ptr_array_add (uids, g_strdup (del->uids->pdata[del->next - 1]));
  ct_store_delete (ct_window_get_store (del->window), uids, del_stored, del);
}

static void
del_archive (Del *del)
{
  ct_mnv_archive_contact_deleted (G_OBJECT (del->window), del->uids->pdata[del->next - 1], del->known, del_archived,
                                  del);
}

static void
del_memory_done (JsonObject *root, const char *err, gpointer data)
{
  Del *del = data;

  if (err)
    {
      fail (del->window, "memory_failed", err);
      del_finish (del);
      return;
    }
  del_archive (del);
}

static void
del_proved (gboolean ok, const char *why, gpointer data)
{
  Del *del = data;
  const char *uid = del->uids->pdata[del->next - 1];
  g_autofree char *action = ct_proof_action (CT_PROOF_DELETE, uid, NULL);
  g_autofree char *proof = ct_proof_json (del->approver, action);

  if (!ok)
    {
      fail (del->window, "proof_failed", why);
      del_finish (del);
      return;
    }
  ct_window_toast_hold (del->window, ct_mnv_text ("busy_contact_delete"));
  ct_mnv_memory_contact_deleted (G_OBJECT (del->window), uid, proof, del_memory_done, del);
}

static void
del_next (Del *del)
{
  const char *uid;
  g_autofree char *name = NULL, *reason = NULL;
  gboolean is_owner;

  if (del->next >= del->uids->len)
    {
      del_finish (del);
      return;
    }
  uid = del->uids->pdata[del->next++];
  if (!del->memory)
    {
      del_archive (del);
      return;
    }
  name = name_of (del->window, uid);
  is_owner = !g_strcmp0 (uid, del->owner_uid);
  reason = ct_mnv_textf ("proof_delete_reason", (const char *const[]) { "name", name, NULL });
  ask_proof (del->window, is_owner ? "proof_method_owner" : "proof_method", reason, del_proved, del);
}

static void
del_typed (const char *text, gpointer data)
{
  Del *del = data;

  if (!text)
    {
      del_finish (del);
      return;
    }
  del_next (del);
}

static void
del_confirmed (int response, gpointer data)
{
  Del *del = data;

  if (response != CT_DIALOG_OK)
    {
      del_finish (del);
      return;
    }
  if (*del->owner_uid && del->typed.name)
    {
      g_autofree char *title = ct_mnv_textf ("owner_heading", (const char *const[]) { "name", del->typed.name, NULL });

      ct_text_dialog (del->window, title, NULL, ct_mnv_text ("owner_hint"), ct_mnv_int ("name_max_chars"), typed_check,
                      del_typed, &del->typed);
      return;
    }
  del_next (del);
}

static void
del_known (JsonObject *root, const char *err, gpointer data)
{
  Del *del = data;
  g_autofree char *count = g_strdup_printf ("%u", del->uids->len);
  g_autofree char *heading = NULL, *body = NULL;
  g_autoptr (GString) text = g_string_new (NULL);
  g_autofree char *first = NULL;

  if (err)
    {
      fail (del->window, "archive_failed", err);
      del_finish (del);
      return;
    }
  del->known = json_object_ref (root);
  del->owner_uid = ct_mnv_owner_contact (root);
  del->approver = g_strdup (ns_json_str (root, "owner"));
  del->memory = ct_mnv_memory_configured ();
  first = name_of (del->window, del->uids->pdata[0]);
  /* the owner's contact among the ones going: her name is to be typed */
  for (guint i = 0; *del->owner_uid && i < del->uids->len; i++)
    if (!g_strcmp0 (del->uids->pdata[i], del->owner_uid))
      del->typed.name = name_of (del->window, del->owner_uid);
  heading = del->uids->len == 1
    ? ct_mnv_textf ("delete_contact_heading", (const char *const[]) { "name", first, NULL })
    : ct_mnv_textf ("delete_contacts_heading", (const char *const[]) { "n", count, NULL });
  g_string_append (text, ct_mnv_text ("delete_contact_body"));
  if (!del->memory)
    g_string_append_printf (text, "\n\n%s", ct_mnv_text ("memory_off"));
  body = g_strdup (text->str);
  ct_confirm_dialog (del->window, heading, body, ct_mnv_text ("delete_contact_ok"), del_confirmed, del);
}

void
ct_removal_delete (GtkWidget *window, GPtrArray *uids, CtRemovalDone done, gpointer data)
{
  Del *del = g_new0 (Del, 1);

  del->window = g_object_ref (window);
  del->uids = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < uids->len; i++)
    g_ptr_array_add (del->uids, g_strdup (uids->pdata[i]));
  del->done = done;
  del->data = data;
  ct_mnv_known (G_OBJECT (window), del_known, del);
}

/* ---- merging ---- */

typedef struct {
  GtkWidget *window;
  char *to, *from;              /* the contact that stays, the one that goes */
  char *to_name, *from_name;
  JsonObject *known;
  gboolean memory;
  char *approver;
  Typed typed;
  CtMergeDone done;
  gpointer data;
} Mrg;

static void
mrg_finish (Mrg *mrg, gboolean merged)
{
  if (mrg->done)
    mrg->done (merged, mrg->data);
  g_object_unref (mrg->window);
  g_free (mrg->to);
  g_free (mrg->from);
  g_free (mrg->to_name);
  g_free (mrg->from_name);
  g_free (mrg->approver);
  g_free (mrg->typed.name);
  g_clear_pointer (&mrg->known, json_object_unref);
  g_free (mrg);
}

static void
mrg_removed (const char *uid, const GError *error, gpointer data)
{
  Mrg *mrg = data;

  if (error)
    {
      fail (mrg->window, "delete_failed", error->message);
      mrg_finish (mrg, FALSE);
      return;
    }
  toast (mrg->window, ct_mnv_text ("contact_merged_done"));
  mrg_finish (mrg, TRUE);
}

static void
mrg_archived (JsonObject *root, const char *err, gpointer data)
{
  Mrg *mrg = data;
  g_autoptr (GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);

  if (err)
    {
      fail (mrg->window, "archive_failed", err);
      mrg_finish (mrg, FALSE);
      return;
    }
  g_ptr_array_add (uids, g_strdup (mrg->from));
  ct_store_delete (ct_window_get_store (mrg->window), uids, mrg_removed, mrg);
}

static void
mrg_memory_done (JsonObject *root, const char *err, gpointer data)
{
  Mrg *mrg = data;

  if (err)
    {
      fail (mrg->window, "memory_failed", err);
      mrg_finish (mrg, FALSE);
      return;
    }
  ct_mnv_archive_contact_merged (G_OBJECT (mrg->window), mrg->from, mrg->to, mrg->known, mrg_archived, mrg);
}

static void
mrg_written (const char *uid, const GError *error, gpointer data)
{
  Mrg *mrg = data;

  if (error)
    {
      fail (mrg->window, "merge_failed", error->message);
      mrg_finish (mrg, FALSE);
      return;
    }
  if (!mrg->memory)
    {
      mrg_memory_done (NULL, NULL, mrg);
      return;
    }
  {
    g_autofree char *action = ct_proof_action (CT_PROOF_MERGE, mrg->from, mrg->to);
    g_autofree char *proof = ct_proof_json (mrg->approver, action);

    ct_mnv_memory_contact_merged (G_OBJECT (mrg->window), mrg->from, mrg->to, proof, mrg_memory_done, mrg);
  }
}

/* the proof is given: the merged card is written into the contact that stays */
static void
mrg_proved (gboolean ok, const char *why, gpointer data)
{
  Mrg *mrg = data;
  CtStore *store = ct_window_get_store (mrg->window);
  CtEntry *to = ct_store_lookup (store, mrg->to), *from = ct_store_lookup (store, mrg->from);
  g_autoptr (EContact) merged = NULL;
  g_autoptr (GPtrArray) changed = NULL;

  if (!ok)
    {
      fail (mrg->window, "proof_failed", why);
      mrg_finish (mrg, FALSE);
      return;
    }
  if (!to || !from)
    {
      fail (mrg->window, "merge_failed", "?");
      mrg_finish (mrg, FALSE);
      return;
    }
  ct_window_toast_hold (mrg->window, ct_mnv_text ("busy_contact_merge"));
  merged = e_contact_duplicate (to->contact);
  ct_contact_merge (merged, from->contact);
  changed = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (changed, g_object_ref (merged));
  ct_store_modify (store, changed, mrg_written, mrg);
}

static void
mrg_ask_proof (Mrg *mrg)
{
  g_autofree char *reason = NULL;

  if (!mrg->memory)
    {
      mrg_proved (TRUE, NULL, mrg);
      return;
    }
  reason = ct_mnv_textf ("proof_merge_reason", (const char *const[]) { "from", mrg->from_name, "to", mrg->to_name, NULL });
  ask_proof (mrg->window, !g_strcmp0 (mrg->from, ct_mnv_owner_contact (mrg->known)) ? "proof_method_owner"
                                                                                     : "proof_method",
             reason, mrg_proved, mrg);
}

static void
mrg_typed (const char *text, gpointer data)
{
  Mrg *mrg = data;

  if (!text)
    {
      mrg_finish (mrg, FALSE);
      return;
    }
  mrg_ask_proof (mrg);
}

static void
mrg_confirmed (int response, gpointer data)
{
  Mrg *mrg = data;

  if (response != CT_DIALOG_OK)
    {
      mrg_finish (mrg, FALSE);
      return;
    }
  /* the card that goes is the owner's: her name typed, as for a deletion */
  if (mrg->typed.name)
    {
      g_autofree char *title = ct_mnv_textf ("owner_heading", (const char *const[]) { "name", mrg->typed.name, NULL });

      ct_text_dialog (mrg->window, title, NULL, ct_mnv_text ("owner_hint"), ct_mnv_int ("name_max_chars"), typed_check,
                      mrg_typed, &mrg->typed);
      return;
    }
  mrg_ask_proof (mrg);
}

/* the dialog's text: what happens, the rule for the merged card, where the two differ, and what Minerva does */
static char *
merge_text (Mrg *mrg, guint conflicts)
{
  const char *const pairs[] = { "from", mrg->from_name, "to", mrg->to_name, NULL };
  g_autoptr (GString) text = g_string_new (NULL);
  static const struct { guint bit; const char *key; } lines[] = {
    { CT_MERGE_NAME, "merge_conflict_name" }, { CT_MERGE_PHOTO, "merge_conflict_photo" },
    { CT_MERGE_ORGANIZATION, "merge_conflict_org" }, { CT_MERGE_BIRTHDAY, "merge_conflict_birthday" },
  };
  g_autofree char *line = ct_mnv_textf ("merge_contacts_body", pairs);

  g_string_append (text, line);
  g_free (line);
  line = ct_mnv_textf ("merge_rule", pairs);
  g_string_append_printf (text, "\n\n%s", line);
  for (guint i = 0; i < G_N_ELEMENTS (lines); i++)
    if (conflicts & lines[i].bit)
      {
        g_free (line);
        line = ct_mnv_textf (lines[i].key, pairs);
        g_string_append_printf (text, "\n- %s", line);
      }
  g_free (line);
  line = ct_mnv_textf (mrg->memory ? "merge_minerva" : "memory_off", pairs);
  g_string_append_printf (text, "\n\n%s", line);
  return g_string_free (g_steal_pointer (&text), FALSE);
}

static void
mrg_known (JsonObject *root, const char *err, gpointer data)
{
  Mrg *mrg = data;
  CtStore *store = ct_window_get_store (mrg->window);
  CtEntry *to = ct_store_lookup (store, mrg->to), *from = ct_store_lookup (store, mrg->from);
  g_autoptr (EContact) scratch = NULL;
  g_autofree char *heading = NULL, *body = NULL;
  guint conflicts;

  if (err || !to || !from)
    {
      fail (mrg->window, "archive_failed", err ? err : "?");
      mrg_finish (mrg, FALSE);
      return;
    }
  mrg->known = json_object_ref (root);
  mrg->memory = ct_mnv_memory_configured ();
  mrg->approver = g_strdup (ns_json_str (root, "owner"));
  if (!g_strcmp0 (mrg->from, ct_mnv_owner_contact (root)))
    mrg->typed.name = g_strdup (mrg->from_name);
  /* the rule is run on a copy first, to say where the two differ before anything is written */
  scratch = e_contact_duplicate (to->contact);
  conflicts = ct_contact_merge (scratch, from->contact);
  heading = ct_mnv_textf ("merge_contacts_heading", (const char *const[]) { "from", mrg->from_name, "to", mrg->to_name,
                                                                            NULL });
  body = merge_text (mrg, conflicts);
  ct_confirm_dialog (mrg->window, heading, body, ct_mnv_text ("merge_contacts_ok"), mrg_confirmed, mrg);
}

static void
mrg_picked (GPtrArray *uids, gpointer data)
{
  Mrg *mrg = data;

  if (!uids->len)
    {
      mrg_finish (mrg, FALSE);
      return;
    }
  mrg->from = g_strdup (uids->pdata[0]);
  mrg->from_name = name_of (mrg->window, mrg->from);
  ct_mnv_known (G_OBJECT (mrg->window), mrg_known, mrg);
}

void
ct_removal_merge (GtkWidget *window, const char *uid, CtMergeDone done, gpointer data)
{
  Mrg *mrg = g_new0 (Mrg, 1);
  g_autoptr (GPtrArray) exclude = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *title = NULL;

  mrg->window = g_object_ref (window);
  mrg->to = g_strdup (uid);
  mrg->to_name = name_of (window, uid);
  mrg->done = done;
  mrg->data = data;
  g_ptr_array_add (exclude, g_strdup (uid));
  title = ct_mnv_textf ("merge_pick", (const char *const[]) { "name", mrg->to_name, NULL });
  /* the picker calls back only when something was picked: the job ends there if it is left */
  ct_window_push (window, ct_picker_new (window, title, exclude, FALSE, mrg_picked, mrg));
}
