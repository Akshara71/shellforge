/*
 * volume.c - Storage Manager (FileGuard)
 *
 * Maps logical volume blocks onto the virtual disks (RAID-0 striping or
 * RAID-1 mirroring) and calls the Virtual Disk Manager (disk.c) to do the
 * actual reads and writes.
 */
#include <string.h>
#include "volume.h"

/* A disk is healthy only if its file exists AND its header is valid */
static int disk_is_healthy(int disk_id)
{
    DiskHeader hdr;

    return disk_read_header(disk_id, &hdr) == DISK_OK;
}

int volume_level(void)
{
    DiskHeader hdr;
    int i;

    /* Any disk with a valid header can tell us the level, so the volume
     * is still recognised when one disk is missing or damaged. */
    for (i = 0; i < NUM_DISKS; i++) {
        if (disk_read_header(i, &hdr) == DISK_OK &&
            (hdr.raid_level == RAID_LEVEL_0 || hdr.raid_level == RAID_LEVEL_1))
            return (int)hdr.raid_level;
    }
    return VOLUME_LEVEL_NONE;
}

static uint32_t capacity_for(int level)
{
    return (level == RAID_LEVEL_1) ? VOLUME_BLOCKS_RAID1 : VOLUME_BLOCKS;
}

uint32_t volume_capacity(void)
{
    return capacity_for(volume_level());
}

int volume_mirror_healthy(void)
{
    int healthy = 0;
    int i;

    for (i = 0; i < MIRROR_COPIES; i++) {
        if (disk_is_healthy(i))
            healthy++;
    }
    return healthy;
}

static int map_block(int level, uint32_t logical_block,
                     int *disk_id, uint32_t *disk_block)
{
    if (logical_block >= capacity_for(level))
        return VOLUME_ERR_RANGE;

    if (level == RAID_LEVEL_1) {
        *disk_id    = 0;                 /* primary copy; Disk 2 has a twin */
        *disk_block = logical_block;
    } else {
        *disk_id    = (int)(logical_block % NUM_DISKS);
        *disk_block = logical_block / NUM_DISKS;
    }
    return VOLUME_OK;
}

int volume_map(uint32_t logical_block, int *disk_id, uint32_t *disk_block)
{
    return map_block(volume_level(), logical_block, disk_id, disk_block);
}

int volume_write(uint32_t logical_block, const void *data, size_t len,
                 unsigned *written_mask)
{
    unsigned char block[BLOCK_SIZE];
    uint32_t disk_block;
    unsigned mask = 0;
    int disk_id, level, copies, i;

    if (written_mask != NULL)
        *written_mask = 0;
    if (len > BLOCK_SIZE)
        return VOLUME_ERR_RANGE;

    level = volume_level();
    if (map_block(level, logical_block, &disk_id, &disk_block) != VOLUME_OK)
        return VOLUME_ERR_RANGE;

    /* Copy the data into a full block and pad the rest with zeros */
    memset(block, 0, sizeof(block));
    memcpy(block, data, len);

    if (level == RAID_LEVEL_1) {
        /* Write the block to every healthy mirror disk */
        copies = 0;
        for (i = 0; i < MIRROR_COPIES; i++) {
            if (disk_is_healthy(i) &&
                disk_write_block(i, disk_block, block) == DISK_OK) {
                mask |= 1u << i;
                copies++;
            }
        }
        if (written_mask != NULL)
            *written_mask = mask;
        if (copies == MIRROR_COPIES)
            return VOLUME_OK;
        if (copies > 0)
            return VOLUME_DEGRADED;      /* only one copy exists now */
        return VOLUME_ERR_IO;            /* nothing was written */
    }

    /* RAID-0 (or no valid disk at all: the write fails with a disk error) */
    if (disk_write_block(disk_id, disk_block, block) != DISK_OK)
        return VOLUME_ERR_IO;
    if (written_mask != NULL)
        *written_mask = 1u << disk_id;
    return VOLUME_OK;
}

int volume_read(uint32_t logical_block, void *buf, int *source_disk)
{
    uint32_t disk_block;
    int disk_id, level, i;

    level = volume_level();
    if (map_block(level, logical_block, &disk_id, &disk_block) != VOLUME_OK)
        return VOLUME_ERR_RANGE;

    if (level == RAID_LEVEL_1) {
        /* Use the first healthy copy: Disk 1, then Disk 2 */
        for (i = 0; i < MIRROR_COPIES; i++) {
            if (disk_is_healthy(i) &&
                disk_read_block(i, disk_block, buf) == DISK_OK) {
                if (source_disk != NULL)
                    *source_disk = i;
                return (i == 0) ? VOLUME_OK : VOLUME_DEGRADED;
            }
        }
        return VOLUME_ERR_IO;
    }

    if (disk_read_block(disk_id, disk_block, buf) != DISK_OK)
        return VOLUME_ERR_IO;
    if (source_disk != NULL)
        *source_disk = disk_id;
    return VOLUME_OK;
}
