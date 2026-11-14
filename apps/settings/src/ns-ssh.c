/*
 * SSH (Security and privacy): who may log in to this phone over SSH, and the user's own key pairs.
 *
 * "Allowed to log in" is ~/.ssh/authorized_keys, changed a line at a time: the other lines, comments and a key's
 * options (from=, command=, ...) stay as they are, and the file is replaced atomically with mode 0600, as sshd's
 * StrictModes wants. Adding or removing a key, and deleting a key pair, first asks for the PIN (or pattern) or a
 * finger, as the lock screen does (ns_pattern_verify_present; owner, 2026-11-14). New keys are checked by
 * ssh-keygen before they go in. The fingerprints in the lists are computed here, the way ssh-keygen -l prints
 * them (SHA256 of the key blob, base64 without padding).
 *
 * "My keys" are the pairs in ~/.ssh: a private key with its .pub next to it. ssh-keygen makes and reads them; a
 * passphrase reaches it through SSH_ASKPASS, answered by this binary (ns_ssh_askpass), so it never shows on a
 * command line. A public key is shared by copying it, as a QR code (libqrencode, opened at run time the way
 * ns-pattern.c opens libcrypt) or as a file. Private keys never leave the phone and are never shown.
 *
 * Not here yet: ~/.ssh/config, known_hosts, and turning password logins off (sshd_config.d, which is root's).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ns-pages.h"
#include "ns-pattern.h"
#include "ns-ui.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- keys as text ------------------------------------------------------------------------------------------- */

typedef struct {
  char *line;         /* the whole line, as in the file (trailing blanks cut) */
  char *options;      /* what comes before the key type (authorized_keys), or NULL */
  char *type;
  char *blob;         /* base64 */
  char *comment;      /* or NULL */
  char *fingerprint;  /* SHA256:... */
} SshKey;

static void
ssh_key_free (SshKey *k)
{
  if (!k)
    return;
  g_free (k->line);
  g_free (k->options);
  g_free (k->type);
  g_free (k->blob);
  g_free (k->comment);
  g_free (k->fingerprint);
  g_free (k);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SshKey, ssh_key_free)

/* no authorized_keys option starts like a key type */
static gboolean
is_key_type (const char *t)
{
  return g_str_has_prefix (t, "ssh-") || g_str_has_prefix (t, "ecdsa-sha2-") || g_str_has_prefix (t, "sk-");
}

/* the next run of non-blanks from *p; `quoted`: blanks inside double quotes belong to it, as sshd reads options */
static char *
next_token (const char **p, gboolean quoted)
{
  const char *s = *p, *e;
  gboolean in_quote = FALSE;

  while (*s == ' ' || *s == '\t')
    s++;
  for (e = s; *e && (in_quote || (*e != ' ' && *e != '\t')); e++) {
    if (quoted && *e == '\\' && e[1])
      e++;
    else if (quoted && *e == '"')
      in_quote = !in_quote;
  }
  *p = e;
  return e > s ? g_strndup (s, e - s) : NULL;
}

static char *
fingerprint_of (const guchar *blob, gsize len)
{
  guint8 digest[32];
  gsize dlen = sizeof digest;
  g_autoptr (GChecksum) sum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (sum, blob, len);
  g_checksum_get_digest (sum, digest, &dlen);
  g_autofree char *b64 = g_base64_encode (digest, dlen);
  char *end = b64 + strlen (b64);
  while (end > b64 && end[-1] == '=')
    *--end = '\0';
  return g_strconcat ("SHA256:", b64, NULL);
}

/* one line of authorized_keys or of a .pub file; NULL for blank lines, comments and anything that is not a key */
static SshKey *
ssh_key_parse (const char *line)
{
  const char *p = line;
  g_autoptr (SshKey) k = g_new0 (SshKey, 1);
  g_autofree char *first = NULL;
  g_autofree guchar *raw = NULL;
  gsize len = 0;

  while (*p == ' ' || *p == '\t')
    p++;
  if (!*p || *p == '#')
    return NULL;
  k->line = g_strchomp (g_strdup (p));
  first = next_token (&p, TRUE);
  if (!first)
    return NULL;
  if (is_key_type (first)) {
    k->type = g_steal_pointer (&first);
  } else {
    k->options = g_steal_pointer (&first);
    k->type = next_token (&p, FALSE);
  }
  k->blob = next_token (&p, FALSE);
  if (!k->type || !is_key_type (k->type) || !k->blob)
    return NULL;
  /* the blob begins with its own type, a string with a 32-bit big-endian length */
  raw = g_base64_decode (k->blob, &len);
  if (len < 4)
    return NULL;
  guint32 tl = (guint32) raw[0] << 24 | (guint32) raw[1] << 16 | (guint32) raw[2] << 8 | raw[3];
  if (tl != strlen (k->type) || tl > len - 4 || memcmp (raw + 4, k->type, tl) != 0)
    return NULL;
  k->fingerprint = fingerprint_of (raw, len);
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p)
    k->comment = g_strchomp (g_strdup (p));
  return g_steal_pointer (&k);
}

