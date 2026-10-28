#ifndef CORE_UTIL_POLL_INTERVAL_H
#define CORE_UTIL_POLL_INTERVAL_H

/* Shared 30 ms floor for important reactive work: pos watcher, key monitor,
   CEPOS / CEMODE / CEAUTH send throttle, callback wakeup rate limit.
   Edge-triggered commands still fire only on their event; this cap just
   keeps them on the same tick as CEPOS instead of a slower private interval.
   CEPING is armed by the same tick but sends at most once per second (083). */
#define PLUGIN_POLL_INTERVAL_MS  30

#endif /* CORE_UTIL_POLL_INTERVAL_H */
