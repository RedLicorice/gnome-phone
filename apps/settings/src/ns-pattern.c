/*
 * "Set pattern" / "Set PIN" (Lock screen): draw an unlock pattern, draw it again to confirm, and it becomes
 * the user's password: the dots joined in order as digits ("1478"), which gnome-phone's lock screen (keyguard.js,
 * bouncer.js) hands gp-auth, and gp-auth to PAM, when the same pattern is drawn there. As on Android the current pattern (or PIN/password) comes first,
 * checked through PAM like the lock screen does; the password is then set through AccountsService (a polkit rule
 * of ours lets the active local user do that without polkit's own text prompt, which wanted the pattern typed
 * as digits). The login keyring then gets the same password (neo-keyring-rekey), so that unlocking the lock
 * screen unlocks it too (pam_gnome_keyring) and apps find their secrets. A PIN (or a password) goes the same
 * way, typed instead of drawn: on the lock screen's PIN pad, not the keyboard (owner, 2026-11-12: the current PIN
 * and a new one are typed on "the same numeric keyboard like on lock screen", the phone's first password being a
 * PIN). A current password that is not digits still has a text field, behind "Use password".
 *
 * Taken from the Mobile Settings plugin (gnome/neo-phosh/ms-plugin-surya). libcrypt is opened at run time: the
 * phone has the library but not its header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */



#include "ns-pattern.h"
#include "ns-pattern-pad.h"

#include <adwaita.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIN_PIN 4

/* the lock screen's PIN pad (extension/bouncer.js), in dp: 3 x 4 squircle keys (|x|^4 + |y|^4, squarer pressed) */
#define PAD_KEY 68.0
#define PAD_COLUMN 96.0
#define PAD_ROW 82.0
#define PAD_DIGIT_PX 30
#define PAD_KEYS 12
static const char *const pad_keys[PAD_KEYS] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "delete", "0", "enter" };

/* libcrypt's crypt_gensalt and crypt, found at run time */
static char *
hash_password (const char *password)
{
  static GModule *lib;
  static char *(*gensalt) (const char *, unsigned long, const char *, int);
  static char *(*crypt_) (const char *, const char *);
  if (!lib) {
    lib = g_module_open ("libcrypt.so.1", G_MODULE_BIND_LAZY);
    if (!lib || !g_module_symbol (lib, "crypt_gensalt", (gpointer *) &gensalt)
        || !g_module_symbol (lib, "crypt", (gpointer *) &crypt_))
      return NULL;
  }
  /* yescrypt, as passwd writes it */
  const char *salt = gensalt ("$y$", 0, NULL, 0);
  const char *h = salt ? crypt_ (password, salt) : NULL;
  return h && h[0] != '*' ? g_strdup (h) : NULL;
}

typedef enum {
  STAGE_CURRENT,
  STAGE_DRAW,
  STAGE_CONFIRM,
} Stage;

typedef struct {
  AdwDialog      *dialog;
  GtkWidget      *area;
  GtkWidget      *entry;        /* a current password that is not digits ("Use password") */
  GtkWidget      *pad;          /* the PIN pad: a current PIN, a new one */
  GtkWidget      *dots;         /* what the pad typed, as dots */
  GtkWidget      *use_text;     /* the current password in a text field instead */
  gboolean        text_mode;
  char            typed[128];
  guint           typed_len;
  int             pressed;      /* the pad's key under the finger, or -1 */
  gboolean        current_is_pattern;
  char            current[128];
  GtkWidget      *message;
  GtkWidget      *cancel;
  GtkWidget      *next;
  Stage           stage;
  gboolean        pin;          /* a PIN typed, not a pattern drawn */
  char            seq[10];
  guint           len;
  char            first[128];
  gboolean        done;
  NsPatternDone   callback;
  gpointer        user_data;
  /* ns_pattern_verify_present: the current secret only, or a finger on the sensor (fprintd) */
  gboolean        verify;
  char           *reason;
  GDBusConnection *fp_bus;
  char           *fp_device;
  GCancellable   *fp_cancel;
  guint           fp_signal;
  gboolean        fp_claimed;
  gboolean        fp_listening;
  int             fp_misses;
  gboolean        fp_matched;   /* a finger matched while the PAM check of the secret was running */
  /* references: the dialog's own (dropped in on_closed) and one per PAM check in flight (check_thread) */
  int             refs;
  gboolean        closed;
  gboolean        checking;
} PatternDialog;



static gboolean
pad_shown (PatternDialog *pd)
{
  return pd->stage == STAGE_CURRENT ? !pd->current_is_pattern && !pd->text_mode : pd->pin;
}


/* the PIN or password typed: on the pad, or in the text field */
static const char *
typed_text (PatternDialog *pd)
{
  return pad_shown (pd) ? pd->typed : gtk_editable_get_text (GTK_EDITABLE (pd->entry));
}


