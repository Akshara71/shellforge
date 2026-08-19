#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "executor.h"
#include "builtin.h"

int execute_command(command_t *cmd)
{
    pid_t pid;
    int status;

    if (cmd == NULL || cmd->argc == 0)
    {
        return -1;
    }

    /* Built-ins run inside the shell itself, no new process */
    if (is_builtin(cmd))
    {
        return execute_builtin(cmd);
    }

    /* External command -> create a child process */
    pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return -1;
    }

    if (pid == 0)
    {
        /* CHILD PROCESS */
        char *args[MAX_ARGS + 1];

        for (int i = 0; i < cmd->argc; i++)
        {
            args[i] = cmd->argv[i];
        }
        args[cmd->argc] = NULL;

        execvp(args[0], args);

        /* Only reached if execvp() failed */
        perror("Shellforge");
        exit(EXIT_FAILURE);
    }

    /* PARENT PROCESS */
    if (waitpid(pid, &status, 0) == -1)
    {
        perror("waitpid");
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
