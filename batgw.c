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

#include "log.h"
#include "amqtt.h"

#include "batgw_config.h"
#include "batgw.h"

#define BATGW_CONFFILE "/etc/batgw.conf"

struct batgw_mqtt;

struct batgw_b_state {
	unsigned int		 bs_running;

	unsigned int		 bs_rated_capacity_ah;
	unsigned int		 bs_rated_voltage_dv;
	unsigned int		 bs_rated_capacity_wh;

	unsigned int		 bs_min_voltage_dv;
	unsigned int		 bs_max_voltage_dv;

        unsigned int             bs_max_charge_w;
        unsigned int             bs_max_discharge_w;

	unsigned int		 bs_min_cell_voltage_mv;
	unsigned int		 bs_max_cell_voltage_mv;

	unsigned int		 bs_valid;
#define BATGW_B_VALID_SOC		(1 << 0)
#define BATGW_B_VALID_VOLTAGE		(1 << 1)
#define BATGW_B_VALID_CURRENT		(1 << 2)
#define BATGW_B_VALID_MIN_TEMP		(1 << 4)
#define BATGW_B_VALID_MAX_TEMP		(1 << 5)
#define BATGW_B_VALID_AVG_TEMP		(1 << 6)


        unsigned int             bs_soc_cpct;
        unsigned int             bs_voltage_dv;
	int			 bs_current_da;

        int                      bs_min_temp_dc;
        int                      bs_max_temp_dc;
        int                      bs_avg_temp_dc;
};

struct batgw_i_state {
	unsigned int		 is_running;
	unsigned int		 is_contactor;
};

struct batgw {
	const struct batgw_config *
				 bg_conf;
	unsigned int		 bg_verbose;

	struct event_base	*bg_evbase;

	struct batgw_mqtt	*bg_mqtt;

	const struct batgw_battery
				*bg_battery;
	void			*bg_battery_sc;
	struct batgw_b_state	 bg_battery_state;

	const struct batgw_inverter *
				 bg_inverter;
	void			*bg_inverter_sc;
	struct batgw_i_state	 bg_inverter_state;

	const char		*bg_unsafe_reason;
};

extern const struct batgw_battery battery_byd;
extern const struct batgw_inverter inverter_byd_can;

static void	can_recv(int, short, void *);
static void	can_poll(int, short, void *);

static void	teleperiod(int, short, void *);
static const struct timeval teleperiod_tv = { 60, 0 };

static void	batgw_mqtt_init(struct batgw *);

static struct batgw _bg;

static unsigned int v_safe;
static unsigned int v_unsafe;

__dead static void
usage(void)
{
	const char *progname = getprogname();

	fprintf(stderr, "usage: %s [-dnv] [-D macro=value] [-f file]\n",
	    progname);

	exit(1);
}

