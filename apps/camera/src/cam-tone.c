/* cam-tone: see cam-tone.h. */
#include "cam-tone.h"

#include <math.h>

void
cam_tone_clamp (CamTone *tone)
{
  tone->gamma = CLAMP (tone->gamma, CAM_GAMMA_MIN, CAM_GAMMA_MAX);
  tone->contrast = CLAMP (tone->contrast, CAM_CONTRAST_MIN, CAM_CONTRAST_MAX);
  tone->saturation = CLAMP (tone->saturation, CAM_SATURATION_MIN, CAM_SATURATION_MAX);
}

static gboolean
lookup (GVariant *dict, const char *camera_id, CamTone *tone)
{
  g_autoptr(GVariant) entry = camera_id ? g_variant_lookup_value (dict, camera_id, G_VARIANT_TYPE ("(ddd)")) : NULL;

  if (!entry)
    return FALSE;
  g_variant_get (entry, "(ddd)", &tone->gamma, &tone->contrast, &tone->saturation);
  cam_tone_clamp (tone);
  return TRUE;
}

gboolean
cam_tone_load (GSettings *settings, const char *camera_id, CamTone *tone)
{
  g_autoptr(GVariant) dict = g_settings_get_value (settings, "tuning");

  return lookup (dict, camera_id, tone);
}

gboolean
cam_tone_load_default (GSettings *settings, const char *camera_id, CamTone *tone)
{
  g_autoptr(GVariant) dict = g_settings_get_default_value (settings, "tuning");

  return lookup (dict, camera_id, tone);
}

void
cam_tone_store (GSettings *settings, const char *camera_id, const CamTone *tone)
{
  g_autoptr(GVariant) old = g_settings_get_value (settings, "tuning");
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{s(ddd)}"));
  g_variant_iter_init (&iter, old);
  while (g_variant_iter_next (&iter, "{&s@(ddd)}", &key, &value))
    {
      if (g_strcmp0 (key, camera_id))
        g_variant_builder_add (&builder, "{s@(ddd)}", key, value);
      g_variant_unref (value);
    }
  g_variant_builder_add (&builder, "{s(ddd)}", camera_id, tone->gamma, tone->contrast, tone->saturation);
  g_settings_set_value (settings, "tuning", g_variant_builder_end (&builder));
}
