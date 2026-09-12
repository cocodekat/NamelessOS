#include <memory.h>
#include "tar.h"

// USTAR header format — every entry in the archive starts with one of these,
// padded/aligned to 512 bytes, followed by that many bytes of file data
// (also padded to a multiple of 512).
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];      // file size in octal ASCII text, e.g. "00000033"
    char mtime[12];
    char chksum[8];
    char typeflag;       // '0' or '\0' = regular file, '5' = directory
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

// tar stores sizes as octal text, not binary — convert manually.
static size_t octal_to_size(const char *str, size_t len) {
    size_t result = 0;
    for (size_t i = 0; i < len && str[i] >= '0' && str[i] <= '7'; i++) {
        result = result * 8 + (str[i] - '0');
    }
    return result;
}

static int streq_tar(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

// tar entries are prefixed with "./" by GNU tar (e.g. "./docs/notes.txt").
// Skip that prefix so callers can just use "docs/notes.txt".
static const char *skip_prefix(const char *name) {
    if (name[0] == '.' && name[1] == '/') {
        return name + 2;
    }
    return name;
}

static const void *tar_walk(const void *archive, const char *path,
                             size_t *out_size,
                             void (*callback)(const char *name, size_t size)) {
    const char *ptr = (const char *)archive;

    for (;;) {
        const struct tar_header *hdr = (const struct tar_header *)ptr;

        // An empty name means end of archive.
        if (hdr->name[0] == '\0') {
            break;
        }

        size_t size = octal_to_size(hdr->size, sizeof(hdr->size));
        const char *name = skip_prefix(hdr->name);

        int is_file = (hdr->typeflag == '0' || hdr->typeflag == '\0');
        int is_dir  = (hdr->typeflag == '5');

        if (callback != NULL && (is_file || is_dir)) {
            callback(name, size);
        }

        if (path != NULL && is_file && streq_tar(name, path)) {
            if (out_size != NULL) *out_size = size;
            return ptr + 512;
        }

        // Advance past this header and its (padded) data to the next header.
        size_t data_blocks = (size + 511) / 512;
        ptr += 512 + (data_blocks * 512);
    }

    return NULL;
}

const void *tar_find(const void *archive, const char *path, size_t *out_size) {
    return tar_walk(archive, path, out_size, NULL);
}

void tar_list(const void *archive, void (*callback)(const char *name, size_t size)) {
    tar_walk(archive, NULL, NULL, callback);
}

static void size_to_octal(char *dest, size_t dest_len, size_t value) {
    for (int i = (int)dest_len - 2; i >= 0; i--) {
        dest[i] = '0' + (char)(value % 8);
        value /= 8;
    }
    dest[dest_len - 1] = '\0';
}

static size_t compute_checksum(const struct tar_header *hdr) {
    const unsigned char *bytes = (const unsigned char *)hdr;
    size_t sum = 0;
    for (size_t i = 0; i < sizeof(struct tar_header); i++) {
        if (i >= offsetof(struct tar_header, chksum) &&
            i < offsetof(struct tar_header, chksum) + sizeof(hdr->chksum)) {
            sum += ' '; // checksum field itself counts as spaces during calc
        } else {
            sum += bytes[i];
        }
    }
    return sum;
}

static void write_checksum(struct tar_header *hdr) {
    size_t sum = compute_checksum(hdr);
    for (int i = 5; i >= 0; i--) {
        hdr->chksum[i] = '0' + (char)(sum % 8);
        sum /= 8;
    }
    hdr->chksum[6] = '\0';
    hdr->chksum[7] = ' ';
}

static void copy_name(char *dest, size_t dest_size, const char *name) {
    size_t i = 0;
    while (name[i] && i < dest_size - 1) {
        dest[i] = name[i];
        i++;
    }
}

static size_t tar_end_offset(const void *archive) {
    const unsigned char *base = (const unsigned char *)archive;
    size_t offset = 0;
    for (;;) {
        const struct tar_header *hdr = (const struct tar_header *)(base + offset);
        if (hdr->name[0] == '\0') return offset;
        size_t size = octal_to_size(hdr->size, sizeof(hdr->size));
        size_t padded = ((size + 511) / 512) * 512;
        offset += 512 + padded;
    }
}

size_t tar_used_size(const void *archive) {
    return tar_end_offset(archive);
}

int tar_append_file(void *archive, size_t capacity, const char *path, const void *data, size_t size) {
    size_t offset = tar_end_offset(archive);
    size_t padded_size = ((size + 511) / 512) * 512;

    if (offset + 512 + padded_size + 1024 > capacity) {
        return 0; // not enough room left
    }

    unsigned char *base = (unsigned char *)archive;
    struct tar_header *hdr = (struct tar_header *)(base + offset);

    memset(hdr, 0, sizeof(struct tar_header));
    copy_name(hdr->name, sizeof(hdr->name), path);
    hdr->typeflag = '0';
    memcpy(hdr->mode, "0000644", 7);
    memcpy(hdr->uid, "0000000", 7);
    memcpy(hdr->gid, "0000000", 7);
    size_to_octal(hdr->size, sizeof(hdr->size), size);
    memcpy(hdr->magic, "ustar", 5);
    memcpy(hdr->version, "00", 2);
    write_checksum(hdr);

    if (size > 0 && data != NULL) {
        memcpy(base + offset + 512, data, size);
    }

    return 1;
}

int tar_append_dir(void *archive, size_t capacity, const char *path) {
    size_t offset = tar_end_offset(archive);
    if (offset + 512 + 1024 > capacity) {
        return 0;
    }

    char dirname[100];
    size_t len = 0;
    while (path[len] && len < sizeof(dirname) - 2) {
        dirname[len] = path[len];
        len++;
    }
    if (len == 0 || dirname[len - 1] != '/') {
        dirname[len++] = '/';
    }
    dirname[len] = '\0';

    unsigned char *base = (unsigned char *)archive;
    struct tar_header *hdr = (struct tar_header *)(base + offset);

    memset(hdr, 0, sizeof(struct tar_header));
    copy_name(hdr->name, sizeof(hdr->name), dirname);
    hdr->typeflag = '5';
    memcpy(hdr->mode, "0000755", 7);
    memcpy(hdr->uid, "0000000", 7);
    memcpy(hdr->gid, "0000000", 7);
    size_to_octal(hdr->size, sizeof(hdr->size), 0);
    memcpy(hdr->magic, "ustar", 5);
    memcpy(hdr->version, "00", 2);
    write_checksum(hdr);

    return 1;
}

static int name_matches(const char *name, const char *path) {
    size_t name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len > 0 && name[name_len - 1] == '/') name_len--;

    size_t path_len = 0;
    while (path[path_len]) path_len++;
    if (path_len > 0 && path[path_len - 1] == '/') path_len--;

    if (name_len != path_len) return 0;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] != path[i]) return 0;
    }
    return 1;
}