static void
clear_typed (PatternDialog *pd)
{
  memset (pd->typed, 0, sizeof pd->typed);
  pd->typed_len = 0;
  gtk_editable_set_text (GTK_EDITABLE (pd->entry), "");
  gtk_widget_queue_draw (pd->dots);
}


static void update_next (PatternDialog *pd);


/* the line asking for the current secret; in a verify dialog the reason comes first, then `note` (what the
 * sensor just said, or NULL), and the fingerprint sensor is offered while fprintd listens */
static char *
current_prompt (PatternDialog *pd, const char *note)
{
  const char *what = pd->current_is_pattern ? "pattern" : pd->text_mode ? "password" : "PIN";
  const char *verb = pd->current_is_pattern ? "Draw" : "Enter";

  if (!pd->verify)
    return g_strdup_printf ("%s your current %s.", verb, what);
  g_autofree char *ask = pd->fp_listening
                         ? g_strdup_printf ("%s your %s or touch the fingerprint sensor.", verb, what)
                         : g_strdup_printf ("%s your %s.", verb, what);
  return g_strjoin ("\n\n", pd->reason ? pd->reason : ask, pd->reason ? (note ? note : ask) : NULL,
                    pd->reason && note ? ask : NULL, NULL);
}


static void
set_stage (PatternDialog *pd, Stage stage)
{
  pd->stage = stage;
  pd->len = 0;
  pd->seq[0] = '\0';
  if (NS_IS_PATTERN_PAD (pd->area))
    ns_pattern_pad_clear (NS_PATTERN_PAD (pd->area));
  gboolean draw = stage == STAGE_CURRENT ? pd->current_is_pattern : !pd->pin;
  gboolean pad = pad_shown (pd);
  gtk_widget_set_visible (pd->area, draw);
  gtk_widget_set_visible (pd->pad, pad);
  gtk_widget_set_visible (pd->dots, pad);
  gtk_widget_set_visible (pd->entry, !draw && !pad);
  gtk_widget_set_visible (pd->use_text, stage == STAGE_CURRENT && !pd->current_is_pattern);
  gtk_button_set_label (GTK_BUTTON (pd->use_text), pd->text_mode ? "Use the PIN pad" : "Use password");
  clear_typed (pd);
  if (stage == STAGE_CURRENT) {
    g_autofree char *prompt = current_prompt (pd, NULL);
    gtk_label_set_text (GTK_LABEL (pd->message), prompt);
    gtk_button_set_label (GTK_BUTTON (pd->next), pd->verify ? "Confirm" : "Continue");
    if (!pd->current_is_pattern && pd->text_mode)
      gtk_widget_grab_focus (pd->entry);
  } else if (stage == STAGE_DRAW) {
    gtk_label_set_text (GTK_LABEL (pd->message),
                        pd->pin ? "Enter a new PIN of at least 4 digits. It becomes your password."
                                : "Draw your unlock pattern. It becomes your password: connect at least 4 dots.");
    gtk_button_set_label (GTK_BUTTON (pd->next), "Continue");
  } else {
    gtk_label_set_text (GTK_LABEL (pd->message), pd->pin ? "Enter the PIN again to confirm."
                                                         : "Draw the pattern again to confirm.");
    gtk_button_set_label (GTK_BUTTON (pd->next), "Confirm");
  }
  gtk_widget_set_sensitive (pd->next, FALSE);
  gtk_widget_queue_draw (pd->area);
}


/* the pattern pad, when the pattern library is there (else a note in its place) */
static NsPatternPad *
pattern_pad (PatternDialog *pd)
{
  return NS_IS_PATTERN_PAD (pd->area) ? NS_PATTERN_PAD (pd->area) : NULL;
}


/* a finger went down on the pad: a new pattern */
static void
on_pattern_started (PatternDialog *pd)
{
  pd->len = 0;
  pd->seq[0] = '\0';
  gtk_widget_set_sensitive (pd->next, FALSE);
}


/* the finger was lifted: the pattern is the pad's digits; a short one (libminerva-pattern's min_dots) is none */
static void
on_pattern_finished (PatternDialog *pd)
{
  NsPatternPad *pad = pattern_pad (pd);

  g_strlcpy (pd->seq, ns_pattern_pad_get_digits (pad), sizeof pd->seq);
  pd->len = strlen (pd->seq);
  if (pd->stage == STAGE_CURRENT) {
    gtk_widget_set_sensitive (pd->next, TRUE);
  } else if (!ns_pattern_pad_complete (pad)) {
    ns_pattern_pad_set_error (pad, TRUE);
    gtk_label_set_text (GTK_LABEL (pd->message), "Connect at least 4 dots. Try again.");
  } else if (pd->stage == STAGE_CONFIRM && g_strcmp0 (pd->seq, pd->first) != 0) {
    ns_pattern_pad_set_error (pad, TRUE);
    gtk_label_set_text (GTK_LABEL (pd->message), "That was a different pattern. Try again.");
  } else {
    gtk_label_set_text (GTK_LABEL (pd->message),
                        pd->stage == STAGE_DRAW ? "Pattern recorded." : "Pattern confirmed.");
    gtk_widget_set_sensitive (pd->next, TRUE);
  }
}


