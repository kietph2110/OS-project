#include "kernel/types.h"
#include "user/user.h"
#include "user/usync.h"

static int passed;

static void
check(int condition, const char *name)
{
  if (!condition) {
    printf("FAIL: %s\n", name);
    exit(1);
  }

  printf("PASS: %s\n", name);
  passed++;
}

static int
lock_state(uspinlock_t *lk)
{
  return __atomic_load_n(&lk->locked, __ATOMIC_RELAXED);
}

int
main(void)
{
  uspinlock_t lock;
  volatile int counter = 0;
  int i;
  int states_ok = 1;

  printf("usynctest: single-process spinlock test\n");

  uspin_init(&lock);
  check(lock_state(&lock) == 0, "init sets unlocked state");

  uspin_lock(&lock);
  check(lock_state(&lock) == 1, "lock sets locked state");

  uspin_unlock(&lock);
  check(lock_state(&lock) == 0, "unlock clears locked state");

  uspin_lock(&lock);
  check(lock_state(&lock) == 1, "lock can be acquired again");
  uspin_unlock(&lock);

  for (i = 0; i < 100000; i++) {
    uspin_lock(&lock);

    if (lock_state(&lock) != 1)
      states_ok = 0;

    counter++;

    uspin_unlock(&lock);

    if (lock_state(&lock) != 0)
      states_ok = 0;
  }

  check(states_ok && counter == 100000, "100000 lock/unlock cycles");

  printf("usynctest: %d/5 PASS\n", passed);
  exit(0);
}
