
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

#include <sys/types.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <err.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "../batgw_config.h"
#include "../batgw.h"
#include "../log.h"

/*
 * hardware
 */

#define BYD_HVS_FW_MAJOR		0x03
#define BYD_HVS_FW_MINOR		0x29

#define BYD_HVS_PID_VENDOR		0x2d0
#define BYD_HVS_PID_PRODUCT		0x3d0

static const char byd_hvs_vendor[] = "BYD";
static const char byd_hvs_product[] = "Battery-Box Premium HVS";

#define BYD_HVS_VOLTAGE_OFFSET_DV	20

/*
 * glue
 */

static int	 byd_can_i_check(const struct batgw_config_inverter *);
static void	 byd_can_i_config(struct batgw_config_inverter *);
static void	*byd_can_i_attach(struct batgw *);
static void	 byd_can_i_dispatch(struct batgw *, void *);
static void	 byd_can_i_teleperiod(struct batgw *, void *);

const struct batgw_inverter inverter_byd_can = {
	.i_check =			byd_can_i_check,
	.i_config =			byd_can_i_config,
	.i_attach =			byd_can_i_attach,
	.i_dispatch =			byd_can_i_dispatch,
	.i_teleperiod =			byd_can_i_teleperiod,
};

/*
 * byd inverter driver
 */

enum byd_can_kvs {
	BYD_CAN_KV_TEMP,
	BYD_CAN_KV_SEND_VOLTAGE,
	BYD_CAN_KV_RECV_VOLTAGE,
	BYD_CAN_KV_DISCHARGE_CURRENT,
	BYD_CAN_KV_CHARGE_CURRENT,

	BYD_CAN_KV_COUNT
};

static const struct batgw_kv_tpl byd_can_kvs_tpl[BYD_CAN_KV_COUNT] = {
	[BYD_CAN_KV_TEMP] =
		{ "temperature",	KV_T_TEMP,      1 },
	[BYD_CAN_KV_SEND_VOLTAGE] =
		{ "send-voltage",	KV_T_VOLTAGE,	1 },
	[BYD_CAN_KV_RECV_VOLTAGE] =
		{ "recv-voltage",	KV_T_VOLTAGE,	1 },
	[BYD_CAN_KV_DISCHARGE_CURRENT] =
		{ "max-discharge",	KV_T_CURRENT,   1 },
	[BYD_CAN_KV_DISCHARGE_CURRENT] =
		{ "max-charge",		KV_T_CURRENT,   1 },
};

enum byd_can_ivals {
	BYD_CAN_IVAL_2S,
	BYD_CAN_IVAL_10S,
	BYD_CAN_IVAL_60S,

	BYD_CAN_IVAL_COUNT
};

struct byd_can_i_softc {
	unsigned int		 running;

	int			 can;
	struct event		*can_recv;
	struct event		*can_wdog;

	struct event		*can_ivals[BYD_CAN_IVAL_COUNT];

	time_t			 inverter_time;
	struct batgw_kv		 kvs[BYD_CAN_KV_COUNT];
};

static void	byd_can_i_poll(int, short, void *);
static void	byd_can_i_recv(int, short, void *);
static void	byd_can_i_wdog(int, short, void *);

static void	byd_can_i_2s(int, short, void *);
static void	byd_can_i_10s(int, short, void *);
static void	byd_can_i_60s(int, short, void *);

static const event_callback_fn byd_can_ivals[BYD_CAN_IVAL_COUNT] = {
	[BYD_CAN_IVAL_2S] =	byd_can_i_2s,
	[BYD_CAN_IVAL_10S] =	byd_can_i_10s,
	[BYD_CAN_IVAL_60S] =	byd_can_i_60s,
};

static const struct timeval byd_wdog_tv = { 60, 0 };
static const struct timeval byd_2s = { 2, 000000 };
static const struct timeval byd_10s = { 10, 000000 };
static const struct timeval byd_60s = { 60, 000000 };

static int
byd_can_i_check(const struct batgw_config_inverter *iconf)
{
	int rv = 0;

	if (iconf->ifname == NULL) {
		fprintf(stderr, "%s inverter: interface not configured\n",
		    iconf->protocol);
		rv = -1;
	}

	return (rv);
}