static char *
key_kind (const char *type)
{
  static const struct { const char *prefix, *name; } kinds[] = {
    { "ssh-ed25519", "Ed25519" }, { "ssh-rsa", "RSA" }, { "ecdsa-sha2-", "ECDSA" },
    { "sk-ssh-ed25519", "Ed25519 security key" }, { "sk-ecdsa-", "ECDSA security key" }, { "ssh-dss", "DSA" },
  };
  const char *name = type;

  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
    if (g_str_has_prefix (type, kinds[i].prefix)) {
      name = kinds[i].name;
      break;
    }
  }
  return g_strdup_printf ("%s%s", name, strstr (type, "-cert-") ? " certificate" : "");
}

/* the key alone, as a service wants it pasted: type, key, name */
static char *
key_public_text (SshKey *k)
{
  return g_strjoin (" ", k->type, k->blob, k->comment, NULL);
}

/* ---- files -------------------------------------------------------------------------------------------------- */

static char *
ssh_path (const char *name)
{
  return g_build_filename (g_get_home_dir (), ".ssh", name, NULL);
}

static gboolean
ensure_ssh_dir (GError **error)
{
  g_autofree char *dir = ssh_path (NULL);
  if (g_mkdir_with_parents (dir, 0700) == 0)
    return TRUE;
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: %s", dir, g_strerror (errno));
  return FALSE;
}

/* the file's lines, an empty list when there is no file, NULL when it cannot be read (then it is not written) */
static GPtrArray *
read_lines (const char *path, GError **error)
{
  g_autofree char *text = NULL;
  g_autoptr (GError) local = NULL;
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);

  if (!g_file_get_contents (path, &text, NULL, &local)) {
    if (g_error_matches (local, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      return lines;
    g_propagate_error (error, g_steal_pointer (&local));
    g_ptr_array_unref (lines);
    return NULL;
  }
  char **split = g_strsplit (text, "\n", -1);
  for (char **l = split; *l; l++)
    g_ptr_array_add (lines, *l);
  g_free (split);
  /* the newline at the end leaves an empty last piece */
  if (lines->len && !*(char *) g_ptr_array_index (lines, lines->len - 1))
    g_ptr_array_remove_index (lines, lines->len - 1);
  return lines;
}

static gboolean
write_authorized (GPtrArray *lines, GError **error)
{
  g_autofree char *path = ssh_path ("authorized_keys");
  g_autoptr (GString) text = g_string_new (NULL);

  if (!ensure_ssh_dir (error))
    return FALSE;
  for (guint i = 0; i < lines->len; i++) {
    g_string_append (text, g_ptr_array_index (lines, i));
    g_string_append_c (text, '\n');
  }
  return g_file_set_contents_full (path, text->str, text->len,
                                   G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

static gboolean
same_line (const char *a, const char *b)
{
  g_autofree char *x = g_strstrip (g_strdup (a)), *y = g_strstrip (g_strdup (b));
  return g_str_equal (x, y);
}

/* a file name for a key of ours: no paths, no dot files, none of ssh's own files */
static gboolean
valid_key_name (const char *name)
{
  static const char *const taken[] = { "authorized_keys", "authorized_keys2", "known_hosts", "known_hosts.old",
                                       "config", "environment", "rc", NULL };
  if (!name || !*name || *name == '.' || g_str_has_suffix (name, ".pub") || g_strv_contains (taken, name))
    return FALSE;
  for (const char *c = name; *c; c++) {
    if (!g_ascii_isalnum (*c) && *c != '_' && *c != '-' && *c != '.')
      return FALSE;
  }
  return TRUE;
}

static gboolean
key_name_free (const char *name)
{
  g_autofree char *priv = ssh_path (name);
  g_autofree char *pub = g_strconcat (priv, ".pub", NULL);
  return !g_file_test (priv, G_FILE_TEST_EXISTS) && !g_file_test (pub, G_FILE_TEST_EXISTS);
}

/* `base`, or base-2, base-3, ... whichever is not there yet */
static char *
free_key_name (const char *base)
{
  if (key_name_free (base))
    return g_strdup (base);
  for (int i = 2;; i++) {
    g_autofree char *name = g_strdup_printf ("%s-%d", base, i);
    if (key_name_free (name))
      return g_steal_pointer (&name);
  }
}

/* ---- ssh-keygen ---------------------------------------------------------------------------------------------- */

/* ssh-keygen asks SSH_ASKPASS for a passphrase when it has no terminal (SSH_ASKPASS_REQUIRE=force: always), and
 * the askpass program is this binary, which prints NS_SSH_PASSPHRASE: the passphrase stays out of every command
 * line (/proc/PID/environ is the user's own). main () calls this when NS_SSH_ASKPASS is set. */
void
ns_ssh_askpass (void)
{
  const char *pass = g_getenv ("NS_SSH_PASSPHRASE");
  printf ("%s\n", pass ? pass : "");
}

/* `passphrase` (NULL: ssh-keygen is not expected to ask) is what it gets if it asks */
static GSubprocess *
keygen (const char *passphrase, const char *const *args, GError **error)
{
  g_autoptr (GSubprocessLauncher) l =
    g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                               G_SUBPROCESS_FLAGS_STDERR_PIPE);
  if (passphrase) {
    g_autofree char *self = g_file_read_link ("/proc/self/exe", error);
    if (!self)
      return NULL;
    g_subprocess_launcher_setenv (l, "SSH_ASKPASS", self, TRUE);
    g_subprocess_launcher_setenv (l, "SSH_ASKPASS_REQUIRE", "force", TRUE);
    g_subprocess_launcher_setenv (l, "NS_SSH_ASKPASS", "1", TRUE);
    g_subprocess_launcher_setenv (l, "NS_SSH_PASSPHRASE", passphrase, TRUE);
  }
  return g_subprocess_launcher_spawnv (l, args, error);
}

/* ---- small things ------------------------------------------------------------------------------------------- */

static AdwNavigationPage *ssh_page;  /* the SSH page while it is open, to be filled again after a change */

static void
refresh (void)
{
  if (ssh_page)
    ns_later (ssh_page);
}

static void
say (GtkWidget *parent, const char *heading, const char *body)
{
  if (!gtk_widget_get_root (parent))
    return;
  AdwDialog *d = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "ok", "OK");
  adw_dialog_present (d, parent);
}

/* back from a key's page to the list */
static void
back (GtkWidget *w)
{
  if (gtk_widget_get_root (w))
    gtk_widget_activate_action (w, "navigation.pop", NULL);
}

static void
copy_tapped (GtkWidget *row, gpointer data)
{
  g_autofree char *text = key_public_text (data);
  gdk_clipboard_set_text (gtk_widget_get_clipboard (row), text);
  ns_row_set_subtitle (row, "Copied");
}

/* ---- allowed to log in: removing ----------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *row;
  char      *line;
} Removal;

static void
removal_free (Removal *r)
{
  g_object_unref (r->row);
  g_free (r->line);
  g_free (r);
}

static void
remove_verified (gboolean ok, gpointer data)
{
  Removal *r = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = ssh_path ("authorized_keys");
  g_autoptr (GPtrArray) lines = NULL;
  gboolean found = FALSE;

  if (!ok) {
    removal_free (r);
    return;
  }
  /* read again: the line goes by its text, whatever else changed in the file meanwhile */
  lines = read_lines (path, &error);
  for (guint i = 0; lines && i < lines->len && !found; i++) {
    if (same_line (g_ptr_array_index (lines, i), r->line)) {
      g_ptr_array_remove_index (lines, i);
      found = TRUE;
    }
  }
  if (found && write_authorized (lines, &error)) {
    refresh ();
    back (r->row);
  } else {
    say (r->row, "The key was not removed", error ? error->message : "It is no longer in the file.");
  }
  removal_free (r);
}

