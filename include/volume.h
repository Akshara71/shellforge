/*
 * volume.h - Storage Manager interface (FileGuard)
 *
 * The volume presents the virtual disks as ONE array of logical blocks.
 * The RAID level is stored in the disk headers (see disk.h):
 *
 *   RAID-0 (striping): 4 disks, 4096 logical blocks, no redundancy
 *       disk       = logical_block % NUM_DISKS
 *       disk_block = logical_block / NUM_DISKS
 *
 *   RAID-1 (mirror): Disk 1 and Disk 2 hold identical copies of every
 *       block, so the volume has 1024 logical blocks. Disk 3 and Disk 4
 *       are not used.
 *       disk_block = logical_block, stored on BOTH mirror disks
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stdint.h>
#include <stddef.h>
#include "disk.h"

/* Capacity (in logical blocks) of each RAID level */
#define VOLUME_BLOCKS       (NUM_DISKS * DISK_DATA_BLOCKS)   /* RAID-0 */
#define VOLUME_BLOCKS_RAID1 (DISK_DATA_BLOCKS)               /* RAID-1 */

/* RAID-1 uses disks 0 .. MIRROR_COPIES-1 (Disk 1 and Disk 2) */
#define MIRROR_COPIES       2

/* volume_level() result when no disk has a valid header */
#define VOLUME_LEVEL_NONE  -1

/* Return codes */
#define VOLUME_OK           0   /* done, full redundancy intact             */
#define VOLUME_DEGRADED     1   /* done, but only one RAID-1 copy was used  */
#define VOLUME_ERR_RANGE   -1   /* logical block or data length too large   */
#define VOLUME_ERR_IO      -2   /* no disk could do the operation           */

/* RAID level of the volume (RAID_LEVEL_0, RAID_LEVEL_1 or VOLUME_LEVEL_NONE) */
int volume_level(void);

/* Number of logical blocks in the volume at its current RAID level */
uint32_t volume_capacity(void);

/* RAID-1: how many of the mirror disks have a valid header (0..2) */
int volume_mirror_healthy(void);

/* Translate a logical block to (disk, block-on-disk). For RAID-1 this is
 * the primary copy (Disk 1); the same block number is also on Disk 2. */
int volume_map(uint32_t logical_block, int *disk_id, uint32_t *disk_block);

/* Write up to BLOCK_SIZE bytes into a logical block (zero-padded).
 * written_mask (may be NULL): bit n is set if disk n received the data. */
int volume_write(uint32_t logical_block, const void *data, size_t len,
                 unsigned *written_mask);

/* Read one full logical block (BLOCK_SIZE bytes) into buf.
 * source_disk (may be NULL): which disk (0-based) supplied the data. */
int volume_read(uint32_t logical_block, void *buf, int *source_disk);

#endif /* VOLUME_H */
