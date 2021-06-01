#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "sysinfo.h"
#include "spinlock.h"
#include "proc.h"

int
sysinfo(struct sysinfo * si)
{
  struct sysinfo output;
  struct proc *p = myproc();

  output.freemem = freemem() * 4096;
  output.nproc = numproc();
  if(copyout(p->pagetable, (uint64) si, (char *)&output, sizeof(output)) < 0)
    return -1;
  return 0;
}
