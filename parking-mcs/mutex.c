/*
 * this is heavily inspired by the WTF::Lock from Locking in WebKit[1]
 * and some of the references it provides.
 *
 * the big idea taken from WTF::Lock is to separate the lock from
 * the machinery used to wait for the lock. this machinery is a
 * parking lot for cpus waiting to access mutexes, which is analogous
 * to what futex provides in userland, and the sleep queues underneath
 * kernel sleeps (and therefore sleeping locks like rwlock and cond).
 *
 * the parking lot allows struct mutex to remain small as it only
 * needs to record ownership and whether another cpu is "parked"
 * waiting for the lock.
 *
 * unlike WTF::Lock, the mutex is still a spinning lock. the parking
 * lot is used to publish the location each cpu is spinning on so the
 * current owner can locate a waiting cpu and write to the location
 * the other cpu is spinning on.
 *
 * unlike WTF::Lock, a woken CPU is responsible for taking itself
 * out of the parking lot. this reduces the number of states that are
 * represented by the lock word in struct mutex, and simplifies both
 * the acquisition and release of the lock. by keeping the waiter in
 * the parking lot until the lock is acquired, we amortise the cost
 * of adding and removing the waiter from the list if a woken cpu
 * loses a race to a "barging" cpu. this also maintains it's position
 * in the queue, making the lock more fair.
 *
 * 1. https://webkit.org/blog/6161/locking-in-webkit/
 */

#include <pthread.h>

#include <mutex.h>
#include "../atomic.h"

#include <machine/spinlock.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/*
 * compat with the kernel
 */
#define ISSET(_w, _m) ((_w) & (_m))

#define intr_disable() (0)
#define intr_restore(m) do { } while (0)

struct cpu_info;

#define curcpu() ((struct cpu_info *)1)

/*
 * pretend this is the top of src/sys/kern/kern_lock.c
 */
static void mtx_init_parking(void);

void __attribute__((constructor))
_kernel_lock_init(void)
{
	mtx_init_parking();
}

struct waiter {
	struct mutex		*mtx;
	TAILQ_ENTRY(waiter)	 entry;
#ifdef PARKING_PAD
	char			 __pad[128];
#endif
	volatile unsigned int	 wait;
} __aligned(64);

TAILQ_HEAD(mtx_waitlist, waiter);

struct mcs_node {
	struct mcs_node * volatile
				 next;
	volatile unsigned int	 wait;
};

struct mcs_lock {
	struct mcs_lock		*tail;
};

static void
mcs_init(struct mcs_lock *l)
{
	l->tail = NULL;
}

static void
mcs_enter(struct mcs_lock *l, struct mcs_node *n)
{
	struct mcs_node *pn;

	n->next = NULL;

	pn = atomic_swap_ptr(&l->tail, n);
	if (pn == NULL) {
		membar_enter_after_atomic();
		return;
	}

	n->wait = 1;
	pn->next = n;

	do {
		CPU_BUSY_CYCLE();
	} while (n->wait);

	membar_enter();
}

static void
mcs_leave(struct mcs_lock *l, struct mcs_node *n)
{
	struct mcs_node *nn;

	nn = n->next;
	if (nn == NULL) {
		membar_exit_before_atomic();
		if (atomic_cas_ptr(&l->tail, n, NULL) == n)
			return;

		do {
			CPU_BUSY_CYCLE();
			nn = n->next;
		} while (nn == NULL);
	} else
		membar_exit();

	nn->wait = 0;
}

struct mtx_park {
	struct mcs_lock		 lock;
	struct mtx_waitlist	 waiters;
} __aligned(CACHELINESIZE);

#define MTX_PARKING_BITS	7
#define MTX_PARKING_LOTS	(1 << MTX_PARKING_BITS)
#define MTX_PARKING_MASK	(MTX_PARKING_LOTS - 1)

static struct mtx_park mtx_parking[MTX_PARKING_LOTS];

static void
mtx_init_parking(void)
{
	size_t i;

	for (i = 0; i < nitems(mtx_parking); i++) {
		struct mtx_park *p = &mtx_parking[i];

		mcs_init(&p->lock);
		TAILQ_INIT(&p->waiters);
	}
}