int
main(int argc, char *argv[])
{
	struct batgw *bg = &_bg;

	const char *conffile = BATGW_CONFFILE;
	int confcheck = 0;
	int debug = 0;
	int ch;

	struct batgw_config *conf;
	struct batgw_config_mqtt *mqttconf;

	v_safe = arc4random();
	do {
		v_unsafe = arc4random();
	} while (v_unsafe == v_safe);

	while ((ch = getopt(argc, argv, "dD:f:nv")) != -1) {
		switch (ch) {
		case 'd':
			debug = 0;
			break;
		case 'D':
			if (cmdline_symset(optarg) < 0) {
				errx(1, "could not parse macro definition %s",
				    optarg);
			}
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'n':
			confcheck = 1;
			break;
		case 'v':
			bg->bg_verbose++;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	conf = parse_config(conffile);
	if (conf == NULL)
		exit(1);

	bg->bg_battery = &battery_byd; /* XXX */
	bg->bg_inverter = &inverter_byd_can; /* XXX */

	if (bg->bg_battery->b_check(&conf->battery) != 0)
		return (1);

	if (bg->bg_inverter->i_check(&conf->inverter) != 0)
		return (1);

	if (confcheck && !bg->bg_verbose) {
		dump_config(conf);
		return (0);
	}

	mqttconf = conf->mqtt;
	if (mqttconf != NULL) {
		if (mqttconf->port == NULL)
			mqttconf->port = BATGW_MQTT_PORT;
		if (mqttconf->topic == NULL)
			mqttconf->topic = BATGW_MQTT_TOPIC;
		if (mqttconf->clientid == NULL) {
			int rv = asprintf(&mqttconf->clientid, "%s-%d-%08x%08x",
			    getprogname(), getpid(),
			    arc4random(), arc4random());
			if (rv == -1)
				errx(1, "unable to create mqtt client id");
		}

		if (mqttconf->keepalive == BATGW_MQTT_KEEPALIVE_UNSET)
			mqttconf->keepalive = BATGW_MQTT_KEEPALIVE_DEFAULT;
		if (mqttconf->teleperiod == 0)
			mqttconf->teleperiod = BATGW_MQTT_TELEPERIOD;
		if (mqttconf->reconnect_tmo == 0)
			mqttconf->reconnect_tmo = 30;
	}

	if (conf->battery.max_charge_w == 0) {
		conf->battery.max_charge_w = BATGW_CHARGE_MAX_DEFAULT;
		conf->battery.charge_w = BATGW_CHARGE_DEFAULT;
	}
	if (conf->battery.max_discharge_w == 0) {
		conf->battery.max_discharge_w = BATGW_DISCHARGE_MAX_DEFAULT;
		conf->battery.discharge_w = BATGW_DISCHARGE_DEFAULT;
	}

	bg->bg_battery->b_config(&conf->battery);
	bg->bg_inverter->i_config(&conf->inverter);

	if (confcheck) {
		dump_config(conf);
		return (0);
	}

	/* let's try and get going */

	bg->bg_conf = conf;
	bg->bg_evbase = event_base_new();
	if (bg->bg_evbase == NULL)
		errx(1, "event_base_new failed");

	bg->bg_battery_sc = bg->bg_battery->b_attach(bg);
	bg->bg_inverter_sc = bg->bg_inverter->i_attach(bg);

	if (mqttconf != NULL)
		batgw_mqtt_init(bg);

	bg->bg_battery->b_dispatch(bg, bg->bg_battery_sc);
	bg->bg_inverter->i_dispatch(bg, bg->bg_inverter_sc);

	event_base_dispatch(bg->bg_evbase);

	return (0);
}

void
dump_config(const struct batgw_config *conf)
{
	const struct batgw_config_mqtt *mqtt = conf->mqtt;

	if (mqtt != NULL) {
		printf("mqtt {\n");

		switch (mqtt->af) {
		case PF_UNSPEC:
			break;
		case PF_INET:
			printf("\t" "ipv4\n");
			break;
		case PF_INET6:
			printf("\t" "ipv6\n");
			break;
		}
		printf("\t" "host \"%s\"" "\n", mqtt->host);
		if (mqtt->port != NULL)
			printf("\t" "port \"%s\"" "\n", mqtt->port);

		if (mqtt->user != NULL) {
			printf("\t" "username \"%s\" password \"%s\"" "\n",
			    mqtt->user, mqtt->pass);
		}
		if (mqtt->clientid != NULL) {
			printf("\t" "client id \"%s\"" "\n",
			    mqtt->clientid);
		}
		if (mqtt->topic != NULL) {
			printf("\t" "topic \"%s\"" "\n",
			    mqtt->topic);
		}
		if (mqtt->keepalive != BATGW_MQTT_KEEPALIVE_UNSET) {
			printf("\t" "keep alive ");
			if (mqtt->keepalive == BATGW_MQTT_KEEPALIVE_OFF)
				printf("off");
			else
				printf("%d", mqtt->keepalive);
			printf("\n");
		}
		if (mqtt->teleperiod != 0) {
			printf("\t" "teleperiod %u" "\n",
			    mqtt->teleperiod);
		}
		if (mqtt->connect_tmo != 0) {
			printf("\t" "connect timeout %u" "\n",
			    mqtt->connect_tmo);
		}
		if (mqtt->reconnect_tmo != 0) {
			printf("\t" "reconnect timeout %u" "\n",
			    mqtt->reconnect_tmo);
		}
		printf("}\n\n");
	}

	printf("battery {\n");
	printf("\t" "protocol \"%s\"" "\n", conf->battery.protocol);
	if (conf->battery.ifname)
		printf("\t" "interface \"%s\"" "\n", conf->battery.ifname);
	if (conf->battery.max_charge_w != 0) {
		printf("\t" "charge limit %u max %u\n",
		    conf->battery.max_charge_w, conf->battery.charge_w);
	}
	if (conf->battery.max_discharge_w != 0) {
		printf("\t" "discharge limit %u max %u\n",
		    conf->battery.max_discharge_w, conf->battery.discharge_w);
	}
	printf("}\n\n");

	printf("inverter {\n");
	printf("\t" "protocol \"%s\"" "\n", conf->inverter.protocol);
	if (conf->inverter.ifname)
		printf("\t" "interface \"%s\"" "\n", conf->inverter.ifname);
	printf("}\n");
}

static void
batgw_evtimer_add(struct event *evt, time_t seconds)
{
	struct timeval tv = { .tv_sec = seconds };
	evtimer_add(evt, &tv);
}

/*
 * mqtt functionality
 */

static void	batgw_mqtt_disconnect(struct batgw *);
static void	batgw_mqtt_teleperiod(int, short, void *);

struct batgw_mqtt {
	struct evdns_base		*evdnsbase;
	struct event			*ev_to_reconnect;
	struct event			*ev_to_teleperiod;

	struct evutil_addrinfo		 hints;
	struct evutil_addrinfo		*res0;
	struct evutil_addrinfo		*resn;
	struct evdns_getaddrinfo_request
					*req;

	const char			*will_topic;
	size_t				 will_topic_len;

	struct mqtt_conn		*conn;
	struct event			*ev_rd;
	struct event			*ev_wr;
	struct event			*ev_to;

	unsigned int			 running;
};

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

void
batgw_mqtt_rd(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mqtt_conn *mc = bg->bg_mqtt->conn;
	char buf[16384];
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
		lwarnx("disconnected");
		batgw_mqtt_disconnect(bg);
		return;
	default:
		break;
	}

	mqtt_input(mc, buf, rv);
}

void
batgw_mqtt_wr(int fd, short events, void *arg)
{
	struct batgw *bg = arg;
	struct mqtt_conn *mc = bg->bg_mqtt->conn;

	mqtt_output(mc);
}

static void
batgw_mqtt_want_output(struct mqtt_conn *mc)
{
	struct batgw *bg = mqtt_cookie(mc);
	event_add(bg->bg_mqtt->ev_wr, NULL);
}

static ssize_t
batgw_mqtt_output(struct mqtt_conn *mc, const void *buf, size_t len)
{
	struct batgw *bg = mqtt_cookie(mc);
	int fd = EVENT_FD(bg->bg_mqtt->ev_wr);
	ssize_t rv;

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
	struct mqtt_conn *mc = bg->bg_mqtt->conn;

	mqtt_timeout(mc);
}

static void
batgw_mqtt_want_timeout(struct mqtt_conn *mc, const struct timespec *ts)
{
	struct batgw *bg = mqtt_cookie(mc);
	struct timeval tv;

	TIMESPEC_TO_TIMEVAL(&tv, ts);

	event_add(bg->bg_mqtt->ev_to, &tv);
}

static void
batgw_mqtt_on_connect(struct mqtt_conn *mc)
{
	struct batgw *bg = mqtt_cookie(mc);
	struct batgw_mqtt *bgm = bg->bg_mqtt;
	static const char online[] = "Online";

	if (mqtt_publish(mc,
	    bgm->will_topic, bgm->will_topic_len,
	    online, sizeof(online) - 1, MQTT_QOS0, MQTT_RETAIN) == -1) {
		warnx("mqtt publish %s %s", bg->bg_mqtt->will_topic, online);
		batgw_mqtt_disconnect(bg);
		return;
	}

	bgm->running = 1;

	batgw_mqtt_teleperiod(0, 0, bg);
}

static void
batgw_mqtt_on_suback(struct mqtt_conn *mc, void *cookie,
    const uint8_t *rcodes, size_t nrcodes)
{
	/* cool */
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

static inline void
batgw_mqtt_reconnect(struct batgw *bg)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;

	batgw_evtimer_add(bgm->ev_to_reconnect, mqttconf->reconnect_tmo);
}

static void
batgw_mqtt_disconnect(struct batgw *bg)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;
	int s = EVENT_FD(bgm->ev_rd);

	bgm->running = 0;

	event_del(bgm->ev_to_teleperiod);
	event_del(bgm->ev_to);
	event_del(bgm->ev_wr);
	event_del(bgm->ev_rd);

	mqtt_conn_destroy(bgm->conn);

	event_free(bgm->ev_to);
	event_free(bgm->ev_wr);
	event_free(bgm->ev_rd);

	close(s);

	batgw_mqtt_reconnect(bg);
}

