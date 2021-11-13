// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

struct {
  struct spinlock lock;
  struct buf head;
} bucket[NBUCKET];

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
  }
  b = bcache.buf;
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bucket[i].lock, "bcache");
    for(int j = 0; j < NBUF / NBUCKET; j++){
      b->blockno = i;
      b->next = bucket[i].head.next;
      bucket[i].head.next = b;
      b++;
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // acquire(&bcache.lock);

  int id = blockno % NBUCKET;
  acquire(&bucket[id].lock);

  // Is the block already cached?
  // for(b = bcache.head.next; b != &bcache.head; b = b->next){
  //   if(b->dev == dev && b->blockno == blockno){
  //     b->refcnt++;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  for(b = bucket[id].head.next; b != 0; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket[id].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  uint nowmin = __UINT32_MAX__;
  struct buf *ans = 0;
  for(b = bucket[id].head.next; b != 0; b = b->next){
    if(b->refcnt == 0 && b->time_stamp < nowmin){
      ans = b;
      nowmin = b->time_stamp;
    }
  }
  if(ans){
    ans->dev = dev;
    ans->blockno = blockno;
    ans->valid = 0;
    ans->refcnt = 1;
    release(&bucket[id].lock);
    acquiresleep(&ans->lock);
    return ans;
  }

  acquire(&bcache.lock);
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    if(b->refcnt == 0 && b->time_stamp < nowmin){
      ans = b;
      nowmin = b->time_stamp;
    }
  }
  if(ans){
    int findid = ans->blockno % 13;
    acquire(&bucket[findid].lock);
    struct buf *pre = &(bucket[findid].head);
    struct buf *now = bucket[findid].head.next;
    while(now != ans){
      now = now->next;
      pre = pre->next;
    }
    pre->next = now->next;
    ans->next = bucket[id].head.next;
    bucket[id].head.next = ans;
    release(&bucket[findid].lock);
    release(&bcache.lock);
    ans->dev = dev;
    ans->blockno = blockno;
    ans->valid = 0;
    ans->refcnt = 1;
    release(&bucket[id].lock);
    acquiresleep(&ans->lock);
    return ans;
  }
  else
    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // acquire(&bcache.lock);
  // b->refcnt--;
  // if (b->refcnt == 0) {
  //   // no one is waiting for it.
  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
  
  // release(&bcache.lock);

  int id = b->blockno % NBUCKET;
  acquire(&bucket[id].lock);
  b->refcnt--;
  if(b->refcnt == 0){
    b->time_stamp = ticks;
  }
  release(&bucket[id].lock);
}

void
bpin(struct buf *b) {
  // acquire(&bcache.lock);
  // b->refcnt++;
  // release(&bcache.lock);

  int id = b->blockno % NBUCKET;
  acquire(&bucket[id].lock);
  b->refcnt++;
  release(&bucket[id].lock);
}

void
bunpin(struct buf *b) {
  // acquire(&bcache.lock);
  // b->refcnt--;
  // release(&bcache.lock);

  int id = b->blockno % NBUCKET;
  acquire(&bucket[id].lock);
  b->refcnt--;
  release(&bucket[id].lock);
}


