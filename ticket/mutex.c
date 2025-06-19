#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

void
mtx_init(struct mutex *mtx)
{
	mtx->tick = 0;
	mtx->next = 0;
}

int
mtx_enter_try(struct mutex *mtx)
{
	return (0);
}

#define atomic_xadd(P, V) __sync_fetch_and_add((P), (V))

void
mtx_enter(struct mutex *mtx)
{
	unsigned int next = atomic_xadd(&mtx->next, 1);
	while (mtx->tick != next)
		CPU_BUSY_CYCLE();
}

void
mtx_leave(struct mutex *mtx)
{
	membar_exit();
	mtx->tick++;
}
