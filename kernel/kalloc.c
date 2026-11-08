// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// Reference count for every physical page in [KERNBASE, PHYSTOP).
// kalloc() sets it to 1, kfree() decrements it and only returns the
// page to the freelist when it reaches 0. A page shared between
// processes (PTE_SH) gets extra references through kref_inc().
#define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
#define NPHYSPAGES ((PHYSTOP - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int refcnt[NPHYSPAGES];
} kref;

static void
checkpa(void *pa, char *who)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic(who);
}

// Add one reference to an allocated page.
void
kref_inc(void *pa)
{
  checkpa(pa, "kref_inc");
  acquire(&kref.lock);
  if (kref.refcnt[PA2IDX(pa)] <= 0)
    panic("kref_inc: free page");
  kref.refcnt[PA2IDX(pa)]++;
  release(&kref.lock);
}

// Current number of references (for tests/debugging).
int
kref_get(void *pa)
{
  int n;
  checkpa(pa, "kref_get");
  acquire(&kref.lock);
  n = kref.refcnt[PA2IDX(pa)];
  release(&kref.lock);
  return n;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kref.lock, "kref");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    kref.refcnt[PA2IDX(p)] = 1; // so that kfree() drops it to 0
    kfree(p);
  }
}

// Drop one reference to the page of physical memory pointed at by pa,
// which normally should have been returned by a call to kalloc().
// The page goes back to the freelist only when no reference is left.
// (The exception is when initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int left;

  checkpa(pa, "kfree");

  acquire(&kref.lock);
  if (kref.refcnt[PA2IDX(pa)] <= 0)
    panic("kfree: double free");
  left = --kref.refcnt[PA2IDX(pa)];
  release(&kref.lock);
  if (left > 0)
    return; // still used by another process

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r) {
    acquire(&kref.lock);
    if (kref.refcnt[PA2IDX(r)] != 0)
      panic("kalloc: refcnt");
    kref.refcnt[PA2IDX(r)] = 1;
    release(&kref.lock);
    memset((char *)r, 5, PGSIZE); // fill with junk
  }
  return (void *)r;
}
