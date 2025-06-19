#include <sys/resource.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

#include <pthread.h>

#include <mutex.h>

#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))

int ncpus;
#define LOOPS 1000000LLU
//000000

struct state {
	volatile int		bar;
	struct mutex		mtx;
	uint64_t		loops;
	volatile uint64_t	v;
};

__dead static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-n nthreads]\n", __progname);

	exit(0);
}

void *
worker(void *arg)
{
	struct state *s = arg;
	uint64_t i;
	uint64_t loops = s->loops;

	while (s->bar)
		pthread_yield();

	for (i = 0; i < loops; i++) {
		mtx_enter(&s->mtx);
//if ((s->v % 100) == 0)
//	warnx("%d %llu", getthrid(), s->v);
		s->v++;
		mtx_leave(&s->mtx);
	}

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

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus == -1)
		err(1, "sysconf(_SC_NPROCESSORS_ONLN)");

	nthreads = ncpus;

	while ((ch = getopt(argc, argv, "l:n:")) != -1) {
		switch (ch) {
		case 'n':
			nthreads = strtonum(optarg, 1, 128, &errstr);
			if (errstr != NULL)
				errx(1, "nthreads: %s", errstr);
			break;
		case 'l':
			loops = strtonum(optarg, 1, LOOPS, &errstr);
			if (errstr != NULL)
				errx(1, "loops: %s", errstr);
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	s.bar = 1;
	mtx_init(&s.mtx);
	s.loops = loops;
	s.v = 0;

	printf("starting %d threads for %llu loops\n", nthreads, loops);

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

	timespecsub(&tock, &tick, &diff);

	printf("real time: ");
	time2ival(diff.tv_sec);
	printf(".%02lds, ", diff.tv_nsec / 10000000);

	printf("user time: ");
	time2ival(ru.ru_utime.tv_sec);
	printf(".%02lds\n", ru.ru_utime.tv_usec / 10000);

	if (s.v != nthreads * loops)
		errx(1, "unexpected value after workers finished");

	return (0);
}

struct interval {
	const char	p;
	time_t		s;
};

static const struct interval intervals[] = {
	{ 'w',  60 * 60 * 24 * 7 },
	{ 'd',  60 * 60 * 24 },
	{ 'h',  60 * 60 },
	{ 'm',  60 },
	/* { 's',  1 }, */
};

static void
time2ival(time_t sec)
{
	size_t i;

	for (i = 0; i < nitems(intervals); i++) {
		const struct interval *ival = &intervals[i];

		if (sec >= ival->s) {
			time_t d = sec / ival->s;
                        printf("%lld%c", d, ival->p);
			sec -= d * ival->s;
		}
	}

	printf("%lld", sec);
}
