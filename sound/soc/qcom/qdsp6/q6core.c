// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/jiffies.h>
#include <linux/soc/qcom/apr.h>
#include "q6core.h"
#include "q6dsp-errno.h"
#include "q6voice-common.h"

#define ADSP_STATE_READY_TIMEOUT_MS    3000
#define Q6_READY_TIMEOUT_MS 100
#define Q6_TOPOLOGY_TIMEOUT_MS 1000
#define AVCS_CMD_ADSP_EVENT_GET_STATE		0x0001290C
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE	0x0001290D
#define AVCS_GET_VERSIONS       0x00012905
#define AVCS_CMD_LOAD_TOPO_MODULES	0x0001296C
#define AVCS_GET_VERSIONS_RSP   0x00012906
#define AVCS_CMD_GET_FWK_VERSION	0x001292c
#define AVCS_CMDRSP_GET_FWK_VERSION	0x001292d
#define AVCS_CMD_REGISTER_TOPOLOGIES		0x00012923
#define AVCS_CMD_SHARED_MEM_MAP_REGIONS	0x00012924
#define AVCS_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00012925
#define AVCS_CMD_SHARED_MEM_UNMAP_REGIONS	0x00012926

#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL	3

struct avcs_cmd_shared_mem_map_regions {
	struct apr_hdr hdr;
	u16 mem_pool_id;
	u16 num_regions;
	u32 property_flag;
	u32 shm_addr_lsw;
	u32 shm_addr_msw;
	u32 mem_size_bytes;
} __packed;

struct avcs_cmd_register_topologies {
	struct apr_hdr hdr;
	u32 payload_addr_lsw;
	u32 payload_addr_msw;
	u32 mem_map_handle;
	u32 payload_size;
} __packed;

struct avcs_cmd_shared_mem_unmap_regions {
	struct apr_hdr hdr;
	u32 mem_map_handle;
} __packed;

struct avcs_svc_info {
	uint32_t service_id;
	uint32_t version;
} __packed;

struct avcs_cmdrsp_get_version {
	uint32_t build_id;
	uint32_t num_services;
	struct avcs_svc_info svc_api_info[];
} __packed;

/* for ADSP2.8 and above */
struct avcs_svc_api_info {
	uint32_t service_id;
	uint32_t api_version;
	uint32_t api_branch_version;
} __packed;

struct avcs_cmdrsp_get_fwk_version {
	uint32_t build_major_version;
	uint32_t build_minor_version;
	uint32_t build_branch_version;
	uint32_t build_subbranch_version;
	uint32_t num_services;
	struct avcs_svc_api_info svc_api_info[];
} __packed;

struct q6core {
	struct apr_device *adev;
	wait_queue_head_t wait;
	uint32_t avcs_state;
	struct mutex lock;
	bool resp_received;
	u32 cmd_status;
	u32 mem_map_handle;
	bool custom_topologies_registered;
	bool custom_topologies_failed;
	uint32_t num_services;
	struct avcs_cmdrsp_get_fwk_version *fwk_version;
	struct avcs_cmdrsp_get_version *svc_version;
	bool fwk_version_supported;
	bool get_state_supported;
	bool get_version_supported;
	bool is_version_requested;
};

static struct q6core *g_core;

