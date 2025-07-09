.include <bsd.own.mk>

SUBDIR=	spinlock spinlockrd backoff \
	ticket k42 k42alt wtflock spinlist spinlistfair parking parkingfair

.PHONY: bench

bench: _SUBDIRUSE

.include <bsd.subdir.mk>

