/*
 * disk.h - Virtual Disk Manager interface (FileGuard Core Module-I)
 *
 * A "virtual disk" is an ordinary file in the data/ folder.
 * Layout of every disk file:
 *   Block 0          : header (identifies the disk)
 *   Blocks 1..1024   : data blocks
 */
#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stddef.h>

#define NUM_DISKS         4          /* number of virtual disks          */
#define BLOCK_SIZE        512        /* bytes per block                  */
#define DISK_DATA_BLOCKS  1024       /* data blocks per disk (no header) */
#define DISK_DIR          "data"     /* folder holding the .img files    */
#define DISK_MAGIC        0x46475244u /* identifies a FileGuard disk     */

/* Return codes used by the disk functions */
#define DISK_OK            0
#define DISK_ERR_MISSING  -1   /* disk file does not exist           */
#define DISK_ERR_INVALID  -2   /* file exists but header is wrong    */
#define DISK_ERR_IO       -3   /* some other system call failure     */

/* Header stored in block 0 of every disk file */
typedef struct {
    uint32_t magic;        /* must equal DISK_MAGIC                  */
    uint32_t version;      /* format version                         */
    uint32_t disk_id;      /* 0-based disk number                    */
    uint32_t num_disks;    /* total disks in the array               */
    uint32_t block_size;   /* bytes per block                        */
    uint32_t data_blocks;  /* number of data blocks (excl. header)   */
} DiskHeader;

/* Build the file path for a disk, e.g. disk 0 -> "data/disk1.img" */
void disk_path(int disk_id, char *buf, size_t size);

/* Create (or recreate) a disk file and write its header */
int disk_create(int disk_id);

/* Read and validate a disk's header */
int disk_read_header(int disk_id, DiskHeader *hdr);

/* Read/write one data block (block_no is 0..DISK_DATA_BLOCKS-1) */
int disk_read_block(int disk_id, uint32_t block_no, void *buf);
int disk_write_block(int disk_id, uint32_t block_no, const void *buf);

#endif /* DISK_H */
