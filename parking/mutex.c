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

int
mtx_enter_try(struct mutex *mtx)
{
	unsigned long self = (unsigned long)pthread_self();

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
	struct waiter w, *n;
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;
	unsigned int i;
	unsigned long m;

	owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
	if (owner == 0) {
		/* we got the lock first go. this is the fast path */
		goto locked;
	}

	if (__predict_false(owner == (self | 1))) {
		/*
		 * panic("%s(%p): locking against myself", __func__, mtx);
		 */
		abort();
	}

	/* take the really slow path */
	p = mtx_park(mtx);

	/* spinning++ */
	w.mtx = mtx;
	m = mtx_enter_park(p);
	TAILQ_INSERT_TAIL(&p->waiters, &w, entry);
	mtx_leave_park(p, m);

	for (;;) {
		unsigned long o;

		o = atomic_cas_ulong(&mtx->mtx_owner, owner, owner | 1);
		if (o == owner || ISSET(o, 1)) {
			while (w.mtx != NULL)
				CPU_BUSY_CYCLE();
		}

		owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
		if (owner == 0)
			break;

		w.mtx = mtx;
	}

	m = mtx_enter_park(p);
	TAILQ_REMOVE(&p->waiters, &w, entry);
	TAILQ_FOREACH(n, &p->waiters, entry) {
		if (n->mtx == mtx) {
			mtx->mtx_owner = self | 1;
			break;
		}
	}
	mtx_leave_park(p, m);
	/* spinning-- */

locked:
	membar_enter_after_atomic();
}

void
mtx_leave(struct mutex *mtx)
{
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;

	membar_exit_before_atomic();
	owner = atomic_swap_ulong(&mtx->mtx_owner, 0);
	if (owner != self) {
		struct mtx_park *p;
		unsigned long m;
		struct waiter *w;

		if (__predict_false(owner != (self | 1))) {
			/*
			 * panic("%s(%p): not owner", __func__, mtx);
			 */
			abort();
		}

		p = mtx_park(mtx);
		m = mtx_enter_park(p);
		TAILQ_FOREACH(w, &p->waiters, entry) {
			if (w->mtx == mtx) {
				w->mtx = NULL;
				break;
			}
		}
		mtx_leave_park(p, m);
	}
}
