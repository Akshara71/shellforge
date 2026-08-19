#ifndef BUILTIN_H
#define BUILTIN_H

#include "parser.h"

int is_builtin(const command_t *cmd);
int execute_builtin(command_t *cmd);

#endif