static void fp_stop (PatternDialog *pd);

static void
finish (PatternDialog *pd, gboolean ok)
{
  if (pd->done)
    return;
  pd->done = TRUE;
  fp_stop (pd);
  if (pd->callback)
    pd->callback (ok, pd->user_data);
  adw_dialog_force_close (pd->dialog);
}


/* --- a finger instead of the secret (verify dialogs): fprintd, the way the lock screen's PAM asks it -------- */

#define FP_BUS "net.reactivated.Fprint"
#define FP_DEVICE "net.reactivated.Fprint.Device"
#define FP_MAX_MISSES 5

static void fp_listen (PatternDialog *pd);

static void
fp_say (PatternDialog *pd, const char *note)
{
  if (pd->done || pd->stage != STAGE_CURRENT)
    return;
  g_autofree char *prompt = current_prompt (pd, note);
  gtk_label_set_text (GTK_LABEL (pd->message), prompt);
}


/* the sensor is let go: the verify stopped, the device released (fire and forget, the dialog may be gone) */
static void
fp_stop (PatternDialog *pd)
{
  g_cancellable_cancel (pd->fp_cancel);
  g_clear_object (&pd->fp_cancel);
  if (pd->fp_bus && pd->fp_signal)
    g_dbus_connection_signal_unsubscribe (pd->fp_bus, pd->fp_signal);
  pd->fp_signal = 0;
  /* Release also when a Claim was still on its way: one that fprintd granted would otherwise hold the sensor
   * until Settings quits (an unclaimed device just answers with an error) */
  if (pd->fp_bus && pd->fp_device) {
    if (pd->fp_listening)
      g_dbus_connection_call (pd->fp_bus, FP_BUS, pd->fp_device, FP_DEVICE, "VerifyStop", NULL, NULL,
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_dbus_connection_call (pd->fp_bus, FP_BUS, pd->fp_device, FP_DEVICE, "Release", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  }
  pd->fp_claimed = pd->fp_listening = FALSE;
  g_clear_pointer (&pd->fp_device, g_free);
  g_clear_object (&pd->fp_bus);
}


/* a call's answer; NULL when it failed, or when the dialog went away meanwhile (then `pd` is not touched) */
static GVariant *
fp_finish (GObject *source, GAsyncResult *res, PatternDialog **pd, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GVariant *ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    *pd = NULL;
    return NULL;
  }
  *pd = data;
  if (!ret)
    g_debug ("fprintd: %s", error->message);
  return ret;
}


static void
on_fp_restarted (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd;
  g_autoptr (GVariant) ret = fp_finish (source, res, &pd, data);
  if (pd)
    fp_listen (pd);
}


static void
on_fp_status (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
              GVariant *params, gpointer data)
{
  PatternDialog *pd = data;
  const char *result;
  gboolean done;

  if (pd->done)
    return;
  g_variant_get (params, "(&sb)", &result, &done);
  if (g_str_equal (result, "verify-match")) {
    /* the secret is being checked on a thread that writes to the dialog when it ends: on_checked finishes */
    if (pd->checking)
      pd->fp_matched = TRUE;
    else
      finish (pd, TRUE);
    return;
  }
  if (g_str_equal (result, "verify-no-match")) {
    pd->fp_misses++;
    fp_say (pd, pd->fp_misses < FP_MAX_MISSES ? "Fingerprint not recognised." : NULL);
  } else if (g_str_has_prefix (result, "verify-retry") || g_str_has_prefix (result, "verify-swipe")
             || g_str_equal (result, "verify-finger-not-centered") || g_str_equal (result, "verify-remove-and-retry")) {
    fp_say (pd, "Try the finger again.");
  }
  if (!done)
    return;
  /* the verify is over (a miss, the sensor gone, its own timeout): listen again unless that was the last try */
  pd->fp_listening = FALSE;
  if (pd->fp_misses >= FP_MAX_MISSES || !g_str_equal (result, "verify-no-match")) {
    fp_say (pd, pd->fp_misses >= FP_MAX_MISSES ? "Too many tries with the finger." : NULL);
    return;
  }
  g_dbus_connection_call (pd->fp_bus, FP_BUS, pd->fp_device, FP_DEVICE, "VerifyStop", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, pd->fp_cancel, on_fp_restarted, pd);
}


static void
on_fp_listening (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd;
  g_autoptr (GVariant) ret = fp_finish (source, res, &pd, data);
  if (!pd)
    return;
  /* no prints enrolled, or the sensor busy: the secret alone */
  pd->fp_listening = ret != NULL;
  fp_say (pd, NULL);
}


static void
fp_listen (PatternDialog *pd)
{
  g_dbus_connection_call (pd->fp_bus, FP_BUS, pd->fp_device, FP_DEVICE, "VerifyStart", g_variant_new ("(s)", "any"),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, pd->fp_cancel, on_fp_listening, pd);
}


static void
on_fp_claimed (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd;
  g_autoptr (GVariant) ret = fp_finish (source, res, &pd, data);
  if (!pd || !ret)
    return;
  pd->fp_claimed = TRUE;
  pd->fp_signal = g_dbus_connection_signal_subscribe (pd->fp_bus, FP_BUS, FP_DEVICE, "VerifyStatus", pd->fp_device,
                                                      NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_fp_status, pd, NULL);
  fp_listen (pd);
}


static void
on_fp_device (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd;
  g_autoptr (GVariant) ret = fp_finish (source, res, &pd, data);
  if (!pd || !ret)
    return;
  g_variant_get (ret, "(o)", &pd->fp_device);
  /* "": the caller's own prints */
  g_dbus_connection_call (pd->fp_bus, FP_BUS, pd->fp_device, FP_DEVICE, "Claim", g_variant_new ("(s)", ""), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, pd->fp_cancel, on_fp_claimed, pd);
}


/* no fprintd, no sensor, no prints: nothing is offered and the secret is asked for alone */
static void
fp_start (PatternDialog *pd)
{
  pd->fp_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (!pd->fp_bus)
    return;
  pd->fp_cancel = g_cancellable_new ();
  g_dbus_connection_call (pd->fp_bus, FP_BUS, "/net/reactivated/Fprint/Manager", "net.reactivated.Fprint.Manager",
                          "GetDefaultDevice", NULL, G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                          pd->fp_cancel, on_fp_device, pd);
}


/* --- setting the password through AccountsService ------------------------------------------------------------- */

/* The account password is set either way; a keyring left on the old password only means apps ask for it. */
static void
on_keyring_rekeyed (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *err_out = NULL;

  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &err_out, &error))
    g_warning ("neo-keyring-rekey: %s", error->message);
  else if (!g_subprocess_get_successful (G_SUBPROCESS (source)))
    g_warning ("The login keyring kept its old password: %s", err_out ? err_out : "");
  finish (pd, TRUE);
}


