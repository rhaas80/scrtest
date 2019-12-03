#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include <mpi.h>

#include "scr.h"

#define ITER_INC 3 // how many iterations before we terminate voluntarily
int counter;       // my "state", just the iteration counter offset by rank

void checkpoint(int rank)
{
  // inform SCR that we are starting a new checkpoint
  SCR_Start_checkpoint();

  // build the filename for our checkpoint file
  char buf[100];
  sprintf(buf, "ckpt.%d.txt", rank);

  // register our checkpoint file with SCR,
  // and ask SCR where to write the file
  char scr_file[SCR_MAX_FILENAME];
  SCR_Route_file(buf, scr_file); 
  printf("checkpoint: %s\n", scr_file);

  // write our checkpoint file
  int valid = 1;
  FILE *fh = fopen(scr_file, "w");
  assert(fh);
  fprintf(fh, "%d\n", counter);
  fclose(fh);

  // inform SCR whether this process wrote each of its
  // checkpoint files successfully
  SCR_Complete_checkpoint(valid);
}

void restart(int rank)
{
  // inform SCR that we are starting a recovery
  SCR_Start_restart(NULL);

  // build the filename for our checkpoint file
  char buf[100];
  sprintf(buf, "ckpt.%d.txt", rank);

  // ask SCR where to read the checkpoint from
  char scr_file[SCR_MAX_FILENAME];
  SCR_Route_file(buf, scr_file);
  printf("recover: %s\n", scr_file);

  // read our checkpoint file
  int valid = 1;
  FILE *fh = fopen(scr_file, "r");
  assert(fh);
  fscanf(fh, "%d", &counter);
  fclose(fh);

  // inform SCR whether this process read each of its
  // checkpoint files successfully
  SCR_Complete_restart(valid);
}

int main(int argc, char **argv)
{
  int rc = 1;

  MPI_Init(&argc, &argv);

  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  if(SCR_Init() == SCR_SUCCESS) {
    // initialization
    int have_restart;
    SCR_Have_restart(&have_restart, NULL);
    if (have_restart)
      restart(rank);
    else
      counter = rank;

    // main loop
    int initial_counter = counter;
    while(counter - initial_counter < ITER_INC) {
      // "science" loop
      sleep(1);
      counter += 1;

      // ask SCR whether we need to checkpoint
      int flag = 0;
      SCR_Need_checkpoint(&flag);
      if (flag) {
        // execute the checkpoint code
        checkpoint(rank);
      }
    }

    // termination checkpoint
    checkpoint(rank);

    SCR_Finalize();

    // all is good
    rc = 0;
  }

  MPI_Finalize();

  return rc;
}