static void
byd_can_i_config(struct batgw_config_inverter *iconf)
{

}

static void *
byd_can_i_attach(struct batgw *bg)
{
	const struct batgw_config_inverter *iconf = batgw_i_config(bg);
	struct byd_can_i_softc *sc;
	int fd;
	size_t i;

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		err(1, "%s alloc", __func__);

	fd = can_open("byd inverter", iconf->ifname);

	sc->can = fd;

	sc->can_recv = event_new(batgw_event_base(bg), fd, EV_READ|EV_PERSIST,
	    byd_can_i_recv, bg);
	if (sc->can_recv == NULL)
		errx(1, "new byd can inverter recv event failed");

	sc->can_wdog = evtimer_new(batgw_event_base(bg),
	    byd_can_i_wdog, bg);
	if (sc->can_wdog == NULL)
		errx(1, "new byd can inverter wdog event failed");

	for (i = 0; i < nitems(sc->can_ivals); i++) {
		sc->can_ivals[i] = evtimer_new(batgw_event_base(bg),
		    byd_can_ivals[i], bg);
		if (sc->can_ivals[i] == NULL)
			errx(1, "new byd can inverter ival %zu failed", i);
	}

	for (i = 0; i < nitems(sc->kvs); i++)
		batgw_kv_init_tpl(&sc->kvs[i], &byd_can_kvs_tpl[i]);

	return (sc);
}

static void
byd_can_i_dispatch(struct batgw *bg, void *arg)
{
	struct byd_can_i_softc *sc = arg;

	event_add(sc->can_recv, NULL);
}

static void
byd_can_i_teleperiod(struct batgw *bg, void *arg)
{
	struct byd_can_i_softc *sc = arg;
	const struct batgw_kv *kv;
	size_t i;

	for (i = 0; i < nitems(sc->kvs); i++) {
		kv = &sc->kvs[i];
		if (kv->kv_v == INT_MIN)
			continue;

		batgw_kv_publish(bg, "inverter", kv);
	}
}

static void
byd_can_i_wdog(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_can_i_softc *sc = batgw_i_softc(bg);
	size_t i;

	lwarnx("%s", __func__);
	batgw_i_set_stopped(bg);
	batgw_i_set_contactor(bg, 0);

	for (i = 0; i < nitems(sc->can_ivals); i++)
		evtimer_del(sc->can_ivals[i]);
}

void
byd_can_i_send_str(struct byd_can_i_softc *sc, uint16_t id,
    const char *str, size_t len)
{
	struct can_frame frame = { .can_id = id, .len = 8 };
	uint8_t i = 0;
	ssize_t rv;

	for (;;) {
		size_t flen = len;
		if (flen > (sizeof(frame.data) - 1))
			flen = sizeof(frame.data) - 1;

		memset(frame.data, 0, sizeof(frame.data));
		frame.data[0] = i;
		memcpy(frame.data + 1, str, flen);

		rv = send(sc->can, &frame, sizeof(frame), 0);
		if (rv == -1)
			lwarnx("byd can inverter send 0x%03x %u", id, i);

		len -= flen;
		if (len == 0)
			break;

		str += flen;
		i++;
	}
}