static void
rekey_keyring (PatternDialog *pd)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) proc = NULL;
  /* the new password, then the current one: with it the keyring's password is changed in place */
  g_autofree char *line = g_strconcat (pd->first, "\n", pd->current, "\n", NULL);

  gtk_label_set_text (GTK_LABEL (pd->message), "Moving the keyring to the new password…");
  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                           G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
                           NS_REKEY, NULL);
  if (!proc) {
    g_warning ("neo-keyring-rekey: %s", error->message);
    finish (pd, TRUE);
    return;
  }
  g_subprocess_communicate_utf8_async (proc, line, NULL, on_keyring_rekeyed, pd);
}


static void
on_password_set (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (!ret) {
    g_warning ("Setting the password failed: %s", error->message);
    gtk_label_set_text (GTK_LABEL (pd->message), "The password was not changed (not authorized?). Try again.");
    gtk_widget_set_sensitive (pd->next, TRUE);
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  rekey_keyring (pd);
}


static void
on_user_found (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  g_autofree char *hashed = NULL;
  const char *path;

  if (!ret) {
    g_warning ("AccountsService has no user: %s", error->message);
    gtk_label_set_text (GTK_LABEL (pd->message), "AccountsService is not available: the password was not changed.");
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  g_variant_get (ret, "(&o)", &path);
  hashed = hash_password (pd->first);
  if (!hashed) {
    gtk_label_set_text (GTK_LABEL (pd->message), "Could not hash the password.");
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  /* AccountsService's polkit policy may ask for the current password (GNOME Shell's polkit prompt) */
  g_dbus_connection_call (G_DBUS_CONNECTION (source), "org.freedesktop.Accounts", path,
                          "org.freedesktop.Accounts.User", "SetPassword",
                          g_variant_new ("(ss)", hashed, ""), NULL,
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 120000, NULL,
                          on_password_set, pd);
}


/* --- the current password, checked as the lock screen checks it ---------------------------------------------- */

static int
pam_answer (int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
  struct pam_response *r = calloc (n, sizeof *r);

  if (!r)
    return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
      r[i].resp = strdup (data);
  }
  *resp = r;
  return PAM_SUCCESS;
}


static void
check_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  struct pam_conv conv = { pam_answer, data };
  pam_handle_t *handle = NULL;
  /* the lock screen's own PAM service (gp-auth's "gnome-phone", /etc/pam.d/gnome-phone), so what unlocks the
   * phone is what is asked for here */
  int rc = pam_start ("gnome-phone", g_get_user_name (), &conv, &handle);

  if (rc == PAM_SUCCESS)
    rc = pam_authenticate (handle, 0);
  pam_end (handle, rc);
  g_task_return_boolean (task, rc == PAM_SUCCESS);
}


static void pd_unref (PatternDialog *pd);

static void
on_checked (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  gboolean ok = g_task_propagate_boolean (G_TASK (res), NULL);

  pd->checking = FALSE;
  /* the dialog went away meanwhile (its parent closed): nothing left to write to */
  if (pd->closed) {
    pd_unref (pd);
    return;
  }
  adw_dialog_set_can_close (pd->dialog, TRUE);
  gtk_widget_set_sensitive (pd->cancel, TRUE);
  gtk_widget_set_sensitive (pd->pad, TRUE);
  if (ok || (pd->verify && pd->fp_matched)) {
    if (pd->verify)
      finish (pd, TRUE);
    else
      set_stage (pd, STAGE_DRAW);
    pd_unref (pd);
    return;
  }
  memset (pd->current, 0, sizeof pd->current);
  if (NS_IS_PATTERN_PAD (pd->area))
    ns_pattern_pad_set_error (NS_PATTERN_PAD (pd->area), TRUE);
  gtk_label_set_text (GTK_LABEL (pd->message), pd->current_is_pattern ? "That is not your current pattern. Try again."
                                               : pd->text_mode         ? "That is not your current password."
                                                                       : "That is not your current PIN.");
  clear_typed (pd);
  gtk_widget_queue_draw (pd->area);
  pd_unref (pd);
}


static void
check_current (PatternDialog *pd)
{
  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_checked, pd);

  pd->refs++;
  pd->checking = TRUE;
  g_strlcpy (pd->current, pd->current_is_pattern ? pd->seq : typed_text (pd), sizeof pd->current);
  gtk_widget_set_sensitive (pd->next, FALSE);
  gtk_widget_set_sensitive (pd->cancel, FALSE);
  gtk_widget_set_sensitive (pd->pad, FALSE);
  gtk_label_set_text (GTK_LABEL (pd->message), "Checking…");
  /* the dialog stays until the answer is in: on_checked writes to it */
  adw_dialog_set_can_close (pd->dialog, FALSE);
  g_task_set_task_data (task, g_strdup (pd->current), g_free);
  g_task_run_in_thread (task, check_thread);
}


