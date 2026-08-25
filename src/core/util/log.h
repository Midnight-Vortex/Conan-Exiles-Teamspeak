#ifndef CORE_UTIL_LOG_H
#define CORE_UTIL_LOG_H

#include <wchar.h>

/*
 * Plugin log file — Documents\Conan Exiles TeamSpeak plugin\plugin.log
 *
 * Thread contract: callable from ANY thread. Uses its own private lock and
 * never touches the TS API or any other plugin lock (lesson from the old
 * plugin: sharing the API lock with logging corrupted the critical section).
 *
 * log_write  — always written (boot, shutdown, errors; low volume).
 * log_debug  — only written when debug mode is enabled in plugin.cfg.
 *
 * plugin.log is truncated when it exceeds 100 MB (safety cap; any thread,
 * under the log lock).
 */

void log_set_enabled(int enabled);
int log_is_enabled(void);
const wchar_t* log_get_path(void);
void log_write(const char* fmt, ...);
void log_debug(const char* fmt, ...);
void log_close(void);

#endif /* CORE_UTIL_LOG_H */
