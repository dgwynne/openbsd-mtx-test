#include <machine/spinlock.h>
#include <sys/queue.h>

struct mutex_waiter;
TAILQ_HEAD(mutex_waiting, mutex_waiter);

struct mutex {
	_atomic_lock_t		 mtx_spin;
	pthread_t		 mtx_owner;
	struct mutex_waiting	 mtx_waiting;
};

#include "../mutex_api.h"
