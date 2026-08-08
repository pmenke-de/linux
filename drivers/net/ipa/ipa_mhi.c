// SPDX-License-Identifier: GPL-2.0

/* Copyright (C) 2026 Linaro Ltd. */

#include <linux/bitfield.h>
#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/iommu.h>
#include <linux/mhi.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/string.h>

#include "ipa.h"
#include "ipa_mhi.h"
#include "ipa_qmi_msg.h"
#include "ipa_uc.h"

#define IPA_UC_MAILBOX_PHYS_OFFSET	0x000c2000
#define IPA_UC_MAILBOX_M_STRIDE		0x80
#define IPA_UC_MAILBOX_N_STRIDE		0x4
#define IPA_UC_MHI_MBOX_M		1
#define IPA_UC_MHI_UL_CH_N		0
#define IPA_UC_MHI_UL_ER_N		1
#define IPA_UC_MHI_DL_CH_N		2
#define IPA_UC_MHI_DL_ER_N		3

/* Vendor kona/apollo MHI doorbell pages for SDX55 fusion. */
#define IPA_MHI_CHDB_BASE_DEFAULT	0x64300300
#define IPA_MHI_ERDB_BASE_DEFAULT	0x64300700
#define IPA_MHI_DB_STRIDE		8
#define IPA_MHI_CTRL_IOVA_BASE_DEFAULT	0x00010000
#define IPA_MHI_CTRL_IOVA_SIZE_DEFAULT	0x0fff0000
#define IPA_MHI_DATA_IOVA_BASE_DEFAULT	0x10000000
#define IPA_MHI_DATA_IOVA_SIZE_DEFAULT	0x0fffffff

#define CHAN_CTX_BRSTMODE_MASK		GENMASK(9, 8)
#define CHAN_CTX_POLLCFG_MASK		GENMASK(15, 10)
#define EV_CTX_INTMODC_MASK		GENMASK(15, 8)
#define EV_CTX_INTMODT_MASK		GENMASK(31, 16)

struct ipa_mhi_chan_ctxt {
	__le32 chcfg;
	__le32 chtype;
	__le32 erindex;
	__le64 rbase __packed __aligned(4);
	__le64 rlen __packed __aligned(4);
	__le64 rp __packed __aligned(4);
	__le64 wp __packed __aligned(4);
};

struct ipa_mhi_event_ctxt {
	__le32 intmod;
	__le32 ertype;
	__le32 msivec;
	__le64 rbase __packed __aligned(4);
	__le64 rlen __packed __aligned(4);
	__le64 rp __packed __aligned(4);
	__le64 wp __packed __aligned(4);
};

struct ipa_mhi_state {
	struct mutex mutex;
	struct ipa *ipa;
	struct mhi_device *mhi_dev;
	struct sockaddr_qrtr modem_sq;
	struct sockaddr_qrtr modem_sq_alt;
	bool have_modem_alt;
	u32 ul_chan;
	u32 dl_chan;
	u32 ul_er;
	u32 dl_er;
	u32 chdb_base;
	u32 erdb_base;
	u64 ctrl_iova_base;
	u64 ctrl_iova_size;
	u64 data_iova_base;
	u64 data_iova_size;
	struct ipa_mhi_alloc_channel_req alloc_req;
	struct ipa_mhi_mem_addr_info ctrl_map[IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX];
	struct ipa_mhi_mem_addr_info data_map[IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX];
	u32 ctrl_map_len;
	u32 data_map_len;
	unsigned long db_iova;
	size_t db_size;
	bool have_ipa;
	bool have_mhi;
	bool have_modem;
	bool ready_sent;
	bool smmu_mapped;
	bool db_mapped;
	bool registered;
};

static struct ipa_mhi_state ipa_mhi_state;
static struct mhi_driver ipa_mhi_driver;

static u32 ipa_mhi_mailbox_addr(struct ipa *ipa, u32 n)
{
	struct platform_device *pdev = to_platform_device(ipa->dev);
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ipa-reg");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return 0;

	return lower_32_bits(res->start + IPA_UC_MAILBOX_PHYS_OFFSET +
			    IPA_UC_MAILBOX_M_STRIDE * IPA_UC_MHI_MBOX_M +
			    IPA_UC_MAILBOX_N_STRIDE * n);
}

