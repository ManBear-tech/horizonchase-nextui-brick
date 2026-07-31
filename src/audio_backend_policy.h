/*
 * audio_backend_policy.h -- pure SDL audio-driver policy shared by the
 * runtime and its host-side regression test.
 */

#ifndef HC_AUDIO_BACKEND_POLICY_H
#define HC_AUDIO_BACKEND_POLICY_H

/*
 * Normalize the legacy SDL name used by some PortMaster environments.
 * Other non-empty names are returned unchanged.
 */
const char *hc_audio_normalize_driver_name(const char *name);

/*
 * Select an initial SDL backend before SDL_InitSubSystem/SDL_OpenAudioDevice.
 *
 * An explicit Horizon/AUDIO_DRIVER request always wins.  Otherwise an
 * inherited PulseAudio-only PortMaster selection may be replaced by ALSA
 * when ALSA exists.  This avoids entering an unavailable server backend that
 * can block inside SDL_OpenAudioDevice on ROCKNIX's AArch64 SDL build.
 *
 * NULL means retain SDL's inherited/automatic selection.
 */
const char *hc_audio_choose_initial_driver(
    const char *explicit_driver,
    const char *inherited_sdl_driver,
    int alsa_available,
    int keep_inherited_pulse,
    int *used_inherited_pulse_fallback);

#endif