static void
remove_tapped (GtkWidget *row, gpointer data)
{
  SshKey *k = data;
  Removal *r = g_new0 (Removal, 1);
  r->row = g_object_ref (row);
  r->line = g_strdup (k->line);
  g_autofree char *reason = g_strdup_printf ("Remove “%s”? It will no longer log in to this phone.",
                                             k->comment ? k->comment : k->fingerprint);
  ns_pattern_verify_present (row, "Remove key", reason, remove_verified, r);
}

static void
allowed_tapped (GtkWidget *row, gpointer data)
{
  SshKey *listed = g_object_get_data (G_OBJECT (row), "ns-key");
  SshKey *k = ssh_key_parse (listed->line);  /* the page's own: the list's rows go when it is filled again */
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (k->comment ? k->comment : "Key", &c);
  g_object_set_data_full (G_OBJECT (page), "ns-key", k, (GDestroyNotify) ssh_key_free);
  g_autofree char *kind = key_kind (k->type);

  GtkWidget *g = ns_group (c, NULL);
  ns_row_info (g, "Name", k->comment ? k->comment : "None");
  ns_row_info (g, "Type", kind);
  ns_row_info (g, "Fingerprint", k->fingerprint);
  if (k->options)
    ns_row_info (g, "Restrictions", k->options);
  ns_note (c, "To be sure it is the right key, compare the fingerprint with what “ssh-keygen -lf” shows for it on "
              "the device it belongs to.");
  g = ns_group (c, NULL);
  ns_row_button (g, "Copy key", FALSE, copy_tapped, k);
  ns_row_button (g, "Remove", TRUE, remove_tapped, k);
  ns_push (page);
}

/* ---- allowed to log in: adding ------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *row;
  GPtrArray *keys;  /* SshKey: the new ones */
} Adding;

static void
adding_free (Adding *a)
{
  g_object_unref (a->row);
  g_ptr_array_unref (a->keys);
  g_free (a);
}

static void
add_verified (gboolean ok, gpointer data)
{
  Adding *a = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = ssh_path ("authorized_keys");
  g_autoptr (GPtrArray) lines = NULL;

  if (ok) {
    lines = read_lines (path, &error);
    for (guint i = 0; lines && i < a->keys->len; i++)
      g_ptr_array_add (lines, g_strdup (((SshKey *) g_ptr_array_index (a->keys, i))->line));
    if (lines && write_authorized (lines, &error))
      refresh ();
    else
      say (a->row, "The key was not added", error->message);
  }
  adding_free (a);
}

static void
add_checked (GObject *source, GAsyncResult *res, gpointer data)
{
  Adding *a = data;
  g_autofree char *out = NULL;
  guint n = 0;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL, NULL) &&
                g_subprocess_get_successful (G_SUBPROCESS (source));

  /* ssh-keygen -l prints a line for each key it could read */
  for (const char *p = out; ok && p && *p; p++)
    n += *p == '\n';
  if (!ok || n != a->keys->len) {
    say (a->row, "Not a valid key", "ssh-keygen could not read it. Check that the whole key was copied.");
    adding_free (a);
    return;
  }
  g_autoptr (GString) names = g_string_new (NULL);
  for (guint i = 0; i < a->keys->len; i++) {
    SshKey *k = g_ptr_array_index (a->keys, i);
    g_string_append_printf (names, "%s“%s”", i ? ", " : "", k->comment ? k->comment : k->fingerprint);
  }
  g_autofree char *reason = g_strdup_printf ("Let %s log in to this phone as you, without a password?", names->str);
  ns_pattern_verify_present (a->row, a->keys->len == 1 ? "Add key" : "Add keys", reason, add_verified, a);
}