static __maybe_unused int ipa_mhi_map_doorbells(struct ipa_mhi_state *state)
{
	struct ipa *ipa = state->ipa;
	int ret;

	ret = ipa_uc_mhi_remote_info(ipa,
				     state->chdb_base +
				     state->ul_chan * IPA_MHI_DB_STRIDE,
				     IPA_UC_MHI_UL_CH_N);
	if (ret) {
		dev_err(ipa->dev, "REMOTE_IPA_INFO UL channel failed: %d\n",
			ret);
		return ret;
	}

	ret = ipa_uc_mhi_remote_info(ipa,
				     state->erdb_base +
				     state->ul_er * IPA_MHI_DB_STRIDE,
				     IPA_UC_MHI_UL_ER_N);
	if (ret) {
		dev_err(ipa->dev, "REMOTE_IPA_INFO UL event failed: %d\n",
			ret);
		return ret;
	}

	ret = ipa_uc_mhi_remote_info(ipa,
				     state->chdb_base +
				     state->dl_chan * IPA_MHI_DB_STRIDE,
				     IPA_UC_MHI_DL_CH_N);
	if (ret) {
		dev_err(ipa->dev, "REMOTE_IPA_INFO DL channel failed: %d\n",
			ret);
		return ret;
	}

	ret = ipa_uc_mhi_remote_info(ipa,
				     state->erdb_base +
				     state->dl_er * IPA_MHI_DB_STRIDE,
				     IPA_UC_MHI_DL_ER_N);
	if (ret) {
		dev_err(ipa->dev, "REMOTE_IPA_INFO DL event failed: %d\n",
			ret);
		return ret;
	}

	dev_info(ipa->dev,
		 "REMOTE_IPA_INFO mapped chdb=0x%08x erdb=0x%08x for channels %u/%u events %u/%u\n",
		 state->chdb_base, state->erdb_base, state->ul_chan,
		 state->dl_chan, state->ul_er, state->dl_er);

	return 0;
}

static void ipa_mhi_round_db_window(struct ipa_mhi_state *state,
				    unsigned long *iova, phys_addr_t *pa,
				    size_t *size)
{
	u32 start = min(state->chdb_base, state->erdb_base);
	u32 end = max(state->chdb_base, state->erdb_base) + PAGE_SIZE;

	*iova = start & PAGE_MASK;
	*pa = start & PAGE_MASK;
	*size = PAGE_ALIGN(end - *iova);
}

static int ipa_mhi_map_uc_doorbells(struct ipa_mhi_state *state)
{
	struct iommu_domain *domain;
	struct ipa *ipa = state->ipa;
	unsigned long iova;
	phys_addr_t pa;
	size_t size;
	int ret;

	if (state->db_mapped)
		return 0;

	domain = iommu_get_domain_for_dev(ipa->dev);
	if (!domain)
		return -EINVAL;

	ipa_mhi_round_db_window(state, &iova, &pa, &size);

	ret = iommu_map(domain, iova, pa, size,
			IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO, GFP_KERNEL);
	if (ret) {
		dev_err(ipa->dev,
			"failed to map IPA uC MHI doorbell page iova=0x%lx pa=%pa size=0x%zx ret=%d\n",
			iova, &pa, size, ret);
		return ret;
	}

	state->db_iova = iova;
	state->db_size = size;
	state->db_mapped = true;

	dev_info(ipa->dev,
		 "mapped IPA uC MHI doorbell page iova=0x%lx pa=%pa size=0x%zx\n",
		 iova, &pa, size);

	return 0;
}

static void ipa_mhi_unmap_uc_doorbells(struct ipa_mhi_state *state)
{
	struct iommu_domain *domain;

	if (!state->db_mapped || !state->ipa)
		return;

	domain = iommu_get_domain_for_dev(state->ipa->dev);
	if (domain)
		iommu_unmap(domain, state->db_iova, state->db_size);

	state->db_mapped = false;
	state->db_iova = 0;
	state->db_size = 0;
}

