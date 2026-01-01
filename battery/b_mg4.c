/* */

/*
 * Copyright (c) 2025 David Gwynne <david@gwynne.id.au>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "../compat.h"

#include <stdlib.h>
#include <err.h>
#include <limits.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "../batgw_config.h"
#include "../batgw.h"
#include "../log.h"

uint8_t crc8(const uint8_t *, size_t, uint8_t);

/*
 * hardware details
 */

#define BYD_MIN_CELL_VOLTAGE_MV		2800
#define BYD_MAX_CELL_VOLTAGE_MV		3800
#define BYD_DEV_CELL_VOLTAGE_MV		150

#define BYD_PID_BATTERY_SOC		0x0005
#define BYD_PID_BATTERY_VOLTAGE		0x0008
#define BYD_PID_BATTERY_CURRENT		0x0009
#define BYD_PID_CELL_TEMP_MIN		0x002f
#define BYD_PID_CELL_TEMP_MAX		0x0031
#define BYD_PID_CELL_TEMP_AVG		0x0032
#define BYD_PID_CELL_MV_MIN		0x002b
#define BYD_PID_CELL_MV_MAX		0x002d
#define BYD_PID_MAX_CHARGE_POWER	0x000a
#define BYD_PID_MAX_DISCHARGE_POWER	0x000e
#define BYD_PID_CHARGE_TIMES		0x000b
#define BYD_PID_TOTAL_CHARGED_AH	0x000f
#define BYD_PID_TOTAL_DISCHARGED_AH	0x0010
#define BYD_PID_TOTAL_CHARGED_KWH	0x0011
#define BYD_PID_TOTAL_DISCHARGED_KWH	0x0012

/*
 * glue
 */

static int	 mg4_check(const struct batgw_config_battery *);
static void	 mg4_config(struct batgw_config_battery *);
static void	*mg4_attach(struct batgw *);
static void	 mg4_dispatch(struct batgw *, void *);
static void	 mg4_teleperiod(struct batgw *, void *);

const struct batgw_battery battery_mg4 = {
	.b_check =			mg4_check,
	.b_config =			mg4_config,
	.b_attach =			mg4_attach,
	.b_dispatch =			mg4_dispatch,
	.b_teleperiod =			mg4_teleperiod,
};

/*
 * mg4 software driver
 */

enum mg4_kvs {
	MG4_KV_SOC,
	MG4_KV_VOLTAGE,
	MG4_KV_CURRENT,
	MG4_KV_POWER,

	MG4_KV_COUNT

};

static const struct batgw_kv_tpl mg4_kvs_tpl[MG4_KV_COUNT] = {
	[MG4_KV_SOC] =
		{ "soc",		KV_T_PERCENT,	1 },
	[MG4_KV_VOLTAGE] =
		{ NULL,			KV_T_VOLTAGE,	1 },
	[MG4_KV_CURRENT] =
		{ NULL,			KV_T_CURRENT,	1 },
	[MG4_KV_POWER] =
		{ NULL,			KV_T_POWER,	2 },
};

struct mg4_softc {
	int			 can;
	struct event		*can_recv;

	struct event		*can_keepalive;
	struct event		*can_contactor;
	unsigned int		 can_contactor_idx;

	struct event		*can_poll;
	unsigned int		 can_poll_idx;
	struct event		*can_wdog;

	struct batgw_kv		 kvs[MG4_KV_COUNT];
};

static void	mg4_can_keepalive(int, short, void *);
static void	mg4_can_contactor(int, short, void *);
static void	mg4_can_poll_start(int, short, void *);
static void	mg4_can_poll(int, short, void *);
static void	mg4_can_recv(int, short, void *);
static void	mg4_can_wdog(int, short, void *);

static const struct timeval mg4_200ms = { 0, 199000 };
static const struct timeval mg4_1s = { 1, 0 };
static const struct timeval mg4_wdog_tv = { 10, 0 };
static const struct timeval mg4_keepalive_tv = { 0, 100000 };
static const struct timeval mg4_contactor_tv = { 0,  10000 };

static int
mg4_check(const struct batgw_config_battery *bconf)
{
	int rv = 0;

	if (bconf->ifname == NULL) {
		fprintf(stderr, "%s: interface not configured\n",
		    bconf->protocol);
		rv = -1;
	}

	if (bconf->min_cell_voltage_mv != 0) {
		fprintf(stderr, "%s: min cell voltage is configured\n",
		    bconf->protocol);
		rv = -1;
	}
	if (bconf->max_cell_voltage_mv != 0) {
		fprintf(stderr, "%s: max cell voltage is configured\n",
		    bconf->protocol);
		rv = -1;
	}
	if (bconf->dev_cell_voltage_mv != 0) {
		fprintf(stderr, "%s: "
		    "cell voltage deviation is configured\n",
		    bconf->protocol);
		rv = -1;
	}

	return (rv);
}

