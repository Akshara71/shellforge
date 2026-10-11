#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "executor.h"
#include "builtin.h"
#include "jobs.h"

static pid_t shell_pgid;

/* ---- SIGCHLD handler: reaps finished background children so they
   never become zombies, and marks their job as Done ---- */
static void sigchld_handler(int sig)
{
    int saved_errno = errno;
    (void)sig;
    pid_t pid;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    {
        job_done(pid);
    }

    errno = saved_errno;
}

void setup_background_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/* Take control of the terminal for the shell itself, and make sure
   the shell can never be stopped/hijacked by Ctrl+Z or terminal
   driver signals while it's just sitting at the prompt. */
void setup_job_control(void)
{
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

/* Apply this command's < input / > output / >> append redirection
   to the CURRENT process. Must be called inside a child, before execvp(). */
static void apply_redirection(command_t *cmd)
{
    if (cmd->input[0] != '\0')
    {
        int fd = open(cmd->input, O_RDONLY);
        if (fd < 0)
        {
            perror(cmd->input);
            _exit(127);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (cmd->output[0] != '\0')
    {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->output, flags, 0644);
        if (fd < 0)
        {
            perror(cmd->output);
            _exit(127);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

/* Build a "cmd arg1 arg2 &" style string for the job table / jobs display */
static void build_command_string(command_t *cmd, char *out, size_t out_size)
{
    out[0] = '\0';

    for (int i = 0; i < cmd->argc; i++)
    {
        strncat(out, cmd->argv[i], out_size - strlen(out) - 1);
        if (i < cmd->argc - 1)
        {
            strncat(out, " ", out_size - strlen(out) - 1);
        }
    }

    strncat(out, " &", out_size - strlen(out) - 1);
}

/* Strip a trailing " &" from a command string (used when a FOREGROUND
   job gets stopped with Ctrl+Z - it wasn't started with &, so it
   shouldn't be displayed with one). */
static void strip_trailing_amp(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[len - 2] == ' ' && s[len - 1] == '&')
    {
        s[len - 2] = '\0';
    }
}

/* Execute a single command (no pipeline). Builtins with no redirection
   run directly in the shell's own process; everything else forks. */
int execute_command(command_t *cmd)
{
    pid_t pid;
    int status;

    if (cmd == NULL || cmd->argc == 0)
    {
        return -1;
    }

    if (is_builtin(cmd) && !cmd->background)
    {
        /* No redirection -> run directly (needed so cd/exit affect the real shell) */
        if (cmd->input[0] == '\0' && cmd->output[0] == '\0')
        {
            return execute_builtin(cmd);
        }

        /* Has redirection -> fork so the builtin's output can go to the file */
        pid = fork();

        if (pid < 0)
        {
            perror("fork");
            return -1;
        }

        if (pid == 0)
        {
            apply_redirection(cmd);
            int result = execute_builtin(cmd);
            _exit(result < 0 ? 1 : result);
        }

        if (waitpid(pid, &status, 0) == -1)
        {
            perror("waitpid");
            return -1;
        }

        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return -1;
    }

    if (pid == 0)
    {
        /* Every external command runs in its own process group,
           so job control (fg/bg) can target it independently */
        setpgid(0, 0);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        /* Background jobs shouldn't read from the terminal */
        if (cmd->background)
        {
            int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0)
            {
                dup2(null_fd, STDIN_FILENO);
                close(null_fd);
            }
        }

        apply_redirection(cmd);

        if (is_builtin(cmd))
        {
            int result = execute_builtin(cmd);
            _exit(result < 0 ? 1 : result);
        }

        char *args[MAX_ARGS + 1];
        for (int i = 0; i < cmd->argc; i++)
        {
            args[i] = cmd->argv[i];
        }
        args[cmd->argc] = NULL;

        execvp(args[0], args);

        perror("Shellforge");
        _exit(127);
    }

    /* ---- PARENT ---- */
    if (cmd->background)
    {
        setpgid(pid, pid);

        char command_string[MAX_JOB_COMMAND];
        build_command_string(cmd, command_string, sizeof(command_string));

        int job_id = job_add(pid, command_string, JOB_RUNNING);
        if (job_id > 0)
        {
            printf("[%d] %d\n", job_id, pid);
        }

        return 0; /* Do not wait -> shell returns to prompt immediately */
    }

    /* Foreground: hand the terminal over to this job while it runs */
    setpgid(pid, pid);
    tcsetpgrp(STDIN_FILENO, pid);

    if (waitpid(pid, &status, WUNTRACED) == -1)
    {
        perror("waitpid");
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        return -1;
    }

    tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (WIFSTOPPED(status))
    {
        char command_string[MAX_JOB_COMMAND];
        build_command_string(cmd, command_string, sizeof(command_string));
        strip_trailing_amp(command_string);

        int job_id = job_add(pid, command_string, JOB_STOPPED);
        if (job_id > 0)
        {
            printf("\n[%d]  Stopped  %s\n", job_id, command_string);
        }
        return -1;
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status))
    {
        fprintf(stderr, "Process terminated by signal %d\n", WTERMSIG(status));
        return -1;
    }

    return 0;
}

/* Execute a full pipeline of one or more commands, connecting them
   with real pipe()s. All processes in the pipeline share the same
   process group (led by the first command) so fg/bg can control the
   whole pipeline as one job. If the LAST command has the & flag, the
   whole pipeline runs in the background and the shell does not wait. */
int execute_pipeline(pipeline_t *pipeline)
{
    if (pipeline == NULL || pipeline->command_count == 0)
    {
        return -1;
    }

    int command_count = pipeline->command_count;
    int background = pipeline->commands[command_count - 1].background;

    /* Single command -> no piping needed, just run it directly */
    if (command_count == 1)
    {
        return execute_command(&pipeline->commands[0]);
    }

    pid_t pids[MAX_COMMANDS];
    int previous_read = -1;

    for (int i = 0; i < command_count; i++)
    {
        command_t *cmd = &pipeline->commands[i];
        int pipefd[2];
        int has_next_pipe = (i < command_count - 1);

        if (has_next_pipe)
        {
            if (pipe(pipefd) == -1)
            {
                perror("pipe");
                return -1;
            }
        }

        pid_t pid = fork();

        if (pid < 0)
        {
            perror("fork");
            return -1;
        }

        if (pid == 0)
        {
            /* ---- CHILD ---- */
            if (i == 0)
            {
                setpgid(0, 0);
            }
            else
            {
                setpgid(0, pids[0]);
            }
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (previous_read != -1)
            {
                dup2(previous_read, STDIN_FILENO);
            }

            if (has_next_pipe)
            {
                dup2(pipefd[1], STDOUT_FILENO);
            }

            /* Only the first stage of a background pipeline needs its
               stdin cut off from the terminal */
            if (background && i == 0 && previous_read == -1)
            {
                int null_fd = open("/dev/null", O_RDONLY);
                if (null_fd >= 0)
                {
                    dup2(null_fd, STDIN_FILENO);
                    close(null_fd);
                }
            }

            /* Explicit < / > / >> overrides the pipe wiring if set */
            apply_redirection(cmd);

            if (previous_read != -1)
            {
                close(previous_read);
            }

            if (has_next_pipe)
            {
                close(pipefd[0]);
                close(pipefd[1]);
            }

            if (is_builtin(cmd))
            {
                int result = execute_builtin(cmd);
                _exit(result < 0 ? 1 : result);
            }

            char *args[MAX_ARGS + 1];
            for (int j = 0; j < cmd->argc; j++)
            {
                args[j] = cmd->argv[j];
            }
            args[cmd->argc] = NULL;

            execvp(args[0], args);

            perror("Shellforge");
            _exit(127);
        }

        /* ---- PARENT ---- */
        pids[i] = pid;

        if (i == 0)
        {
            setpgid(pids[i], pids[i]);
        }
        else
        {
            setpgid(pids[i], pids[0]);
        }

        if (previous_read != -1)
        {
            close(previous_read);
        }

        if (has_next_pipe)
        {
            close(pipefd[1]);
            previous_read = pipefd[0];
        }
        else
        {
            previous_read = -1;
        }
    }

    if (background)
    {
        char command_string[MAX_JOB_COMMAND];
        build_command_string(&pipeline->commands[0], command_string, sizeof(command_string));

        int job_id = job_add(pids[0], command_string, JOB_RUNNING);
        if (job_id > 0)
        {
            printf("[%d] %d\n", job_id, pids[0]);
        }

        return 0; /* Do not wait -> SIGCHLD handler reaps them later */
    }

    /* Foreground: hand the terminal over to the pipeline's process group */
    tcsetpgrp(STDIN_FILENO, pids[0]);

    int status;
    int last_status = 0;
    int stopped = 0;

    for (int i = 0; i < command_count; i++)
    {
        waitpid(pids[i], &status, WUNTRACED);

        if (WIFSTOPPED(status))
        {
            stopped = 1;
        }

        if (i == command_count - 1)
        {
            last_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }

    tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (stopped)
    {
        char command_string[MAX_JOB_COMMAND];
        build_command_string(&pipeline->commands[0], command_string, sizeof(command_string));
        strip_trailing_amp(command_string);

        int job_id = job_add(pids[0], command_string, JOB_STOPPED);
        if (job_id > 0)
        {
            printf("\n[%d]  Stopped  %s\n", job_id, command_string);
        }
        return -1;
    }

    return last_status;
}
