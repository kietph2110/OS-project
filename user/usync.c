#include "kernel/types.h"
#include "user/user.h"
#include "user/usync.h"

// Initialize before making the lock available to other users.
void
uspin_init(uspinlock_t *lk)
{
  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELAXED);
}

// Busy-wait until the lock is acquired.
void
uspin_lock(uspinlock_t *lk)
{
  while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0)
    ;
}

// Only the current holder may release the lock.
void
uspin_unlock(uspinlock_t *lk)
{
  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
}

// Initialize before exposing the mutex to other processes.
void
mutex_init(mutex_t *m)
{
  __atomic_store_n(&m->state, 0, __ATOMIC_RELAXED);
}

void
mutex_lock(mutex_t *m)
{
  int expected = 0;

  // Fast path: acquire without entering the kernel.
  if (__atomic_compare_exchange_n(&m->state, &expected, 1, 0, __ATOMIC_ACQUIRE,
                                  __ATOMIC_RELAXED))
    return;

  // Slow path: keep the possible-waiter state when acquiring.
  for (;;) {
    if (__atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE) == 0)
      return;

    futex_wait(&m->state, 2);
  }
}

void
mutex_unlock(mutex_t *m)
{
  int old = __atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE);

  if (old == 2)
    futex_wake(&m->state, 1);
}
