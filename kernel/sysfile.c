//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  int i, length, prot, flags, fd, offset;
  uint64 addr;
  struct proc *proc;
  struct VMA *free_memory_vma, *VMA;
  struct file *f;

  if(argaddr(0, &addr) < 0 || argint(1, &length) || argint(2, &prot)){
    return -1;
  }

  if(argint(3, &flags) < 0 || argfd(4, &fd, &f) || argint(5, &offset)){
    return -1;
  }

  proc = myproc();

  free_memory_vma = proc->vma_head;

  if (proc->sz + length > MAXVA)
    panic("mmap");

  if((prot & PROT_WRITE) && (f->writable==0) && (flags & MAP_SHARED))
    return -1;

  int found = 0;

  for (i=0; i<16; i++){
    // 16 is max index, so if we are at 17, no free VMA
    if(!proc->VMA[i].valid){
      VMA = &proc->VMA[i];
      found = 1;
      break;
    }
  }
  if (!found)
    panic("mmap: no free VMA");
  found = 0;

  free_memory_vma = proc->vma_head;
  do{
    if(free_memory_vma->valid && !free_memory_vma->mapped && free_memory_vma->length > length){
      found = 1;
      break;
    }
  } while((free_memory_vma = free_memory_vma->next));

  if (!found)
    panic("mmap: not enough free space");

  filedup(f);

  addr = free_memory_vma->start_address;
  free_memory_vma->start_address = PGROUNDUP(free_memory_vma->start_address - PGSIZE - length);
  free_memory_vma->length = free_memory_vma->start_address - free_memory_vma->end_address;
  VMA->f = f;
  VMA->start_address = addr;
  VMA->end_address = PGROUNDDOWN(addr-length);
  VMA->length = length;
  VMA->offset = offset;
  VMA->prot = prot;
  VMA->flags = flags;
  VMA->valid = 1;
  VMA->mapped = 1;

  if(proc->vma_head == free_memory_vma)
    proc->vma_head = VMA;

  VMA->prev = free_memory_vma->prev;

  if(VMA->prev)
    VMA->prev->next = VMA;

  VMA->next = free_memory_vma;
  free_memory_vma->prev = VMA;

  free_memory_vma = proc->vma_head;

  return VMA->end_address;
}

int
map_from_vma(uint64 failing_addr)
{
    int prot, found = 0;
    struct proc *p = myproc();
    struct VMA *VMA = p->vma_head;
    do {
      if (VMA->mapped){
        if (VMA->end_address <= failing_addr && VMA->start_address >= failing_addr){
          found = 1;
          break;
        }
      }
    } while((VMA = VMA->next));
    if(!found)
      return -1;

    if ((VMA->prot & PROT_WRITE) && (VMA->prot & PROT_READ))
      prot = PTE_W|PTE_R|PTE_U;
    else if (VMA->prot & PROT_WRITE)
      prot = PTE_W|PTE_U;
    else if (VMA->prot & PROT_READ)
      prot = PTE_R|PTE_U;
    else
      panic("no prots set");
    
    char *mem;

    if(!(mem = kalloc()))
      panic("cannot kalloc for mapping vma");
    memset(mem, 0, PGSIZE);
    if(mappages(p->pagetable, failing_addr, PGSIZE, (uint64)mem, prot) != 0){
      kfree(mem);
      exit(-1);
    }

    ilock(VMA->f->ip);

    readi(VMA->f->ip, 1, PGROUNDDOWN(failing_addr), PGROUNDDOWN(failing_addr - VMA->end_address), PGSIZE);
    iunlock(VMA->f->ip);
    return 0;
}

uint64
sys_munmap(void)
{
  uint64 addr, len;
  if(argaddr(0, &addr) < 0 || argaddr(1, &len) < 0)
    return -1;

  return munmap(addr, len);
}

uint64
munmap(uint64 addr, uint64 len)
{
  int found = 0;
  struct proc *p = myproc();
  struct VMA *VMA = p->vma_head;

  do{
    // if the mmapped regions are not page aligned these checks probably do not
    // work since we round the start and the end addresses to agree with page
    // boundaries
    if(addr == VMA->end_address || addr+len == VMA->start_address){
      found = 1;
      break;
    }
  } while((VMA=VMA->next));

  if(!found)
    panic("no valid VMA for unmapping");

  if(addr < VMA->end_address || addr+len > VMA->start_address)
    panic("seemingly valid VMA some parameter outside range");

  if(VMA->flags & MAP_SHARED){
      begin_op();
      ilock(VMA->f->ip);
      // this returns something smaller than len if not all could be written,
      // the operation is in that case abandoned but we are fine with that,
      // these are the regions that had not been mapped anyway
      writei(VMA->f->ip, 1, addr, addr-VMA->end_address+VMA->offset, len);
      iunlock(VMA->f->ip);
      end_op();
  }
  uvmunmap(p->pagetable, PGROUNDDOWN(addr), PGROUNDUP(len)/PGSIZE, 1);


  if(addr == VMA->end_address && addr+len == VMA->start_address){
    // make the VMA invalid
    if(p->vma_head == VMA){
      p->vma_head = VMA->next;
    }
    if(VMA->prev)
      VMA->prev->next = VMA->next; 
    
    if(VMA->next)
      VMA->next->prev = VMA->prev; 

    VMA->valid = 0;
    VMA->mapped = 0;
    fileclose(VMA->f);
    return 0;
  }

  // need to insert a new VMA into the newly freed regions

  struct VMA *new_vma = 0;
  for(int i=0; i<16; i++){
    if(!p->VMA[i].valid){
      new_vma = &p->VMA[i];
      break;
    }
  }

  if(!new_vma)
    panic("munmap: not enough vma");

  // more efficient technically if we expand neighbouring free regions if
  // available
  if(addr == VMA->end_address){
    new_vma->end_address = VMA->end_address;
    VMA->end_address = PGROUNDUP(VMA->end_address+len);
    VMA->length = VMA->start_address - VMA->end_address;

    VMA->offset += len; // add to the offset since we lose starting parts of the file
    new_vma->start_address = VMA->end_address-1;
    new_vma->length = new_vma->start_address - VMA->end_address;
    new_vma->valid = 1;
    new_vma->mapped = 0;
    new_vma->prev = VMA;
    new_vma->next = VMA->next;
    VMA->next = new_vma;
    return 0;
  }

  if(addr+len == VMA->start_address){
    if(p->vma_head == VMA){
      p->vma_head = new_vma;
    }
    new_vma->start_address = VMA->start_address;
    VMA->start_address = PGROUNDDOWN(addr)-1;
    VMA->length = VMA->start_address - VMA->end_address;

    new_vma->end_address = PGROUNDDOWN(addr);
    new_vma->length = new_vma->start_address - VMA->end_address;
    new_vma->valid = 1;
    new_vma->mapped = 0;
    new_vma->prev = VMA->prev;
    new_vma->next = VMA;
    VMA->prev = new_vma;
    return 0;
  }

  return -1;
}
