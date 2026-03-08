DEBUG=-g

TESTNAME=${.CURDIR:T}

CFLAGS+=-DTESTNAME=${TESTNAME}

.PHONY: bench hyperfine_one

.include "Makefile.vars"

bench: all
	@seq ${NCPUS} | while read n; do ${.OBJDIR}/test ${PADDED} -n $$n -l ${LOOPS}; done

hyperfine_one: all
	@hyperfine -N -n "${TESTNAME} -n {threads} -l ${LOOPS} -w ${WORK}" \
	    --export-json /tmp/foo \
	    --parameter-scan threads 1 ${NCPUS} \
	    "${.OBJDIR}/test -n {threads} -l ${LOOPS} -w ${WORK}"
