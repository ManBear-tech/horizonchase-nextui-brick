#ifndef HORIZON_INPUT_H
#define HORIZON_INPUT_H

#include <stdint.h>

/*
 * Native controller bridge for Horizon Chase 2.6.9.
 *
 * Linux/SDL controllers are normalized to one Xbox layout and exposed at the
 * game's own GamepadInputSource boundary.  The rest of the navigation and
 * gameplay flow remains inside Horizon's managed code.
 */
int hc_input_install(uintptr_t il2cpp_base);
void hc_input_poll(void);
int hc_input_exit_requested(void);
void hc_input_shutdown(void);

#endif
