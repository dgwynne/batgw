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
 * glue
 */

static int	 byd_b_check(const struct batgw_config_battery *);
static void	 byd_b_config(struct batgw_config_battery *);
static void	*byd_b_attach(struct batgw *);
static void	 byd_b_dispatch(struct batgw *, void *);
static void	 byd_b_teleperiod(struct batgw *, void *);

const struct batgw_battery byd_battery = {
	.b_check =			byd_b_check,
	.b_config =			byd_b_config,
	.b_attach =			byd_b_attach,
	.b_dispatch =			byd_b_dispatch,
	.b_teleperiod =			byd_b_teleperiod,
};

/*
 * byd software driver
 */

struct byd_softc {
	int			 can;
	struct event		*can_recv;
	struct event		*can_poll;
	unsigned int		 can_poll_idx;
	struct event		*can_wdog;
};

static void	byd_can_poll(int, short, void *);
static void	byd_can_recv(int, short, void *);
static void	byd_can_wdog(int, short, void *);

static const struct timeval byd_200ms = { 0, 200000 };
static const struct timeval byd_wdog_tv = { 10, 0 };

#if 0
enum byd_kv_map {
	BYD_AMBIENT_TEMP,
	BYD_MIN_TEMP,
	BYD_MAX_TEMP,
	BYD_AVG_TEMP,

	BYD_VOLTAGE,
	BYD_SOC,

	BYD_NKVS
};

struct b_byd_softc {
	
	struct batgw_kv byd_kvs[] = {
	[BYD_AMBIENT_TEMP]	= { "ambient", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_MIN_TEMP]		= { "min", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_MAX_TEMP]		= { "max", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_AVG_TEMP]		= { "avg", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_VOLTAGE]		= { "", INT_MIN, KV_T_VOLTAGE, 0 },
	[BYD_SOC]		= { "soc", INT_MIN, KV_T_PERCENT, 1 },
};
#endif

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

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		err(1, "%s alloc", __func__);

	fd = can_open("byd battery", bconf->ifname);

	sc->can = fd;

	sc->can_recv = event_new(batgw_event_base(bg), fd, EV_READ|EV_PERSIST,
	    byd_can_recv, bg);
	if (sc->can_recv == NULL)
		errx(1, "new byd battery can recv event failed");

	sc->can_poll = evtimer_new(batgw_event_base(bg),
	    byd_can_poll, bg);
	if (sc->can_poll == NULL)
		errx(1, "new byd battery can poll event failed");

	sc->can_wdog = evtimer_new(batgw_event_base(bg),
	    byd_can_wdog, bg);
	if (sc->can_wdog == NULL)
		errx(1, "new byd battery can wdog event failed");

	return (sc);
}

static void
byd_b_dispatch(struct batgw *bg, void *arg)
{
	struct byd_softc *sc = arg;

//	batgw_b_set_min_voltage_dv(bg, 3800);
//	batgw_b_set_max_voltage_dv(bg, 4410);

	event_add(sc->can_recv, NULL);
	byd_can_poll(0, 0, bg);
}

static void
byd_b_teleperiod(struct batgw *bg, void *arg)
{
	struct byd_softc *sc = arg;
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
	int v;

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
#if 0
	case 0x245:
		switch (frame.data[0]) {
		case 0x01:
			byd_kv_update(bg, BYD_AMBIENT_TEMP,
			    byd2degc(&frame, 4));
			break;
		}
		break;
	case 0x43c:
		switch (frame.data[0]) {
		case 0x00:
			for (i = 0; i < 6; i++) {
				batgw_kv_update(bg, "battery",
				    byd_temps + 0 + i,
				    byd2degc(&frame, 1 + i));
			}
			break;
		case 0x01:
			for (i = 0; i < 4; i++) {
				batgw_kv_update(bg, "battery",
				    byd_temps + 6 + i,
				    byd2degc(&frame, 1 + i));
			}
			break;
		}
		break;
	case 0x43d:
		uint8_t base = frame.data[0] * 3;
		if (base >= nitems(byd_cells))
			break;

		for (int i = 0; i < 3; i++) {
			batgw_kv_update(bg, "battery",
			    byd_cells + base + i,
			    can_letoh16(&frame, 1 + (2 * i)));
		}
		break;
	case 0x444:
		byd_kv_update(bg, BYD_VOLTAGE, can_letoh16(&frame, 0));
		break;
#endif
	case 0x447:
		uint16_t soc = can_letoh16(&frame, 4);
		batgw_b_set_soc_c_pct(bg, soc * 10);
		printf("soc %u.%u%%\n", soc / 10, soc % 10);
		printf("lo temp? %u\n", frame.data[1] - 40);
		printf("hi temp? %u\n", frame.data[3] - 40);
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
//			printf("pid soc %u\n", frame.data[4]);
			break;
		case BYD_PID_BATTERY_VOLTAGE:
			printf("pid voltage %u\n", can_letoh16(&frame, 4));
			break;
		case BYD_PID_BATTERY_CURRENT:
			printf("pid current %.1f\n",
			    (can_letoh16(&frame, 4) - 5000) / 10.0);
			break;
		case BYD_PID_CELL_TEMP_MIN:
			batgw_b_set_min_temp_dc(bg, bydtodegc(&frame, 4) * 10);
//			byd_kv_update(bg, BYD_MIN_TEMP,
//			    byd2degc(&frame, 4));
			printf("pid cell temp min %u\n", frame.data[4] - 40);
			break;
		case BYD_PID_CELL_TEMP_MAX:
			batgw_b_set_max_temp_dc(bg, bydtodegc(&frame, 4) * 10);
//			byd_kv_update(bg, BYD_MAX_TEMP,
//			    byd2degc(&frame, 4));
			printf("pid cell temp max %u\n", frame.data[4] - 40);
			break;
		case BYD_PID_CELL_TEMP_AVG:
			batgw_b_set_avg_temp_dc(bg, bydtodegc(&frame, 4) * 10);
//			byd_kv_update(bg, BYD_AVG_TEMP,
//			    byd2degc(&frame, 4));
			printf("pid cell temp avg %u\n", frame.data[4] - 40);
			break;
		case BYD_PID_CELL_MV_MIN:
			printf("pid cell mv min %u\n", can_letoh16(&frame, 4));
			break;
		case BYD_PID_CELL_MV_MAX:
			printf("pid cell mv max %u\n", can_letoh16(&frame, 4));
			break;
		case BYD_PID_MAX_CHARGE_POWER:
			printf("pid max charge power %uW\n",
			    can_letoh16(&frame, 4) * 100);
			break;
		case BYD_PID_MAX_DISCHARGE_POWER:
			printf("pid max discharge power %uW\n",
			    can_letoh16(&frame, 4) * 100);
			break;
		case BYD_PID_CHARGE_TIMES:
			printf("pid charge times %u\n",
			    can_letoh16(&frame, 4));
			break;
		case BYD_PID_TOTAL_CHARGED_AH:
			printf("pid total charged ah %u\n",
			    can_letoh16(&frame, 4));
			break;
		case BYD_PID_TOTAL_DISCHARGED_AH:
			printf("pid total discharged ah %u\n",
			    can_letoh16(&frame, 4));
			break;
		case BYD_PID_TOTAL_CHARGED_KWH:
			printf("pid total charged kwh %u\n",
			    can_letoh16(&frame, 4));
			break;
		case BYD_PID_TOTAL_DISCHARGED_KWH:
			printf("pid total discharged kwh %u\n",
			    can_letoh16(&frame, 4));
			break;
		}
		break;
	}
}
