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

#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <err.h>

#include <bsd/string.h> /* strlcpy */
#include <bsd/stdlib.h> /* getprogname */

#include <event2/dns.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "batgw.h"
#include "log.h"
#include "amqtt.h"

#define MQTT_PORT "1883"

static int	can_open(const char *name);
static void	can_recv(int, short, void *);
static void	can_poll(int, short, void *);

static void	byd_attach(struct batgw *);
static void	teleperiod(int, short, void *);
static const struct timeval teleperiod_tv = { 60, 0 };

static void	batgw_mqtt_init(struct batgw *);

/* wrappers */

static void	batgw_mqtt_rd(int, short, void *);
static void	batgw_mqtt_wr(int, short, void *);
static void	batgw_mqtt_to(int, short, void *);

/* callbacks */

static void	batgw_mqtt_want_output(struct mqtt_conn *);
static ssize_t	batgw_mqtt_output(struct mqtt_conn *, const void *, size_t);

static void	batgw_mqtt_on_connect(struct mqtt_conn *);
static void	batgw_mqtt_on_suback(struct mqtt_conn *, void *,
		    const uint8_t *, size_t);
static void	batgw_mqtt_on_message(struct mqtt_conn *,
		    char *, size_t, char *, size_t, enum mqtt_qos);
static void	batgw_mqtt_dead(struct mqtt_conn *);

static void	batgw_mqtt_want_timeout(struct mqtt_conn *,
		    const struct timespec *);

static const struct mqtt_settings batgw_mqtt_settings = {
	.mqtt_want_output = batgw_mqtt_want_output,
	.mqtt_output = batgw_mqtt_output,
	.mqtt_want_timeout = batgw_mqtt_want_timeout,

	.mqtt_on_connect = batgw_mqtt_on_connect,
	.mqtt_on_suback = batgw_mqtt_on_suback,
	.mqtt_on_message = batgw_mqtt_on_message,
	.mqtt_dead = batgw_mqtt_dead,
};

static const struct timeval can_200ms = { .tv_sec = 0, .tv_usec = 200000 };

struct event *sev, *tev;

__dead static void
usage(void)
{
	const char *progname = getprogname();

	fprintf(stderr, "usage: %s [-46] [-D mqttdevice] -h mqtthost\n",
	    progname);

	exit(1);
}

static struct evutil_addrinfo batgw_mqtt_hints = {
	.ai_family = AF_UNSPEC,
	.ai_socktype = SOCK_STREAM,
	.ai_protocol = IPPROTO_TCP,
};

static struct batgw _bg = {
	.bg_mqtt_fd = -1,

	.bg_mqtt = {
		.keep_alive = 30,
		.hints = &batgw_mqtt_hints,
		.port = MQTT_PORT,
	},
};

