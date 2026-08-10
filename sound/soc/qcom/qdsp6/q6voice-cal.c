// SPDX-License-Identifier: GPL-2.0
/*
 * Voice calibration for the ADSP.
 *
 * The vocproc will accept a call without any calibration and run it silently:
 * every command succeeds, and the only visible complaint is that asking for a
 * volume step fails, because the ADSP has no table to look the step up in.
 * The tables come from the calibration database the board shipped with, which
 * lives in userspace, so they arrive here as firmware.
 *
 * Handing them over is a two step affair. The ADSP is first lent the memory --
 * by physical address, through a table describing the blocks, which is why
 * there are two allocations below -- and answers with a handle. Individual
 * commands then refer to the calibration by that handle plus an address and a
 * length, so the same block can back several registrations.
 *
 * The blob itself is a sequence of parameters the ADSP interprets; the kernel
 * only has to place it somewhere the ADSP can read and describe where it is.
 * See tools/remoteproc/acdb_voice_cal.py for where it comes from.
 */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "q6mvm.h"
#include "q6voice-cal.h"

/* Header the extraction tool puts in front of the calibration. */
#define Q6VOICE_CAL_MAGIC	0x43563651	/* "Q6VC" */
#define Q6VOICE_CAL_VERSION	1

struct q6voice_cal_file_hdr {
	__le32 magic;
	__le32 version;
	__le32 col_size;	/* column description, then... */
	__le32 data_size;	/* ...the calibration itself */
};

struct q6voice_cal {
	struct device *dev;

	void *table;		/* block list the ADSP reads */
	phys_addr_t table_phys;

	void *data;		/* the calibration blocks themselves */
	phys_addr_t data_phys;
	u32 data_size;

	void *col_info;
	u32 col_size;

	u32 mem_handle;
};

/*
 * What the ADSP expects to find at the address given to
 * VSS_IMEMORY_CMD_MAP_PHYSICAL: a descriptor for a further table (unused, so
 * zeroed) followed by the blocks being lent.
 */
struct q6voice_cal_table {
	__le32 next_lsw;
	__le32 next_msw;
	__le32 next_size;
	__le64 block_addr;
	__le32 block_size;
} __packed;

void q6voice_cal_free(struct q6voice_cal *cal)
{
	if (!cal)
		return;

	if (cal->mem_handle) {
		int ret = q6mvm_unmap_memory(cal->mem_handle);

		if (ret)
			dev_warn(cal->dev, "failed to unmap calibration: %d\n",
				 ret);
	}

	if (cal->table)
		free_pages_exact(cal->table, PAGE_SIZE);
	if (cal->data)
		free_pages_exact(cal->data, cal->data_size);
	kfree(cal->col_info);
	kfree(cal);
}
EXPORT_SYMBOL_GPL(q6voice_cal_free);

/*
 * Load the calibration for @name and lend it to the ADSP. Returns NULL when
 * there is none installed, which is not an error: calls still work, they are
 * just stuck at whatever volume the vocproc defaults to.
 */
struct q6voice_cal *q6voice_cal_load(struct device *dev, const char *name)
{
	const struct q6voice_cal_file_hdr *hdr;
	struct q6voice_cal_table *table;
	const struct firmware *fw;
	struct q6voice_cal *cal;
	u32 col_size, data_size;
	int ret;

	ret = firmware_request_nowarn(&fw, name, dev);
	if (ret)
		return NULL;

	if (fw->size < sizeof(*hdr)) {
		dev_err(dev, "%s: too short for a header\n", name);
		goto err_release;
	}

	hdr = (const struct q6voice_cal_file_hdr *)fw->data;
	if (le32_to_cpu(hdr->magic) != Q6VOICE_CAL_MAGIC ||
	    le32_to_cpu(hdr->version) != Q6VOICE_CAL_VERSION) {
		dev_err(dev, "%s: not calibration this driver understands\n",
			name);
		goto err_release;
	}

	col_size = le32_to_cpu(hdr->col_size);
	data_size = le32_to_cpu(hdr->data_size);
	if (sizeof(*hdr) + col_size + data_size > fw->size) {
		dev_err(dev, "%s: truncated\n", name);
		goto err_release;
	}

	cal = kzalloc(sizeof(*cal), GFP_KERNEL);
	if (!cal)
		goto err_release;

	cal->dev = dev;
	cal->col_size = col_size;
	cal->data_size = data_size;

	cal->col_info = kmemdup(fw->data + sizeof(*hdr), col_size, GFP_KERNEL);
	if (!cal->col_info)
		goto err_free;

	/*
	 * The ADSP reads both of these by physical address, so they must be
	 * physically contiguous -- no vmalloc -- and page aligned, which is the
	 * alignment the mapping command declares.
	 */
	cal->data = alloc_pages_exact(data_size, GFP_KERNEL);
	if (!cal->data)
		goto err_free;

	memcpy(cal->data, fw->data + sizeof(*hdr) + col_size, data_size);
	cal->data_phys = virt_to_phys(cal->data);

	cal->table = alloc_pages_exact(PAGE_SIZE, GFP_KERNEL | GFP_DMA32 |
					__GFP_ZERO);
	if (!cal->table)
		goto err_free;

	cal->table_phys = virt_to_phys(cal->table);

	table = cal->table;
	table->block_addr = cpu_to_le64(cal->data_phys);
	table->block_size = cpu_to_le32(data_size);

	ret = q6mvm_map_memory(cal->table_phys, sizeof(*table),
			       &cal->mem_handle);
	if (ret) {
		dev_err(dev, "failed to lend calibration to the ADSP: %d\n",
			ret);
		goto err_free;
	}

	dev_info(dev, "calibration %s: %u bytes, handle %#x\n",
		 name, data_size, cal->mem_handle);

	release_firmware(fw);
	return cal;

err_free:
	cal->mem_handle = 0;
	q6voice_cal_free(cal);
err_release:
	release_firmware(fw);
	return NULL;
}
EXPORT_SYMBOL_GPL(q6voice_cal_load);

int q6voice_cal_register_vol(struct q6voice_cal *cal,
			     struct q6voice_session *cvp)
{
	if (!cal)
		return 0;

	return q6cvp_register_vol_cal(cvp, cal->mem_handle, cal->data_phys,
				      cal->data_size, cal->col_info,
				      cal->col_size);
}
EXPORT_SYMBOL_GPL(q6voice_cal_register_vol);

MODULE_DESCRIPTION("Q6Voice calibration");
MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE("qcom/q6voice-vol-cal.bin");
