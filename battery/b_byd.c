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
 * bytes 6 and 7 in the 50ms message decrement the top nibble by 1.
 * the low nibble stays the same.
 */
#define BYD_50MS_6_INITIALIZER		0xbf
#define BYD_50MS_7_INITIALIZER		0x59
#define BYD_50MS_DECR			0x10

static const struct timeval byd_50ms_change_tv = { 1, 150000 };

/*
 * glue
 */

static int	 byd_b_check(const struct batgw_config_battery *);
static void	 byd_b_config(struct batgw_config_battery *);
static void	*byd_b_attach(struct batgw *);
static void	 byd_b_dispatch(struct batgw *, void *);
static void	 byd_b_teleperiod(struct batgw *, void *);

const struct batgw_battery battery_byd = {
	.b_check =			byd_b_check,
	.b_config =			byd_b_config,
	.b_attach =			byd_b_attach,
	.b_dispatch =			byd_b_dispatch,
	.b_teleperiod =			byd_b_teleperiod,
};

/*
 * byd software driver
 */

enum byd_kvs {
	BYD_KV_AMBIENT,
	BYD_KV_VOLTAGE,
	BYD_KV_SOC,

	BYD_KV_PID_SOC,
	BYD_KV_PID_VOLTAGE,
	BYD_KV_PID_CURRENT,
	BYD_KV_PID_TEMP_MIN,
	BYD_KV_PID_TEMP_MAX,
	BYD_KV_PID_TEMP_AVG,
	BYD_KV_PID_MV_MIN,
	BYD_KV_PID_MV_MAX,
	BYD_KV_PID_MV_DELTA,
	BYD_KV_PID_DISCHARGE_POWER,
	BYD_KV_PID_CHARGE_POWER,
	BYD_KV_PID_CHARGE_COUNT,
	BYD_KV_PID_CHARGED_AH,
	BYD_KV_PID_DISCHARGED_AH,
	BYD_KV_PID_CHARGED_KWH,
	BYD_KV_PID_DISCHARGED_KWH,

	BYD_KV_COUNT

};

static const struct batgw_kv_tpl byd_kvs_tpl[BYD_KV_COUNT] = {
	[BYD_KV_AMBIENT] =
		{ "ambient",		KV_T_TEMP,	0 },
	[BYD_KV_VOLTAGE] =
		{ NULL,			KV_T_VOLTAGE,	0 },
	[BYD_KV_SOC] =
		{ "soc",		KV_T_PERCENT,	1 },

	[BYD_KV_PID_SOC] =
		{ "pid-soc",		KV_T_PERCENT,	0 },
	[BYD_KV_PID_VOLTAGE] =
		{ "pid",		KV_T_VOLTAGE,	0 },
	[BYD_KV_PID_CURRENT] =
		{ "pid",		KV_T_CURRENT,	1 },
	[BYD_KV_PID_TEMP_MIN] =
		{ "min",		KV_T_TEMP,	0 },
	[BYD_KV_PID_TEMP_MAX] =
		{ "max",		KV_T_TEMP,	0 },
	[BYD_KV_PID_TEMP_AVG] =
		{ "avg",		KV_T_TEMP,	0 },
	[BYD_KV_PID_MV_MIN] =
		{ "cell-min",		KV_T_VOLTAGE,	3 },
	[BYD_KV_PID_MV_MAX] =
		{ "cell-max",		KV_T_VOLTAGE,	3 },
	[BYD_KV_PID_MV_DELTA] =
		{ "cell-delta",		KV_T_VOLTAGE,	3 },
	[BYD_KV_PID_DISCHARGE_POWER] =
		{ "max-discharge",	KV_T_POWER,	0 },
	[BYD_KV_PID_CHARGE_POWER] =
		{ "max-charge",		KV_T_POWER,	0 },
	[BYD_KV_PID_CHARGE_COUNT] =
		{ "charge-count",	KV_T_COUNT,	0 },
	[BYD_KV_PID_CHARGED_AH] =
		{ "charged",		KV_T_AMPHOUR,	0 },
	[BYD_KV_PID_DISCHARGED_AH] =
		{ "discharged",		KV_T_AMPHOUR,	0 },
	[BYD_KV_PID_CHARGED_KWH] =
		{ "charged",		KV_T_ENERGY,	0 },
	[BYD_KV_PID_DISCHARGED_KWH] =
		{ "discharged",		KV_T_ENERGY,	0 },
};

