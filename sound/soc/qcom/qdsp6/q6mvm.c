// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/soc/qcom/apr.h>
#include "q6mvm.h"
#include "q6voice-common.h"

#define VSS_IVERSION_CMD_GET				0x00011378
#define VSS_IVERSION_RSP_GET				0x00011379
#define CVD_VERSION_STRING_MAX_SIZE			31

#define VSS_IPKTEXG_CMD_SET_MAILBOX_MEMORY_CONFIG	0x0001333B
/*
 * The modem's own request for the same configuration, and the reply that
 * carries it. The ADSP keeps one copy in a single global, answers this from it
 * whoever asks, and replies with a plain error when nothing has been stored --
 * so asking it ourselves is a direct read of the state the modem sees.
 */
#define VSS_IPKTEXG_CMD_REQUEST_MAILBOX_MEMORY_CONFIG	0x0001333C
#define VSS_IPKTEXG_RSP_MAILBOX_MEMORY_CONFIG		0x0001333D

/*
 * Tell the ADSP where the shared voice mailbox lives. On boards where the modem
 * sits behind PCIe the two agents reach the same buffer through different
 * IOMMUs, so both translations have to be handed over. Not tied to a session:
 * the ADSP accepts it before any call exists.
 */
struct vss_ipktexg_cmd_set_mailbox_memory_config {
	struct apr_hdr hdr;
	u64 mailbox_mem_address_adsp;
	u64 mailbox_mem_address_pcie;
	u32 mem_size;
} __packed;

/* The same three values coming back, without a header of their own. */
struct vss_ipktexg_rsp_mailbox_memory_config {
	u64 mailbox_mem_address_adsp;
	u64 mailbox_mem_address_pcie;
	u32 mem_size;
} __packed;

#define VSS_IMEMORY_CMD_MAP_PHYSICAL			0x00011334
#define VSS_IMEMORY_RSP_MAP				0x00011336
#define VSS_IMEMORY_CMD_UNMAP				0x00011337

/*
 * Lend the ADSP a block of memory. Calibration is too big to pass inline, so
 * it is placed in memory the ADSP is told about once and then referred to by
 * the handle this returns.
 *
 * The block list is not passed directly: the command points at a table that
 * itself lives in memory, which is why there are two levels of address here.
 * Both are addresses in the ADSP's own IOMMU domain despite the command's
 * name -- "physical" refers to the memory being contiguous, not to whose
 * address space the numbers belong to.
 */
struct vss_imemory_table_descriptor {
	u32 mem_address_lsw;
	u32 mem_address_msw;
	u32 mem_size;
} __packed;

struct vss_imemory_block {
	u64 mem_address;
	u32 mem_size;
} __packed;

struct vss_imemory_table {
	struct vss_imemory_table_descriptor next_table_descriptor;
	struct vss_imemory_block blocks[1];
} __packed;

struct vss_imemory_cmd_map_physical {
	struct apr_hdr hdr;
	struct vss_imemory_table_descriptor table_descriptor;
	bool is_cached;
	u16 cache_line_size;
	u32 access_mask;
	u32 page_align;
	u8 min_data_width;
	u8 max_data_width;
} __packed;

struct vss_imemory_cmd_unmap {
	struct apr_hdr hdr;
	u32 mem_handle;
} __packed;

/* What the ADSP supports; the values are not negotiable. */
#define VSS_IMEMORY_CACHE_LINE_SIZE	128
#define VSS_IMEMORY_PAGE_ALIGN		4096
#define VSS_IMEMORY_ACCESS_READ		BIT(0)
#define VSS_IMEMORY_ACCESS_WRITE	BIT(1)
#define VSS_IMEMORY_MIN_DATA_WIDTH	8
#define VSS_IMEMORY_MAX_DATA_WIDTH	64

#define VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x000110FF

struct vss_imvm_cmd_create_control_session_cmd {
	struct apr_hdr hdr;

	/* A variable-sized stream name. */
	char name[20];
} __packed;

#define VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL		0x00011327

/* This command is required to let MVM know who is in control of session. */
struct vss_imvm_cmd_set_policy_dual_control_cmd {
	struct apr_hdr hdr;

	/* Set to TRUE to enable modem state machine control */
	bool enable;
} __packed;

#define VSS_IMVM_CMD_ATTACH_STREAM			0x0001123C
#define VSS_IMVM_CMD_DETACH_STREAM			0x0001123D

/* Attach/detach a stream (CVS) session to the MVM. */
struct vss_imvm_cmd_attach_stream_cmd {
	struct apr_hdr hdr;

	/* Handle of the stream session to attach */
	u16 handle;
} __packed;

#define VSS_IMVM_CMD_ATTACH_VOCPROC			0x0001123E
#define VSS_IMVM_CMD_DETACH_VOCPROC			0x0001123F

