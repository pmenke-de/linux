// SPDX-License-Identifier: GPL-2.0
/*
 * Voice mailbox bridge for modems attached over PCIe/MHI.
 *
 * On boards where the modem is a separate chip on the PCIe bus (a SDX55 next to
 * a SM8250, for example) call audio never passes through the CPU. The modem
 * DMAs voice frames into a shared buffer in system memory and the ADSP picks
 * them up from there. Neither side can discover that buffer on its own: the
 * modem reaches it through the PCIe SMMU and the ADSP through its own, so the
 * same physical memory has two different addresses, and only the AP can map it
 * into both.
 *
 * That is all this driver does. It allocates the buffer, maps it into both
 * IOMMU domains, and hands both translations to the ADSP with a single APR
 * command. Afterwards it is out of the audio path entirely; q6voice creates the
 * call session as usual and the data flows modem <-> ADSP without us.
 *
 * The MHI channel it binds to ("AUDIO_VOICE_0") is declared as an offload
 * channel, so the host never moves data on it. Binding is only how we get hold
 * of the modem's PCI device -- the one whose IOMMU domain the modem's DMA goes
 * through -- and a notification for when the modem comes and goes.
 */

#include <linux/dma-mapping.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>

#include "q6mvm.h"

/*
 * Size and placement follow the vendor carveout: 128 KiB, and reachable by a
 * 32-bit DMA address (the reserved region upstream is capped at 0xffffffff).
 */
#define VOICE_MAILBOX_SIZE	SZ_128K

struct q6voice_mhi {
	struct device *adsp_dev;	/* mapped into the ADSP's IOMMU domain */
	struct device *pcie_dev;	/* mapped into the PCIe/modem domain */

	void *buf;
	phys_addr_t phys;
	dma_addr_t iova_adsp;
	dma_addr_t iova_pcie;

	struct mutex lock;
};

/*
 * The platform device and the MHI device show up independently and in no fixed
 * order, so the two halves rendezvous through this.
 */
static struct q6voice_mhi *q6voice_mhi_ctx;

static void q6voice_mhi_unmap_pcie(struct q6voice_mhi *ctx)
{
	if (!ctx->iova_pcie)
		return;

	dma_unmap_resource(ctx->pcie_dev, ctx->iova_pcie, VOICE_MAILBOX_SIZE,
			   DMA_BIDIRECTIONAL, 0);
	ctx->iova_pcie = 0;
	ctx->pcie_dev = NULL;
}

static int q6voice_mhi_map_and_configure(struct q6voice_mhi *ctx,
					 struct device *pcie_dev)
{
	dma_addr_t iova;
	int ret;

	iova = dma_map_resource(pcie_dev, ctx->phys, VOICE_MAILBOX_SIZE,
				DMA_BIDIRECTIONAL, 0);
	if (dma_mapping_error(pcie_dev, iova)) {
		dev_err(ctx->adsp_dev, "failed to map mailbox for PCIe\n");
		return -ENOMEM;
	}

	ctx->pcie_dev = pcie_dev;
	ctx->iova_pcie = iova;

	dev_dbg(ctx->adsp_dev,
		"mailbox: phys %pa, adsp iova %pad, pcie iova %pad, %u bytes\n",
		&ctx->phys, &ctx->iova_adsp, &ctx->iova_pcie,
		VOICE_MAILBOX_SIZE);

	ret = q6mvm_set_mailbox_memory(ctx->iova_adsp, ctx->iova_pcie,
				       VOICE_MAILBOX_SIZE);
	if (ret) {
		dev_err(ctx->adsp_dev, "failed to configure mailbox: %d\n", ret);
		q6voice_mhi_unmap_pcie(ctx);
		return ret;
	}

	dev_info(ctx->adsp_dev, "voice mailbox configured\n");
	return 0;
}

static int q6voice_mhi_probe_mhi(struct mhi_device *mhi_dev,
				 const struct mhi_device_id *id)
{
	struct q6voice_mhi *ctx = q6voice_mhi_ctx;
	int ret;

	if (!ctx)
		return -EPROBE_DEFER;

	mutex_lock(&ctx->lock);
	if (ctx->iova_pcie) {
		mutex_unlock(&ctx->lock);
		return -EBUSY;
	}

	/*
	 * The modem DMAs through the PCI device's IOMMU domain, not through
	 * the MHI device (which is a logical child with no DMA of its own).
	 */
	ret = q6voice_mhi_map_and_configure(ctx, mhi_dev->mhi_cntrl->cntrl_dev);
	mutex_unlock(&ctx->lock);
	if (ret)
		return ret;

	dev_set_drvdata(&mhi_dev->dev, ctx);
	return 0;
}

