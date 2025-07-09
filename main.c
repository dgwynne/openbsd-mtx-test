#include <sys/resource.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

#include <pthread.h>

#include <mutex.h>

#define XSTR(S) #S
#define STR(S) XSTR(S)

#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))

int ncpus;
#define LOOPS 1000000LLU
int x = 8;
//000000

struct state {
	volatile int		bar;
	struct mutex		mtx;
	uint64_t		loops;
	volatile uint64_t	v;
};

const char *testname;

__dead static void
usage(void)
{
	fprintf(stderr, "usage: %s [-n nthreads]\n", testname);

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

#ifdef TESTNAME
	setprogname(testname = STR(TESTNAME));
#else
	testname = getprogname();
#endif

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus == -1)
		err(1, "sysconf(_SC_NPROCESSORS_ONLN)");

	nthreads = ncpus;

	while ((ch = getopt(argc, argv, "l:n:x:")) != -1) {
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
	s.v = 0;

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

	if (s.v != nthreads * loops)
		errx(1, "unexpected value after workers finished");

	timespecsub(&tock, &tick, &diff);

	printf("{");
	printf("\"lock\":\"%s\",", testname);
	printf("\"loops\":%llu,", loops);
	printf("\"nthreads\":%d,", nthreads);
	printf("\"time\":%lld.%03ld", diff.tv_sec, diff.tv_nsec / 1000000);
	printf("}\n");

	return (0);
}
