/*	$OpenBSD$ */

/*
 * Copyright (c) 2010 David Gwynne <dlg@openbsd.org>
 * Copyright (c) 2004, 2005 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2004 Ryan McBride <mcbride@openbsd.org>
 * Copyright (c) 2002, 2003, 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
 * Copyright (c) 2001 Daniel Hartmeier.  All rights reserved.
 * Copyright (c) 2001 Theo de Raadt.  All rights reserved.
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

%{
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bsd/sys/queue.h>
#include <bsd/stdlib.h>

#include "batgw_config.h"

TAILQ_HEAD(files, file)		 files = TAILQ_HEAD_INITIALIZER(files);
static struct file {
	TAILQ_ENTRY(file)	 entry;
	FILE			*stream;
	char			*name;
	size_t			 ungetpos;
	size_t			 ungetsize;
	u_char			*ungetbuf;
	int			 eof_reached;
	int			 lineno;
	int			 errors;
} *file, *topfile;
struct file	*pushfile(const char *, int);
int		 popfile(void);
int		 yyparse(void);
int		 yylex(void);
int		 yyerror(const char *, ...)
    __attribute__((__format__ (printf, 1, 2)))
    __attribute__((__nonnull__ (1)));
int		 kw_cmp(const void *, const void *);
int		 lookup(char *);
int		 igetc(void);
int		 lgetc(int);
void		 lungetc(int);
int		 findeol(void);

TAILQ_HEAD(symhead, sym)	symhead = TAILQ_HEAD_INITIALIZER(symhead);
struct sym {
	TAILQ_ENTRY(sym)         entry;
	int			 used;
	int			 persist;
	char			*nam;
	char                    *val;
};
int              symset(const char *, const char *, int);
char            *symget(const char *);

static int			 errors;
static struct batgw_config	*conf;

static const char *check_mqtt_topic(const char *);

typedef struct {
	union {
		int			 i;
		int64_t			 number;
		char			*string;
	} v;
	int lineno;
} YYSTYPE;

%}

%token	MQTT HOST PORT USERNAME PASSWORD CLIENT ID TOPIC TELEPERIOD RECONNECT
%token	KEEP ALIVE OFF
%token	INET INET6 IPV4 IPV6
%token	BATTERY CHARGE DISCHARGE LIMIT MAX
%token	INVERTER
%token	PROTOCOL INTERFACE
%token	INCLUDE
%token	ERROR
%token	<v.string>		STRING
%token	<v.number>		NUMBER
%type	<v.number>		limit_max
%type	<v.i>			af mqtt_keepalive
%type	<v.string>		string

%%

grammar		: /* empty */
		| grammar '\n'
		| grammar include '\n'
		| grammar varset '\n'
		| grammar mqtt '\n'
		| grammar battery '\n'
		| grammar inverter '\n'
		| grammar error '\n'		{ file->errors++; }
		;

include		: INCLUDE STRING		{
			struct file	*nfile;

			if ((nfile = pushfile($2, 1)) == NULL) {
				yyerror("failed to include file %s", $2);
				free($2);
				YYERROR;
			}
			free($2);

			file = nfile;
			lungetc('\n');
		}
		;

string		: string STRING	{
			if (asprintf(&$$, "%s %s", $1, $2) == -1) {
				free($1);
				free($2);
				yyerror("string: asprintf");
				YYERROR;
			}
			free($1);
			free($2);
		}
		| STRING
		;

varset		: STRING '=' string		{
			char *s = $1;
			while (*s++) {
				if (isspace((unsigned char)*s)) {
					yyerror("macro name cannot contain "
					    "whitespace");
					free($1);
					free($3);
					YYERROR;
				}
			}
			if (symset($1, $3, 0) == -1)
				err(1, "cannot store variable");
			free($1);
			free($3);
		}
		;

optnl		: nl
		|
		;

nl		: '\n' optnl		/* one or more newlines */
		;

