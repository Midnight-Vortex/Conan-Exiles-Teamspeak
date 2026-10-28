#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "ui/util/ui_chat_ring.h"

#include <string.h>
#include <windows.h>

/*
 * Deferred TS chat messages: producers queue from any thread; the callback
 * thread drains via ts3_plugin_flush_pending_chat() during CEDRAIN.
 * UI/util layer — flush calls TS shims in plugin_ui_compat.c.
 *
 * Thread contract (review-fix 088): MULTI-producer / single-consumer.
 *  - ts3_queue_chat_message / ts3_plugin_has_pending_chat /
 *    ts3_plugin_clear_pending_chat: any thread (producers are the key-monitor
 *    thread, the settings-dialog thread, the pos watcher, ...).
 *  - ts3_plugin_flush_pending_chat: callback thread only (no-op elsewhere).
 *    It pops ONE line under the ring lock, releases the lock, and only then
 *    calls the TS shim — the TS API is never entered while a lock is held.
 * The ring mechanics + lock live in ui_chat_ring.h (host-tested).
 */

static UiChatRing g_chatRing; /* zero-initialised static == valid empty ring */

void ts3_queue_chat_message(const char* message) {
    if (!message) {
        return;
    }
    if (!ui_chat_ring_push(&g_chatRing, message)) {
        ts3_debug_log("TS-CHAT: queue full — message dropped");
    }
}

int ts3_plugin_has_pending_chat(void) {
    return ui_chat_ring_nonempty(&g_chatRing);
}

void ts3_plugin_clear_pending_chat(void) {
    ui_chat_ring_clear(&g_chatRing);
}

void ts3_plugin_flush_pending_chat(void) {
    if (!ts3_plugin_is_on_callback_thread()) {
        return;
    }
    char line[UI_CHAT_MSG_MAX];
    while (ui_chat_ring_pop(&g_chatRing, line, sizeof(line))) {
        ts3_debug_logf("TS-CHAT: flush '%.120s'", line);
        ts3_adapter_print_chat_force(line);
    }
}
