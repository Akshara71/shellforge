#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/history.h>
#include <readline/readline.h>
#include "token.h"
#include "lexer.h"
#include "parser.h"
#include "expand.h"

static void print_history(void)
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

int main(void)
{
    printf("=====================================\n");
    printf("     Shellforge\n");
    printf(" A Unix Style Shell written in C\n");
    printf("=====================================\n");

    char *line;
    token_list_t tokens;
    pipeline_t pipeline;

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

        if (strcmp(line, "exit") == 0)
        {
            free(line);
            printf("Exiting...\n");
            break;
        }

        if (strcmp(line, "history") == 0)
        {
            print_history();
            free(line);
            continue;
        }

        lexer(line, &tokens);
        token_print(&tokens);

        if (parse(&tokens, &pipeline))
        {
            expand_variables(&pipeline);
            pipeline_print(&pipeline);
        }

        free(line);
    }

    return 0;
}