/* public keys in `text` (pasted, or a file's), one per line */
static void
add_text (GtkWidget *row, const char *text)
{
  g_autofree char *path = ssh_path ("authorized_keys");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) lines = NULL;
  g_autoptr (GHashTable) have = NULL;
  g_auto (GStrv) split = NULL;
  g_autoptr (GString) check = g_string_new (NULL);
  guint already = 0;

  if (strstr (text, "PRIVATE KEY")) {
    say (row, "That is a private key",
         "Add the public key instead: the .pub file, or the line that starts with ssh-ed25519, ssh-rsa or "
         "ecdsa-sha2. A private key should never leave the device it was made on.");
    return;
  }
  lines = read_lines (path, &error);
  if (!lines) {
    say (row, "The allowed keys could not be read", error->message);
    return;
  }
  have = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < lines->len; i++) {
    g_autoptr (SshKey) k = ssh_key_parse (g_ptr_array_index (lines, i));
    if (k)
      g_hash_table_add (have, g_strdup (k->fingerprint));
  }
  Adding *a = g_new0 (Adding, 1);
  a->row = g_object_ref (row);
  a->keys = g_ptr_array_new_with_free_func ((GDestroyNotify) ssh_key_free);
  split = g_strsplit (text, "\n", -1);
  for (char **l = split; *l; l++) {
    SshKey *k = ssh_key_parse (*l);
    if (!k)
      continue;
    if (g_hash_table_contains (have, k->fingerprint)) {
      already++;
      ssh_key_free (k);
      continue;
    }
    g_hash_table_add (have, g_strdup (k->fingerprint));
    g_string_append_printf (check, "%s\n", k->line);
    g_ptr_array_add (a->keys, k);
  }
  if (!a->keys->len) {
    if (already)
      say (row, "Already allowed", "That key can already log in to this phone.");
    else
      say (row, "No public key found",
           "Paste the whole line: the key type (ssh-ed25519, ssh-rsa, …), the key, and its name if it has one.");
    adding_free (a);
    return;
  }
  /* ssh-keygen has the last word on whether these are keys */
  const char *argv[] = { "ssh-keygen", "-l", "-f", "-", NULL };
  g_autoptr (GSubprocess) proc = keygen (NULL, argv, &error);
  if (!proc) {
    say (row, "ssh-keygen is missing", error->message);
    adding_free (a);
    return;
  }
  g_subprocess_communicate_utf8_async (proc, check->str, NULL, add_checked, a);
}

static void
pasted (const char *text, gpointer data)
{
  GtkWidget *row = data;
  if (text && *text)
    add_text (row, text);
  g_object_unref (row);
}

static void
add_paste_tapped (GtkWidget *row, gpointer data)
{
  ns_ask (row, "Add a key", "Paste the public key of the device that should log in, for example the contents of "
                            "its ~/.ssh/id_ed25519.pub.", NULL, FALSE, "Add", pasted, g_object_ref (row));
}

static void
add_file_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
  GtkWidget *row = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;

  if (file && g_file_load_contents (file, NULL, &text, NULL, NULL, &error))
    add_text (row, text);
  else if (file)
    say (row, "The file could not be read", error->message);
  g_object_unref (row);
}

static void
add_file_tapped (GtkWidget *row, gpointer data)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (d, "Public key file");
  gtk_file_dialog_open (d, GTK_WINDOW (gtk_widget_get_root (row)), NULL, add_file_chosen, g_object_ref (row));
}

/* ---- my keys: sharing ---------------------------------------------------------------------------------------- */

typedef struct {
  SshKey *key;   /* the .pub */
  char   *name;  /* the private key's file name in ~/.ssh */
} Pair;

static void
pair_free (Pair *p)
{
  ssh_key_free (p->key);
  g_free (p->name);
  g_free (p);
}

/* libqrencode's QRcode: one byte per module, bit 0 dark */
typedef struct {
  int            version;
  int            width;
  unsigned char *data;
} QrCode;

static void (*qr_free) (QrCode *);

static QrCode *
qr_encode (const char *text)
{
  static GModule *lib;
  static QrCode *(*encode) (const char *, int, int, int, int);

  if (!lib) {
    lib = g_module_open ("libqrencode.so.4", G_MODULE_BIND_LAZY);
    if (!lib || !g_module_symbol (lib, "QRcode_encodeString", (gpointer *) &encode) ||
        !g_module_symbol (lib, "QRcode_free", (gpointer *) &qr_free))
      encode = NULL;
  }
  /* version 0: as small as fits; level M (1); 8-bit mode (QR_MODE_8, 2); case kept */
  return encode ? encode (text, 0, 1, 2, 1) : NULL;
}

