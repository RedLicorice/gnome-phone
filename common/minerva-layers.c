/* Vendored unchanged from Minerva's services/common/minerva-layers.c (Minerva docs/packaging.md, "Configuration"):
 * keep the two the same. */
/* The configuration layers (docs/packaging.md, "Configuration"); see minerva-layers.h. GLib only, so a Phosh plugin
 * compiles this file too. */
#include "minerva-layers.h"

#include <string.h>

#define LAYERS_ENV "MINERVA_LAYERS_CONF"
#define LAYERS_GROUP "layers"
#define XDG_CONFIG_VAR "${XDG_CONFIG_HOME}"
#define FALLBACK_SUFFIX "_fallback"

/* The layer keys of layers.conf, lowest first; the last one is the owner's. */
static const char *const layer_keys[] = { "shipped_dir", "device_dir", "system_dir", "user_dir" };
#define USER_LAYER (G_N_ELEMENTS (layer_keys) - 1)

/* configparser takes a line that starts with ";" as a comment as well as "#"; GKeyFile only "#": the ";" lines become
 * "#" lines before GKeyFile reads a file. */
static void
semicolon_comments (char *text)
{
  for (char *line = text; *line; line = strchr (line, '\n') ? strchr (line, '\n') + 1 : line + strlen (line)) {
    char *first = line + strspn (line, " \t");

    if (*first == ';')
      *first = '#';
  }
}

/* value with ~ and ${XDG_CONFIG_HOME} expanded as minerva_layers.py does; NULL when it names $XDG_CONFIG_HOME, the
 * variable is not set and there is no fallback. */
static char *
layers_expand (const char *value, const char *fallback)
{
  if (strstr (value, XDG_CONFIG_VAR)) {
    const char *xdg = g_getenv ("XDG_CONFIG_HOME");
    g_autoptr (GString) out = NULL;

    if (!xdg || !*xdg)
      return fallback ? layers_expand (fallback, NULL) : NULL;
    out = g_string_new (value);
    g_string_replace (out, XDG_CONFIG_VAR, xdg, 0);
    return layers_expand (out->str, NULL);
  }
  if (value[0] == '~' && (value[1] == '/' || !value[1]))
    return g_build_filename (g_get_home_dir (), value + 1, NULL);
  return g_strdup (value);
}

/* layers.conf as a key file, NULL (said once on stderr) when it cannot be read */
static GKeyFile *
layers_conf (void)
{
  const char *conf = g_getenv (LAYERS_ENV);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *text = NULL;
  g_autofree char *data = NULL;
  g_autoptr (GError) error = NULL;

  if (!conf || !*conf)
    conf = MINERVA_LAYERS_FILE;
  /* layers.conf is key = value lines without a section, as configparser reads it with one put in front */
  if (g_file_get_contents (conf, &text, NULL, &error)) {
    semicolon_comments (text);
    data = g_strconcat ("[" LAYERS_GROUP "]\n", text, NULL);
    if (g_key_file_load_from_data (kf, data, -1, G_KEY_FILE_NONE, &error))
      return g_steal_pointer (&kf);
  }
  g_printerr ("cannot read the configuration layers %s: %s\n", conf, error->message);
  return NULL;
}

/* name in the folder of layer i, NULL when layers.conf lacks it or it names an unset $XDG_CONFIG_HOME without fallback */
static char *
layer_path (GKeyFile *kf, guint i, const char *name)
{
  g_autofree char *fallback_key = g_strconcat (layer_keys[i], FALLBACK_SUFFIX, NULL);
  g_autofree char *value = g_key_file_get_value (kf, LAYERS_GROUP, layer_keys[i], NULL);
  g_autofree char *fallback = g_key_file_get_value (kf, LAYERS_GROUP, fallback_key, NULL);
  g_autofree char *dir = value ? layers_expand (g_strstrip (value), fallback ? g_strstrip (fallback) : NULL) : NULL;

  return dir ? g_build_filename (dir, name, NULL) : NULL;
}

GStrv
minerva_layers_paths (const char *name, gboolean with_user)
{
  g_autoptr (GKeyFile) kf = layers_conf ();
  g_autoptr (GStrvBuilder) out = g_strv_builder_new ();

  for (guint i = 0; kf && i < (with_user ? G_N_ELEMENTS (layer_keys) : USER_LAYER); i++) {
    char *path = layer_path (kf, i, name);

    if (path)
      g_strv_builder_take (out, path);
  }
  return g_strv_builder_end (out);
}

GStrv
minerva_layers_files (const char *name, gboolean with_user)
{
  g_auto (GStrv) paths = minerva_layers_paths (name, with_user);
  g_autoptr (GStrvBuilder) out = g_strv_builder_new ();

  for (char **p = paths; *p; p++)
    if (g_file_test (*p, G_FILE_TEST_EXISTS))
      g_strv_builder_add (out, *p);
  return g_strv_builder_end (out);
}

char *
minerva_layers_user_file (const char *name)
{
  g_autoptr (GKeyFile) kf = layers_conf ();

  return kf ? layer_path (kf, USER_LAYER, name) : NULL;
}

/* the keys of path over the ones already in into; FALSE when the file cannot be read */
static gboolean
layer_over (GKeyFile *into, const char *path)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) groups = NULL;

  if (!g_file_get_contents (path, &text, NULL, &error)) {
    g_printerr ("cannot read %s: %s\n", path, error->message);
    return FALSE;
  }
  semicolon_comments (text);
  if (!g_key_file_load_from_data (kf, text, -1, G_KEY_FILE_NONE, &error)) {
    g_printerr ("cannot read %s: %s\n", path, error->message);
    return FALSE;
  }
  groups = g_key_file_get_groups (kf, NULL);
  for (char **g = groups; *g; g++) {
    g_auto (GStrv) keys = g_key_file_get_keys (kf, *g, NULL, NULL);

    for (char **k = keys; k && *k; k++) {
      g_autofree char *value = g_key_file_get_value (kf, *g, *k, NULL);

      g_key_file_set_value (into, *g, *k, value);
    }
  }
  return TRUE;
}

GKeyFile *
minerva_layers_load (const char *const *files)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  gboolean any = FALSE;

  for (; files && *files; files++)
    any |= layer_over (kf, *files);
  return any ? g_steal_pointer (&kf) : NULL;
}