struct byd_softc {
	int			 can;
	struct event		*can_recv;

	uint8_t			 can_50ms_6;
	uint8_t			 can_50ms_7;
	struct event		*can_50ms;
	struct event		*can_50ms_change;

	struct event		*can_100ms;

	struct event		*can_poll;
	unsigned int		 can_poll_idx;
	struct event		*can_wdog;

	struct batgw_kv		 kvs[BYD_KV_COUNT];
	struct batgw_kv		 pack[10];

	struct batgw_kv		*cell;
	unsigned int		 ncell;
};

static void	byd_can_50ms(int, short, void *);
static void	byd_can_50ms_change(int, short, void *);
static void	byd_can_100ms(int, short, void *);
static void	byd_can_poll(int, short, void *);
static void	byd_can_recv(int, short, void *);
static void	byd_can_wdog(int, short, void *);

static const struct timeval byd_50ms = { 0, 50000 };
static const struct timeval byd_100ms = { 0, 100000 };
static const struct timeval byd_200ms = { 0, 200000 };
static const struct timeval byd_wdog_tv = { 10, 0 };

static int
byd_b_check(const struct batgw_config_battery *bconf)
{
	int rv = 0;

	if (bconf->ifname == NULL) {
		fprintf(stderr, "%s battery: interface not configured\n",
		    bconf->protocol);
		rv = -1;
	}

	if (bconf->min_cell_voltage_mv != 0) {
		fprintf(stderr, "%s battery: min cell voltage is configured\n",
		    bconf->protocol);
		rv = -1;
	}
	if (bconf->max_cell_voltage_mv != 0) {
		fprintf(stderr, "%s battery: max cell voltage is configured\n",
		    bconf->protocol);
		rv = -1;
	}
	if (bconf->dev_cell_voltage_mv != 0) {
		fprintf(stderr, "%s battery: "
		    "cell voltage deviation is configured\n",
		    bconf->protocol);
		rv = -1;
	}

	return (rv);
}

static void
byd_b_config(struct batgw_config_battery *bconf)
{
	/* XXX this is too magical */

	bconf->rated_capacity_ah = 150;
	bconf->rated_voltage_dv = 4032;

	bconf->ncells = 126;

	bconf->min_cell_voltage_mv = BYD_MIN_CELL_VOLTAGE_MV;
	bconf->max_cell_voltage_mv = BYD_MAX_CELL_VOLTAGE_MV;
	bconf->dev_cell_voltage_mv = BYD_DEV_CELL_VOLTAGE_MV;
}

static void *
byd_b_attach(struct batgw *bg)
{
	const struct batgw_config_battery *bconf = batgw_b_config(bg);
	struct byd_softc *sc;
	int fd;
	unsigned int i;
	char key[16];

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		err(1, "%s alloc", __func__);

	fd = can_open("byd battery", bconf->ifname);

	sc->can = fd;

	sc->can_recv = event_new(batgw_event_base(bg), fd, EV_READ|EV_PERSIST,
	    byd_can_recv, bg);
	if (sc->can_recv == NULL)
		errx(1, "new byd battery can recv event failed");

	sc->can_50ms_6 = BYD_50MS_6_INITIALIZER;
	sc->can_50ms_7 = BYD_50MS_7_INITIALIZER;
	sc->can_50ms = evtimer_new(batgw_event_base(bg),
	    byd_can_50ms, bg);
	if (sc->can_50ms == NULL)
		errx(1, "new byd battery can 50ms event failed");
	sc->can_50ms_change = evtimer_new(batgw_event_base(bg),
	    byd_can_50ms_change, bg);
	if (sc->can_50ms == NULL)
		errx(1, "new byd battery can 50ms event failed");

	sc->can_100ms = evtimer_new(batgw_event_base(bg),
	    byd_can_100ms, bg);
	if (sc->can_100ms == NULL)
		errx(1, "new byd battery can 100ms event failed");

	sc->can_poll = evtimer_new(batgw_event_base(bg),
	    byd_can_poll, bg);
	if (sc->can_poll == NULL)
		errx(1, "new byd battery can poll event failed");

	sc->can_wdog = evtimer_new(batgw_event_base(bg),
	    byd_can_wdog, bg);
	if (sc->can_wdog == NULL)
		errx(1, "new byd battery can wdog event failed");

	for (i = 0; i < nitems(sc->kvs); i++)
		batgw_kv_init_tpl(&sc->kvs[i], &byd_kvs_tpl[i]);

	for (i = 0; i < nitems(sc->pack); i++) {
		snprintf(key, sizeof(key), "pack%u", i); /* XXX rv */
		batgw_kv_init(&sc->pack[i], key, KV_T_TEMP, 0);
	}

	sc->cell = calloc(bconf->ncells, sizeof(*sc->cell));
	if (sc->cell == NULL)
		err(1, "%u cell alloc", bconf->ncells);
	for (i = 0; i < bconf->ncells; i++) {
		snprintf(key, sizeof(key), "cell%u", i); /* XXX rv */
		batgw_kv_init(&sc->cell[i], key, KV_T_VOLTAGE, 3);
	}
	sc->ncell = bconf->ncells;

	return (sc);
}

