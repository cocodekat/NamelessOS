// ata.c
#include "ata.h"

#define ATA_IO_BASE 0x1F0

#define ATA_REG_DATA     (ATA_IO_BASE + 0)
#define ATA_REG_SECCOUNT (ATA_IO_BASE + 2)
#define ATA_REG_LBA_LOW  (ATA_IO_BASE + 3)
#define ATA_REG_LBA_MID  (ATA_IO_BASE + 4)
#define ATA_REG_LBA_HIGH (ATA_IO_BASE + 5)
#define ATA_REG_DRIVE    (ATA_IO_BASE + 6)
#define ATA_REG_STATUS   (ATA_IO_BASE + 7)
#define ATA_REG_COMMAND  (ATA_IO_BASE + 7)

#define ATA_SR_ERR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_BSY 0x80

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xE7

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    asm volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void ata_wait_not_busy(void) {
    while (inb(ATA_REG_STATUS) & ATA_SR_BSY);
}

// Waits until the drive has a sector ready to transfer. Returns 0 if the
// drive reported an error instead.
static int ata_wait_drq(void) {
    for (;;) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) return 1;
    }
}

static void ata_setup_lba(uint32_t lba, uint8_t count) {
    outb(ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F)); // master drive, LBA mode
    outb(ATA_REG_SECCOUNT, count);
    outb(ATA_REG_LBA_LOW, (uint8_t)(lba));
    outb(ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
}

// A single ATA PIO command's sector-count register is 8 bits, and 0 means
// "256" — we just cap each command well below that so we never have to
// think about the edge case, and loop for anything bigger.
#define ATA_MAX_SECTORS_PER_CMD 128

int ata_read(uint32_t lba, uint32_t sector_count, void *buffer) {
    /*uint16_t *buf = (uint16_t *)buffer;

    while (sector_count > 0) {
        uint8_t chunk = (sector_count > ATA_MAX_SECTORS_PER_CMD)
            ? ATA_MAX_SECTORS_PER_CMD : (uint8_t)sector_count;

        ata_wait_not_busy();
        ata_setup_lba(lba, chunk);
        outb(ATA_REG_COMMAND, ATA_CMD_READ);

        for (uint8_t s = 0; s < chunk; s++) {
            if (!ata_wait_drq()) return 0;
            for (int i = 0; i < 256; i++) {
                buf[i] = inw(ATA_REG_DATA);
            }
            buf += 256;
        }

        lba += chunk;
        sector_count -= chunk;
    }
    return 1;*/
    (void)lba;
    (void)sector_count;
    (void)buffer;
    return 0;
}

int ata_write(uint32_t lba, uint32_t sector_count, const void *buffer) {
    /*const uint16_t *buf = (const uint16_t *)buffer;

    while (sector_count > 0) {
        uint8_t chunk = (sector_count > ATA_MAX_SECTORS_PER_CMD)
            ? ATA_MAX_SECTORS_PER_CMD : (uint8_t)sector_count;

        ata_wait_not_busy();
        ata_setup_lba(lba, chunk);
        outb(ATA_REG_COMMAND, ATA_CMD_WRITE);

        for (uint8_t s = 0; s < chunk; s++) {
            if (!ata_wait_drq()) return 0;
            for (int i = 0; i < 256; i++) {
                outw(ATA_REG_DATA, buf[i]);
            }
            buf += 256;
        }

        lba += chunk;
        sector_count -= chunk;
    }

    // Force the writes to actually hit the backing image/disk.
    ata_wait_not_busy();
    outb(ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_wait_not_busy();

    return 1;*/
    (void)lba;
    (void)sector_count;
    (void)buffer;
    return 0;
}