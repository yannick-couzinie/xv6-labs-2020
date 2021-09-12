// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define REFPOS(pa) (((uint64) pa - (uint64) end)/PGSIZE)

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 ref_count[32730];
  // 32730 = REFPOS(PHYSTOP)
} kmem;


void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    kmem.ref_count[REFPOS(p)] = 1; // kfree will free everything and put refs back to 0
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  kmem.ref_count[REFPOS(pa)]--;
  if (kget_ref((uint64) pa) <= 0){
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    r->next = kmem.freelist;
    kmem.freelist = r;
  }
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
  if(r){
    kmem.freelist = r->next;
    kmem.ref_count[REFPOS(r)] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// call this whenever you want to write (do not call when reading) onto a page
// that might not have been alloced accordingly, trap and copyout call this.
// Only allocs if PTE_RSW1 is set, since we only make cow pages on pages that
// had writing permissions originally.
int
cow_alloc(uint64 va, pagetable_t pagetable){
    pte_t *pte;
    uint64 pa, a;
    char *mem;
    
    if (va >= MAXVA)
      return -1;

    a = PGROUNDDOWN(va);
    // Let us put this into its own fucntion we can also call from copyout
    if((pte = walk(pagetable, a, 0)) != 0){
      // this page exists
      if ((*pte & PTE_V) == 0){
        // invalid page
        return -1;
      }
      if ((*pte & PTE_RSW1) != 0){
        pa = PTE2PA(*pte);
        acquire(&kmem.lock);
        if(kget_ref(pa) == 1){
          // there is only one reference left so we can make the current page
          // writable and remove the cow flag
          *pte = (*pte | PTE_W) & ~PTE_RSW1;
          release(&kmem.lock);
          return 0;
        } else if (kget_ref(pa) < 1)
          panic("should not happen");

        release(&kmem.lock);

        if((mem = kalloc()) == 0){
          return -1;
        }

        memmove(mem, (char*)pa, PGSIZE);
        *pte = (PA2PTE(mem) | PTE_FLAGS(*pte) | PTE_W) & ~PTE_RSW1;
        kdecr_ref(pa);
      }
      return 0;
    }
    return -1;
}


// increment the ref count of page belonging to pa
void
kincr_ref(uint64 pa){
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kincr_ref");
  acquire(&kmem.lock);
  kmem.ref_count[REFPOS(pa)]++;
  release(&kmem.lock);
}

void
kdecr_ref(uint64 pa){
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kdecr_ref");
  acquire(&kmem.lock);
  kmem.ref_count[REFPOS(pa)]--;
  release(&kmem.lock);
}

int
kget_ref(uint64 pa){
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kget_ref");
  return kmem.ref_count[REFPOS(pa)];
}