static void
batgw_mqtt_connected(struct batgw *bg, int s)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;

	static const char offline[] = "Offline";
	struct mqtt_conn_settings mcs = {
		.clean_session = 1,
		.keep_alive = mqttconf->keepalive,

		.clientid = mqttconf->clientid,
		.clientid_len = strlen(mqttconf->clientid),

		.will_topic = bgm->will_topic,
		.will_topic_len = bgm->will_topic_len,
		.will_payload = offline,
		.will_payload_len = sizeof(offline) - 1,
		.will_retain = 1,
	};
	struct mqtt_conn *mc;

	evutil_freeaddrinfo(bgm->res0);
	bgm->res0 = NULL;

	mc = mqtt_conn_create(&batgw_mqtt_settings, bg);
	if (mc == NULL) {
		lwarnx("unable to create mqtt connection");
		goto reconnect;
	}

	bgm->ev_rd = event_new(bg->bg_evbase, s, EV_READ|EV_PERSIST,
	    batgw_mqtt_rd, bg);
	if (bgm->ev_rd == NULL) {
		lwarnx("new mqtt ev rd failed");
		goto destroy;
	}

	bgm->ev_wr = event_new(bg->bg_evbase, s, EV_WRITE,
	    batgw_mqtt_wr, bg);
	if (bgm->ev_wr == NULL) {
		lwarnx("new mqtt ev wr failed");
		goto rd_free;
	}

	bgm->ev_to = evtimer_new(bg->bg_evbase,
	    batgw_mqtt_to, bg);
	if (bgm->ev_to == NULL) {
		lwarnx("new mqtt ev to failed");
		goto wr_free;
	}

	bgm->conn = mc;
	if (mqtt_connect(mc, &mcs) == -1) {
		lwarnx("mqtt_connect server %s port %s",
		    mqttconf->host, mqttconf->port);
		goto to_free;
	}

	linfo("connected to mqtt server %s port %s",
	    mqttconf->host, mqttconf->port);

	event_add(bgm->ev_rd, NULL);
	return;

