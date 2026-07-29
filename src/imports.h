/*
 * imports.h -- .so import resolution
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"
#include <stdio.h>

extern FILE *stderr_fake;
extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

#endif
