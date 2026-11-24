/* IWRL6432 ROM UART bootloader client. See iwrl6432_bootloader.h. */
#include <string.h>

#include "iwrl6432_bootloader.h"

#define BL_SYNC                 0xAAU
#define BL_ACK                  0xCCU
#define BL_NACK                 0x33U
#define BL_OP_PING              0x20U
#define BL_OP_START_DOWNLOAD    0x21U
#define BL_OP_FILE_CLOSE        0x22U
#define BL_OP_SEND_DATA         0x24U

#define BL_STORAGE_SFLASH       2U
#define BL_FILE_META_IMAGE1     4U

static void put_be32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static int send_packet(const struct iwrl_port *port, const uint8_t *payload, size_t len)
{
	uint8_t header[4];
	uint8_t checksum = 0U;
	size_t i;
	int rc;

	for (i = 0U; i < len; i++) {
		checksum = (uint8_t)(checksum + payload[i]);
	}
	header[0] = BL_SYNC;
	header[1] = (uint8_t)((len + 2U) >> 8);
	header[2] = (uint8_t)(len + 2U);
	header[3] = checksum;

	rc = port->write(port->ctx, header, sizeof(header));
	if (rc < 0) {
		return rc;
	}
	return port->write(port->ctx, payload, len);
}

static int read_exact(const struct iwrl_port *port, uint8_t *data, size_t len,
		      uint32_t timeout_ms)
{
	size_t got = 0U;

	while (got < len) {
		int rc = port->read(port->ctx, &data[got], len - got, timeout_ms);

		if (rc < 0) {
			return rc;
		}
		if (rc == 0) {
			return IWRL_ERR_TIMEOUT;
		}
		got += (size_t)rc;
	}
	return IWRL_OK;
}

/* The answer is: length (2), checksum (1), 0x00, then ACK or NACK. The first
 * four bytes are skipped unseen, as arprog does: the checksum byte of an ACK
 * frame is itself 0xCC. */
static int read_ack(const struct iwrl_port *port, uint32_t timeout_ms)
{
	uint8_t byte[4];
	unsigned int guard;
	int rc;

	rc = read_exact(port, byte, 4U, timeout_ms);
	if (rc != IWRL_OK) {
		return rc;
	}
	for (guard = 0U; guard < 64U; guard++) {
		rc = read_exact(port, byte, 1U, timeout_ms);
		if (rc != IWRL_OK) {
			return rc;
		}
		if (byte[0] == BL_ACK) {
			return IWRL_OK;
		}
		if (byte[0] == BL_NACK) {
			return IWRL_ERR_NACK;
		}
	}
	return IWRL_ERR_TIMEOUT;
}

static int command(const struct iwrl_port *port, const uint8_t *payload, size_t len,
		   uint32_t timeout_ms)
{
	int rc = send_packet(port, payload, len);

	if (rc < 0) {
		return rc;
	}
	return read_ack(port, timeout_ms);
}

void iwrl_bl_default_timeouts(struct iwrl_bl_timeouts *t)
{
	t->connect_ms = 300U;
	t->erase_ms = 20000U;
	t->chunk_ms = 5000U;
}

int iwrl_bl_connect(const struct iwrl_port *port, const struct iwrl_bl_timeouts *t,
		    unsigned int attempts)
{
	static const uint8_t ping[] = { BL_OP_PING };
	int rc = IWRL_ERR_TIMEOUT;
	unsigned int attempt;

	for (attempt = 0U; attempt < attempts; attempt++) {
		port->flush_rx(port->ctx);
		rc = command(port, ping, sizeof(ping), t->connect_ms);
		if (rc == IWRL_OK) {
			break;
		}
	}
	return rc;
}

int iwrl_bl_download(const struct iwrl_port *port, const struct iwrl_bl_timeouts *t,
		     const uint8_t *image, size_t image_len,
		     iwrl_bl_progress_fn progress, void *user)
{
	uint8_t packet[1U + IWRL_BL_CHUNK_SIZE];
	size_t offset = 0U;
	int rc;

	if ((image == NULL) || (image_len == 0U) || (image_len >= IWRL_BL_MAX_IMAGE_SIZE)) {
		return IWRL_ERR_ARGUMENT;
	}

	/* START_DOWNLOAD: size, storage, file id, mirror enabled (0). */
	packet[0] = BL_OP_START_DOWNLOAD;
	put_be32(&packet[1], (uint32_t)image_len);
	put_be32(&packet[5], BL_STORAGE_SFLASH);
	put_be32(&packet[9], BL_FILE_META_IMAGE1);
	put_be32(&packet[13], 0U);
	port->flush_rx(port->ctx);
	rc = command(port, packet, 17U, t->erase_ms);
	if (rc != IWRL_OK) {
		return rc;
	}

	while (offset < image_len) {
		size_t chunk = image_len - offset;

		if (chunk > IWRL_BL_CHUNK_SIZE) {
			chunk = IWRL_BL_CHUNK_SIZE;
		}
		packet[0] = BL_OP_SEND_DATA;
		memcpy(&packet[1], &image[offset], chunk);
		rc = command(port, packet, 1U + chunk, t->chunk_ms);
		if (rc != IWRL_OK) {
			return rc;
		}
		offset += chunk;
		if (progress != NULL) {
			progress(offset, image_len, user);
		}
	}

	packet[0] = BL_OP_FILE_CLOSE;
	put_be32(&packet[1], BL_FILE_META_IMAGE1);
	return command(port, packet, 5U, t->erase_ms);
}