mqtt		: MQTT {
			if (conf->mqtt != NULL) {
				yyerror("mqtt is already configured");
				YYERROR;
			}

			conf->mqtt = calloc(1, sizeof(*conf->mqtt));
			if (conf->mqtt == NULL)
				err(1, "mqtt configuration");

			conf->mqtt->af = AF_UNSPEC;
			conf->mqtt->keepalive =
			    BATGW_MQTT_KEEPALIVE_UNSET;
		} '{' optnl mqttopts_l '}' {
			if (conf->mqtt->host == NULL) {
				yyerror("mqtt host not specified");
				YYERROR;
			}

			if ((conf->mqtt->user == NULL) !=
			    (conf->mqtt->pass == NULL)) {
				yyerror("mqtt username and password "
				    "not configured together");
				YYERROR;
			}
		}
		;

mqttopts_l	: mqttopts_l mqttopts optnl
		| mqttopts optnl
		;

mqttopts	: HOST STRING {
			if (conf->mqtt->host != NULL) {
				yyerror("mqtt host is already configured");
				free($2);
				YYERROR;
			}
			conf->mqtt->host = $2;
		}
		| PORT STRING {
			if (conf->mqtt->port != NULL) {
				yyerror("mqtt port is already configured");
				free($2);
				YYERROR;
			}
			conf->mqtt->port = $2;
		}
		| af {
			if (conf->mqtt->af != PF_UNSPEC) {
				yyerror("mqtt af is already configured");
				YYERROR;
			}
			conf->mqtt->af = $1;
		}
		| USERNAME STRING PASSWORD STRING {
			if (conf->mqtt->user != NULL) {
				yyerror("mqtt username "
				    "is already configured");
				free($2);
				YYERROR;
			}
			conf->mqtt->user = $2;
		}
		| CLIENT ID STRING {
			if (conf->mqtt->clientid != NULL) {
				yyerror("mqtt client id "
				    "is already configured");
				free($3);
				YYERROR;
			}
			conf->mqtt->clientid = $3;
		}
		| KEEP ALIVE mqtt_keepalive {
			if (conf->mqtt->keepalive != 0) {
				yyerror("mqtt keep alive "
				    "is already configured");
				YYERROR;
			}

			conf->mqtt->keepalive = $3;
		}
		| TOPIC STRING {
			const char *errstr;

			if (conf->mqtt->topic != NULL) {
				yyerror("mqtt topic is already configured");
				free($2);
				YYERROR;
			}

			errstr = check_mqtt_topic($2);
			if (errstr != NULL) {
				yyerror("mqtt topic: %s", errstr);
				free($2);
				YYERROR;
			}

			conf->mqtt->topic = $2;
		}
		| TELEPERIOD NUMBER {
			if (conf->mqtt->teleperiod != 0) {
				yyerror("mqtt teleperiod "
				    "is already configured");
				YYERROR;
			}
			if ($2 < BATGW_MQTT_TELEPERIOD_MIN) {
				yyerror("mqtt teleperiod is too short");
				YYERROR;
			}
			if ($2 > BATGW_MQTT_TELEPERIOD_MAX) {
				yyerror("mqtt teleperiod is too long");
				YYERROR;
			}

			conf->mqtt->teleperiod = $2;
		}
		| RECONNECT NUMBER {
			if (conf->mqtt->reconnect_tmo != 0) {
				yyerror("mqtt reconnect "
				    "is already configured");
				YYERROR;
			}
			if ($2 < 2) {
				yyerror("mqtt reconnect is too short");
				YYERROR;
			}
			if ($2 > 300) {
				yyerror("mqtt reconnect is too long");
				YYERROR;
			}

			conf->mqtt->reconnect_tmo = $2;
		}
		;

af		: IPV4				{ $$ = PF_INET; }
		| IPV6				{ $$ = PF_INET6; }
		| INET				{ $$ = PF_INET; }
		| INET6				{ $$ = PF_INET6; }
		;

mqtt_keepalive	: OFF				{ $$ = 0; }
		| NUMBER {
			if ($1 < 1) {
				yyerror("mqtt keep alive is too low");
				YYERROR;
			}
			if ($1 > 65535) {
				yyerror("mqtt keep alive is too high");
				YYERROR;
			}

			$$ = (int)$1;
		}
		;