static void
update_next (PatternDialog *pd)
{
  const char *t = typed_text (pd);
  if (pd->stage == STAGE_CURRENT && !pd->current_is_pattern)
    gtk_widget_set_sensitive (pd->next, *t != '\0');
  else if (pd->stage != STAGE_CURRENT && pd->pin) {
    gboolean digits = strlen (t) >= MIN_PIN && strlen (t) < sizeof pd->first;
    for (const char *c = t; *c; c++)
      digits = digits && g_ascii_isdigit (*c);
    gtk_widget_set_sensitive (pd->next, digits);
  }
}


static void
on_next (PatternDialog *pd)
{
  g_autoptr (GDBusConnection) bus = NULL;

  if (pd->stage == STAGE_CURRENT) {
    if (gtk_widget_get_sensitive (pd->next))
      check_current (pd);
    return;
  }
  if (!gtk_widget_get_sensitive (pd->next))
    return;
  if (pd->stage == STAGE_DRAW) {
    g_strlcpy (pd->first, pd->pin ? typed_text (pd) : pd->seq, sizeof pd->first);
    set_stage (pd, STAGE_CONFIRM);
    return;
  }
  if (pd->pin && g_strcmp0 (pd->first, typed_text (pd))) {
    gtk_label_set_text (GTK_LABEL (pd->message), "That was a different PIN. Try again.");
    clear_typed (pd);
    gtk_widget_set_sensitive (pd->next, FALSE);
    return;
  }
  gtk_widget_set_sensitive (pd->next, FALSE);
  gtk_widget_set_sensitive (pd->cancel, FALSE);
  gtk_label_set_text (GTK_LABEL (pd->message), "Setting the pattern as your password…");
  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (!bus) {
    gtk_label_set_text (GTK_LABEL (pd->message), "No system bus: the password was not changed.");
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  g_dbus_connection_call (bus, "org.freedesktop.Accounts", "/org/freedesktop/Accounts",
                          "org.freedesktop.Accounts", "FindUserById",
                          g_variant_new ("(x)", (gint64) getuid ()), G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_user_found, pd);
}


/* --- the PIN pad ------------------------------------------------------------------------------------------------ */

/* the lock screen's key click (haptics.js click(): feedbackd's button-released) as the finger lands */
static void
pad_click (void)
{
  g_autoptr (GDBusConnection) session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session)
    g_dbus_connection_call (session, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback",
                            "TriggerFeedback", g_variant_new ("(ssa{sv}i)", "de.yesman.NeoSettings",
                                                              "button-released", NULL, -1),
                            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}


static void
pad_center (GtkWidget *w, int key, double *cx, double *cy)
{
  *cx = gtk_widget_get_width (w) / 2.0 + (key % 3 - 1) * PAD_COLUMN;
  *cy = PAD_ROW / 2.0 + (key / 3) * PAD_ROW;
}


static int
pad_key_at (GtkWidget *w, double x, double y)
{
  for (int k = 0; k < PAD_KEYS; k++) {
    double cx, cy;
    pad_center (w, k, &cx, &cy);
    if (fabs (x - cx) <= PAD_COLUMN / 2.0 && fabs (y - cy) <= PAD_ROW / 2.0)
      return k;
  }
  return -1;
}


static void
squircle (cairo_t *cr, double cx, double cy, double r, double n)
{
  cairo_new_path (cr);
  for (int i = 0; i < 96; i++) {
    double t = 2 * G_PI * i / 96, c = cos (t), s = sin (t);
    double x = cx + r * copysign (pow (fabs (c), 2 / n), c), y = cy + r * copysign (pow (fabs (s), 2 / n), s);
    if (i == 0)
      cairo_move_to (cr, x, y);
    else
      cairo_line_to (cr, x, y);
  }
  cairo_close_path (cr);
}


static void
pad_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PatternDialog *pd = data;
  GtkWidget *w = GTK_WIDGET (area);
  GdkRGBA fg;
  gtk_widget_get_color (w, &fg);
  PangoLayout *layout = gtk_widget_create_pango_layout (w, NULL);
  PangoFontDescription *font = pango_font_description_from_string ("Google Sans Flex Semi-Bold");
  pango_font_description_set_absolute_size (font, PAD_DIGIT_PX * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);

  for (int k = 0; k < PAD_KEYS; k++) {
    double cx, cy;
    gboolean action = k == 9 || k == 11, pressed = k == pd->pressed;
    pad_center (w, k, &cx, &cy);
    squircle (cr, cx, cy, PAD_KEY / 2, pressed ? 5 : 4);
    if (pressed)
      cairo_set_source_rgb (cr, 16 / 255.0, 78 / 255.0, 86 / 255.0);          /* primary container */
    else
      cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, action ? 0.10 : 0.14);
    cairo_fill (cr);
    cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 1);
    if (k == 9) {
      /* delete: a key cap pointing left, with an x */
      double s = PAD_KEY * 0.16;
      cairo_set_line_width (cr, 2);
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
      cairo_move_to (cr, cx - 1.6 * s, cy);
      cairo_line_to (cr, cx - 0.8 * s, cy - s);
      cairo_line_to (cr, cx + 1.4 * s, cy - s);
      cairo_line_to (cr, cx + 1.4 * s, cy + s);
      cairo_line_to (cr, cx - 0.8 * s, cy + s);
      cairo_close_path (cr);
      cairo_move_to (cr, cx - 0.1 * s, cy - 0.45 * s);
      cairo_line_to (cr, cx + 0.8 * s, cy + 0.45 * s);
      cairo_move_to (cr, cx + 0.8 * s, cy - 0.45 * s);
      cairo_line_to (cr, cx - 0.1 * s, cy + 0.45 * s);
      cairo_stroke (cr);
    } else if (k == 11) {
      /* enter: a tick */
      double s = PAD_KEY * 0.16;
      cairo_set_line_width (cr, 2.5);
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
      cairo_move_to (cr, cx - 1.1 * s, cy + 0.05 * s);
      cairo_line_to (cr, cx - 0.3 * s, cy + 0.85 * s);
      cairo_line_to (cr, cx + 1.2 * s, cy - 0.75 * s);
      cairo_stroke (cr);
    } else {
      int tw, th;
      if (pressed)
        cairo_set_source_rgb (cr, 184 / 255.0, 235 / 255.0, 243 / 255.0);     /* accent1_100 */
      pango_layout_set_text (layout, pad_keys[k], -1);
      pango_layout_get_pixel_size (layout, &tw, &th);
      cairo_move_to (cr, cx - tw / 2.0, cy - th / 2.0);
      pango_cairo_show_layout (cr, layout);
    }
  }
  pango_font_description_free (font);
  g_object_unref (layout);
}


