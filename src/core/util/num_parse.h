#ifndef CORE_UTIL_NUM_PARSE_H
#define CORE_UTIL_NUM_PARSE_H

/*
 * Lenient decimal parsing for hand-written settings (plugin.cfg, hub channel
 * description).  German admins and players type "13,5" where strtod() only
 * understands "13.5" and silently stops at the comma (-> 13).
 *
 * num_strtod() accepts ONE comma as decimal separator, but only in the safe
 * unambiguous shape  <digit>,<digit>  with no '.' in the same number:
 *     "13,5"  -> 13.5      "0,25" -> 0.25     "1,5e3" -> 1500
 *     "13.5"  -> 13.5      (unchanged)
 *     "1,000.5", "1,2,3", ",5", "13," -> exactly what strtod() returned before
 * Anything that does not parse keeps strtod()'s result (0.0), so typos fall
 * back exactly like they did before.
 *
 * Use it ONLY for single-number values.  Comma separated LISTS (SteamID=...)
 * must never go through here.
 *
 * Thread contract: header-only, pure, any thread.
 */

#include <stdlib.h>
#include <string.h>

static inline int num_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* strtod() with the single-decimal-comma extension described above. */
static inline double num_strtod(const char* s) {
    char buf[64];
    size_t n = 0;
    size_t commaAt = 0;
    int commas = 0;
    int dots = 0;
    const char* t;

    if (!s) {
        return 0.0;
    }
    t = s;
    while (*t == ' ' || *t == '\t') {
        t++;
    }

    /* The numeric token: only characters that can be part of a decimal number. */
    while (t[n] != '\0' && n < sizeof(buf) - 1
        && (num_is_digit(t[n]) || t[n] == '+' || t[n] == '-' || t[n] == '.'
            || t[n] == ',' || t[n] == 'e' || t[n] == 'E')) {
        if (t[n] == ',') {
            commas++;
            commaAt = n;
        }
        else if (t[n] == '.') {
            dots++;
        }
        n++;
    }

    if (commas == 1 && dots == 0 && commaAt > 0
        && num_is_digit(t[commaAt - 1]) && num_is_digit(t[commaAt + 1])) {
        memcpy(buf, t, n);
        buf[n] = '\0';
        buf[commaAt] = '.';
        return strtod(buf, NULL);
    }
    return strtod(s, NULL);
}

#endif /* CORE_UTIL_NUM_PARSE_H */
