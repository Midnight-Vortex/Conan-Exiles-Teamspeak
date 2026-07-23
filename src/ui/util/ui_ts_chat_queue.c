#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"

#include <string.h>
#include <windows.h>

/*
 * Deferred TS chat messages: producers queue from any thread; the callback
 * thread drains via ts3_plugin_flush_pending_chat() during CEDRAIN.
 * UI/util layer — flush calls TS shims in plugin_ui_compat.c.
 */

#define TS3_CHAT_QUEUE_SIZE 32

static char ts3ChatQueue[TS3_CHAT_QUEUE_SIZE][512];
static volatile long ts3ChatQueueHead = 0;
static volatile long ts3ChatQueueTail = 0;

void ts3_queue_chat_message(const char* message) {
    if (!message) {
        return;
    }
    const long tail = InterlockedCompareExchange(&ts3ChatQueueTail, 0, 0);
    const long next = (tail + 1) % TS3_CHAT_QUEUE_SIZE;
    if (next == InterlockedCompareExchange(&ts3ChatQueueHead, 0, 0)) {
        ts3_debug_log("TS-CHAT: queue full — message dropped");
        return;
    }
    strncpy_s(ts3ChatQueue[tail], sizeof(ts3ChatQueue[tail]), message, _TRUNCATE);
    InterlockedExchange(&ts3ChatQueueTail, next);
}

int ts3_plugin_has_pending_chat(void) {
    const long head = InterlockedCompareExchange(&ts3ChatQueueHead, 0, 0);
    const long tail = InterlockedCompareExchange(&ts3ChatQueueTail, 0, 0);
    return head != tail;
}

void ts3_plugin_clear_pending_chat(void) {
    InterlockedExchange(&ts3ChatQueueHead, 0);
    InterlockedExchange(&ts3ChatQueueTail, 0);
}

void ts3_plugin_flush_pending_chat(void) {
    if (!ts3_plugin_is_on_callback_thread()) {
        return;
    }
    for (;;) {
        const long head = InterlockedCompareExchange(&ts3ChatQueueHead, 0, 0);
        const long tail = InterlockedCompareExchange(&ts3ChatQueueTail, 0, 0);
        if (head == tail) {
            break;
        }
        ts3_debug_logf("TS-CHAT: flush '%.120s'", ts3ChatQueue[head]);
        ts3_adapter_print_chat_force(ts3ChatQueue[head]);
        InterlockedExchange(&ts3ChatQueueHead, (head + 1) % TS3_CHAT_QUEUE_SIZE);
    }
}
