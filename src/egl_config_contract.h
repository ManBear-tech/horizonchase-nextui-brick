/*
 * egl_config_contract.h -- EGLConfig requirements shared by the SDL/EGL
 * bridge and its host-side regression test.
 */

#ifndef HC_EGL_CONFIG_CONTRACT_H
#define HC_EGL_CONFIG_CONTRACT_H

#include <stddef.h>

enum {
  HC_EGL_BUFFER_SIZE = 0x3020,
  HC_EGL_ALPHA_SIZE = 0x3021,
  HC_EGL_BLUE_SIZE = 0x3022,
  HC_EGL_GREEN_SIZE = 0x3023,
  HC_EGL_RED_SIZE = 0x3024,
  HC_EGL_DEPTH_SIZE = 0x3025,
  HC_EGL_STENCIL_SIZE = 0x3026,
  HC_EGL_NATIVE_VISUAL_TYPE = 0x302f,
  HC_EGL_SAMPLES = 0x3031,
  HC_EGL_SAMPLE_BUFFERS = 0x3032,
  HC_EGL_SURFACE_TYPE = 0x3033,
  HC_EGL_NONE = 0x3038,
  HC_EGL_COLOR_BUFFER_TYPE = 0x303f,
  HC_EGL_RENDERABLE_TYPE = 0x3040,
  HC_EGL_CONFORMANT = 0x3042,
  HC_EGL_COVERAGE_SAMPLES_NV = 0x30e1,
  HC_EGL_DEPTH_ENCODING_NV = 0x30e2,
  HC_EGL_DEPTH_ENCODING_NONLINEAR_NV = 0x30e3,
  HC_EGL_RGB_BUFFER = 0x308e,
  HC_EGL_PBUFFER_BIT = 0x0001,
  HC_EGL_WINDOW_BIT = 0x0004,
  HC_EGL_OPENGL_ES2_BIT = 0x0004,
  HC_EGL_OPENGL_ES3_BIT_KHR = 0x0040,
  HC_EGL_RGBA_CHANNEL_BITS = 8,
  HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY = 17
};

typedef struct {
  int red;
  int green;
  int blue;
  int alpha;
  int depth;
  int stencil;
  int samples;
  int renderable;
  int surfaces;
  int native_visual_type;
} hc_egl_config_properties;

/*
 * Build the real EGL selection request used underneath SDL. Horizon Chase's
 * Unity build requires an exact RGBA8888 color format, not RGBX8888.
 */
size_t hc_egl_build_window_config_attributes(
    int es_major, int depth, int stencil, int *attributes, size_t capacity);

/* Read one value from a terminated EGL attribute list. */
int hc_egl_attribute_value(
    const int *attributes, int attribute, int fallback);

/* Reproduce the relevant part of Unity's minimum-spec filter. */
int hc_egl_config_meets_unity(
    const hc_egl_config_properties *candidate,
    const hc_egl_config_properties *required);

#endif