int tar_remove(void *archive, const char *path) {
    unsigned char *base = (unsigned char *)archive;
    size_t offset = 0;
    size_t entry_offset = 0, entry_total = 0;
    int found = 0;

    for (;;) {
        struct tar_header *hdr = (struct tar_header *)(base + offset);
        if (hdr->name[0] == '\0') break;

        size_t size = octal_to_size(hdr->size, sizeof(hdr->size));
        size_t padded = ((size + 511) / 512) * 512;
        size_t total = 512 + padded;
        const char *name = skip_prefix(hdr->name);

        int is_file = (hdr->typeflag == '0');
        int is_dir  = (hdr->typeflag == '5');

        if ((is_file || is_dir) && name_matches(name, path)) {
            entry_offset = offset;
            entry_total = total;
            found = 1;
            break;
        }
        offset += total;
    }

    if (!found) return 0;

    size_t end_offset = tar_end_offset(archive);
    size_t tail_start = entry_offset + entry_total;
    size_t tail_len = end_offset - tail_start;

    memmove(base + entry_offset, base + tail_start, tail_len);

    size_t new_end = entry_offset + tail_len;
    memset(base + new_end, 0, entry_total + 1024);

    return 1;
}

int tar_dir_has_children(const void *archive, const char *dirpath) {
    const unsigned char *base = (const unsigned char *)archive;
    size_t offset = 0;

    size_t dirpath_len = 0;
    while (dirpath[dirpath_len]) dirpath_len++;

    for (;;) {
        const struct tar_header *hdr = (const struct tar_header *)(base + offset);
        if (hdr->name[0] == '\0') break;

        size_t size = octal_to_size(hdr->size, sizeof(hdr->size));
        size_t padded = ((size + 511) / 512) * 512;
        const char *name = skip_prefix(hdr->name);

        // A child looks like "dirpath/something" — must match dirpath + '/' as a prefix.
        int is_child = 1;
        for (size_t i = 0; i < dirpath_len; i++) {
            if (name[i] != dirpath[i]) { is_child = 0; break; }
        }
        if (is_child && name[dirpath_len] != '/') is_child = 0;
        if (is_child && name[dirpath_len + 1] == '\0') is_child = 0; // exclude the directory's own entry

        if (is_child) return 1;

        offset += 512 + padded;
    }
    return 0;
}

int tar_first_child(const void *archive, const char *dirpath, char *out, size_t out_size) {
    const unsigned char *base = (const unsigned char *)archive;
    size_t offset = 0;
    size_t dirpath_len = 0;
    while (dirpath[dirpath_len]) dirpath_len++;

    for (;;) {
        const struct tar_header *hdr = (const struct tar_header *)(base + offset);
        if (hdr->name[0] == '\0') break;

        size_t size = octal_to_size(hdr->size, sizeof(hdr->size));
        size_t padded = ((size + 511) / 512) * 512;
        const char *name = skip_prefix(hdr->name);

        int is_child = 1;
        for (size_t i = 0; i < dirpath_len; i++) {
            if (name[i] != dirpath[i]) { is_child = 0; break; }
        }
        if (is_child && name[dirpath_len] != '/') is_child = 0;
        if (is_child && name[dirpath_len + 1] == '\0') is_child = 0; // exclude the directory's own entry

        if (is_child) {
            size_t i = 0;
            while (name[i] && i < out_size - 1) { out[i] = name[i]; i++; }
            out[i] = '\0';
            return 1;
        }

        offset += 512 + padded;
    }
    return 0;
}