battery		: BATTERY {
			if (conf->battery.protocol != NULL) {
				yyerror("battery is already configured");
				YYERROR;
			}
		} '{' optnl batteryopts_l '}' {
			if (conf->battery.protocol == NULL) {
				yyerror("battery protocol is not configured");
				YYERROR;
			}
		}
		;

batteryopts_l	: batteryopts_l batteryopts optnl
		| batteryopts optnl
		;

batteryopts	: PROTOCOL STRING {
			if (conf->battery.protocol != NULL) {
				yyerror("battery protocol "
				    "is already configured");
				free($2);
				YYERROR;
			}
			conf->battery.protocol = $2;
		}
		| INTERFACE STRING {
			if (conf->battery.ifname != NULL) {
				yyerror("battery interface "
				    "is already configured");
				free($2);
				YYERROR;
			}
			conf->battery.ifname = $2;
		}
		| CHARGE LIMIT NUMBER limit_max {
			static const char *cfg = "battery charge limit";
			unsigned int w, maxw;
			if (conf->battery.max_charge_w != 0) {
				yyerror("%s is already configured", cfg);
				YYERROR;
			}
			if ($3 > UINT_MAX) {
				yyerror("% is way too high", cfg);
				YYERROR;
			}
			if ($4 > UINT_MAX) {
				yyerror("% max is way too high", cfg);
				YYERROR;
			}
			w = $3;
			maxw = $4;
			if (maxw == 0)
				maxw = w;
			else {
				if (maxw > 10000) { /* XXX */
					yyerror("% max is too high", cfg);
					YYERROR;
				}
				if (w > maxw) {
					yyerror("%s above max", cfg);
					YYERROR;
				}
			}

			if (w < 0) {
				yyerror("%s is too low", cfg);
				YYERROR;
			}

			conf->battery.max_charge_w = maxw;
			conf->battery.charge_w = w;
		}
		| DISCHARGE LIMIT NUMBER limit_max {
			static const char *cfg = "battery discharge limit";
			unsigned int w, maxw;
			if (conf->battery.max_discharge_w != 0) {
				yyerror("%s is already configured", cfg);
				YYERROR;
			}
			if ($3 > UINT_MAX) {
				yyerror("% is way too high", cfg);
				YYERROR;
			}
			if ($4 > UINT_MAX) {
				yyerror("% max is way too high", cfg);
				YYERROR;
			}
			w = $3;
			maxw = $4;
			if (maxw == 0)
				maxw = w;
			else {
				if (maxw > 10000) { /* XXX */
					yyerror("% max is too high", cfg);
					YYERROR;
				}
				if (w > maxw) {
					yyerror("%s above max", cfg);
					YYERROR;
				}
			}

			if (w < 0) {
				yyerror("%s is too low", cfg);
				YYERROR;
			}

			conf->battery.max_discharge_w = maxw;
			conf->battery.discharge_w = w;
		}
		;

limit_max	: /* nop */ {
			$$ = 0;
		}
		| MAX NUMBER {
			if ($2 <= 0) {
				yyerror("max limit is too low");
				YYERROR;
			}
			$$ = $2;
		}
		;

inverter	: INVERTER {
			if (conf->inverter.protocol != NULL) {
				yyerror("inverter is already configured");
				YYERROR;
			}
		} '{' optnl inverteropts_l '}' {
			if (conf->inverter.protocol == NULL) {
				yyerror("inverter protocol is not configured");
				YYERROR;
			}
		}
		;

inverteropts_l	: inverteropts_l inverteropts optnl
		| inverteropts optnl
		;

inverteropts	: PROTOCOL STRING {
			if (conf->inverter.protocol != NULL) {
				yyerror("inverter protocol "
				    "is already configured");
				free($2);
				YYERROR;
			}
			conf->inverter.protocol = $2;
		}
		| INTERFACE STRING {
			if (conf->inverter.ifname != NULL) {
				yyerror("inverter interface "
				    "is already configured");
				free($2);
				YYERROR;
			}
			conf->inverter.ifname = $2;
		}
		;
		;

%%

struct keywords {
	const char	*k_name;
	int		 k_val;
};

