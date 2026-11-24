/*
 * Wi-Fi HaLow station on the FGH200M (Morse Micro MM8108), using the
 * official mm-iot-zephyr driver through Zephyr's Wi-Fi management API, with
 * DTIM power save.
 */
#ifndef HALOW_STA_H
#define HALOW_STA_H

#include <stdbool.h>

/* Register for network events and request the first connection. Returns
 * -ENOTSUP when no SSID is configured. */
int halow_sta_start(void);

/* True while associated and holding an IPv4 address. */
bool halow_sta_is_online(void);

/* Schedule disconnect/reconnect on the system workqueue; never call SDK APIs
 * from network event callbacks or interrupt context. */
int halow_sta_reconnect(void);

#endif /* HALOW_STA_H */
