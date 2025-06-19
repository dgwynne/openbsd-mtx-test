#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>

#include <pthread.h>

#include <mutex.h>

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

int
main(int argc, char *argv[])
{
	struct state s;

	pthread_t *pths;
	int nthreads;
	uint64_t loops = LOOPS;
	int i;

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

	s.bar = 0;

	for (i = 0; i < nthreads; i++) {
		void *v;
		error = pthread_join(pths[i], &v);
		if (error != 0)
			errc(1, error, "pthread_join %d", i);
		if (v != NULL)
			errx(1, "pthread_join %i unexpected value %p", i, v);
	}

	if (s.v != nthreads * loops)
		errx(1, "unexpected value after workers finished");

	return (0);
}
