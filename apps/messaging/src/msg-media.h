/* What a provider's message carries, as a conversation shows it: the preview the message came with, then, once the
 * media is on the phone, the picture itself (a photo, a sticker) loaded at the size it is shown, a sticker that moves
 * and a GIF (a looping, silent video) playing only while they are on screen. */
#pragma once

#include <gtk/gtk.h>

#include "sms-store.h"

G_BEGIN_DECLS

/* the picture of @part fitted inside @box × @box px keeping its shape; NULL when it has nothing to show */
GtkWidget *msg_media_new (const SmsPart *part, guint box);

/* whether the media of @part is on the phone */
gboolean   msg_media_is_here (const SmsPart *part);

G_END_DECLS