int
yyerror(const char *fmt, ...)
{
	va_list	ap;

	file->errors++;
	va_start(ap, fmt);
	fprintf(stderr, "%s:%d: ", file->name, yylval.lineno);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	return (0);
}

int
kw_cmp(const void *k, const void *e)
{
	return (strcmp(k, ((const struct keywords *)e)->k_name));
}

int
lookup(char *s)
{
	/* this has to be sorted always */
	static const struct keywords keywords[] = {
		{"alive",		ALIVE},
		{"battery",		BATTERY},
		{"charge",		CHARGE},
		{"client",		CLIENT},
		{"discharge",		DISCHARGE},
		{"host",		HOST},
		{"id",			ID},
		{"iface",		INTERFACE},
		{"inet",		INET},
		{"inet6",		INET6},
		{"interface",		INTERFACE},
		{"inverter",		INVERTER},
		{"ipv4",		IPV4},
		{"ipv6",		IPV6},
		{"keep",		KEEP},
		{"limit",		LIMIT},
		{"max",			MAX},
		{"mqtt",		MQTT},
		{"off",			OFF},
		{"password",		PASSWORD},
		{"port",		PORT},
		{"protocol",		PROTOCOL},
		{"reconnect",		RECONNECT},
		{"teleperiod",		TELEPERIOD},
		{"topic",		TOPIC},
		{"username",		USERNAME},
	};
	const struct keywords	*p;

	p = bsearch(s, keywords, sizeof(keywords)/sizeof(keywords[0]),
	    sizeof(keywords[0]), kw_cmp);

	if (p)
		return (p->k_val);
	else
		return (STRING);
}

#define START_EXPAND	1
#define DONE_EXPAND	2

static int	expanding;

int
igetc(void)
{
	int	c;

	while (1) {
		if (file->ungetpos > 0)
			c = file->ungetbuf[--file->ungetpos];
		else
			c = getc(file->stream);

		if (c == START_EXPAND)
			expanding = 1;
		else if (c == DONE_EXPAND)
			expanding = 0;
		else
			break;
	}
	return (c);
}

int
lgetc(int quotec)
{
	int		c, next;

	if (quotec) {
		if ((c = igetc()) == EOF) {
			yyerror("reached end of file while parsing "
			    "quoted string");
			if (file == topfile || popfile() == EOF)
				return (EOF);
			return (quotec);
		}
		return (c);
	}

	while ((c = igetc()) == '\\') {
		next = igetc();
		if (next != '\n') {
			c = next;
			break;
		}
		yylval.lineno = file->lineno;
		file->lineno++;
	}

	while (c == EOF) {
		if (file == topfile || popfile() == EOF)
			return (EOF);
		c = getc(file->stream);
	}
	return (c);
}

void
lungetc(int c)
{
	if (c == EOF)
		return;

	if (file->ungetpos >= file->ungetsize) {
		void *p = reallocarray(file->ungetbuf, file->ungetsize, 2);
		if (p == NULL)
			err(1, "%s", __func__);
		file->ungetbuf = p;
		file->ungetsize *= 2;
	}
	file->ungetbuf[file->ungetpos++] = c;
}

int
findeol(void)
{
	int	c;

	/* skip to either EOF or the first real EOL */
	while (1) {
		c = lgetc(0);
		if (c == '\n') {
			file->lineno++;
			break;
		}
		if (c == EOF)
			break;
	}
	return (ERROR);
}

