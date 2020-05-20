#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mpi.h>

#include "scr.h"
// internal SCR function to get remaining job time
extern long int scr_env_seconds_remaining(void);

#define SCR_HALT_SECONDS 360

int counter;       // my "state", just the iteration counter offset by rank
int restartcount;  // how often we have restarted
const char *prefix;

enum failure_t {NO_FAIL, FAIL, FAIL_MEDIAN, FAIL_LATE}; /* should we trigger an intentional failure? */

void checkpoint(int rank, enum failure_t do_fail)
{
  // inform SCR that we are starting a new checkpoint
  SCR_Start_checkpoint();

  MPI_Barrier(MPI_COMM_WORLD);
  // check behaviour if one rank fails to create a checkpoint file
  if(do_fail == FAIL) {
    // this waits for the MPI_Barrier after flcose
    MPI_Barrier(MPI_COMM_WORLD);
    exit(1);
  }

  // build the filename for our checkpoint file
  char buf[SCR_MAX_FILENAME];
  sprintf(buf, "%s/ckpt.%d.txt", prefix, rank);

  // register our checkpoint file with SCR,
  // and ask SCR where to write the file
  char scr_file[SCR_MAX_FILENAME];
  SCR_Route_file(buf, scr_file); 
  printf("checkpoint: %s\n", scr_file);

  // write our checkpoint file
  int valid = 1;
  FILE *fh = fopen(scr_file, "w");
  assert(fh);
  fprintf(fh, "counter %d\n", counter);
  fprintf(fh, "rank %d\n", rank);
  fclose(fh);

  MPI_Barrier(MPI_COMM_WORLD);

  // check behaviour if one rank create a valid checkpoint file but does not
  if(do_fail == FAIL_MEDIAN)
    exit(1);

  // inform SCR whether this process wrote each of its
  // checkpoint files successfully
  SCR_Complete_checkpoint(valid);

  MPI_Barrier(MPI_COMM_WORLD);
  // simulate missing checkpoint file even though I claim to have written it
  if(do_fail == FAIL_LATE) {
    remove(scr_file); /* simulate node failure by removing checkpoint */
    exit(1);
  }
}

void restart(int rank, enum failure_t do_fail)
{
  // inform SCR that we are starting a recovery
  SCR_Start_restart(NULL);

  MPI_Barrier(MPI_COMM_WORLD);
  if(do_fail == FAIL)
    exit(1);

  // build the filename for our checkpoint file
  char buf[SCR_MAX_FILENAME];
  sprintf(buf, "%s/ckpt.%d.txt", prefix, rank);

  // ask SCR where to read the checkpoint from
  char scr_file[SCR_MAX_FILENAME];
  SCR_Route_file(buf, scr_file);
  printf("recover: %s\n", scr_file);

  // read our checkpoint file
  int valid = 1;
  FILE *fh = fopen(scr_file, "r");
  assert(fh);
  fscanf(fh, "counter %d", &counter);
  int cprank;
  fscanf(fh, "rank %d\n", &cprank);
  if(cprank != rank) {
    fprintf(stderr, "Unexpected rank value in checkpoint file: %d != %d\n", cprank, rank);
    exit(1);
  }
  fclose(fh);

  // inform SCR whether this process read each of its
  // checkpoint files successfully
  SCR_Complete_restart(valid);

  MPI_Barrier(MPI_COMM_WORLD);
  if(do_fail == FAIL_LATE)
    exit(1);
}

long current_time_on_rank0(int rank)
{
  long retval;

  if(rank == 0) {
    time_t now = time(NULL);
    retval = (long)now;
  }
  MPI_Bcast(&retval, 1, MPI_LONG, 0, MPI_COMM_WORLD);

  return retval;
}

void read_restart(int rank)
{
  const char *fn = "restartcount.dat";

  // read previous value
  FILE *fh = fopen(fn, "r");
  if(fh != NULL) {
    fscanf(fh, "restartcount %d", &restartcount);
    fclose(fh);
  } else {
    restartcount = 0;
  }

  restartcount += 1;

  MPI_Barrier(MPI_COMM_WORLD);

  // store new value
  if(rank == 0) {
    FILE *fh = fopen(fn, "w");
    if(fh == NULL) {
      fprintf(stderr, "Failed to open '%s': %s", fn, strerror(errno));
      exit(1);
    }
    fprintf(fh, "restartcount %d\n", restartcount);
    fclose(fh);
  }
}