static void
qr_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  QrCode *q = data;
  int modules = q->width + 8;  /* a quiet zone of four modules around it */
  double cell = floor (MIN (width, height) / (double) modules);
  if (cell < 1)
    cell = MIN (width, height) / (double) modules;
  double size = cell * modules, x0 = floor ((width - size) / 2), y0 = floor ((height - size) / 2);

  /* dark on light whatever the theme: scanners want it so */
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, x0, y0, size, size);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0, 0, 0);
  for (int y = 0; y < q->width; y++) {
    for (int x = 0; x < q->width; x++) {
      if (q->data[y * q->width + x] & 1)
        cairo_rectangle (cr, x0 + (x + 4) * cell, y0 + (y + 4) * cell, cell, cell);
    }
  }
  cairo_fill (cr);
}

static void
qr_tapped (GtkWidget *row, gpointer data)
{
  Pair *p = data;
  g_autofree char *text = key_public_text (p->key);
  QrCode *q = qr_encode (text);

  if (!q) {
    say (row, "No QR code", "libqrencode is not installed, or the key is too long for a QR code.");
    return;
  }
  AdwDialog *d = adw_dialog_new ();
  adw_dialog_set_title (d, p->name);
  adw_dialog_set_content_width (d, 360);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_bottom (box, 24);
  GtkWidget *area = gtk_drawing_area_new ();
  gtk_widget_set_size_request (area, 320, 320);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), qr_draw, q, (GDestroyNotify) qr_free);
  gtk_box_append (GTK_BOX (box), area);
  GtkWidget *label = gtk_label_new ("The public key, to scan on the other device. It lets this phone log in "
                                    "where it is added; it reveals nothing secret.");
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (box), label);
  GtkWidget *fp = gtk_label_new (p->key->fingerprint);
  gtk_label_set_wrap (GTK_LABEL (fp), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (fp), PANGO_WRAP_CHAR);
  gtk_widget_add_css_class (fp, "monospace");
  gtk_widget_add_css_class (fp, "caption");
  gtk_box_append (GTK_BOX (box), fp);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_dialog_set_child (d, toolbar);
  adw_dialog_present (d, row);
}

typedef struct {
  GtkWidget *row;
  char      *text;
} Saving;

static void
saved (GObject *source, GAsyncResult *res, gpointer data)
{
  Saving *s = data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res, NULL);
  g_autoptr (GError) error = NULL;

  if (file && !g_file_replace_contents (file, s->text, strlen (s->text), NULL, FALSE, G_FILE_CREATE_NONE, NULL,
                                        NULL, &error))
    say (s->row, "The key was not saved", error->message);
  else if (file)
    ns_row_set_subtitle (s->row, "Saved");
  g_object_unref (s->row);
  g_free (s->text);
  g_free (s);
}

static void
save_tapped (GtkWidget *row, gpointer data)
{
  Pair *p = data;
  Saving *s = g_new0 (Saving, 1);
  g_autofree char *key = key_public_text (p->key);
  g_autofree char *initial = g_strconcat (p->name, ".pub", NULL);
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();

  s->row = g_object_ref (row);
  s->text = g_strconcat (key, "\n", NULL);
  gtk_file_dialog_set_title (d, "Save the public key");
  gtk_file_dialog_set_initial_name (d, initial);
  gtk_file_dialog_save (d, GTK_WINDOW (gtk_widget_get_root (row)), NULL, saved, s);
}

/* ---- my keys: deleting --------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *row;
  char      *name;
} Deletion;

static void
delete_verified (gboolean ok, gpointer data)
{
  Deletion *d = data;

  if (ok) {
    g_autofree char *priv = ssh_path (d->name);
    g_autofree char *pub = g_strconcat (priv, ".pub", NULL);
    /* out of the agent too (gcr's, once it loaded the key); it needs the .pub, so before that goes */
    const char *argv[] = { "ssh-add", "-d", pub, NULL };
    g_spawn_sync (NULL, (char **) argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                  NULL, NULL, NULL);
    if (g_unlink (priv) < 0 && errno != ENOENT) {
      g_autofree char *body = g_strdup_printf ("%s: %s", priv, g_strerror (errno));
      say (d->row, "The key was not deleted", body);
    } else {
      g_unlink (pub);
      refresh ();
      back (d->row);
    }
  }
  g_object_unref (d->row);
  g_free (d->name);
  g_free (d);
}

static void
delete_tapped (GtkWidget *row, gpointer data)
{
  Pair *p = data;
  Deletion *d = g_new0 (Deletion, 1);
  d->row = g_object_ref (row);
  d->name = g_strdup (p->name);
  g_autofree char *reason = g_strdup_printf ("Delete the key “%s”? This phone can then no longer log in where its "
                                             "public key was added. This cannot be undone.", p->name);
  ns_pattern_verify_present (row, "Delete key", reason, delete_verified, d);
}

