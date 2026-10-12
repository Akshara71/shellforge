/*
 * volume.c - Storage Manager (FileGuard)
 *
 * Maps logical volume blocks onto the virtual disks (RAID-0 striping,
 * RAID-1 mirroring or RAID-5 rotating parity) and calls the Virtual Disk
 * Manager (disk.c) to do the actual reads and writes.
 */
#include <string.h>
#include "volume.h"

#define RAID5_DATA_DISKS (NUM_DISKS - 1)

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
     * is still recognised when a disk is missing or damaged. */
    for (i = 0; i < NUM_DISKS; i++) {
        if (disk_read_header(i, &hdr) == DISK_OK &&
            (hdr.raid_level == RAID_LEVEL_0 ||
             hdr.raid_level == RAID_LEVEL_1 ||
             hdr.raid_level == RAID_LEVEL_5))
            return (int)hdr.raid_level;
    }
    return VOLUME_LEVEL_NONE;
}

static uint32_t capacity_for(int level)
{
    if (level == RAID_LEVEL_1)
        return VOLUME_BLOCKS_RAID1;
    if (level == RAID_LEVEL_5)
        return VOLUME_BLOCKS_RAID5;
    return VOLUME_BLOCKS;
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

int volume_failed_disks(unsigned *failed_mask)
{
    unsigned mask = 0;
    int count = 0;
    int i;

    for (i = 0; i < NUM_DISKS; i++) {
        if (!disk_is_healthy(i)) {
            mask |= 1u << i;
            count++;
        }
    }
    if (failed_mask != NULL)
        *failed_mask = mask;
    return count;
}

/* ---- RAID-5 geometry ------------------------------------------------ */

/* The parity block moves one disk to the left on every stripe:
 * stripe 0 -> Disk 4, stripe 1 -> Disk 3, stripe 2 -> Disk 2,
 * stripe 3 -> Disk 1, stripe 4 -> Disk 4 again, ... */
static int raid5_parity_disk(uint32_t stripe)
{
    return (NUM_DISKS - 1) - (int)(stripe % NUM_DISKS);
}

/* The data disk for position pos (0..2) of a stripe: the pos-th disk that
 * is not the parity disk. */
static int raid5_data_disk(uint32_t stripe, uint32_t pos)
{
    int parity = raid5_parity_disk(stripe);
    uint32_t seen = 0;
    int d;

    for (d = 0; d < NUM_DISKS; d++) {
        if (d == parity)
            continue;
        if (seen == pos)
            return d;
        seen++;
    }
    return 0;   /* not reached: pos is always 0..2 */
}

static void xor_block(unsigned char *dst, const unsigned char *src)
{
    int i;

    for (i = 0; i < BLOCK_SIZE; i++)
        dst[i] ^= src[i];
}

static int map_block(int level, uint32_t logical_block,
                     int *disk_id, uint32_t *disk_block)
{
    if (logical_block >= capacity_for(level))
        return VOLUME_ERR_RANGE;

    if (level == RAID_LEVEL_1) {
        *disk_id    = 0;                 /* primary copy; Disk 2 has a twin */
        *disk_block = logical_block;
    } else if (level == RAID_LEVEL_5) {
        uint32_t stripe = logical_block / RAID5_DATA_DISKS;

        *disk_id    = raid5_data_disk(stripe, logical_block % RAID5_DATA_DISKS);
        *disk_block = stripe;
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

int volume_parity_disk(uint32_t logical_block)
{
    if (volume_level() != RAID_LEVEL_5 || logical_block >= VOLUME_BLOCKS_RAID5)
        return -1;
    return raid5_parity_disk(logical_block / RAID5_DATA_DISKS);
}

/* ---- RAID-5 write --------------------------------------------------- */

static int write_raid5(uint32_t stripe, int data_disk,
                       const unsigned char *new_data, unsigned *written_mask)
{
    unsigned char old_data[BLOCK_SIZE];
    unsigned char parity_blk[BLOCK_SIZE];
    unsigned char tmp[BLOCK_SIZE];
    unsigned failed = 0;
    unsigned mask = 0;
    int nfailed = volume_failed_disks(&failed);
    int parity = raid5_parity_disk(stripe);
    int d;

    /* One parity block can only repair ONE missing block per stripe.
     * With two failed disks we refuse to write rather than lose data. */
    if (nfailed > 1)
        return VOLUME_ERR_IO;

    if (failed & (1u << data_disk)) {
        /* The data disk is down. Keep the new data alive inside the parity:
         * parity = new data XOR the other two data blocks. A later read of
         * this block rebuilds it from the parity. */
        memcpy(parity_blk, new_data, BLOCK_SIZE);
        for (d = 0; d < NUM_DISKS; d++) {
            if (d == data_disk || d == parity)
                continue;
            if (disk_read_block(d, stripe, tmp) != DISK_OK)
                return VOLUME_ERR_IO;
            xor_block(parity_blk, tmp);
        }
        if (disk_write_block(parity, stripe, parity_blk) != DISK_OK)
            return VOLUME_ERR_IO;
        mask = 1u << parity;
    } else if (failed & (1u << parity)) {
        /* The parity disk is down: there is no parity to update. */
        if (disk_write_block(data_disk, stripe, new_data) != DISK_OK)
            return VOLUME_ERR_IO;
        mask = 1u << data_disk;
    } else {
        /* Normal small write (read-modify-write):
         * new parity = old parity XOR old data XOR new data.
         * This also works when a different data disk is down, because
         * the old parity already contains that block. */
        if (disk_read_block(data_disk, stripe, old_data) != DISK_OK ||
            disk_read_block(parity, stripe, parity_blk) != DISK_OK)
            return VOLUME_ERR_IO;
        xor_block(parity_blk, old_data);
        xor_block(parity_blk, new_data);
        if (disk_write_block(data_disk, stripe, new_data) != DISK_OK)
            return VOLUME_ERR_IO;
        if (disk_write_block(parity, stripe, parity_blk) != DISK_OK)
            return VOLUME_ERR_IO;
        mask = (1u << data_disk) | (1u << parity);
    }

    if (written_mask != NULL)
        *written_mask = mask;
    return (nfailed > 0) ? VOLUME_DEGRADED : VOLUME_OK;
}

/* ---- RAID-5 read ---------------------------------------------------- */

static int read_raid5(uint32_t stripe, int data_disk, void *buf,
                      int *source_disk)
{
    unsigned char rebuilt[BLOCK_SIZE];
    unsigned char tmp[BLOCK_SIZE];
    unsigned failed = 0;
    int nfailed = volume_failed_disks(&failed);
    int d;

    if (source_disk != NULL)
        *source_disk = data_disk;

    /* The block's own disk is fine: read it directly. */
    if (!(failed & (1u << data_disk))) {
        if (disk_read_block(data_disk, stripe, buf) != DISK_OK)
            return VOLUME_ERR_IO;
        return VOLUME_OK;
    }

    /* The block's disk is down: XOR the other data blocks and the parity.
     * That only works when no other disk is down. */
    if (nfailed > 1)
        return VOLUME_ERR_IO;

    memset(rebuilt, 0, sizeof(rebuilt));
    for (d = 0; d < NUM_DISKS; d++) {
        if (d == data_disk)
            continue;
        if (disk_read_block(d, stripe, tmp) != DISK_OK)
            return VOLUME_ERR_IO;
        xor_block(rebuilt, tmp);
    }
    memcpy(buf, rebuilt, BLOCK_SIZE);
    return VOLUME_DEGRADED;
}

/* ---- public write / read -------------------------------------------- */

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

    if (level == RAID_LEVEL_5)
        return write_raid5(disk_block, disk_id, block, written_mask);

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

    if (level == RAID_LEVEL_5)
        return read_raid5(disk_block, disk_id, buf, source_disk);

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
