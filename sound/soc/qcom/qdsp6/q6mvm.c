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

static inline const char *q6mvm_session_name(enum q6voice_path_type path)
{
	switch (path) {
	case Q6VOICE_PATH_VOICE:
		return "default modem voice";
	default:
		return NULL;
	}
}

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
	struct vss_imvm_cmd_create_control_session_cmd cmd;
	struct q6voice_session *mvm;
	const char *session_name;
	int ret;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION;

	session_name = q6mvm_session_name(path);
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
