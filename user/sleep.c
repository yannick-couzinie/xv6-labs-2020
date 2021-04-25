#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc == 1 || argc > 2){
    fprintf(2, "Usage: 'sleep n' where n is number of ticks to wait for.\n");
    exit(1);
  }

  sleep(atoi(argv[1]));

  exit(0);
}
