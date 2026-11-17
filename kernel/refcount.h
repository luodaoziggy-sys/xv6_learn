#ifndef XV6_REFCOUNT_H
#define XV6_REFCOUNT_H

#define NPAGE (PHYSTOP / PGSIZE)

struct page_refcounts {
  struct spinlock lock;
  int count[NPAGE];
};

extern struct page_refcounts lock_counting;

#endif