to_free:
	event_free(bgm->ev_to);
wr_free:
	event_free(bgm->ev_wr);
rd_free:
	event_free(bgm->ev_rd);
destroy:
	mqtt_conn_destroy(mc);
reconnect:
	close(s);
	batgw_mqtt_reconnect(bg);
}

static void
batgw_mqtt_connected_ev(int s, short events, void *arg)
{
	struct batgw *bg = arg;
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;
	int error;
	socklen_t slen = sizeof(error);

	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &slen) == -1)
		lerr(1, "%s getsockopt");
	if (error == EINPROGRESS)
		return;

	event_del(bg->bg_mqtt->ev_wr);
	event_free(bg->bg_mqtt->ev_wr);

	if (error != 0) {
		errno = error;
		lwarn("mqtt server %s port %s connect",
		    mqttconf->host, mqttconf->port);
		close(s);
		batgw_mqtt_connect(bg);
		return;
	}

	batgw_mqtt_connected(bg, s);
}

static void
batgw_mqtt_connect(struct batgw *bg)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;
	struct evutil_addrinfo *res;
	int s;

	while ((res = bgm->resn) != NULL) {
		bgm->resn = res->ai_next;

		s = socket(res->ai_family, res->ai_socktype|SOCK_NONBLOCK,
		    res->ai_protocol);
		if (s == -1)
			continue;

		if (connect(s, res->ai_addr, res->ai_addrlen) == -1) {
			if (errno == EINPROGRESS) {
				bg->bg_mqtt->ev_wr = event_new(bg->bg_evbase,
				    s, EV_WRITE|EV_PERSIST,
				    batgw_mqtt_connected_ev, bg);
				if (bg->bg_mqtt->ev_wr == NULL)
					lerr(1, "mqtt connect event_new");

				event_add(bg->bg_mqtt->ev_wr, NULL);
				return;
			}

			lwarn("mqtt server %s port %s connect",
			    mqttconf->host, mqttconf->port);
			close(s);
			continue;
		}

		batgw_mqtt_connected(bg, s);
		return;
	}

	errno = EHOSTUNREACH;
	lwarn("mqtt server %s port %s", mqttconf->host, mqttconf->port);
	evutil_freeaddrinfo(bgm->res0);
	bgm->res0 = NULL;

	batgw_mqtt_reconnect(bg);
}

static void
batgw_mqtt_addrinfo(int errcode, struct evutil_addrinfo *res0, void *arg)
{
	struct batgw *bg = arg;
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;
	struct evutil_addrinfo *res;
	int s = -1;
	int serrno;
	const char *cause;

	if (errcode != 0) {
		lwarnx("mqtt server %s port %s: %s",
		    mqttconf->host, mqttconf->port,
		    evutil_gai_strerror(errcode));
		batgw_mqtt_reconnect(bg);
		return;
	}

	bgm->res0 = res0;
	bgm->resn = res0;

	batgw_mqtt_connect(bg);
}

static void
batgw_mqtt_start(struct batgw *bg)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;

	bgm->req = evdns_getaddrinfo(bgm->evdnsbase,
	    mqttconf->host, mqttconf->port, &bgm->hints,
	    batgw_mqtt_addrinfo, bg);
	if (bgm->req == NULL) {
		lwarnx("mqtt server %s port %s: resolve failed",
		    mqttconf->host, mqttconf->port);
		batgw_mqtt_reconnect(bg);
		return;
	}
}

static void
batgw_mqtt_to_reconnect(int nil, short events, void *arg)
{
	struct batgw *bg = arg;

	ldebug("%s", __func__);
	batgw_mqtt_start(bg);
}