static struct mtx_park *
mtx_park(struct mutex *mtx)
{
	unsigned long addr = (unsigned long)mtx;
	addr >>= 6;
	addr ^= addr >> MTX_PARKING_BITS;
	addr &= MTX_PARKING_MASK;

	return &mtx_parking[addr];
}

static unsigned long
mtx_enter_park(struct mtx_park *p, struct mcs_node *n)
{
	unsigned long m;

	m = intr_disable();
	mcs_enter(&p->lock, n);

	return (m);
}

static void
mtx_leave_park(struct mtx_park *p, struct mcs_node *n, unsigned long m)
{
	mcs_leave(&p->lock, n);
	intr_restore(m);
}

void
mtx_init(struct mutex *mtx)
{
	mtx->mtx_owner = 0;
}

int
mtx_enter_try(struct mutex *mtx)
{
	unsigned long self = (unsigned long)pthread_self();

	if (atomic_cas_ulong(&mtx->mtx_owner, 0, self) == 0) {
		membar_enter_after_atomic();
		return (1);
	}

	return (0);
}

#include <err.h>

void
mtx_enter(struct mutex *mtx)
{
	struct mtx_park *p;
	struct waiter w, *n;
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;
	struct mcs_node mn;
	unsigned long m;
#ifndef NOMEDIUM
	unsigned int i;
#endif

	owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
	if (owner == 0) {
		/* we got the lock first go. this is the fast path */
		goto locked;
	}

	if (__predict_false(owner == (self | 1))) {
		warnx("locking against myself owner %lx self %lx", owner, self);
		/*
		 * panic("%s(%p): locking against myself", __func__, mtx);
		 */
		abort();
	}

#ifndef NOMEDIUM
	for (i = 0; i < 40; i++) {
		if (ISSET(owner, 1))
			break;
		CPU_BUSY_CYCLE();
		owner = mtx->mtx_owner;
		if (owner == 0) {
			owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
			if (owner == 0)
				goto locked;
		}
	}
#endif

	w.mtx = mtx;

	/* take the really slow path */
	p = mtx_park(mtx);

	/* spinning++ */
	m = mtx_enter_park(p, &mn);
	TAILQ_INSERT_TAIL(&p->waiters, &w, entry);
	mtx_leave_park(p, &mn, m);

	do {
		unsigned long o;

		assert(owner != 0);

		w.wait = 1;
		membar_enter(); /* StoreStore|StoreLoad */
		o = atomic_cas_ulong(&mtx->mtx_owner, owner, owner | 1);
		if (o == owner) {
			while (w.wait)
				CPU_BUSY_CYCLE();
			membar_consumer(); /* don't pre-fetch owner */
		} else if (o != 0) {
			owner = o;
			continue;
		}

		owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self | 1);
	} while (owner != 0);

	m = mtx_enter_park(p, &mn);
	TAILQ_REMOVE(&p->waiters, &w, entry);
	mtx_leave_park(p, &mn, m);
	/* spinning-- */

locked:
	membar_enter_after_atomic();
}

void
mtx_leave(struct mutex *mtx)
{
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;

	membar_exit_before_atomic();
	owner = atomic_cas_ulong(&mtx->mtx_owner, self, 0);
	if (owner != self) {
		struct mtx_park *p;
		struct mcs_node mn;
		unsigned long m;
		struct waiter *w;

		if (__predict_false(owner != (self | 1))) {
			warnx("not owner, owner %lx self %lx", owner, self);
			/*
			 * panic("%s(%p): not owner", __func__, mtx);
			 */
			abort();
		}

		p = mtx_park(mtx);
		m = mtx_enter_park(p, &mn);
		mtx->mtx_owner = 0;
		membar_producer(); /* StoreStore */
		TAILQ_FOREACH(w, &p->waiters, entry) {
			if (w->mtx == mtx) {
				w->wait = 0;
				break;
			}
		}
		mtx_leave_park(p, &mn, m);
	}
}
