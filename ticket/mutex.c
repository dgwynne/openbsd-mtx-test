/*
 * a basic ticket lock.
 *
 * this is not a good candidate for the kernel lock because
 * mtx_enter_try is too hard (or impossible?) with separate words for
 * the tick and next counters.
 */

#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

void
mtx_init(struct mutex *mtx)
{
	mtx->tick = 1;
	mtx->next = 0;
}

int
mtx_enter_try(struct mutex *mtx)
{
	return (0);
}

void
mtx_enter(struct mutex *mtx)
{
	unsigned int next = atomic_inc_int_nv(&mtx->next);
	while (mtx->tick != next)
		CPU_BUSY_CYCLE();
	membar_enter();
}

void
mtx_leave(struct mutex *mtx)
{
	membar_exit();
	mtx->tick++;
}