static void
dots_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PatternDialog *pd = data;
  GdkRGBA fg;
  gtk_widget_get_color (GTK_WIDGET (area), &fg);
  double step = 18, r = 5, x0 = width / 2.0 - (pd->typed_len - 1) * step / 2.0;
  cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 1);
  for (guint i = 0; i < pd->typed_len; i++) {
    cairo_arc (cr, x0 + i * step, height / 2.0, r, 0, 2 * G_PI);
    cairo_fill (cr);
  }
}


static void
pad_key (PatternDialog *pd, int k)
{
  if (!gtk_widget_get_sensitive (pd->pad))
    return;
  if (k == 9) {
    if (pd->typed_len > 0)
      pd->typed[--pd->typed_len] = '\0';
  } else if (k == 11) {
    on_next (pd);
    return;
  } else if (pd->typed_len < 16) {
    pd->typed[pd->typed_len++] = pad_keys[k][0];
    pd->typed[pd->typed_len] = '\0';
  }
  gtk_widget_queue_draw (pd->dots);
  update_next (pd);
}


static void
pad_pressed (GtkGestureClick *click, int n, double x, double y, PatternDialog *pd)
{
  pd->pressed = pad_key_at (pd->pad, x, y);
  if (pd->pressed >= 0)
    pad_click ();
  gtk_widget_queue_draw (pd->pad);
}


