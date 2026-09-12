// ata.h
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>

// Reads `sector_count` 512-byte sectors starting at `lba` from the primary
// ATA master drive into `buffer`. Returns 1 on success, 0 on failure.
int ata_read(uint32_t lba, uint32_t sector_count, void *buffer);

// Writes `sector_count` 512-byte sectors starting at `lba` to the primary
// ATA master drive from `buffer`, flushing the drive's cache afterward.
// Returns 1 on success, 0 on failure.
int ata_write(uint32_t lba, uint32_t sector_count, const void *buffer);

#endif