.include <bsd.own.mk>

SUBDIR=	spinlock spinlockrd backoff \
	ticket k42 k42alt wtflock spinlist spinlistfair parking parkingfair

.include <bsd.subdir.mk>

