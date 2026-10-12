#define _POSIX_C_SOURCE 200809L
/*
 * fileguard.c - FileGuard command interface
 *
 * This is the old FileGuard main() turned into a function so that the
 * Shellforge shell can call it as a builtin command.
 *
 * Commands:
 *   fileguard init [raid0|raid1|raid5]  create the virtual disks (default raid0)
 *   fileguard status                    show the state of every virtual disk
 *   fileguard write <block> "text"      write text into a logical block
 *   fileguard read <block>              read a logical block back
 *   fileguard layout [block]            show where a logical block is stored
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
    printf("  fileguard init [raid0|raid1|raid5]  Create %d virtual disks "
           "(default: raid0)\n", NUM_DISKS);
    printf("  fileguard status                 Show the status of every disk\n");
    printf("  fileguard write <block> \"text\"   Write text to a logical block\n");
    printf("  fileguard read <block>           Read a logical block\n");
    printf("  fileguard layout [block]         Show where a block is stored\n");
    printf("\nRAID-0 has %d logical blocks (0 to %d), RAID-1 has %d "
           "(0 to %d), RAID-5 has %d (0 to %d).\n",
           VOLUME_BLOCKS, VOLUME_BLOCKS - 1,
           VOLUME_BLOCKS_RAID1, VOLUME_BLOCKS_RAID1 - 1,
           VOLUME_BLOCKS_RAID5, VOLUME_BLOCKS_RAID5 - 1);
}

/* Convert text to a logical block number (0..capacity-1). 0 = success. */
static int parse_block(const char *text, uint32_t capacity, uint32_t *out)
{
    char *end;
    unsigned long value;

    if (text[0] == '\0' || text[0] == '-')
        return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (*end != '\0' || errno != 0 || value >= capacity)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static void print_block_error(uint32_t capacity)
{
    fprintf(stderr, "Error: block must be a number from 0 to %u\n",
            capacity - 1);
}

static const char *level_name(int level)
{
    if (level == RAID_LEVEL_1)
        return "RAID-1 (mirror)";
    if (level == RAID_LEVEL_5)
        return "RAID-5 (rotating parity)";
    return "RAID-0 (striping)";
}

static uint32_t level_capacity(int level)
{
    if (level == RAID_LEVEL_1)
        return VOLUME_BLOCKS_RAID1;
    if (level == RAID_LEVEL_5)
        return VOLUME_BLOCKS_RAID5;
    return VOLUME_BLOCKS;
}

static const char *disk_role(int level, int disk_id)
{
    if (level == RAID_LEVEL_1)
        return (disk_id < MIRROR_COPIES) ? "mirror" : "unused";
    if (level == RAID_LEVEL_5)
        return "data+parity";
    if (level == RAID_LEVEL_0)
        return "stripe";
    return "-";
}

/* The 1-based number of the first failed disk, or 0 if none has failed */
static int first_failed_disk(void)
{
    unsigned mask = 0;
    int i;

    volume_failed_disks(&mask);
    for (i = 0; i < NUM_DISKS; i++) {
        if (mask & (1u << i))
            return i + 1;
    }
    return 0;
}

static int cmd_init(int argc, char *argv[])
{
    char path[DISK_PATH_MAX];
    int level = RAID_LEVEL_0;
    int i;

    if (argc > 3) {
        fprintf(stderr, "Usage: fileguard init [raid0|raid1|raid5]\n");
        return 1;
    }
    if (argc == 3) {
        if (strcmp(argv[2], "raid0") == 0) {
            level = RAID_LEVEL_0;
        } else if (strcmp(argv[2], "raid1") == 0) {
            level = RAID_LEVEL_1;
        } else if (strcmp(argv[2], "raid5") == 0) {
            level = RAID_LEVEL_5;
        } else {
            fprintf(stderr, "Error: unknown RAID level '%s' "
                            "(use raid0, raid1 or raid5)\n", argv[2]);
            return 1;
        }
    }

    /* make sure the data folder exists (it is fine if it already does) */
    if (mkdir(disk_get_data_dir(), 0755) != 0 && errno != EEXIST) {
        perror(disk_get_data_dir());
        return 1;
    }

    printf("Initializing %d virtual disks as %s "
           "(existing data is erased)...\n", NUM_DISKS, level_name(level));
    for (i = 0; i < NUM_DISKS; i++) {
        if (disk_create(i, (uint32_t)level) != DISK_OK) {
            fprintf(stderr, "Failed to create disk %d\n", i + 1);
            return 1;
        }
        disk_path(i, path, sizeof(path));
        printf("  Created %s (%d data blocks of %d bytes)\n",
               path, DISK_DATA_BLOCKS, BLOCK_SIZE);
    }
    if (level == RAID_LEVEL_1) {
        printf("Mirror: Disk 1 and Disk 2 hold identical copies; "
               "Disk 3 and Disk 4 are not used.\n");
    } else if (level == RAID_LEVEL_5) {
        printf("Parity: every stripe has 3 data blocks and 1 parity block; "
               "the parity block rotates across the 4 disks.\n");
    }
    printf("Volume capacity: %u logical blocks (%u bytes)\n",
           level_capacity(level), level_capacity(level) * BLOCK_SIZE);
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
    int level = volume_level();
    int rc, i;

    printf("FileGuard - Virtual Disk Status\n");
    printf("Data folder: %s\n", disk_get_data_dir());
    printf("--------------------------------------------------------------\n");
    printf("%-8s %-18s %12s %8s  %-8s %s\n",
           "Disk", "File", "Size(bytes)", "Blocks", "State", "Role");

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

        printf("%-8s %-18s %12lld %8u  %-8s %s\n",
               label, path, size, blocks, state, disk_role(level, i));
    }

    printf("--------------------------------------------------------------\n");
    printf("Disks online: %d of %d\n", online, NUM_DISKS);

    if (level == RAID_LEVEL_1) {
        int healthy = volume_mirror_healthy();

        printf("Volume: RAID-1 mirror of Disk 1 and Disk 2, "
               "%d logical blocks (%d bytes)\n",
               VOLUME_BLOCKS_RAID1, VOLUME_BLOCKS_RAID1 * BLOCK_SIZE);
        if (healthy == MIRROR_COPIES)
            printf("Mirror state: HEALTHY (%d of %d copies available)\n",
                   healthy, MIRROR_COPIES);
        else if (healthy > 0)
            printf("Mirror state: DEGRADED (%d of %d copies available, "
                   "no redundancy until the failed disk is replaced)\n",
                   healthy, MIRROR_COPIES);
        else
            printf("Mirror state: FAILED (0 of %d copies available, "
                   "data unavailable)\n", MIRROR_COPIES);
    } else if (level == RAID_LEVEL_5) {
        printf("Volume: RAID-5 rotating parity over %d disks, "
               "%d logical blocks (%d bytes)\n",
               NUM_DISKS, VOLUME_BLOCKS_RAID5, VOLUME_BLOCKS_RAID5 * BLOCK_SIZE);
        if (online == NUM_DISKS)
            printf("Array state: HEALTHY (%d of %d disks available)\n",
                   online, NUM_DISKS);
        else if (online == NUM_DISKS - 1)
            printf("Array state: DEGRADED (%d of %d disks available, "
                   "no redundancy until the failed disk is replaced)\n",
                   online, NUM_DISKS);
        else
            printf("Array state: FAILED (%d of %d disks available, "
                   "more than one disk lost, data on failed disks "
                   "unavailable)\n", online, NUM_DISKS);
    } else if (level == RAID_LEVEL_0) {
        printf("Volume: RAID-0 striping, %d logical blocks (%d bytes), "
               "no redundancy\n",
               VOLUME_BLOCKS, VOLUME_BLOCKS * BLOCK_SIZE);
    } else {
        printf("Volume: not initialised (run 'fileguard init', "
               "'fileguard init raid1' or 'fileguard init raid5')\n");
    }
    return 0;
}

