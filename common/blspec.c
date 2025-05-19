// SPDX-License-Identifier: GPL-2.0-or-later
#define pr_fmt(fmt)  "blspec: " fmt

#include <environment.h>
#include <globalvar.h>
#include <firmware.h>
#include <malloc.h>
#include <fcntl.h>
#include <libfile.h>
#include <libbb.h>
#include <init.h>
#include <bootm.h>
#include <glob.h>
#include <fs.h>
#include <of.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/err.h>
#include <uapi/spec/dps.h>
#include <boot.h>

#include <bootscan.h>

struct blspec_entry {
	struct bootentry entry;

	struct device_node *node;
	struct cdev *cdev;
	const char *rootpath;
	const char *configpath;
};

/*
 * blspec_entry_var_get - get the value of a variable
 */
static const char *blspec_entry_var_get(struct blspec_entry *entry, const char *name)
{
	const char *str;
	int ret;

	ret = of_property_read_string(entry->node, name, &str);

	return ret ? NULL : str;
}

/*
 * blspec_entry_var_set - set a variable to a value
 */
static int blspec_entry_var_set(struct blspec_entry *entry, const char *name,
		const char *val)
{
	return of_set_property(entry->node, name, val,
			val ? strlen(val) + 1 : 0, 1);
}

static int blspec_overlay_fixup(struct device_node *root, void *ctx)
{
	struct blspec_entry *entry = ctx;
	const char *overlays;
	char *overlay;
	char *sep, *freep;

	overlays = blspec_entry_var_get(entry, "devicetree-overlay");

	sep = freep = xstrdup(overlays);

	while ((overlay = strsep(&sep, " "))) {
		char *path;

		if (!*overlay)
			continue;

		path = basprintf("%s/%s", entry->rootpath, overlay);

		of_overlay_apply_file(root, path, false);

		free(path);
	}

	free(freep);

	return 0;
}

/*
 * blspec_boot - boot an entry
 *
 * This boots an entry. On success this function does not return.
 * In case of an error the error code is returned. This function may
 * return 0 in case of a successful dry run.
 */
static int blspec_boot(struct bootentry *be, int verbose, int dryrun)
{
	struct blspec_entry *entry = container_of(be, struct blspec_entry, entry);
	int ret;
	const char *abspath, *devicetree, *options, *initrd, *linuximage;
	const char *overlays;
	const char *appendroot;
	char *old_fws, *fws;
	struct bootm_data data = {};

	bootm_data_init_defaults(&data);
	data.dryrun = max_t(int, dryrun, data.dryrun);
	data.os_file = data.oftree_file = data.initrd_file = NULL;
	data.verbose = max(verbose, data.verbose);

	devicetree = blspec_entry_var_get(entry, "devicetree");
	initrd = blspec_entry_var_get(entry, "initrd");
	options = blspec_entry_var_get(entry, "options");
	linuximage = blspec_entry_var_get(entry, "linux");
	overlays = blspec_entry_var_get(entry, "devicetree-overlay");

	if (entry->rootpath)
		abspath = entry->rootpath;
	else
		abspath = "";

	data.os_file = basprintf("%s/%s", abspath, linuximage);

	if (devicetree)
		data.oftree_file = basprintf("%s/%s", abspath, devicetree);

	if (overlays)
		of_register_fixup(blspec_overlay_fixup, entry);

	if (initrd)
		data.initrd_file = basprintf("%s/%s", abspath, initrd);

	globalvar_add_simple("linux.bootargs.dyn.bootentries", options);

	appendroot = blspec_entry_var_get(entry, "linux-appendroot");
	if (appendroot) {
		int val;

		ret = strtobool(appendroot, &val);
		if (ret) {
			pr_err("Invalid value \"%s\" for appendroot option\n",
			       appendroot);
			goto err_out;
		}
		data.appendroot = val;
	}

	pr_info("booting %s from %s\n", blspec_entry_var_get(entry, "title"),
			(entry->cdev && entry->cdev->dev) ?
			dev_name(entry->cdev->dev) : "none");

	of_overlay_set_basedir(abspath);

	old_fws = firmware_get_searchpath();
	if (old_fws && *old_fws)
		fws = basprintf("%s/lib/firmware:%s", abspath, old_fws);
	else
		fws = basprintf("%s/lib/firmware", abspath);
	firmware_set_searchpath(fws);
	free(fws);

	ret = bootm_boot(&data);
	if (ret)
		pr_err("Booting failed\n");

	if (overlays)
		of_unregister_fixup(blspec_overlay_fixup, entry);

	of_overlay_set_basedir("/");
	firmware_set_searchpath(old_fws);
	free(old_fws);

err_out:
	free((char *)data.oftree_file);
	free((char *)data.initrd_file);
	free((char *)data.os_file);

	return ret;
}

