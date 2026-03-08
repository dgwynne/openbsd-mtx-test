.include <bsd.own.mk>

LOCKS?=spinlock,spinlockrd,backoff,ticket,k42,wtflock,parking,parking-nomedium

# k42alt seems to deadlock
# spinlist and spinlistfair are made up things
# parkingfair is a toy

SUBDIR=${LOCKS:S/,/ /g}

.PHONY: bench hyperfine hyperfine_one

bench: _SUBDIRUSE

hyperfine_one: _SUBDIRUSE

.include "Makefile.vars"

JSON?=/dev/null

hyperfine:
	@hyperfine -N --export-json ${JSON} \
	    --parameter-list LOCK ${LOCKS} \
	    --parameter-list WORK ${WORK} \
	    -n "{LOCK} -n ${NCPUS} -l ${LOOPS} -w {WORK}" \
	    "${.CURDIR}/{LOCK}/obj/test -n ${NCPUS} -l ${LOOPS} -w {WORK}"

.include <bsd.subdir.mk>
