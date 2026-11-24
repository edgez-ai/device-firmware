/* IWRL6432 CLI configuration sender. See iwrl6432_cli.h. */
#include <string.h>

#include "iwrl6432_cli.h"

#define REPLY_MAX   256U

/* Collect answer text until the prompt appears. The text is kept (truncated
 * to REPLY_MAX - 1 characters) so "Error" can be found and traced. */
static int read_until_prompt(const struct iwrl_port *port, char *reply, uint32_t timeout_ms)
{
	static const char prompt[] = IWRL_CLI_PROMPT;
	const size_t prompt_len = sizeof(prompt) - 1U;
	size_t matched = 0U;
	size_t len = 0U;

	reply[0] = '\0';
	for (;;) {
		uint8_t byte;
		int rc = port->read(port->ctx, &byte, 1U, timeout_ms);

		if (rc < 0) {
			return rc;
		}
		if (rc == 0) {
			return IWRL_ERR_TIMEOUT;
		}
		if ((byte >= 0x20U) && (byte < 0x7FU) && (len < (REPLY_MAX - 1U))) {
			reply[len++] = (char)byte;
			reply[len] = '\0';
		}
		if ((char)byte == prompt[matched]) {
			matched++;
			if (matched == prompt_len) {
				return IWRL_OK;
			}
		} else {
			matched = ((char)byte == prompt[0]) ? 1U : 0U;
		}
	}
}

int iwrl_cli_wait_ready(const struct iwrl_port *port, int poke, uint32_t timeout_ms)
{
	char reply[REPLY_MAX];

	if (poke != 0) {
		static const uint8_t newline[] = { '\n' };
		int rc;

		port->flush_rx(port->ctx);
		rc = port->write(port->ctx, newline, sizeof(newline));
		if (rc < 0) {
			return rc;
		}
	}
	return read_until_prompt(port, reply, timeout_ms);
}

int iwrl_cli_send_config(const struct iwrl_port *port, const char *cfg, size_t cfg_len,
			 uint32_t timeout_ms, iwrl_cli_trace_fn trace, void *user)
{
	char line[IWRL_CLI_MAX_LINE + 2U];
	char reply[REPLY_MAX];
	size_t pos = 0U;

	while (pos < cfg_len) {
		size_t start = pos;
		size_t end;
		size_t len;
		int is_start;
		int rc;

		while ((pos < cfg_len) && (cfg[pos] != '\n')) {
			pos++;
		}
		end = pos;
		if (pos < cfg_len) {
			pos++;
		}
		while ((start < end) && ((cfg[start] == ' ') || (cfg[start] == '\t'))) {
			start++;
		}
		while ((end > start) && ((cfg[end - 1U] == '\r') || (cfg[end - 1U] == ' ') ||
					 (cfg[end - 1U] == '\t'))) {
			end--;
		}
		len = end - start;
		if ((len == 0U) || (cfg[start] == '%') || (cfg[start] == '#')) {
			continue;
		}
		if (len > IWRL_CLI_MAX_LINE) {
			return IWRL_ERR_ARGUMENT;
		}
		memcpy(line, &cfg[start], len);
		line[len] = '\n';
		line[len + 1U] = '\0';
		is_start = (len >= 11U) && (memcmp(line, "sensorStart", 11U) == 0);

		port->flush_rx(port->ctx);
		rc = port->write(port->ctx, (const uint8_t *)line, len + 1U);
		if (rc < 0) {
			return rc;
		}
		line[len] = '\0';
		if (is_start) {
			if (trace != NULL) {
				trace(line, "", user);
			}
			return IWRL_OK;
		}

		rc = read_until_prompt(port, reply, timeout_ms);
		if (trace != NULL) {
			trace(line, reply, user);
		}
		if (rc != IWRL_OK) {
			return rc;
		}
		/* An older radar firmware answers a command it lacks (such as
		 * hostReportCfg) with "... is not recognized as a CLI command". */
		if ((strstr(reply, "Error") != NULL) ||
		    (strstr(reply, "not recognized") != NULL)) {
			return IWRL_ERR_CLI;
		}
	}
	return IWRL_OK;
}