static int q6core_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	struct q6core *core = dev_get_drvdata(&adev->dev);
	const struct aprv2_ibasic_rsp_result_t *result;
	const struct apr_hdr *hdr = &data->hdr;

	result = data->payload;
	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT:{
		result = data->payload;
		switch (result->opcode) {
		case AVCS_GET_VERSIONS:
			if (result->status == ADSP_EUNSUPPORTED)
				core->get_version_supported = false;
			core->resp_received = true;
			break;
		case AVCS_CMD_GET_FWK_VERSION:
			if (result->status == ADSP_EUNSUPPORTED)
				core->fwk_version_supported = false;
			core->resp_received = true;
			break;
		case AVCS_CMD_ADSP_EVENT_GET_STATE:
			if (result->status == ADSP_EUNSUPPORTED)
				core->get_state_supported = false;
			core->resp_received = true;
			break;
		case AVCS_CMD_LOAD_TOPO_MODULES:
			core->cmd_status = result->status;
			core->resp_received = true;
			break;
		case AVCS_CMD_SHARED_MEM_MAP_REGIONS:
		case AVCS_CMD_SHARED_MEM_UNMAP_REGIONS:
		case AVCS_CMD_REGISTER_TOPOLOGIES:
			core->cmd_status = result->status;
			core->resp_received = true;
			break;
		}
		break;
	}
	case AVCS_CMDRSP_GET_FWK_VERSION: {
		struct avcs_cmdrsp_get_fwk_version *fwk;

		fwk = data->payload;

		core->fwk_version = kmemdup(data->payload,
					    struct_size(fwk, svc_api_info,
							fwk->num_services),
					    GFP_ATOMIC);
		if (!core->fwk_version)
			return -ENOMEM;

		core->fwk_version_supported = true;
		core->resp_received = true;

		break;
	}
	case AVCS_GET_VERSIONS_RSP: {
		struct avcs_cmdrsp_get_version *v;

		v = data->payload;

		core->svc_version = kmemdup(data->payload,
					    struct_size(v, svc_api_info,
							v->num_services),
					    GFP_ATOMIC);
		if (!core->svc_version)
			return -ENOMEM;

		core->get_version_supported = true;
		core->resp_received = true;

		break;
	}
	case AVCS_CMDRSP_SHARED_MEM_MAP_REGIONS:
		if (data->payload_size < sizeof(u32)) {
			dev_err(&adev->dev, "short shared-memory map response\n");
			return -EINVAL;
		}

		core->mem_map_handle = *(const u32 *)data->payload;
		core->cmd_status = 0;
		core->resp_received = true;
		break;
	case AVCS_CMDRSP_ADSP_EVENT_GET_STATE:
		core->get_state_supported = true;
		core->avcs_state = result->opcode;

		core->resp_received = true;
		break;
	default:
		dev_err(&adev->dev, "Message id from adsp core svc: 0x%x\n",
			hdr->opcode);
		break;
	}

	if (core->resp_received)
		wake_up(&core->wait);

	return 0;
}

