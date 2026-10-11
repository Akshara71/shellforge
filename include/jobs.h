#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>

#define MAX_JOBS 32
#define MAX_JOB_COMMAND 256

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

typedef struct {
    int job_id;
    pid_t pgid;
    job_state_t state;
    char command[MAX_JOB_COMMAND];
    int in_use;
} job_t;

void jobs_init(void);
int job_add(pid_t pgid, const char *command, job_state_t state);
job_t *job_find(int job_id);
job_t *job_find_by_pgid(pid_t pgid);
void job_remove(int job_id);
void jobs_print(void);
void job_stop(pid_t pgid);
void job_continue(pid_t pgid);
void job_done(pid_t pgid);

#endif
