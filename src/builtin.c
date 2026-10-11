#define _POSIX_C_SOURCE 200809L

/*
 * =========================================================
 * Name    : V. AKSHARA
 * ID      : 2500031100
 * File    : builtin.c
 * Project : Shellforge - A Unix Style Shell in C
 * =========================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <readline/history.h>

#include "builtin.h"
#include "jobs.h"
#include "fileguard.h"   /* NEW: FileGuard storage commands */

/* NEW: controls whether main.c prints the TOKENS / PIPELINE tables */
int g_debug_output = 1;

__attribute__((constructor))
static void print_author(void)
{
    printf("Developed by V. Akshara, ID: 2500031100\n");
}

/* ---- cd ---- */
static int builtin_cd(command_t *cmd)
{
    const char *directory;

    if (cmd->argc == 1)
    {
        directory = getenv("HOME");
        if (directory == NULL)
        {
            fprintf(stderr, "cd: HOME not set\n");
            return -1;
        }
    }
    else if (cmd->argc == 2)
    {
        directory = cmd->argv[1];
    }
    else
    {
        fprintf(stderr, "cd: too many arguments\n");
        return -1;
    }

    if (chdir(directory) != 0)
    {
        perror("cd");
        return -1;
    }

    return 0;
}

/* ---- pwd ---- */
static int builtin_pwd(command_t *cmd)
{
    char current_directory[4096];

    if (cmd->argc > 1)
    {
        fprintf(stderr, "pwd: too many arguments\n");
        return -1;
    }

    if (getcwd(current_directory, sizeof(current_directory)) == NULL)
    {
        perror("pwd");
        return -1;
    }

    printf("%s\n", current_directory);
    return 0;
}

/* ---- echo ---- */
static int builtin_echo(command_t *cmd)
{
    for (int i = 1; i < cmd->argc; i++)
    {
        printf("%s", cmd->argv[i]);
        if (i < cmd->argc - 1)
        {
            printf(" ");
        }
    }
    printf("\n");
    return 0;
}

/* ---- exit ---- */
static int builtin_exit(command_t *cmd)
{
    if (cmd->argc > 1)
    {
        fprintf(stderr, "exit: too many arguments\n");
        return -1;
    }

    return 1;
}

/* ---- history (bonus builtin) ---- */
static int builtin_history(void)
{
    HIST_ENTRY **hist_list = history_list();

    printf("------ Command History ------\n");

    if (hist_list)
    {
        for (int i = 0; hist_list[i]; i++)
        {
            printf("%d  %s\n", i + 1, hist_list[i]->line);
        }
    }

    printf("------------------------------\n");
    return 0;
}

/* ---- jobs : list background jobs ---- */
static int builtin_jobs(command_t *cmd)
{
    (void)cmd;
    jobs_print();
    return 0;
}

/* ---- bg : resume a stopped job in the background ---- */
static int builtin_bg(command_t *cmd)
{
    if (cmd->argc < 2)
    {
        fprintf(stderr, "bg: usage: bg <job_id>\n");
        return -1;
    }

    int job_id = atoi(cmd->argv[1]);
    job_t *job = job_find(job_id);

    if (!job)
    {
        fprintf(stderr, "bg: job %d not found\n", job_id);
        return -1;
    }

    kill(-job->pgid, SIGCONT);
    job_continue(job->pgid);
    printf("[%d] %s &\n", job->job_id, job->command);

    return 0;
}

/* ---- fg : bring a job to the foreground and wait for it ---- */
static int builtin_fg(command_t *cmd)
{
    if (cmd->argc < 2)
    {
        fprintf(stderr, "fg: usage: fg <job_id>\n");
        return -1;
    }

    int job_id = atoi(cmd->argv[1]);
    job_t *job = job_find(job_id);

    if (!job)
    {
        fprintf(stderr, "fg: job %d not found\n", job_id);
        return -1;
    }

    printf("%s\n", job->command);

    if (job->state == JOB_STOPPED)
    {
        kill(-job->pgid, SIGCONT);
    }

    job_continue(job->pgid);

    int status;
    tcsetpgrp(STDIN_FILENO, job->pgid);
    waitpid(-job->pgid, &status, WUNTRACED);
    tcsetpgrp(STDIN_FILENO, getpgrp());

    if (WIFSTOPPED(status))
    {
        job_stop(job->pgid);
        printf("\n[%d]  Stopped  %s\n", job->job_id, job->command);
    }
    else
    {
        job_done(job->pgid);
        job_remove(job->job_id);
    }

    return 0;
}