int
yylex(void)
{
	char	 buf[8096];
	char	*p, *val;
	int	 quotec, next, c;
	int	 token;

top:
	p = buf;
	while ((c = lgetc(0)) == ' ' || c == '\t')
		; /* nothing */

	yylval.lineno = file->lineno;
	if (c == '#')
		while ((c = lgetc(0)) != '\n' && c != EOF)
			; /* nothing */
	if (c == '$' && !expanding) {
		while (1) {
			if ((c = lgetc(0)) == EOF)
				return (0);

			if (p + 1 >= buf + sizeof(buf) - 1) {
				yyerror("string too long");
				return (findeol());
			}
			if (isalnum(c) || c == '_') {
				*p++ = c;
				continue;
			}
			*p = '\0';
			lungetc(c);
			break;
		}
		val = symget(buf);
		if (val == NULL) {
			yyerror("macro '%s' not defined", buf);
			return (findeol());
		}
		p = val + strlen(val) - 1;
		lungetc(DONE_EXPAND);
		while (p >= val) {
			lungetc((unsigned char)*p);
			p--;
		}
		lungetc(START_EXPAND);
		goto top;
	}

	switch (c) {
	case '\'':
	case '"':
		quotec = c;
		while (1) {
			if ((c = lgetc(quotec)) == EOF)
				return (0);
			if (c == '\n') {
				file->lineno++;
				continue;
			} else if (c == '\\') {
				if ((next = lgetc(quotec)) == EOF)
					return (0);
				if (next == quotec || next == ' ' ||
				    next == '\t')
					c = next;
				else if (next == '\n') {
					file->lineno++;
					continue;
				} else
					lungetc(next);
			} else if (c == quotec) {
				*p = '\0';
				break;
			} else if (c == '\0') {
				yyerror("syntax error");
				return (findeol());
			}
			if (p + 1 >= buf + sizeof(buf) - 1) {
				yyerror("string too long");
				return (findeol());
			}
			*p++ = c;
		}
		yylval.v.string = strdup(buf);
		if (yylval.v.string == NULL)
			err(1, "%s", __func__);
		return (STRING);
	}

#define allowed_to_end_number(x) \
	(isspace(x) || x == ')' || x ==',' || x == '/' || x == '}' || x == '=')

	if (c == '-' || isdigit(c)) {
		do {
			*p++ = c;
			if ((size_t)(p-buf) >= sizeof(buf)) {
				yyerror("string too long");
				return (findeol());
			}
		} while ((c = lgetc(0)) != EOF && isdigit(c));
		lungetc(c);
		if (p == buf + 1 && buf[0] == '-')
			goto nodigits;
		if (c == EOF || allowed_to_end_number(c)) {
			const char *errstr = NULL;

			*p = '\0';
			yylval.v.number = strtonum(buf, LLONG_MIN,
			    LLONG_MAX, &errstr);
			if (errstr) {
				yyerror("\"%s\" invalid number: %s",
				    buf, errstr);
				return (findeol());
			}
			return (NUMBER);
		} else {
nodigits:
			while (p > buf + 1)
				lungetc((unsigned char)*--p);
			c = (unsigned char)*--p;
			if (c == '-')
				return (c);
		}
	}

#define allowed_in_string(x) \
	(isalnum(x) || (ispunct(x) && x != '(' && x != ')' && \
	x != '{' && x != '}' && \
	x != '!' && x != '=' && x != '#' && \
	x != ','))

	if (isalnum(c) || c == ':' || c == '_') {
		do {
			*p++ = c;
			if ((size_t)(p-buf) >= sizeof(buf)) {
				yyerror("string too long");
				return (findeol());
			}
		} while ((c = lgetc(0)) != EOF && (allowed_in_string(c)));
		lungetc(c);
		*p = '\0';
		if ((token = lookup(buf)) == STRING)
			if ((yylval.v.string = strdup(buf)) == NULL)
				err(1, "%s", __func__);
		return (token);
	}
	if (c == '\n') {
		yylval.lineno = file->lineno;
		file->lineno++;
	}
	if (c == EOF)
		return (0);
	return (c);
}

struct file *
pushfile(const char *name, int secret)
{
	struct file	*nfile;

	if ((nfile = calloc(1, sizeof(struct file))) == NULL) {
		warn("%s", __func__);
		return (NULL);
	}
	if ((nfile->name = strdup(name)) == NULL) {
		warn("%s", __func__);
		free(nfile);
		return (NULL);
	}
	if ((nfile->stream = fopen(nfile->name, "r")) == NULL) {
		warn("%s: %s", __func__, nfile->name);
		free(nfile->name);
		free(nfile);
		return (NULL);
	}
	nfile->lineno = TAILQ_EMPTY(&files) ? 1 : 0;
	nfile->ungetsize = 16;
	nfile->ungetbuf = malloc(nfile->ungetsize);
	if (nfile->ungetbuf == NULL) {
		warn("%s", __func__);
		fclose(nfile->stream);
		free(nfile->name);
		free(nfile);
		return (NULL);
	}
	TAILQ_INSERT_TAIL(&files, nfile, entry);
	return (nfile);
}

