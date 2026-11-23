#include "kernel/types.h"
#include "user/user.h"

// Tests for futex_wait/futex_wake: bad arguments, a killed waiter
// and waking a process that sleeps on a shared page.

int word;
int fails;

void
expect(char *name, int got, int want)
{
  if (got == want) {
    printf("%s: PASS\n", name);
  } else {
    printf("%s: FAIL (got %d, want %d)\n", name, got, want);
    fails++;
  }
}

// A child sleeps in futex_wait with nobody to wake it; the parent kills
// it and the child must leave the kernel instead of sleeping forever.
void
killed_waiter(void)
{
  int pid = fork();

  if (pid == 0) {
    futex_wait(&word, 0);
    // A killed process never gets back to user space.
    exit(0);
  }
  pause(5);
  kill(pid);
  int status = -2;
  wait(&status);
  expect("kill sleeping waiter", status, -1);
}

// The parent sleeps in futex_wait on a word in a shared page and the
// child wakes it. The child exits with what futex_wake returned.
void
parent_wait_child_wake(void)
{
  int *w = shmalloc(1);
  int r, status;

  if (w == 0) {
    expect("shmalloc for wait/wake", 0, 1);
    return;
  }
  if (fork() == 0) {
    pause(5);
    *w = 1;
    exit(futex_wake(w, 1));
  }
  r = futex_wait(w, 0);
  status = -1;
  wait(&status);
  expect("parent woken by child", r, 0);
  expect("word set by child", *w, 1);
  expect("child woke one waiter", status, 1);
}

// Two children sleep on the same word. futex_wake(w, 1) must wake
// exactly one of them each time, and nobody once both are gone.
void
wake_one_at_a_time(void)
{
  int *w = shmalloc(1);
  int *ready = w + 1;

  if (w == 0) {
    expect("shmalloc for wake count", 0, 1);
    return;
  }
  for (int i = 0; i < 2; i++) {
    if (fork() == 0) {
      __atomic_fetch_add(ready, 1, __ATOMIC_SEQ_CST);
      while (*w == 0)
        futex_wait(w, 0);
      exit(0);
    }
  }
  while (*ready < 2)
    pause(1);
  // Give both children time to get into futex_wait.
  pause(5);
  *w = 1;
  expect("first wake", futex_wake(w, 1), 1);
  expect("second wake", futex_wake(w, 1), 1);
  expect("third wake", futex_wake(w, 1), 0);
  wait(0);
  wait(0);
}

int
main(void)
{
  expect("valid addr", futex_wake(&word, 1), 0);
  expect("unaligned addr", futex_wake((int *)((char *)&word + 1), 1), -1);
  expect("unmapped addr", futex_wake((int *)0x80000000L, 1), -1);
  expect("bad n", futex_wake(&word, 0), -1);
  expect("wait wrong val", futex_wait(&word, 1), -1);
  expect("wake no waiter", futex_wake(&word, 5), 0);
  killed_waiter();
  parent_wait_child_wake();
  wake_one_at_a_time();
  exit(fails);
}
