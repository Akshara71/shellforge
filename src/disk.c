#define _POSIX_C_SOURCE 200809L
/*
 * disk.c - Virtual Disk Manager (FileGuard)
 *
 * All access to the virtual disk files goes through Linux system calls:
 * open, close, ftruncate, pread, pwrite.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "disk.h"

/* Folder that holds the disk image files (can be changed at run time) */
static char g_data_dir[400] = DISK_DIR;

void disk_set_data_dir(const char *dir)
{
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);
}

const char *disk_get_data_dir(void)
{
    return g_data_dir;
}

void disk_path(int disk_id, char *buf, size_t size)
{
    /* disk_id 0 -> disk1.img, so file names are 1-based */
    snprintf(buf, size, "%s/disk%d.img", g_data_dir, disk_id + 1);
}

int disk_create(int disk_id, uint32_t raid_level)
{
    char path[DISK_PATH_MAX];
    unsigned char block[BLOCK_SIZE];
    DiskHeader hdr;
    off_t total_size;
    int fd;

    disk_path(disk_id, path, sizeof(path));

    /* open() asks the kernel for a file descriptor to the disk file */
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        perror(path);
        return DISK_ERR_IO;
    }

    /* ftruncate() sets the file size; new space reads back as zeros */
    total_size = (off_t)(1 + DISK_DATA_BLOCKS) * BLOCK_SIZE;
    if (ftruncate(fd, total_size) < 0) {
        perror("ftruncate");
        close(fd);
        return DISK_ERR_IO;
    }

    /* Fill in the header and place it in a full zero-padded block */
    hdr.magic       = DISK_MAGIC;
    hdr.version     = 2;
    hdr.disk_id     = (uint32_t)disk_id;
    hdr.num_disks   = NUM_DISKS;
    hdr.block_size  = BLOCK_SIZE;
    hdr.data_blocks = DISK_DATA_BLOCKS;
    hdr.raid_level  = raid_level;

    memset(block, 0, sizeof(block));
    memcpy(block, &hdr, sizeof(hdr));

    /* pwrite() writes at an exact offset (0 = start of file) */
    if (pwrite(fd, block, BLOCK_SIZE, 0) != BLOCK_SIZE) {
        perror("pwrite header");
        close(fd);
        return DISK_ERR_IO;
    }

    close(fd);
    return DISK_OK;
}

int disk_read_header(int disk_id, DiskHeader *hdr)
{
    char path[DISK_PATH_MAX];
    unsigned char block[BLOCK_SIZE];
    ssize_t n;
    int fd;

    disk_path(disk_id, path, sizeof(path));

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return DISK_ERR_MISSING;
        return DISK_ERR_IO;
    }

    n = pread(fd, block, BLOCK_SIZE, 0);
    close(fd);
    if (n != BLOCK_SIZE)
        return DISK_ERR_INVALID;

    /* Old version-1 disks have zeros after their 24-byte header, so
     * raid_level reads as 0 (RAID-0), which is exactly what they are. */
    memcpy(hdr, block, sizeof(DiskHeader));
    if (hdr->magic != DISK_MAGIC || hdr->disk_id != (uint32_t)disk_id)
        return DISK_ERR_INVALID;

    return DISK_OK;
}

int disk_write_block(int disk_id, uint32_t block_no, const void *buf)
{
    char path[DISK_PATH_MAX];
    off_t offset;
    ssize_t n;
    int fd;

    if (block_no >= DISK_DATA_BLOCKS)
        return DISK_ERR_IO;

    disk_path(disk_id, path, sizeof(path));
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return (errno == ENOENT) ? DISK_ERR_MISSING : DISK_ERR_IO;

    /* +1 skips the header block */
    offset = (off_t)(1 + block_no) * BLOCK_SIZE;
    n = pwrite(fd, buf, BLOCK_SIZE, offset);
    close(fd);

    return (n == BLOCK_SIZE) ? DISK_OK : DISK_ERR_IO;
}

int disk_read_block(int disk_id, uint32_t block_no, void *buf)
{
    char path[DISK_PATH_MAX];
    off_t offset;
    ssize_t n;
    int fd;

    if (block_no >= DISK_DATA_BLOCKS)
        return DISK_ERR_IO;

    disk_path(disk_id, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return (errno == ENOENT) ? DISK_ERR_MISSING : DISK_ERR_IO;

    offset = (off_t)(1 + block_no) * BLOCK_SIZE;
    n = pread(fd, buf, BLOCK_SIZE, offset);
    close(fd);

    return (n == BLOCK_SIZE) ? DISK_OK : DISK_ERR_IO;
}
