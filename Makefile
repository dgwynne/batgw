PROG=		batgw
SRCS=		batgw.c
SRCS+=		log.c amqtt.c
MAN=

SRCS+=		parse.y
CLEANFILES+=	parse.c y.tab.h
CFLAGS+=	-I${.CURDIR}

SRCS+=	battery/b_byd.c

SRCS+=	inverter/i_byd_can.c

LDADD=-levent -lbsd
DEBUG=-g

.include <bsd.prog.mk>