static void blspec_entry_free(struct bootentry *be)
{
	struct blspec_entry *entry = container_of(be, struct blspec_entry, entry);

	of_delete_node(entry->node);
	free_const(entry->configpath);
	free_const(entry->rootpath);
	free(entry);
}

static struct blspec_entry *blspec_entry_alloc(struct bootentries *bootentries)
{
	struct blspec_entry *entry;

	entry = xzalloc(sizeof(*entry));

	entry->node = of_new_node(NULL, NULL);
	entry->entry.release = blspec_entry_free;
	entry->entry.boot = blspec_boot;

	return entry;
}

/*
 * blspec_entry_open - open an entry given a path
 */
static struct blspec_entry *blspec_entry_open(struct bootentries *bootentries,
		const char *abspath)
{
	struct blspec_entry *entry;
	char *end, *line, *next;
	char *buf;

	pr_debug("%s: %s\n", __func__, abspath);

	buf = read_file(abspath, NULL);
	if (!buf)
		return ERR_PTR(-errno);

	entry = blspec_entry_alloc(bootentries);

	next = buf;

	while (next && *next) {
		char *name, *val;

		line = next;

		next = strchr(line, '\n');
		if (next) {
			*next = 0;
			next++;
		}

		if (*line == '#')
			continue;

		name = line;
		end = name;

		while (*end && (*end != ' ' && *end != '\t'))
			end++;

		if (!*end) {
			blspec_entry_var_set(entry, name, NULL);
			continue;
		}

		*end = 0;

		end++;

		while (*end == ' ' || *end == '\t')
			end++;

		if (!*end) {
			blspec_entry_var_set(entry, name, NULL);
			continue;
		}

		val = end;

		if (!strcmp(name, "options")) {
			/* If there was a previous "options" key given, prepend its value
			 * (as per spec). */
			const char *prev_val = blspec_entry_var_get(entry, name);
			if (prev_val) {
				char *opts = xasprintf("%s %s", prev_val, val);
				blspec_entry_var_set(entry, name, opts);
				free(opts);
				continue;
			}
		}

		blspec_entry_var_set(entry, name, val);
	}

	free(buf);

	return entry;
}

/*
 * is_blspec_entry - check if a bootentry is a blspec entry
 */
static inline bool is_blspec_entry(struct bootentry *entry)
{
	return entry->boot == blspec_boot;
}

/*
 * blspec_have_entry - check if we already have an entry with
 *                     a certain path
 */
static int blspec_have_entry(struct bootentries *bootentries, const char *path)
{
	struct bootentry *be;
	struct blspec_entry *e;

	list_for_each_entry(be, &bootentries->entries, list) {
		if (!is_blspec_entry(be))
			continue;
		e = container_of(be, struct blspec_entry, entry);
		if (e->configpath && !strcmp(e->configpath, path))
			return 1;
	}

	return 0;
}

/*
 * entry_is_of_compatible - check if a bootspec entry is compatible with
 *                          the current machine.
 *
 * returns true if the entry is compatible, false otherwise
 */
