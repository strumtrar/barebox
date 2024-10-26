/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_DRIVER_H_
#define LINUX_DRIVER_H_

#include <driver.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <linux/overflow.h>
#include <mmu.h>

#define device_driver driver

typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

void *devres_open_group(struct device *dev, void *id, gfp_t gfp);

void devres_for_each_res(struct device *dev, dr_release_t release,
			dr_match_t match, void *match_data,
			void (*fn)(struct device *, void *, void *),
			void *data);

void *devres_get(struct device *dev, void *new_res,
			dr_match_t match, void *match_data);
void *devres_remove(struct device *dev, dr_release_t release,
			   dr_match_t match, void *match_data);

void *devres_find(struct device *dev, dr_release_t release,
			 dr_match_t match, void *match_data);

void devres_close_group(struct device *dev, void *id);
void devres_remove_group(struct device *dev, void *id);
int devres_release_group(struct device *dev, void *id);

#define __devm_wrapper(fn, dev, ...) ({ BUG_ON(!dev); fn(__VA_ARGS__); })

#ifdef CONFIG_DEVRES

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) __alloc_size(2);
__printf(3, 0) char *devm_kvasprintf(struct device *dev, gfp_t gfp,
				     const char *fmt, va_list ap) __malloc;
__printf(3, 4) char *devm_kasprintf(struct device *dev, gfp_t gfp,
				    const char *fmt, ...) __malloc;

static inline void *devm_kzalloc(struct device *dev,
				 size_t size, gfp_t flags)
{
	return devm_kmalloc(dev, size, flags | __GFP_ZERO);
}

static inline void *devm_kmalloc_array(struct device *dev,
				       size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;

	return devm_kmalloc(dev, bytes, flags);
}

static inline void *devm_kcalloc(struct device *dev, size_t n,
				 size_t size, gfp_t gfp)
{
	return devm_kmalloc_array(dev, n, size, gfp | __GFP_ZERO);
}

void devm_kfree(struct device *dev, const void *p);
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp) __malloc;
static inline const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp)
{
	return devm_kstrdup(dev, s, gfp);
}
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
	__realloc_size(3);

void *devm_xmalloc(struct device *dev, size_t size, gfp_t) __xalloc_size(2);

#else
#define devm_kmalloc(...)		__devm_wrapper(kmalloc, __VA_ARGS__)
#define devm_kvasprintf(...)		__devm_wrapper(kvasprintf, __VA_ARGS__)
#define devm_kasprintf(...)		__devm_wrapper(kasprintf, __VA_ARGS__)
#define devm_kzalloc(...)		__devm_wrapper(kzalloc, __VA_ARGS__)
#define devm_kmalloc_array(...)		__devm_wrapper(kmalloc_array, __VA_ARGS__)
#define devm_kcalloc(...)		__devm_wrapper(kcalloc, __VA_ARGS__)
#define devm_kfree(...)			__devm_wrapper(kfree, __VA_ARGS__)
#define devm_kstrdup(...)		__devm_wrapper(kstrdup, __VA_ARGS__)
#define devm_kstrdup_const(...)		__devm_wrapper(kstrdup_const, __VA_ARGS__)
#define devm_kmemdup(...)		__devm_wrapper(kmemdup, __VA_ARGS__)
#define devm_xmalloc(...)		__devm_wrapper(xmalloc, __VA_ARGS__)
#endif

/* TODO: add devm_ variant that isn't freed on barebox shutdown */
#define devm_bitmap_zalloc(dev, nbits, gfp)	\
	__devm_wrapper(bitmap_zalloc, dev, nbits)

static inline void *devm_xzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	return devm_xmalloc(dev, size, gfp | __GFP_ZERO);
}

#define device_register register_device
#define device_unregister unregister_device

#define driver_register register_driver
#define driver_unregister unregister_driver


static inline void __iomem *dev_platform_ioremap_resource(struct device *dev,
							  int resource)
{
	/*
	 * barebox maps everything outside the RAM banks suitably for MMIO,
	 * so we don't need to do anything besides requesting the regions
	 * and can leave the memory attributes unchanged.
	 */
	return dev_request_mem_region(dev, resource);
}

static inline void __iomem *devm_ioremap(struct device *dev,
					 resource_size_t start,
					 resource_size_t size)
{
	if (start)
		remap_range((void *)start, size, MAP_UNCACHED);

	return IOMEM(start);
}

static inline int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data,
				   int (*fn)(struct device *dev, void *data))
{
	struct device *dev;
	int ret;

	bus_for_each_device(bus, dev) {
		if (start) {
			if (dev == start)
				start = NULL;
			continue;
		}

		ret = fn(dev, data);
		if (ret)
			return ret;
	}

	return 0;
}


/**
 * dev_set_drvdata - set driver private data for device
 * @dev: device instance
 * @data: driver-specific data
 *
 * Returns private driver data or NULL if none was set.
 *
 * NOTE: This does _not_ return the match data associated with
 * the match. For that use device_get_match_data instead.
 */
static inline void dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}

/**
 * dev_get_drvdata - get driver match data associated for device
 * @dev: device instance
 * @data: driver-specific data
 *
 * Set some driver and device specific data for later retrieval
 * by dev_get_drvdata.
 *
 * NOTE: This does _not_ return the match data associated with
 * the match. For that use device_get_match_data instead.
 */
static inline void *dev_get_drvdata(const struct device *dev)
{
	return dev->driver_data;
}

#endif
