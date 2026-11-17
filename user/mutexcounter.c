#include "kernel/types.h"
#include "user/user.h"
#include "user/usync.h"

#define WORKERS 4
#define LOOPS   10000

struct shared_data {
  mutex_t lock;
  int counter;
};

static void
check(int ok, const char *name)
{
  if (!ok) {
    printf("FAIL: %s\n", name);
    exit(1);
  }

  printf("PASS: %s\n", name);
}

static void
basic_test(void)
{
  mutex_t m;
  int counter = 0;

  mutex_init(&m);
  check(__atomic_load_n(&m.state, __ATOMIC_RELAXED) == 0, "mutex initialized");

  mutex_lock(&m);
  check(__atomic_load_n(&m.state, __ATOMIC_RELAXED) == 1, "uncontended lock");
  mutex_unlock(&m);

  check(__atomic_load_n(&m.state, __ATOMIC_RELAXED) == 0, "mutex unlocked");

  for (int i = 0; i < LOOPS; i++) {
    mutex_lock(&m);
    counter++;
    mutex_unlock(&m);
  }

  check(counter == LOOPS, "repeated lock/unlock");
}

int
main(void)
{
  struct shared_data *s;
  int gate[2];
  int created = 0;
  int failed = 0;
  char token = 'G';

  printf("mutexcounter: week 4 mutex test\n");
  basic_test();

  s = shmalloc(1);
  check(s != 0, "shared allocation");

  mutex_init(&s->lock);
  s->counter = 0;

  if (pipe(gate) < 0) {
    printf("FAIL: pipe\n");
    exit(1);
  }

  // Children wait until the parent has created the workers.
  for (int i = 0; i < WORKERS; i++) {
    int pid = fork();

    if (pid < 0) {
      failed = 1;
      break;
    }

    if (pid == 0) {
      close(gate[1]);

      if (read(gate[0], &token, 1) != 1) {
        close(gate[0]);
        exit(1);
      }

      close(gate[0]);

      for (int j = 0; j < LOOPS; j++) {
        mutex_lock(&s->lock);

        int value = s->counter;

        // Encourage contention even when running on one CPU.
        if (j % 1000 == 0)
          pause(1);

        s->counter = value + 1;

        mutex_unlock(&s->lock);
      }

      exit(0);
    }

    created++;
  }

  close(gate[0]);

  for (int i = 0; i < created; i++) {
    if (write(gate[1], &token, 1) != 1) {
      failed = 1;
      break;
    }
  }

  close(gate[1]);

  for (int i = 0; i < created; i++) {
    int status = -1;

    if (wait(&status) < 0 || status != 0)
      failed = 1;
  }

  printf("Workers: %d\n", created);
  printf("Increments per worker: %d\n", LOOPS);
  printf("Expected counter: %d\n", WORKERS * LOOPS);
  printf("Actual counter: %d\n", s->counter);

  check(!failed && created == WORKERS, "all workers completed");
  check(s->counter == WORKERS * LOOPS, "shared counter correct");
  check(__atomic_load_n(&s->lock.state, __ATOMIC_RELAXED) == 0,
        "mutex released after workers");

  printf("mutexcounter: ALL PASS\n");
  exit(0);
}