int
popfile(void)
{
	struct file	*prev;

	if ((prev = TAILQ_PREV(file, files, entry)) != NULL)
		prev->errors += file->errors;

	TAILQ_REMOVE(&files, file, entry);
	fclose(file->stream);
	free(file->name);
	free(file->ungetbuf);
	free(file);
	file = prev;
	return (file ? 0 : EOF);
}

struct batgw_config *
parse_config(const char *filename)
{
	struct sym	*sym, *next;

	file = pushfile(filename, 1);
	if (file == NULL)
		return (NULL);
	topfile = file;

	conf = calloc(1, sizeof(struct batgw_config));
	if (conf == NULL)
		return (NULL);

	yyparse();

	if (conf->battery.protocol == NULL)
		yyerror("battery has not been configured");
	if (conf->inverter.protocol == NULL)
		yyerror("inverter has not been configured");

	errors = file->errors;
	popfile();

	/* Free macros and check which have not been used. */
	TAILQ_FOREACH_SAFE(sym, &symhead, entry, next) {
		if (!sym->persist) {
			free(sym->nam);
			free(sym->val);
			TAILQ_REMOVE(&symhead, sym, entry);
			free(sym);
		}
	}

	if (errors) {
		clear_config(conf);
		return (NULL);
	}

	return (conf);
}

int
cmdline_symset(const char *s)
{
	char	*sym, *val;
	int	ret;

	if ((val = strrchr(s, '=')) == NULL)
		return (-1);
	sym = strndup(s, val - s);
	if (sym == NULL)
		errx(1, "%s: strndup", __func__);
	ret = symset(sym, val + 1, 1);
	free(sym);

	return (ret);
}

char *
symget(const char *nam)
{
	struct sym	*sym;

	TAILQ_FOREACH(sym, &symhead, entry) {
		if (strcmp(nam, sym->nam) == 0) {
			sym->used = 1;
			return (sym->val);
		}
	}
	return (NULL);
}

int
symset(const char *nam, const char *val, int persist)
{
	struct sym	*sym;

	TAILQ_FOREACH(sym, &symhead, entry) {
		if (strcmp(nam, sym->nam) == 0)
			break;
	}

	if (sym != NULL) {
		if (sym->persist == 1)
			return (0);
		else {
			free(sym->nam);
			free(sym->val);
			TAILQ_REMOVE(&symhead, sym, entry);
			free(sym);
		}
	}
	if ((sym = calloc(1, sizeof(*sym))) == NULL)
		return (-1);

	sym->nam = strdup(nam);
	if (sym->nam == NULL) {
		free(sym);
		return (-1);
	}
	sym->val = strdup(val);
	if (sym->val == NULL) {
		free(sym->nam);
		free(sym);
		return (-1);
	}
	sym->used = 0;
	sym->persist = persist;
	TAILQ_INSERT_TAIL(&symhead, sym, entry);
	return (0);
}

void
clear_config(struct batgw_config *c)
{
	struct batgw_config_mqtt *mqtt;

	mqtt = c->mqtt;
	if (mqtt != NULL) {
		free(mqtt->host);
		free(mqtt->port);
		free(mqtt->user);
		free(mqtt->pass);
		free(mqtt->topic);
		free(mqtt->clientid);
		free(mqtt);
	}

	free(c);
}

static const char *
check_mqtt_topic(const char *topic)
{
	size_t len, i;

	len = strlen(topic);
	if (len == 0)
		return ("too short");
	if (len > 64)
		return ("too long");

	for (i = 0; i < len; i++) {
		int ch = topic[i];
		if (isalnum(ch))
			continue;
		if (ch == '_' || ch == '-' || ch == '/')
			continue;

		return ("invalid character");
	}

	return (NULL);
}
