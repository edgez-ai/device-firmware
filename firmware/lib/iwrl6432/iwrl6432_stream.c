/* IWRL6432 UART output stream decoder. See iwrl6432_stream.h. */
#include <string.h>

#include "iwrl6432_stream.h"

#define HEADER_SIZE          40U
#define TLV_HEADER_SIZE      8U
#define HUMAN_RESULT_SIZE    8U
#define OFFSET_TOTAL_LEN     12U
#define OFFSET_FRAME_NUMBER  20U
#define OFFSET_NUM_TLVS      32U

static const uint8_t magic[8] = { 0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07 };

static uint32_t get_le32(const uint8_t *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
	       ((uint32_t)in[3] << 24);
}

static void parse_packet(struct iwrl_stream *stream, iwrl_stream_result_fn on_result,
			 void *user)
{
	const uint8_t *packet = stream->buffer;
	uint32_t num_tlvs = get_le32(&packet[OFFSET_NUM_TLVS]);
	uint32_t offset = HEADER_SIZE;
	uint32_t index;

	stream->packets++;
	for (index = 0U; index < num_tlvs; index++) {
		uint32_t type;
		uint32_t length;

		if ((offset + TLV_HEADER_SIZE) > stream->total) {
			stream->dropped++;
			return;
		}
		type = get_le32(&packet[offset]);
		length = get_le32(&packet[offset + 4U]);
		offset += TLV_HEADER_SIZE;
		if ((length > stream->total) || ((offset + length) > stream->total)) {
			stream->dropped++;
			return;
		}
		if ((type == IWRL_TLV_HUMAN_DETECT) && (length == HUMAN_RESULT_SIZE)) {
			struct iwrl_human_result result;

			result.frame_number = get_le32(&packet[OFFSET_FRAME_NUMBER]);
			result.present = packet[offset];
			result.cluster_now = packet[offset + 1U];
			result.num_points = (uint16_t)(packet[offset + 2U] |
						       ((uint16_t)packet[offset + 3U] << 8));
			/* IEEE-754 little-endian, same as the Cortex-M host. */
			memcpy(&result.range_m, &packet[offset + 4U], sizeof(result.range_m));
			stream->results++;
			if (on_result != NULL) {
				on_result(&result, user);
			}
			return;
		}
		offset += length;
	}
}

void iwrl_stream_init(struct iwrl_stream *stream)
{
	memset(stream, 0, sizeof(*stream));
}

void iwrl_stream_feed(struct iwrl_stream *stream, const uint8_t *data, size_t len,
		      iwrl_stream_result_fn on_result, void *user)
{
	size_t i;

	for (i = 0U; i < len; i++) {
		uint8_t byte = data[i];

		if (stream->skip > 0U) {
			stream->skip--;
			continue;
		}

		if (stream->have < sizeof(magic)) {
			/* Hunting for the magic word, tolerating CLI text before it. */
			if (byte == magic[stream->have]) {
				stream->buffer[stream->have++] = byte;
			} else {
				stream->have = (byte == magic[0]) ? 1U : 0U;
				stream->buffer[0] = magic[0];
			}
			continue;
		}

		stream->buffer[stream->have++] = byte;
		if (stream->have == HEADER_SIZE) {
			stream->total = get_le32(&stream->buffer[OFFSET_TOTAL_LEN]);
			if (stream->total < HEADER_SIZE) {
				stream->dropped++;
				stream->have = 0U;
				stream->total = 0U;
			} else if (stream->total > IWRL_STREAM_MAX_PACKET) {
				/* Not ours to decode: drop the rest of it. A corrupt
				 * length is bounded so the stream recovers. */
				stream->dropped++;
				stream->skip = stream->total - HEADER_SIZE;
				if (stream->skip > 65536U) {
					stream->skip = 0U;
				}
				stream->have = 0U;
				stream->total = 0U;
			}
		}
		if ((stream->total != 0U) && (stream->have == stream->total)) {
			parse_packet(stream, on_result, user);
			stream->have = 0U;
			stream->total = 0U;
		}
	}
}
