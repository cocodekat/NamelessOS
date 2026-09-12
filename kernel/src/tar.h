#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include <stddef.h>

size_t tar_used_size(const void *archive);

// Returns a pointer to the file's data, and sets *out_size, or NULL if not found.
const void *tar_find(const void *archive, const char *path, size_t *out_size);

// Calls the callback once per file entry found in the archive.
void tar_list(const void *archive, void (*callback)(const char *name, size_t size));

int tar_append_file(void *archive, size_t capacity, const char *path, const void *data, size_t size);
int tar_append_dir(void *archive, size_t capacity, const char *path);
int tar_remove(void *archive, const char *path);
int tar_dir_has_children(const void *archive, const char *dirpath);
int tar_first_child(const void *archive, const char *dirpath, char *out, size_t out_size);

#endif