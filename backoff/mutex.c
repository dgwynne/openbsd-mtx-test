/*
 * this implements a cas based spinlock using exponential backoff
 * between attempts to try take the lock.
 *
 * it's supposed to implement the kernel mutex from
 * src/sys/kern/kern_lock.c r1.76 and r1.79.
 */

#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

extern int ncpus;

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

	owner = mtx->mtx_owner;
	if (owner == NULL) {
		owner = atomic_cas_ptr(&mtx->mtx_owner, NULL, self);
		if (owner == NULL) {
			membar_enter_after_atomic();
			return (1);
		}
	}

	return (0);
}

void
mtx_enter(struct mutex *mtx)
{
	unsigned int i, ncycle = 1;

	while (mtx_enter_try(mtx) == 0) {
		/* Busy loop with exponential backoff. */
		for (i = ncycle; i > 0; i--)
			CPU_BUSY_CYCLE();
		if (ncycle < ncpus)
			ncycle += ncycle;
	}
}

void
mtx_leave(struct mutex *mtx)
{
	membar_exit();
	mtx->mtx_owner = NULL;
}
