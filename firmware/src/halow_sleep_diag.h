#ifndef HALOW_SLEEP_DIAG_H
#define HALOW_SLEEP_DIAG_H

#include <stdbool.h>
#include <stddef.h>

/* Keep the string plus resource TLV header inside a single 256-byte block. */
#define HALOW_SLEEP_DIAG_TEXT_SIZE 251

#if defined(CONFIG_RADAR_HALOW_SLEEP_DIAG)
void halow_sleep_diag_activity(void);
void halow_sleep_diag_link_changed(bool online);
void halow_sleep_diag_report(char *text, size_t size);
#else
static inline void halow_sleep_diag_activity(void) {}
static inline void halow_sleep_diag_link_changed(bool online) { (void)online; }
#endif

#endif