/*
 * Attach/detach a vocproc to the MVM.
 * The MVM will symmetrically connect/disconnect this vocproc
 * to/from all the streams currently attached to it.
 */
struct vss_imvm_cmd_attach_vocproc_cmd {
	struct apr_hdr hdr;

	/* Handle of vocproc being attached. */
	u16 handle;
} __packed;

#define VSS_IMVM_CMD_START_VOICE			0x00011190
#define VSS_IMVM_CMD_STOP_VOICE				0x00011192

#define VSS_ISTREAM_CMD_SET_TTY_MODE			0x00011196

struct vss_istream_cmd_set_tty_mode {
	struct apr_hdr hdr;

	u32 mode;
} __packed;

static int q6mvm_set_dual_control(struct q6voice_session *mvm)
{
	struct vss_imvm_cmd_set_policy_dual_control_cmd cmd;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL;

	cmd.enable = true;

	return q6voice_common_send(mvm, &cmd.hdr);
}

struct q6voice_session *q6mvm_session_create(enum q6voice_path_type path)
{
	struct vss_imvm_cmd_create_control_session_cmd cmd = {0};
	struct q6voice_session *mvm;
	const char *session_name;
	int ret;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION;

	session_name = q6voice_session_name(path);
	if (session_name)
		strscpy(cmd.name, session_name, sizeof(cmd.name));

	mvm = q6voice_session_create(Q6VOICE_SERVICE_MVM, path, &cmd.hdr);
	if (IS_ERR(mvm))
		return mvm;

	ret = q6mvm_set_dual_control(mvm);
	if (ret) {
		dev_err(mvm->dev, "failed to set dual control: %d\n", ret);
		q6voice_session_release(mvm);
		return ERR_PTR(ret);
	}

	return mvm;
}
EXPORT_SYMBOL_GPL(q6mvm_session_create);

int q6mvm_attach(struct q6voice_session *mvm, struct q6voice_session *cvp,
		 bool state)
{
	struct vss_imvm_cmd_attach_vocproc_cmd cmd;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = state ? VSS_IMVM_CMD_ATTACH_VOCPROC : VSS_IMVM_CMD_DETACH_VOCPROC;

	cmd.handle = cvp->handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_attach);

int q6mvm_attach_stream(struct q6voice_session *mvm, struct q6voice_session *cvs,
			bool state)
{
	struct vss_imvm_cmd_attach_stream_cmd cmd;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = state ? VSS_IMVM_CMD_ATTACH_STREAM : VSS_IMVM_CMD_DETACH_STREAM;

	cmd.handle = cvs->handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_attach_stream);

int q6mvm_set_tty_mode(struct q6voice_session *mvm, u32 mode)
{
	struct vss_istream_cmd_set_tty_mode cmd = {0};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_ISTREAM_CMD_SET_TTY_MODE;
	cmd.mode = mode;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_set_tty_mode);

int q6mvm_start(struct q6voice_session *mvm, bool state)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = state ? VSS_IMVM_CMD_START_VOICE : VSS_IMVM_CMD_STOP_VOICE;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_start);

/*
 * Ask the ADSP which CVD it is running. The vendor driver gates most of the
 * vocproc configuration on this: topology commit needs >= 2.2 and the per-path
 * media format commands >= 2.3, so without it we cannot tell an unsupported
 * command from a missing prerequisite.
 */
int q6mvm_get_cvd_version(char *version, size_t len)
{
	char rsp[CVD_VERSION_STRING_MAX_SIZE + 1] = {0};
	struct apr_pkt cmd = {0};
	int ret;

	cmd.hdr.opcode = VSS_IVERSION_CMD_GET;

	ret = q6voice_common_send_svc_rsp(Q6VOICE_SERVICE_MVM, &cmd.hdr,
					  APR_HDR_SIZE, VSS_IVERSION_RSP_GET,
					  rsp, CVD_VERSION_STRING_MAX_SIZE);
	if (ret)
		return ret;

	rsp[CVD_VERSION_STRING_MAX_SIZE] = '\0';
	strscpy(version, rsp, len);

	return 0;
}
EXPORT_SYMBOL_GPL(q6mvm_get_cvd_version);

int q6mvm_set_mailbox_memory(u64 adsp_iova, u64 pcie_iova, u32 size)
{
	struct vss_ipktexg_cmd_set_mailbox_memory_config cmd = {0};

	cmd.hdr.opcode = VSS_IPKTEXG_CMD_SET_MAILBOX_MEMORY_CONFIG;
	cmd.mailbox_mem_address_adsp = adsp_iova;
	cmd.mailbox_mem_address_pcie = pcie_iova;
	cmd.mem_size = size;

	return q6voice_common_send_svc(Q6VOICE_SERVICE_MVM, &cmd.hdr,
				       sizeof(cmd));
}
EXPORT_SYMBOL_GPL(q6mvm_set_mailbox_memory);

