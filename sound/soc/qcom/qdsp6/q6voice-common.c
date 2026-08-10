// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/qcom/apr.h>
#include "q6voice-common.h"

#define APRV2_IBASIC_CMD_DESTROY_SESSION	0x0001003C

#define TIMEOUT_MS	300

struct q6voice_service {
	struct apr_device *adev;
	enum q6voice_service_type type;

	/* Protect sessions array */
	spinlock_t lock;
	struct q6voice_session *sessions[Q6VOICE_PATH_COUNT];

	/*
	 * Slot for a command addressed to the service itself rather than to a
	 * session. Only one may be outstanding, which is enough: these are
	 * one-off configuration commands issued outside the call path.
	 */
	struct mutex svc_lock;
	wait_queue_head_t svc_wait;
	u32 svc_expected_opcode;
	u32 svc_result;

	/* for replies that carry a payload instead of a basic result */
	u32 svc_rsp_opcode;
	void *svc_rsp;
	u32 svc_rsp_size;
};

/* Protect q6voice_services */
static DEFINE_SPINLOCK(q6voice_services_lock);
static struct q6voice_service *q6voice_services[Q6VOICE_SERVICE_COUNT] = {0};

/*
 * The ADSP can restart under us -- it has its own watchdog, and userspace can
 * ask for it. Its APR services disappear and come back when that happens, and
 * anything the ADSP was told before is gone with them: it comes back knowing
 * nothing about, say, where the voice mailbox lives. Drivers that configured
 * something once at probe need to hear about it so they can say it again.
 */
static void (*q6voice_svc_notifier)(enum q6voice_service_type type);

void q6voice_common_set_svc_notifier(void (*notify)(enum q6voice_service_type))
{
	q6voice_svc_notifier = notify;
}
EXPORT_SYMBOL_GPL(q6voice_common_set_svc_notifier);

int q6voice_common_probe(struct apr_device *adev, enum q6voice_service_type type)
{
	struct device *dev = &adev->dev;
	struct q6voice_service *svc, *current_svc;
	unsigned long flags;

	if (type >= Q6VOICE_SERVICE_COUNT)
		return -EINVAL;

	svc = devm_kzalloc(dev, sizeof(*svc), GFP_KERNEL);
	if (!svc)
		return -ENOMEM;

	svc->adev = adev;
	svc->type = type;
	spin_lock_init(&svc->lock);
	mutex_init(&svc->svc_lock);
	init_waitqueue_head(&svc->svc_wait);

	dev_set_drvdata(dev, svc);

	spin_lock_irqsave(&q6voice_services_lock, flags);
	current_svc = q6voice_services[type];
	if (!current_svc)
		q6voice_services[type] = svc;
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	if (current_svc)
		return -EEXIST;

	/* Announce it only once it can actually be sent to. */
	if (q6voice_svc_notifier)
		q6voice_svc_notifier(type);

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_probe);

void q6voice_common_remove(struct apr_device *adev)
{
	struct q6voice_service *svc = dev_get_drvdata(&adev->dev);
	enum q6voice_service_type type = svc->type;
	unsigned long flags;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	if (q6voice_services[type] == svc)
		q6voice_services[type] = NULL;
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	/* TODO: Should probably free up sessions here??? */
}
EXPORT_SYMBOL_GPL(q6voice_common_remove);

static void q6voice_session_free(struct kref *ref)
{
	struct q6voice_session *s = container_of(ref, struct q6voice_session,
						 refcount);

	kfree(s);
}

static int q6voice_session_destroy(struct q6voice_session *s)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = APRV2_IBASIC_CMD_DESTROY_SESSION;

	return q6voice_common_send(s, &cmd.hdr);
}

void q6voice_session_release(struct q6voice_session *s)
{
	struct q6voice_service *svc = s->svc;
	unsigned long flags;

	if (s->handle)
		q6voice_session_destroy(s);

	spin_lock_irqsave(&svc->lock, flags);
	if (svc->sessions[s->port] == s)
		svc->sessions[s->port] = NULL;
	spin_unlock_irqrestore(&svc->lock, flags);

	kref_put(&s->refcount, q6voice_session_free);
}
EXPORT_SYMBOL_GPL(q6voice_session_release);

struct q6voice_session *
q6voice_session_create(enum q6voice_service_type type,
		       enum q6voice_path_type path, struct apr_hdr *hdr)
{
	struct q6voice_service *svc;
	struct q6voice_session *s;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	svc = q6voice_services[type];
	spin_unlock_irqrestore(&q6voice_services_lock, flags);
	if (!svc)
		return ERR_PTR(-ENODEV);

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	s->dev = &svc->adev->dev;
	s->svc = svc;
	s->port = path;