int main(int argc, char **argv)
{
  int rc = 1;

  MPI_Init(&argc, &argv);

  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  long start_time = current_time_on_rank0(rank);

  // disabled buffering for progress output
  setbuf(stdout, NULL);

  // end time from SLRUM scontrol
  long end_time = strtol(getenv("SCRTEST_END_TIME"), NULL, 10);
  long job_start_time = strtol(getenv("SCRTEST_JOB_START_TIME"), NULL, 10);

  if (rank == 0)
    printf("Starting...\n");

  // find out which step in the test scheme we are at. Do not use SCR
  // checkpoints for this so that we can test recovery failures.
  read_restart(rank);
  if(rank == 0)
    printf("Restart count: %d\n", restartcount);

  // this can only be set via ENV vars since the Perl scripts only look at them
  if ((prefix = getenv("SCR_PREFIX")) == NULL)
    prefix = ".";

  // set some SCR options
  const char *cfg1 = SCR_Config("SCR_CHECKPOINT_SECONDS=30");
  assert(cfg1);
  char cfg2buf[100];
  snprintf(cfg2buf, sizeof(cfg2buf), "SCR_HALT_SECONDS=%d", SCR_HALT_SECONDS);
  const char *cfg2 = SCR_Config(cfg2buf);
  assert(cfg2);

  if(SCR_Init() == SCR_SUCCESS) {
    // initialization
    int have_restart;
    SCR_Have_restart(&have_restart, NULL);
    if (have_restart) {
      /* TODO: figure out how to simulate restarting failing */
      restart(rank, NO_FAIL);
    } else {
      counter = 0;
    }

    // main loop
    if (rank == 0)
      printf("Starging from %d\n", counter);
    while(1) {
      if (rank == 0)
        printf("Checking consistency...\n");
      // check consistency of data across ranks
      int global_counter = counter;
      MPI_Bcast(&global_counter, 1, MPI_INT, 0, MPI_COMM_WORLD);
      if(global_counter != counter) {
        fprintf(stderr, "Inconsistent state %d != %d on rank %d\n", global_counter, counter, rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
      }

      long now = current_time_on_rank0(rank);
      if (rank == 0)
        printf("In iteration %d, running %ld seconds\n", counter, now - start_time);
      if (rank == 0) {
          long my_remaining = end_time - now;
          // apaprently this time is only good on rank 0
          int scr_remaining = scr_env_seconds_remaining();
          printf("Time since job start: %d rem: %d %d\n", (int)(time(NULL) - job_start_time), (int)my_remaining, (int)scr_remaining);
      }
      // "science" loop
      sleep(10);
      counter += 1;
      if (rank == 0) {
          long my_remaining = end_time - time(NULL);
          // apaprently this time is only good on rank 0
          int scr_remaining = scr_env_seconds_remaining();
          printf("Time since job start: %d rem: %d %d\n", (int)(time(NULL) - job_start_time), (int)my_remaining, (int)scr_remaining);
      }

      // trigger various issues
      MPI_Barrier(MPI_COMM_WORLD);
      if(counter == 1 && restartcount == 0) {
        if(rank == 0)
          printf("Writing initial checkpoint\n");
        // get some initial checkpoint
        checkpoint(rank, NO_FAIL);
      }
      if(rank == 0 && counter == 2 && restartcount == 0) {
        // trigger a failure so that we recover from checkpoint just written
        if(rank == 0)
          printf("Triggering failure before writing checkpoint\n");
        exit(1);
      }
      if(counter == 3 && restartcount == 1) {
        // trigger a failure while writing a checkpoint
        if(rank == 0)
          printf("Triggering failure after Start_checkpoint while writing checkpoint\n");
        if(rank == 1) {
          checkpoint(rank, FAIL);
        } else {
          checkpoint(rank, NO_FAIL);
        }
      }
      if(counter == 4 && restartcount == 2) {
        // try simulating failure by first writing a checkpoint then wiping it
        if(rank == 0)
          printf("Triggering failure after Complete_checkpoint while writing checkpoint\n");
        if(rank == 1) {
          checkpoint(rank, FAIL_LATE);
        } else {
          checkpoint(rank, NO_FAIL);
        }
      }
      if(counter == 5 && restartcount == 3) {
        // try simulating failure after having written a valid file but not marking it so
        if(rank == 0)
          printf("Triggering failure after fclose while writing checkpoint\n");
        if(rank == 1) {
          checkpoint(rank, FAIL_LATE);
        } else {
          checkpoint(rank, NO_FAIL);
        }
      }
      if(counter == 6 && restartcount == 4) {
        // cause a failure later enough so that SCR should not restart
        if(rank == 0) {
          long my_remaining = end_time - time(NULL);
          // apaprently this time is only good on rank 0
          int scr_remaining = scr_env_seconds_remaining();
          printf("rank %d: my = %d scr = %d, diff = %d\n", rank, (int) my_remaining, (int) scr_remaining, (int)(scr_remaining - my_remaining));

          int wait_time = scr_remaining - SCR_HALT_SECONDS + 30;
          printf("Running simulation out of tiem by waiting %d seconds\n", wait_time);
          sleep(wait_time);
          printf("exiting\n");
          exit(1);
        }
      }
      MPI_Barrier(MPI_COMM_WORLD);

      // ask SCR whether we need to checkpoint
      int flag = 0;
      SCR_Need_checkpoint(&flag);
      if (flag) {
        if (rank == 0)
          printf("Checkpointing at iteration: %d\n", counter);
        // execute the checkpoint code
        checkpoint(rank, NO_FAIL);
        MPI_Barrier(MPI_COMM_WORLD);
      }

      // should we exit?
      int exit_flag = 0;
      SCR_Should_exit(&exit_flag);
      if (exit_flag)
        break;
    }
    printf("Done with iterations\n");

    // termination checkpoint
    checkpoint(rank, NO_FAIL);

    SCR_Finalize();

    // all is good
    rc = 0;
  }
  printf("Exiting...\n");

  MPI_Finalize();

  return rc;
}
