/*
 * alternative version of k42 acquire_lock, as per
 * https://www.cs.rochester.edu/research/synchronization/pseudocode/ss.html
 */

#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_next = NULL;
	mtx->mtx_tail = NULL;
}

static inline struct mutex *
mtx_cas(struct mutex **mtxp, struct mutex *e, struct mutex *p)
{
	return atomic_cas_ptr(mtxp, e, p);
}

static inline struct mutex *
mtx_swap(struct mutex **mtxp, struct mutex *n)
{
	return atomic_swap_ptr(mtxp, n);
}

int
mtx_enter_try(struct mutex *mtx)
{
	struct mutex *tail;

	tail = mtx_cas(&mtx->mtx_tail, NULL, mtx);
	if (tail == NULL) {
		membar_enter_after_atomic();
		return (1);
	}

	return (0);
}

void
mtx_enter(struct mutex *mtx)
{
	struct mutex self = { .mtx_next = NULL };
	struct mutex *v;

	v = mtx_swap(&mtx->mtx_tail, &self);
	if (v != NULL) {
		/* queue was non-empty */
		self.mtx_tail = &self; /* set locked */
		WRITE_ONCE(v->mtx_next, &self);

		/* wait for the lock */
		while (READ_ONCE(self.mtx_tail) != NULL)
			CPU_BUSY_CYCLE();
	}

	v = READ_ONCE(self.mtx_next); /* read successor */
	if (v == NULL) {
		if (mtx_cas(&mtx->mtx_tail, &self, mtx) != &self) {
			/* somebody got into the timing window */
			while ((v = READ_ONCE(self.mtx_next)) == NULL)
				CPU_BUSY_CYCLE();
		}
	}
	WRITE_ONCE(mtx->mtx_next, v);
}

void
mtx_leave(struct mutex *mtx)
{
	struct mutex *v;

	membar_exit();

	v = READ_ONCE(mtx->mtx_next);
	if (v == NULL) {
		/* no known successor */
		if (mtx_cas(&mtx->mtx_tail, mtx, NULL) == mtx)
			return;

		while ((v = READ_ONCE(mtx->mtx_next)) == NULL)
			CPU_BUSY_CYCLE();
	}

	v->mtx_tail = NULL;
}
