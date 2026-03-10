.PATH:		${.CURDIR}/..

SRCS=		mutex.c main.c
PROG=		test
MAN=		

CFLAGS+=	-I${.CURDIR} -DPARKING_PAD

LDADD=		-lpthread
DPADD=		${LIBPTHREAD}

.include <bsd.prog.mk>
