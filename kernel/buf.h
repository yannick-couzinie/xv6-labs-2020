struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  int ticks;   // last used
  int bucket;
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *next;
  struct buf *prev;
  uchar data[BSIZE];
};

