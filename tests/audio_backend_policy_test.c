#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "audio_backend_policy.h"

int main(void) {
  int inherited_fallback = -1;

  assert(strcmp(hc_audio_normalize_driver_name("pulse"),
                "pulseaudio") == 0);
  assert(strcmp(hc_audio_normalize_driver_name("alsa"), "alsa") == 0);
  assert(hc_audio_normalize_driver_name(NULL) == NULL);
  assert(hc_audio_normalize_driver_name("") == NULL);

  const char *driver = hc_audio_choose_initial_driver(
      "pulseaudio", "pulseaudio", 1, 0, &inherited_fallback);
  assert(strcmp(driver, "pulseaudio") == 0);
  assert(inherited_fallback == 0);

  driver = hc_audio_choose_initial_driver(
      "pulse", "pulseaudio", 1, 0, &inherited_fallback);
  assert(strcmp(driver, "pulseaudio") == 0);
  assert(inherited_fallback == 0);

  driver = hc_audio_choose_initial_driver(
      NULL, "pulseaudio", 1, 0, &inherited_fallback);
  assert(strcmp(driver, "alsa") == 0);
  assert(inherited_fallback == 1);

  driver = hc_audio_choose_initial_driver(
      NULL, "pulse", 1, 0, &inherited_fallback);
  assert(strcmp(driver, "alsa") == 0);
  assert(inherited_fallback == 1);

  driver = hc_audio_choose_initial_driver(
      NULL, "pulseaudio", 0, 0, &inherited_fallback);
  assert(driver == NULL);
  assert(inherited_fallback == 0);

  driver = hc_audio_choose_initial_driver(
      NULL, "pulseaudio", 1, 1, &inherited_fallback);
  assert(driver == NULL);
  assert(inherited_fallback == 0);

  driver = hc_audio_choose_initial_driver(
      NULL, "alsa", 1, 0, &inherited_fallback);
  assert(driver == NULL);
  assert(inherited_fallback == 0);

  driver = hc_audio_choose_initial_driver(
      NULL, NULL, 1, 0, &inherited_fallback);
  assert(driver == NULL);
  assert(inherited_fallback == 0);

  puts("audio_backend_policy_test: OK");
  return 0;
}
