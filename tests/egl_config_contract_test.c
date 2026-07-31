#include <assert.h>
#include <stdio.h>

#include "egl_config_contract.h"

int main(void) {
  int attributes[HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY];
  size_t count = hc_egl_build_window_config_attributes(
      3, 24, 8, attributes,
      sizeof attributes / sizeof attributes[0]);

  assert(count == HC_EGL_WINDOW_CONFIG_ATTR_CAPACITY);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_RED_SIZE, -1) == 8);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_GREEN_SIZE, -1) == 8);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_BLUE_SIZE, -1) == 8);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_ALPHA_SIZE, -1) == 8);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_DEPTH_SIZE, -1) == 24);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_STENCIL_SIZE, -1) == 8);
  assert(hc_egl_attribute_value(
             attributes, HC_EGL_RENDERABLE_TYPE, -1) ==
         HC_EGL_OPENGL_ES3_BIT_KHR);

  hc_egl_config_properties required = {
      .red = 8,
      .green = 8,
      .blue = 8,
      .alpha = 8,
      .depth = 24,
      .stencil = 8,
      .samples = 0,
      .renderable = HC_EGL_OPENGL_ES3_BIT_KHR,
      .surfaces = HC_EGL_WINDOW_BIT | HC_EGL_PBUFFER_BIT,
      .native_visual_type = 0
  };
  hc_egl_config_properties panfrost_rgbx = required;
  panfrost_rgbx.alpha = 0;
  assert(!hc_egl_config_meets_unity(&panfrost_rgbx, &required));

  hc_egl_config_properties panfrost_rgba = required;
  assert(hc_egl_config_meets_unity(&panfrost_rgba, &required));

  hc_egl_config_properties shallow_depth = required;
  shallow_depth.depth = 16;
  assert(!hc_egl_config_meets_unity(&shallow_depth, &required));

  hc_egl_config_properties es2_only = required;
  es2_only.renderable = HC_EGL_OPENGL_ES2_BIT;
  assert(!hc_egl_config_meets_unity(&es2_only, &required));

  puts("egl_config_contract_test: OK");
  return 0;
}
