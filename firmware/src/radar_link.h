/*
 * nRF54L15 side of the IWRL6432 link: the UART, the GPIO that switches the
 * radar's 3.3 V supply (the radar's NRESET is not connected, so the nRF
 * restarts it by power-cycling), and the optional wake-up on the UART RX line
 * while the UART is switched off. Pins come from the devicetree overlay.
 *
 * The radar's boot mode (flashing or functional) is selected by its SOP pins
 * at reset. Select functional mode for nRF operation; flash separately via USB.
 */
#ifndef RADAR_LINK_H
#define RADAR_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "iwrl6432_port.h"

/* Start receive-only radar or prepare the link for CLI configuration. */
int radar_link_init(void);

/* Switch the radar off and on again, wait for its application to boot, then
 * enable the UART. The radar then waits for its configuration. */
int radar_link_power_cycle(void);

/* Byte transport for the iwrl6432 library. */
const struct iwrl_port *radar_link_port(void);

/* Enable or disable (and power down) UART reception. */
int radar_link_rx_enable(bool enable);

/* True if the UART RX pin can wake the CPU while the UART is off (the overlay
 * has radar-rx-wake-gpios and the pin can interrupt). */
bool radar_link_has_wake(void);

/* Call with reception disabled. Sleep until the RX line goes low: the radar
 * sends one 0x00 byte ahead of each report. Returns 1 when it did (enable
 * reception now; that byte itself is lost), 0 after timeout_ms without it, or
 * a negative error. Returns 1 at once when there is no wake-up on RX. */
int radar_link_wait_wake(uint32_t timeout_ms);

/* Log cumulative traffic/errors and the first 64 raw received bytes. */
void radar_link_log_diagnostics(void);

#endif /* RADAR_LINK_H */
