#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/history.h>
#include "builtin.h"

/* ---- cd : change directory ---- */
static void builtin_cd(command_t *cmd)
{
    char *dir;

    if (cmd->argc == 1)
    {
        dir = getenv("HOME");
        if (!dir)
        {
            printf("cd: HOME not set\n");
            return;
        }
    }
    else if (cmd->argc == 2)
    {
        dir = cmd->argv[1];
    }
    else
    {
        printf("cd: too many arguments\n");
        return;
    }

    if (chdir(dir) != 0)
    {
        perror("cd");
    }
}

/* ---- pwd : print working directory ---- */
static void builtin_pwd(command_t *cmd)
{
    if (cmd->argc > 1)
    {
        printf("pwd: too many arguments\n");
        return;
    }

    char buffer[1024];
    if (getcwd(buffer, sizeof(buffer)) == NULL)
    {
        perror("pwd");
        return;
    }

    printf("%s\n", buffer);
}

/* ---- echo : print arguments ---- */
static void builtin_echo(command_t *cmd)
{
    for (int i = 1; i < cmd->argc; i++)
    {
        printf("%s", cmd->argv[i]);
        if (i != cmd->argc - 1)
        {
            printf(" ");
        }
    }
    printf("\n");
}

/* ---- exit : terminate the shell ---- */
static int builtin_exit(command_t *cmd)
{
    if (cmd->argc > 1)
    {
        printf("exit: too many arguments (ignored)\n");
    }
    return 1; /* signal main loop to stop */
}

/* ---- history : bonus builtin, lists past commands ---- */
static void builtin_history(void)
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
}

int is_builtin(const char *cmd_name)
{
    if (!cmd_name)
    {
        return 0;
    }

    return (strcmp(cmd_name, "cd") == 0 ||
            strcmp(cmd_name, "pwd") == 0 ||
            strcmp(cmd_name, "echo") == 0 ||
            strcmp(cmd_name, "exit") == 0 ||
            strcmp(cmd_name, "history") == 0);
}

int execute_builtin(command_t *cmd)
{
    if (cmd->argc == 0)
    {
        return 0;
    }

    if (strcmp(cmd->argv[0], "cd") == 0)
    {
        builtin_cd(cmd);
        return 0;
    }

    if (strcmp(cmd->argv[0], "pwd") == 0)
    {
        builtin_pwd(cmd);
        return 0;
    }

    if (strcmp(cmd->argv[0], "echo") == 0)
    {
        builtin_echo(cmd);
        return 0;
    }

    if (strcmp(cmd->argv[0], "exit") == 0)
    {
        return builtin_exit(cmd);
    }

    if (strcmp(cmd->argv[0], "history") == 0)
    {
        builtin_history();
        return 0;
    }

    return 0;
}
