#include "kernel/types.h"
#include "user/user.h"

// Runs the project's tests one after another and reports which ones
// failed. Exits with the number of failed tests.

char *tests[] = {
  "futexstub",
  "shmtest",
  "usynctest",
  "mutexcounter",
};

#define NTESTS ((int)(sizeof(tests) / sizeof(tests[0])))

// Runs one test in a child process and returns its exit status.
int
run(char *name)
{
  char *argv[] = {name, 0};
  int pid, status;

  pid = fork();
  if (pid < 0) {
    printf("projecttests: fork failed\n");
    return -1;
  }
  if (pid == 0) {
    exec(name, argv);
    printf("projecttests: exec %s failed\n", name);
    exit(1);
  }
  status = -1;
  wait(&status);
  return status;
}

int
main(void)
{
  int fails = 0;

  for (int i = 0; i < NTESTS; i++) {
    printf("=== %s\n", tests[i]);
    if (run(tests[i]) == 0) {
      printf("=== %s: PASS\n", tests[i]);
    } else {
      printf("=== %s: FAIL\n", tests[i]);
      fails++;
    }
  }
  if (fails == 0)
    printf("projecttests: ALL PASS\n");
  else
    printf("projecttests: %d of %d FAIL\n", fails, NTESTS);
  exit(fails);
}