	kref_init(&s->refcount);
	spin_lock_init(&s->lock);
	init_waitqueue_head(&s->wait);

	spin_lock_irqsave(&svc->lock, flags);
	if (svc->sessions[path]) {
		spin_unlock_irqrestore(&svc->lock, flags);
		kfree(s);
		return ERR_PTR(-EBUSY);
	}
	svc->sessions[path] = s;
	spin_unlock_irqrestore(&svc->lock, flags);

	dev_dbg(s->dev, "create session\n");

	ret = q6voice_common_send(s, hdr);
	if (ret)
		goto err;

	if (!s->handle) {
		dev_warn(s->dev, "failed to receive handle\n");
		ret = -EIO;
		goto err;
	}

	dev_dbg(s->dev, "handle: %d\n", s->handle);

	return s;

err:
	q6voice_session_release(s);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6voice_session_create);

static void q6voice_session_callback(struct q6voice_session *s,
				     const struct apr_resp_pkt *data)
{
	struct aprv2_ibasic_rsp_result_t *result = data->payload;
	unsigned long flags;

	if (data->hdr.opcode != APR_BASIC_RSP_RESULT)
		return; /* Not handled here */

	dev_dbg(s->dev, "basic result: opcode %#x, status: %#x\n",
		result->opcode, result->status);

	spin_lock_irqsave(&s->lock, flags);
	if (result->opcode != s->expected_opcode) {
		spin_unlock_irqrestore(&s->lock, flags);
		dev_warn(s->dev, "unexpected reply for opcode %#x (status: %#x)\n",
			 result->opcode, result->status);
		return;
	}

	if (!s->handle) {
		s->handle = data->hdr.src_port;
	} else if (s->handle != data->hdr.src_port) {
		spin_unlock_irqrestore(&s->lock, flags);
		dev_warn(s->dev, "unexpected reply for session %#x (!= %#x)\n",
			 data->hdr.src_port, s->handle);
		return;
	}

	s->result = result->status;
	s->expected_opcode = 0;
	spin_unlock_irqrestore(&s->lock, flags);

	wake_up(&s->wait);
}

/*
 * Send a command whose reply comes back to the service rather than to a
 * session. @dest_port selects what inside the ADSP is being addressed: zero
 * for the service itself, as the mailbox configuration uses, or a session
 * handle for commands that belong to an existing session even though their
 * reply carries its own payload. @size is the full packet size including the
 * header.
 */
static int q6voice_send_svc(enum q6voice_service_type type, struct apr_hdr *hdr,
			    u32 size, u16 dest_port, u32 rsp_opcode, void *rsp,
			    u32 rsp_size)
{
	struct q6voice_service *svc;
	unsigned long flags;
	int ret;

	if (type >= Q6VOICE_SERVICE_COUNT)
		return -EINVAL;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	svc = q6voice_services[type];
	spin_unlock_irqrestore(&q6voice_services_lock, flags);
	if (!svc)
		return -ENODEV;

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				       APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	hdr->pkt_size = size;
	hdr->src_port = Q6VOICE_SVC_PORT;
	hdr->dest_port = dest_port;
	hdr->token = 0;

	mutex_lock(&svc->svc_lock);

	svc->svc_expected_opcode = hdr->opcode;
	svc->svc_result = 0;
	svc->svc_rsp_opcode = rsp_opcode;
	svc->svc_rsp = rsp;
	svc->svc_rsp_size = rsp_size;

	ret = apr_send_pkt(svc->adev, (struct apr_pkt *)hdr);
	if (ret < 0)
		goto out;

	ret = wait_event_timeout(svc->svc_wait, (svc->svc_expected_opcode == 0),
				 msecs_to_jiffies(TIMEOUT_MS));
	if (!ret) {
		svc->svc_expected_opcode = 0;
		ret = -ETIMEDOUT;
		goto out;
	}

	if (svc->svc_result > 0) {
		dev_err(&svc->adev->dev, "command %#x failed with error %d\n",
			hdr->opcode, svc->svc_result);
		ret = -EIO;
		goto out;
	}

	ret = 0;
out:
	svc->svc_rsp_opcode = 0;
	svc->svc_rsp = NULL;
	mutex_unlock(&svc->svc_lock);
	return ret;
}

