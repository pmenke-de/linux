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

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>

#include "q6mvm.h"
#include "q6voice-common.h"

/*
 * Size and placement follow the vendor carveout: 128 KiB, and reachable by a
 * 32-bit DMA address (the reserved region upstream is capped at 0xffffffff).
 */
#define VOICE_MAILBOX_SIZE	SZ_128K

struct q6voice_mhi {
	struct device *adsp_dev;	/* mapped into the ADSP's IOMMU domain */
	struct device *pcie_dev;	/* mapped into the PCIe/modem domain */
	struct mhi_device *mhi_dev;	/* voted awake for the length of a call */

	void *buf;
	phys_addr_t phys;
	dma_addr_t iova_adsp;
	dma_addr_t iova_pcie;

	struct dentry *debugfs;

	bool configured;
	unsigned int votes;
	struct mutex lock;

	/* re-announces the mailbox after the ADSP restarts */
	struct work_struct reconfig;
};

/*
 * Is anything actually crossing the mailbox?
 *
 * Every command in a call can succeed while the buffer stays untouched, which
 * looks identical from the AP side to a call that works. The buffer starts
 * zeroed and neither agent has a reason to write zeroes into it, so a non-zero
 * byte is proof that the modem and the ADSP are talking to each other, and an
 * all-zero buffer during a call is proof that they are not.
 */