static void q6voice_mhi_remove_mhi(struct mhi_device *mhi_dev)
{
	struct q6voice_mhi *ctx = dev_get_drvdata(&mhi_dev->dev);

	if (!ctx)
		return;

	mutex_lock(&ctx->lock);
	q6voice_mhi_unmap_pcie(ctx);
	mutex_unlock(&ctx->lock);
}

/* Offload channel: the host is never asked to move data on it. */
static void q6voice_mhi_xfer_cb(struct mhi_device *mhi_dev,
				struct mhi_result *result)
{
}

static const struct mhi_device_id q6voice_mhi_match[] = {
	{ .chan = "AUDIO_VOICE_0" },
	{},
};
MODULE_DEVICE_TABLE(mhi, q6voice_mhi_match);

static struct mhi_driver q6voice_mhi_driver = {
	.id_table = q6voice_mhi_match,
	.probe = q6voice_mhi_probe_mhi,
	.remove = q6voice_mhi_remove_mhi,
	.ul_xfer_cb = q6voice_mhi_xfer_cb,
	.dl_xfer_cb = q6voice_mhi_xfer_cb,
	.driver = {
		.name = "q6voice-mhi",
	},
};

static int q6voice_mhi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct q6voice_mhi *ctx;
	int ret;

	if (q6voice_mhi_ctx)
		return -EEXIST;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	ctx->adsp_dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA\n");

	/*
	 * Allocate the pages directly rather than with dma_alloc_coherent():
	 * we need a known physical address to map a second time for the modem,
	 * and a coherent allocation would only give us this device's IOVA.
	 */
	ctx->buf = (void *)__get_free_pages(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO,
					    get_order(VOICE_MAILBOX_SIZE));
	if (!ctx->buf)
		return -ENOMEM;

	ctx->phys = virt_to_phys(ctx->buf);

	/* This device carries the ADSP stream ID, so this is the ADSP's view */
	ctx->iova_adsp = dma_map_single(dev, ctx->buf, VOICE_MAILBOX_SIZE,
					DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, ctx->iova_adsp)) {
		ret = -ENOMEM;
		goto err_free;
	}

	platform_set_drvdata(pdev, ctx);
	q6voice_mhi_ctx = ctx;

	/*
	 * Register last: the modem may already be up, in which case this
	 * probes the MHI side immediately and completes the configuration.
	 */
	ret = mhi_driver_register(&q6voice_mhi_driver);
	if (ret) {
		dev_err(dev, "failed to register MHI driver: %d\n", ret);
		goto err_unmap;
	}

	return 0;

err_unmap:
	q6voice_mhi_ctx = NULL;
	dma_unmap_single(dev, ctx->iova_adsp, VOICE_MAILBOX_SIZE,
			 DMA_BIDIRECTIONAL);
err_free:
	free_pages((unsigned long)ctx->buf, get_order(VOICE_MAILBOX_SIZE));
	return ret;
}

static void q6voice_mhi_remove(struct platform_device *pdev)
{
	struct q6voice_mhi *ctx = platform_get_drvdata(pdev);

	mhi_driver_unregister(&q6voice_mhi_driver);

	mutex_lock(&ctx->lock);
	q6voice_mhi_unmap_pcie(ctx);
	mutex_unlock(&ctx->lock);

	dma_unmap_single(ctx->adsp_dev, ctx->iova_adsp, VOICE_MAILBOX_SIZE,
			 DMA_BIDIRECTIONAL);
	free_pages((unsigned long)ctx->buf, get_order(VOICE_MAILBOX_SIZE));
	q6voice_mhi_ctx = NULL;
}

static const struct of_device_id q6voice_mhi_device_id[] = {
	{ .compatible = "qcom,q6voice-mhi" },
	{},
};
MODULE_DEVICE_TABLE(of, q6voice_mhi_device_id);

static struct platform_driver q6voice_mhi_platform_driver = {
	.driver = {
		.name = "q6voice-mhi",
		.of_match_table = q6voice_mhi_device_id,
	},
	.probe = q6voice_mhi_probe,
	.remove = q6voice_mhi_remove,
};
module_platform_driver(q6voice_mhi_platform_driver);

MODULE_DESCRIPTION("Q6Voice mailbox bridge for PCIe/MHI attached modems");
MODULE_LICENSE("GPL v2");
