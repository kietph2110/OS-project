#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Sleeps on addr if the futex word still equals val, else returns -1.
uint64
sys_futex_wait(void)
{
  uint64 addr, key;
  int val;

  argaddr(0, &addr);
  argint(1, &val);
  key = futex_key(myproc()->pagetable, addr);
  if (key == 0)
    return -1;

  acquire(&futex_lock);
  if (*(int *)key != val) {
    release(&futex_lock);
    return -1;
  }
  // Register on the channel before dropping futex_lock so a wake that
  // arrives in between still finds this process.
  sleep_prepare((void *)key);
  release(&futex_lock);
  sleep();

  // kkill() wakes a sleeper without clearing its channel; clear it here
  // so a later futex_wake does not count this process as a waiter.
  if (killed(myproc())) {
    acquire(&myproc()->lock);
    myproc()->chan = 0;
    release(&myproc()->lock);
    return -1;
  }
  return 0;
}

// Wakes at most n waiters on addr and returns how many.
uint64
sys_futex_wake(void)
{
  uint64 addr, key;
  int n, woken;

  argaddr(0, &addr);
  argint(1, &n);
  if (n <= 0)
    return -1;
  key = futex_key(myproc()->pagetable, addr);
  if (key == 0)
    return -1;

  acquire(&futex_lock);
  woken = wakeup_n((void *)key, n);
  release(&futex_lock);
  return woken;
}