static int ipa_mhi_send_ready_locked(struct ipa_mhi_state *state)
{
	struct ipa_mhi_ready_ind ind = { };
	struct qmi_handle *qmi;
	struct ipa *ipa = state->ipa;
	int ret;

	if (!state->have_ipa || !state->have_mhi || !state->have_modem)
		return 0;
	if (state->ready_sent)
		return 0;

	if (state->ul_chan == U32_MAX || state->dl_chan == U32_MAX ||
	    state->ul_er == U32_MAX || state->dl_er == U32_MAX)
		return -ENODEV;

	/* The IPA microcontroller never posts INIT_COMPLETED on the SDX55
	 * fusion target (apollo's ipa_fws.mdt does not include a running uC),
	 * so we cannot use the uC mailbox proxy that
	 * ipa_mhi_map_doorbells/REMOTE_IPA_INFO would set up.  Send the
	 * indication with the mailbox addresses anyway — the modem firmware's
	 * MCFG-Refresh-STM watchdog appears to gate on receiving MHI_READY
	 * itself, not on the doorbell-translation path being functional.
	 */
	ret = ipa_mhi_map_uc_doorbells(state);
	if (ret)
		dev_warn(ipa->dev,
			 "error %d mapping IPA uC doorbell window (continuing)\n",
			 ret);

	ind.ch_info_arr_len = 2;
	ind.ch_info_arr[0].ch_id = state->ul_chan;
	ind.ch_info_arr[0].er_id = state->ul_er;
	ind.ch_info_arr[0].direction_type = DMA_TO_DEVICE;
	ind.ch_info_arr[0].ch_doorbell_addr =
		ipa_mhi_mailbox_addr(ipa, IPA_UC_MHI_UL_CH_N);
	ind.ch_info_arr[0].er_doorbell_addr =
		ipa_mhi_mailbox_addr(ipa, IPA_UC_MHI_UL_ER_N);

	ind.ch_info_arr[1].ch_id = state->dl_chan;
	ind.ch_info_arr[1].er_id = state->dl_er;
	ind.ch_info_arr[1].direction_type = DMA_FROM_DEVICE;
	ind.ch_info_arr[1].ch_doorbell_addr =
		ipa_mhi_mailbox_addr(ipa, IPA_UC_MHI_DL_CH_N);
	ind.ch_info_arr[1].er_doorbell_addr =
		ipa_mhi_mailbox_addr(ipa, IPA_UC_MHI_DL_ER_N);

	ind.smmu_info_valid = true;
	ind.smmu_info.iova_ctl_base_addr = state->ctrl_iova_base;
	ind.smmu_info.iova_ctl_size = state->ctrl_iova_size;
	ind.smmu_info.iova_data_base_addr = state->data_iova_base;
	ind.smmu_info.iova_data_size = state->data_iova_size;

	qmi = &ipa->qmi.server_handle;
	ret = qmi_send_indication(qmi, &state->modem_sq, IPA_QMI_MHI_READY,
				  IPA_QMI_MHI_READY_IND_SZ,
				  ipa_mhi_ready_ind_ei, &ind);
	if (ret) {
		dev_err(ipa->dev, "error %d sending MHI ready indication\n",
			ret);
		return ret;
	}

	state->ready_sent = true;
	dev_info(ipa->dev,
		 "sent IPA MHI ready indication for channels %u/%u events %u/%u ctrl_iova=0x%llx+0x%llx data_iova=0x%llx+0x%llx\n",
		 state->ul_chan, state->dl_chan, state->ul_er, state->dl_er,
		 state->ctrl_iova_base, state->ctrl_iova_size,
		 state->data_iova_base, state->data_iova_size);

	/* On the fusion target the modem accepts MHI_READY but never sends
	 * INDICATION_REGISTER, so the upstream INIT_COMPLETE-on-request path
	 * is never taken.  Send INIT_COMPLETE unconditionally here — the
	 * modem's MCFG-Refresh-STM watchdog appears to wait for it after
	 * MHI_READY before continuing mission-mode bringup.
	 *
	 * SDX55 fusion publishes IPA on two PD instances; deliver
	 * INIT_COMPLETE to both so neither modem-PD task starves.
	 */
	{
		struct ipa_init_complete_ind init_ind = { };

		init_ind.status.result = QMI_RESULT_SUCCESS_V01;
		init_ind.status.error = QMI_ERR_NONE_V01;
		ret = qmi_send_indication(qmi, &state->modem_sq,
					  IPA_QMI_INIT_COMPLETE,
					  IPA_QMI_INIT_COMPLETE_IND_SZ,
					  ipa_init_complete_ind_ei,
					  &init_ind);
		if (ret)
			dev_err(ipa->dev,
				"error %d sending IPA INIT_COMPLETE indication\n",
				ret);
		else
			dev_info(ipa->dev,
				 "sent IPA INIT_COMPLETE indication after MHI ready\n");

		if (state->have_modem_alt) {
			ret = qmi_send_indication(qmi, &state->modem_sq_alt,
						  IPA_QMI_INIT_COMPLETE,
						  IPA_QMI_INIT_COMPLETE_IND_SZ,
						  ipa_init_complete_ind_ei,
						  &init_ind);
			if (ret)
				dev_err(ipa->dev,
					"error %d sending alt IPA INIT_COMPLETE\n",
					ret);
			else
				dev_info(ipa->dev,
					 "sent IPA INIT_COMPLETE to alt PD %u:%u\n",
					 state->modem_sq_alt.sq_node,
					 state->modem_sq_alt.sq_port);
		}
	}

	return 0;
}