static void
batgw_mqtt_init(struct batgw *bg)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm;
	char *topic;
	int rv;

	bgm = calloc(1, sizeof(*bgm));
	if (bgm == NULL)
		err(1, "mqtt state alloc");

	bgm->ev_to_reconnect = evtimer_new(bg->bg_evbase,
	    batgw_mqtt_to_reconnect, bg);
	if (bgm->ev_to_reconnect == NULL)
		errx(1, "mqtt ev_to_connect evtimer_new failed");

	bgm->ev_to_teleperiod = evtimer_new(bg->bg_evbase,
	    batgw_mqtt_teleperiod, bg);
	if (bgm->ev_to_teleperiod == NULL)
		errx(1, "mqtt ev_to_teleperiod evtimer_new failed");

	rv = asprintf(&topic, "%s/LWT", mqttconf->topic);
	if (rv == -1)
		errx(1, "mqtt lwt topic printf error");
	bgm->will_topic = topic;
        bgm->will_topic_len = rv;

	bgm->evdnsbase = evdns_base_new(bg->bg_evbase,
	    EVDNS_BASE_INITIALIZE_NAMESERVERS);
	if (bgm->evdnsbase == NULL)
		errx(1, "mqtt evdns_base_new failed");

	bgm->hints.ai_family = mqttconf->af;
	bgm->hints.ai_socktype = SOCK_STREAM;
	bgm->hints.ai_protocol = IPPROTO_TCP;

	bg->bg_mqtt = bgm;

	batgw_mqtt_start(bg);
}

static int
batgw_mqtt_running(struct batgw *bg)
{
	const struct batgw_config_mqtt *mqttconf;
	struct batgw_mqtt *bgm;

	mqttconf = bg->bg_conf->mqtt;
	if (mqttconf == NULL)
		return (0);

	bgm = bg->bg_mqtt;
	if (bgm == NULL)
		return (0);

	return (bgm->running);
}

static void
batgw_mqtt_publish(struct batgw *bg,
    const char *t, size_t tlen, const char *p, size_t plen)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;

	if (mqtt_publish(bgm->conn, t, tlen, p, plen, MQTT_QOS0, 0) == -1)
		batgw_mqtt_disconnect(bg);
}

void
batgw_publish(struct batgw *bg,
    const char *t, size_t tlen, const char *p, size_t plen)
{
	if (!batgw_mqtt_running(bg))
		return;

	batgw_mqtt_publish(bg, t, tlen, p, plen);
}

static void
batgw_mqtt_teleperiod(int nil, short events, void *arg)
{
	struct batgw *bg = arg;
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;

	batgw_evtimer_add(bgm->ev_to_teleperiod, mqttconf->teleperiod);

	bg->bg_battery->b_teleperiod(bg, bg->bg_battery_sc);
	bg->bg_inverter->i_teleperiod(bg, bg->bg_inverter_sc);
}

static const char *const batgw_kv_type_names[KV_T_MAXTYPE] = {
	[KV_T_TEMP] =			"temperature",
	[KV_T_VOLTAGE] =		"voltage",
	[KV_T_CURRENT] =		"current",
	[KV_T_POWER] =			"power",
	[KV_T_AMPHOUR] =		"amphour",
	[KV_T_WATTHOUR] =		"watthour",
	[KV_T_ENERGY] =			"energy",
	[KV_T_PERCENT] =		"percent",
	[KV_T_COUNT] =			"count",
	[KV_T_RAW] =			"raw",
};

void
batgw_kv_init(struct batgw_kv *kv, const char *key, enum batgw_kv_type type,
    unsigned int precision)
{
	memset(kv, 0, sizeof(*kv));
	if (key != NULL) {
		if (strlcpy(kv->kv_key, key, sizeof(kv->kv_key)) >=
		    sizeof(kv->kv_key)) {
			warnx("%s: key too long", __func__);
			abort();
		}
	}
	kv->kv_v = INT_MIN;
	kv->kv_type = type;
	kv->kv_precision = precision;
}

void
batgw_kv_init_tpl(struct batgw_kv *kv, const struct batgw_kv_tpl *tpl)
{
	batgw_kv_init(kv, tpl->kv_key, tpl->kv_type, tpl->kv_precision);
}

void
batgw_kv_publish(struct batgw *bg,
    const char *scope, const struct batgw_kv *kv)
{
	const struct batgw_config_mqtt *mqttconf = bg->bg_conf->mqtt;
	struct batgw_mqtt *bgm = bg->bg_mqtt;

	char t[128];
	int tlen;
	char p[128];
	int plen;
	int div = 1;
	int len;

	if (!batgw_mqtt_running(bg))
		return;

	/* XXX this should handle snprintf return values better */

	tlen = snprintf(t, sizeof(t), "%s", mqttconf->topic);
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
	    "/%s", batgw_kv_type_names[kv->kv_type]);
	tlen += len;

	if (kv->kv_precision == 0)
		plen = snprintf(p, sizeof(p), "%d", kv->kv_v);
	else {
		static unsigned int divs[] = { 1, 10, 100, 1000, 10000 };
		const char *neg = "";
		unsigned int div;
		unsigned int v;
		assert(kv->kv_precision < nitems(divs));
		div = divs[kv->kv_precision];
		if (kv->kv_v < 0) {
			v = 0 - kv->kv_v;
			neg = "-";
		} else
			v = kv->kv_v;
		plen = snprintf(p, sizeof(p), "%s%u.%0*u",
		    neg, v / div, kv->kv_precision, v % div);
	}

	mqtt_publish(bgm->conn, t, tlen, p, plen, MQTT_QOS0, 0);
}

