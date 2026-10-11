#ifndef BUILTIN_H
#define BUILTIN_H

#include "parser.h"

/* 1 = print the TOKENS / PIPELINE tables, 0 = hide them (see "debug" builtin) */
extern int g_debug_output;

int is_builtin(const command_t *cmd);
int execute_builtin(command_t *cmd);

#endif
