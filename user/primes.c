#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
sieve_layer(int p[2], int base){
  // Sieve layer reads numbers from p[1] checks whether they are divisible by
  // base and if
  //  - if its the first non-divisible number prints it and spawns another
  //  sieve layer
  //  - else passes the number on through the newly generated pipe in the first
  //  step
  int n, has_spawned = 0, p2[2];
  pipe(p2);

  while( read(p[0], &n, 4) != 0){
    if (n % base != 0){
      if (!has_spawned) {
        printf("prime %d\n", n);
        if (fork() == 0){
          // The child goes into its own sieve layer and should not continue the
          // loop upon returning
          close(p2[1]);
          sieve_layer(p2, n);
          return 0;
        }
        
      } else {
        write(p2[1], &n, 4);
      }
    }
  }
  close(p2[1]);
  close(p[0]);
  wait(0);
  exit(0);
}

int
main(int argc, char *argv[])
{
  int i, p[2];
  pipe(p);
  printf("prime 2\n");
  printf("prime 3\n");

  if (fork() == 0){
    close(p[1]);
    sieve_layer(p, 3);
    return 0;
  }
  close(p[0]);

  for (i = 4; i < 35; i++){
    if (i % 2 != 0){
      write(p[1], &i, 4);
    }
  }
  close(p[1]);

  wait(0);
  exit(0);
}
