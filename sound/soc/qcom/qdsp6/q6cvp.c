// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/apr.h>
#include "q6cvp.h"
#include "q6voice-common.h"

#define VSS_IVOCPROC_DIRECTION_RX	0
#define VSS_IVOCPROC_DIRECTION_TX	1
#define VSS_IVOCPROC_DIRECTION_RX_TX	2

#define VSS_IVOCPROC_PORT_ID_NONE	0xFFFF

#define VSS_IVOCPROC_TOPOLOGY_ID_NONE			0x00010F70
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_DM_FLUENCE		0x00010F72


#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING		0x00010F7C
#define VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING		0x00010F7D

#define VSS_ICOMMON_CAL_NETWORK_ID_NONE			0x0001135E

#define VSS_IVOCPROC_CMD_ENABLE				0x000100C6
#define VSS_IVOCPROC_CMD_DISABLE			0x000110E1
#define VSS_ICOMMON_CMD_SET_PARAM_V2			0x0001133D
#define VSS_MODULE_CVD_GENERIC				0x0001316E
#define VSS_PARAM_VOCPROC_TX_CHANNEL_INFO		0x0001328E
#define VSS_PARAM_VOCPROC_RX_CHANNEL_INFO		0x0001328F
#define VSS_NUM_CHANNELS_MAX				32
#define PCM_CHANNEL_FC					3

/*
 * Describe how many channels each side of the vocproc carries. The ADSP will
 * not accept VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT until both directions have been
 * described, and without the commit the vocproc is created but never runs.
 * Narrowband call audio is mono 16-bit, mapped to front centre.
 */
struct vss_param_vocproc_dev_channel_info {
	u32 num_channels;
	u32 bits_per_sample;
	u8 channel_mapping[VSS_NUM_CHANNELS_MAX];
} __packed;

struct vss_icommon_param_data_channel_info {
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
	struct vss_param_vocproc_dev_channel_info channel_info;
} __packed;

struct vss_icommon_cmd_set_param_channel_info {
	struct apr_hdr hdr;
	/* zero: the payload is in-band, immediately below */
	u32 mem_handle;
	u64 mem_address;
	u32 mem_size;
	struct vss_icommon_param_data_channel_info param_data;
} __packed;

#define VSS_ICOMMON_CMD_SET_PARAM_V3			0x00013245
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO		0x00013253
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO		0x00013254

/*
 * Media format for each endpoint. Unlike the channel info above this goes
 * through the newer parameter path: the ADSP reports instance-ID support, so
 * the header is a v3 and the command is SET_PARAM_V3. Narrowband call audio is
 * mono 16-bit at 8 kHz.
 */
struct param_hdr_v3 {
	u32 module_id;
	u16 instance_id;
	u16 reserved;
	u32 param_id;
	u32 param_size;
} __packed;

struct vss_param_endpoint_media_format_info {
	u32 port_id;
	u16 num_channels;
	u16 bits_per_sample;
	u32 sample_rate;
	u8 channel_mapping[VSS_NUM_CHANNELS_MAX];
} __packed;

struct vss_icommon_cmd_set_param_media_format {
	struct apr_hdr hdr;
	/* zero: the payload is in-band, immediately below */
	u32 mem_handle;
	u64 mem_address;
	u32 payload_size;
	struct param_hdr_v3 param_hdr;
	struct vss_param_endpoint_media_format_info media_format;
} __packed;

#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT		0x00013198

#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2	0x000112BF

struct vss_ivocproc_cmd_create_full_control_session_v2_cmd {
	struct apr_hdr hdr;

	/*
	 * Vocproc direction. The supported values:
	 * VSS_IVOCPROC_DIRECTION_RX
	 * VSS_IVOCPROC_DIRECTION_TX
	 * VSS_IVOCPROC_DIRECTION_RX_TX
	 */
	u16 direction;

	/*
	 * Tx device port ID to which the vocproc connects. If a port ID is
	 * not being supplied, set this to #VSS_IVOCPROC_PORT_ID_NONE.
	 */
	u16 tx_port_id;

	/*
	 * Tx path topology ID. If a topology ID is not being supplied, set
	 * this to #VSS_IVOCPROC_TOPOLOGY_ID_NONE.
	 */
	u32 tx_topology_id;

	/*
	 * Rx device port ID to which the vocproc connects. If a port ID is
	 * not being supplied, set this to #VSS_IVOCPROC_PORT_ID_NONE.
	 */
	u16 rx_port_id;

	/*
	 * Rx path topology ID. If a topology ID is not being supplied, set
	 * this to #VSS_IVOCPROC_TOPOLOGY_ID_NONE.
	 */
	u32 rx_topology_id;

	/* Voice calibration profile ID. */
	u32 profile_id;

	/*
	 * Vocproc mode. The supported values:
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING
	 */
	u32 vocproc_mode;