static void
byd_b_dispatch(struct batgw *bg, void *arg)
{
	const struct batgw_config_battery *bconf = batgw_b_config(bg);
	struct byd_softc *sc = arg;

	batgw_b_set_rated_capacity_ah(bg, bconf->rated_capacity_ah);
	batgw_b_set_rated_voltage_dv(bg, bconf->rated_voltage_dv);

	batgw_b_set_min_voltage_dv(bg, 3800);
	batgw_b_set_max_voltage_dv(bg, 4410);

	event_add(sc->can_recv, NULL);
	byd_can_50ms(0, 0, bg);
	evtimer_add(sc->can_50ms_change, &byd_50ms_change_tv);
	byd_can_100ms(0, 0, bg);
	byd_can_poll(0, 0, bg);
}

static void
byd_b_teleperiod(struct batgw *bg, void *arg)
{
	struct byd_softc *sc = arg;
	const struct batgw_kv *kv;
	unsigned int i;

	for (i = 0; i < nitems(sc->kvs); i++) {
		kv = &sc->kvs[i];
		if (kv->kv_v == INT_MIN)
			continue;

		batgw_kv_publish(bg, "battery", kv);
	}

	for (i = 0; i < nitems(sc->pack); i++) {
		kv = &sc->pack[i];
		if (kv->kv_v == INT_MIN)
			continue;

		batgw_kv_publish(bg, "battery", kv);
	}

	for (i = 0; i < sc->ncell; i++) {
		kv = &sc->cell[i];
		if (kv->kv_v == INT_MIN)
			continue;

		batgw_kv_publish(bg, "battery", kv);
	}
}

static void
byd_can_50ms_change(int nil, short events, void *arg)
{
	/*
	 * nop
	 *
	 * byd_can_50ms checks whether this evtimer is pending.
	 */
}

static void
byd_can_50ms(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_softc *sc = batgw_b_softc(bg);
	struct can_frame frame = {
		.can_id = 0x12d,
		.len = 8,
		.data = { 0xa0, 0x28, 0x02, 0xa0, 0x0c, 0x71, 0x00, 0x00 },
	};
	ssize_t rv;

	evtimer_add(sc->can_50ms, &byd_50ms);

	if (!evtimer_pending(sc->can_50ms_change, NULL)) {
		frame.data[2] = 0x00;
		frame.data[3] = 0x22;
		frame.data[5] = 0x31;
	}

	frame.data[6] = (sc->can_50ms_6 -= BYD_50MS_DECR);
	frame.data[7] = (sc->can_50ms_7 -= BYD_50MS_DECR);

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1) {
		lwarn("byd battery 50ms send");
		return;
	}
}

static void
byd_can_100ms(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_softc *sc = batgw_b_softc(bg);
	struct can_frame frame = {
		.can_id = 0x441,
		.len = 8,
		.data = { 0x98, 0x3a, 0x88, 0x13, 0x00, 0x00, 0xff, 0x00 },
	};
	ssize_t rv;
	int v; /* volts */
	unsigned int csum = 0;
	size_t i;

	evtimer_add(sc->can_100ms, &byd_100ms);

	v = batgw_kv_get(&sc->kvs[BYD_KV_VOLTAGE]);
	if (v <= 12 || !batgw_b_get_contactor(bg))
		v = 12;

	can_htole16(&frame, 4, v);

	for (i = 0; i < sizeof(frame.data) - 1; i++)
		csum += frame.data[i];
	frame.data[7] = ~csum;

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1) {
		lwarn("byd battery 100ms send");
		return;
	}
}

