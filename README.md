# OpenBSD mutex

This is a harness for trying different algorithms as backends for
the OpenBSD kernel mutex lock.

The harness runs a number of threads and runs a loop that takes the
lock, increments a number in the critical section, and then releases
the lock. This means the critical section is incredibly short, so
the run time should be dominated by the locking operations and will
be most affected by how they deal with contention. The number that
is incremented is right next to the mutex in memory, meaning the
lock and the work fight each other for the cacheline, adding more
contention.

Each subdirectory contains a mutex implementation, and reaches back
into the root of the repository to include the test harness. Each
subdir builds a binary called `test`.

```
usage: test [-n nthreads] [-l nloops]
```

The tests should build fine on an OpenBSD box with `make`.

The harness will time the work itself:

```
$ ./spinlock/obj/test -n 8
starting 8 threads for 1000000 loops
real time: 1.52s, user time: 8.34s
$ ./ticket/obj/test -n 8
starting 8 threads for 1000000 loops
real time: 6.73s, user time: 45.32s
$ 
``` 
