/*
 * Receive-only UART supervisor for the autostart IWRL6432 firmware.
 */
#ifndef RADAR_APP_H
#define RADAR_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "iwrl6432_stream.h"

struct radar_status {
	bool     running;          /* results are arriving */
	bool     valid;            /* `result` holds a decoded frame */
	int64_t  result_uptime_ms; /* k_uptime_get() of the last result */
	struct iwrl_human_result result;
	uint32_t restarts;         /* retained status field; zero in receive-only mode */
};

/* Start the supervisor thread. */
int radar_app_start(void);

/* Copy the latest status. */
void radar_app_get_status(struct radar_status *status);

/* Called from the radar thread for every decoded frame. The weak default
 * logs changes; an application can override it to forward the data. */
void radar_app_on_result(const struct iwrl_human_result *result, bool changed);

#endif /* RADAR_APP_H */
