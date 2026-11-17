// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "refcount.h"

struct page_refcounts lock_counting;

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

// #define NPAGE (PHYSTOP/PGSIZE)

// struct{
//   struct spinlock lock;
//   int count[NPAGE];
// }lock_counting;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&lock_counting.lock,"lock_counting");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    lock_counting.count[(uint64)p/PGSIZE] = 1;
    kfree(p);
  }

}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  acquire(&lock_counting.lock);
  if(lock_counting.count[(uint64)pa / PGSIZE] <= 0)
    panic("kfree refcount");
  lock_counting.count[(uint64)pa / PGSIZE]--;
  if(lock_counting.count[(uint64)pa / PGSIZE] > 0){
    release(&lock_counting.lock);
    return;
  }
  release(&lock_counting.lock);
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
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
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    acquire(&lock_counting.lock);
    lock_counting.count[(uint64)r / PGSIZE] = 1;
    release(&lock_counting.lock);
  }
  return (void*)r;
}
