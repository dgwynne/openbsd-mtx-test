/*
 * this implements a cas based spinlock, but when contended it reads
 * mtx_owner until it clears.
 *
 * the idea is to let the lock cacheline move to a shared state,
 * but this doesn't work that well if the lock is on the same cacheline
 * as whatever is being modified inside the critical section, or if
 * the critical sections are very short.
 */

#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_owner = NULL;
}

int
mtx_enter_try(struct mutex *mtx)
{
	pthread_t self = pthread_self();
	pthread_t owner;

	owner = atomic_cas_ptr(&mtx->mtx_owner, NULL, self);
	if (owner == NULL) {
		membar_enter_after_atomic();
		return (1);
	}

	return (0);
}

void
mtx_enter(struct mutex *mtx)
{
	while (mtx_enter_try(mtx) == 0) {
		do {
			CPU_BUSY_CYCLE();
		} while (mtx->mtx_owner != NULL);
	}
}

void
mtx_leave(struct mutex *mtx)
{
	membar_exit();
	mtx->mtx_owner = NULL;
}
