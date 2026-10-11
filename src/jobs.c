#include <stdio.h>
#include <string.h>
#include "jobs.h"

static job_t job_table[MAX_JOBS];
static int next_job_id = 1;

void jobs_init(void)
{
    memset(job_table, 0, sizeof(job_table));
    next_job_id = 1;
}

int job_add(pid_t pgid, const char *command, job_state_t state)
{
    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (!job_table[i].in_use)
        {
            job_table[i].in_use = 1;
            job_table[i].job_id = next_job_id++;
            job_table[i].pgid = pgid;
            job_table[i].state = state;
            strncpy(job_table[i].command, command, MAX_JOB_COMMAND - 1);
            job_table[i].command[MAX_JOB_COMMAND - 1] = '\0';
            return job_table[i].job_id;
        }
    }
    return -1;
}

job_t *job_find(int job_id)
{
    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (job_table[i].in_use && job_table[i].job_id == job_id)
        {
            return &job_table[i];
        }
    }
    return NULL;
}

job_t *job_find_by_pgid(pid_t pgid)
{
    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (job_table[i].in_use && job_table[i].pgid == pgid)
        {
            return &job_table[i];
        }
    }
    return NULL;
}

void job_remove(int job_id)
{
    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (job_table[i].in_use && job_table[i].job_id == job_id)
        {
            job_table[i].in_use = 0;
            return;
        }
    }
}

static const char *job_state_name(job_state_t state)
{
    switch (state)
    {
        case JOB_RUNNING: return "Running";
        case JOB_STOPPED: return "Stopped";
        case JOB_DONE:    return "Done";
        default:           return "Unknown";
    }
}

void jobs_print(void)
{
    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (job_table[i].in_use)
        {
            printf("[%d]  %-8s %s\n",
                   job_table[i].job_id,
                   job_state_name(job_table[i].state),
                   job_table[i].command);

            if (job_table[i].state == JOB_DONE)
            {
                job_table[i].in_use = 0;
            }
        }
    }
}

void job_stop(pid_t pgid)
{
    job_t *job = job_find_by_pgid(pgid);
    if (job)
    {
        job->state = JOB_STOPPED;
    }
}

void job_continue(pid_t pgid)
{
    job_t *job = job_find_by_pgid(pgid);
    if (job)
    {
        job->state = JOB_RUNNING;
    }
}

void job_done(pid_t pgid)
{
    job_t *job = job_find_by_pgid(pgid);
    if (job)
    {
        job->state = JOB_DONE;
    }
}