static void
pad_released (GtkGestureClick *click, int n, double x, double y, PatternDialog *pd)
{
  int k = pd->pressed;
  pd->pressed = -1;
  gtk_widget_queue_draw (pd->pad);
  if (k >= 0 && k == pad_key_at (pd->pad, x, y))
    pad_key (pd, k);
}


static void
pad_cancelled (PatternDialog *pd)
{
  pd->pressed = -1;
  gtk_widget_queue_draw (pd->pad);
}


/* a hardware keyboard types on the pad too */
static gboolean
pad_key_pressed (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, PatternDialog *pd)
{
  if (!gtk_widget_get_visible (pd->pad))
    return GDK_EVENT_PROPAGATE;
  guint32 c = gdk_keyval_to_unicode (keyval);
  if (c >= '0' && c <= '9')
    pad_key (pd, c == '0' ? 10 : (int) (c - '1'));
  else if (keyval == GDK_KEY_BackSpace)
    pad_key (pd, 9);
  else if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    pad_key (pd, 11);
  else
    return GDK_EVENT_PROPAGATE;
  return GDK_EVENT_STOP;
}


static void
on_use_text (PatternDialog *pd)
{
  pd->text_mode = !pd->text_mode;
  set_stage (pd, STAGE_CURRENT);
}


static void
on_cancel (PatternDialog *pd)
{
  finish (pd, FALSE);
}


static void
pd_unref (PatternDialog *pd)
{
  if (--pd->refs > 0)
    return;
  g_free (pd->reason);
  memset (pd->current, 0, sizeof pd->current);
  memset (pd->first, 0, sizeof pd->first);
  memset (pd->typed, 0, sizeof pd->typed);
  g_free (pd);
}


static void
on_closed (PatternDialog *pd)
{
  fp_stop (pd);
  if (!pd->done && pd->callback)
    pd->callback (FALSE, pd->user_data);
  pd->done = pd->closed = TRUE;
  pd_unref (pd);
}