static void
mg4_config(struct batgw_config_battery *bconf)
{
	/* XXX this is too magical */

	bconf->rated_capacity_ah = 156;
	bconf->rated_voltage_dv = 3270;

	bconf->min_cell_voltage_mv = BYD_MIN_CELL_VOLTAGE_MV;
	bconf->max_cell_voltage_mv = BYD_MAX_CELL_VOLTAGE_MV;
	bconf->dev_cell_voltage_mv = BYD_DEV_CELL_VOLTAGE_MV;
}

static void *
mg4_attach(struct batgw *bg)
{
	const struct batgw_config_battery *bconf = batgw_b_config(bg);
	struct mg4_softc *sc;
	int fd;
	unsigned int i;

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		err(1, "%s alloc", __func__);

	fd = can_open("mg4", bconf->ifname);

	sc->can = fd;

	sc->can_recv = event_new(batgw_event_base(bg), fd, EV_READ|EV_PERSIST,
	    mg4_can_recv, bg);
	if (sc->can_recv == NULL)
		errx(1, "new mg4 can recv event failed");

	sc->can_keepalive = event_new(batgw_event_base(bg), 0, EV_PERSIST,
	    mg4_can_keepalive, bg);
	if (sc->can_keepalive == NULL)
		errx(1, "new mg4 keepalive event failed");

	sc->can_contactor = event_new(batgw_event_base(bg), 0, EV_PERSIST,
	    mg4_can_contactor, bg);
	if (sc->can_contactor == NULL)
		errx(1, "new mg4 contactor event failed");

//	sc->can_poll = evtimer_new(batgw_event_base(bg),
//	    mg4_can_poll, bg);
//	if (sc->can_poll == NULL)
//		errx(1, "new mg4 can poll event failed");

	sc->can_wdog = evtimer_new(batgw_event_base(bg),
	    mg4_can_wdog, bg);
	if (sc->can_wdog == NULL)
		errx(1, "new mg4 can wdog event failed");

	for (i = 0; i < nitems(sc->kvs); i++)
		batgw_kv_init_tpl(&sc->kvs[i], &mg4_kvs_tpl[i]);

	return (sc);
}

static void
mg4_dispatch(struct batgw *bg, void *arg)
{
	const struct batgw_config_battery *bconf = batgw_b_config(bg);
	struct mg4_softc *sc = arg;

	batgw_b_set_rated_capacity_ah(bg, bconf->rated_capacity_ah);
	batgw_b_set_rated_voltage_dv(bg, bconf->rated_voltage_dv);

	batgw_b_set_min_voltage_dv(bg, 2600 + 200);
	batgw_b_set_max_voltage_dv(bg, 3790 - 200);

	batgw_b_set_charge_w(bg, 5000);
	batgw_b_set_discharge_w(bg, 5000);

	batgw_b_set_min_temp_dc(bg, 290);
	batgw_b_set_max_temp_dc(bg, 310);
	batgw_b_set_avg_temp_dc(bg, 300); /* 30.0 degC */

	batgw_b_set_min_cell_voltage_mv(bg, 2999);
	batgw_b_set_max_cell_voltage_mv(bg, 3001);

	event_add(sc->can_recv, NULL);

#if 1
	evtimer_add(sc->can_keepalive, &mg4_keepalive_tv);
	evtimer_add(sc->can_contactor, &mg4_contactor_tv);

	mg4_can_keepalive(0, 0, bg);
	mg4_can_contactor(0, 0, bg);
#endif
//	mg4_can_poll_start(0, 0, bg);
}

static void
mg4_teleperiod(struct batgw *bg, void *arg)
{
	struct mg4_softc *sc = arg;
	const struct batgw_kv *kv;
	unsigned int i;

	for (i = 0; i < nitems(sc->kvs); i++) {
		kv = &sc->kvs[i];
		if (kv->kv_v == INT_MIN)
			continue;

		batgw_kv_publish(bg, "battery", kv);
	}
}

static void
mg4_can_keepalive(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mg4_softc *sc = batgw_b_softc(bg);
	static const struct can_frame frame = {
		.can_id = 0x4f3,
		.len = 8,
		.data = { 0xf3, 0x10, 0x48, 0x00, 0xff, 0xff, 0x00, 0x11 },
	};
	ssize_t rv;

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1)
		lwarn("mg4 keepalive");
}