int ipa_mhi_send_ready(struct ipa *ipa)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	int ret;

	/* The proxy state mutex is only initialized when ipa_mhi_setup()
	 * matched a fusion compatible.  Skip cleanly otherwise so the
	 * uc-loaded retry hook is safe on non-fusion hardware.
	 */
	if (!state->have_ipa)
		return 0;

	mutex_lock(&state->mutex);
	ret = ipa_mhi_send_ready_locked(state);
	mutex_unlock(&state->mutex);

	return ret;
}

void ipa_mhi_modem_server(struct ipa *ipa, struct sockaddr_qrtr *sq)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;

	mutex_lock(&state->mutex);
	state->modem_sq = *sq;
	state->have_modem = true;
	state->ready_sent = false;
	mutex_unlock(&state->mutex);
}

void ipa_mhi_modem_server_alt(struct ipa *ipa, struct sockaddr_qrtr *sq)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	struct ipa_init_complete_ind init_ind = { };
	struct qmi_handle *qmi;
	bool send_now = false;
	int ret;

	mutex_lock(&state->mutex);
	state->modem_sq_alt = *sq;
	state->have_modem_alt = true;
	/*
	 * If the primary handshake already completed (modem PDs published in
	 * different order than expected), send the alt INIT_COMPLETE here.
	 */
	if (state->ready_sent)
		send_now = true;
	mutex_unlock(&state->mutex);

	if (!send_now)
		return;

	qmi = &ipa->qmi.server_handle;
	init_ind.status.result = QMI_RESULT_SUCCESS_V01;
	init_ind.status.error = QMI_ERR_NONE_V01;
	ret = qmi_send_indication(qmi, &state->modem_sq_alt,
				  IPA_QMI_INIT_COMPLETE,
				  IPA_QMI_INIT_COMPLETE_IND_SZ,
				  ipa_init_complete_ind_ei,
				  &init_ind);
	if (ret)
		dev_err(ipa->dev,
			"error %d sending late alt IPA INIT_COMPLETE\n", ret);
	else
		dev_info(ipa->dev,
			 "sent late IPA INIT_COMPLETE to alt PD %u:%u\n",
			 sq->sq_node, sq->sq_port);
}

int ipa_mhi_modem_ready(struct ipa *ipa)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	bool need_register = false;
	int ret = 0;

	if (!of_device_is_compatible(ipa->dev->of_node, "qcom,sm8250-ipa"))
		return 0;

	mutex_lock(&state->mutex);
	state->ready_sent = false;
	if (!state->registered) {
		state->registered = true;
		need_register = true;
	}
	mutex_unlock(&state->mutex);

	if (need_register) {
		ret = mhi_driver_register(&ipa_mhi_driver);
		if (ret) {
			dev_err(ipa->dev,
				"failed to register IPA MHI proxy driver: %d\n",
				ret);
			mutex_lock(&state->mutex);
			state->registered = false;
			mutex_unlock(&state->mutex);
			return ret;
		}
		dev_info(ipa->dev, "IPA MHI proxy driver registered\n");
	}

	/* MHI device may already be present (probe fired previously); try
	 * to send the ready indication now in case all preconditions are met.
	 */
	return ipa_mhi_send_ready(ipa);
}

void ipa_mhi_modem_shutdown(struct ipa *ipa)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	bool unregister = false;

	mutex_lock(&state->mutex);
	state->have_modem = false;
	state->ready_sent = false;
	if (state->registered) {
		state->registered = false;
		unregister = true;
	}
	mutex_unlock(&state->mutex);

	if (unregister)
		mhi_driver_unregister(&ipa_mhi_driver);
}

