#include "fs.h"
#include "heap.h"
#include "ata.h"
#include "tar.h"
#include <memory.h>

// Extra room past the original archive's size, for files created at runtime.
#define FS_EXTRA_SPACE (1 * 1024 * 1024) // 1 MB

// Sector 0 of the storage disk holds a small superblock so we can tell a
// previously-saved filesystem apart from a blank/garbage disk. Everything
// from sector 1 onward is the raw tar archive.
#define FS_SUPERBLOCK_LBA 0
#define FS_DATA_LBA       1
#define FS_MAGIC          0x3053464Bu // "KFS0"

struct fs_superblock {
    uint32_t magic;
    uint32_t used_size; // bytes of valid tar data stored on disk
    uint8_t  reserved[504]; // pad out to exactly one 512-byte sector
};

static unsigned char *archive_buf = NULL;
static size_t archive_capacity = 0;

static int fs_load_from_disk(void) {
    struct fs_superblock sb;

    if (!ata_read(FS_SUPERBLOCK_LBA, 1, &sb)) return 0;
    if (sb.magic != FS_MAGIC) return 0;
    if (sb.used_size == 0 || sb.used_size > archive_capacity) return 0;

    size_t sectors = (sb.used_size + 511) / 512;
    if (!ata_read(FS_DATA_LBA, (uint32_t)sectors, archive_buf)) return 0;

    return 1;
}

void fs_init(const void *initial_data, size_t initial_size) {
    size_t capacity = initial_size + FS_EXTRA_SPACE;
    archive_capacity = capacity;
    archive_buf = kalloc(capacity);

    if (fs_load_from_disk()) {
        return; // restored a previously-saved filesystem
    }

    // Nothing valid on disk yet (first boot, or a blank image) — fall back
    // to whatever Limine handed us.
    memcpy(archive_buf, initial_data, initial_size);
}

void *fs_get_archive(void) {
    return archive_buf;
}

size_t fs_get_capacity(void) {
    return archive_capacity;
}

int fs_sync(void) {
    /*if (archive_buf == NULL) return 0;

    size_t used = tar_used_size(archive_buf);
    if (used > archive_capacity) return 0;

    struct fs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.used_size = (uint32_t)used;

    if (!ata_write(FS_SUPERBLOCK_LBA, 1, &sb)) return 0;

    size_t sectors = (used + 511) / 512;
    if (sectors > 0 && !ata_write(FS_DATA_LBA, (uint32_t)sectors, archive_buf)) return 0;

    return 1;*/
    return 0;
}