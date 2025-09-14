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

struct batgw;

enum batgw_kv_type {
	KV_T_TEMP,
	KV_T_VOLTAGE,
	KV_T_CURRENT,
	KV_T_POWER,
	KV_T_AMPHOUR,
	KV_T_WATTHOUR,
	KV_T_ENERGY, /* kwh */
	KV_T_PERCENT,
	KV_T_COUNT,
	KV_T_RAW,

	KV_T_MAXTYPE
};

struct batgw_kv {
	char			 kv_key[16];
	int			 kv_v;
	unsigned int		 kv_updated;
	enum batgw_kv_type	 kv_type;
	unsigned int		 kv_precision;
};

void	batgw_kv_init(struct batgw_kv *, const char *key,
	    enum batgw_kv_type, unsigned int precision);
void	batgw_kv_update(struct batgw *, const char *,
	    struct batgw_kv *, int);
void	batgw_kv_publish(struct batgw *, const char *,
	    const struct batgw_kv *);
int	batgw_kv_get(const struct batgw_kv *);

struct batgw_kv_tpl {
	const char		*kv_key;
	enum batgw_kv_type	 kv_type;
	unsigned int		 kv_precision;
};

void	batgw_kv_init_tpl(struct batgw_kv *, const struct batgw_kv_tpl *);

struct batgw_battery {
	int	 (*b_check)(const struct batgw_config_battery *);
	void	 (*b_config)(struct batgw_config_battery *);
	void	*(*b_attach)(struct batgw *);
	void	 (*b_dispatch)(struct batgw *, void *);
	void	 (*b_teleperiod)(struct batgw *, void *);
};

struct batgw_inverter {
	int	 (*i_check)(const struct batgw_config_inverter *);
	void	 (*i_config)(struct batgw_config_inverter *);
	void	*(*i_attach)(struct batgw *);
	void	 (*i_dispatch)(struct batgw *, void *);
	void	 (*i_teleperiod)(struct batgw *, void *);
};

struct event_base	*batgw_event_base(struct batgw *);
unsigned int		 batgw_verbose(const struct batgw *);

void		*batgw_b_softc(struct batgw *);
const struct batgw_config_battery *
		 batgw_b_config(struct batgw *);

void		 batgw_b_set_running(struct batgw *);
void		 batgw_b_set_stopped(struct batgw *);
void		 batgw_b_set_rated_capacity_ah(struct batgw *, unsigned int);
void		 batgw_b_set_rated_voltage_dv(struct batgw *, unsigned int);
void		 batgw_b_set_rated_capacity_wh(struct batgw *, unsigned int);
void		 batgw_b_set_max_voltage_dv(struct batgw *, unsigned int);
void		 batgw_b_set_min_voltage_dv(struct batgw *, unsigned int);
void		 batgw_b_set_max_voltage_dv(struct batgw *, unsigned int);
void		 batgw_b_set_soc_c_pct(struct batgw *, unsigned int);
void		 batgw_b_set_voltage_dv(struct batgw *, unsigned int);
void		 batgw_b_set_current_da(struct batgw *, int);
void		 batgw_b_set_min_temp_dc(struct batgw *, int);
void		 batgw_b_set_max_temp_dc(struct batgw *, int);
void		 batgw_b_set_avg_temp_dc(struct batgw *, int);
void		 batgw_b_set_min_cell_voltage_mv(struct batgw *, unsigned int);
void		 batgw_b_set_max_cell_voltage_mv(struct batgw *, unsigned int);

void		 batgw_b_set_charge_w(struct batgw *, unsigned int);
void		 batgw_b_set_discharge_w(struct batgw *, unsigned int);

int		 batgw_b_get_running(const struct batgw *);
unsigned int	 batgw_b_get_contactor(const struct batgw *);

void		*batgw_i_softc(struct batgw *);
const struct batgw_config_inverter *
		 batgw_i_config(struct batgw *);

void		 batgw_i_set_running(struct batgw *);
void		 batgw_i_set_stopped(struct batgw *);
void		 batgw_i_set_contactor(struct batgw *, unsigned int);

int		 batgw_i_get_min_temp_dc(const struct batgw *, int *);
int		 batgw_i_get_max_temp_dc(const struct batgw *, int *);
int		 batgw_i_get_avg_temp_dc(const struct batgw *, int *);
int		 batgw_i_get_rated_capacity_wh(const struct batgw *,
		     unsigned int *);
int		 batgw_i_get_rated_capacity_ah(const struct batgw *,
		     unsigned int *);
int		 batgw_i_get_remaining_capacity_ah(const struct batgw *,
		     unsigned int *);
int		 batgw_i_get_min_voltage_dv(const struct batgw *,
		     unsigned int *);
int		 batgw_i_get_max_voltage_dv(const struct batgw *,
		     unsigned int *);
int		 batgw_i_get_soc_cpct(const struct batgw *, unsigned int *);
int		 batgw_i_get_voltage_dv(const struct batgw *, unsigned int *);
int		 batgw_i_get_current_da(const struct batgw *, int *);

unsigned int	 batgw_i_get_safety(struct batgw *);
int		 batgw_i_issafe(struct batgw *, unsigned int);
unsigned int	 batgw_i_get_charge_da(struct batgw *, unsigned int);
unsigned int	 batgw_i_get_discharge_da(struct batgw *, unsigned int);

int		 can_open(const char *, const char *);
uint16_t	 can_betoh16(const struct can_frame *, size_t);
uint32_t	 can_betoh32(const struct can_frame *, size_t);
uint16_t	 can_letoh16(const struct can_frame *, size_t);
void		 can_htobe16(struct can_frame *, size_t, uint16_t);
void		 can_htole16(struct can_frame *, size_t, uint16_t);
