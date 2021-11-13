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

struct {
  struct spinlock lock;
  struct run *freelist;
} mem[NCPU];

void
kinit()
{
  // initlock(&kmem.lock, "kmem");
  // freerange(end, (void*)PHYSTOP);
  for(int i = 0; i < NCPU; i++)
    initlock(&mem[i].lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // acquire(&kmem.lock);
  // r->next = kmem.freelist;
  // kmem.freelist = r;
  // release(&kmem.lock);
  push_off();
  int id = cpuid();
  acquire(&mem[id].lock);
  r->next = mem[id].freelist;
  mem[id].freelist = r;
  release(&mem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // acquire(&kmem.lock);
  // r = kmem.freelist;
  // if(r)
  //   kmem.freelist = r->next;
  // release(&kmem.lock);

  push_off();
  int id = cpuid();
  acquire(&mem[id].lock);
  r = mem[id].freelist;
  if(r){
    mem[id].freelist = r->next;
    release(&mem[id].lock);
    memset((char*)r, 5, PGSIZE);
    pop_off();
    return (void*)r;
  }
  release(&mem[id].lock);
  for(int i = 0; i < NCPU; i++){
    if(id == i)
      continue;
    acquire(&mem[i].lock);
    r = mem[i].freelist;
    if(r){
      mem[i].freelist = r->next;
      release(&mem[i].lock);
      memset((char*)r, 5, PGSIZE);
      pop_off();
      return (void*)r;
    }
    release(&mem[i].lock);
  }
  pop_off();
  // if(r)
  //   memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
