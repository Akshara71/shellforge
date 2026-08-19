#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/history.h>

#include "builtin.h"

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

    return -1;
}
