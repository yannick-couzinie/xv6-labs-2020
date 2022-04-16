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

#define NBUCKETS 13

struct buf buf[NBUF];

// this is the lock we hold when we enter a section where the contents of the
// buffers need to be updated (e.g. when evicting)
struct spinlock bcache_update_buf;

struct {
  struct spinlock lock;

  // Linked list of all buffers, through prev/next.
  struct buf *head;
} bcache[NBUCKETS];

void
binit(void)
{
  int i=0;
  struct buf *b;
  initlock(&bcache_update_buf, "bcache_update_buf");

  for(i=0; i<NBUCKETS; i++){
    initlock(&bcache[i].lock, "bcache");
    bcache[i].head = 0;
  }

  // Create linked list of buffers in each bucket
  i=0;
  for(b = buf; b < buf+NBUF; b++){
    if (bcache[i].head == 0){
      bcache[i].head = b;
      b->next = 0;
      b->prev = 0;
    }
    else{
      b->next = bcache[i].head;
      bcache[i].head->prev = b;
      bcache[i].head = b;
      b->prev = 0;
    }
    b->ticks = 0;
    b->bucket = i;
    initsleeplock(&b->lock, "buffer");
    i = (i+1) % NBUCKETS;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  /* printf("enter read\n"); */
  int bucket, i;
  struct buf *b, *lru;


  bucket = blockno % NBUCKETS;
  acquire(&bcache[bucket].lock);
  // Is the block already cached?
  // Skip the check if the bucket is empty of course
  if(bcache[bucket].head){
    b = bcache[bucket].head;
    do{
      /* printf("infiniloop %p\n", b->next); */
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        b->ticks = ticks;
        release(&bcache[bucket].lock);
        acquiresleep(&b->lock);
        /* printf("found cache, valid=%d\n", b->valid); */
        return b;
      }
    } while((b = b->next));
  }
  /* printf("exit read\n"); */

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  //
  // We need to enforce an acquisition order, so the update_buf lock is highest
  // priority, then we take the bucket locks in increasing bucket index.
  release(&bcache[bucket].lock);
  acquire(&bcache_update_buf);
  for(i=0; i<NBUCKETS; i++)
    acquire(&bcache[i].lock);

  // since we released the bucket lock once we cannot be sure anymore that it
  // is not actually cached, so copy the code from above.
  if(bcache[bucket].head){
    b = bcache[bucket].head;
    do{
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        b->ticks = ticks;
        release(&bcache_update_buf);
        for(i=0; i<NBUCKETS; i++)
          release(&bcache[i].lock);
        acquiresleep(&b->lock);
        /* printf("finish read, found after release, valid=%d\n", b->valid); */
        return b;
      }
    } while((b = b->next));
  }

  lru = buf;
  for(b=buf; b<buf+NBUF; b++){
    // never been used
    if(b->ticks == 0){
      lru = b;
      break;
    }
    // if the initial buf has a non-zero refcount take any b
    if (lru->refcnt != 0 && b->refcnt == 0)
      lru = b;
    if (b->ticks < lru->ticks && b->refcnt == 0){
      lru = b;
    }
  }

  // Release all locks apart from up to three locks, the bucket of the lru, the target
  // bucket, and the eviction lock
  for(i=0; i<NBUCKETS; i++){
    if(i==bucket || i==lru->bucket)
      continue;
    release(&bcache[i].lock);
  }

  // if the initial lru has not been overwritten and the initial lru does not
  // have refcnt 0 we have no buffers
  if(lru->refcnt != 0){
    panic("bget: no buffers");
  }

  int old_bucket = lru->bucket;

  lru->dev = dev;
  lru->blockno = blockno;
  lru->valid = 0;
  lru->refcnt = 1;
  lru->ticks = ticks;
  lru->bucket = bucket;
  
  // evict the lru from its old bucket
  if(!lru->next && !lru->prev){
    // lru is the last element in its bucket
    bcache[old_bucket].head = 0;
  }
  else{
    if(lru->next)
      lru->next->prev = lru->prev;
    if(lru->prev)
      lru->prev->next = lru->next;
    if(bcache[old_bucket].head == lru)
      bcache[old_bucket].head = lru->next;
  }
  //eviction end

  //insert into target bucket
  if(!bcache[bucket].head){
      lru->prev = 0;
      lru->next = 0;
  }
  else{
    // the head should always have 0 as previous
    if(bcache[bucket].head->prev != 0){
      panic("wtf");
    }
    lru->prev = 0;
    lru->next = bcache[bucket].head;
    bcache[bucket].head->prev = lru;
  }
  bcache[bucket].head = lru;
  //insertion end
  //
  release(&bcache[bucket].lock);
  if(bucket!=old_bucket){
    release(&bcache[old_bucket].lock);
  }
  release(&bcache_update_buf);
  acquiresleep(&lru->lock);
  return lru;
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

  /* acquire(&bcache[b->dev%NBUCKETS].lock); */
  b->refcnt--;
  b->ticks = ticks;
  /* if (b->refcnt == 0) { */
  /*   // no one is waiting for it. */
  /*   b->next->prev = b->prev; */
  /*   b->prev->next = b->next; */
  /*   b->next = bcache.head.next; */
  /*   b->prev = &bcache.head; */
  /*   bcache.head.next->prev = b; */
  /*   bcache.head.next = b; */
  /* } */
  /* release(&bcache[b->dev%NBUCKETS].lock); */
}

void
bpin(struct buf *b) {
  /* acquire(&bcache[b->bucket].lock); */
  b->refcnt++;
  /* release(&bcache[b->bucket].lock); */
}

void
bunpin(struct buf *b) {
  /* acquire(&bcache[b->bucket].lock); */
  b->refcnt--;
  /* release(&bcache[b->bucket].lock); */
}