static void
mine_tapped (GtkWidget *row, gpointer data)
{
  Pair *listed = g_object_get_data (G_OBJECT (row), "ns-pair");
  Pair *p = g_new0 (Pair, 1);
  p->key = ssh_key_parse (listed->key->line);
  p->name = g_strdup (listed->name);
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (p->name, &c);
  g_object_set_data_full (G_OBJECT (page), "ns-pair", p, (GDestroyNotify) pair_free);
  g_autofree char *kind = key_kind (p->key->type);
  g_autofree char *file = ssh_path (p->name);

  GtkWidget *g = ns_group (c, NULL);
  ns_row_info (g, "Type", kind);
  ns_row_info (g, "Name in the key", p->key->comment ? p->key->comment : "None");
  ns_row_info (g, "Fingerprint", p->key->fingerprint);
  ns_row_info (g, "File", file);
  g = ns_group (c, "Share the public key");
  ns_row_button (g, "Copy", FALSE, copy_tapped, p->key);
  ns_row_button (g, "Show QR code", FALSE, qr_tapped, p);
  ns_row_button (g, "Save to a file", FALSE, save_tapped, p);
  ns_note (c, "Add the public key to a service or another device and this phone can log in there. The private key "
              "stays on the phone.");
  g = ns_group (c, NULL);
  ns_row_button (g, "Delete key", TRUE, delete_tapped, p);
  ns_push (page);
}

/* ---- my keys: a new one -------------------------------------------------------------------------------------- */

static const char *const new_types[] = { "Ed25519 (recommended)", "RSA, 4096 bits", NULL };
static const char *const new_type_args[] = { "ed25519", "rsa" };
static const char *const new_type_names[] = { "id_ed25519", "id_rsa" };

typedef struct {
  GtkWidget *name, *comment, *pass, *create;
  guint      type;
  char      *passphrase;  /* the confirmed one, or NULL */
} NewKey;

static void
new_key_free (NewKey *nk)
{
  if (nk->passphrase)
    memset (nk->passphrase, 0, strlen (nk->passphrase));
  g_free (nk->passphrase);
  g_free (nk);
}

static void
type_picked (GtkWidget *row, guint index, gpointer data)
{
  NewKey *nk = data;
  const char *name = ns_row_value_get (nk->name);
  /* the file name follows the type while it is still a default one */
  if (g_str_has_prefix (name, new_type_names[nk->type]) && index != nk->type) {
    g_autofree char *other = free_key_name (new_type_names[index]);
    ns_row_value_set (nk->name, other);
  }
  nk->type = index;
}

static void
pass_confirmed (const char *text, gpointer data)
{
  NewKey *nk = data;
  const char *first = ns_row_value_get (nk->pass);

  g_clear_pointer (&nk->passphrase, g_free);
  if (text && g_str_equal (text, first)) {
    nk->passphrase = g_strdup (text);
    return;
  }
  ns_row_value_set (nk->pass, NULL);
  if (text)
    say (nk->pass, "The passphrases are different", "The key gets no passphrase until you set one again.");
}

static void
pass_entered (const char *text, gpointer data)
{
  NewKey *nk = data;
  g_clear_pointer (&nk->passphrase, g_free);
  if (!text || !*text) {
    ns_row_value_set (nk->pass, NULL);
    return;
  }
  ns_ask (nk->pass, "Passphrase again", "Type the passphrase again to confirm it.", NULL, TRUE, "OK",
          pass_confirmed, nk);
}

static void
created (GObject *source, GAsyncResult *res, gpointer data)
{
  GtkWidget *row = data;
  g_autofree char *err = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &err, NULL) &&
                g_subprocess_get_successful (G_SUBPROCESS (source));

  gtk_widget_set_sensitive (row, TRUE);
  ns_row_set_subtitle (row, NULL);
  if (ok) {
    refresh ();
    back (row);
  } else {
    say (row, "The key was not made", err && *err ? g_strstrip (err) : "ssh-keygen failed.");
  }
  g_object_unref (row);
}

static void
create_go (GtkWidget *row, gpointer data)
{
  NewKey *nk = data;
  const char *name = ns_row_value_get (nk->name);
  const char *comment = ns_row_value_get (nk->comment);
  g_autoptr (GError) error = NULL;

  if (!valid_key_name (name)) {
    say (row, "Choose another file name", "Use letters, digits, “-”, “_” and “.”, not starting with a dot and not "
                                          "ending in .pub.");
    return;
  }
  if (!key_name_free (name)) {
    say (row, "That name is taken", "There is already a key with that file name.");
    return;
  }
  if (!ensure_ssh_dir (&error)) {
    say (row, "The key was not made", error->message);
    return;
  }
  g_autofree char *path = ssh_path (name);
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "ssh-keygen");
  g_ptr_array_add (argv, "-q");
  g_ptr_array_add (argv, "-t");
  g_ptr_array_add (argv, (char *) new_type_args[nk->type]);
  if (nk->type == 1) {
    g_ptr_array_add (argv, "-b");
    g_ptr_array_add (argv, "4096");
  }
  g_ptr_array_add (argv, "-C");
  g_ptr_array_add (argv, (char *) comment);
  g_ptr_array_add (argv, "-f");
  g_ptr_array_add (argv, path);
  /* no passphrase: -N "" on the command line; one: asked for (twice) through SSH_ASKPASS */
  if (!nk->passphrase) {
    g_ptr_array_add (argv, "-N");
    g_ptr_array_add (argv, "");
  }
  g_ptr_array_add (argv, NULL);
  g_autoptr (GSubprocess) proc = keygen (nk->passphrase ? nk->passphrase : "",
                                         (const char *const *) argv->pdata, &error);
  if (!proc) {
    say (row, "The key was not made", error->message);
    return;
  }
  gtk_widget_set_sensitive (row, FALSE);
  ns_row_set_subtitle (row, nk->type == 1 ? "Making the key, this takes a while…" : "Making the key…");
  g_subprocess_communicate_utf8_async (proc, NULL, NULL, created, g_object_ref (row));
}

