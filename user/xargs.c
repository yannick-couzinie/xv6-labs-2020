#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int
main(int argc, char *argv[])
{
  int i, eof;
  if (argc < 1){
    fprintf(2, "Usage: xargs COMMAND [...]\n");
    exit(1);
  }

  char *exec_argv[MAXARG]; 
  
  for (i = 1; i < argc; i++)
    exec_argv[i-1] = argv[i];
  
  char buf[256], *stdin_line = buf;
  while((eof = read(0, stdin_line, sizeof(char)))) {

    if (*stdin_line != '\n' && eof != 0){
      // if eof == 0 treat the final line and exit
      stdin_line++;
      continue;
    }
    *stdin_line = 0; // make buf end here for now

    exec_argv[argc-1] = buf;

    if (fork() == 0){
      exec(argv[1], exec_argv);
      exit(1);
      }
      else {
        wait(0);
      }
    stdin_line = buf; // go to beginning of buffer
    if (eof == 0)
      exit(0);
    }
  exit(1); // this point should never be reached
}
