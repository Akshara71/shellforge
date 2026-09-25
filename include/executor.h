#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h"

void setup_background_handler(void);
int execute_command(command_t *cmd);
int execute_pipeline(pipeline_t *pipeline);

#endif
