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

#include <event.h>

enum batgw_kv_type {
        KV_T_TEMPERATURE,
        KV_T_VOLTAGE,
        KV_T_PERCENT,

        KV_T_MAXTYPE
};

struct batgw_kv {
	char			 kv_key[16];
	int			 kv_v;
	enum batgw_kv_type	 kv_type;
	unsigned int		 kv_precision;
	time_t			 kv_updated;
};

struct batgw_b_config {
	const char		*bc_name;
	unsigned int		 bc_cells;
	unsigned int		 bc_capacity_wh;

	unsigned int		 bc_max_pack_dv;
	unsigned int		 bc_min_pack_dv;

	unsigned int		 bc_max_cell_mv;
	unsigned int		 bc_min_cell_mv;
	unsigned int		 bc_max_cell_gap_mv;
};

struct batgw_b_state {
	unsigned int		 bs_valid;
#define BEMU_B_VALID_SOC		(1 << 0)
#define BEMU_B_VALID_VOLTAGE		(1 << 1)
#define BEMU_B_VALID_MAX_CHARGE		(1 << 2)
#define BEMU_B_VALID_MAX_DISCHARGE	(1 << 3)
#define BEMU_B_VALID_MIN_TEMP		(1 << 4)
#define BEMU_B_VALID_MAX_TEMP		(1 << 5)
#define BEMU_B_VALID_AVG_TEMP		(1 << 6)

	unsigned int		 bs_soc;
	int			 bs_current_;
	unsigned int		 bs_voltage_dv;
	unsigned int		 bs_max_charge_w;
	unsigned int		 bs_max_discharge_w;

	unsigned int		 bs_min_temp_c;
	unsigned int		 bs_max_temp_c;
	unsigned int		 bs_avg_temp_c;

};

struct batgw_i_config {
	const char		*ic_name;
};

struct mqtt_conn;
struct evutil_addrinfo;
struct evdns_getaddrinfo_request;

struct batgw {
	struct event_base	*bg_evbase;
	struct evdns_base	*bg_evdnsbase;

	const struct bemu_b_config
				*bg_b_config;
	void			*bg_b_private;

	struct batgw_b_state	 bg_b_state;

	struct {
		struct evutil_addrinfo	*hints;
		struct evutil_addrinfo	*res0;
		struct evutil_addrinfo	*resn;
		struct evdns_getaddrinfo_request
					*req;

		unsigned int		 keep_alive;
		const char		*host;
		const char		*port;

		const char		*devname;
		const char		*will_topic;
		size_t			 will_topic_len;
	}			 bg_mqtt;

	struct mqtt_conn	*bg_mqtt_conn;
	int			 bg_mqtt_fd;
	struct event		*bg_mqtt_ev_rd;
	struct event		*bg_mqtt_ev_wr;
	struct event		*bg_mqtt_ev_to;
};
