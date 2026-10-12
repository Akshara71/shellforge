/*
 * recovery.c - Failure simulation, rebuild and verification (FileGuard)
 *
 *   fileguard fail <disk>      simulate the failure of a disk
 *   fileguard rebuild <disk>   rebuild a failed disk and verify the result
 *   fileguard verify           check that mirror copies / parity agree
 *
 * Rebuild rules (so a half-finished rebuild can never pass for a good disk):
 *   1. the new disk file is created WITHOUT a valid header (it shows as CORRUPT)
 *   2. every block is computed from the surviving disks and written
 *   3. the new disk is read back and every block is compared again with what
 *      the surviving disks require
 *   4. only then is the header written, which puts the disk back in service
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "recovery.h"
#include "disk.h"
#include "volume.h"

#define VERIFY_LISTED 8      /* how many bad blocks/stripes verify lists */

/* Return codes of the internal rebuild / verify functions */
#define REC_OK           0
#define REC_ERR_STATE   -1   /* not possible in the current state    */
#define REC_ERR_IO      -2   /* a disk operation failed              */
#define REC_ERR_MISMATCH -3  /* data does not match                  */

typedef struct {
    int level;                  /* RAID level of the volume              */
    uint32_t blocks_written;    /* blocks written to the new disk        */
    uint32_t blocks_verified;   /* blocks that matched when read back    */
    uint32_t bad_block;         /* first block that did not match        */
    const char *reason;         /* why a rebuild was refused or failed   */
} RebuildReport;

typedef struct {
    int level;                  /* RAID level of the volume              */
    uint32_t units;             /* blocks (RAID-1) or stripes (RAID-5)   */
    uint32_t bad;               /* how many did not match                */
    uint32_t first_bad[VERIFY_LISTED];
    int listed;                 /* entries used in first_bad             */
    int failed_disk;            /* 0-based unavailable disk, or -1       */
    const char *reason;
} VerifyReport;

static const char *level_label(int level)
{
    if (level == RAID_LEVEL_1)
        return "RAID-1 (mirror)";
    if (level == RAID_LEVEL_5)
        return "RAID-5 (rotating parity)";
    return "RAID-0 (striping)";
}