static bool entry_is_of_compatible(struct blspec_entry *entry)
{
	const char *devicetree;
	const char *abspath;
	int compatlen, ret;
	struct device_node *barebox_root;
	size_t size;
	void *fdt;
	const char *machine;
	char *filename;
	const char *compats;

	/* If the entry doesn't specify a devicetree we are compatible */
	devicetree = blspec_entry_var_get(entry, "devicetree");
	if (!devicetree)
		return true;

	if (!strcmp(devicetree, "none"))
		return true;

	/* If we don't have a root node every entry is compatible */
	barebox_root = of_get_root_node();
	if (!barebox_root)
		return true;

	ret = of_property_read_string(barebox_root, "compatible", &machine);
	if (ret)
		return false;

	if (entry->rootpath)
		abspath = entry->rootpath;
	else
		abspath = "";

	filename = basprintf("%s/%s", abspath, devicetree);

	fdt = read_file(filename, &size);
	if (!fdt) {
		ret = false;
		goto out;
	}

	compats = fdt_machine_get_compatible(fdt, size, &compatlen);
	if (compats && fdt_string_is_compatible(compats, compatlen, machine)) {
		ret = true;
		goto out;
	}

	pr_info("ignoring entry with incompatible devicetree: %s\n", devicetree);

	ret = false;

out:
	free(fdt);
	free(filename);

	return ret;
}

/*
 * entry_is_match_machine_id - check if a bootspec entry is match with
 *                            the machine id given by global variable.
 *
 * returns true if the entry is match, false otherwise
 */

static bool entry_is_match_machine_id(struct blspec_entry *entry)
{
	int ret = true;
	const char *env_machineid = getenv_nonempty("global.boot.machine_id");

	if (env_machineid) {
		const char *machineid = blspec_entry_var_get(entry, "machine-id");
		if (!machineid || strcmp(machineid, env_machineid)) {
			pr_debug("ignoring entry with mismatched machine-id " \
				"\"%s\" != \"%s\"\n", env_machineid, machineid);
			ret = false;
		}
	}

	return ret;
}

static int __blspec_scan_file(struct bootentries *bootentries, const char *root,
			      const char *configname)
{
	char *devname = NULL, *hwdevname = NULL;
	struct blspec_entry *entry;

	if (blspec_have_entry(bootentries, configname))
		return -EEXIST;

	entry = blspec_entry_open(bootentries, configname);
	if (IS_ERR(entry))
		return PTR_ERR(entry);

	root = root ?: get_mounted_path(configname);
	entry->rootpath = xstrdup_const(root);
	entry->configpath = xstrdup_const(configname);
	entry->cdev = get_cdev_by_mountpath(root);

	if (!entry_is_of_compatible(entry)) {
		blspec_entry_free(&entry->entry);
		return -ENODEV;
	}

	if (!entry_is_match_machine_id(entry)) {
		blspec_entry_free(&entry->entry);
		return -ENODEV;
	}

	if (entry->cdev && entry->cdev->dev) {
		devname = xstrdup(dev_name(entry->cdev->dev));
		if (entry->cdev->dev->parent)
			hwdevname = xstrdup(dev_name(entry->cdev->dev->parent));
	}

	entry->entry.title = xasprintf("%s (%s)", blspec_entry_var_get(entry, "title"),
				       configname);
	entry->entry.description = basprintf("blspec entry, device: %s hwdevice: %s",
					    devname ? devname : "none",
					    hwdevname ? hwdevname : "none");
	free(devname);
	free(hwdevname);

	entry->entry.me.type = MENU_ENTRY_NORMAL;
	entry->entry.release = blspec_entry_free;

	bootentries_add_entry(bootentries, &entry->entry);
	return 1;
}

static int blspec_scan_file(struct bootscanner *scanner,
			    struct bootentries *bootentries,
			    const char *configname)
{
	if (!strends(configname, ".conf"))
		return 0;

	return __blspec_scan_file(bootentries, NULL, configname);
}

/*
 * blspec_scan_directory - scan over a directory
 *
 * Given a root path collects all bootentries entries found under /bootentries/entries/.
 *
 * returns the number of entries found or a negative error value otherwise.
 */
static int blspec_scan_directory(struct bootscanner *bootscanner,
				 struct bootentries *bootentries, const char *root)
{
	glob_t globb;
	char *abspath;
	int ret, found = 0;
	const char *dirname = "loader/entries";
	int i;

	pr_debug("%s: %s %s\n", __func__, root, dirname);

	abspath = basprintf("%s/%s/*.conf", root, dirname);

	ret = glob(abspath, 0, NULL, &globb);
	if (ret) {
		pr_debug("%s: %s: %m\n", __func__, abspath);
		ret = -errno;
		goto err_out;
	}

