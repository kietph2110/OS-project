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

/*
 * Initialize a counting semaphore.
 */
void
sem_init(sem_t *s, int initial)
{
  mutex_init(&s->lock);
  s->value = initial < 0 ? 0 : initial;
  s->next_ticket = 0;
  s->served_ticket = 0;
  __atomic_store_n(&s->sequence, 0, __ATOMIC_RELAXED);
}

/*
 * Acquire one permit, sleeping if unavailable.
 */
void
sem_wait(sem_t *s)
{
  mutex_lock(&s->lock);

  s->value--;

  // A permit was immediately available.
  if (s->value >= 0) {
    mutex_unlock(&s->lock);
    return;
  }

  // No permit: obtain a waiting ticket.
  int ticket = s->next_ticket++;

  for (;;) {
    // This ticket has received a permit.
    if (s->served_ticket > ticket) {
      mutex_unlock(&s->lock);
      return;
    }

    int expected =  __atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE);

    mutex_unlock(&s->lock);

    // May sleep, or return immediately if sequence changed.
    futex_wait(&s->sequence, expected);

    mutex_lock(&s->lock);
  }
}

/*
 * Release one permit.
 */
void
sem_post(sem_t *s)
{
  int wake;

  mutex_lock(&s->lock);

  // A negative value means a wait request is unserved.
  wake = (s->value < 0);
  s->value++;

  if (wake) {
    s->served_ticket++;
    __atomic_add_fetch(&s->sequence, 1, __ATOMIC_RELEASE);
  }

  mutex_unlock(&s->lock);

  // Wake all registered waiters; ticket order
  // decides which waiter has received a permit.
  if (wake)
    futex_wake(&s->sequence, 64);
}
