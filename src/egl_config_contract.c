/*
 * egl_config_contract.c -- the framebuffer contract expected by Horizon
 * Chase's Unity 2022.3.33f1 EGL backend.
 */

#include "egl_config_contract.h"

#include <string.h>

size_t hc_egl_build_window_config_attributes(
    int es_major, int depth, int stencil, int *attributes, size_t capacity) {
  const int renderable =
      es_major >= 3 ? HC_EGL_OPENGL_ES3_BIT_KHR : HC_EGL_OPENGL_ES2_BIT;
  const int values[HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY] = {
      HC_EGL_RED_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_GREEN_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_BLUE_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_ALPHA_SIZE, HC_EGL_RGBA_CHANNEL_BITS,
      HC_EGL_DEPTH_SIZE, depth,
      HC_EGL_STENCIL_SIZE, stencil,
      HC_EGL_SURFACE_TYPE, HC_EGL_WINDOW_BIT | HC_EGL_PBUFFER_BIT,
      HC_EGL_RENDERABLE_TYPE, renderable,
      HC_EGL_NONE
  };

  if (!attributes || capacity < HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY)
    return 0;
  memcpy(attributes, values, sizeof values);
  return HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY;
}

int hc_egl_attribute_value(
    const int *attributes, int attribute, int fallback) {
  if (!attributes)
    return fallback;
  for (size_t index = 0; index + 1 < 128; index += 2) {
    if (attributes[index] == HC_EGL_NONE)
      break;
    if (attributes[index] == attribute)
      return attributes[index + 1];
  }
  return fallback;
}

int hc_egl_config_meets_unity(
    const hc_egl_config_properties *candidate,
    const hc_egl_config_properties *required) {
  if (!candidate || !required)
    return 0;

  return candidate->native_visual_type != 0x108 &&
         candidate->red == required->red &&
         candidate->green == required->green &&
         candidate->blue == required->blue &&
         candidate->alpha == required->alpha &&
         candidate->depth >= required->depth &&
         candidate->stencil >= required->stencil &&
         candidate->samples >= required->samples &&
         (candidate->renderable & required->renderable) ==
             required->renderable &&
         (candidate->surfaces & required->surfaces) == required->surfaces;
}
