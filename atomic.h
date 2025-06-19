
#include <sys/atomic.h>

#if defined(__i386__) || defined(__amd64__)
#define CPU_BUSY_CYCLE()	asm volatile("pause": : : "memory")
#elif defined(__sparcv9__)
        __asm volatile(                                                 \
                "       rd      %%ccr, %%g0                     \n"     \
                "       rd      %%ccr, %%g0                     \n"     \
                "       rd      %%ccr, %%g0                     \n"     \
                : : : "memory")
#else
#define CPU_BUSY_CYCLE()	asm volatile("": : : "memory")
#endif

static inline void
membar_datadep_consumer(void)
{
#ifdef __alpha__
	membar_consumer();
#endif
}

#define READ_ONCE(x) ({                                                 \
	typeof(x) __tmp = *(volatile typeof(x) *)&(x);                  \
	membar_datadep_consumer();                                      \
	__tmp;                                                          \
})

#define WRITE_ONCE(x, val) ({                                           \
	typeof(x) __tmp = (val);                                        \
	*(volatile typeof(x) *)&(x) = __tmp;                            \
	__tmp;                                                          \
})

#define KASSERT(c) assert(c)
