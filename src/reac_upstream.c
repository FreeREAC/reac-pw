// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Braid layout per norihiro/obs-h8819-source convert_to_pcm24lep (GPL-3.0-or-later),
// confirmed for the S-1608/S-0808 upstream against rig captures (task #108).

#include "reac_upstream.h"

#include <reac/reac.h>

int reac_upstream_channels(size_t len)
{
	/* OHRCA path (M-5000/M-480, and the S-4000S — captured on OHRCA) appends a 2-byte
	 * CRC-16 trailer AFTER the C2 EA end marker: the upstream analogue of the downstream
	 * 1492->1494 (+2) already handled in reac_rx.c's gate. Strip it so the box-width math
	 * below sees the clean frame; without this the S-4000's 1206 B (52 + 32*36 + 2) fails
	 * the %36 check, the RX gate rejects every frame, and reac-capture is silent. */
	if (len >= REAC_UPSTREAM_OVERHEAD &&
	    (len - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH == 2)
		len -= 2;
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
	/* End-marker check against the CLEAN frame length (excludes any OHRCA +2 CRC trailer);
	 * the audio region [50 : 50+nch*36] the loop below reads is unaffected by the trailer. */
	size_t clean_len = REAC_UPSTREAM_OVERHEAD + (size_t)nch * REAC_UPSTREAM_BYTES_PER_CH;
	if (raw[clean_len - 2] != REAC_END_MARKER_0 || raw[clean_len - 1] != REAC_END_MARKER_1)
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
