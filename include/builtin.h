#ifndef BUILTIN_H
#define BUILTIN_H

#include "parser.h"

int is_builtin(const char *cmd_name);
int execute_builtin(command_t *cmd);

#endif
