/* The avatar's live preview at the top of Minerva > Avatar: the real face (Minerva's libminerva-face, the library the
 * Phosh widget draws with), animated, with pose buttons that drive only the preview. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/* conf: the "preview" object of the kiosk's SettingsDescribe (sizes, profile, zoom key, poses, engine options);
 * lang: the labels' language. NULL when conf is NULL or the face library is not installed. */
GtkWidget *ns_avatar_preview_new (JsonObject *conf, const char *lang);

/* The same face without the pose strip, for a host that drives it (the voice enrollment dialog) */
GtkWidget *ns_avatar_preview_new_bare (JsonObject *conf);

/* The face's pose (a state or an expression) and effect (an effect set; NULL or "": none), kept over a new Skin */
void ns_avatar_preview_set_pose (GtkWidget *preview, const char *pose, const char *effect);

/* A level 0..1 the face's wave follows (the loop's level(), valid for its effects.level_timeout_ms) */
void ns_avatar_preview_level (GtkWidget *preview, double level);
