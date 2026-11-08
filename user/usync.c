#include "usync.h"

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
