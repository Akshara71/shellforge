#ifndef PARSER_H
#define PARSER_H

#include "token.h"

#define MAX_ARGS 64
#define MAX_COMMANDS 16

typedef struct {
    int argc;
    char *argv[MAX_ARGS];
    char input[MAX_TOKEN_LEN];
    char output[MAX_TOKEN_LEN];
    int append;
    int background;
} command_t;

typedef struct {
    command_t commands[MAX_COMMANDS];
    int command_count;
} pipeline_t;

void command_init(command_t *cmd);
int parse(token_list_t *tokens, pipeline_t *pipeline);
void pipeline_print(const pipeline_t *pipeline);

#endif