static const struct ipa_mhi_er_info *
ipa_mhi_find_er(const struct ipa_mhi_alloc_channel_req *req, u8 er_id)
{
	u32 i;

	for (i = 0; i < req->er_info_arr_len; i++)
		if (req->er_info_arr[i].er_id == er_id)
			return &req->er_info_arr[i];

	return NULL;
}

static u32 ipa_mhi_brstmode(u32 mode)
{
	if (mode == IPA_MHI_BRST_MODE_ENABLED)
		return 3;
	if (mode == IPA_MHI_BRST_MODE_DISABLED)
		return 2;

	return 0;
}

static void ipa_mhi_round_mapping(const struct ipa_mhi_mem_addr_info *map,
				  unsigned long *iova, phys_addr_t *pa,
				  size_t *size)
{
	*iova = map->iova & PAGE_MASK;
	*pa = map->pa & PAGE_MASK;
	*size = PAGE_ALIGN(map->size + map->pa - *pa);
}

static int ipa_mhi_map_one(struct ipa_mhi_state *state,
			   const struct ipa_mhi_mem_addr_info *map,
			   bool map_it)
{
	struct iommu_domain *domain;
	unsigned long iova;
	phys_addr_t pa;
	size_t size;
	int prot;
	int ret;

	domain = iommu_get_domain_for_dev(state->mhi_dev->dev.parent);
	if (!domain)
		return -EINVAL;

	ipa_mhi_round_mapping(map, &iova, &pa, &size);
	if (!size)
		return -EINVAL;

	if (!map_it) {
		iommu_unmap(domain, iova, size);
		return 0;
	}

	prot = IOMMU_READ | IOMMU_WRITE;
	if (pa >= 0x1e40000 && pa < 0x1e40000 + 0x100000)
		prot |= IOMMU_MMIO;

	ret = iommu_map(domain, iova, pa, size, prot, GFP_KERNEL);
	if (!ret && state->ipa)
		dev_info(state->ipa->dev,
			 "mapped IPA MHI SMMU iova=0x%lx pa=%pa size=0x%zx prot=0x%x\n",
			 iova, &pa, size, prot);
	else if (ret && state->ipa)
		dev_err(state->ipa->dev,
			"failed IPA MHI SMMU map iova=0x%lx pa=%pa size=0x%zx ret=%d\n",
			iova, &pa, size, ret);

	return ret;
}

static int ipa_mhi_map_smmu(struct ipa_mhi_state *state,
			    const struct ipa_mhi_alloc_channel_req *req)
{
	u32 i;
	int ret;

	if (state->smmu_mapped)
		return 0;

	if (req->ctrl_addr_map_info_len > IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX ||
	    req->data_addr_map_info_len > IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX)
		return -EINVAL;

	state->ctrl_map_len = req->ctrl_addr_map_info_len;
	state->data_map_len = req->data_addr_map_info_len;
	memcpy(state->ctrl_map, req->ctrl_addr_map_info,
	       sizeof(state->ctrl_map[0]) * state->ctrl_map_len);
	memcpy(state->data_map, req->data_addr_map_info,
	       sizeof(state->data_map[0]) * state->data_map_len);

	for (i = 0; i < state->ctrl_map_len; i++) {
		ret = ipa_mhi_map_one(state, &state->ctrl_map[i], true);
		if (ret)
			goto err_unmap_ctrl;
	}

	for (i = 0; i < state->data_map_len; i++) {
		ret = ipa_mhi_map_one(state, &state->data_map[i], true);
		if (ret)
			goto err_unmap_data;
	}

	state->smmu_mapped = true;
	return 0;

err_unmap_data:
	while (i--)
		ipa_mhi_map_one(state, &state->data_map[i], false);
	i = state->ctrl_map_len;
err_unmap_ctrl:
	while (i--)
		ipa_mhi_map_one(state, &state->ctrl_map[i], false);
	state->ctrl_map_len = 0;
	state->data_map_len = 0;
	return ret;
}

static void ipa_mhi_unmap_smmu(struct ipa_mhi_state *state)
{
	u32 i;

	if (!state->smmu_mapped)
		return;

	for (i = 0; i < state->data_map_len; i++)
		ipa_mhi_map_one(state, &state->data_map[i], false);
	for (i = 0; i < state->ctrl_map_len; i++)
		ipa_mhi_map_one(state, &state->ctrl_map[i], false);

	state->ctrl_map_len = 0;
	state->data_map_len = 0;
	state->smmu_mapped = false;
}