void
batgw_kv_update(struct batgw *bg, const char *scope,
    struct batgw_kv *kv, int v)
{
	struct timeval tv;

	if (kv->kv_v == v)
		return;
	kv->kv_v = v;

	if (event_gettime_monotonic(bg->bg_evbase, &tv) == 0) {
		unsigned int now = (unsigned int)tv.tv_sec;
		if ((now - kv->kv_updated) < 10)
			return;
		kv->kv_updated = now;
	}

	batgw_kv_publish(bg, scope, kv);
}

int
batgw_kv_get(const struct batgw_kv *kv)
{
	return (kv->kv_v);
}

/*
 * CAN related code
 */

int
can_open(const char *scope, const char *name)
{
	struct ifreq ifr;
	struct sockaddr_can can;
	int fd;

	memset(&ifr, 0, sizeof(ifr));
	if (strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name)) >=
	    sizeof(ifr.ifr_name))
		errx(1, "%s %s: name too long", scope, name);

	fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
	if (fd == -1)
		err(1, "%s %s socket", scope, name);

	if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1)
		err(1, "%s %s index", scope, name);

	memset(&can, 0, sizeof(can));
	can.can_family = AF_CAN;
	can.can_ifindex = ifr.ifr_ifindex;

	if (bind(fd, (struct sockaddr *)&can, sizeof(can)) == -1)
		err(1, "%s %s bind", scope, name);

	return (fd);
}

uint16_t
can_betoh16(const struct can_frame *frame, size_t o)
{
	uint16_t h16;

	h16 = (uint16_t)frame->data[o + 0] << 8;
	h16 |= (uint16_t)frame->data[o + 1] << 0;

	return (h16);
}

uint32_t
can_betoh32(const struct can_frame *frame, size_t o)
{
	uint32_t h32;

	h32 = (uint16_t)frame->data[o + 0] << 24;
	h32 |= (uint16_t)frame->data[o + 1] << 16;
	h32 |= (uint16_t)frame->data[o + 2] << 8;
	h32 |= (uint16_t)frame->data[o + 3] << 0;

	return (h32);
}

void
can_htobe16(struct can_frame *frame, size_t o, uint16_t h16)
{
	frame->data[o + 0] = h16 >> 8;
	frame->data[o + 1] = h16 >> 0;
}

uint16_t
can_letoh16(const struct can_frame *frame, size_t o)
{
	uint16_t h16;

	h16 = (uint16_t)frame->data[o + 0] << 0;
	h16 |= (uint16_t)frame->data[o + 1] << 8;

	return (h16);
}

void
can_htole16(struct can_frame *frame, size_t o, uint16_t h16)
{
	frame->data[o + 0] = h16 >> 0;
	frame->data[o + 1] = h16 >> 8;
}

struct event_base *
batgw_event_base(struct batgw *bg)
{
	return (bg->bg_evbase);
}

unsigned int
batgw_verbose(const struct batgw *bg)
{
	return (bg->bg_verbose);
}

const struct batgw_config_battery *
batgw_b_config(struct batgw *bg)
{
	return (&bg->bg_conf->battery);
}

void *
batgw_b_softc(struct batgw *bg)
{
	return (bg->bg_battery_sc);
}

void
batgw_b_set_running(struct batgw *bg)
{
	bg->bg_battery_state.bs_running = 1;
}

void
batgw_b_set_stopped(struct batgw *bg)
{
	bg->bg_battery_state.bs_running = 0;
}

int
batgw_b_get_running(const struct batgw *bg)
{
	return (bg->bg_battery_state.bs_running);
}

void
batgw_b_set_rated_capacity_ah(struct batgw *bg, unsigned int ah)
{
	bg->bg_battery_state.bs_rated_capacity_ah = ah;
}

void
batgw_b_set_rated_voltage_dv(struct batgw *bg, unsigned int dv)
{
	bg->bg_battery_state.bs_rated_voltage_dv = dv;
}

void
batgw_b_set_soc_c_pct(struct batgw *bg, unsigned int soc)
{
	SET(bg->bg_battery_state.bs_valid, BATGW_B_VALID_SOC);
	bg->bg_battery_state.bs_soc_cpct = soc;
}

void
batgw_b_set_min_voltage_dv(struct batgw *bg, unsigned int dv)
{
	bg->bg_battery_state.bs_min_voltage_dv = dv;
}

void
batgw_b_set_max_voltage_dv(struct batgw *bg, unsigned int dv)
{
	bg->bg_battery_state.bs_min_voltage_dv = dv;
}