static int cmd_write(const char *block_text, const char *text)
{
    uint32_t capacity = volume_capacity();
    uint32_t lb, disk_block;
    unsigned mask = 0;
    int disk_id, level, rc;
    size_t len = strlen(text);

    if (parse_block(block_text, capacity, &lb) != 0) {
        print_block_error(capacity);
        return 1;
    }
    if (len > BLOCK_SIZE - 1) {
        fprintf(stderr, "Error: text too long (maximum %d characters)\n",
                BLOCK_SIZE - 1);
        return 1;
    }

    level = volume_level();
    /* len + 1 also stores the end-of-string character */
    rc = volume_write(lb, text, len + 1, &mask);
    if (rc != VOLUME_OK && rc != VOLUME_DEGRADED) {
        fprintf(stderr, "Error: write failed (run 'fileguard init' first?)\n");
        return 1;
    }
    volume_map(lb, &disk_id, &disk_block);

    if (level == RAID_LEVEL_1) {
        if (rc == VOLUME_OK) {
            printf("Wrote %zu bytes to logical block %u -> "
                   "Disk 1 and Disk 2, block %u\n", len, lb, disk_block);
        } else {
            int alive = (mask & 1u) ? 1 : 2;     /* 1-based disk number */
            int dead = (alive == 1) ? 2 : 1;
            printf("Wrote %zu bytes to logical block %u -> Disk %d only, "
                   "block %u (DEGRADED: Disk %d unavailable)\n",
                   len, lb, alive, disk_block, dead);
        }
    } else if (level == RAID_LEVEL_5) {
        int parity = volume_parity_disk(lb);
        int data_ok = (mask >> disk_id) & 1u;
        int parity_ok = (mask >> parity) & 1u;

        if (data_ok && parity_ok)
            printf("Wrote %zu bytes to logical block %u -> Disk %d, "
                   "block %u (parity on Disk %d)\n",
                   len, lb, disk_id + 1, disk_block, parity + 1);
        else if (data_ok)
            printf("Wrote %zu bytes to logical block %u -> Disk %d, "
                   "block %u (parity not updated)\n",
                   len, lb, disk_id + 1, disk_block);
        else
            printf("Wrote %zu bytes to logical block %u -> Disk %d "
                   "unavailable, block %u kept in parity on Disk %d\n",
                   len, lb, disk_id + 1, disk_block, parity + 1);
        if (rc == VOLUME_DEGRADED)
            printf("  (DEGRADED: Disk %d unavailable)\n", first_failed_disk());
    } else {
        printf("Wrote %zu bytes to logical block %u -> Disk %d, block %u\n",
               len, lb, disk_id + 1, disk_block);
    }
    return 0;
}