static int ipa_mhi_configure_one(struct ipa_mhi_state *state,
				 const struct ipa_mhi_tr_info *tr,
				 struct ipa_mhi_ch_alloc_resp *ch_rsp)
{
	struct ipa_mhi_chan_ctxt ch_ctxt = { };
	struct ipa_mhi_event_ctxt er_ctxt = { };
	const struct ipa_mhi_er_info *er;
	struct mhi_buf cfg[2];
	enum dma_data_direction dir;
	u32 er_id;
	u32 chcfg;
	int ret;

	if (tr->ch_id == state->ul_chan) {
		dir = DMA_TO_DEVICE;
		er_id = state->ul_er;
	} else if (tr->ch_id == state->dl_chan) {
		dir = DMA_FROM_DEVICE;
		er_id = state->dl_er;
	} else {
		ch_rsp->ch_id = tr->ch_id;
		ch_rsp->is_success = 0;
		return -EINVAL;
	}

	er = ipa_mhi_find_er(&state->alloc_req, er_id);
	if (!er) {
		ch_rsp->ch_id = tr->ch_id;
		ch_rsp->is_success = 0;
		return -EINVAL;
	}

	chcfg = FIELD_PREP(CHAN_CTX_BRSTMODE_MASK,
			   ipa_mhi_brstmode(tr->brst_mode_type));
	chcfg |= FIELD_PREP(CHAN_CTX_POLLCFG_MASK, tr->poll_cfg);
	ch_ctxt.chcfg = cpu_to_le32(chcfg);
	ch_ctxt.chtype = cpu_to_le32(dir);
	ch_ctxt.erindex = cpu_to_le32(er_id);
	ch_ctxt.rbase = cpu_to_le64(tr->ring_iova);
	ch_ctxt.rlen = cpu_to_le64(tr->ring_len);
	ch_ctxt.rp = cpu_to_le64(tr->rp);
	ch_ctxt.wp = cpu_to_le64(tr->wp);

	er_ctxt.intmod = cpu_to_le32(FIELD_PREP(EV_CTX_INTMODC_MASK,
						er->intmod_count) |
				     FIELD_PREP(EV_CTX_INTMODT_MASK,
						er->intmod_cycles));
	er_ctxt.ertype = cpu_to_le32(1);
	er_ctxt.msivec = cpu_to_le32(er->msi_addr);
	er_ctxt.rbase = cpu_to_le64(er->ring_iova);
	er_ctxt.rlen = cpu_to_le64(er->ring_len);
	er_ctxt.rp = cpu_to_le64(er->rp);
	er_ctxt.wp = cpu_to_le64(er->wp);

	cfg[0].buf = &ch_ctxt;
	cfg[0].name = "CCA";
	cfg[0].len = sizeof(ch_ctxt);
	cfg[1].buf = &er_ctxt;
	cfg[1].name = "ECA";
	cfg[1].len = sizeof(er_ctxt);

	ret = mhi_device_configure(state->mhi_dev, dir, cfg, ARRAY_SIZE(cfg));
	ch_rsp->ch_id = tr->ch_id;
	ch_rsp->is_success = !ret;
	if (!ret && state->ipa)
		dev_info(state->ipa->dev,
			 "configured remote MHI channel %u event %u ring=0x%llx len=0x%llx\n",
			 tr->ch_id, er_id, tr->ring_iova, tr->ring_len);

	return ret;
}

