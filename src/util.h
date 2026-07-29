/*
 * util.h -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stddef.h>
#include <stdint.h>

int debugPrintf(const char *text, ...);

/*
 * Runtime root shared by NextOS and PortMaster/ArkOS launchers.
 *
 * HC_GAMEDIR is authoritative when it contains an absolute path.  Otherwise
 * the process' initial working directory is used, with the historical NextOS
 * path retained only as a last-resort fallback.
 */
const char *hc_game_dir(void);
int hc_game_path(char *dst, size_t dst_size, const char *relative);

int ret0(void);
int ret1(void);
int retm1(void);

#endif
