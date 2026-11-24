/*
 * Decode the radar's UART output stream: TI's packet (8-byte magic word,
 * 40-byte header, TLVs) carrying the human presence result in TLV 1101
 * (firmware/src/common/human_detect.h, docs/HUMAN_DETECT.md).
 */
#ifndef IWRL6432_STREAM_H
#define IWRL6432_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IWRL_TLV_HUMAN_DETECT     1101U
/* Packets longer than this (for example with a point cloud enabled) are
 * skipped without being stored. */
#define IWRL_STREAM_MAX_PACKET    256U

struct iwrl_human_result {
	uint32_t frame_number;
	uint8_t  present;       /* 1 = human in range, 0 = nobody */
	uint8_t  cluster_now;   /* undebounced: a cluster exists in this frame */
	uint16_t num_points;    /* points of this frame that passed the gates */
	float    range_m;       /* nearest cluster in metres, 0 when nobody */
};

struct iwrl_stream {
	uint8_t  buffer[IWRL_STREAM_MAX_PACKET];
	uint32_t have;          /* bytes of the current packet stored in buffer */
	uint32_t total;         /* packet length from the header, 0 = not known */
	uint32_t skip;          /* bytes of an oversized packet still to drop */
	uint32_t packets;       /* complete packets seen */
	uint32_t results;       /* packets that carried TLV 1101 */
	uint32_t dropped;       /* malformed or oversized packets */
};

typedef void (*iwrl_stream_result_fn)(const struct iwrl_human_result *result, void *user);

void iwrl_stream_init(struct iwrl_stream *stream);

/* Feed received bytes in any chunking. `on_result` is called once for every
 * packet that contains a valid TLV 1101. */
void iwrl_stream_feed(struct iwrl_stream *stream, const uint8_t *data, size_t len,
		      iwrl_stream_result_fn on_result, void *user);

#ifdef __cplusplus
}
#endif

#endif /* IWRL6432_STREAM_H */
