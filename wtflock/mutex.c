/*
 * this is heavily inspired by the WTF::Lock from Locking in WebKit[1]
 * and some of the references it provides.
 *
 * 1. https://webkit.org/blog/6161/locking-in-webkit/
 */

#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

#include <machine/spinlock.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define ISSET(_w, _m) ((_w) & (_m))

#define intr_disable() (0)
#define intr_restore(m) do { } while (0)

struct cpu_info;

#define curcpu() ((struct cpu_info *)1)

#define MTX_ISLOCKED		1
#define MTX_HASPARKED		2

struct waiter {
	struct mutex		*volatile mtx;
	TAILQ_ENTRY(waiter)	 entry;
} __aligned(64);

TAILQ_HEAD(waiters, waiter);

struct mtx_park {
	struct cpu_info		*lock;
	struct waiters		 waiters;
} __aligned(64);

#define PARK_INITIALIZER(_park) {				\
	.lock = NULL,						\
	.waiters = TAILQ_HEAD_INITIALIZER(_park.waiters),	\
}

static struct mtx_park mtx_parking[1] = {
	PARK_INITIALIZER(mtx_parking[0]),
};

static struct mtx_park *
mtx_park(struct mutex *mtx)
{
	return mtx_parking;
}

static unsigned long
mtx_enter_park(struct mtx_park *p)
{
	struct cpu_info * const ci = curcpu();
	unsigned long m;

	m = intr_disable();
	while (atomic_cas_ptr(&p->lock, NULL, ci) != 0)
		CPU_BUSY_CYCLE();
	membar_enter_after_atomic();

	return (m);
}

static void
mtx_leave_park(struct mtx_park *p, unsigned long m)
{
	membar_exit();
	p->lock = NULL;
	intr_restore(m);
}

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_owner = 0;
}

static inline unsigned long
mtx_self(void)
{
	/* summarise ownership with bit MTX_ISLOCKED */
	return ((unsigned long)pthread_self() | MTX_ISLOCKED);
}

int
mtx_enter_try(struct mutex *mtx)
{
	unsigned long self = mtx_self();

	if (atomic_cas_ulong(&mtx->mtx_owner, 0, self) == 0) {
		membar_enter_after_atomic();
		return (1);
	}

	return (0);
}

void
mtx_enter(struct mutex *mtx)
{
	struct mtx_park *p;
	struct waiter w;
	unsigned long self = mtx_self();
	unsigned long owner;
	unsigned int i;
	unsigned long m;
	int cond;

	/* Extra bit from the Spinning section after Barging */

	/* Fast path: */
	if (atomic_cas_ulong(&mtx->mtx_owner, 0, self) == 0)
		goto locked;

	for (i = 40; i--;) {
		/* Do not spin if there is a queue. */
		owner = mtx->mtx_owner;
		if (owner & MTX_HASPARKED)
			break;
		/* Try to get the lock. */
		if (atomic_cas_ulong(&mtx->mtx_owner, 0, self) == 0)
			goto locked;
		CPU_BUSY_CYCLE();
	}

	p = mtx_park(mtx);
	for (;;) {
		owner = mtx->mtx_owner; /* load current state */

                /*
		 * Fast path, which enables barging since we are
		 * happy to grab the lock even if there are threads
		 * parked.
		 */
		if (!ISSET(owner, MTX_ISLOCKED)) {
			if (atomic_cas_ulong(&mtx->mtx_owner,
			    owner, owner | self) == owner)
				break;
		}

		/*
		 * Before we park we should make sure that the
		 * hasParkedBit is set. Note that because compareAndPark
		 * will anyway check if the state is still
		 * isLockedBit | hasParkedBit, we don't have to
		 * worry too much about this CAS possibly failing
		 * spuriously.
		 */
		atomic_cas_ulong(&mtx->mtx_owner,
		    owner, owner | MTX_HASPARKED);

		/*
		 * Try to park so long as the lock's state is that both
		 * isLockedBit and hasParkedBit are set.
		 */

		w.mtx = mtx;

		m = mtx_enter_park(p);
		owner = mtx->mtx_owner;
		cond = (owner & (MTX_ISLOCKED|MTX_HASPARKED)) ==
		    (MTX_ISLOCKED|MTX_HASPARKED);
		if (cond)
			TAILQ_INSERT_TAIL(&p->waiters, &w, entry);
		mtx_leave_park(p, m);

		if (cond) {
			while (w.mtx != NULL)
				CPU_BUSY_CYCLE();
		}
	}
locked:
	membar_enter_after_atomic();
}

void
mtx_leave(struct mutex *mtx)
{
	unsigned long self = mtx_self();
	unsigned long owner;

	membar_exit_before_atomic();
	/* We can unlock the fast way if the hasParkedBit was not set. */
	owner = atomic_cas_ulong(&mtx->mtx_owner, self, 0);
	if (owner != self) {
		struct mtx_park *p;
		unsigned long m;
		struct waiter *w;

		if (__predict_false(owner != (self | MTX_HASPARKED))) {
			/*
			 * panic("%s(%p): not owner", __func__, mtx);
			 */
			abort();
		}

		/* Fast unlocking failed, so unpark a thread. */
		p = mtx_park(mtx);
		m = mtx_enter_park(p);
		TAILQ_FOREACH(w, &p->waiters, entry) {
			if (w->mtx == mtx) {
				TAILQ_REMOVE(&p->waiters, w, entry);
				w->mtx = NULL;
				break;
			}
		}
		mtx->mtx_owner = TAILQ_EMPTY(&p->waiters) ? 0 : MTX_HASPARKED;
		mtx_leave_park(p, m);
	}
}