static void
byd_can_i_hello(struct batgw *bg, struct byd_can_i_softc *sc)
{
	struct can_frame frame = { .len = 8 };
	unsigned int wh;
	ssize_t rv;
	size_t i;

	if (batgw_i_get_rated_capacity_wh(bg, &wh) != 0)
		return;

	wh /= 100;

	frame.can_id = 0x250;
	frame.data[0] = BYD_HVS_FW_MAJOR;
	frame.data[1] = BYD_HVS_FW_MINOR;
	frame.data[2] = 0x00;
	frame.data[3] = 0x66;
	can_htobe16(&frame, 4, wh);
	frame.data[6] = 0x02;
	frame.data[7] = 0x09;

	rv = send(sc->can, &frame, sizeof(frame), 0);
	if (rv == -1)
		lwarnx("byd can inverter send 0x%03x", frame.can_id);

	frame.can_id = 0x290;
	frame.data[0] = 0x06;
	frame.data[1] = 0x37;
	frame.data[2] = 0x10;
	frame.data[3] = 0xd9;
	frame.data[4] = 0x00;
	frame.data[5] = 0x00;
	frame.data[6] = 0x00;
	frame.data[7] = 0x00;

	rv = send(sc->can, &frame, sizeof(frame), 0);
	if (rv == -1)
		lwarnx("byd can inverter send 0x%03x", frame.can_id);

	byd_can_i_send_str(sc, BYD_HVS_PID_VENDOR,
	    byd_hvs_vendor, sizeof(byd_hvs_vendor));
	byd_can_i_send_str(sc, BYD_HVS_PID_PRODUCT,
	    byd_hvs_product, sizeof(byd_hvs_product));

	for (i = 0; i < nitems(sc->can_ivals); i++) {
		event_callback_fn fn = event_get_callback(sc->can_ivals[i]);
		fn(0, 0, bg);
	}
}

static void
byd_can_i_recv(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_can_i_softc *sc = batgw_i_softc(bg);
	struct can_frame frame;
	ssize_t rv;
	size_t i;
	int v;
	unsigned int bdv, idv;
	unsigned int contactor = 0;

	rv = recv(fd, &frame, sizeof(frame), 0);
	if (rv == -1) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			break;
		default:
			lwarn("byd can inverter recv");
			break;
		}
		return;
	}

	if (frame.len != 8) {
		/* this is unexpected */
		return;
	}

	if (!sc->running) {
		if (frame.can_id != 0x151 && frame.data[0] != 0x01)
			return;
		if (!batgw_b_get_running(bg))
			return;

		sc->running = 1;
	}

	switch (frame.can_id) {
	case 0x019:
	case 0x0d1:
	case 0x111:
	case 0x151:
		batgw_i_set_running(bg);
		evtimer_add(sc->can_wdog, &byd_wdog_tv);
		break;
	}

	if (batgw_verbose(bg) > 1) {
		printf("i 0x%03x [%u]", frame.can_id, frame.len);
		for (i = 0; i < frame.len; i++) {
			printf(" %02x", frame.data[i]);
		}
		printf("\n");
	}

	switch (frame.can_id) {
	case 0x151:
		switch (frame.data[0]) {
		case 0x00:
			linfo("inverter brand %s", frame.data + 1);
			break;
		case 0x01:
			byd_can_i_hello(bg, sc);
			break;
		}
		break;

	case 0x091:
		idv = can_betoh16(&frame, 0);
		batgw_kv_update(bg, "inverter",
		    &sc->kvs[BYD_CAN_KV_RECV_VOLTAGE], idv);

		if (batgw_i_get_voltage_dv(bg, &bdv) != 0) {
			contactor =
			    (bdv + BYD_HVS_VOLTAGE_OFFSET_DV) > idv &&
			    (bdv - BYD_HVS_VOLTAGE_OFFSET_DV) < idv;
		}
		batgw_i_set_contactor(bg, contactor);

		//printf("i current %u\n", can_betoh16(&frame, 2));
		batgw_kv_update(bg, "inverter",
		    &sc->kvs[BYD_CAN_KV_TEMP], can_betoh16(&frame, 4));
		break;
	case 0x0d1:
		/* use gmtime to pull this apart event though it's not UTC */
		//printf("i soc %u\n", can_betoh16(&frame, 0));
		break;
	case 0x111:
		sc->inverter_time = can_betoh32(&frame, 0);
		break;
	}
}

