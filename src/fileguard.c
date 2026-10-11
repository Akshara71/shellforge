#define _POSIX_C_SOURCE 200809L
/*
 * fileguard.c - FileGuard command interface (Core Module-I)
 *
 * This is the old FileGuard main() turned into a function so that the
 * Shellforge shell can call it as a builtin command.
 *
 * Commands:
 *   fileguard init                  create the virtual disks
 *   fileguard status                show the state of every virtual disk
 *   fileguard write <block> "text"  write text into a logical block
 *   fileguard read <block>          read a logical block back
 *   fileguard layout [block]        show which disk a logical block maps to
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "fileguard.h"
#include "disk.h"
#include "volume.h"

static int g_paths_ready = 0;

/*
 * Put the disk images in a "data" folder next to the running program, so
 * the location does not change when the user types "cd" inside the shell.
 */
static void setup_data_dir(void)
{
    char exe[300];
    char dir[DISK_PATH_MAX];
    char *slash;
    ssize_t n;

    if (g_paths_ready)
        return;
    g_paths_ready = 1;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return;                       /* keep the default "data" folder */
    exe[n] = '\0';

    slash = strrchr(exe, '/');
    if (slash == NULL)
        return;
    *slash = '\0';                    /* remove the program name */

    snprintf(dir, sizeof(dir), "%s/data", exe);
    disk_set_data_dir(dir);
}

static void print_usage(void)
{
    printf("FileGuard - user-space RAID-based virtual storage system\n\n");
    printf("Usage:\n");
    printf("  fileguard init                   Create %d virtual disks\n",
           NUM_DISKS);
    printf("  fileguard status                 Show the status of every disk\n");
    printf("  fileguard write <block> \"text\"   Write text to a logical block\n");
    printf("  fileguard read <block>           Read a logical block\n");
    printf("  fileguard layout [block]         Show block-to-disk mapping\n");
    printf("\nLogical blocks are numbered 0 to %d.\n", VOLUME_BLOCKS - 1);
}

