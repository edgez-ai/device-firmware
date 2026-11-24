/*
 * Flash an application image into the IWRL6432 through its ROM UART
 * bootloader. This is a C port of the part of TI's arprog.py
 * (tools/flash/arprog.py) that tools/flash/arprog_cmdline.py uses for
 *   -s SFLASH -t META_IMAGE1 -f <appimage>
 *
 * Before calling iwrl_bl_connect() the caller must have started the ROM
 * bootloader: SOP pins in flashing mode, UART TX held low (break) across the
 * radar reset. After a successful download the caller returns the SOP pins to
 * functional mode and resets the radar.
 *
 * Framing (115200 8N1):
 *   host -> radar : 0xAA, length (big-endian 16 bit, payload + 2),
 *                   checksum (sum of payload bytes), payload
 *   radar -> host : 2 length bytes, checksum, 0x00, then 0xCC (ACK) or 0x33 (NACK)
 */
#ifndef IWRL6432_BOOTLOADER_H
#define IWRL6432_BOOTLOADER_H

#include "iwrl6432_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IWRL_BL_CHUNK_SIZE          240U           /* arprog DEFAULT_CHUNK_SIZE */
#define IWRL_BL_MAX_IMAGE_SIZE      (1024U * 1024U) /* arprog MAX_FILE_SIZE */

struct iwrl_bl_timeouts {
	uint32_t connect_ms;   /* per ping while waiting for the bootloader */
	uint32_t erase_ms;     /* START_DOWNLOAD and FILE_CLOSE: flash erase/commit */
	uint32_t chunk_ms;     /* one data chunk */
};

/* Called after every chunk; may be NULL. */
typedef void (*iwrl_bl_progress_fn)(size_t sent, size_t total, void *user);

/* Fill t with defaults that are generous for a 200 KB image. */
void iwrl_bl_default_timeouts(struct iwrl_bl_timeouts *t);

/* Confirm that the ROM bootloader is listening: discards stale input (such as
 * the ACK the ROM sends when it sees the break) and pings up to `attempts`
 * times. Returns IWRL_OK or IWRL_ERR_TIMEOUT / IWRL_ERR_NACK. */
int iwrl_bl_connect(const struct iwrl_port *port, const struct iwrl_bl_timeouts *t,
		    unsigned int attempts);

/* Write `image` as META_IMAGE1 to the radar's serial flash. */
int iwrl_bl_download(const struct iwrl_port *port, const struct iwrl_bl_timeouts *t,
		     const uint8_t *image, size_t image_len,
		     iwrl_bl_progress_fn progress, void *user);

#ifdef __cplusplus
}
#endif

#endif /* IWRL6432_BOOTLOADER_H */
