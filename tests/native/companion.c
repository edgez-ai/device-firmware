/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_protocol.h"
#include "prov_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct companion_identity identity = {0x5037cdf35bb6ULL, "Test", "EZ-DEV54-CAM/test", "camera", 0};
int main(void)
{
	/* The timeout that races connection establishment must leave the live
	 * connection awake. A close already committed must not be reopened. */
	assert(prov_session_hold_awake(true, false, false));
	assert(!prov_session_hold_awake(false, false, false));
	assert(!prov_session_hold_awake(true, true, false));
	assert(!prov_session_hold_awake(true, false, true));
	assert(prov_session_can_claim(true, false, true, true, false));
	assert(!prov_session_can_claim(true, true, true, true, false));
	assert(!prov_session_can_claim(true, false, false, true, false));
	assert(!prov_session_can_claim(true, false, true, true, true));
	assert(!prov_session_can_claim(true, false, true, false, false));
	assert(!prov_session_can_claim(false, false, true, true, false));
	assert(prov_session_provisioning_open(true, false, false));
	assert(!prov_session_provisioning_open(true, false, true));
	assert(!prov_session_provisioning_open(true, true, false));
	assert(!prov_session_provisioning_open(false, false, false));
	uint8_t output[512];
	const uint8_t init[] = {0x18, 1, 0x20, 5, 0xba, 6, 0};
	int size = companion_protocol_exchange(&identity, init, sizeof(init), output, sizeof(output));
	assert(size > 0);
	/* Protobuf response: from / to / RESPONSE / BLE / status field 102. */
	assert(output[0] == 8);
	bool status = false;
	for (int i = 0; i < size - 1; ++i) if (output[i] == 0xb2 && output[i + 1] == 6) status = true;
	assert(status);
	assert(companion_protocol_exchange(&identity, init, sizeof(init), output, 3) == -ENOSPC);
	const uint8_t get[] = {0x18, 1, 0xc2, 6, 2, 8, 1};
	assert(companion_protocol_exchange(&identity, get, sizeof(get), output, sizeof(output)) > 0);
	const uint8_t set[] = {0x18, 1, 0xc2, 6, 2, 8, 2};
	assert(companion_protocol_exchange(&identity, set, sizeof(set), output, sizeof(output)) == -EACCES);
	const uint8_t unknown[] = {0x18, 1, 0xca, 6, 0};
	assert(companion_protocol_exchange(&identity, unknown, sizeof(unknown), output, sizeof(output)) == -ENOTSUP);
	const uint8_t wrong_target[] = {0x10, 1, 0x18, 1, 0xba, 6, 0};
	assert(companion_protocol_exchange(&identity, wrong_target, sizeof(wrong_target), output, sizeof(output)) == -EACCES);
	const uint8_t duplicate_body[] = {0x18, 1, 0xba, 6, 0, 0xc2, 6, 2, 8, 1};
	assert(companion_protocol_exchange(&identity, duplicate_body, sizeof(duplicate_body), output, sizeof(output)) == -EBADMSG);
	/* Every truncated nested message/varint is rejected; no out-of-bounds
	 * access is permitted even with an exact-size or zero-sized output. */
	for (size_t i = 0; i < sizeof(get); ++i) assert(companion_protocol_exchange(&identity, get, i, output, sizeof(output)) < 0);
	uint8_t fuzz[512]; unsigned random = 17;
	for (unsigned i = 0; i < 10000; ++i) {
		size_t length = i % sizeof(fuzz) + 1;
		for (size_t j = 0; j < length; ++j) { random = random * 1664525U + 1013904223U; fuzz[j] = random >> 24; }
		int result = companion_protocol_exchange(&identity, fuzz, length, output, i % sizeof(output));
		assert(result < 0 || (size_t)result <= i % sizeof(output));
	}
	puts("Companion lifecycle, protobuf bounds, and write rejection passed");
	return 0;
}