	for (i = 0; i < globb.gl_pathc; i++) {
		const char *configname = globb.gl_pathv[i];
		struct stat s;

		ret = stat(configname, &s);
		if (ret || !S_ISREG(s.st_mode))
			continue;

		ret = __blspec_scan_file(bootentries, root, configname);
		if (ret > 0)
			found += ret;
	}

	ret = found;

	globfree(&globb);
err_out:
	free(abspath);

	return ret;
}

static int blspec_scan_disk(struct bootscanner *scanner,
			    struct bootentries *bootentries, struct cdev *cdev)
{
	struct cdev *partcdev;
	int ret, err = 0, found = 0;
	bool have_esp = false;

	for_each_cdev_partition(partcdev, cdev) {
		struct cdev *match = NULL;

		/*
		 * If the OS is installed on a disk with:
		 *
		 * - MBR disk label, and a partition with the MBR type id of 0xEA
		 *   already exists
		 *
		 * - GPT disk label, and a partition with the GPT type GUID of
		 *   bc13c2ff-59e6-4262-a352-b275fd6f7172 already exists
		 *
		 * - an EFI System Partition and none of the above
		 *
		 * it should be used as $BOOT
		 */
		if (partcdev->flags & DEVFS_PARTITION_BOOTABLE_ESP)
			have_esp = true;
		else if (cdev_is_mbr_partitioned(cdev)) {
			if (partcdev->dos_partition_type == 0xea)
				match = partcdev;
		} else if (cdev_is_gpt_partitioned(cdev)) {
			if (guid_equal(&partcdev->typeuuid, &SD_GPT_XBOOTLDR))
				match = partcdev;
		}

		if (!match)
			continue;

		ret = boot_scan_cdev(scanner, bootentries, partcdev, true);
		if (ret > 0)
			found += ret;
		else
			err = ret ?: -ENOENT;
	}

	if (!have_esp)
		goto out;

	for_each_cdev_partition(partcdev, cdev) {
		if (!(partcdev->flags & DEVFS_PARTITION_BOOTABLE_ESP))
			continue;

		/*
		 * ESP is only a fallback. If we have an ESP, but no bootloader spec
		 * files inside, this is not an error.
		 */
		ret = boot_scan_cdev(scanner, bootentries, partcdev, true);
		if (ret >= 0)
			found += ret;
		else
			err = ret;
	}

out:
	return found ?: err;
}

/*
 * blspec_scan_device - scan a device for child cdevs
 *
 * Given a device this functions scans over all child cdevs looking
 * for bootentries entries.
 * Returns the number of entries found or a negative error code if some unexpected
 * error occurred.
 */
static int blspec_scan_device(struct bootscanner *scanner,
			      struct bootentries *bootentries, struct device *dev)
{
	struct device *child;
	struct cdev *cdev;
	int ret, found = 0;

	pr_debug("%s: %s\n", __func__, dev_name(dev));

	/* Try child devices */
	device_for_each_child(dev, child) {
		ret = blspec_scan_device(scanner, bootentries, child);
		if (ret > 0)
			return ret;
	}

	/*
	 * As a last resort try all cdevs (Not only the ones explicitly stated
	 * by the bootblspec spec).
	 */
	list_for_each_entry(cdev, &dev->cdevs, devices_list) {
		ret = boot_scan_cdev(scanner, bootentries, cdev, true);
		if (ret > 0)
			found += ret;
	}

	return found;
}

static struct bootscanner blspec_scanner = {
	.name		= "blspec",
	.scan_file	= blspec_scan_file,
	.scan_directory	= blspec_scan_directory,
	.scan_disk	= blspec_scan_disk,
	.scan_device	= blspec_scan_device,
};

static int blspec_bootentry_generate(struct bootentries *bootentries,
				     const char *name)
{
	return bootentry_scan_generate(&blspec_scanner, bootentries, name);
}

static struct bootentry_provider blspec_bootentry_provider = {
	.generate = blspec_bootentry_generate,
};

static int blspec_init(void)
{
	return bootentry_register_provider(&blspec_bootentry_provider);
}
device_initcall(blspec_init);