void
batgw_b_set_voltage_dv(struct batgw *bg, unsigned int dv)
{
	SET(bg->bg_battery_state.bs_valid, BATGW_B_VALID_VOLTAGE);
	bg->bg_battery_state.bs_voltage_dv = dv;
}

void
batgw_b_set_current_da(struct batgw *bg, int da)
{
	SET(bg->bg_battery_state.bs_valid, BATGW_B_VALID_CURRENT);
	bg->bg_battery_state.bs_current_da = da;
}

void
batgw_b_set_min_temp_dc(struct batgw *bg, int temp)
{
	SET(bg->bg_battery_state.bs_valid, BATGW_B_VALID_MIN_TEMP);
	bg->bg_battery_state.bs_min_temp_dc = temp;
}

void
batgw_b_set_max_temp_dc(struct batgw *bg, int temp)
{
	SET(bg->bg_battery_state.bs_valid, BATGW_B_VALID_MAX_TEMP);
	bg->bg_battery_state.bs_max_temp_dc = temp;
}

void
batgw_b_set_avg_temp_dc(struct batgw *bg, int temp)
{
	struct batgw_b_state *bs = &bg->bg_battery_state;

	SET(bs->bs_valid, BATGW_B_VALID_AVG_TEMP);
	bs->bs_avg_temp_dc = temp;
}

void
batgw_b_set_charge_w(struct batgw *bg, unsigned int w)
{
	struct batgw_b_state *bs = &bg->bg_battery_state;
	bs->bs_max_charge_w = w;
}

void
batgw_b_set_discharge_w(struct batgw *bg, unsigned int w)
{
	struct batgw_b_state *bs = &bg->bg_battery_state;
	bs->bs_max_discharge_w = w;
}

void
batgw_b_set_min_cell_voltage_mv(struct batgw *bg, unsigned int mv)
{
	struct batgw_b_state *bs = &bg->bg_battery_state;
	bs->bs_min_cell_voltage_mv = mv;
}

void
batgw_b_set_max_cell_voltage_mv(struct batgw *bg, unsigned int mv)
{
	struct batgw_b_state *bs = &bg->bg_battery_state;
	bs->bs_max_cell_voltage_mv = mv;
}

const struct batgw_config_inverter *
batgw_i_config(struct batgw *bg)
{
	return (&bg->bg_conf->inverter);
}

void *
batgw_i_softc(struct batgw *bg)
{
	return (bg->bg_inverter_sc);
}

void
batgw_i_set_running(struct batgw *bg)
{
	bg->bg_inverter_state.is_running = 1;
}

void
batgw_i_set_stopped(struct batgw *bg)
{
	bg->bg_inverter_state.is_running = 0;
}

void
batgw_i_set_contactor(struct batgw *bg, unsigned int closed)
{
	bg->bg_inverter_state.is_contactor = closed;
}

unsigned int
batgw_b_get_contactor(const struct batgw *bg)
{
	return (bg->bg_inverter_state.is_contactor);
}

static inline int
isset(unsigned int v, unsigned int m)
{
	return ((v & m) == m);
}

