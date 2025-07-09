DEBUG=-g

CFLAGS+=-DTESTNAME=${.CURDIR:T}

LOOPS?=1000000
NCPUS!!=getconf NPROCESSORS_ONLN

.PHONY: bench

bench: all
	@seq ${NCPUS} | while read n; do ${.OBJDIR}/test -n $$n -l ${LOOPS}; done
