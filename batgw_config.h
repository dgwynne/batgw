
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

#define BATGW_MQTT_PORT			"1883"
#define BATGW_MQTT_TOPIC		"battery-gateway"

#define BATGW_MQTT_TELEPERIOD		300 /* seconds */
#define BATGW_MQTT_TELEPERIOD_MIN	4
#define BATGW_MQTT_TELEPERIOD_MAX	3600

#define BATGW_MQTT_KEEPALIVE_UNSET	-1
#define BATGW_MQTT_KEEPALIVE_OFF	0
#define BATGW_MQTT_KEEPALIVE_DEFAULT	30

struct batgw_config_mqtt {
	int		 af;
	char		*host;
	char		*port;
	char		*user;
	char		*pass;
	char		*clientid;
	char		*topic;

	int		 keepalive;
	unsigned int	 teleperiod;
	unsigned int	 connect_tmo;		/* approx seconds */
	unsigned int	 reconnect_tmo;		/* approx seconds */
};

struct batgw_config_battery {
	char		*protocol;
	char		*ifname;

	unsigned int	 rated_capacity_ah;
	unsigned int	 rated_voltage_dv;
	unsigned int	 rated_capacity_wh;

	unsigned int	 min_voltage_dv;
	unsigned int	 max_voltage_dv;

	unsigned int	 ncells;
	unsigned int	 min_cell_voltage_mv;
	unsigned int	 max_cell_voltage_mv;
	unsigned int	 dev_cell_voltage_mv;

	unsigned int	 max_charge_w;
	unsigned int	 max_discharge_w;
};

struct batgw_config_inverter {
	char		*protocol;
	char		*ifname;
};

struct batgw_config {
	struct batgw_config_mqtt	*mqtt;
	struct batgw_config_battery	 battery;
	struct batgw_config_inverter	 inverter;
};

struct batgw_config	*parse_config(const char *);
void			 dump_config(const struct batgw_config *);
void			 clear_config(struct batgw_config *);
int			 cmdline_symset(const char *);
