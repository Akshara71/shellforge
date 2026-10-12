/*
 * recovery.h - Failure simulation, rebuild and verification (FileGuard)
 *
 * recovery_command() handles three FileGuard sub-commands:
 *   fileguard fail <disk>      simulate the failure of a disk
 *   fileguard rebuild <disk>   rebuild a failed disk and verify the result
 *   fileguard verify           check that mirror copies / parity agree
 *
 * argc/argv are the same values fileguard_run() received.
 * Returns 0 on success and 1 on failure.
 */
#ifndef RECOVERY_H
#define RECOVERY_H

int recovery_command(int argc, char *argv[]);

#endif /* RECOVERY_H */
