#include "kernel/types.h"
#include "user/user.h"

// Tests for shmalloc(): pages from shmalloc() are shared between
// parent and child after fork(), ordinary memory is still copied.

#define PGSIZE 4096

int fails;
int private_var = 1;

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

void
bad_args(void)
{
  expect("shmalloc(0)", shmalloc(0) == 0, 1);
  expect("shmalloc(-1)", shmalloc(-1) == 0, 1);
  expect("shmalloc(17)", shmalloc(17) == 0, 1);
}

void
zeroed_page(void)
{
  char *p = shmalloc(1);
  int nonzero = 0;

  expect("shmalloc(1) ok", p != 0, 1);
  if (p == 0)
    return;
  expect("page aligned", ((uint64)p % PGSIZE) == 0, 1);
  for (int i = 0; i < PGSIZE; i++)
    if (p[i] != 0)
      nonzero++;
  expect("page zeroed", nonzero, 0);
}

// The child writes to the shared page and to a global variable;
// only the shared page change is visible to the parent.
void
child_writes(void)
{
  int *p = shmalloc(1);

  if (fork() == 0) {
    p[0] = 42;
    private_var = 2;
    exit(0);
  }
  wait(0);
  expect("child write seen by parent", p[0], 42);
  expect("private memory still copied", private_var, 1);
}

void
many_pages(void)
{
  int *q = shmalloc(4);

  if (q == 0) {
    expect("shmalloc(4) ok", 0, 1);
    return;
  }
  if (fork() == 0) {
    for (int i = 0; i < 4; i++)
      q[i * PGSIZE / sizeof(int)] = i + 1;
    exit(0);
  }
  wait(0);
  int ok = 1;
  for (int i = 0; i < 4; i++)
    if (q[i * PGSIZE / sizeof(int)] != i + 1)
      ok = 0;
  expect("4 pages shared", ok, 1);
}

void
grandchild(void)
{
  int *p = shmalloc(1);

  if (fork() == 0) {
    if (fork() == 0) {
      p[0] = 99;
      exit(0);
    }
    wait(0);
    exit(0);
  }
  wait(0);
  expect("grandchild write seen", p[0], 99);
}

// Process A allocates a shared page, forks B and exits at once.
// B must still be able to use the page after A is gone.
void
parent_exits_first(void)
{
  int *r = shmalloc(1);

  if (fork() == 0) {
    int *s = shmalloc(1);
    s[0] = 5;
    if (fork() == 0) {
      pause(3);
      r[0] = (s[0] == 5) ? 1 : -1;
      s[0] = 6;
      exit(0);
    }
    exit(0);
  }
  wait(0);
  for (int i = 0; i < 100 && r[0] == 0; i++)
    pause(1);
  expect("page survives parent exit", r[0], 1);
}

// The parent sleeps in futex_wait on a word in the shared page and
// the child wakes it up.
void
futex_shared(void)
{
  int *w = shmalloc(1);

  if (fork() == 0) {
    pause(2);
    *w = 1;
    futex_wake(w, 1);
    exit(0);
  }
  while (*w == 0)
    futex_wait(w, 0);
  wait(0);
  expect("futex wake across processes", *w, 1);
}

void
repeat_fork(void)
{
  int *c = shmalloc(1);

  for (int i = 0; i < 50; i++) {
    if (fork() == 0) {
      c[0]++;
      exit(0);
    }
    wait(0);
  }
  expect("50 fork/exit rounds", c[0], 50);
}

int
main(void)
{
  bad_args();
  zeroed_page();
  child_writes();
  many_pages();
  grandchild();
  parent_exits_first();
  futex_shared();
  repeat_fork();
  if (fails == 0)
    printf("shmtest: ALL PASS\n");
  else
    printf("shmtest: %d FAIL\n", fails);
  exit(fails);
}
