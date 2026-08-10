// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/apr.h>
#include <linux/string.h>
#include "q6cvs.h"
#include "q6voice-common.h"

#define VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x00011140

#define VSS_IVOLUME_CMD_MUTE_V2				0x0001138B

#define VSS_IVOLUME_MUTE_OFF				0
#define VSS_IVOLUME_MUTE_ON				1

struct vss_ivolume_cmd_mute_v2 {
	struct apr_hdr hdr;

	u16 direction;
	u16 mute_flag;
	u16 ramp_duration_ms;
} __packed;

struct vss_istream_cmd_create_control_session_cmd {
	struct apr_hdr hdr;

	/* A variable-sized stream name. */
	char name[20];
} __packed;

/*
 * The stream session is what actually carries voice packets. For a modem-driven
 * (circuit switched) call it is passive: the modem owns the vocoder and drives
 * the exchange, we only have to create the session and let the MVM attach it.
 * Without it the ADSP has a vocproc but no stream to feed, which looks like a
 * perfectly healthy but completely silent call.
 */
struct q6voice_session *q6cvs_session_create(enum q6voice_path_type path)
{
	struct vss_istream_cmd_create_control_session_cmd cmd = {0};
	const char *session_name;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION;

	session_name = q6voice_session_name(path);
	if (session_name)
		strscpy(cmd.name, session_name, sizeof(cmd.name));

	return q6voice_session_create(Q6VOICE_SERVICE_CVS, path, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvs_session_create);

/*
 * Mute state of the stream itself, as opposed to the device. The vendor sends
 * this for the uplink on every call rather than trusting the ADSP's default.
 */
int q6cvs_set_mute(struct q6voice_session *cvs, u16 direction, bool mute,
		   u16 ramp_ms)
{
	struct vss_ivolume_cmd_mute_v2 cmd = {0};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOLUME_CMD_MUTE_V2;

	cmd.direction = direction;
	cmd.mute_flag = mute ? VSS_IVOLUME_MUTE_ON : VSS_IVOLUME_MUTE_OFF;
	cmd.ramp_duration_ms = ramp_ms;

	return q6voice_common_send(cvs, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvs_set_mute);

static int q6cvs_probe(struct apr_device *adev)
{
	return q6voice_common_probe(adev, Q6VOICE_SERVICE_CVS);
}

static const struct of_device_id q6cvs_device_id[]  = {
	{ .compatible = "qcom,q6cvs" },
	{},
};
MODULE_DEVICE_TABLE(of, q6cvs_device_id);

static struct apr_driver qcom_q6cvs_driver = {
	.probe = q6cvs_probe,
	.remove = q6voice_common_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6cvs",
		.of_match_table = of_match_ptr(q6cvs_device_id),
	},
};

module_apr_driver(qcom_q6cvs_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Core Voice Stream");
MODULE_LICENSE("GPL v2");