static void
present (GtkWidget *parent, gboolean pin, gboolean verify, const char *title, const char *reason,
         NsPatternDone callback, gpointer user_data)
{
  PatternDialog *pd = g_new0 (PatternDialog, 1);
  GtkWidget *box, *buttons, *toolbar, *header;

  pd->refs = 1;
  pd->callback = callback;
  pd->user_data = user_data;
  pd->pin = pin;
  pd->verify = verify;
  pd->reason = g_strdup (reason);
  pd->pressed = -1;
  {
    /* what unlocks the phone now: the dot grid for a pattern, a text field for a PIN or a password */
    GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                               "de.yesman.neo", TRUE);
    if (schema) {
      /* made from the schema just found: g_settings_new () on a schema that is not installed aborts the app */
      g_autoptr (GSettings) neo = g_settings_new_full (schema, NULL, NULL);
      g_autofree char *type = g_settings_get_string (neo, "lock-type");

      pd->current_is_pattern = g_strcmp0 (type, "pattern") == 0;
      g_settings_schema_unref (schema);
    }
  }
  pd->dialog = ADW_DIALOG (adw_dialog_new ());
  /* floating, not a bottom sheet: a sheet is dragged down to close, the same stroke that draws a pattern */
  adw_dialog_set_presentation_mode (pd->dialog, ADW_DIALOG_FLOATING);
  adw_dialog_set_title (pd->dialog, verify ? title : pin ? "Set PIN" : "Set pattern");
  adw_dialog_set_content_width (pd->dialog, 360);
  adw_dialog_set_content_height (pd->dialog, 620);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);

  pd->message = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (pd->message), TRUE);
  gtk_label_set_justify (GTK_LABEL (pd->message), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (box), pd->message);

  /* the pattern: libminerva-pattern's pad (the lock screen's own); without the library a note, no pattern */
  if (ns_pattern_pad_available ()) {
    pd->area = ns_pattern_pad_new ();
    g_signal_connect_swapped (pd->area, "started", G_CALLBACK (on_pattern_started), pd);
    g_signal_connect_swapped (pd->area, "finished", G_CALLBACK (on_pattern_finished), pd);
  } else {
    pd->area = gtk_label_new ("The unlock pattern is not installed (minerva-pattern).");
    gtk_label_set_wrap (GTK_LABEL (pd->area), TRUE);
  }
  gtk_widget_set_size_request (pd->area, 260, 260);
  gtk_widget_set_vexpand (pd->area, TRUE);
  gtk_box_append (GTK_BOX (box), pd->area);

  pd->entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (pd->entry), TRUE);
  gtk_widget_set_valign (pd->entry, GTK_ALIGN_START);
  gtk_widget_set_vexpand (pd->entry, TRUE);
  g_signal_connect_swapped (pd->entry, "changed", G_CALLBACK (update_next), pd);
  g_signal_connect_swapped (pd->entry, "activate", G_CALLBACK (on_next), pd);
  gtk_box_append (GTK_BOX (box), pd->entry);

  pd->dots = gtk_drawing_area_new ();
  gtk_widget_set_size_request (pd->dots, -1, 24);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (pd->dots), dots_draw, pd, NULL);
  gtk_box_append (GTK_BOX (box), pd->dots);

  pd->pad = gtk_drawing_area_new ();
  gtk_widget_set_size_request (pd->pad, (int) (3 * PAD_COLUMN), (int) (4 * PAD_ROW));
  gtk_widget_set_vexpand (pd->pad, TRUE);
  gtk_widget_set_valign (pd->pad, GTK_ALIGN_CENTER);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (pd->pad), pad_draw, pd, NULL);
  {
    GtkGesture *click = gtk_gesture_click_new ();
    g_signal_connect (click, "pressed", G_CALLBACK (pad_pressed), pd);
    g_signal_connect (click, "released", G_CALLBACK (pad_released), pd);
    g_signal_connect_swapped (click, "cancel", G_CALLBACK (pad_cancelled), pd);
    gtk_widget_add_controller (pd->pad, GTK_EVENT_CONTROLLER (click));
  }
  gtk_box_append (GTK_BOX (box), pd->pad);

  pd->use_text = gtk_button_new_with_label ("Use password");
  gtk_widget_add_css_class (pd->use_text, "flat");
  gtk_widget_set_halign (pd->use_text, GTK_ALIGN_CENTER);
  g_signal_connect_swapped (pd->use_text, "clicked", G_CALLBACK (on_use_text), pd);
  gtk_box_append (GTK_BOX (box), pd->use_text);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  pd->cancel = gtk_button_new_with_label ("Cancel");
  gtk_widget_add_css_class (pd->cancel, "pill");
  pd->next = gtk_button_new_with_label ("Continue");
  gtk_widget_add_css_class (pd->next, "pill");
  gtk_widget_add_css_class (pd->next, "suggested-action");
  g_signal_connect_swapped (pd->cancel, "clicked", G_CALLBACK (on_cancel), pd);
  g_signal_connect_swapped (pd->next, "clicked", G_CALLBACK (on_next), pd);
  gtk_box_append (GTK_BOX (buttons), pd->cancel);
  gtk_box_append (GTK_BOX (buttons), pd->next);
  gtk_box_append (GTK_BOX (box), buttons);

  toolbar = adw_toolbar_view_new ();
  header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_dialog_set_child (pd->dialog, toolbar);
  g_signal_connect_swapped (pd->dialog, "closed", G_CALLBACK (on_closed), pd);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();
    g_signal_connect (keys, "key-pressed", G_CALLBACK (pad_key_pressed), pd);
    gtk_widget_add_controller (GTK_WIDGET (pd->dialog), keys);
  }

  set_stage (pd, STAGE_CURRENT);
  if (verify)
    fp_start (pd);
  adw_dialog_present (pd->dialog, parent);
}


void
ns_pattern_dialog_present (GtkWidget *parent, gboolean pin, NsPatternDone callback, gpointer user_data)
{
  present (parent, pin, FALSE, NULL, NULL, callback, user_data);
}


void
ns_pattern_verify_present (GtkWidget *parent, const char *title, const char *reason, NsPatternDone callback,
                           gpointer user_data)
{
  present (parent, FALSE, TRUE, title, reason, callback, user_data);
}
