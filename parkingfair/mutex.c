/*
 * this is an extended version of ../parking/mutex.c that adds extra
 * fairness by having mtx_leave move ownership instead of release it
 * if the next cpu has lost too many races to take the lock.
 *
 * the "fairness" is tunable with the -x opt.
 *
 *
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
	struct mutex		*volatile mtx;
	unsigned long		 self;
	unsigned int		 spins;
	TAILQ_ENTRY(waiter)	 entry;
} __aligned(64);

TAILQ_HEAD(mtx_waitlist, waiter);

struct mtx_park {
	struct cpu_info		*lock;
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

		p->lock = NULL;
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
mtx_enter_park(struct mtx_park *p)
{
	struct cpu_info * const ci = curcpu();
	unsigned long m;

	m = intr_disable();
	while (atomic_cas_ptr(&p->lock, NULL, ci) != 0)
		CPU_BUSY_CYCLE();
	membar_enter_after_atomic();

	return (m);
}

static void
mtx_leave_park(struct mtx_park *p, unsigned long m)
{
	membar_exit();
	p->lock = NULL;
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

void
mtx_enter(struct mutex *mtx)
{
	struct mtx_park *p;
	struct waiter w, *n;
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;
	unsigned int i;
	unsigned long m;

	owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
	if (owner == 0) {
		/* we got the lock first go. this is the fast path */
		goto locked;
	}

	if (__predict_false(owner == (self | 1))) {
		/*
		 * panic("%s(%p): locking against myself", __func__, mtx);
		 */
		abort();
	}

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

	/* take the really slow path */
	p = mtx_park(mtx);

	/* spinning++ */
	w.mtx = mtx;
	w.self = self;
	w.spins = 0;
	m = mtx_enter_park(p);
	TAILQ_INSERT_TAIL(&p->waiters, &w, entry);
	mtx_leave_park(p, m);

	for (;;) {
		unsigned long nowner = owner | 1;
		unsigned long o;

		o = atomic_cas_ulong(&mtx->mtx_owner, owner, nowner);
		if (o == owner)
			o = nowner;
		if ((o | 1) == (self | 1))
			break;
		if (ISSET(o, 1)) {
			while (w.mtx != NULL)
				CPU_BUSY_CYCLE();
			w.spins++;
		}

		owner = atomic_cas_ulong(&mtx->mtx_owner, 0, self);
		if (owner == 0) // || (owner & ~1UL) == self)
			break;

		w.mtx = mtx;
	}

	m = mtx_enter_park(p);
	TAILQ_REMOVE(&p->waiters, &w, entry);
	TAILQ_FOREACH(n, &p->waiters, entry) {
		if (n->mtx == mtx) {
			mtx->mtx_owner = self | 1;
			break;
		}
	}
	mtx_leave_park(p, m);
	/* spinning-- */

locked:
	membar_enter_after_atomic();
}

extern unsigned int x;

void
mtx_leave(struct mutex *mtx)
{
	unsigned long self = (unsigned long)pthread_self();
	unsigned long owner;

	membar_exit_before_atomic();
	owner = atomic_cas_ulong(&mtx->mtx_owner, self, 0);
	if (owner != self) {
		struct mtx_park *p;
		unsigned long m;
		struct waiter *w;

		if (__predict_false(owner != (self | 1))) {
			/*
			 * panic("%s(%p): not owner", __func__, mtx);
			 */
			abort();
		}

		p = mtx_park(mtx);
		m = mtx_enter_park(p);
		TAILQ_FOREACH(w, &p->waiters, entry) {
			if (w->mtx == mtx) {
				mtx->mtx_owner = (w->spins > x) ? w->self : 0;
				w->mtx = NULL;
				goto leave;
			}
		}
		mtx->mtx_owner = 0;
leave:
		mtx_leave_park(p, m);
	}
}
