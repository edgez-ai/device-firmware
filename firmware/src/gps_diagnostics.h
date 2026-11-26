/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
/* RAM-only SWD diagnostics. Millisecond stamps use the low 32 bits of uptime;
 * UINT32_MAX means the event has not occurred in this acquisition. */
struct gps_uart_diagnostics {
    uint32_t starts, started_ms, first_rx_ms, last_rx_ms;
    uint32_t received_bytes, dropped_bytes, stopped_ms;
};
struct gps_acquisition_diagnostics {
    uint32_t sequence, requested_ms, first_step_ms, first_line_ms, fixed_ms, finished_ms;
    uint32_t lines, rmc_lines, active_rmc_lines, rejected_rmc_lines, state, result;
    char last_line[128], last_rmc[128];
};
extern volatile struct gps_uart_diagnostics gps_uart_diagnostics;
extern volatile struct gps_acquisition_diagnostics gps_acquisition_diagnostics;