int
batgw_i_get_min_voltage_dv(const struct batgw *bg, unsigned int *dvp)
{
	unsigned int dv = bg->bg_battery_state.bs_min_voltage_dv;

	if (dv != 0) {
		*dvp = dv;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_max_voltage_dv(const struct batgw *bg, unsigned int *dvp)
{
	unsigned int dv = bg->bg_battery_state.bs_min_voltage_dv;

	if (dv != 0) {
		*dvp = dv;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_soc_cpct(const struct batgw *bg, unsigned int *cpctp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;

	if (ISSET(bs->bs_valid, BATGW_B_VALID_SOC)) {
		*cpctp = bs->bs_soc_cpct;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_voltage_dv(const struct batgw *bg, unsigned int *dvp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;

	if (ISSET(bs->bs_valid, BATGW_B_VALID_VOLTAGE)) {
		*dvp = bs->bs_voltage_dv;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_current_da(const struct batgw *bg, int *dap)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;

	if (ISSET(bs->bs_valid, BATGW_B_VALID_CURRENT)) {
		*dap = bs->bs_current_da;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_avg_temp_dc(const struct batgw *bg, int *tempp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	int diff;

	if (ISSET(bs->bs_valid, BATGW_B_VALID_AVG_TEMP)) {
		*tempp = bs->bs_avg_temp_dc;
		return (0);
	}

	if (!isset(bs->bs_valid,
	    BATGW_B_VALID_MIN_TEMP|BATGW_B_VALID_MAX_TEMP))
		return (-1);

	diff = bs->bs_max_temp_dc - bs->bs_min_temp_dc;
	*tempp = bs->bs_min_temp_dc + (diff / 2);
	return (0);
}

int
batgw_i_get_min_temp_dc(const struct batgw *bg, int *tempp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	int diff;

	if (ISSET(bs->bs_valid, BATGW_B_VALID_MIN_TEMP)) {
		*tempp = bs->bs_min_temp_dc;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_max_temp_dc(const struct batgw *bg, int *tempp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	int diff;

	if (ISSET(bs->bs_valid, BATGW_B_VALID_MAX_TEMP)) {
		*tempp = bs->bs_max_temp_dc;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_rated_capacity_ah(const struct batgw *bg, unsigned int *ahp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	unsigned int ah = bs->bs_rated_capacity_ah;

	if (ah != 0) {
		*ahp = ah;
		return (0);
	}

	return (-1);
}

int
batgw_i_get_rated_capacity_wh(const struct batgw *bg, unsigned int *whp)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	unsigned int wh;

	if (bs->bs_rated_capacity_wh != 0) {
		*whp = bs->bs_rated_capacity_wh;
		return (0);
	}

	/* this will be 0 if either or both are 0 */
	wh = bs->bs_rated_capacity_ah * bs->bs_rated_voltage_dv;
	if (wh != 0) {
		*whp = wh / 10;
		return (0);
	}

	return (-1);
}

unsigned int
batgw_i_get_safety(struct batgw *bg)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	const struct batgw_config_battery *bconf = batgw_b_config(bg);
	const char *reason;
	int diff;

	if (!bs->bs_running) {
		reason = "battery is not running";
		goto unsafe;
	}

	/*
	 * XXX call into the battery driver here so it can do it's own
	 * checks and prepare for the ones below
	 */

#define CHECK(_c, _r) do {	\
	if (!(_c)) {		\
		reason = (_r);	\
		goto unsafe;	\
	}			\
} while (0)

	CHECK(ISSET(bs->bs_valid, BATGW_B_VALID_MIN_TEMP),
	    "minimum battery temperature has not been reported");
	CHECK(ISSET(bs->bs_valid, BATGW_B_VALID_MAX_TEMP),
	    "maximum battery temperature has not been reported");

	CHECK(bs->bs_min_temp_dc >= -250 /* bconf->min_temp_dc */,
	    "battery is too cold");
	CHECK(bs->bs_max_temp_dc <= 500 /* bconf->max_temp_dc */,
	    "battery is too hot");
	CHECK(bs->bs_min_temp_dc <= bs->bs_max_temp_dc,
	    "battery min temp is higher than max temp");
	diff = bs->bs_max_temp_dc - bs->bs_min_temp_dc;
	CHECK(diff < 150 /* bconf->dev_tmp_dc */,
	    "battery temperature difference is too high");

	CHECK(bs->bs_min_cell_voltage_mv != 0,
	    "minimum cell voltage has not been reported");
	CHECK(bs->bs_max_cell_voltage_mv != 0,
	    "maximum cell voltage has not been reported");
	CHECK(bs->bs_min_cell_voltage_mv <= bs->bs_max_cell_voltage_mv,
	    "min cell voltage is higher than max cell voltage");
	diff = bs->bs_max_cell_voltage_mv - bs->bs_min_cell_voltage_mv;
	CHECK(diff < bconf->dev_cell_voltage_mv,
	    "battery cell voltage difference is too high");

	return (v_safe);

unsafe:
	if (bg->bg_unsafe_reason != reason) {
		lwarnx("battery unsafe: %s", reason);
		bg->bg_unsafe_reason = reason;
	}
	return (v_unsafe);
}

int
batgw_i_issafe(struct batgw *bg, unsigned int safety)
{
	if (safety == v_safe)
		return (1);
	if (safety == v_unsafe)
		return (0);
	abort();
}

static unsigned int
batgw_get_safety_limited_da(struct batgw *bg,
    unsigned int w, unsigned int wlimit)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	unsigned int dv, da;

	dv = bs->bs_voltage_dv;
	if (dv == 0)
		return (0);

	if (w > wlimit)
		w = wlimit;

	da = (w * 100) / dv;

	return (da);
}

unsigned int
batgw_i_get_charge_da(struct batgw *bg, unsigned int safety)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	const struct batgw_config_battery *bconf = batgw_b_config(bg);

	if (!batgw_i_issafe(bg, safety))
		return (0);

	if (bs->bs_max_cell_voltage_mv > bconf->max_cell_voltage_mv)
		return (0);

	return (batgw_get_safety_limited_da(bg,
	    bs->bs_max_charge_w, bconf->charge_w));
}

unsigned int
batgw_i_get_discharge_da(struct batgw *bg, unsigned int safety)
{
	const struct batgw_b_state *bs = &bg->bg_battery_state;
	const struct batgw_config_battery *bconf = batgw_b_config(bg);

	if (!batgw_i_issafe(bg, safety))
		return (0);

	if (bs->bs_min_cell_voltage_mv < bconf->min_cell_voltage_mv)
		return (0);

	return (batgw_get_safety_limited_da(bg,
	    bs->bs_max_discharge_w, bconf->discharge_w));
}
