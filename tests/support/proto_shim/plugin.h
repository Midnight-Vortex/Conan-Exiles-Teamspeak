#ifndef TESTS_SUPPORT_PROTO_SHIM_PLUGIN_H
#define TESTS_SUPPORT_PROTO_SHIM_PLUGIN_H

/*
 * TEST-ONLY replacement for src/plugin.h (protocol flush/receive harness).
 *
 * The real plugin.h drags in the whole Win32 UI/config type zoo. The CE
 * protocol modules (ts3_cemode.c, ts3_ceping.c, ...) only need one global
 * from it: pluginShuttingDown. The test passes this directory with
 * `-iquote`, which gcc searches BEFORE every -I directory (including -Isrc),
 * so `#include "plugin.h"` resolves here on the host; the real Windows builds
 * never see this file.
 */

#include <windows.h>

extern BOOL pluginShuttingDown;

#endif /* TESTS_SUPPORT_PROTO_SHIM_PLUGIN_H */