static const uint64_t contactor_seq[] = {
	0x8100457D7FFEFFFE,
	0xDC01457D7FFEFFFE,
	0xB402457D7FFFFFFE,
	0xE903457D7FFFFFFE,
	0xE804457D7FFEFFFE,
	0xB505457D7FFEFFFE,
	0xDD06457D7FFFFFFE,
	0x0F07457D7FFEFFFE,
	0x5308457D7FFEFFFE,
	0x8109457D7FFFFFFE,
	0x660A457D7FFFFFFE,
	0xB40B457D7FFEFFFE,
	0x3A0C457D7FFEFFFE,
	0x0F0E457D7FFFFFFE,
};

static void
mg4_can_contactor(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mg4_softc *sc = batgw_b_softc(bg);
	struct can_frame frame = {
		.can_id = 0x047,
		.len = 8,
	};
	unsigned int idx;
	ssize_t rv;

	idx = sc->can_contactor_idx;
	can_htobe64(&frame, contactor_seq[idx]);
	if (++idx >= nitems(contactor_seq))
		idx = 0;
	sc->can_contactor_idx = idx;

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1)
		lwarn("mg4 contactor");
}

static const uint16_t mg4_poll_pids[] = {
	0xb046,
	0xb048,
	0xb056,
};

static void
mg4_can_poll_start(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mg4_softc *sc = batgw_b_softc(bg);

	unsigned int idx = sc->can_poll_idx;
	uint16_t pid;
	struct can_frame frame = {
		.can_id = 0x781,
		.len = 8,
		//.data = { 0x03, 0x19, 0x02, 0xff, 0x00, 0x00, 0x00, 0x00 },
		.data = { 0x04, 0x14, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00 },
	};
	ssize_t rv;

	evtimer_add(sc->can_poll, &mg4_200ms);

	if (0) {
		size_t i;

		printf("tx 0x%03x [%u]", frame.can_id, frame.len);
		for (i = 0; i < frame.len; i++) {
			printf(" %02x", frame.data[i]);
		}
		printf("\n");
	}

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1) {
		lwarn("mg4 can poll");
		return;
	}
}

static void
mg4_can_poll(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mg4_softc *sc = batgw_b_softc(bg);

	unsigned int idx = sc->can_poll_idx;
	uint16_t pid;
	struct can_frame frame = {
		.can_id = 0x781,
		.len = 8,
		.data = { 0x03, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	};
	ssize_t rv;

	idx = sc->can_poll_idx;
	pid = mg4_poll_pids[idx];
	if (++idx >= nitems(mg4_poll_pids))
		idx = 0;
	sc->can_poll_idx = idx;

	evtimer_add(sc->can_poll, &mg4_200ms);

	can_htobe16(&frame, 2, pid);

	if (0) {
		size_t i;

		printf("rx 0x%03x [%u]", frame.can_id, frame.len);
		for (i = 0; i < frame.len; i++) {
			printf(" %02x", frame.data[i]);
		}
		printf("\n");
	}

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1) {
		lwarn("mg4 can poll");
		return;
	}
}

static void
mg4_can_wdog(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	batgw_b_set_stopped(bg);
}

static void
mg4_can_recv(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mg4_softc *sc = batgw_b_softc(bg);
	struct can_frame frame;
	ssize_t rv;
	size_t i;
	unsigned int uv;
	int sv;
	unsigned int k;

	rv = recv(fd, &frame, sizeof(frame), 0);
	if (rv == -1) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			break;
		default:
			lwarn("mg4 can recv");
			break;
		}
		return;
	}

	if (frame.len != 8) {
		/* this is unexpected */
		return;
	}

	if (frame.can_id == 0x12c) {
		batgw_b_set_running(bg);
		evtimer_add(sc->can_wdog, &mg4_wdog_tv);
	}

	if ((frame.can_id & 0xf00) == 0x700 || batgw_verbose(bg) > 1) {
		printf("rx 0x%03x [%u]", frame.can_id, frame.len);
		for (i = 0; i < frame.len; i++) {
			printf(" %02x", frame.data[i]);
		}
		printf("\n");
	}

	switch (frame.can_id) {
	case 0x12c:
		/* current */
		sv = can_betoh16(&frame, 2);
		sv -= 20000;
		sv /= 2;

		batgw_b_set_current_da(bg, 0 - sv);
		batgw_kv_update(bg, "battery",
                    &sc->kvs[MG4_KV_CURRENT], sv);

		/* voltage */
		uv = can_betoh16(&frame, 4);
		uv *= 5;
		uv >>= 5;

		batgw_b_set_voltage_dv(bg, uv);
		batgw_kv_update(bg, "battery",
                    &sc->kvs[MG4_KV_VOLTAGE], uv);

		/* power */
		batgw_kv_update(bg, "battery",
                    &sc->kvs[MG4_KV_POWER], uv * sv);
	
		break;

	case 0x401:
		if (frame.data[2] & 0x1)
			break;

		uv = can_betoh16(&frame, 6) & 0x3ff;

		batgw_b_set_soc_c_pct(bg, uv * 10);
		batgw_kv_update(bg, "battery",
                    &sc->kvs[MG4_KV_SOC], uv);
		break;

	case 0x7ed:
		printf("ermagerd\n");

		switch (can_betoh16(&frame, 2)) {
		case 0xb046:
			printf("raw soc\n");
			break;
		}
		break;
	}
}

