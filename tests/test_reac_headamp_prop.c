// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_prop — parse LIVE per-channel head-amp changes out of a PipeWire
 * SPA_PARAM_Props object (task #203). Pure: builds Props pods with the SPA pod
 * builder and asserts the parse, so no live graph is needed. Proves the control
 * carrier (SPA_PROP_params, keyed "reac.headamp.<ch>.<param>") is decoded, that
 * junk/out-of-range entries are skipped without desyncing the (key,value)
 * pairing, and that the three value encodings (Bool/Int/Float) all land. */
#include "reac_headamp_prop.h"
#include "reac_ctrl.h"     /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */

#include <reac/reac.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* Open a Props object with a SPA_PROP_params struct ready for (key,value) pairs. */
static void begin_props(struct spa_pod_builder *b, struct spa_pod_frame *obj,
                        struct spa_pod_frame *st)
{
	spa_pod_builder_push_object(b, obj, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, st);
}

static const struct spa_pod *end_props(struct spa_pod_builder *b,
                                       struct spa_pod_frame *obj,
                                       struct spa_pod_frame *st)
{
	spa_pod_builder_pop(b, st);
	return (const struct spa_pod *)spa_pod_builder_pop(b, obj);
}

static void kv_int(struct spa_pod_builder *b, const char *k, int v)
{
	spa_pod_builder_string(b, k);
	spa_pod_builder_int(b, v);
}

int main(void)
{
	uint8_t buf[2048];
	struct reac_headamp_setting out[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];

	/* 1. A well-formed mix: three valid head-amp entries interleaved with an
	 * unrelated params key, an out-of-range channel, and an out-of-range value.
	 * Only the three valid ones survive, in order. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		kv_int(&b, "reac.headamp.5.phantom", 1);
		kv_int(&b, "reac.headamp.5.sens", 0x10);
		spa_pod_builder_string(&b, "reac.headamp.6.pad");
		spa_pod_builder_bool(&b, true);                       /* Bool encoding */
		kv_int(&b, "some.other.control", 99);                 /* not head-amp */
		/* ch out of range. The bound is the head-amp WIRE-channel space
		 * (0x00..0x2f), NOT libreac's 40 audio slots — ch 40 is a REAL channel
		 * (an S-1608 based at 0x20 owns 0x20..0x2f), so 48 is the first invalid
		 * one. This previously read 40 and so pinned the too-narrow bound. */
		kv_int(&b, "reac.headamp.48.phantom", 1);
		kv_int(&b, "reac.headamp.7.sens", 0x38);              /* value > SENS_MAX */
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int n = reac_headamp_prop_parse(pod, out, (int)(sizeof out / sizeof out[0]));
		CHK(n == 3);
		CHK(out[0].ch == 5 && out[0].param == REAC_HEADAMP_PHANTOM && out[0].value == 1);
		CHK(out[1].ch == 5 && out[1].param == REAC_HEADAMP_SENS && out[1].value == 0x10);
		CHK(out[2].ch == 6 && out[2].param == REAC_HEADAMP_PAD && out[2].value == 1);
	}

	/* 2. Value encodings: a Float slider rounds to the nearest sens step. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, "reac.headamp.8.sens");
		spa_pod_builder_float(&b, 12.4f);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int n = reac_headamp_prop_parse(pod, out, (int)(sizeof out / sizeof out[0]));
		CHK(n == 1);
		CHK(out[0].ch == 8 && out[0].param == REAC_HEADAMP_SENS && out[0].value == 12);
	}

	/* 3. A Props object with NO params entry (a plain volume set) yields 0 — the
	 * volume path is untouched by head-amp parsing. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj;
		spa_pod_builder_push_object(&b, &obj, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
		spa_pod_builder_float(&b, 0.5f);
		const struct spa_pod *pod = (const struct spa_pod *)spa_pod_builder_pop(&b, &obj);

		CHK(reac_headamp_prop_parse(pod, out, (int)(sizeof out / sizeof out[0])) == 0);
	}

	/* 4. NULL / wrong pod type -> -1 (not an accepted Props object). */
	CHK(reac_headamp_prop_parse(NULL, out, 8) == -1);

	/* 5. A malformed non-string in a KEY slot must not desync the pairing: the
	 * parser skips it and re-syncs on the next String key, so the trailing valid
	 * pair is still decoded. Here an Int sits where a key should, then a real
	 * (key,value) pair follows. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_int(&b, 1234);                         /* stray non-string key */
		kv_int(&b, "reac.headamp.3.phantom", 1);               /* still decoded */
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int n = reac_headamp_prop_parse(pod, out, (int)(sizeof out / sizeof out[0]));
		CHK(n == 1);
		CHK(out[0].ch == 3 && out[0].param == REAC_HEADAMP_PHANTOM && out[0].value == 1);
	}

	/* 6. The `max` cap is honoured: ask for at most 1, hand it 2 valid entries. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		kv_int(&b, "reac.headamp.1.phantom", 1);
		kv_int(&b, "reac.headamp.2.phantom", 1);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int n = reac_headamp_prop_parse(pod, out, 1);
		CHK(n == 1);
		CHK(out[0].ch == 1);
	}

	printf("OK: head-amp prop parse — SPA_PROP_params keys, encodings, range gate, resync\n");
	return 0;
}