void ipa_mhi_alloc_channel(struct ipa *ipa,
			   const struct ipa_mhi_alloc_channel_req *req,
			   struct ipa_mhi_alloc_channel_rsp *rsp)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	u32 i;
	int ret = 0;

	memset(rsp, 0, sizeof(*rsp));
	rsp->rsp.result = QMI_RESULT_FAILURE_V01;
	rsp->rsp.error = QMI_ERR_INTERNAL_V01;

	mutex_lock(&state->mutex);
	if (!state->have_mhi || !state->ready_sent || !state->mhi_dev) {
		rsp->rsp.error = QMI_ERR_MALFORMED_MSG_V01;
		goto out_unlock;
	}

	state->alloc_req = *req;

	if (req->tr_info_arr_len > IPA_QMI_REMOTE_MHI_CHANNELS_MAX ||
	    req->er_info_arr_len > IPA_QMI_REMOTE_MHI_CHANNELS_MAX ||
	    req->ctrl_addr_map_info_len > IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX ||
	    req->data_addr_map_info_len > IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX) {
		rsp->rsp.error = QMI_ERR_MALFORMED_MSG_V01;
		goto out_unlock;
	}

	ret = ipa_mhi_map_smmu(state, req);
	if (ret) {
		dev_err(ipa->dev, "error %d mapping IPA MHI SMMU windows\n",
			ret);
		rsp->rsp.error = QMI_ERR_INTERNAL_V01;
		goto out_unlock;
	}

	rsp->alloc_resp_arr_valid = true;
	for (i = 0; i < req->tr_info_arr_len; i++) {
		struct ipa_mhi_ch_alloc_resp *ch_rsp;

		ch_rsp = &rsp->alloc_resp_arr[rsp->alloc_resp_arr_len++];
		ret = ipa_mhi_configure_one(state, &req->tr_info_arr[i],
					    ch_rsp);
		if (ret)
			goto out_unlock;
	}

	ret = mhi_prepare_for_transfer(state->mhi_dev);
	if (ret) {
		rsp->rsp.error = QMI_ERR_MALFORMED_MSG_V01;
		goto out_unlock;
	}

	rsp->rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp->rsp.error = QMI_ERR_NONE_V01;
	dev_info(ipa->dev, "configured IPA MHI offload channels\n");

out_unlock:
	if (ret && state->smmu_mapped)
		ipa_mhi_unmap_smmu(state);
	mutex_unlock(&state->mutex);
}

void ipa_mhi_clk_vote(struct ipa *ipa,
		      const struct ipa_mhi_clk_vote_req *req,
		      struct ipa_mhi_clk_vote_rsp *rsp)
{
	memset(rsp, 0, sizeof(*rsp));
	rsp->rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp->rsp.error = QMI_ERR_NONE_V01;
	dev_dbg(ipa->dev, "IPA MHI clock vote %u rate_valid=%u rate=%u\n",
		req->mhi_vote, req->clk_rate_valid, req->clk_rate);
}

/* The fusion MHI controller config (drivers/bus/mhi/host/pci_generic.c
 * modem_qcom_sdx55_fusion_channels) defines IP_HW_MHIP_0 as offload
 * channel pair 105/106 routed through event rings 11/12.  The upstream
 * struct mhi_device only exposes ul_chan_id/dl_chan_id; the event ring
 * indices live in the private mhi_chan struct, so we hardcode them here
 * to match the controller config.
 */
#define IPA_MHI_PROXY_CHAN	"IP_HW_MHIP_0"
#define IPA_MHI_PROXY_UL_CH	105
#define IPA_MHI_PROXY_DL_CH	106
#define IPA_MHI_PROXY_UL_ER	11
#define IPA_MHI_PROXY_DL_ER	12

static int ipa_mhi_probe(struct mhi_device *mhi_dev,
			 const struct mhi_device_id *id)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	int ret;

	if (!state->ipa)
		return -ENODEV;

	mutex_lock(&state->mutex);

	if (state->mhi_dev) {
		dev_info(state->ipa->dev,
			 "IPA MHI proxy already bound; ignoring extra %s bind\n",
			 id->chan);
		mutex_unlock(&state->mutex);
		return -EBUSY;
	}

	if (mhi_dev->ul_chan_id != IPA_MHI_PROXY_UL_CH ||
	    mhi_dev->dl_chan_id != IPA_MHI_PROXY_DL_CH) {
		dev_err(state->ipa->dev,
			"unexpected MHI proxy channels ul=%d dl=%d\n",
			mhi_dev->ul_chan_id, mhi_dev->dl_chan_id);
		mutex_unlock(&state->mutex);
		return -EINVAL;
	}

	state->mhi_dev = mhi_dev;
	state->ul_chan = mhi_dev->ul_chan_id;
	state->dl_chan = mhi_dev->dl_chan_id;
	state->ul_er = IPA_MHI_PROXY_UL_ER;
	state->dl_er = IPA_MHI_PROXY_DL_ER;
	state->have_mhi = true;

	dev_info(state->ipa->dev,
		 "IPA MHI proxy bound %s: ul_chan=%u dl_chan=%u ul_er=%u dl_er=%u\n",
		 id->chan, state->ul_chan, state->dl_chan,
		 state->ul_er, state->dl_er);

	ret = ipa_mhi_send_ready_locked(state);
	if (ret) {
		dev_err(state->ipa->dev,
			"error %d sending MHI ready from probe\n", ret);
		state->mhi_dev = NULL;
		state->have_mhi = false;
	}
	mutex_unlock(&state->mutex);

	return ret;
}