static void
byd_can_i_2s(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_can_i_softc *sc = batgw_i_softc(bg);
	struct can_frame frame = { .can_id = 0x110, .len = 8 };
	ssize_t rv;
	unsigned int min_dv, max_dv, da;
	unsigned int safety;

	evtimer_add(sc->can_ivals[BYD_CAN_IVAL_2S], &byd_2s);

	if (batgw_i_get_min_voltage_dv(bg, &min_dv) != 0 ||
	    batgw_i_get_max_voltage_dv(bg, &max_dv) != 0)
		return;

	safety = batgw_i_get_safety(bg);

	can_htobe16(&frame, 0, max_dv - BYD_HVS_VOLTAGE_OFFSET_DV);
	can_htobe16(&frame, 2, min_dv + BYD_HVS_VOLTAGE_OFFSET_DV);

	/* max discharge current dA */
	da = batgw_i_get_discharge_da(bg, safety);
	batgw_kv_update(bg, "inverter",
	    &sc->kvs[BYD_CAN_KV_DISCHARGE_CURRENT], da);
	can_htobe16(&frame, 4, da);

	/* min discharge current dA */
	da = batgw_i_get_charge_da(bg, safety);
	batgw_kv_update(bg, "inverter",
	    &sc->kvs[BYD_CAN_KV_CHARGE_CURRENT], da);
	can_htobe16(&frame, 6, da);

	rv = send(sc->can, &frame, sizeof(frame), 0);
	if (rv == -1)
		lwarn("byd can inverter send 0x%03x 2s", frame.can_id);
}

static void
byd_can_send_150(const struct batgw *bg, struct byd_can_i_softc *sc)
{
	struct can_frame frame = { .can_id = 0x150, .len = 8 };
	unsigned int soc, ah;

	if (batgw_i_get_soc_cpct(bg, &soc) != 0 ||
	    batgw_i_get_rated_capacity_ah(bg, &ah) != 0)
		return;

	can_htobe16(&frame, 0, soc); /* soc */
	can_htobe16(&frame, 2, 9900); /* soh */
	can_htobe16(&frame, 4, (ah * soc) / 10000);
	can_htobe16(&frame, 6, ah);

	if (send(sc->can, &frame, sizeof(frame), 0) == -1)
		lwarn("byd can inverter send 0x%03x", frame.can_id);
}

static void
byd_can_send_1d0(struct batgw *bg, struct byd_can_i_softc *sc)
{
	struct can_frame frame = { .can_id = 0x1d0, .len = 8 };
	unsigned int dv;
	int temp;

	if (batgw_i_get_avg_temp_dc(bg, &temp) != 0)
		return;
	if (batgw_i_get_voltage_dv(bg, &dv) != 0)
		dv = 0;

	batgw_kv_update(bg, "inverter",
	    &sc->kvs[BYD_CAN_KV_SEND_VOLTAGE], dv);
	can_htobe16(&frame, 0, dv); /* dV */
	can_htobe16(&frame, 2, 0); /* dA */
	can_htobe16(&frame, 4, temp);

	if (send(sc->can, &frame, sizeof(frame), 0) == -1)
		lwarn("byd can inverter send 0x%03x", frame.can_id);
}

static void
byd_can_send_210(const struct batgw *bg, struct byd_can_i_softc *sc)
{
	struct can_frame frame = { .can_id = 0x210, .len = 8 };
	unsigned int min_temp, max_temp;

	if (batgw_i_get_min_temp_dc(bg, &min_temp) != 0 ||
	    batgw_i_get_min_temp_dc(bg, &max_temp) != 0)
		return;

	can_htobe16(&frame, 0, max_temp);
	can_htobe16(&frame, 2, min_temp);

	if (send(sc->can, &frame, sizeof(frame), 0) == -1)
		lwarn("byd can inverter send 0x%03x", frame.can_id);
}

static void
byd_can_i_10s(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_can_i_softc *sc = batgw_i_softc(bg);
	struct can_frame frame = { .len = 8 };
	ssize_t rv;

	evtimer_add(sc->can_ivals[BYD_CAN_IVAL_10S], &byd_10s);

	byd_can_send_150(bg, sc);
	byd_can_send_1d0(bg, sc);
	byd_can_send_210(bg, sc);
}

static void
byd_can_i_60s(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct byd_can_i_softc *sc = batgw_i_softc(bg);
	struct can_frame frame = {
		.can_id = 0x190,
		.len = 8,
		.data = { [2] = 0x03, }
	};
	ssize_t rv;

	evtimer_add(sc->can_ivals[BYD_CAN_IVAL_60S], &byd_60s);

	rv = send(sc->can, &frame, sizeof(frame), 0);
	if (rv == -1)
		lwarn("byd can inverter send 0x%03x 60s", frame.can_id);
}
