// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end, int cpu_id);
void kfree_with_id(void *pa, int cpu_id);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  uint64 per_cpu_mem, i;
  void* upper;
  per_cpu_mem = ((uint64) PHYSTOP - (uint64) end)/NCPU;
  for(i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
    if (i < NCPU-1)
      upper = (uint64) end + (void*) ((i+1)*per_cpu_mem);
    else
      upper = (void*) PHYSTOP;
    freerange((char*) ((uint64) end + i*per_cpu_mem), upper, i);
  }
}

void
freerange(void *pa_start, void *pa_end, int cpu_id)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    kfree_with_id(p, cpu_id);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  push_off();
  int cpu_id = cpuid();
  pop_off();
  kfree_with_id(pa, cpu_id);
}

void kfree_with_id(void *pa, int cpu_id){
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int cpu_id, i;

  push_off();
  cpu_id = cpuid();
  pop_off();
  for (i=0; i<NCPU; i++){
    acquire(&kmem[cpu_id].lock);
    r = kmem[cpu_id].freelist;
    if(r)
      kmem[cpu_id].freelist = r->next;
    release(&kmem[cpu_id].lock);
    if(r)
      break;
    cpu_id = (cpu_id + 1) % NCPU;
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