int
main(int argc, char *argv[])
{
	struct batgw *bg = &_bg;
	int ch;
	const char *errstr;
	int can0fd;

	bg->bg_mqtt.devname = getprogname();

	while ((ch = getopt(argc, argv, "46D:h:k:p:")) != -1) {
		switch (ch) {
		case '4':
			bg->bg_mqtt.hints->ai_family = AF_INET;
			break;
		case '6':
			bg->bg_mqtt.hints->ai_family = AF_INET;
			break;
		case 'D':
			bg->bg_mqtt.devname = optarg;
			break;
		case 'h':
			bg->bg_mqtt.host = optarg;
			break;
		case 'k':
			bg->bg_mqtt.keep_alive = strtonum(optarg, 1, 1800,
			    &errstr);
			if (errstr != NULL)
				errx(1, "mqtt keep-alive: %s", errstr);
			break;
		case 'p':
			bg->bg_mqtt.port = optarg;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	if (bg->bg_mqtt.host == NULL)
		usage();

	struct event *rev;

	bg->bg_evbase = event_base_new();
	if (bg->bg_evbase == NULL)
		errx(1, "event_base_new failed");

	bg->bg_evdnsbase = evdns_base_new(bg->bg_evbase, 1);
	if (bg->bg_evdnsbase == NULL)
		errx(1, "evdns_base_new failed");

	can0fd = can_open("can0");

	rev = event_new(bg->bg_evbase, can0fd, EV_READ|EV_PERSIST,
	    can_recv, bg);
	if (rev == NULL)
		err(1, "event_new");

	event_add(rev, NULL);

	sev = evtimer_new(bg->bg_evbase, can_poll, (void *)(uintptr_t)can0fd);
	if (sev == NULL)
		err(1, "evtimer_new");
	evtimer_add(sev, &can_200ms);

	tev = evtimer_new(bg->bg_evbase, teleperiod, bg);
	if (tev == NULL)
		err(1, "teleperiod evtimer");

	byd_attach(bg);

	batgw_mqtt_init(bg);

	event_base_dispatch(bg->bg_evbase);

	return (0);
}

void
batgw_mqtt_rd(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mqtt_conn *mc = bg->bg_mqtt_conn;
	char buf[8192];
	ssize_t rv;

	rv = read(fd, buf, sizeof(buf));
	switch (rv) {
	case -1:
		switch (errno) {
		case EAGAIN:
		case EINTR:
			return;
		default:
			break;
		}
		err(1, "%s", __func__);
		/* NOTREACHED */
	case 0:
		mqtt_disconnect(mc);
		mqtt_conn_destroy(mc);
		errx(1, "disconnected");
		/* NOTREACHED */
	default:
		break;
	}

	//hexdump(buf, rv);
	mqtt_input(mc, buf, rv);
}

void
batgw_mqtt_wr(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mqtt_conn *mc = bg->bg_mqtt_conn;

	mqtt_output(mc);
}

static void
batgw_mqtt_want_output(struct mqtt_conn *mc)
{
	struct batgw *bg = mqtt_cookie(mc);
	event_add(bg->bg_mqtt_ev_wr, NULL);
}

static ssize_t
batgw_mqtt_output(struct mqtt_conn *mc, const void *buf, size_t len)
{
	struct batgw *bg = mqtt_cookie(mc);
	int fd = EVENT_FD(bg->bg_mqtt_ev_wr);
	ssize_t rv;

	//hexdump(buf, len);

	rv = write(fd, buf, len);
	if (rv == -1) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			return (0);
		default:
			break;
		}

		err(1, "%s", __func__);
		/* XXX reconnect */
	}

	return (rv);
}

static void
batgw_mqtt_to(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mqtt_conn *mc = bg->bg_mqtt_conn;

	mqtt_timeout(mc);
}

static void
batgw_mqtt_want_timeout(struct mqtt_conn *mc, const struct timespec *ts)
{
	struct batgw *bg = mqtt_cookie(mc);
	struct timeval tv;

	TIMESPEC_TO_TIMEVAL(&tv, ts);

	event_add(bg->bg_mqtt_ev_to, &tv);
}

static void
batgw_mqtt_on_connect(struct mqtt_conn *mc)
{
	struct batgw *bg = mqtt_cookie(mc);
	static const char online[] = "Online";
	int rv;

	lwarnx("%s", __func__);

	if (mqtt_publish(mc,
	    bg->bg_mqtt.will_topic, bg->bg_mqtt.will_topic_len,
	    online, sizeof(online) - 1, MQTT_QOS0, 1) == -1)
		errx(1, "mqtt publish %s %s", bg->bg_mqtt.will_topic, online);

	warnx("%s ho", __func__);

//	evtimer_set(&bg->tele_tick, batgw_tele_tick, batgw);
//	evtimer_add(&bg->tele_tick, &tele_tv);
	teleperiod(0, 0, bg);
}

static void
batgw_mqtt_on_suback(struct mqtt_conn *mc, void *cookie,
    const uint8_t *rcodes, size_t nrcodes)
{
	lwarnx("Subscribed!");
}

static void
batgw_mqtt_on_message(struct mqtt_conn *mc,
    char *topic, size_t topic_len, char *payload, size_t payload_len,
    enum mqtt_qos qos)
{
	linfo("topic %s payload %s", topic, payload);
	free(topic);
	free(payload);
}

static void
batgw_mqtt_dead(struct mqtt_conn *mc)
{
	lerr(1, "%s", __func__);
}

static void	batgw_mqtt_connect(struct batgw *);

