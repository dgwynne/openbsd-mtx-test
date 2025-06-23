/*
 * this is the K42 MCS variant according to
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
	struct mutex self;
	struct mutex *v, *ov;

	v = READ_ONCE(mtx->mtx_tail);
	for (;;) {
		if (v == NULL) {
			/* lock appears not to be held */
			v = mtx_cas(&mtx->mtx_tail, NULL, mtx);
			if (v == NULL) {
				/* we have the lock */
				membar_enter_after_atomic();
				return;
			}
		}

		/* lock appears to be held */
		self.mtx_next = NULL;
		self.mtx_tail = &self;

		ov = mtx_cas(&mtx->mtx_tail, v, &self);
		if (ov != v) {
			v = ov;
			continue;
		}

		/* we are in line */
		WRITE_ONCE(v->mtx_next, &self);
		/* wait for the lock */
		while (READ_ONCE(self.mtx_tail))
			CPU_BUSY_CYCLE();

		/* we now have the lock */
		v = READ_ONCE(self.mtx_next);
		if (v == NULL) {
			WRITE_ONCE(mtx->mtx_next, NULL);
			if (mtx_cas(&mtx->mtx_tail, &self, mtx) != &self) {
				/* somebody got into the timing window */
				while ((v = READ_ONCE(self.mtx_next)) == NULL)
					CPU_BUSY_CYCLE();
				WRITE_ONCE(mtx->mtx_next, v);
			}
		} else
			WRITE_ONCE(mtx->mtx_next, v);
		return;
	}
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
