PROG=		batgw
SRCS=		batgw.c
SRCS+=		log.c amqtt.c
MAN=

SRCS+=		parse.y
CFLAGS+=	-I${.CURDIR}

SRCS+=	battery/b_byd.c

LDADD=-levent -lbsd
DEBUG=-g

.include <bsd.prog.mk>
