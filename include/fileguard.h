/*
 * fileguard.h - FileGuard command entry point for Shellforge
 *
 * fileguard_run() receives the same argc/argv a normal main() would get:
 *   argv[0] = "fileguard", argv[1] = sub-command, ...
 * It returns 0 on success and 1 on failure.
 */
#ifndef FILEGUARD_H
#define FILEGUARD_H

int fileguard_run(int argc, char *argv[]);

#endif /* FILEGUARD_H */
