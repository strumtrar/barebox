// SPDX-License-Identifier: GPL-2.0-only
/*
 * partition.c - devicetree partition parsing
 *
 * Copyright (c) 2012 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * based on Linux devicetree support
 */
#include <common.h>
#include <of.h>
#include <malloc.h>
#include <linux/mtd/mtd.h>
#include <linux/err.h>
#include <nand.h>
#include <linux/nvmem-provider.h>
#include <init.h>
#include <globalvar.h>

/**
 * enum of_binding_name - Name of binding to use for OF partition fixup
 * @MTD_OF_BINDING_NEW:		Fix up new-style partition bindings
 *				with compatible = "fixed-partitions" container
 * @MTD_OF_BINDING_LEGACY:	Fix up legacy partition bindings
 *				directly into the parent node without container
 * @MTD_OF_BINDING_DONTTOUCH:	Don't touch partition nodes at all - no fixups
 * @MTD_OF_BINDING_ADAPTIVE:	Do a new-style fixup with compatible being either:
 *				- the same compatible as in the kernel DT if available
 *				- "fixed-partitions" for MTD
 *				- "barebox,fixed-partitions" otherwise
 */
enum of_binding_name {
	MTD_OF_BINDING_NEW,
	MTD_OF_BINDING_LEGACY,
	MTD_OF_BINDING_DONTTOUCH,
	MTD_OF_BINDING_ADAPTIVE,
};

static unsigned int of_partition_binding = MTD_OF_BINDING_ADAPTIVE;

struct cdev *of_parse_partition(struct cdev *cdev, struct device_node *node)
{
	struct devfs_partition partinfo = {};
	const char *partname;
	char *filename;
	struct cdev *new;
	const __be32 *reg;
	int len;
	int na, ns;

	if (!node)
		return NULL;

	reg = of_get_property(node, "reg", &len);
	if (!reg)
		return NULL;

	na = of_n_addr_cells(node);
	ns = of_n_size_cells(node);

	if (len < (na + ns) * sizeof(__be32)) {
		pr_err("reg property too small in %pOF\n", node);
		return NULL;
	}

	partinfo.offset = of_read_number(reg, na);
	partinfo.size = of_read_number(reg + na, ns);

	partname = of_get_property(node, "label", NULL);
	if (!partname)
		partname = of_get_property(node, "name", NULL);
	if (!partname)
		return NULL;

	debug("add partition: %s.%s 0x%08llx 0x%08llx\n", cdev->name, partname,
	      partinfo.offset, partinfo.size);

	if (of_get_property(node, "read-only", NULL))
		partinfo.flags = DEVFS_PARTITION_READONLY;

	partinfo.name = filename = basprintf("%s.%s", cdev->name, partname);

	new = cdevfs_add_partition(cdev, &partinfo);
	if (IS_ERR(new)) {
		pr_err("Adding partition %s failed: %pe\n", filename, new);
		new = NULL;
		goto out;
	}

	new->device_node = node;
	new->flags |= DEVFS_PARTITION_FROM_OF | DEVFS_PARTITION_FOR_FIXUP;

out:
	free(filename);

	return new;
}

int of_parse_partitions(struct cdev *cdev, struct device_node *node)
{
	struct device_node *n, *subnode;

	if (!node)
		return -EINVAL;

	cdev_set_of_node(cdev, node);

	subnode = of_get_child_by_name(node, "partitions");
	if (subnode) {
		if (!of_node_is_fixed_partitions(subnode))
			return -EINVAL;
		node = subnode;
	}

	for_each_child_of_node(node, n) {
		struct cdev *partcdev;
		struct device *dev;

		partcdev = of_parse_partition(cdev, n);
		if (!partcdev || !subnode)
			continue;

		/* TODO: migrate to a partition-only bus?
		 * Linux uses mtd_notifier for this
		 */
		dev = of_add_child_device(partcdev->dev, partcdev->name,
					  DEVICE_ID_SINGLE, n);
		WARN_ON(IS_ERR(dev));
	}

	return 0;
}

/**
 * of_partition_ensure_probed - ensure a parition is probed
 * @np: pointer to a partition or to a partitionable device
 *      Unfortunately, there is no completely reliable way
 *      to differentiate partitions from devices prior to
 *      probing, because partitions may also have compatibles.
 *
 * Returns zero on success or a negative error code otherwise
 */
int of_partition_ensure_probed(struct device_node *np)
{
	struct device_node *parent = of_get_parent(np);

	/* root node is not a partition */
	if (!parent)
		return -EINVAL;

	/* Check if modern partitions binding */
	if (of_node_is_fixed_partitions(parent)) {
		parent = of_get_parent(parent);

		/*
		 * Can't call of_partition_ensure_probed on root node.
		 * This catches barebox-specific partuuid binding
		 * (top-level partition node)
		 */
		if (!of_get_parent(parent))
			return -EINVAL;

		return of_device_ensure_probed(parent);
	 }

	/* Check if legacy partitions binding */
	if (!of_property_present(np, "compatible"))
		return of_device_ensure_probed(parent);

	/* Doesn't look like a partition, so let's probe directly */
	return of_device_ensure_probed(np);
}
EXPORT_SYMBOL_GPL(of_partition_ensure_probed);

