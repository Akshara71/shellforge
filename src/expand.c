#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "expand.h"

/* If text starts with '$', replace it in-place with the env variable's value */
static void expand_token(char *text)
{
    if (text[0] != '$' || text[1] == '\0')
    {
        return;
    }

    char var_name[256];
    strncpy(var_name, text + 1, sizeof(var_name) - 1);
    var_name[sizeof(var_name) - 1] = '\0';

    char *value = getenv(var_name);

    if (value)
    {
        strncpy(text, value, MAX_TOKEN_LEN - 1);
        text[MAX_TOKEN_LEN - 1] = '\0';
    }
    else
    {
        text[0] = '\0';
    }
}

void expand_variables(pipeline_t *pipeline)
{
    for (int c = 0; c < pipeline->command_count; c++)
    {
        command_t *cmd = &pipeline->commands[c];

        for (int j = 0; j < cmd->argc; j++)
        {
            expand_token(cmd->argv[j]);
        }

        if (cmd->input[0] != '\0')
        {
            expand_token(cmd->input);
        }

        if (cmd->output[0] != '\0')
        {
            expand_token(cmd->output);
        }
    }
}
