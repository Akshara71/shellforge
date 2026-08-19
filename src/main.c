#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/history.h>
#include <readline/readline.h>
#include "token.h"
#include "lexer.h"
#include "parser.h"
#include "expand.h"
#include "builtin.h"
#include "executor.h"

int main(void)
{
    printf("=====================================\n");
    printf("     Shellforge\n");
    printf(" A Unix Style Shell written in C\n");
    printf("=====================================\n");

    char *line;
    token_list_t tokens;
    pipeline_t pipeline;
    int should_exit = 0;

    while (1)
    {
        line = readline("shellforge$ ");

        if (line == NULL)
        {
            printf("\nGoodbye!\n");
            break;
        }

        if (strlen(line) == 0)
        {
            free(line);
            continue;
        }

        add_history(line);

        lexer(line, &tokens);
        token_print(&tokens);

        if (parse(&tokens, &pipeline))
        {
            expand_variables(&pipeline);
            pipeline_print(&pipeline);

            for (int i = 0; i < pipeline.command_count; i++)
            {
                command_t *cmd = &pipeline.commands[i];

                if (cmd->argc == 0)
                {
                    continue;
                }

                if (execute_command(cmd) == 1)
                {
                    should_exit = 1;
                }
            }
        }

        free(line);

        if (should_exit)
        {
            break;
        }
    }

    return 0;
}