/* Convert "1".."4" to a 0-based disk number. Returns 0 on success. */
static int parse_disk(const char *text, int *disk_id)
{
    char *end;
    unsigned long value;

    if (text[0] == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (*end != '\0' || errno != 0 || value < 1 || value > NUM_DISKS)
        return -1;
    *disk_id = (int)value - 1;
    return 0;
}

static void print_disk_error(void)
{
    fprintf(stderr, "Error: disk must be a number from 1 to %d\n", NUM_DISKS);
}

static void xor_into(unsigned char *dst, const unsigned char *src)
{
    int i;

    for (i = 0; i < BLOCK_SIZE; i++)
        dst[i] ^= src[i];
}

/* What block 'row' of disk 'target' must contain, computed ONLY from the
 * surviving disks:
 *   RAID-1: a copy of the other mirror disk
 *   RAID-5: the XOR of the other three disks (this works for data blocks
 *           and for parity blocks alike) */
static int expected_block(int level, int target, uint32_t row,
                          unsigned char *out)
{
    unsigned char tmp[BLOCK_SIZE];
    int d;

    if (level == RAID_LEVEL_1) {
        d = (target == 0) ? 1 : 0;
        return (disk_read_block(d, row, out) == DISK_OK) ? REC_OK : REC_ERR_IO;
    }

    memset(out, 0, BLOCK_SIZE);
    for (d = 0; d < NUM_DISKS; d++) {
        if (d == target)
            continue;
        if (disk_read_block(d, row, tmp) != DISK_OK)
            return REC_ERR_IO;
        xor_into(out, tmp);
    }
    return REC_OK;
}

/* ---- rebuild -------------------------------------------------------- */

static int do_rebuild(int target, RebuildReport *rep)
{
    unsigned char want[BLOCK_SIZE];
    unsigned char have[BLOCK_SIZE];
    DiskHeader hdr;
    unsigned failed = 0;
    uint32_t row;
    int level, nfailed;

    memset(rep, 0, sizeof(*rep));
    level = volume_level();
    rep->level = level;

    if (level == VOLUME_LEVEL_NONE) {
        rep->reason = "the volume is not initialised";
        return REC_ERR_STATE;
    }
    if (level == RAID_LEVEL_0) {
        rep->reason = "RAID-0 has no redundancy, so there is nothing to rebuild from";
        return REC_ERR_STATE;
    }

    nfailed = volume_failed_disks(&failed);
    if (!(failed & (1u << target))) {
        rep->reason = "that disk is not failed";
        return REC_ERR_STATE;
    }

    if (level == RAID_LEVEL_1) {
        if (target >= MIRROR_COPIES) {
            /* An unused disk holds no data: just make it a valid disk again */
            if (disk_create(target, (uint32_t)level) != DISK_OK) {
                rep->reason = "could not create the new disk file";
                return REC_ERR_IO;
            }
            return REC_OK;
        }
        if (failed & (1u << (target == 0 ? 1 : 0))) {
            rep->reason = "the other mirror disk is also unavailable";
            return REC_ERR_STATE;
        }
    } else if (nfailed > 1) {
        rep->reason = "another disk is also unavailable "
                      "(RAID-5 can only rebuild one failed disk)";
        return REC_ERR_STATE;
    }

    /* Step 1: a blank file without a valid header (shows as CORRUPT) */
    if (disk_create_blank(target) != DISK_OK) {
        rep->reason = "could not create the new disk file";
        return REC_ERR_IO;
    }

    /* Step 2: compute every block from the survivors and write it */
    for (row = 0; row < DISK_DATA_BLOCKS; row++) {
        if (expected_block(level, target, row, want) != REC_OK) {
            rep->reason = "could not read the surviving disks";
            return REC_ERR_IO;
        }
        if (disk_write_block(target, row, want) != DISK_OK) {
            rep->reason = "could not write to the new disk";
            return REC_ERR_IO;
        }
        rep->blocks_written++;
    }

    /* Step 3: read the new disk back and compare every block again with
     * what the survivors require (nothing is trusted from step 2) */
    for (row = 0; row < DISK_DATA_BLOCKS; row++) {
        if (expected_block(level, target, row, want) != REC_OK) {
            rep->reason = "could not read the surviving disks";
            return REC_ERR_IO;
        }
        if (disk_read_block(target, row, have) != DISK_OK) {
            rep->reason = "could not read back the new disk";
            return REC_ERR_IO;
        }
        if (memcmp(want, have, BLOCK_SIZE) != 0) {
            rep->bad_block = row;
            rep->reason = "a block on the new disk does not match";
            return REC_ERR_MISMATCH;
        }
        rep->blocks_verified++;
    }

    /* Step 4: only now does the disk become a valid member of the volume */
    if (disk_write_header(target, (uint32_t)level) != DISK_OK ||
        disk_read_header(target, &hdr) != DISK_OK ||
        hdr.raid_level != (uint32_t)level) {
        rep->reason = "could not write the disk header";
        return REC_ERR_IO;
    }
    return REC_OK;
}

/* ---- verify --------------------------------------------------------- */

static int do_verify(VerifyReport *rep)
{
    unsigned char a[BLOCK_SIZE];
    unsigned char b[BLOCK_SIZE];
    unsigned failed = 0;
    uint32_t row;
    int level, d, i, mismatch;

    memset(rep, 0, sizeof(*rep));
    rep->failed_disk = -1;
    level = volume_level();
    rep->level = level;

    if (level == VOLUME_LEVEL_NONE) {
        rep->reason = "the volume is not initialised";
        return REC_ERR_STATE;
    }
    if (level == RAID_LEVEL_0) {
        rep->reason = "RAID-0 keeps no copies or parity";
        return REC_ERR_STATE;
    }

    volume_failed_disks(&failed);
    if (level == RAID_LEVEL_1)
        failed &= (1u << MIRROR_COPIES) - 1;     /* only the mirror disks matter */
    if (failed != 0) {
        for (d = 0; d < NUM_DISKS; d++) {
            if (failed & (1u << d)) {
                rep->failed_disk = d;
                break;
            }
        }
        rep->reason = "a disk is unavailable";
        return REC_ERR_STATE;
    }

    for (row = 0; row < DISK_DATA_BLOCKS; row++) {
        mismatch = 0;
        if (level == RAID_LEVEL_1) {
            if (disk_read_block(0, row, a) != DISK_OK ||
                disk_read_block(1, row, b) != DISK_OK) {
                rep->reason = "could not read a disk";
                return REC_ERR_IO;
            }
            mismatch = (memcmp(a, b, BLOCK_SIZE) != 0);
        } else {
            /* the 3 data blocks and the parity block of a stripe must XOR to 0 */
            memset(a, 0, sizeof(a));
            for (d = 0; d < NUM_DISKS; d++) {
                if (disk_read_block(d, row, b) != DISK_OK) {
                    rep->reason = "could not read a disk";
                    return REC_ERR_IO;
                }
                xor_into(a, b);
            }
            for (i = 0; i < BLOCK_SIZE; i++) {
                if (a[i] != 0) {
                    mismatch = 1;
                    break;
                }
            }
        }
        rep->units++;
        if (mismatch) {
            if (rep->listed < VERIFY_LISTED)
                rep->first_bad[rep->listed++] = row;
            rep->bad++;
        }
    }
    return (rep->bad > 0) ? REC_ERR_MISMATCH : REC_OK;
}

/* Print the result of do_verify(). Returns 0 if the volume is consistent. */
static int print_verify(int rc, const VerifyReport *rep)
{
    const char *unit = (rep->level == RAID_LEVEL_1) ? "blocks" : "stripes";
    int i;

    if (rc == REC_ERR_STATE) {
        if (rep->failed_disk >= 0)
            fprintf(stderr, "Error: cannot verify: Disk %d is unavailable "
                            "(run 'fileguard rebuild %d')\n",
                    rep->failed_disk + 1, rep->failed_disk + 1);
        else
            fprintf(stderr, "Error: cannot verify: %s\n", rep->reason);
        return 1;
    }
    if (rc == REC_ERR_IO) {
        fprintf(stderr, "Error: verify stopped: %s\n", rep->reason);
        return 1;
    }

    if (rep->level == RAID_LEVEL_1)
        printf("Verify (RAID-1 mirror): compared %u blocks of Disk 1 and "
               "Disk 2: %u identical, %u different\n",
               rep->units, rep->units - rep->bad, rep->bad);
    else
        printf("Verify (RAID-5 parity): checked %u stripes: %u with "
               "correct parity, %u wrong\n",
               rep->units, rep->units - rep->bad, rep->bad);

    if (rc == REC_OK) {
        printf("Result: CONSISTENT\n");
        return 0;
    }

    printf("Result: INCONSISTENT - first differing %s:", unit);
    for (i = 0; i < rep->listed; i++)
        printf("%s %u", (i == 0) ? "" : ",", rep->first_bad[i]);
    if (rep->bad > (uint32_t)rep->listed)
        printf(" ...");
    printf("\n");
    printf("(FileGuard only detects this problem; it does not repair it.)\n");
    return 1;
}

/* ---- the three commands --------------------------------------------- */

static int cmd_fail(int argc, char *argv[])
{
    unsigned failed = 0;
    int disk, level, nfailed;

    if (argc != 3) {
        fprintf(stderr, "Usage: fileguard fail <disk>\n");
        return 1;
    }
    if (parse_disk(argv[2], &disk) != 0) {
        print_disk_error();
        return 1;
    }
    level = volume_level();
    if (level == VOLUME_LEVEL_NONE) {
        fprintf(stderr, "Error: the volume is not initialised "
                        "(run 'fileguard init' first)\n");
        return 1;
    }
    volume_failed_disks(&failed);
    if (failed & (1u << disk)) {
        fprintf(stderr, "Error: Disk %d is already failed\n", disk + 1);
        return 1;
    }
    if (disk_remove(disk) != DISK_OK) {
        fprintf(stderr, "Error: could not remove the image file of Disk %d\n",
                disk + 1);
        return 1;
    }

    printf("Simulating failure of Disk %d: its image file was removed.\n",
           disk + 1);
    nfailed = volume_failed_disks(&failed);

    if (level == RAID_LEVEL_0) {
        printf("Warning: RAID-0 has no redundancy, so the data stored on "
               "Disk %d is lost.\n", disk + 1);
    } else if (level == RAID_LEVEL_1) {
        if (disk >= MIRROR_COPIES)
            printf("Disk %d is not used by RAID-1, so the mirror is not "
                   "affected.\n", disk + 1);
        else if ((failed & ((1u << MIRROR_COPIES) - 1)) ==
                 ((1u << MIRROR_COPIES) - 1))
            printf("Warning: both mirror disks have failed, so the data is "
                   "unavailable.\n");
        else
            printf("The mirror is now DEGRADED: one copy of every block "
                   "is left.\n");
    } else {
        if (nfailed == 1)
            printf("The array is now DEGRADED: blocks on Disk %d are rebuilt "
                   "from parity when they are read.\n", disk + 1);
        else
            printf("Warning: %d disks have failed. RAID-5 can only recover "
                   "from one failed disk.\n", nfailed);
    }
    printf("Run 'fileguard status' to see the state, or "
           "'fileguard rebuild %d' to replace the disk.\n", disk + 1);
    return 0;
}

static int cmd_rebuild(int argc, char *argv[])
{
    RebuildReport rep;
    VerifyReport vrep;
    int disk, rc, vrc;

    if (argc != 3) {
        fprintf(stderr, "Usage: fileguard rebuild <disk>\n");
        return 1;
    }
    if (parse_disk(argv[2], &disk) != 0) {
        print_disk_error();
        return 1;
    }

    rc = do_rebuild(disk, &rep);
    if (rc == REC_ERR_STATE) {
        fprintf(stderr, "Error: cannot rebuild Disk %d: %s\n",
                disk + 1, rep.reason);
        return 1;
    }
    if (rc == REC_ERR_MISMATCH) {
        fprintf(stderr, "Error: rebuild verification FAILED at block %u of "
                        "Disk %d. The disk was NOT put back into service "
                        "(it shows as CORRUPT).\n", rep.bad_block, disk + 1);
        return 1;
    }
    if (rc != REC_OK) {
        fprintf(stderr, "Error: rebuild of Disk %d failed: %s. The disk was "
                        "NOT put back into service; run 'fileguard rebuild "
                        "%d' again.\n", disk + 1, rep.reason, disk + 1);
        return 1;
    }

    if (rep.level == RAID_LEVEL_1 && disk >= MIRROR_COPIES) {
        printf("Disk %d is not used by RAID-1: recreated as an empty disk "
               "(there is no data to restore).\n", disk + 1);
        printf("Disk %d is ONLINE again.\n", disk + 1);
        return 0;
    }

    printf("Rebuilt Disk %d of the %s volume.\n", disk + 1,
           level_label(rep.level));
    printf("  Wrote %u blocks, then re-read the new disk and compared every "
           "block with the surviving disks: %u of %u match.\n",
           rep.blocks_written, rep.blocks_verified, DISK_DATA_BLOCKS);
    printf("Disk %d is ONLINE again.\n", disk + 1);

    printf("Running a full verify of the volume...\n");
    vrc = do_verify(&vrep);
    return print_verify(vrc, &vrep);
}

static int cmd_verify(int argc)
{
    VerifyReport rep;
    int level, rc;

    if (argc != 2) {
        fprintf(stderr, "Usage: fileguard verify\n");
        return 1;
    }
    level = volume_level();
    if (level == VOLUME_LEVEL_NONE) {
        fprintf(stderr, "Error: the volume is not initialised "
                        "(run 'fileguard init' first)\n");
        return 1;
    }
    if (level == RAID_LEVEL_0) {
        printf("Verify (RAID-0): RAID-0 keeps no copies or parity, so "
               "FileGuard cannot check the data for consistency.\n");
        return 0;
    }
    rc = do_verify(&rep);
    return print_verify(rc, &rep);
}

int recovery_command(int argc, char *argv[])
{
    if (strcmp(argv[1], "fail") == 0)
        return cmd_fail(argc, argv);
    if (strcmp(argv[1], "rebuild") == 0)
        return cmd_rebuild(argc, argv);
    return cmd_verify(argc);
}
