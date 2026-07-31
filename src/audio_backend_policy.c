/*
 * audio_backend_policy.c -- capability-based initial SDL audio selection.
 */

#include "audio_backend_policy.h"

#include <string.h>

static int hc_audio_is_pulse_name(const char *name) {
  return name &&
         (strcmp(name, "pulse") == 0 || strcmp(name, "pulseaudio") == 0);
}

const char *hc_audio_normalize_driver_name(const char *name) {
  if (!name || !*name)
    return NULL;
  return strcmp(name, "pulse") == 0 ? "pulseaudio" : name;
}

const char *hc_audio_choose_initial_driver(
    const char *explicit_driver,
    const char *inherited_sdl_driver,
    int alsa_available,
    int keep_inherited_pulse,
    int *used_inherited_pulse_fallback) {
  if (used_inherited_pulse_fallback)
    *used_inherited_pulse_fallback = 0;

  const char *normalized =
      hc_audio_normalize_driver_name(explicit_driver);
  if (normalized)
    return normalized;

  if (alsa_available && !keep_inherited_pulse &&
      hc_audio_is_pulse_name(inherited_sdl_driver)) {
    if (used_inherited_pulse_fallback)
      *used_inherited_pulse_fallback = 1;
    return "alsa";
  }

  return NULL;
}