static void delete_subnodes(struct device_node *np)
{
	struct device_node *part, *tmp;

	for_each_child_of_node_safe(np, tmp, part) {
		if (of_get_property(part, "compatible", NULL))
			continue;

		of_delete_node(part);
	}
}

int of_fixup_partitions(struct device_node *np, struct cdev *cdev)
{
	struct cdev *partcdev;
	struct device_node *part, *partnode;
	const char *compat = "fixed-partitions";
	int ret;
	int n_cells, n_parts = 0;

	if (of_partition_binding == MTD_OF_BINDING_DONTTOUCH)
		return 0;

	list_for_each_entry(partcdev, &cdev->partitions, partition_entry) {
		if (!(partcdev->flags & DEVFS_PARTITION_FOR_FIXUP))
			continue;
		n_parts++;
	}

	if (!n_parts)
		return 0;

	if (cdev->size >= 0x100000000)
		n_cells = 2;
	else
		n_cells = 1;

	partnode = of_get_child_by_name(np, "partitions");

	switch (of_partition_binding) {
	case MTD_OF_BINDING_LEGACY:
		of_delete_node(partnode);
		partnode = np;
		break;
	case MTD_OF_BINDING_ADAPTIVE:
		/* If there's already a fixed-partitions node, leave compatible as-is */
		if (of_node_is_fixed_partitions(partnode))
			break;
		if (!cdev->mtd)
			compat = "barebox,fixed-partitions";
		fallthrough;
	case MTD_OF_BINDING_NEW:
		partnode = partnode ?: of_new_node(np, "partitions");
		ret = of_property_write_string(partnode, "compatible", compat);
		if (ret)
			return ret;
		break;
	}

	if (partnode)
		delete_subnodes(partnode);
	else
		delete_subnodes(np);

	ret = of_property_write_u32(partnode, "#size-cells", n_cells);
	if (ret)
		return ret;

	ret = of_property_write_u32(partnode, "#address-cells", n_cells);
	if (ret)
		return ret;

	list_for_each_entry(partcdev, &cdev->partitions, partition_entry) {
		int na, ns, len = 0;
		char *name;
		void *p;
		u8 tmp[16 * 16]; /* Up to 64-bit address + 64-bit size */
		loff_t partoffset;

		if (!(partcdev->flags & DEVFS_PARTITION_FOR_FIXUP))
			continue;

		if (partcdev->mtd)
			partoffset = partcdev->mtd->master_offset;
		else
			partoffset = partcdev->offset;

		name = basprintf("partition@%0llx", partoffset);
		if (!name)
			return -ENOMEM;

		part = of_new_node(partnode, name);
		free(name);
		if (!part)
			return -ENOMEM;

		p = of_new_property(part, "label", partcdev->partname,
                                strlen(partcdev->partname) + 1);
		if (!p)
			return -ENOMEM;

		na = of_n_addr_cells(part);
		ns = of_n_size_cells(part);

		of_write_number(tmp + len, partoffset, na);
		len += na * 4;
		of_write_number(tmp + len, partcdev->size, ns);
		len += ns * 4;

		ret = of_set_property(part, "reg", tmp, len, 1);
		if (ret)
			return ret;

		if (partcdev->flags & DEVFS_PARTITION_READONLY) {
			ret = of_set_property(part, "read-only", NULL, 0, 1);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int of_partition_fixup(struct device_node *root, void *ctx)
{
	struct cdev *cdev = ctx;
	struct device_node *cdev_np, *np;
	char *name;

	cdev_np = cdev_of_node(cdev);
	if (!cdev_np)
		return -EINVAL;

	if (list_empty(&cdev->partitions))
		return 0;

	name = of_get_reproducible_name(cdev_np);
	np = of_find_node_by_reproducible_name(root, name);
	free(name);
	if (!np) {
		dev_err(cdev->dev, "Cannot find nodepath %pOF, cannot fixup\n", cdev_np);
		return -EINVAL;
	}

	return of_fixup_partitions(np, cdev);
}

int of_partitions_register_fixup(struct cdev *cdev)
{
	return of_register_fixup(of_partition_fixup, cdev);
}

static const char *of_binding_names[] = {
	"new", "legacy", "donttouch", "adaptive"
};

static int of_partition_init(void)
{
	if (IS_ENABLED(CONFIG_GLOBALVAR))
		dev_add_param_enum(&global_device, "of_partition_binding",
				   NULL, NULL,
				   &of_partition_binding, of_binding_names,
				   ARRAY_SIZE(of_binding_names), NULL);

	return 0;
}
device_initcall(of_partition_init);
