/* A provider's media in a conversation (msg-media.h).
 *
 * Nothing costs while it is not seen: the picture is loaded (glycin: in its sandboxed loaders) when its row is first
 * mapped, at the pixels it is shown with; a sticker that moves steps its frames, and a GIF's pipeline plays, only while the
 * picture is inside its scrolled window's view. Off screen there is no timer and the pipeline is paused; unmapped,
 * the pipeline is let go. */
#include "msg-media.h"

#include <glycin-gtk4.h>
#include <gst/gst.h>

typedef struct
{
  GtkWidget *picture;           /* not a reference: the data is the picture's own */
  char *kind;
  char *path;
  int width, height;            /* the size shown, in px */
  gboolean loaded, playing, moving;
  GCancellable *cancel;
  GlyImage *image;              /* a sticker that moves: its frames come one after the other */
  gboolean asking;              /* a frame is on its way */
  guint frame;
  GstElement *pipeline;
  guint bus_watch;
  GtkAdjustment *adjustment;
  gulong scrolled;
  guint check;
} Media;

static gboolean
is_kind (const SmsPart *part, const char *kind)
{
  return g_strcmp0 (part->kind, kind) == 0;
}

gboolean
msg_media_is_here (const SmsPart *part)
{
  return part && part->uri && g_file_test (part->uri, G_FILE_TEST_IS_REGULAR);
}

/* ---- playing only while seen ---------------------------------------------------------------------------------- */

static void ask_frame (Media *media);

/* an async call's data: the picture, weakly; its media when the picture is still there (taken: the ref goes) */
static gpointer
weak_picture (Media *media)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, media->picture);
  return ref;
}

static Media *
media_of (gpointer data)
{
  GWeakRef *ref = data;
  g_autoptr(GtkWidget) picture = g_weak_ref_get (ref);

  g_weak_ref_clear (ref);
  g_free (ref);
  return picture ? g_object_get_data (G_OBJECT (picture), "msg-media") : NULL;
}

static gboolean
on_frame_due (gpointer data)
{
  Media *media = data;

  media->frame = 0;
  ask_frame (media);
  return G_SOURCE_REMOVE;
}

static void
on_frame (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GlyFrame) frame = gly_image_next_frame_finish (GLY_IMAGE (source), result, &error);
  Media *media = media_of (data);
  g_autoptr(GdkTexture) texture = NULL;
  gint64 delay;

  if (!media)
    return;
  if (!frame)
    {
      g_debug ("a sticker's frame not shown: %s", error->message);
      media->asking = FALSE;
      return;
    }
  media->asking = FALSE;
  texture = gly_gtk_frame_get_texture (frame);
  gtk_picture_set_paintable (GTK_PICTURE (media->picture), GDK_PAINTABLE (texture));
  /* the next frame when this one has been shown long enough, while it is seen (µs) */
  delay = gly_frame_get_delay (frame);
  if (delay > 0 && media->playing)
    media->frame = g_timeout_add (MAX (1, delay / 1000), on_frame_due, media);
}

static void
ask_frame (Media *media)
{
  if (media->asking || !media->image)
    return;
  media->asking = TRUE;
  gly_image_next_frame_async (media->image, media->cancel, on_frame, weak_picture (media));
}

