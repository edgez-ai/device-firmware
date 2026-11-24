/*
 * Byte transport used by the IWRL6432 host library. The library itself has no
 * Zephyr or hardware dependency: the application supplies these callbacks
 * (see src/radar_link.c for the nRF54L15 implementation and
 * test/iwrl6432_test.c for a simulated radar).
 */
#ifndef IWRL6432_PORT_H
#define IWRL6432_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iwrl_port {
	/* Send all bytes. Returns 0, or a negative error. */
	int (*write)(void *ctx, const uint8_t *data, size_t len);
	/* Receive up to len bytes, waiting at most timeout_ms for the first one
	 * and for each further one. Returns the number of bytes read (0 on
	 * timeout), or a negative error. */
	int (*read)(void *ctx, uint8_t *data, size_t len, uint32_t timeout_ms);
	/* Discard everything received so far. */
	void (*flush_rx)(void *ctx);
	void *ctx;
};

/* Library error codes (negative), in addition to errors from the port. */
#define IWRL_OK              0
#define IWRL_ERR_TIMEOUT    -1001  /* no (complete) answer in time */
#define IWRL_ERR_NACK       -1002  /* the radar rejected a bootloader command */
#define IWRL_ERR_ARGUMENT   -1003
#define IWRL_ERR_CLI        -1004  /* the radar CLI answered "Error" */

#ifdef __cplusplus
}
#endif

#endif /* IWRL6432_PORT_H */
