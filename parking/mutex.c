#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

#include <machine/spinlock.h>
#include <sys/queue.h>
#include <stdlib.h>

void    _spinlock(volatile _atomic_lock_t *);
void    _spinunlock(volatile _atomic_lock_t *);

struct waiter {
	struct mutex		*mtx;
	TAILQ_ENTRY(waiter)	 entry;
};

TAILQ_HEAD(waiters, waiter);

struct park {
	_atomic_lock_t		lock;
	struct waiters		waiters;
};

#define PARK_INITIALIZER(_park) {				\
	.lock = _ATOMIC_LOCK_UNLOCKED,				\
	.waiters = TAILQ_HEAD_INITIALIZER(_park.waiters),	\
}

struct park parking[1] = {
	PARK_INITIALIZER(parking[0]),
};

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_owner = 0;
}

unsigned long
mtx_enter_self(struct mutex *mtx, unsigned long self)
{
	unsigned long owner;

	owner = mtx->mtx_owner;
	if (owner == 0) {
		owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
		if (owner == 0) {
			membar_enter_after_atomic();
			return (0);
		}
	}

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
	struct park *p;
	struct waiter w;
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;
	unsigned int i;

	for (i = 0; i < 40; i++) {
		owner = mtx_enter_self(mtx, self);
		if (owner == 0)
			return;

		CPU_BUSY_CYCLE();
	}

	p = &parking[0]; /* XXX pick a park */

	do {
		unsigned long busy = 0;

		atomic_cas_ulong(&mtx->mtx_owner, owner, owner | 1);

		/* crit_enter */
		_spinlock(&p->lock);
		owner = mtx->mtx_owner;
		if (owner & 1) {
			w.mtx = mtx;
			TAILQ_INSERT_TAIL(&p->waiters, &w, entry);
		}
		_spinunlock(&p->lock);
		/* crit_leave */

		if (owner & 1) {
			while (w.mtx != NULL)
				CPU_BUSY_CYCLE();

			/* crit_enter */
			_spinlock(&p->lock);
			TAILQ_REMOVE(&p->waiters, &w, entry);
			if (!TAILQ_EMPTY(&p->waiters))
				busy = 1;
			_spinunlock(&p->lock);
			/* crit_leave */
		}

		owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self | 1);
	} while (owner != 0);

	membar_enter_after_atomic();
}

void
mtx_leave(struct mutex *mtx)
{
	struct park *p;
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;
	struct waiter *w;

	membar_exit_before_atomic();
	owner = atomic_swap_ulong(&mtx->mtx_owner, 0);
	if (owner == self)
		return;
	if (owner != (self|1)) {
		/*
		 * panic("%s(%p): owner 0x%lx != self 0x%lx | 1",
		 *     __func__, mtx, owner, self);
		 */
		abort();
	}

	p = &parking[0]; /* XXX pick a park */

	/* crit_enter */
	_spinlock(&p->lock);
	TAILQ_FOREACH(w, &p->waiters, entry) {
		if (w->mtx == mtx) {
			w->mtx = NULL;
			break;
		}
	}
	_spinunlock(&p->lock);
	/* crit_leave */
}