static void
play (Media *media, gboolean on)
{
  if (on == media->playing)
    return;
  media->playing = on;
  if (media->image)
    {
      g_clear_handle_id (&media->frame, g_source_remove);
      if (on)
        ask_frame (media);
    }
  if (media->pipeline)
    gst_element_set_state (media->pipeline, on ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

/* whether the picture is inside its scrolled window's view, now */
static gboolean
is_seen (Media *media)
{
  GtkWidget *scrolled = gtk_widget_get_ancestor (media->picture, GTK_TYPE_SCROLLED_WINDOW);
  graphene_rect_t bounds;

  if (!gtk_widget_get_mapped (media->picture))
    return FALSE;
  if (!scrolled)
    return TRUE;
  if (!gtk_widget_compute_bounds (media->picture, scrolled, &bounds))
    return FALSE;
  return bounds.origin.y + bounds.size.height > 0 && bounds.origin.y < gtk_widget_get_height (scrolled);
}

static gboolean
check_seen (gpointer data)
{
  Media *media = data;

  media->check = 0;
  play (media, (media->image || media->pipeline) && is_seen (media));
  return G_SOURCE_REMOVE;
}

/* after a scroll, or once the row is laid out: on the next idle, not on every pixel of a scroll */
static void
schedule_check (Media *media)
{
  if (!media->check)
    media->check = g_idle_add (check_seen, media);
}

/* ---- loading -------------------------------------------------------------------------------------------------- */

/* a photo or a still sticker: one frame, at the pixels it is shown with */
static void
on_still (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GlyFrame) frame = gly_image_get_specific_frame_finish (GLY_IMAGE (source), result, &error);
  g_autoptr(GdkTexture) texture = NULL;
  Media *media = media_of (data);

  if (!media)
    return;
  if (!frame)
    {
      g_debug ("a message's media not shown: %s", error->message);
      return;
    }
  texture = gly_gtk_frame_get_texture (frame);
  gtk_picture_set_paintable (GTK_PICTURE (media->picture), GDK_PAINTABLE (texture));
}

static void
on_image (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GlyImage) image = gly_loader_load_finish (GLY_LOADER (source), result, &error);
  g_autoptr(GlyFrameRequest) request = NULL;
  Media *media = media_of (data);

  if (!media)
    return;
  if (!image)
    {
      g_debug ("a message's media not shown: %s", error->message);
      return;
    }
  if (media->moving)
    {
      media->image = g_steal_pointer (&image);
      schedule_check (media);
      return;
    }
  request = gly_frame_request_new ();
  gly_frame_request_set_scale (request, media->width, media->height);
  gly_image_get_specific_frame_async (image, request, media->cancel, on_still, weak_picture (media));
}

static gboolean
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  Media *media = data;

  /* a GIF loops */
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS)
    gst_element_seek_simple (media->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
  else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    {
      g_autoptr(GError) error = NULL;

      gst_message_parse_error (message, &error, NULL);
      g_debug ("a GIF not played: %s", error->message);
    }
  return G_SOURCE_CONTINUE;
}

/* a GIF is a silent video that loops, drawn by GStreamer into the picture */
static void
make_pipeline (Media *media)
{
  g_autofree char *uri = g_filename_to_uri (media->path, NULL, NULL);
  GstElement *sink;
  g_autoptr(GdkPaintable) paintable = NULL;
  g_autoptr(GstBus) bus = NULL;

  if (!gst_is_initialized ())
    gst_init (NULL, NULL);
  media->pipeline = gst_element_factory_make ("playbin", NULL);
  sink = gst_element_factory_make ("gtk4paintablesink", NULL);
  if (!media->pipeline || !sink || !uri)
    {
      g_debug ("no playbin or gtk4paintablesink: a GIF shows its preview");
      g_clear_object (&sink);
      g_clear_pointer (&media->pipeline, gst_object_unref);
      return;
    }
  g_object_get (sink, "paintable", &paintable, NULL);
  g_object_set (media->pipeline, "uri", uri, "video-sink", sink, NULL);
  gst_util_set_object_arg (G_OBJECT (media->pipeline), "flags", "video");
  bus = gst_element_get_bus (media->pipeline);
  media->bus_watch = gst_bus_add_watch (bus, on_bus, media);
  gtk_picture_set_paintable (GTK_PICTURE (media->picture), paintable);
  gst_element_set_state (media->pipeline, GST_STATE_PAUSED);
}

static void
on_scrolled (GtkAdjustment *adjustment, gpointer data)
{
  schedule_check (data);
}

