/*
 * volume.h - Storage Manager interface (FileGuard Core Module-I)
 *
 * The volume presents the 4 virtual disks as ONE array of logical blocks.
 * Module-I layout: simple striping (no redundancy yet).
 *   disk       = logical_block % NUM_DISKS
 *   disk_block = logical_block / NUM_DISKS
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stdint.h>
#include <stddef.h>
#include "disk.h"

/* Total logical blocks in the volume */
#define VOLUME_BLOCKS  (NUM_DISKS * DISK_DATA_BLOCKS)

/* Return codes */
#define VOLUME_OK          0
#define VOLUME_ERR_RANGE  -1   /* logical block or data length too large */

/* Translate a logical block to (disk, block-on-disk) */
int volume_map(uint32_t logical_block, int *disk_id, uint32_t *disk_block);

/* Write up to BLOCK_SIZE bytes into a logical block (zero-padded) */
int volume_write(uint32_t logical_block, const void *data, size_t len);

/* Read one full logical block (BLOCK_SIZE bytes) into buf */
int volume_read(uint32_t logical_block, void *buf);

#endif /* VOLUME_H */
