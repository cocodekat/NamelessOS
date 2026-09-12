#ifndef FS_H
#define FS_H

#include <stddef.h>

// Copies initial_data (the read-only tar from Limine) into a writable
// heap buffer with extra room to grow. If a previously-saved filesystem is
// found on the storage disk, that's loaded instead of initial_data.
void fs_init(const void *initial_data, size_t initial_size);

void *fs_get_archive(void);
size_t fs_get_capacity(void);

// Persists the current in-memory archive to disk so it survives a reboot.
// Returns 1 on success, 0 on failure.
int fs_sync(void);

#endif