static int q6core_get_fwk_versions(struct q6core *core)
{
	struct apr_device *adev = core->adev;
	struct apr_pkt pkt;
	int rc;

	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				      APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = APR_HDR_SIZE;
	pkt.hdr.opcode = AVCS_CMD_GET_FWK_VERSION;

	rc = apr_send_pkt(adev, &pkt);
	if (rc < 0)
		return rc;

	rc = wait_event_timeout(core->wait, (core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && core->resp_received) {
		core->resp_received = false;

		if (!core->fwk_version_supported)
			return -ENOTSUPP;
		else
			return 0;
	}


	return rc;
}

static int q6core_get_svc_versions(struct q6core *core)
{
	struct apr_device *adev = core->adev;
	struct apr_pkt pkt;
	int rc;

	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				      APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = APR_HDR_SIZE;
	pkt.hdr.opcode = AVCS_GET_VERSIONS;

	rc = apr_send_pkt(adev, &pkt);
	if (rc < 0)
		return rc;

	rc = wait_event_timeout(core->wait, (core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && core->resp_received) {
		core->resp_received = false;
		return 0;
	}

	return rc;
}

static bool __q6core_is_adsp_ready(struct q6core *core)
{
	struct apr_device *adev = core->adev;
	struct apr_pkt pkt;
	int rc;

	core->get_state_supported = false;

	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				      APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = APR_HDR_SIZE;
	pkt.hdr.opcode = AVCS_CMD_ADSP_EVENT_GET_STATE;

	rc = apr_send_pkt(adev, &pkt);
	if (rc < 0)
		return false;

	rc = wait_event_timeout(core->wait, (core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && core->resp_received) {
		core->resp_received = false;

		if (core->avcs_state)
			return true;
	}

	/* assume that the adsp is up if we not support this command */
	if (!core->get_state_supported)
		return true;

	return false;
}

static int q6core_wait_for_response(struct q6core *core, const char *command,
				    unsigned int timeout_ms)
{
	int ret;

	ret = wait_event_timeout(core->wait, core->resp_received,
				 msecs_to_jiffies(timeout_ms));
	if (ret <= 0 || !core->resp_received) {
		dev_err(&core->adev->dev, "%s timed out\n", command);
		return -ETIMEDOUT;
	}

	core->resp_received = false;
	if (core->cmd_status) {
		dev_err(&core->adev->dev, "%s rejected: %#x\n", command,
			core->cmd_status);
		return -EIO;
	}

	return 0;
}

static int q6core_send_and_wait_topology(struct q6core *core,
					 struct apr_pkt *pkt,
					 const char *command)
{
	int ret;

	core->cmd_status = 0;
	core->resp_received = false;
	ret = apr_send_pkt(core->adev, pkt);
	if (ret < 0)
		return ret;

	return q6core_wait_for_response(core, command, Q6_TOPOLOGY_TIMEOUT_MS);
}

/**
 * q6core_register_custom_topologies() - Register a board's AVCS topology blob
 * @firmware_name: firmware file containing the raw AVCS topology payload
 *
 * Qualcomm's Android calibration stack sends this payload through
 * CORE_CUSTOM_TOPOLOGIES_CAL_TYPE before it asks CVD to load OEM topology
 * IDs. Mainline has no audio-calibration ioctl, so register the exact blob
 * directly with AVCS. After AVCS acknowledges REGISTER_TOPOLOGIES, the shared
 * buffer is unmapped and freed.
 */
int q6core_register_custom_topologies(const char *firmware_name)
{
	struct avcs_cmd_shared_mem_map_regions map = { 0 };
	struct avcs_cmd_register_topologies register_topologies = { 0 };
	struct avcs_cmd_shared_mem_unmap_regions unmap = { 0 };
	const struct firmware *fw;
	void *buffer;
	dma_addr_t iova;
	u64 dsp_addr;
	u32 payload_size, map_handle = 0;
	size_t buffer_size;
	bool keep_buffer = false;
	int ret, unmap_ret;

	if (!g_core || !firmware_name)
		return -ENODEV;

	ret = request_firmware(&fw, firmware_name, &g_core->adev->dev);
	if (ret) {
		dev_err(&g_core->adev->dev, "request %s failed: %d\n",
			firmware_name, ret);
		return ret;
	}
	/*
	 * A board's whole topology database, not a single topology, so one page
	 * is not a safe bound -- this one is about eight kilobytes. Cap it at
	 * something that still fits a contiguous coherent allocation.
	 */
	if (!fw->size || fw->size > SZ_128K) {
		dev_err(&g_core->adev->dev, "invalid topology firmware size %zu\n",
			fw->size);
		ret = -EINVAL;
		goto release_firmware;
	}
	buffer_size = ALIGN(fw->size, PAGE_SIZE);

	payload_size = fw->size;
	/*
	 * Android gives AVCS an IOVA in audio SID 1, not a CPU physical
	 * address. A bare physical address can alias unrelated hardware in the
	 * ADSP's address space.
	 */
	buffer = dma_alloc_coherent(&g_core->adev->dev, buffer_size, &iova,
				    GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto release_firmware;
	}
	memset(buffer, 0, buffer_size);
	memcpy(buffer, fw->data, payload_size);
	dsp_addr = q6voice_dsp_address(&g_core->adev->dev, iova);
	release_firmware(fw);

	mutex_lock(&g_core->lock);
	if (g_core->custom_topologies_registered) {
		ret = 0;
		goto free_buffer;
	}
	if (g_core->custom_topologies_failed) {
		ret = -EIO;
		goto free_buffer;
	}

	dev_info(&g_core->adev->dev,
		 "custom topology buffer iova %#llx, dsp address %#llx, size %u\n",
		 (u64)iova, dsp_addr, payload_size);

	map.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					 APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	map.hdr.pkt_size = sizeof(map);
	map.hdr.opcode = AVCS_CMD_SHARED_MEM_MAP_REGIONS;
	map.mem_pool_id = ADSP_MEMORY_MAP_SHMEM8_4K_POOL;
	map.num_regions = 1;
	map.shm_addr_lsw = lower_32_bits(dsp_addr);
	map.shm_addr_msw = upper_32_bits(dsp_addr);
	map.mem_size_bytes = buffer_size;
	g_core->mem_map_handle = 0;

	ret = q6core_send_and_wait_topology(g_core, (struct apr_pkt *)&map,
					 "map custom topologies");
	if (ret) {
		if (ret == -ETIMEDOUT) {
			g_core->custom_topologies_failed = true;
			keep_buffer = true;
		}
		goto free_buffer;
	}
	map_handle = g_core->mem_map_handle;
	if (!map_handle) {
		dev_err(&g_core->adev->dev,
			"map custom topologies returned no handle\n");
		g_core->custom_topologies_failed = true;
		keep_buffer = true;
		ret = -EIO;
		goto free_buffer;
	}

	register_topologies.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
						 APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	register_topologies.hdr.pkt_size = sizeof(register_topologies);
	register_topologies.hdr.opcode = AVCS_CMD_REGISTER_TOPOLOGIES;
	register_topologies.payload_addr_lsw = lower_32_bits(dsp_addr);
	register_topologies.payload_addr_msw = upper_32_bits(dsp_addr);
	register_topologies.mem_map_handle = map_handle;
	register_topologies.payload_size = payload_size;

	ret = q6core_send_and_wait_topology(g_core,
					 (struct apr_pkt *)&register_topologies,
					 "register custom topologies");
	if (ret == -ETIMEDOUT) {
		g_core->custom_topologies_failed = true;
		keep_buffer = true;
		goto free_buffer;
	}
	if (ret)
		goto unmap_buffer;
	g_core->custom_topologies_registered = true;
unmap_buffer:

	unmap.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	unmap.hdr.pkt_size = sizeof(unmap);
	unmap.hdr.opcode = AVCS_CMD_SHARED_MEM_UNMAP_REGIONS;
	unmap.mem_map_handle = map_handle;
	unmap_ret = q6core_send_and_wait_topology(g_core,
					       (struct apr_pkt *)&unmap,
					       "unmap custom topologies");
	if (unmap_ret) {
		g_core->custom_topologies_failed = true;
		keep_buffer = true;
	}
	if (!ret && unmap_ret)
		ret = unmap_ret;

	if (!ret)
		dev_info(&g_core->adev->dev,
			 "registered %u bytes of custom topologies from %s\n",
			 payload_size, firmware_name);
free_buffer:
	mutex_unlock(&g_core->lock);
	if (keep_buffer)
		dev_err(&g_core->adev->dev,
			"retaining custom topology DMA page after uncertain DSP state\n");
	else
		dma_free_coherent(&g_core->adev->dev, buffer_size, buffer, iova);
	return ret;

release_firmware:
	release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL_GPL(q6core_register_custom_topologies);

/**
 * q6core_get_svc_api_info() - Get version number of a service.
 *
 * @svc_id: service id of the service.
 * @ainfo: Valid struct pointer to fill svc api information.
 *
 * Return: zero on success and error code on failure or unsupported
 */
/*
 * Ask the ADSP to load the modules making up a topology. The voice path needs
 * this before VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT: the commit publishes modules
 * that must already have been loaded, so without it there is nothing to commit.
 */
int q6core_load_topo_modules(u32 topology_id)
{
	struct avcs_cmd_load_topo_modules {
		struct apr_hdr hdr;
		u32 topology_id;
	} __packed pkt;
	int rc;

	if (!g_core)
		return -ENODEV;

	memset(&pkt, 0, sizeof(pkt));
	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					  APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = sizeof(pkt);
	pkt.hdr.opcode = AVCS_CMD_LOAD_TOPO_MODULES;
	pkt.topology_id = topology_id;

	mutex_lock(&g_core->lock);
	g_core->cmd_status = 0;
	g_core->resp_received = false;
	rc = apr_send_pkt(g_core->adev, (struct apr_pkt *)&pkt);
	if (rc < 0)
		goto out;

	rc = wait_event_timeout(g_core->wait, (g_core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && g_core->resp_received) {
		g_core->resp_received = false;
		rc = g_core->cmd_status ? -EIO : 0;
		if (rc)
			dev_err(&g_core->adev->dev,
				"load topo modules %#x rejected: %#x\n",
				topology_id, g_core->cmd_status);
	} else {
		rc = -ETIMEDOUT;
	}
out:
	mutex_unlock(&g_core->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(q6core_load_topo_modules);

int q6core_get_svc_api_info(int svc_id, struct q6core_svc_api_info *ainfo)
{
	int i;
	int ret = -ENOTSUPP;

	if (!g_core || !ainfo)
		return 0;

	mutex_lock(&g_core->lock);
	if (!g_core->is_version_requested) {
		if (q6core_get_fwk_versions(g_core) == -ENOTSUPP)
			q6core_get_svc_versions(g_core);
		g_core->is_version_requested = true;
	}

	if (g_core->fwk_version_supported) {
		for (i = 0; i < g_core->fwk_version->num_services; i++) {
			struct avcs_svc_api_info *info;

			info = &g_core->fwk_version->svc_api_info[i];
			if (svc_id != info->service_id)
				continue;

			ainfo->api_version = info->api_version;
			ainfo->api_branch_version = info->api_branch_version;
			ret = 0;
			break;
		}
	} else if (g_core->get_version_supported) {
		for (i = 0; i < g_core->svc_version->num_services; i++) {
			struct avcs_svc_info *info;

			info = &g_core->svc_version->svc_api_info[i];
			if (svc_id != info->service_id)
				continue;

			ainfo->api_version = info->version;
			ainfo->api_branch_version = 0;
			ret = 0;
			break;
		}
	}

	mutex_unlock(&g_core->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(q6core_get_svc_api_info);

/**
 * q6core_is_adsp_ready() - Get status of adsp
 *
 * Return: Will be an true if adsp is ready and false if not.
 */
bool q6core_is_adsp_ready(void)
{
	unsigned long  timeout;
	bool ret = false;

	if (!g_core)
		return false;

	mutex_lock(&g_core->lock);
	timeout = jiffies + msecs_to_jiffies(ADSP_STATE_READY_TIMEOUT_MS);
	for (;;) {
		if (__q6core_is_adsp_ready(g_core)) {
			ret = true;
			break;
		}

		if (!time_after(timeout, jiffies)) {
			ret = false;
			break;
		}
	}

	mutex_unlock(&g_core->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6core_is_adsp_ready);

static int q6core_probe(struct apr_device *adev)
{
	g_core = kzalloc_obj(*g_core);
	if (!g_core)
		return -ENOMEM;

	dev_set_drvdata(&adev->dev, g_core);

	mutex_init(&g_core->lock);
	g_core->adev = adev;
	init_waitqueue_head(&g_core->wait);
	return 0;
}

static void q6core_exit(struct apr_device *adev)
{
	struct q6core *core = dev_get_drvdata(&adev->dev);

	if (core->fwk_version_supported)
		kfree(core->fwk_version);
	if (core->get_version_supported)
		kfree(core->svc_version);

	g_core = NULL;
	kfree(core);
}

#ifdef CONFIG_OF
static const struct of_device_id q6core_device_id[]  = {
	{ .compatible = "qcom,q6core" },
	{},
};
MODULE_DEVICE_TABLE(of, q6core_device_id);
#endif

static struct apr_driver qcom_q6core_driver = {
	.probe = q6core_probe,
	.remove = q6core_exit,
	.callback = q6core_callback,
	.driver = {
		.name = "qcom-q6core",
		.of_match_table = of_match_ptr(q6core_device_id),
	},
};

module_apr_driver(qcom_q6core_driver);
MODULE_DESCRIPTION("q6 core");
MODULE_LICENSE("GPL v2");
