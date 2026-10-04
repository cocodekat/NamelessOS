#ifndef IOMMU_H
#define IOMMU_H

/*
 * Take DMA ownership from firmware before enabling PCI bus masters.
 * myOS does not have an IOMMU domain manager yet, so any firmware-left
 * translation or protected-memory ranges must be disabled.
 */
void iommu_disable_firmware_dma_protection(void);

#endif