/*
 * Read back the mailbox configuration the ADSP is holding.
 *
 * This is the request the modem itself makes while setting up a call, and it is
 * answered out of the same single global the command above writes. Sending it
 * from here therefore answers the one question the AP cannot otherwise settle:
 * whether the values the ADSP will hand the modem are the ones we sent. A
 * command that was acknowledged tells us the packet was well formed; only this
 * tells us it was kept.
 */
int q6mvm_get_mailbox_memory(u64 *adsp_iova, u64 *pcie_iova, u32 *size)
{
	struct vss_ipktexg_rsp_mailbox_memory_config rsp = {0};
	struct apr_hdr hdr = {0};
	int ret;

	hdr.opcode = VSS_IPKTEXG_CMD_REQUEST_MAILBOX_MEMORY_CONFIG;

	ret = q6voice_common_send_svc_rsp(Q6VOICE_SERVICE_MVM, &hdr,
					  sizeof(hdr),
					  VSS_IPKTEXG_RSP_MAILBOX_MEMORY_CONFIG,
					  &rsp, sizeof(rsp));
	if (ret)
		return ret;

	*adsp_iova = rsp.mailbox_mem_address_adsp;
	*pcie_iova = rsp.mailbox_mem_address_pcie;
	*size = rsp.mem_size;

	return 0;
}
EXPORT_SYMBOL_GPL(q6mvm_get_mailbox_memory);

/*
 * Hand @table_addr (a table describing the blocks being lent) to the ADSP and
 * return the handle it answers with. The caller keeps both the table and the
 * blocks alive until q6mvm_unmap_memory(), since the ADSP reads them whenever
 * it pleases.
 */
int q6mvm_map_memory(struct q6voice_session *mvm, dma_addr_t table_addr,
		     u32 table_size, u32 *handle)
{
	struct vss_imemory_cmd_map_physical cmd = {0};
	u32 rsp = 0;
	int ret;

	cmd.hdr.opcode = VSS_IMEMORY_CMD_MAP_PHYSICAL;
	cmd.table_descriptor.mem_address_lsw = lower_32_bits(table_addr);
	cmd.table_descriptor.mem_address_msw = upper_32_bits(table_addr);
	cmd.table_descriptor.mem_size = table_size;
	cmd.is_cached = true;
	cmd.cache_line_size = VSS_IMEMORY_CACHE_LINE_SIZE;
	cmd.access_mask = VSS_IMEMORY_ACCESS_READ | VSS_IMEMORY_ACCESS_WRITE;
	cmd.page_align = VSS_IMEMORY_PAGE_ALIGN;
	cmd.min_data_width = VSS_IMEMORY_MIN_DATA_WIDTH;
	cmd.max_data_width = VSS_IMEMORY_MAX_DATA_WIDTH;

	/*
	 * Addressed to the MVM session, not to the service: the ADSP wedges on
	 * a memory command that belongs to nothing. The reply carries the
	 * handle rather than a basic result.
	 */
	ret = q6voice_common_send_svc_rsp_port(Q6VOICE_SERVICE_MVM, &cmd.hdr,
					       sizeof(cmd), mvm->handle,
					       VSS_IMEMORY_RSP_MAP,
					       &rsp, sizeof(rsp));
	if (ret)
		return ret;

	if (!rsp)
		return -EINVAL;

	*handle = rsp;
	return 0;
}
EXPORT_SYMBOL_GPL(q6mvm_map_memory);

/* Addressed to the session that lent the memory, as the map command is. */
int q6mvm_unmap_memory(struct q6voice_session *mvm, u32 handle)
{
	struct vss_imemory_cmd_unmap cmd = {0};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMEMORY_CMD_UNMAP;
	cmd.mem_handle = handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_unmap_memory);

static int q6mvm_probe(struct apr_device *adev)
{
	int ret;

	ret = q6voice_common_probe(adev, Q6VOICE_SERVICE_MVM);
	if (ret)
		return ret;

	return of_platform_populate(adev->dev.of_node, NULL, NULL, &adev->dev);
}

static void q6mvm_remove(struct apr_device *adev)
{
	of_platform_depopulate(&adev->dev);
	q6voice_common_remove(adev);
}

static const struct of_device_id q6mvm_device_id[]  = {
	{ .compatible = "qcom,q6mvm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6mvm_device_id);

static struct apr_driver qcom_q6mvm_driver = {
	.probe = q6mvm_probe,
	.remove = q6mvm_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6mvm",
		.of_match_table = of_match_ptr(q6mvm_device_id),
	},
};

module_apr_driver(qcom_q6mvm_driver);
MODULE_DESCRIPTION("Q6 Multimode Voice Manager");
MODULE_LICENSE("GPL v2");
