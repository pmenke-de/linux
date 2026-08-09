/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_VOICE_COMMON_H
#define _Q6_VOICE_COMMON_H

#include <linux/soc/qcom/apr.h>
#include "q6voice.h"

enum q6voice_service_type {
	Q6VOICE_SERVICE_MVM,
	Q6VOICE_SERVICE_CVP,
	Q6VOICE_SERVICE_CVS,
	Q6VOICE_SERVICE_COUNT
};

struct q6voice_service;

struct q6voice_session {
	struct device *dev;
	struct q6voice_service *svc;
	struct kref refcount;

	u16 port;
	u16 handle;

	wait_queue_head_t wait;

	/* Protect expected_opcode and result */
	spinlock_t lock;
	u32 expected_opcode;
	u32 result;
};

/*
 * Port used for commands that address a service rather than a session, such as
 * the mailbox memory configuration. The ADSP echoes it back as dest_port, which
 * is how the callback tells them apart from per-session replies.
 */
#define Q6VOICE_SVC_PORT	0x0103

int q6voice_common_probe(struct apr_device *adev, enum q6voice_service_type type);
void q6voice_common_remove(struct apr_device *adev);

int q6voice_common_callback(struct apr_device *adev, const struct apr_resp_pkt *data);
int q6voice_common_send(struct q6voice_session *s, struct apr_hdr *hdr);
int q6voice_common_send_svc(enum q6voice_service_type type, struct apr_hdr *hdr,
			    u32 size);
/*
 * As above, but for commands answered with a payload of their own rather than
 * a basic result. @rsp_opcode is the reply to wait for; up to @rsp_size bytes
 * of its payload are copied into @rsp.
 */
int q6voice_common_send_svc_rsp(enum q6voice_service_type type,
				struct apr_hdr *hdr, u32 size, u32 rsp_opcode,
				void *rsp, u32 rsp_size);

struct q6voice_session *q6voice_session_create(enum q6voice_service_type type,
					       enum q6voice_path_type path,
					       struct apr_hdr *hdr);
void q6voice_session_release(struct q6voice_session *s);

#endif /*_Q6_VOICE_COMMON_H */
