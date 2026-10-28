#ifndef TS_PROXIMITY_TS3_CEPOS_WIRE_H
#define TS_PROXIMITY_TS3_CEPOS_WIRE_H

/*
 * CEPOS receive-side helpers — pure, header-only (see doku/module/ce-protokoll.md).
 *
 * Free of Win32, the TS SDK and globals so the host unit test
 * (tests/cepos_wire_test.c) verifies the shipping code, not a copy. The
 * 56-byte CeposPacket itself is frozen and stays in ts3_cepos.h; nothing here
 * changes the wire format.
 *
 * Thread contract: none — every function is pure (arguments in, result out).
 */

#include <stddef.h>

/* playerName comes straight from another client, so it is hostile input. It is
   stored in the player table and written to plugin.log; a control byte
   (newline, carriage return, ESC ...) could forge extra log lines. Replace
   every byte < 0x20 and 0x7F with '?' — bytes >= 0x80 stay, so UTF-8 names
   still show up unchanged. Stops at the first NUL; `size` bounds the scan for
   a buffer that was not terminated yet. */
static inline void cepos_wire_sanitize_name(char* name, size_t size) {
    size_t i;

    if (!name) {
        return;
    }
    for (i = 0; i < size && name[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) {
            name[i] = '?';
        }
    }
}

#endif /* TS_PROXIMITY_TS3_CEPOS_WIRE_H */
