/*
 * error.h -- error handler
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#ifndef __ERROR_H__
#define __ERROR_H__

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

#endif