static void
create_tapped (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("New key", &c);
  NewKey *nk = g_new0 (NewKey, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-new-key", nk, (GDestroyNotify) new_key_free);
  g_autofree char *name = free_key_name (new_type_names[0]);
  g_autofree char *comment = g_strdup_printf ("%s@%s", g_get_user_name (), g_get_host_name ());

  GtkWidget *g = ns_group (c, NULL);
  ns_row_pick (g, "Type", new_types, 0, type_picked, nk);
  nk->name = ns_row_value (g, "File name", name, FALSE, NULL, NULL);
  nk->comment = ns_row_value (g, "Name in the key", comment, FALSE, NULL, NULL);
  nk->pass = ns_row_value (g, "Passphrase", NULL, TRUE, pass_entered, nk);
  ns_note (c, "The name in the key is how other devices list it. A passphrase protects the key should the file "
              "ever be copied off the phone; without one, the key works without typing anything.");
  g = ns_group (c, NULL);
  nk->create = ns_row_button (g, "Create key", FALSE, create_go, nk);
  ns_push (page);
}

/* ---- my keys: importing -------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *row;
  char      *src;
  gboolean   asked;  /* a passphrase was typed (and was wrong, if it comes to asking again) */
} Import;

static void
import_free (Import *im)
{
  g_object_unref (im->row);
  g_free (im->src);
  g_free (im);
}

static void import_try (Import *im, const char *passphrase);

static void
import_pass (const char *text, gpointer data)
{
  Import *im = data;
  if (!text) {
    import_free (im);
    return;
  }
  im->asked = TRUE;
  import_try (im, text);
}

/* the private key is readable: it and its public half go into ~/.ssh */
static void
import_install (Import *im, const char *public_line)
{
  g_autoptr (SshKey) k = ssh_key_parse (public_line);
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize len = 0;
  g_autofree char *base = g_path_get_basename (im->src);

  if (!k) {
    say (im->row, "The key was not imported", "ssh-keygen gave no public key for it.");
    return;
  }
  /* the same key already here */
  g_autoptr (GDir) dir = NULL;
  g_autofree char *dirpath = ssh_path (NULL);
  dir = g_dir_open (dirpath, 0, NULL);
  for (const char *n; dir && (n = g_dir_read_name (dir));) {
    if (!g_str_has_suffix (n, ".pub"))
      continue;
    g_autofree char *pub = ssh_path (n);
    g_autofree char *text = NULL;
    if (!g_file_get_contents (pub, &text, NULL, NULL))
      continue;
    g_autoptr (SshKey) other = ssh_key_parse (text);
    if (other && g_str_equal (other->fingerprint, k->fingerprint)) {
      g_autofree char *body = g_strdup_printf ("It is already here, as “%.*s”.", (int) strlen (n) - 4, n);
      say (im->row, "Already imported", body);
      return;
    }
  }
  if (!ensure_ssh_dir (&error) || !g_file_get_contents (im->src, &contents, &len, &error)) {
    say (im->row, "The key was not imported", error->message);
    return;
  }
  g_autofree char *name = free_key_name (valid_key_name (base) ? base : "imported_key");
  g_autofree char *priv = ssh_path (name);
  g_autofree char *pub = g_strconcat (priv, ".pub", NULL);
  g_autofree char *pub_text = g_strconcat (k->line, "\n", NULL);
  gboolean ok = g_file_set_contents_full (priv, contents, len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error) &&
                g_file_set_contents_full (pub, pub_text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644, &error);
  memset (contents, 0, len);
  if (!ok)
    say (im->row, "The key was not imported", error->message);
  refresh ();
}

static void
import_tried (GObject *source, GAsyncResult *res, gpointer data)
{
  Import *im = data;
  g_autofree char *out = NULL, *err = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, &err, NULL) &&
                g_subprocess_get_successful (G_SUBPROCESS (source));

  if (ok) {
    import_install (im, out ? out : "");
  } else if (err && strstr (err, "passphrase")) {
    ns_ask (im->row, "Passphrase", im->asked ? "That passphrase is not the key's. Try again."
                                             : "This key is protected by a passphrase. Enter it to import the key.",
            NULL, TRUE, "Import", import_pass, im);
    return;
  } else {
    say (im->row, "Not a private key", "ssh-keygen could not read it. Pick the private key file (without .pub).");
  }
  import_free (im);
}

static void
import_try (Import *im, const char *passphrase)
{
  g_autoptr (GError) error = NULL;
  const char *argv[] = { "ssh-keygen", "-y", "-f", im->src, NULL };
  g_autoptr (GSubprocess) proc = keygen (passphrase, argv, &error);

  if (!proc) {
    say (im->row, "ssh-keygen is missing", error->message);
    import_free (im);
    return;
  }
  g_subprocess_communicate_utf8_async (proc, NULL, NULL, import_tried, im);
}

static void
import_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
  GtkWidget *row = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
  g_autofree char *path = file ? g_file_get_path (file) : NULL;

  if (!path) {
    g_object_unref (row);
    return;
  }
  Import *im = g_new0 (Import, 1);
  im->row = row;
  im->src = g_steal_pointer (&path);
  /* an empty passphrase first: an unprotected key needs none, a protected one says so */
  import_try (im, "");
}