/* Convert text to a logical block number. Returns 0 on success. */
static int parse_block(const char *text, uint32_t *out)
{
    char *end;
    unsigned long value;

    if (text[0] == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (*end != '\0' || errno != 0 || value >= VOLUME_BLOCKS)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static int cmd_init(void)
{
    char path[DISK_PATH_MAX];
    int i;

    /* make sure the data folder exists (it is fine if it already does) */
    if (mkdir(disk_get_data_dir(), 0755) != 0 && errno != EEXIST) {
        perror(disk_get_data_dir());
        return 1;
    }

    printf("Initializing %d virtual disks (existing data is erased)...\n",
           NUM_DISKS);
    for (i = 0; i < NUM_DISKS; i++) {
        if (disk_create(i) != DISK_OK) {
            fprintf(stderr, "Failed to create disk %d\n", i + 1);
            return 1;
        }
        disk_path(i, path, sizeof(path));
        printf("  Created %s (%d data blocks of %d bytes)\n",
               path, DISK_DATA_BLOCKS, BLOCK_SIZE);
    }
    printf("Done. Run 'fileguard status' to check the disks.\n");
    return 0;
}

static int cmd_status(void)
{
    char path[DISK_PATH_MAX];
    char label[16];
    const char *state;
    struct stat st;
    DiskHeader hdr;
    long long size;
    unsigned blocks;
    int online = 0;
    int rc, i;

    printf("FileGuard - Virtual Disk Status\n");
    printf("Data folder: %s\n", disk_get_data_dir());
    printf("--------------------------------------------------------------\n");
    printf("%-8s %-18s %12s %8s  %s\n",
           "Disk", "File", "Size(bytes)", "Blocks", "State");

    for (i = 0; i < NUM_DISKS; i++) {
        disk_path(i, path, sizeof(path));
        snprintf(label, sizeof(label), "Disk %d", i + 1);

        /* stat() asks the kernel for file information, including size */
        size = (stat(path, &st) == 0) ? (long long)st.st_size : 0;

        rc = disk_read_header(i, &hdr);
        blocks = 0;
        if (rc == DISK_OK) {
            state = "ONLINE";
            blocks = hdr.data_blocks;
            online++;
        } else if (rc == DISK_ERR_MISSING) {
            state = "MISSING";
        } else if (rc == DISK_ERR_INVALID) {
            state = "CORRUPT";
        } else {
            state = "ERROR";
        }

        printf("%-8s %-18s %12lld %8u  %s\n",
               label, path, size, blocks, state);
    }

    printf("--------------------------------------------------------------\n");
    printf("Disks online: %d of %d\n", online, NUM_DISKS);
    printf("Volume: %d logical blocks (%d bytes), striped, "
           "no redundancy yet\n",
           VOLUME_BLOCKS, VOLUME_BLOCKS * BLOCK_SIZE);
    return 0;
}

static int cmd_write(const char *block_text, const char *text)
{
    uint32_t lb, disk_block;
    int disk_id;
    size_t len = strlen(text);

    if (parse_block(block_text, &lb) != 0) {
        fprintf(stderr, "Error: block must be a number from 0 to %d\n",
                VOLUME_BLOCKS - 1);
        return 1;
    }
    if (len > BLOCK_SIZE - 1) {
        fprintf(stderr, "Error: text too long (maximum %d characters)\n",
                BLOCK_SIZE - 1);
        return 1;
    }
    /* len + 1 also stores the end-of-string character */
    if (volume_write(lb, text, len + 1) != VOLUME_OK) {
        fprintf(stderr, "Error: write failed (run 'fileguard init' first?)\n");
        return 1;
    }
    volume_map(lb, &disk_id, &disk_block);
    printf("Wrote %zu bytes to logical block %u -> Disk %d, block %u\n",
           len, lb, disk_id + 1, disk_block);
    return 0;
}

static int cmd_read(const char *block_text)
{
    unsigned char buf[BLOCK_SIZE + 1];
    uint32_t lb, disk_block;
    int disk_id;

    if (parse_block(block_text, &lb) != 0) {
        fprintf(stderr, "Error: block must be a number from 0 to %d\n",
                VOLUME_BLOCKS - 1);
        return 1;
    }
    if (volume_read(lb, buf) != VOLUME_OK) {
        fprintf(stderr, "Error: read failed (run 'fileguard init' first?)\n");
        return 1;
    }
    buf[BLOCK_SIZE] = '\0';   /* make sure the text always ends */

    volume_map(lb, &disk_id, &disk_block);
    printf("Logical block %u (Disk %d, block %u):\n",
           lb, disk_id + 1, disk_block);
    if (buf[0] == '\0')
        printf("  (empty block)\n");
    else
        printf("  %s\n", (char *)buf);
    return 0;
}

static void print_layout_row(uint32_t lb)
{
    uint32_t disk_block;
    int disk_id;

    volume_map(lb, &disk_id, &disk_block);
    printf("%13u   Disk %d   %10u\n", lb, disk_id + 1, disk_block);
}

static int cmd_layout(int argc, char *argv[])
{
    uint32_t lb;

    printf("Logical block -> physical location (striping, no parity yet)\n");
    printf("%13s   %-6s   %10s\n", "Logical block", "Disk", "Disk block");

    if (argc >= 3) {
        if (parse_block(argv[2], &lb) != 0) {
            fprintf(stderr, "Error: block must be a number from 0 to %d\n",
                    VOLUME_BLOCKS - 1);
            return 1;
        }
        print_layout_row(lb);
    } else {
        for (lb = 0; lb < 8; lb++)
            print_layout_row(lb);
    }
    return 0;
}

int fileguard_run(int argc, char *argv[])
{
    setup_data_dir();

    if (argc < 2) {
        print_usage();
        return 1;
    }
    if (strcmp(argv[1], "init") == 0)
        return cmd_init();
    if (strcmp(argv[1], "status") == 0)
        return cmd_status();
    if (strcmp(argv[1], "write") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: fileguard write <block> \"text\" "
                            "(put the text in quotes)\n");
            return 1;
        }
        return cmd_write(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "read") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: fileguard read <block>\n");
            return 1;
        }
        return cmd_read(argv[2]);
    }
    if (strcmp(argv[1], "layout") == 0)
        return cmd_layout(argc, argv);

    fprintf(stderr, "Unknown command: %s\n\n", argv[1]);
    print_usage();
    return 1;
}
