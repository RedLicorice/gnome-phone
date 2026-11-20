/* The avatar's live preview at the top of Minerva > Avatar: the real face (Minerva's libminerva-face, the library the
 * Phosh widget draws with), animated, with pose buttons that drive only the preview. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/* conf: the "preview" object of the kiosk's SettingsDescribe (sizes, profile, zoom key, poses, engine options);
 * lang: the labels' language. NULL when conf is NULL or the face library is not installed. */
GtkWidget *ns_avatar_preview_new (JsonObject *conf, const char *lang);