static void ipa_mhi_remove(struct mhi_device *mhi_dev)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;

	mutex_lock(&state->mutex);
	if (state->mhi_dev == mhi_dev) {
		ipa_mhi_unmap_uc_doorbells(state);
		ipa_mhi_unmap_smmu(state);
		state->mhi_dev = NULL;
		state->have_mhi = false;
		state->ready_sent = false;
	}
	mutex_unlock(&state->mutex);
}

static const struct mhi_device_id ipa_mhi_match_table[] = {
	{ .chan = IPA_MHI_PROXY_CHAN },
	{ },
};

static struct mhi_driver ipa_mhi_driver = {
	.id_table = ipa_mhi_match_table,
	.probe = ipa_mhi_probe,
	.remove = ipa_mhi_remove,
	.driver = {
		.name = "ipa_mhi_proxy",
		.owner = THIS_MODULE,
	},
};

int ipa_mhi_setup(struct ipa *ipa)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;

	if (!of_device_is_compatible(ipa->dev->of_node, "qcom,sm8250-ipa"))
		return 0;

	mutex_init(&state->mutex);
	mutex_lock(&state->mutex);
	state->ipa = ipa;
	state->have_ipa = true;
	state->have_modem = false;
	state->ready_sent = false;
	state->ctrl_map_len = 0;
	state->data_map_len = 0;
	state->smmu_mapped = false;
	state->db_mapped = false;
	state->db_iova = 0;
	state->db_size = 0;
	state->registered = false;
	state->chdb_base = IPA_MHI_CHDB_BASE_DEFAULT;
	state->erdb_base = IPA_MHI_ERDB_BASE_DEFAULT;
	state->ctrl_iova_base = IPA_MHI_CTRL_IOVA_BASE_DEFAULT;
	state->ctrl_iova_size = IPA_MHI_CTRL_IOVA_SIZE_DEFAULT;
	state->data_iova_base = IPA_MHI_DATA_IOVA_BASE_DEFAULT;
	state->data_iova_size = IPA_MHI_DATA_IOVA_SIZE_DEFAULT;
	of_property_read_u32(ipa->dev->of_node, "qcom,mhi-chdb-base",
			     &state->chdb_base);
	of_property_read_u32(ipa->dev->of_node, "qcom,mhi-erdb-base",
			     &state->erdb_base);
	of_property_read_u64(ipa->dev->of_node, "qcom,ctrl-iova-base",
			     &state->ctrl_iova_base);
	of_property_read_u64(ipa->dev->of_node, "qcom,ctrl-iova-size",
			     &state->ctrl_iova_size);
	of_property_read_u64(ipa->dev->of_node, "qcom,data-iova-base",
			     &state->data_iova_base);
	of_property_read_u64(ipa->dev->of_node, "qcom,data-iova-size",
			     &state->data_iova_size);
	mutex_unlock(&state->mutex);

	return 0;
}

void ipa_mhi_teardown(struct ipa *ipa)
{
	struct ipa_mhi_state *state = &ipa_mhi_state;
	bool unregister = false;

	if (!of_device_is_compatible(ipa->dev->of_node, "qcom,sm8250-ipa"))
		return;

	mutex_lock(&state->mutex);
	if (state->registered) {
		state->registered = false;
		unregister = true;
	}
	mutex_unlock(&state->mutex);

	if (unregister)
		mhi_driver_unregister(&ipa_mhi_driver);

	mutex_lock(&state->mutex);
	ipa_mhi_unmap_uc_doorbells(state);
	state->ipa = NULL;
	state->have_ipa = false;
	state->have_modem = false;
	state->mhi_dev = NULL;
	state->have_mhi = false;
	state->ready_sent = false;
	state->ctrl_map_len = 0;
	state->data_map_len = 0;
	state->smmu_mapped = false;
	state->db_mapped = false;
	state->db_iova = 0;
	state->db_size = 0;
	mutex_unlock(&state->mutex);
}
