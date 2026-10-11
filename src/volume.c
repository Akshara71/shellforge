/*
 * volume.c - Storage Manager (FileGuard Core Module-I)
 *
 * Maps logical volume blocks onto the virtual disks and calls the
 * Virtual Disk Manager (disk.c) to do the actual reads and writes.
 */
#include <string.h>
#include "volume.h"

int volume_map(uint32_t logical_block, int *disk_id, uint32_t *disk_block)
{
    if (logical_block >= VOLUME_BLOCKS)
        return VOLUME_ERR_RANGE;

    *disk_id    = (int)(logical_block % NUM_DISKS);
    *disk_block = logical_block / NUM_DISKS;
    return VOLUME_OK;
}

int volume_write(uint32_t logical_block, const void *data, size_t len)
{
    unsigned char block[BLOCK_SIZE];
    uint32_t disk_block;
    int disk_id;

    if (len > BLOCK_SIZE)
        return VOLUME_ERR_RANGE;
    if (volume_map(logical_block, &disk_id, &disk_block) != VOLUME_OK)
        return VOLUME_ERR_RANGE;

    /* Copy the data into a full block and pad the rest with zeros */
    memset(block, 0, sizeof(block));
    memcpy(block, data, len);

    return disk_write_block(disk_id, disk_block, block);
}

int volume_read(uint32_t logical_block, void *buf)
{
    uint32_t disk_block;
    int disk_id;

    if (volume_map(logical_block, &disk_id, &disk_block) != VOLUME_OK)
        return VOLUME_ERR_RANGE;

    return disk_read_block(disk_id, disk_block, buf);
}
