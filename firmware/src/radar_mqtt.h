#ifndef RADAR_MQTT_H
#define RADAR_MQTT_H

#include "radar_app.h"

int radar_mqtt_start(void);
/* Called only on initial state/state changes; never performs network I/O. */
void radar_mqtt_update(const struct iwrl_human_result *result);

#endif
