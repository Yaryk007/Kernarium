/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Just enough of the kernel for the management console's wake bookkeeping,
 * which test_darwin_mconsole_suspend.py cuts out of the series' topic and
 * compiles with this header in front. Everything the cut-out code uses is
 * declared here; driver.cpp plays the console around it.
 */
#ifndef MCONSOLE_SUSPEND_KERNEL_H
#define MCONSOLE_SUSPEND_KERNEL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	int counter;
} atomic_t;
#define ATOMIC_INIT(value) { (value) }

static inline int atomic_read(const atomic_t *v)
{
	return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}

static inline void atomic_inc(atomic_t *v)
{
	__atomic_fetch_add(&v->counter, 1, __ATOMIC_RELAXED);
}

#define smp_mb__after_atomic() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, value) (*(volatile __typeof__(x) *)&(x) = (value))

/* The members of the console's request that the bookkeeping reads or writes. */
struct mc_request {
	int wakes;
	int id;
};

#endif
