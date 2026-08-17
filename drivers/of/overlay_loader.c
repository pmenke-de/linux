// SPDX-License-Identifier: GPL-2.0
/*
 * Load device tree overlays from userspace, for development.
 *
 * The overlay core has no userspace interface of its own, so bringing up a
 * board means rebuilding and reflashing the whole DTB for every one-line
 * change. This exposes just enough of it through debugfs to iterate without
 * rebooting:
 *
 *   echo camera-lanes.dtbo > /sys/kernel/debug/of_overlay_loader/apply
 *   cat /sys/kernel/debug/of_overlay_loader/list
 *   echo 1 > /sys/kernel/debug/of_overlay_loader/remove
 *
 * Overlays are read with the firmware loader, so they live in /lib/firmware.
 *
 * Applying an overlay does not re-probe drivers that are already bound to the
 * nodes it changes; unbind and rebind them, or reload their module, for the
 * new properties to be read.
 */

#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#define OVERLAY_LOADER_MAX_NAME	128

struct overlay_entry {
	struct list_head node;
	char name[OVERLAY_LOADER_MAX_NAME];
	int id;
};

static LIST_HEAD(overlay_list);
static DEFINE_MUTEX(overlay_lock);
static struct dentry *overlay_dir;

static ssize_t overlay_apply_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	const struct firmware *fw;
	struct overlay_entry *entry;
	char name[OVERLAY_LOADER_MAX_NAME];
	int id, ret;

	if (count == 0 || count >= sizeof(name))
		return -EINVAL;

	if (copy_from_user(name, buf, count))
		return -EFAULT;

	name[count] = '\0';
	strim(name);

	ret = request_firmware(&fw, name, NULL);
	if (ret) {
		pr_err("of_overlay_loader: cannot read %s: %d\n", name, ret);
		return ret;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		release_firmware(fw);
		return -ENOMEM;
	}

	ret = of_overlay_fdt_apply(fw->data, fw->size, &id, NULL);
	release_firmware(fw);
	if (ret) {
		pr_err("of_overlay_loader: cannot apply %s: %d\n", name, ret);
		kfree(entry);
		return ret;
	}

	strscpy(entry->name, name, sizeof(entry->name));
	entry->id = id;

	mutex_lock(&overlay_lock);
	list_add_tail(&entry->node, &overlay_list);
	mutex_unlock(&overlay_lock);

	pr_info("of_overlay_loader: applied %s as id %d\n", name, id);

	return count;
}

static const struct file_operations overlay_apply_fops = {
	.owner = THIS_MODULE,
	.write = overlay_apply_write,
	.llseek = noop_llseek,
};

static ssize_t overlay_remove_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct overlay_entry *entry, *tmp;
	int id, ret;

	ret = kstrtoint_from_user(buf, count, 0, &id);
	if (ret)
		return ret;

	ret = of_overlay_remove(&id);
	if (ret) {
		pr_err("of_overlay_loader: cannot remove id %d: %d\n", id, ret);
		return ret;
	}

	mutex_lock(&overlay_lock);
	list_for_each_entry_safe(entry, tmp, &overlay_list, node) {
		if (entry->id == id) {
			list_del(&entry->node);
			kfree(entry);
		}
	}
	mutex_unlock(&overlay_lock);

	pr_info("of_overlay_loader: removed id %d\n", id);

	return count;
}

static const struct file_operations overlay_remove_fops = {
	.owner = THIS_MODULE,
	.write = overlay_remove_write,
	.llseek = noop_llseek,
};

static int overlay_list_show(struct seq_file *s, void *data)
{
	struct overlay_entry *entry;

	mutex_lock(&overlay_lock);
	list_for_each_entry(entry, &overlay_list, node)
		seq_printf(s, "%d\t%s\n", entry->id, entry->name);
	mutex_unlock(&overlay_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(overlay_list);

static int __init overlay_loader_init(void)
{
	overlay_dir = debugfs_create_dir("of_overlay_loader", NULL);

	debugfs_create_file("apply", 0200, overlay_dir, NULL,
			    &overlay_apply_fops);
	debugfs_create_file("remove", 0200, overlay_dir, NULL,
			    &overlay_remove_fops);
	debugfs_create_file("list", 0400, overlay_dir, NULL,
			    &overlay_list_fops);

	return 0;
}

static void __exit overlay_loader_exit(void)
{
	struct overlay_entry *entry, *tmp;

	debugfs_remove_recursive(overlay_dir);

	/*
	 * Overlays are left applied on unload: removing them here would tear
	 * down devices that are in use, and the changesets can only come off
	 * in reverse order anyway.
	 */
	mutex_lock(&overlay_lock);
	list_for_each_entry_safe(entry, tmp, &overlay_list, node) {
		list_del(&entry->node);
		kfree(entry);
	}
	mutex_unlock(&overlay_lock);
}

module_init(overlay_loader_init);
module_exit(overlay_loader_exit);

MODULE_AUTHOR("Roy Kaandorp <roykaandorp@gmail.com>");
MODULE_DESCRIPTION("Load device tree overlays from userspace");
MODULE_LICENSE("GPL");
