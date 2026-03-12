#include <sys/resource.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

#include <pthread.h>

#include <mutex.h>

#include "atomic.h"

#define XSTR(S) #S
#define STR(S) XSTR(S)

#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))

int ncpus;
#define LOOPS 1000000LLU
int x = 8;
//000000

struct work;

struct state {
	volatile int		bar;
	struct mutex		mtx;
	struct mutex		mtx1;
	uint64_t		loops;
	uint64_t		nthreads;
	const struct work	*w;
	volatile uint64_t	v;
	u_char			_pad[128];
	volatile uint64_t	pv;
};

const char *testname;

__dead static void
usage(void)
{
	fprintf(stderr, "usage: %s [-n nthreads]\n", testname);

	exit(0);
}

static void
work_inc(struct state *s)
{
	uint64_t i;
	uint64_t loops = s->loops;

	for (i = 0; i < loops; i++) {
		mtx_enter(&s->mtx);
		s->v++;
		mtx_leave(&s->mtx);
	}
}

static void
check_inc(struct state *s)
{
	if (s->v != s->loops * s->nthreads)
		errx(1, "unexpected value %llu after workers finished", s->v);
}

static void
work_inc_padded(struct state *s)
{
	uint64_t i;
	uint64_t loops = s->loops;

	for (i = 0; i < loops; i++) {
		mtx_enter(&s->mtx);
		s->pv++;
		mtx_leave(&s->mtx);
	}
}

static void
check_inc_padded(struct state *s)
{
	if (s->pv != s->loops * s->nthreads)
		errx(1, "unexpected value %llu after workers finished", s->pv);
}

static void
work_inc_nops(struct state *s)
{
	uint64_t i, c;
	uint64_t loops = s->loops;

	for (i = 0; i < loops; i++) {
		mtx_enter(&s->mtx);
		s->v++;
		for (c = 0; c < 64; c++)
			CPU_BUSY_CYCLE();
		mtx_leave(&s->mtx);
	}
}

/*
 * access to a "resource" is protected by a mutex.
 *
 * threads coordinate via a mutex to take ownership of a "resource",
 * which they then operate on outside the mutex, before taking the
 * mutex again to return it.
 */

static void
work_inc_res(struct state *s)
{
	uint64_t i;
	uint64_t loops = s->loops;
	uint64_t v;

	for (i = 0; i < loops; i++) {
		do {
			mtx_enter(&s->mtx);
			v = s->v;
			if (v == 0)
				s->v = 1;
			mtx_leave(&s->mtx);
		} while (v != 0);

		s->pv++;

		mtx_enter(&s->mtx);
		s->v = 0;
		mtx_leave(&s->mtx);
	}
}

static void
work_arc4random(struct state *s)
{
	uint64_t i;
	uint64_t loops = s->loops;
	uint64_t v;

	for (i = 0; i < loops; i++) {
		mtx_enter(&s->mtx);
		arc4random();
		mtx_leave(&s->mtx);
	}
}

static void
check_arc4random(struct state *s)
{
	/* nop */
}

struct work {
	const char *name;
	void (*func)(struct state *);
	void (*check)(struct state *);
};

static const struct work workers[] = {
	{ "inc",	work_inc,		 check_inc },
	{ "inc-padded",	work_inc_padded,	 check_inc_padded },
	{ "inc-nops",	work_inc_nops,		 check_inc },
	{ "res",	work_inc_res,		 check_inc_padded },
	{ "arc4random",	work_arc4random,	 check_arc4random },
};

void *
worker(void *arg)
{
	struct state *s = arg;

	while (s->bar)
		pthread_yield();

	s->w->func(s);

	return (NULL);
}

static void time2ival(time_t);

int
main(int argc, char *argv[])
{
	struct state s;

	pthread_t *pths;
	int nthreads;
	uint64_t loops = LOOPS;
	int i;
	struct timespec tick, tock, diff;
	struct rusage ru;

	int ch;
	const char *errstr;
	int error;
	const char *workname = "inc";
	const struct work *w = NULL;
	uint64_t v;

#ifdef TESTNAME
	setprogname(testname = STR(TESTNAME));
#else
	testname = getprogname();
#endif

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus == -1)
		err(1, "sysconf(_SC_NPROCESSORS_ONLN)");

	nthreads = ncpus;

	while ((ch = getopt(argc, argv, "l:n:w:x:")) != -1) {
		switch (ch) {
		case 'n':
			nthreads = strtonum(optarg, 1, ncpus, &errstr);
			if (errstr != NULL)
				errx(1, "nthreads: %s", errstr);
			break;
		case 'l':
			loops = strtonum(optarg, 1, UINT64_MAX / ncpus,
			    &errstr);
			if (errstr != NULL)
				errx(1, "loops: %s", errstr);
			break;
		case 'w':
			workname = optarg;
			break;
		case 'x':
			x = strtonum(optarg, 0, 128, &errstr);
			if (errstr != NULL)
				errx(1, "x: %s", errstr);
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	s.bar = 1;
	mtx_init(&s.mtx);
	s.loops = loops;
	s.nthreads = nthreads;
	s.v = s.pv = 0;

	for (i = 0; i < nitems(workers); i++) {
		const struct work *nw = &workers[i];
		if (strcmp(nw->name, workname) == 0) {
			w = nw;
			break;
		}
	}

	if (w == NULL)
		errx(1, "%s work not found", workname);

	s.w = w;

	warnx("starting %d threads for %llu loops", nthreads, loops);

	pths = calloc(nthreads, sizeof(*pths));
	if (pths == NULL)
		err(1, "pth calloc");

	for (i = 0; i < nthreads; i++) {
		error = pthread_create(&pths[i], NULL, worker, &s);
		if (error != 0)
			errc(1, error, "pthread_create %d", i);
	}

	if (clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
		err(1, "tick");

	s.bar = 0;

	for (i = 0; i < nthreads; i++) {
		void *v;
		error = pthread_join(pths[i], &v);
		if (error != 0)
			errc(1, error, "pthread_join %d", i);
		if (v != NULL)
			errx(1, "pthread_join %i unexpected value %p", i, v);
	}
	if (clock_gettime(CLOCK_MONOTONIC, &tock) == -1)
		err(1, "tock");

	if (getrusage(RUSAGE_SELF, &ru) == -1)
		err(1, "getrusage self");

	w->check(&s);

	timespecsub(&tock, &tick, &diff);

	printf("{");
	printf("\"lock\":\"%s\",", testname);
	printf("\"work\":\"%s\",", workname);
	printf("\"loops\":%llu,", loops);
	printf("\"nthreads\":%d,", nthreads);
	printf("\"time\":%lld.%03ld", diff.tv_sec, diff.tv_nsec / 1000000);
	printf("}\n");

	return (0);
}
