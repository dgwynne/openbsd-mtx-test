.include <bsd.own.mk>

LOCKS?=	spinlock spinlockrd backoff \
	ticket k42 k42alt wtflock spinlist spinlistfair parking parkingfair

SUBDIR=${LOCKS}

.PHONY: bench

bench: _SUBDIRUSE

.include <bsd.subdir.mk>

