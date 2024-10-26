/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/types.h>

struct device;

#ifdef CONFIG_DEVRES
int devres_release_all(struct device *dev, bool shutdown);
#else
static inline int devres_release_all(struct device *dev, bool shutdown)
{
	return 0;
}
#endif

int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name);
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);

void devm_release_action(struct device *dev, void (*action)(void *), void *data);
