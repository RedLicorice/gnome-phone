/* Vendored unchanged from Minerva's services/common/minerva-layers.h (Minerva docs/packaging.md, "Configuration"):
 * keep the two the same. */
/* The configuration layers (docs/packaging.md, "Configuration"): every configuration file a package ships is read from
 * the folders layers.conf names ($MINERVA_LAYERS_CONF, else MINERVA_LAYERS_FILE, the build's layers-file option:
 * minerva-base's /usr/lib/minerva/layers.conf), lowest first: shipped, device, system, owner. Each layer holds only the
 * keys it sets; a reader lays each one's keys over the ones below, a writer writes only the owner's file. GLib only. */
#pragma once

#include <glib.h>

/* The paths of name in every layer folder, lowest first, existing or not (what to monitor); with_user FALSE leaves out
 * the owner's layer (a system daemon has no owner's home). Newly allocated, empty without layers.conf. */
GStrv minerva_layers_paths (const char *name, gboolean with_user);

/* The same, only the files that exist (what to read). */
GStrv minerva_layers_files (const char *name, gboolean with_user);

/* name in the owner's layer folder (the one file a writer writes), NULL when layers.conf cannot be read. */
char *minerva_layers_user_file (const char *name);

/* files read in order, each one's keys over the ones before (";" comment lines taken as configparser does). NULL when
 * none can be read. */
GKeyFile *minerva_layers_load (const char *const *files);
