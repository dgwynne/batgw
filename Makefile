PROG=	batgw
SRCS=	batgw.c
SRCS+=	log.c amqtt.c

SRCS+=	battery/b_byd.c
MAN=

LDADD=-levent -lbsd

DEBUG=-g

.include <bsd.prog.mk>
