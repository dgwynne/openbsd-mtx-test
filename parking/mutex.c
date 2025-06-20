#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

#include <machine/spinlock.h>
#include <sys/queue.h>
#include <stdlib.h>

#define intr_disable() (0)
#define intr_restore(m) do { } while (0)

struct waiter {
	struct mutex		*mtx;
	TAILQ_ENTRY(waiter)	 entry;
};

TAILQ_HEAD(waiters, waiter);

struct mtx_park {
	pthread_t		lock;
	struct waiters		waiters;
};

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
	//struct cpu_info *ci = curcpu();
	//struct cpu_info *owner;
	pthread_t ci = pthread_self();
	pthread_t owner;
	unsigned long m;

	m = intr_disable();
#if 1
	while ((owner = atomic_cas_ptr(&p->lock, NULL, ci)) != NULL)
		CPU_BUSY_CYCLE();
#else
	for (;;) {
		owner = p->lock;
		if (owner == NULL) {
			owner = atomic_cas_ptr(&p->lock, NULL, ci);
			if (owner == NULL)
				break;
		}
		CPU_BUSY_CYCLE();
	}
#endif
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

unsigned long
mtx_enter_self(struct mutex *mtx, unsigned long self)
{
	unsigned long owner;

#if 1
	owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
	if (owner == 0)
		membar_enter_after_atomic();
#else
	owner = mtx->mtx_owner;
	if (owner == 0) {
		owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
		if (owner == 0)
			membar_enter_after_atomic();
	}
#endif

	return (owner);
}

int
mtx_enter_try(struct mutex *mtx)
{
	return (mtx_enter_self(mtx, (unsigned long)pthread_self()) == 0);
}

void
mtx_enter(struct mutex *mtx)
{
	struct mtx_park *p;
	struct waiter w;
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;
	unsigned int i;

	owner = mtx_enter_self(mtx, self);
	if (owner == 0)
		goto locked;

	/* spinning++ */
	for (i = 0; i < 40; i++) {
		CPU_BUSY_CYCLE();

		owner = mtx->mtx_owner;
		if (owner == 0) {
			owner = mtx_enter_self(mtx, self);
			if (owner == 0)
				goto spinlocked;
		}
	}

	p = mtx_park(mtx);
	do {
		unsigned long nself = self;
		unsigned long m;

		atomic_cas_ulong(&mtx->mtx_owner, owner, owner | 1);

		m = mtx_enter_park(p);
		owner = mtx->mtx_owner;
		if (owner & 1) {
			w.mtx = mtx;
			TAILQ_INSERT_TAIL(&p->waiters, &w, entry);
		}
		mtx_leave_park(p, m);

		if (owner & 1) {
			while (w.mtx != NULL)
				CPU_BUSY_CYCLE();

			m = mtx_enter_park(p);
			TAILQ_REMOVE(&p->waiters, &w, entry);
			if (!TAILQ_EMPTY(&p->waiters))
				nself |= 1;
			mtx_leave_park(p, m);
		}

		owner = mtx_enter_self(mtx, nself);
	} while (owner != 0);

spinlocked:
	/* spinning-- */
locked:
	return;
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

		if ((owner & ~0x1UL) != self) {
			/*
			 * panic("%s(%p): not locked", __func__, mtx);
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