static const uint8_t crc8_table[256] = {
	0x00, 0x1D, 0x3A, 0x27, 0x74, 0x69, 0x4E, 0x53,
	0xE8, 0xF5, 0xD2, 0xCF, 0x9C, 0x81, 0xA6, 0xBB,
	0xCD, 0xD0, 0xF7, 0xEA, 0xB9, 0xA4, 0x83, 0x9E,
	0x25, 0x38, 0x1F, 0x02, 0x51, 0x4C, 0x6B, 0x76,
	0x87, 0x9A, 0xBD, 0xA0, 0xF3, 0xEE, 0xC9, 0xD4,
	0x6F, 0x72, 0x55, 0x48, 0x1B, 0x06, 0x21, 0x3C,
	0x4A, 0x57, 0x70, 0x6D, 0x3E, 0x23, 0x04, 0x19,
	0xA2, 0xBF, 0x98, 0x85, 0xD6, 0xCB, 0xEC, 0xF1,
	0x13, 0x0E, 0x29, 0x34, 0x67, 0x7A, 0x5D, 0x40,
	0xFB, 0xE6, 0xC1, 0xDC, 0x8F, 0x92, 0xB5, 0xA8,
	0xDE, 0xC3, 0xE4, 0xF9, 0xAA, 0xB7, 0x90, 0x8D,
	0x36, 0x2B, 0x0C, 0x11, 0x42, 0x5F, 0x78, 0x65,
	0x94, 0x89, 0xAE, 0xB3, 0xE0, 0xFD, 0xDA, 0xC7,
	0x7C, 0x61, 0x46, 0x5B, 0x08, 0x15, 0x32, 0x2F,
	0x59, 0x44, 0x63, 0x7E, 0x2D, 0x30, 0x17, 0x0A,
	0xB1, 0xAC, 0x8B, 0x96, 0xC5, 0xD8, 0xFF, 0xE2,
	0x26, 0x3B, 0x1C, 0x01, 0x52, 0x4F, 0x68, 0x75,
	0xCE, 0xD3, 0xF4, 0xE9, 0xBA, 0xA7, 0x80, 0x9D,
	0xEB, 0xF6, 0xD1, 0xCC, 0x9F, 0x82, 0xA5, 0xB8,
	0x03, 0x1E, 0x39, 0x24, 0x77, 0x6A, 0x4D, 0x50,
	0xA1, 0xBC, 0x9B, 0x86, 0xD5, 0xC8, 0xEF, 0xF2,
	0x49, 0x54, 0x73, 0x6E, 0x3D, 0x20, 0x07, 0x1A,
	0x6C, 0x71, 0x56, 0x4B, 0x18, 0x05, 0x22, 0x3F,
	0x84, 0x99, 0xBE, 0xA3, 0xF0, 0xED, 0xCA, 0xD7,
	0x35, 0x28, 0x0F, 0x12, 0x41, 0x5C, 0x7B, 0x66,
	0xDD, 0xC0, 0xE7, 0xFA, 0xA9, 0xB4, 0x93, 0x8E,
	0xF8, 0xE5, 0xC2, 0xDF, 0x8C, 0x91, 0xB6, 0xAB,
	0x10, 0x0D, 0x2A, 0x37, 0x64, 0x79, 0x5E, 0x43,
	0xB2, 0xAF, 0x88, 0x95, 0xC6, 0xDB, 0xFC, 0xE1,
	0x5A, 0x47, 0x60, 0x7D, 0x2E, 0x33, 0x14, 0x09,
	0x7F, 0x62, 0x45, 0x58, 0x0B, 0x16, 0x31, 0x2C,
	0x97, 0x8A, 0xAD, 0xB0, 0xE3, 0xFE, 0xD9, 0xC4,
};

uint8_t 
crc8(const uint8_t *data, size_t len, uint8_t crc)
{
	size_t i;

	for (i = 0; i < len; i++)
		crc = crc8_table[crc ^ data[i]];

	return (crc);
}
