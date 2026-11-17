// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void *superblock[KALLOC_2M]; // 2M pages list


void freerange(void *pa_start, void *pa_end);
void freerange_2M(void *pa_start, void *pa_end);
extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange_2M(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
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

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void
freerange_2M(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  int si = 0;
  while (p+PGSIZE<=(char*)pa_end)
  {
   if(si<KALLOC_2M&&((uint64)p%KALLOC_2M_SIZE)==0&&((uint64)p+KALLOC_2M_SIZE)<=(uint64)pa_end)
   {
    superblock[si] = p;
    si++;
    p+=KALLOC_2M_SIZE;
   }
   else{
    kfree(p);
    p+=PGSIZE;
   }
  }

}


void *
kalloc_2M(void)
{
  void *p=0;
    acquire(&kmem.lock);
    for(int i = 0; i < KALLOC_2M; i++){
        if(superblock[i]){
            p = superblock[i];
            superblock[i] = 0;
            break;
        }
    }
    release(&kmem.lock);

    if(p)
        memset(p, 5, KALLOC_2M_SIZE);   // 填充垃圾（调试）
    return p;

}

void
superfree(void *p)
{
    acquire(&kmem.lock);
    for(int i = 0; i < KALLOC_2M; i++){
        if(superblock[i] == 0){
            superblock[i] = p;
            break;
        }
    }
    release(&kmem.lock);
}
