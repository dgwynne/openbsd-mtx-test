#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

struct mutex_waiter {
	unsigned int wait;
	TAILQ_ENTRY(mutex_waiter) entry;
};

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_spin = 0;
	mtx->mtx_owner = NULL;
	TAILQ_INIT(&mtx->mtx_waiting);
}

static void
mtx_enter_spin(struct mutex *mtx)
{
	while (atomic_cas_uint(&mtx->mtx_spin, 0, 1) != 0)
		CPU_BUSY_CYCLE();
	membar_enter_after_atomic();
}

static void
mtx_leave_spin(struct mutex *mtx)
{
	membar_exit();
	mtx->mtx_spin = 0;
}

int
mtx_enter_try(struct mutex *mtx)
{
	pthread_t self = pthread_self();
	pthread_t owner;

	mtx_enter_spin(mtx);
	owner = mtx->mtx_owner;
	if (owner == NULL)
		mtx->mtx_owner = self;
	mtx_leave_spin(mtx);

	return (owner == NULL);
}

void
mtx_enter(struct mutex *mtx)
{
	struct mutex_waiter w = { .wait = 1 };
	pthread_t self = pthread_self();
	pthread_t owner;

	mtx_enter_spin(mtx);
	owner = mtx->mtx_owner;
	if (owner == NULL)
		mtx->mtx_owner = self;
	else {
		if (mtx->mtx_waiting.tqh_last == NULL) { /* sigh */
			/* work around TAILQ_HEAD_INITIALIZER */
			mtx->mtx_waiting.tqh_last =
			    &mtx->mtx_waiting.tqh_first;
		}

		TAILQ_INSERT_TAIL(&mtx->mtx_waiting, &w, entry);
	}
	mtx_leave_spin(mtx);

	while (owner != NULL) {
		while (READ_ONCE(w.wait))
			CPU_BUSY_CYCLE();

		mtx_enter_spin(mtx);
		owner = mtx->mtx_owner;
		if (owner == NULL) {
			mtx->mtx_owner = self;
			TAILQ_REMOVE(&mtx->mtx_waiting, &w, entry);
		} else
			w.wait = 1;
		mtx_leave_spin(mtx);
	}
}

void
mtx_leave(struct mutex *mtx)
{
	struct mutex_waiter *n;

	mtx_enter_spin(mtx);
	mtx->mtx_owner = NULL;
	n = TAILQ_FIRST(&mtx->mtx_waiting);
	if (n != NULL)
		n->wait = 0;
	mtx_leave_spin(mtx);
}
