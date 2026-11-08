#include "kernel/types.h"
#include "user/user.h"

// Checks that futex_wait/futex_wake reach the kernel
// and reject bad arguments.

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

int
main(void)
{
  expect("valid addr", futex_wake(&word, 1), 0);
  expect("unaligned addr", futex_wake((int *)((char *)&word + 1), 1), -1);
  expect("unmapped addr", futex_wake((int *)0x80000000L, 1), -1);
  expect("bad n", futex_wake(&word, 0), -1);
  expect("wait valid addr", futex_wait(&word, 0), 0);
  expect("wait wrong val", futex_wait(&word, 1), -1);
  expect("wake no waiter", futex_wake(&word, 5), 0);
  exit(fails);
}