static void
import_tapped (GtkWidget *row, gpointer data)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (d, "Private key file");
  gtk_file_dialog_open (d, GTK_WINDOW (gtk_widget_get_root (row)), NULL, import_chosen, g_object_ref (row));
}

/* ---- the page ----------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget    *server;
  GtkWidget    *allowed;
  GtkWidget    *mine;
  GFileMonitor *monitor;
} Ssh;

static void
ssh_free (Ssh *s)
{
  if (s->monitor)
    g_file_monitor_cancel (s->monitor);
  g_clear_object (&s->monitor);
  g_free (s);
}

static int
compare_names (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(char **) a, *(char **) b);
}

static void
ssh_fill (AdwNavigationPage *page)
{
  Ssh *s = g_object_get_data (G_OBJECT (page), "ns-ssh");
  const char *argv[] = { "systemctl", "is-active", "ssh", NULL };
  g_autofree char *state = ns_run_line (argv);
  g_autofree char *path = ssh_path ("authorized_keys");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) lines = read_lines (path, &error);
  guint n = 0;

  ns_row_set_subtitle (s->server, g_strcmp0 (state, "active") == 0 ? "On: devices with an allowed key can log in"
                                                                    : "Off: nothing can log in over SSH");
  ns_group_clear (s->allowed);
  for (guint i = 0; lines && i < lines->len; i++) {
    SshKey *k = ssh_key_parse (g_ptr_array_index (lines, i));
    if (!k)
      continue;
    g_autofree char *kind = key_kind (k->type);
    g_autofree char *sub = g_strdup_printf ("%s • %.19s…", kind, k->fingerprint);
    GtkWidget *r = ns_row (s->allowed, "channel-secure-symbolic", "green", k->comment ? k->comment : "Key without a name",
                           sub);
    g_object_set_data_full (G_OBJECT (r), "ns-key", k, (GDestroyNotify) ssh_key_free);
    ns_row_on_activate (r, allowed_tapped, NULL);
    n++;
  }
  if (!lines)
    ns_row (s->allowed, NULL, NULL, "The allowed keys could not be read", error->message);
  else if (!n)
    ns_row (s->allowed, NULL, NULL, "No keys yet", "Only your password logs in");
  ns_row_button (s->allowed, "Add a key", FALSE, add_paste_tapped, NULL);
  ns_row_button (s->allowed, "Add a key from a file", FALSE, add_file_tapped, NULL);

  ns_group_clear (s->mine);
  g_autofree char *dirpath = ssh_path (NULL);
  g_autoptr (GDir) dir = g_dir_open (dirpath, 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  for (const char *name; dir && (name = g_dir_read_name (dir));) {
    if (g_str_has_suffix (name, ".pub"))
      g_ptr_array_add (names, g_strndup (name, strlen (name) - 4));
  }
  g_ptr_array_sort (names, compare_names);
  for (guint i = 0; i < names->len; i++) {
    const char *name = g_ptr_array_index (names, i);
    g_autofree char *priv = ssh_path (name);
    g_autofree char *pub = g_strconcat (priv, ".pub", NULL);
    g_autofree char *text = NULL;
    if (!g_file_test (priv, G_FILE_TEST_IS_REGULAR) || !g_file_get_contents (pub, &text, NULL, NULL))
      continue;
    SshKey *k = ssh_key_parse (text);
    if (!k)
      continue;
    Pair *p = g_new0 (Pair, 1);
    p->key = k;
    p->name = g_strdup (name);
    g_autofree char *kind = key_kind (k->type);
    g_autofree char *sub = k->comment ? g_strdup_printf ("%s • %s", kind, k->comment) : g_strdup (kind);
    GtkWidget *r = ns_row (s->mine, "dialog-password-symbolic", "blue", name, sub);
    g_object_set_data_full (G_OBJECT (r), "ns-pair", p, (GDestroyNotify) pair_free);
    ns_row_on_activate (r, mine_tapped, NULL);
  }
  ns_row_button (s->mine, "Create a key", FALSE, create_tapped, NULL);
  ns_row_button (s->mine, "Import a key", FALSE, import_tapped, NULL);
}

void
ns_page_ssh (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("SSH", &c);
  Ssh *s = g_new0 (Ssh, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-ssh", s, (GDestroyNotify) ssh_free);
  ns_later_init (page, ssh_fill);
  ssh_page = page;
  g_object_add_weak_pointer (G_OBJECT (page), (gpointer *) &ssh_page);

  GtkWidget *g = ns_group (c, NULL);
  s->server = ns_row (g, "network-server-symbolic", "violet", "SSH server", NULL);
  s->allowed = ns_group (c, "Allowed to log in to this phone");
  ns_note (c, "Each of these keys logs in as you without a password. Adding or removing one asks for your PIN or a "
              "fingerprint.");
  s->mine = ns_group (c, "My keys");
  ns_note (c, "Keys this phone uses to log in elsewhere. Share a public key by copying it or as a QR code; the "
              "private key never leaves the phone.");

  /* what changes in ~/.ssh shows here, also when something else changed it */
  g_autofree char *dirpath = ssh_path (NULL);
  g_autoptr (GFile) dir = g_file_new_for_path (dirpath);
  s->monitor = g_file_monitor_directory (dir, G_FILE_MONITOR_NONE, NULL, NULL);
  if (s->monitor)
    g_signal_connect_object (s->monitor, "changed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  ssh_fill (page);
  ns_push (page);
}
