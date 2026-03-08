.PATH:		${.CURDIR}/..

SRCS=		mutex.c main.c
PROG=		test
MAN=		

CFLAGS+=	-I${.CURDIR} -DNOMEDIUM

LDADD=		-lpthread
DPADD=		${LIBPTHREAD}

.include <bsd.prog.mk>
