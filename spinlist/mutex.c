#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

void    _spinlock(volatile _atomic_lock_t *);
void    _spinunlock(volatile _atomic_lock_t *);

struct mutex_waiter {
	unsigned int wait;
	TAILQ_ENTRY(mutex_waiter) entry;
};

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_spin = _ATOMIC_LOCK_UNLOCKED;
	mtx->mtx_owner = NULL;
	TAILQ_INIT(&mtx->mtx_waiting);
}

int
mtx_enter_try(struct mutex *mtx)
{
	pthread_t self = pthread_self();
	pthread_t owner;

	_spinlock(&mtx->mtx_spin);
	owner = mtx->mtx_owner;
	if (owner == NULL)
		mtx->mtx_owner = self;
	_spinunlock(&mtx->mtx_spin);

	return (owner == NULL);
}

void
mtx_enter(struct mutex *mtx)
{
	struct mutex_waiter w = { .wait = 1 };
	pthread_t self = pthread_self();
	pthread_t owner;

	_spinlock(&mtx->mtx_spin);
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
	_spinunlock(&mtx->mtx_spin);

	while (owner != NULL) {
		while (READ_ONCE(w.wait))
			CPU_BUSY_CYCLE();

		_spinlock(&mtx->mtx_spin);
		owner = mtx->mtx_owner;
		if (owner == NULL) {
			mtx->mtx_owner = self;
			TAILQ_REMOVE(&mtx->mtx_waiting, &w, entry);
		} else
			w.wait = 1;
		_spinunlock(&mtx->mtx_spin);
	}
}

void
mtx_leave(struct mutex *mtx)
{
	struct mutex_waiter *n;

	_spinlock(&mtx->mtx_spin);
	mtx->mtx_owner = NULL;
	n = TAILQ_FIRST(&mtx->mtx_waiting);
	if (n != NULL)
		n->wait = 0;
	_spinunlock(&mtx->mtx_spin);
}
