/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2012 Marc Kleine-Budde <mkl@pengutronix.de> */

#include <dma.h>
#include <asm/io.h>

static inline dma_addr_t cpu_to_dma(struct device *dev, void *cpu_addr)
{
	if (dev && dev->dma_offset)
		return (unsigned long)cpu_addr - dev->dma_offset;

	return virt_to_phys(cpu_addr);
}

static inline void *dma_to_cpu(struct device *dev, dma_addr_t addr)
{
	if (dev && dev->dma_offset)
		return (void *)(addr + dev->dma_offset);

	return phys_to_virt(addr);
}

dma_addr_t dma_map_single(struct device *dev, void *ptr, size_t size,
			  enum dma_data_direction dir)
{
	unsigned long addr = (unsigned long)ptr;

	dma_sync_single_for_device(addr, size, dir);

	return cpu_to_dma(dev, ptr);
}

void dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size,
		      enum dma_data_direction dir)
{
	unsigned long addr = (unsigned long)dma_to_cpu(dev, dma_addr);

	dma_sync_single_for_cpu(addr, size, dir);
}