static void
batgw_mqtt_connected(struct batgw *bg, int s)
{
	static const char offline[] = "Offline";
	struct mqtt_conn_settings mcs = {
		.clean_session = 1,
		.keep_alive = bg->bg_mqtt.keep_alive,

		.clientid = bg->bg_mqtt.devname,
		.clientid_len = strlen(bg->bg_mqtt.devname),

		.will_topic = bg->bg_mqtt.will_topic,
		.will_topic_len = bg->bg_mqtt.will_topic_len,
		.will_payload = offline,
		.will_payload_len = sizeof(offline) - 1,
		.will_retain = 1,
	};
	struct mqtt_conn *mc;

	evutil_freeaddrinfo(bg->bg_mqtt.res0);
	bg->bg_mqtt.res0 = NULL;

	linfo("connected to mqtt server %s port %s",
	    bg->bg_mqtt.host, bg->bg_mqtt.port);

	mc = mqtt_conn_create(&batgw_mqtt_settings, bg);
	if (mc == NULL)
		lerrx(1, "unable to create mqtt connection");

	bg->bg_mqtt_ev_rd = event_new(bg->bg_evbase, s, EV_READ|EV_PERSIST,
	    batgw_mqtt_rd, bg);
	if (bg->bg_mqtt_ev_rd == NULL)
		lerrx(1, "new mqtt ev rd failed");

	bg->bg_mqtt_ev_wr = event_new(bg->bg_evbase, s, EV_WRITE,
	    batgw_mqtt_wr, bg);
	if (bg->bg_mqtt_ev_wr == NULL)
		lerrx(1, "new mqtt ev wr failed");

	bg->bg_mqtt_ev_to = evtimer_new(bg->bg_evbase,
	    batgw_mqtt_to, bg);
	if (bg->bg_mqtt_ev_to == NULL)
		lerrx(1, "new mqtt ev to failed");

	bg->bg_mqtt_conn = mc;
	if (mqtt_connect(mc, &mcs) == -1) {
		lerrx(1, "mqtt_connect server %s port %s",
		    bg->bg_mqtt.host, bg->bg_mqtt.port);
	}

	event_add(bg->bg_mqtt_ev_rd, NULL);
}

static void
batgw_mqtt_connected_ev(int s, short events, void *arg)
{
	struct batgw *bg = arg;
	int error;
	socklen_t slen = sizeof(error);

	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &slen) == -1)
		lerr(1, "%s getsockopt");
	if (error == EINPROGRESS)
		return;

	event_del(bg->bg_mqtt_ev_wr);
	event_free(bg->bg_mqtt_ev_wr);

	if (error != 0) {
		errno = error;
		lwarn("mqtt server %s port %s connect",
		    bg->bg_mqtt.host, bg->bg_mqtt.port);
		close(s);
		batgw_mqtt_connect(bg);
		return;
	}

	batgw_mqtt_connected(bg, s);
}

static void
batgw_mqtt_connect(struct batgw *bg)
{
	struct evutil_addrinfo *res;
	int s;

	while ((res = bg->bg_mqtt.resn) != NULL) {
		bg->bg_mqtt.resn = res->ai_next;

		s = socket(res->ai_family, res->ai_socktype|SOCK_NONBLOCK,
		    res->ai_protocol);
		if (s == -1)
			continue;

		if (connect(s, res->ai_addr, res->ai_addrlen) == -1) {
			if (errno == EINPROGRESS) {
				bg->bg_mqtt_ev_wr = event_new(bg->bg_evbase,
				    s, EV_WRITE|EV_PERSIST,
				    batgw_mqtt_connected_ev, bg);
				if (bg->bg_mqtt_ev_wr == NULL)
					lerr(1, "mqtt connect event_new");

				event_add(bg->bg_mqtt_ev_wr, NULL);
				return;
			}

			lwarn("mqtt server %s port %s connect",
			    bg->bg_mqtt.host, bg->bg_mqtt.port);
			close(s);
			continue;
		}

		batgw_mqtt_connected(bg, s);
		return;
	}

	errno = EHOSTUNREACH;
	lwarn("mqtt server %s port %s",
	    bg->bg_mqtt.host, bg->bg_mqtt.port);
	evutil_freeaddrinfo(bg->bg_mqtt.res0);
	bg->bg_mqtt.res0 = NULL;
	return;
}

static void
batgw_mqtt_addrinfo(int errcode, struct evutil_addrinfo *res0, void *arg)
{
	struct batgw *bg = arg;
	struct evutil_addrinfo *res;
	int s = -1;
	int serrno;
	const char *cause;

	if (errcode != 0) {
		lwarnx("mqtt server %s port %s: %s",
		    bg->bg_mqtt.host, bg->bg_mqtt.port,
		    evutil_gai_strerror(errcode));
		return;
	}

	bg->bg_mqtt.res0 = res0;
	bg->bg_mqtt.resn = res0;

	batgw_mqtt_connect(bg);
}