	/*
	 * Port ID to which the vocproc connects for receiving echo
	 * cancellation reference signal. If a port ID is not being supplied,
	 * set this to #VSS_IVOCPROC_PORT_ID_NONE. This parameter value is
	 * ignored when the vocproc_mode parameter is set to
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING.
	 */
	u16 ec_ref_port_id;

	/*
	 * Session name string used to identify a session that can be shared
	 * with passive controllers (optional).
	 */
	char name[20];
} __packed;

struct q6voice_session *q6cvp_session_create(enum q6voice_path_type path,
					     u16 tx_port, u16 rx_port)
{
	struct vss_ivocproc_cmd_create_full_control_session_v2_cmd cmd;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2;

	/* TODO: Implement calibration */
	cmd.tx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
	cmd.rx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;

	cmd.direction = VSS_IVOCPROC_DIRECTION_RX_TX;
	cmd.tx_port_id = tx_port;
	cmd.rx_port_id = rx_port;
	cmd.profile_id = VSS_ICOMMON_CAL_NETWORK_ID_NONE;
	cmd.vocproc_mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
	cmd.ec_ref_port_id = VSS_IVOCPROC_PORT_ID_NONE;

	return q6voice_session_create(Q6VOICE_SERVICE_CVP, path, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_session_create);

static int q6cvp_set_channel_info_one(struct q6voice_session *cvp, u32 param_id)
{
	struct vss_icommon_cmd_set_param_channel_info cmd = {0};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V2;

	cmd.mem_size = sizeof(cmd.param_data);

	cmd.param_data.module_id = VSS_MODULE_CVD_GENERIC;
	cmd.param_data.param_id = param_id;
	cmd.param_data.param_size = sizeof(cmd.param_data.channel_info);

	cmd.param_data.channel_info.num_channels = 1;
	cmd.param_data.channel_info.bits_per_sample = 16;
	cmd.param_data.channel_info.channel_mapping[0] = PCM_CHANNEL_FC;

	return q6voice_common_send(cvp, &cmd.hdr);
}

int q6cvp_set_channel_info(struct q6voice_session *cvp)
{
	int ret;

	ret = q6cvp_set_channel_info_one(cvp, VSS_PARAM_VOCPROC_RX_CHANNEL_INFO);
	if (ret)
		return ret;

	return q6cvp_set_channel_info_one(cvp, VSS_PARAM_VOCPROC_TX_CHANNEL_INFO);
}
EXPORT_SYMBOL_GPL(q6cvp_set_channel_info);

static int q6cvp_set_media_format_one(struct q6voice_session *cvp, u32 param_id,
				      u16 port_id)
{
	struct vss_icommon_cmd_set_param_media_format cmd = {0};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V3;

	cmd.payload_size = sizeof(cmd.param_hdr) + sizeof(cmd.media_format);

	cmd.param_hdr.module_id = VSS_MODULE_CVD_GENERIC;
	cmd.param_hdr.param_id = param_id;
	cmd.param_hdr.param_size = sizeof(cmd.media_format);

	cmd.media_format.port_id = port_id;
	cmd.media_format.num_channels = 1;
	cmd.media_format.bits_per_sample = 16;
	cmd.media_format.sample_rate = 8000;
	cmd.media_format.channel_mapping[0] = PCM_CHANNEL_FC;

	return q6voice_common_send(cvp, &cmd.hdr);
}

int q6cvp_set_media_format(struct q6voice_session *cvp, u16 tx_port, u16 rx_port)
{
	int ret;

	ret = q6cvp_set_media_format_one(cvp,
					 VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO,
					 rx_port);
	if (ret)
		return ret;

	return q6cvp_set_media_format_one(cvp,
					  VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO,
					  tx_port);
}
EXPORT_SYMBOL_GPL(q6cvp_set_media_format);

/*
 * Tell the vocproc its topology is fully described and may be brought up.
 * The vendor treats this as mandatory between creating the vocproc and
 * enabling it -- without it the session is accepted but stays inert.
 */
int q6cvp_topology_commit(struct q6voice_session *cvp)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_topology_commit);

int q6cvp_enable(struct q6voice_session *cvp, bool state)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = state ? VSS_IVOCPROC_CMD_ENABLE : VSS_IVOCPROC_CMD_DISABLE;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_enable);

static int q6cvp_probe(struct apr_device *adev)
{
	return q6voice_common_probe(adev, Q6VOICE_SERVICE_CVP);
}

static const struct of_device_id q6cvp_device_id[]  = {
	{ .compatible = "qcom,q6cvp" },
	{},
};
MODULE_DEVICE_TABLE(of, q6cvp_device_id);

static struct apr_driver qcom_q6cvp_driver = {
	.probe = q6cvp_probe,
	.remove = q6voice_common_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6cvp",
		.of_match_table = of_match_ptr(q6cvp_device_id),
	},
};

module_apr_driver(qcom_q6cvp_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Core Voice Processor");
MODULE_LICENSE("GPL v2");
