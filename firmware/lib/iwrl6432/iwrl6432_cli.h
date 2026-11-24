/*
 * Send a sensor configuration (.cfg text, as in firmware/chirp_configs/) to
 * the radar application's CLI and start it.
 */
#ifndef IWRL6432_CLI_H
#define IWRL6432_CLI_H

#include "iwrl6432_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IWRL_CLI_PROMPT          "mmwDemo:/>"
#define IWRL_CLI_MAX_LINE        200U

/* Called with each command and the radar's answer text; may be NULL. */
typedef void (*iwrl_cli_trace_fn)(const char *command, const char *reply, void *user);

/* Wait until the radar prints its prompt (after reset, or after an empty
 * line is sent when `poke` is non-zero). */
int iwrl_cli_wait_ready(const struct iwrl_port *port, int poke, uint32_t timeout_ms);

/* Send every command line of `cfg` ('%' and '#' lines and blank lines are
 * skipped). Each command must be answered with the prompt within
 * `timeout_ms`, without the word "Error" and without "not recognized". The answer to the final
 * "sensorStart" is not awaited, because binary data follows it immediately.
 * Returns IWRL_OK, IWRL_ERR_CLI, IWRL_ERR_TIMEOUT or a port error. */
int iwrl_cli_send_config(const struct iwrl_port *port, const char *cfg, size_t cfg_len,
			 uint32_t timeout_ms, iwrl_cli_trace_fn trace, void *user);

#ifdef __cplusplus
}
#endif

#endif /* IWRL6432_CLI_H */