/* ---- NEW: fileguard : FileGuard storage commands ---- */
static int builtin_fileguard(command_t *cmd)
{
    /* Copy the argument pointers into a plain argv array for FileGuard */
    char *args[cmd->argc + 1];
    int result;

    for (int i = 0; i < cmd->argc; i++)
    {
        args[i] = cmd->argv[i];
    }
    args[cmd->argc] = NULL;

    result = fileguard_run(cmd->argc, args);

    /* Flush now: a forked builtin ends with _exit(), which does not flush */
    fflush(stdout);
    fflush(stderr);

    /* Return 0 or -1 only. Never 1, because 1 means "exit the shell". */
    return (result == 0) ? 0 : -1;
}

/* ---- NEW: debug : turn the TOKENS / PIPELINE tables on or off ---- */
static int builtin_debug(command_t *cmd)
{
    if (cmd->argc == 1)
    {
        printf("debug output is %s\n", g_debug_output ? "on" : "off");
        return 0;
    }

    if (cmd->argc == 2 && strcmp(cmd->argv[1], "on") == 0)
    {
        g_debug_output = 1;
        return 0;
    }

    if (cmd->argc == 2 && strcmp(cmd->argv[1], "off") == 0)
    {
        g_debug_output = 0;
        return 0;
    }

    fprintf(stderr, "debug: usage: debug [on|off]\n");
    return -1;
}

/* ---- check if command is a builtin ---- */
int is_builtin(const command_t *cmd)
{
    if (cmd == NULL || cmd->argc == 0)
    {
        return 0;
    }

    if (strcmp(cmd->argv[0], "cd") == 0) return 1;
    if (strcmp(cmd->argv[0], "pwd") == 0) return 1;
    if (strcmp(cmd->argv[0], "echo") == 0) return 1;
    if (strcmp(cmd->argv[0], "exit") == 0) return 1;
    if (strcmp(cmd->argv[0], "history") == 0) return 1;
    if (strcmp(cmd->argv[0], "jobs") == 0) return 1;
    if (strcmp(cmd->argv[0], "fg") == 0) return 1;
    if (strcmp(cmd->argv[0], "bg") == 0) return 1;
    if (strcmp(cmd->argv[0], "fileguard") == 0) return 1;   /* NEW */
    if (strcmp(cmd->argv[0], "debug") == 0) return 1;       /* NEW */

    return 0;
}

/* ---- execute the matching builtin ---- */
int execute_builtin(command_t *cmd)
{
    if (cmd == NULL || cmd->argc == 0)
    {
        return -1;
    }

    if (strcmp(cmd->argv[0], "cd") == 0) return builtin_cd(cmd);
    if (strcmp(cmd->argv[0], "pwd") == 0) return builtin_pwd(cmd);
    if (strcmp(cmd->argv[0], "echo") == 0) return builtin_echo(cmd);
    if (strcmp(cmd->argv[0], "exit") == 0) return builtin_exit(cmd);
    if (strcmp(cmd->argv[0], "history") == 0) return builtin_history();
    if (strcmp(cmd->argv[0], "jobs") == 0) return builtin_jobs(cmd);
    if (strcmp(cmd->argv[0], "fg") == 0) return builtin_fg(cmd);
    if (strcmp(cmd->argv[0], "bg") == 0) return builtin_bg(cmd);
    if (strcmp(cmd->argv[0], "fileguard") == 0) return builtin_fileguard(cmd);   /* NEW */
    if (strcmp(cmd->argv[0], "debug") == 0) return builtin_debug(cmd);           /* NEW */

    return -1;
}
