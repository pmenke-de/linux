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
 * The memory is lent through the MVM session, not to the audio service at
 * large: a memory command addressed to the service belongs to nothing, and the
 * ADSP answers no further commands at all afterwards.
 *
 * Handing them over is a two step affair. The ADSP is first lent the memory --
 * through a table describing the blocks, which is why there are two
 * allocations below -- and answers with a handle. Individual commands then
 * refer to the calibration by that handle plus an address and a length, so the
 * same block can back several registrations.
 *
 * Whatever the ADSP is handed has to be memory it can actually reach, and that
 * has proven to be the hard part. It is allocated from a reserved pool of its
 * own rather than from ordinary kernel pages, which is how the vendor arranges
 * every buffer it shares with the DSP. The addresses are logged before the
 * command goes out: when this goes wrong the board resets without the kernel
 * being involved, so anything not already printed is lost.
 *
 * The blob itself is a sequence of parameters the ADSP interprets; the kernel
 * only has to place it somewhere the ADSP can read and describe where it is.
 * See tools/remoteproc/acdb_voice_cal.py for where it comes from.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
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
	struct device *dev;	/* carries the ADSP's stream id */

	void *table;		/* block list the ADSP reads */
	dma_addr_t table_iova;

	void *data;		/* the calibration blocks themselves */
	dma_addr_t data_iova;
	u32 data_size;		/* what the calibration actually occupies */
	u32 block_size;		/* what was lent: page aligned, as required */

	void *col_info;
	u32 col_size;

	u32 mem_handle;
};

/*
 * The platform device exists only to carry the IOMMU stream id; the
 * calibration is loaded lazily from the call path, which runs elsewhere.
 */
static struct device *q6voice_cal_dev;

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

	/*
	 * No unmap: that command has to be addressed to the MVM session that
	 * lent the memory, and by the time calibration is freed the call --
	 * and the session -- are long gone. Addressing it to the service
	 * instead is what wedged the ADSP in the first place. The mapping goes
	 * away with the session anyway.
	 */

	if (cal->table)
		dma_free_coherent(cal->dev, PAGE_SIZE, cal->table,
				  cal->table_iova);
	if (cal->data)
		dma_free_coherent(cal->dev, cal->block_size, cal->data,
				  cal->data_iova);
	kfree(cal->col_info);
	kfree(cal);
}
EXPORT_SYMBOL_GPL(q6voice_cal_free);

/*
 * Load the calibration for @name and lend it to the ADSP. Returns NULL when
 * there is none installed, which is not an error: calls still work, they are
 * just stuck at whatever volume the vocproc defaults to.
 */
struct q6voice_cal *q6voice_cal_load(struct device *dev, const char *name,
				     struct q6voice_session *mvm)
{
	const struct q6voice_cal_file_hdr *hdr;
	struct q6voice_cal_table *table;
	const struct firmware *fw;
	struct q6voice_cal *cal;
	u32 col_size, data_size;
	int ret;

	/*
	 * No device means no stream id to allocate against, and an address the
	 * ADSP cannot translate is worse than no calibration at all.
	 */
	if (!q6voice_cal_dev)
		return NULL;

	dev = q6voice_cal_dev;

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
	 * physically contiguous -- no vmalloc.
	 *
	 * The block lent to the ADSP must also be a whole number of the pages
	 * the mapping command declares, so round it up and zero the remainder:
	 * handing over a block shorter than that has the ADSP reading past the
	 * calibration into whatever happens to follow it.
	 */
	cal->block_size = PAGE_ALIGN(data_size);
	cal->data = dma_alloc_coherent(dev, cal->block_size, &cal->data_iova,
				       GFP_KERNEL);
	if (!cal->data)
		goto err_free;

	memcpy(cal->data, fw->data + sizeof(*hdr) + col_size, data_size);

	cal->table = dma_alloc_coherent(dev, PAGE_SIZE, &cal->table_iova,
					GFP_KERNEL);
	if (!cal->table)
		goto err_free;

	table = cal->table;
	table->block_addr = cpu_to_le64(cal->data_iova);
	table->block_size = cpu_to_le32(cal->block_size);

	dev_info(dev, "lending %u bytes at %pad, table at %pad\n",
		 cal->block_size, &cal->data_iova, &cal->table_iova);

	ret = q6mvm_map_memory(mvm, cal->table_iova, sizeof(*table),
			       &cal->mem_handle);
	if (ret) {
		dev_err(dev, "failed to lend calibration to the ADSP: %d\n",
			ret);
		goto err_free;
	}

	dev_info(dev, "calibration %s: %u bytes in a %u byte block, handle %#x\n",
		 name, data_size, cal->block_size, cal->mem_handle);

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

	return q6cvp_register_vol_cal(cvp, cal->mem_handle, cal->data_iova,
				      cal->data_size, cal->col_info,
				      cal->col_size);
}
EXPORT_SYMBOL_GPL(q6voice_cal_register_vol);

static int q6voice_cal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA\n");

	/*
	 * A reserved pool if the board gives us one, otherwise allocate against
	 * this device's IOMMU domain -- the audio domain, which the DSP reads
	 * from for ordinary playback, so it can reach this too.
	 */
	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "bad calibration memory pool\n");

	q6voice_cal_dev = dev;
	return 0;
}

static void q6voice_cal_remove(struct platform_device *pdev)
{
	q6voice_cal_dev = NULL;
}

static const struct of_device_id q6voice_cal_device_id[] = {
	{ .compatible = "qcom,q6voice-cal" },
	{},
};
MODULE_DEVICE_TABLE(of, q6voice_cal_device_id);

static struct platform_driver q6voice_cal_platform_driver = {
	.driver = {
		.name = "q6voice-cal",
		.of_match_table = q6voice_cal_device_id,
	},
	.probe = q6voice_cal_probe,
	.remove = q6voice_cal_remove,
};
module_platform_driver(q6voice_cal_platform_driver);

MODULE_DESCRIPTION("Q6Voice calibration");
MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE("qcom/q6voice-vol-cal.bin");