static void
batgw_mqtt_start(struct batgw *bg)
{
	bg->bg_mqtt.req = evdns_getaddrinfo(bg->bg_evdnsbase,
	    bg->bg_mqtt.host, bg->bg_mqtt.port, bg->bg_mqtt.hints,
	    batgw_mqtt_addrinfo, bg);
	if (bg->bg_mqtt.req == NULL) {
		lwarnx("getaddrinfo %s port %s failed",
		    bg->bg_mqtt.host, bg->bg_mqtt.port);
		return;
	}
}

static void
batgw_mqtt_init(struct batgw *bg)
{
	char *topic;
	int rv;

	rv = asprintf(&topic, "%s/LWT", bg->bg_mqtt.devname);
	if (rv == -1)
		errx(1, "mqtt lwt topic printf error");

	bg->bg_mqtt.will_topic = topic;
        bg->bg_mqtt.will_topic_len = rv;

	batgw_mqtt_start(bg);
}

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

struct byd_pid {
	uint16_t		pid;
};

static const struct byd_pid byd_poll_pids[] = {
	{ BYD_PID_BATTERY_SOC },
	{ BYD_PID_BATTERY_VOLTAGE },
	{ BYD_PID_BATTERY_CURRENT },
	{ BYD_PID_CELL_TEMP_MIN },
	{ BYD_PID_CELL_TEMP_MAX },
	{ BYD_PID_CELL_TEMP_AVG },
	{ BYD_PID_CELL_MV_MIN },
	{ BYD_PID_CELL_MV_MAX },
	{ BYD_PID_MAX_CHARGE_POWER },
	{ BYD_PID_MAX_DISCHARGE_POWER },
	{ BYD_PID_CHARGE_TIMES },
	{ BYD_PID_TOTAL_CHARGED_AH },
	{ BYD_PID_TOTAL_DISCHARGED_AH },
	{ BYD_PID_TOTAL_CHARGED_KWH },
	{ BYD_PID_TOTAL_DISCHARGED_KWH },
};

