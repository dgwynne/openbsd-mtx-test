.PATH:		${.CURDIR}/..

SRCS=		mutex.c main.c
PROG=		test
MAN=		

CFLAGS+=	-I${.CURDIR}

LDADD=		-lpthread
DPADD=		${LIBPTHREAD}

.include <bsd.prog.mk>