int q6voice_common_send_svc(enum q6voice_service_type type, struct apr_hdr *hdr,
			    u32 size)
{
	return q6voice_send_svc(type, hdr, size, 0, 0, NULL, 0);
}
EXPORT_SYMBOL_GPL(q6voice_common_send_svc);

int q6voice_common_send_svc_rsp(enum q6voice_service_type type,
				struct apr_hdr *hdr, u32 size, u32 rsp_opcode,
				void *rsp, u32 rsp_size)
{
	return q6voice_send_svc(type, hdr, size, 0, rsp_opcode, rsp, rsp_size);
}
EXPORT_SYMBOL_GPL(q6voice_common_send_svc_rsp);

/*
 * As above, but addressed to a session. Lending the ADSP memory is such a
 * command: it belongs to the MVM session, yet answers with a handle of its own
 * rather than a plain result. Addressing it to the service instead leaves the
 * ADSP with a command for nothing in particular, and it stops answering
 * altogether.
 */
int q6voice_common_send_svc_rsp_port(enum q6voice_service_type type,
				     struct apr_hdr *hdr, u32 size,
				     u16 dest_port, u32 rsp_opcode, void *rsp,
				     u32 rsp_size)
{
	return q6voice_send_svc(type, hdr, size, dest_port, rsp_opcode, rsp,
				rsp_size);
}
EXPORT_SYMBOL_GPL(q6voice_common_send_svc_rsp_port);

int q6voice_common_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	struct device *dev = &adev->dev;
	struct q6voice_service *v = dev_get_drvdata(dev);
	struct q6voice_session *s;
	unsigned long flags;

	dev_dbg(dev, "callback: %#x\n", data->hdr.opcode);

	/* Reply to a service-level command rather than to a session */
	if (data->hdr.dest_port == Q6VOICE_SVC_PORT) {
		struct aprv2_ibasic_rsp_result_t *result = data->payload;

		/* a reply carrying its own payload, e.g. a version string */
		if (v->svc_rsp_opcode &&
		    data->hdr.opcode == v->svc_rsp_opcode) {
			if (v->svc_rsp)
				memcpy(v->svc_rsp, data->payload,
				       min(v->svc_rsp_size,
					   (u32)data->hdr.pkt_size));
			v->svc_result = 0;
			v->svc_expected_opcode = 0;
			wake_up(&v->svc_wait);
			return 0;
		}

		if (data->hdr.opcode != APR_BASIC_RSP_RESULT)
			return 0;

		dev_dbg(dev, "svc result: opcode %#x, status: %#x\n",
			result->opcode, result->status);

		if (result->opcode != v->svc_expected_opcode) {
			dev_warn(dev, "unexpected svc reply for opcode %#x\n",
				 result->opcode);
			return 0;
		}

		v->svc_result = result->status;
		v->svc_expected_opcode = 0;
		wake_up(&v->svc_wait);
		return 0;
	}

	if (data->hdr.dest_port >= Q6VOICE_PATH_COUNT) {
		dev_warn(dev, "callback() called for unhandled/invalid path: %d\n",
			 data->hdr.dest_port);
		return 0;
	}

	spin_lock_irqsave(&v->lock, flags);
	s = v->sessions[data->hdr.dest_port];
	if (s)
		kref_get(&s->refcount);
	spin_unlock_irqrestore(&v->lock, flags);

	if (s) {
		q6voice_session_callback(s, data);
		kref_put(&s->refcount, q6voice_session_free);
	} else {
		dev_warn(dev, "callback() called for inactive path: %d\n",
			 data->hdr.dest_port);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_callback);

int q6voice_common_send(struct q6voice_session *s, struct apr_hdr *hdr)
{
	unsigned long flags;
	int ret;

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				       APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	hdr->src_port = s->port;
	hdr->dest_port = s->handle;
	hdr->token = 0;

	spin_lock_irqsave(&s->lock, flags);
	s->expected_opcode = hdr->opcode;
	s->result = 0;
	spin_unlock_irqrestore(&s->lock, flags);

	ret = apr_send_pkt(s->svc->adev, (struct apr_pkt *)hdr);
	if (ret < 0)
		return ret;

	ret = wait_event_timeout(s->wait, (s->expected_opcode == 0),
				 msecs_to_jiffies(TIMEOUT_MS));
	if (!ret) {
		s->expected_opcode = 0;
		return -ETIMEDOUT;
	}

	if (s->result > 0) {
		dev_err(s->dev, "command %#x failed with error %d\n",
			hdr->opcode, s->result);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_send);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice common session management");
MODULE_LICENSE("GPL v2");
