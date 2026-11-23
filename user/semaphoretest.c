
#include "kernel/types.h"
#include "user/user.h"
#include "user/usync.h"

#define WORKERS 4
#define LOOPS 20

struct shared_data {
  sem_t sem;
  int active;
  int violations;
  int completed;
};

static void
check(int condition, const char *name)
{
  if (!condition) {
    printf("FAIL: %s\n", name);
    exit(1);
  }
  printf("PASS: %s\n", name);
}

static void
test_blocking(struct shared_data *s)
{
  sem_init(&s->sem, 0);
  s->completed = 0;

  int pid = fork();
  check(pid >= 0, "fork blocking test");

  if (pid == 0) {
    sem_wait(&s->sem);
    __atomic_add_fetch(&s->completed, 1,
                       __ATOMIC_SEQ_CST);
    exit(0);
  }

  // Wait until the child records its wait request.
  int waiting = 0;
  for (int i = 0; i < 100; i++) {
    mutex_lock(&s->sem.lock);
    waiting = (s->sem.value == -1);
    mutex_unlock(&s->sem.lock);

    if (waiting)
      break;
    pause(1);
  }

  check(waiting, "child registered wait");
  check(s->completed == 0, "child is blocked");

  sem_post(&s->sem);

  int status = -1;
  check(wait(&status) == pid && status == 0,
        "child woke successfully");
  check(s->completed == 1,
        "blocking semaphore correct");
}

static void
test_contention(struct shared_data *s)
{
  sem_init(&s->sem, 2);
  s->active = 0;
  s->violations = 0;
  s->completed = 0;

  for (int i = 0; i < WORKERS; i++) {
    int pid = fork();
    check(pid >= 0, "fork worker");

    if (pid == 0) {
      for (int j = 0; j < LOOPS; j++) {
        sem_wait(&s->sem);

        int n = __atomic_add_fetch(
          &s->active, 1, __ATOMIC_SEQ_CST);

        if (n > 2)
          __atomic_add_fetch(
            &s->violations, 1,
            __ATOMIC_SEQ_CST);

        // Keep the permit briefly to cause contention.
        pause(1);

        __atomic_sub_fetch(
          &s->active, 1, __ATOMIC_SEQ_CST);

        __atomic_add_fetch(
          &s->completed, 1,
          __ATOMIC_SEQ_CST);

        sem_post(&s->sem);
      }
      exit(0);
    }
  }

  for (int i = 0; i < WORKERS; i++) {
    int status = -1;
    check(wait(&status) > 0 && status == 0,
          "worker completed");
  }

  printf("Expected operations: %d\n",
         WORKERS * LOOPS);
  printf("Actual operations: %d\n",
         s->completed);
  printf("Permit violations: %d\n",
         s->violations);

  check(s->completed == WORKERS * LOOPS,
        "all operations completed");
  check(s->violations == 0,
        "at most two concurrent workers");
  check(s->active == 0,
        "no active workers remain");
  check(s->sem.value == 2,
        "all permits returned");
}

int
main(void)
{
  struct shared_data *s = shmalloc(1);
  check(s != 0, "shared memory allocation");

  test_blocking(s);
  test_contention(s);

  printf("semaphoretest: ALL PASS\n");
  exit(0);
}