static int cmd_read(const char *block_text)
{
    unsigned char buf[BLOCK_SIZE + 1];
    uint32_t capacity = volume_capacity();
    uint32_t lb, disk_block;
    int disk_id, source = 0, rc, level;

    if (parse_block(block_text, capacity, &lb) != 0) {
        print_block_error(capacity);
        return 1;
    }
    level = volume_level();
    rc = volume_read(lb, buf, &source);
    if (rc != VOLUME_OK && rc != VOLUME_DEGRADED) {
        fprintf(stderr, "Error: read failed (run 'fileguard init' first?)\n");
        return 1;
    }
    buf[BLOCK_SIZE] = '\0';   /* make sure the text always ends */

    volume_map(lb, &disk_id, &disk_block);
    printf("Logical block %u (Disk %d, block %u):\n",
           lb, source + 1, disk_block);
    if (buf[0] == '\0')
        printf("  (empty block)\n");
    else
        printf("  %s\n", (char *)buf);
    if (rc == VOLUME_DEGRADED) {
        if (level == RAID_LEVEL_5)
            printf("  (DEGRADED: Disk %d unavailable, "
                   "block rebuilt from parity)\n", source + 1);
        else
            printf("  (DEGRADED: Disk 1 unavailable, "
                   "data served from Disk %d)\n", source + 1);
    }
    return 0;
}

static void print_layout_row(uint32_t lb, int level)
{
    uint32_t disk_block;
    int disk_id;

    volume_map(lb, &disk_id, &disk_block);
    if (level == RAID_LEVEL_1)
        printf("%13u   %-15s   %10u\n", lb, "Disk 1 + Disk 2", disk_block);
    else if (level == RAID_LEVEL_5)
        printf("%13u   Disk %d   %10u   Disk %d\n",
               lb, disk_id + 1, disk_block, volume_parity_disk(lb) + 1);
    else
        printf("%13u   Disk %d   %10u\n", lb, disk_id + 1, disk_block);
}

static int cmd_layout(int argc, char *argv[])
{
    uint32_t capacity = volume_capacity();
    int level = volume_level();
    uint32_t lb, rows;

    if (level == RAID_LEVEL_1) {
        printf("Logical block -> physical location "
               "(RAID-1 mirror, %u blocks)\n", capacity);
        printf("%13s   %-15s   %10s\n", "Logical block", "Disks", "Disk block");
    } else if (level == RAID_LEVEL_5) {
        printf("Logical block -> physical location "
               "(RAID-5 rotating parity, %u blocks)\n", capacity);
        printf("%13s   %-6s   %10s   %s\n",
               "Logical block", "Disk", "Disk block", "Parity");
    } else {
        printf("Logical block -> physical location "
               "(RAID-0 striping, %u blocks)\n", capacity);
        printf("%13s   %-6s   %10s\n", "Logical block", "Disk", "Disk block");
    }

    if (argc >= 3) {
        if (parse_block(argv[2], capacity, &lb) != 0) {
            print_block_error(capacity);
            return 1;
        }
        print_layout_row(lb, level);
    } else {
        /* RAID-5 shows 12 rows so one full rotation of the parity is visible */
        rows = (level == RAID_LEVEL_5) ? 12 : 8;
        if (capacity < rows)
            rows = capacity;
        for (lb = 0; lb < rows; lb++)
            print_layout_row(lb, level);
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
        return cmd_init(argc, argv);
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