static void
on_map (GtkWidget *picture, gpointer data)
{
  Media *media = data;
  GtkWidget *scrolled = gtk_widget_get_ancestor (picture, GTK_TYPE_SCROLLED_WINDOW);

  if (scrolled && !media->adjustment)
    {
      media->adjustment = g_object_ref (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scrolled)));
      media->scrolled = g_signal_connect (media->adjustment, "value-changed", G_CALLBACK (on_scrolled), media);
    }
  if (!media->loaded)
    {
      media->loaded = TRUE;
      if (g_str_equal (media->kind, "gif"))
        make_pipeline (media);
      else
        {
          g_autoptr(GFile) file = g_file_new_for_path (media->path);
          g_autoptr(GlyLoader) loader = gly_loader_new (file);
          int scale = gtk_widget_get_scale_factor (picture);

          media->width *= scale;
          media->height *= scale;
          gly_loader_load_async (loader, media->cancel, on_image, weak_picture (media));
        }
    }
  else if (g_str_equal (media->kind, "gif") && !media->pipeline)
    make_pipeline (media);
  schedule_check (media);
}

static void
on_unmap (GtkWidget *picture, gpointer data)
{
  Media *media = data;

  play (media, FALSE);
  g_clear_handle_id (&media->check, g_source_remove);
  if (media->adjustment)
    g_clear_signal_handler (&media->scrolled, media->adjustment);
  g_clear_object (&media->adjustment);
  /* off the window, a GIF's decoder is let go: mapped again, it is made again */
  if (media->pipeline)
    {
      gst_element_set_state (media->pipeline, GST_STATE_NULL);
      g_clear_handle_id (&media->bus_watch, g_source_remove);
      g_clear_pointer (&media->pipeline, gst_object_unref);
    }
}

static void
media_free (Media *media)
{
  g_cancellable_cancel (media->cancel);
  on_unmap (media->picture, media);
  g_clear_object (&media->cancel);
  g_clear_object (&media->image);
  g_free (media->kind);
  g_free (media->path);
  g_free (media);
}

/* ---- the widget ----------------------------------------------------------------------------------------------- */

GtkWidget *
msg_media_new (const SmsPart *part, guint box)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = part->thumbnail ? gdk_texture_new_from_bytes (part->thumbnail, &error) : NULL;
  gboolean here = msg_media_is_here (part);
  gboolean shown = here && (is_kind (part, "image") || is_kind (part, "sticker") || is_kind (part, "gif"));
  /* the media's own shape when it is known, the preview's otherwise */
  double w = part->width && part->height ? (double) part->width : texture ? gdk_texture_get_width (texture) : 0;
  double h = part->width && part->height ? (double) part->height : texture ? gdk_texture_get_height (texture) : 0;
  double scale;
  GtkWidget *picture, *frame;

  if (error)
    g_debug ("a preview that is not an image: %s", error->message);
  if ((!texture && !shown) || w <= 0 || h <= 0)
    return NULL;
  scale = box / MAX (w, h);
  picture = gtk_picture_new_for_paintable (texture ? GDK_PAINTABLE (texture) : NULL);
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  gtk_widget_set_size_request (picture, MAX (1, (int) (w * scale)), MAX (1, (int) (h * scale)));
  gtk_widget_add_css_class (picture, "msg-media-preview");
  if (is_kind (part, "sticker"))
    gtk_widget_add_css_class (picture, "sticker");
  if (shown)
    {
      Media *media = g_new0 (Media, 1);

      media->picture = picture;
      media->kind = g_strdup (part->kind);
      media->path = g_strdup (part->uri);
      media->width = MAX (1, (int) (w * scale));
      media->height = MAX (1, (int) (h * scale));
      media->cancel = g_cancellable_new ();
      media->moving = is_kind (part, "sticker") && part->animated;
      g_object_set_data_full (G_OBJECT (picture), "msg-media", media, (GDestroyNotify) media_free);
      g_signal_connect (picture, "map", G_CALLBACK (on_map), media);
      g_signal_connect (picture, "unmap", G_CALLBACK (on_unmap), media);
    }
  /* a picture's height follows the width it is given: in a horizontal box it is given its own, not the bubble's */
  frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (frame), picture);
  return frame;
}