static const uint16_t byd_poll_pids[] = {
	BYD_PID_BATTERY_SOC,
	BYD_PID_BATTERY_VOLTAGE,
	BYD_PID_BATTERY_CURRENT,
	BYD_PID_CELL_TEMP_MIN,
	BYD_PID_CELL_TEMP_MAX,
	BYD_PID_CELL_TEMP_AVG,
	BYD_PID_CELL_MV_MIN,
	BYD_PID_CELL_MV_MAX,
	BYD_PID_MAX_CHARGE_POWER,
	BYD_PID_MAX_DISCHARGE_POWER,
	BYD_PID_CHARGE_TIMES,
	BYD_PID_TOTAL_CHARGED_AH,
	BYD_PID_TOTAL_DISCHARGED_AH,
	BYD_PID_TOTAL_CHARGED_KWH,
	BYD_PID_TOTAL_DISCHARGED_KWH,
};

static void
byd_can_poll(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_softc *sc = batgw_b_softc(bg);

	unsigned int idx = sc->can_poll_idx;
	uint16_t pid;
	struct can_frame frame = {
		.can_id = 0x7e7,
		.len = 8,
		.data = { 0x03, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	};
	ssize_t rv;

	idx = sc->can_poll_idx;
	pid = byd_poll_pids[idx];
	if (++idx >= nitems(byd_poll_pids))
		idx = 0;
	sc->can_poll_idx = idx;

	evtimer_add(sc->can_poll, &byd_200ms);

	frame.data[2] = pid >> 8;
	frame.data[3] = pid >> 0;

	rv = send(EVENT_FD(sc->can_recv), &frame, sizeof(frame), 0);
	if (rv == -1) {
		lwarn("byd battery can send");
		return;
	}
}

static void
byd_can_wdog(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	batgw_b_set_stopped(bg);
}

static uint16_t
bydtoh12(const struct can_frame *frame, size_t o)
{
        return (can_letoh16(frame, o) & 0xfff);
}

static int
bydtodegc(const struct can_frame *frame, size_t o)
{
        return ((int)frame->data[o] - 40);
}

static void
byd_can_recv(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_softc *sc = batgw_b_softc(bg);
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
			lwarn("byd battery can recv");
			break;
		}
		return;
	}

	if (frame.len != 8) {
		/* this is unexpected */
		return;
	}

	switch (frame.can_id) {
	case 0x244:
	case 0x245:
	case 0x286:
	case 0x344:
	case 0x345:
	case 0x347:
	case 0x34a:
	case 0x35e:
	case 0x360:
	case 0x36c:
	case 0x438:
	case 0x43a:
	case 0x43b:
	case 0x43c:
	case 0x43d:
	case 0x444:
	case 0x445:
	case 0x446:
	case 0x447:
	case 0x47b:
	case 0x524:
		batgw_b_set_running(bg);
		evtimer_add(sc->can_wdog, &byd_wdog_tv);
		break;
	}

	if (batgw_verbose(bg) > 1) {
		printf("0x%03x [%u]", frame.can_id, frame.len);
		for (i = 0; i < frame.len; i++) {
			printf(" %02x", frame.data[i]);
		}
		printf("\n");
	}

	switch (frame.can_id) {
	case 0x245:
		switch (frame.data[0]) {
		case 0x01:
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_AMBIENT], bydtodegc(&frame, 4));
			break;
		}
		break;
	case 0x43c:
		k = frame.data[0] * 6;
		for (i = 0; i < 6; i++) {
			unsigned int key = k + i;
			if (key >= nitems(sc->pack))
				break;

			batgw_kv_update(bg, "battery",
			    &sc->pack[key], bydtodegc(&frame, 1 + i));
		}
		break;
	case 0x43d:
		k = frame.data[0] * 3;
		for (int i = 0; i < 3; i++) {
			unsigned int key = k + i;
			if (key >= sc->ncell)
				break;

			batgw_kv_update(bg, "battery",
			    sc->cell + key, can_letoh16(&frame, 1 + (2 * i)));
		}
		break;
	case 0x444:
		batgw_kv_update(bg, "battery",
		    &sc->kvs[BYD_KV_VOLTAGE], can_letoh16(&frame, 0));
		break;
	case 0x447:
		uv = can_letoh16(&frame, 4);
		batgw_b_set_soc_c_pct(bg, uv * 10);
		batgw_kv_update(bg, "battery",
		    &sc->kvs[BYD_KV_SOC], uv);
		//printf("lo temp? %u\n", frame.data[1] - 40);
		//printf("hi temp? %u\n", frame.data[3] - 40);
		break;
	case 0x7ef:
		if (frame.data[0] == 0x10) {
			static const struct can_frame ack = {
				.can_id = 0x7e7,
				.len = 8,
				.data = { 0x30, 0x08, 0x05, 0x00,
				    0x00, 0x00, 0x00, 0x00 },
			};

			rv = send(fd, &ack, sizeof(ack), 0);
			if (rv == -1)
				lwarn("byd battery pid ack write");
		}

		switch (can_betoh16(&frame, 2)) {
		case BYD_PID_BATTERY_SOC:
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_SOC], frame.data[4]);
			break;
		case BYD_PID_BATTERY_VOLTAGE:
			uv = can_letoh16(&frame, 4);
			batgw_b_set_voltage_dv(bg, uv * 10);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_VOLTAGE], uv);
			break;
		case BYD_PID_BATTERY_CURRENT:
			sv = can_letoh16(&frame, 4);
			sv -= 5000;
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_CURRENT], sv);
			break;
		case BYD_PID_CELL_TEMP_MIN:
			sv = bydtodegc(&frame, 4);
			batgw_b_set_min_temp_dc(bg, sv * 10);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_TEMP_MIN], sv);
			break;
		case BYD_PID_CELL_TEMP_MAX:
			sv = bydtodegc(&frame, 4);
			batgw_b_set_max_temp_dc(bg, sv * 10);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_TEMP_MAX], sv);
			break;
		case BYD_PID_CELL_TEMP_AVG:
			sv = bydtodegc(&frame, 4);
			batgw_b_set_avg_temp_dc(bg, sv * 10);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_TEMP_AVG], sv);
			break;
		case BYD_PID_CELL_MV_MIN:
			uv = can_letoh16(&frame, 4);
			batgw_b_set_min_cell_voltage_mv(bg, uv);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_MV_MIN], uv);
			break;
		case BYD_PID_CELL_MV_MAX:
			uv = can_letoh16(&frame, 4);
			batgw_b_set_max_cell_voltage_mv(bg, uv);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_MV_MAX], uv);

			sv = uv - batgw_kv_get(&sc->kvs[BYD_KV_PID_MV_MIN]);
			if (sv >= 0) {
				batgw_kv_update(bg, "battery",
				    &sc->kvs[BYD_KV_PID_MV_DELTA], sv);
			}
			break;
		case BYD_PID_MAX_CHARGE_POWER:
			uv = can_letoh16(&frame, 4) * 100;
			batgw_b_set_charge_w(bg, uv);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_CHARGE_POWER], uv);
			break;
		case BYD_PID_MAX_DISCHARGE_POWER:
			uv = can_letoh16(&frame, 4) * 100;
			batgw_b_set_discharge_w(bg, uv);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_DISCHARGE_POWER], uv);
			break;
		case BYD_PID_CHARGE_TIMES:
			uv = can_letoh16(&frame, 4);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_CHARGE_COUNT], uv);
			break;
		case BYD_PID_TOTAL_CHARGED_AH:
			uv = can_letoh16(&frame, 4);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_CHARGED_AH], uv);
			break;
		case BYD_PID_TOTAL_DISCHARGED_AH:
			uv = can_letoh16(&frame, 4);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_DISCHARGED_AH], uv);
			break;
		case BYD_PID_TOTAL_CHARGED_KWH:
			uv = can_letoh16(&frame, 4);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_CHARGED_KWH], uv);
			break;
		case BYD_PID_TOTAL_DISCHARGED_KWH:
			uv = can_letoh16(&frame, 4);
			batgw_kv_update(bg, "battery",
			    &sc->kvs[BYD_KV_PID_DISCHARGED_KWH], uv);
			break;
		}
		break;
	}
}