static int q6voice_mhi_mailbox_show(struct seq_file *s, void *unused)
{
	struct q6voice_mhi *ctx = s->private;
	const u8 *buf = ctx->buf;
	size_t nonzero = 0;
	size_t i;

	/* Both agents write this behind our back; take the CPU's view back. */
	dma_sync_single_for_cpu(ctx->adsp_dev, ctx->iova_adsp,
				VOICE_MAILBOX_SIZE, DMA_FROM_DEVICE);

	for (i = 0; i < VOICE_MAILBOX_SIZE; i++)
		if (buf[i])
			nonzero++;

	seq_printf(s, "adsp iova:  %pad\n", &ctx->iova_adsp);
	seq_printf(s, "pcie iova:  %pad\n", &ctx->iova_pcie);
	seq_printf(s, "size:       %u\n", VOICE_MAILBOX_SIZE);
	seq_printf(s, "votes:      %u\n", ctx->votes);
	seq_printf(s, "nonzero:    %zu\n", nonzero);
	seq_printf(s, "head:       %*ph\n", 64, buf);

	dma_sync_single_for_device(ctx->adsp_dev, ctx->iova_adsp,
				   VOICE_MAILBOX_SIZE, DMA_FROM_DEVICE);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(q6voice_mhi_mailbox);

/*
 * The platform device and the MHI device show up independently and in no fixed
 * order, so the two halves rendezvous through this.
 */
static struct q6voice_mhi *q6voice_mhi_ctx;

/* Defined below; a call reaches for it before the definition is in scope. */
static int q6voice_mhi_configure(struct q6voice_mhi *ctx);

/*
 * Hold the modem awake while a call runs.
 *
 * Nothing else keeps it busy: the voice frames go straight to the ADSP without
 * the host touching the link, so from the host's point of view an established
 * call looks exactly like an idle modem, and the autosuspend timer takes it
 * down a couple of seconds in. The frames stop arriving and the call goes
 * silent while every command that set it up still reports success.
 *
 * Counted, because a call has a capture and a playback side and either may
 * start first.
 */
static int q6voice_mhi_start(void)
{
	struct q6voice_mhi *ctx = q6voice_mhi_ctx;
	int ret = 0;

	if (!ctx)
		return 0;

	mutex_lock(&ctx->lock);

	/*
	 * Last chance to describe the mailbox before it is needed. By now the
	 * ADSP has been up long enough to listen, which is not true at probe.
	 */
	q6voice_mhi_configure(ctx);

	/* No modem bound means no link to hold up; let the call proceed. */
	if (!ctx->mhi_dev)
		goto out;

	if (!ctx->votes) {
		ret = mhi_device_get_sync(ctx->mhi_dev);
		if (ret) {
			dev_err(ctx->adsp_dev,
				"failed to wake the modem: %d\n", ret);
			goto out;
		}
	}

	ctx->votes++;

out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static void q6voice_mhi_end(void)
{
	struct q6voice_mhi *ctx = q6voice_mhi_ctx;

	if (!ctx)
		return;

	mutex_lock(&ctx->lock);

	if (ctx->mhi_dev && ctx->votes && !--ctx->votes)
		mhi_device_put(ctx->mhi_dev);

	mutex_unlock(&ctx->lock);
}

static const struct q6voice_modem_link q6voice_mhi_link = {
	.start	= q6voice_mhi_start,
	.end	= q6voice_mhi_end,
};

static void q6voice_mhi_unmap_pcie(struct q6voice_mhi *ctx)
{
	if (!ctx->iova_pcie)
		return;

	/* Whatever the ADSP was told described memory that is going away. */
	ctx->configured = false;

	dma_unmap_resource(ctx->pcie_dev, ctx->iova_pcie, VOICE_MAILBOX_SIZE,
			   DMA_BIDIRECTIONAL, 0);
	ctx->iova_pcie = 0;
	ctx->pcie_dev = NULL;
}

/*
 * Tell the ADSP where the mailbox is. Split out because it has to be said
 * again every time the ADSP restarts: it comes back knowing nothing, and a
 * mailbox it has not been told about is one it will never read.
 */
static int q6voice_mhi_configure(struct q6voice_mhi *ctx)
{
	int ret;

	if (ctx->configured || !ctx->iova_pcie)
		return 0;

	/*
	 * The ADSP resolves what it is given through its own SMMU, so it needs
	 * the address in the form that says which stream to use. The PCIe
	 * address is the modem's and is left alone.
	 */
	ret = q6mvm_set_mailbox_memory(q6voice_dsp_address(ctx->adsp_dev,
							   ctx->iova_adsp),
				       ctx->iova_pcie,
				       VOICE_MAILBOX_SIZE);
	if (ret) {
		/*
		 * Not fatal, and worth retrying: the ADSP refuses this while it
		 * is still coming up, and after a restart its services appear
		 * milliseconds before it is ready to be told anything.
		 */
		dev_warn(ctx->adsp_dev, "mailbox not configured yet: %d\n", ret);
		return ret;
	}

	ctx->configured = true;
	dev_info(ctx->adsp_dev, "voice mailbox configured\n");
	return 0;
}

static void q6voice_mhi_reconfig_work(struct work_struct *work)
{
	struct q6voice_mhi *ctx = container_of(work, struct q6voice_mhi,
					       reconfig);

	mutex_lock(&ctx->lock);
	/* Nothing to announce until the modem has been mapped in. */
	if (ctx->iova_pcie)
		q6voice_mhi_configure(ctx);
	mutex_unlock(&ctx->lock);
}

/*
 * The ADSP announces itself here, including after a restart. Anything it was
 * told before that is gone, so it has to be told again -- but not from here:
 * this runs while the service is still probing, and saying it involves waiting
 * for the ADSP to answer.
 */
static void q6voice_mhi_svc_up(enum q6voice_service_type type)
{
	struct q6voice_mhi *ctx = q6voice_mhi_ctx;

	if (type != Q6VOICE_SERVICE_MVM || !ctx)
		return;

	/* A service that has just appeared knows nothing yet. */
	ctx->configured = false;
	schedule_work(&ctx->reconfig);
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

	/*
	 * Keep the mapping even if the ADSP will not hear it yet -- a call
	 * tries again before it needs the mailbox, and tearing the mapping
	 * down here leaves nothing to tell it about later.
	 */
	q6voice_mhi_configure(ctx);

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
	if (!ret)
		ctx->mhi_dev = mhi_dev;
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

	/* A call may still be running; hand the wake reference back. */
	if (ctx->votes) {
		mhi_device_put(mhi_dev);
		ctx->votes = 0;
	}
	ctx->mhi_dev = NULL;

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
	INIT_WORK(&ctx->reconfig, q6voice_mhi_reconfig_work);
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

	ctx->debugfs = debugfs_create_dir("q6voice-mhi", NULL);
	debugfs_create_file("mailbox", 0444, ctx->debugfs, ctx,
			    &q6voice_mhi_mailbox_fops);

	q6voice_set_modem_link(&q6voice_mhi_link);
	q6voice_common_set_svc_notifier(q6voice_mhi_svc_up);

	/*
	 * Register last: the modem may already be up, in which case this
	 * probes the MHI side immediately and completes the configuration.
	 */
	ret = mhi_driver_register(&q6voice_mhi_driver);
	if (ret) {
		dev_err(dev, "failed to register MHI driver: %d\n", ret);
		goto err_unregister;
	}

	return 0;

err_unregister:
	q6voice_common_set_svc_notifier(NULL);
	q6voice_set_modem_link(NULL);
	debugfs_remove_recursive(ctx->debugfs);
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

	debugfs_remove_recursive(ctx->debugfs);

	q6voice_common_set_svc_notifier(NULL);
	q6voice_set_modem_link(NULL);
	cancel_work_sync(&ctx->reconfig);
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