static void
can_poll(int nil, short events, void *arg)
{
	int can0fd = (int)(uintptr_t)arg;
	static unsigned int idx = 0;
	const struct byd_pid *pid = &byd_poll_pids[idx];
	struct can_frame frame = {
		.can_id = 0x7e7,
		.len = 8,
		.data = { 0x03, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	};
	ssize_t rv;

	if (++idx >= nitems(byd_poll_pids))
		idx = 0;

	evtimer_add(sev, &can_200ms);

	frame.data[2] = pid->pid >> 8;
	frame.data[3] = pid->pid >> 0;

	rv = write(can0fd, &frame, sizeof(frame));
	if (rv == -1) {
		lwarn("can send");
		return;
	}
}

static uint16_t
can2h16(const struct can_frame *frame, unsigned int o)
{
	uint16_t h16;

	h16 = (uint16_t)frame->data[o] << 8;
	h16 |= (uint16_t)frame->data[o + 1] << 0;

	return (h16);
}

static uint16_t
byd2h16(const struct can_frame *frame, size_t o)
{
	uint16_t h16;

	h16 = (uint16_t)frame->data[o];
	h16 |= (uint16_t)frame->data[o + 1] << 8;

	return (h16);
}

static uint16_t
byd2h12(const struct can_frame *frame, size_t o)
{
	return (byd2h16(frame, o) & 0xfff);
}

static int
byd2degc(const struct can_frame *frame, size_t o)
{
	return (frame->data[o] - 40);
}

static const char *const batgw_kv_type_names[KV_T_MAXTYPE] = {
	[KV_T_TEMPERATURE] =		"temperature",
	[KV_T_VOLTAGE] =		"voltage",
	[KV_T_PERCENT] =		"percent",
};

enum byd_kv_map {
	BYD_AMBIENT_TEMP,
	BYD_MIN_TEMP,
	BYD_MAX_TEMP,
	BYD_AVG_TEMP,

	BYD_VOLTAGE,
	BYD_SOC,

	BYD_NKVS
};

struct batgw_kv byd_kvs[] = {
	[BYD_AMBIENT_TEMP]	= { "ambient", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_MIN_TEMP]		= { "min", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_MAX_TEMP]		= { "max", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_AVG_TEMP]		= { "avg", INT_MIN, KV_T_TEMPERATURE, 0 },
	[BYD_VOLTAGE]		= { "", INT_MIN, KV_T_VOLTAGE, 0 },
	[BYD_SOC]		= { "soc", INT_MIN, KV_T_PERCENT, 1 },
};

struct batgw_kv byd_temps[10];
struct batgw_kv byd_cells[126];

static void
byd_attach(struct batgw *bg)
{
	size_t i;

	for (i = 0; i < nitems(byd_temps); i++) {
		struct batgw_kv *kv = &byd_temps[i];

		snprintf(kv->kv_key, sizeof(kv->kv_key), "pack%zu", i);
		kv->kv_v = INT_MIN;
		kv->kv_type = KV_T_TEMPERATURE,
		kv->kv_precision = 0;
	}

	for (i = 0; i < nitems(byd_cells); i++) {
		struct batgw_kv *kv = &byd_cells[i];

		snprintf(kv->kv_key, sizeof(kv->kv_key), "cell%zu", i);
		kv->kv_v = INT_MIN;
		kv->kv_type = KV_T_VOLTAGE,
		kv->kv_precision = 3;
	}
}

static void
batgw_kv_publish(struct batgw *bg,
    const char *scope, const struct batgw_kv *kv)
{
	char t[128];
	int tlen;
	char p[128];
	int plen;
	int div = 1;
	const char *tname = batgw_kv_type_names[kv->kv_type];
	int len;

	tlen = snprintf(t, sizeof(t), "%s", bg->bg_mqtt.devname);
	if (scope != NULL) {
		len = snprintf(t + tlen, sizeof(t) - tlen,
		    "/%s", scope);
		tlen += len;
	}
	if (kv->kv_key[0] != '\0') {
		len = snprintf(t + tlen, sizeof(t) - tlen,
		    "/%s", kv->kv_key);
		tlen += len;
	}
	len = snprintf(t + tlen, sizeof(t) - tlen,
	    "/%s", tname);
	tlen += len;

	if (kv->kv_precision == 0)
		plen = snprintf(p, sizeof(p), "%u", kv->kv_v);
	else {
		static unsigned int divs[] = { 1, 10, 100, 1000, 10000 };
		unsigned int div;
		assert(kv->kv_precision < nitems(divs));
		div = divs[kv->kv_precision];
		plen = snprintf(p, sizeof(p), "%u.%*u",
		    kv->kv_v / div, kv->kv_precision, kv->kv_v % div);
	}

	mqtt_publish(bg->bg_mqtt_conn, t, tlen, p, plen, MQTT_QOS0, 0);
}

static void
batgw_kv_update(struct batgw *bg, const char *scope,
    struct batgw_kv *kv, int v)
{
	struct timeval tv;

	if (kv->kv_v == v)
		return;
	kv->kv_v = v;

#if 1
	if (event_gettime_monotonic(bg->bg_evbase, &tv) == 0) {
		if ((tv.tv_sec - kv->kv_updated) < 10)
			return;
		kv->kv_updated = tv.tv_sec;
	}
#endif

	batgw_kv_publish(bg, scope, kv);
}

static void
byd_kv_update(struct batgw *bg, enum byd_kv_map e, int v)
{
	batgw_kv_update(bg, "battery", &byd_kvs[e], v);
}

static void
teleperiod(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	const struct batgw_kv *kv;
	size_t i;

	evtimer_add(tev, &teleperiod_tv);

	for (i = 0; i < nitems(byd_temps); i++) {
		kv = &byd_temps[i];
		if (kv->kv_v == INT_MIN)
			continue;
		batgw_kv_publish(bg, "battery", kv);
	}

	for (i = 0; i < nitems(byd_kvs); i++) {
		kv = &byd_kvs[i];
		if (kv->kv_v == INT_MIN)
			continue;
		batgw_kv_publish(bg, "battery", kv);
	}

	for (i = 0; i < nitems(byd_cells); i++) {
		kv = &byd_cells[i];
		if (kv->kv_v == INT_MIN)
			continue;
		batgw_kv_publish(bg, "battery", kv);
	}
}

static void
can_recv(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct can_frame frame;
	ssize_t rv;
	size_t i;
	int v;

	rv = read(fd, &frame, sizeof(frame));
	if (rv == -1) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			break;
		default:
			lwarn("can recv");
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
		/* still alive */
		break;
	}

	printf("0x%03x [%u]", frame.can_id, frame.len);
	for (i = 0; i < frame.len; i++) {
		printf(" %02x", frame.data[i]);
	}
	printf("\n");

	switch (frame.can_id) {
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
			    byd2h12(&frame, 1 + (2 * i)));
		}
		break;
	case 0x444:
		byd_kv_update(bg, BYD_VOLTAGE, byd2h12(&frame, 0));
		break;
	case 0x447:
		uint16_t soc = byd2h12(&frame, 4);
		byd_kv_update(bg, BYD_SOC, soc);
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

			rv = write(fd, &ack, sizeof(ack));
			if (rv == -1)
				lwarn("pid ack write");
		}

		switch (can2h16(&frame, 2)) {
		case BYD_PID_BATTERY_SOC:
			printf("pid soc %u\n", frame.data[4]);
			break;
		case BYD_PID_BATTERY_VOLTAGE:
			printf("pid voltage %u\n", byd2h16(&frame, 4));
			break;
		case BYD_PID_BATTERY_CURRENT:
			printf("pid current %.1f\n",
			    (byd2h16(&frame, 4) - 5000) / 10.0);
			break;
		case BYD_PID_CELL_TEMP_MIN:
			byd_kv_update(bg, BYD_MIN_TEMP,
			    byd2degc(&frame, 4));
			printf("pid cell temp min %u\n", frame.data[4] - 40);
			break;
		case BYD_PID_CELL_TEMP_MAX:
			byd_kv_update(bg, BYD_MAX_TEMP,
			    byd2degc(&frame, 4));
			printf("pid cell temp max %u\n", frame.data[4] - 40);
			break;
		case BYD_PID_CELL_TEMP_AVG:
			byd_kv_update(bg, BYD_AVG_TEMP,
			    byd2degc(&frame, 4));
			printf("pid cell temp avg %u\n", frame.data[4] - 40);
			break;
		case BYD_PID_CELL_MV_MIN:
			printf("pid cell mv min %u\n", byd2h16(&frame, 4));
			break;
		case BYD_PID_CELL_MV_MAX:
			printf("pid cell mv max %u\n", byd2h16(&frame, 4));
			break;
		case BYD_PID_MAX_CHARGE_POWER:
			printf("pid max charge power %uW\n",
			    byd2h16(&frame, 4) * 100);
			break;
		case BYD_PID_MAX_DISCHARGE_POWER:
			printf("pid max discharge power %uW\n",
			    byd2h16(&frame, 4) * 100);
			break;
		case BYD_PID_CHARGE_TIMES:
			printf("pid charge times %u\n",
			    byd2h16(&frame, 4));
			break;
		case BYD_PID_TOTAL_CHARGED_AH:
			printf("pid total charged ah %u\n",
			    byd2h16(&frame, 4));
			break;
		case BYD_PID_TOTAL_DISCHARGED_AH:
			printf("pid total discharged ah %u\n",
			    byd2h16(&frame, 4));
			break;
		case BYD_PID_TOTAL_CHARGED_KWH:
			printf("pid total charged kwh %u\n",
			    byd2h16(&frame, 4));
			break;
		case BYD_PID_TOTAL_DISCHARGED_KWH:
			printf("pid total discharged kwh %u\n",
			    byd2h16(&frame, 4));
			break;
		}
		break;
	}
}

static int
can_open(const char *name)
{
	struct ifreq ifr;
	struct sockaddr_can can;
	int fd;

	memset(&ifr, 0, sizeof(ifr));
	if (strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name)) >=
	    sizeof(ifr.ifr_name))
		errx(1, "%s: name too long", name);

	fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
	if (fd == -1)
		err(1, "%s socket", name);

	if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1)
		err(1, "%s index", name);

	memset(&can, 0, sizeof(can));
	can.can_family = AF_CAN;
	can.can_ifindex = ifr.ifr_ifindex;

	if (bind(fd, (struct sockaddr *)&can, sizeof(can)) == -1)
		err(1, "%s bind", name);

	return (fd);
}
