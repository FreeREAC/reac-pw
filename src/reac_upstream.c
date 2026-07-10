// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Braid layout per norihiro/obs-h8819-source convert_to_pcm24lep (GPL-3.0-or-later),
// confirmed for the S-1608/S-0808 upstream against rig captures (task #108).

#include "reac_upstream.h"

#include <reac/reac.h>

int reac_upstream_channels(size_t len)
{
	if (len < REAC_UPSTREAM_OVERHEAD + 2 * REAC_UPSTREAM_BYTES_PER_CH)
		return -1;
	if ((len - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH != 0)
		return -1;
	size_t nch = (len - REAC_UPSTREAM_OVERHEAD) / REAC_UPSTREAM_BYTES_PER_CH;
	if (nch & 1)               /* the braid carries channel PAIRS */
		return -1;
	if (nch >= REAC_MAX_CHANNELS) /* 40 ch = 1492 B = the downstream broadcast */
		return -1;
	return (int)nch;
}

int reac_upstream_decode(const uint8_t *raw, size_t len, uint8_t *out)
{
	if (!raw || !out)
		return -1;
	int nch = reac_upstream_channels(len);
	if (nch < 0)
		return -1;
	if (raw[12] != 0x88 || raw[13] != 0x19)
		return -1;
	if (raw[len - 2] != REAC_END_MARKER_0 || raw[len - 1] != REAC_END_MARKER_1)
		return -1;

	const uint8_t *audio = raw + REAC_L2_HEADER_LEN;
	const size_t sample_stride = (size_t)nch * REAC_RESOLUTION;

	uint8_t *dptr = out;
	for (int ch = 0; ch < nch; ch++) {
		/* the 6-byte group this channel shares with its pair partner */
		const uint8_t *g = audio + (size_t)(ch & ~1) * REAC_RESOLUTION;
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			if ((ch & 1) == 0) {
				*dptr++ = g[3];  /* lo  */
				*dptr++ = g[0];  /* mid */
				*dptr++ = g[1];  /* hi  */
			} else {
				*dptr++ = g[4];
				*dptr++ = g[5];
				*dptr++ = g[2];
			}
			g += sample_stride;
		}
	}
	return REAC_SAMPLES_PER_PKT;
}
