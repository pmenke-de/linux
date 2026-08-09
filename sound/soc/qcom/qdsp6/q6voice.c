// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "q6afe.h"
#include "q6core.h"
#include "q6cvp.h"
#include "q6cvs.h"
#include "q6mvm.h"
#include "q6voice-common.h"

struct q6voice_path_runtime {
	struct q6voice_session *sessions[Q6VOICE_SERVICE_COUNT];
	unsigned int started;
};

struct q6voice_path {
	struct q6voice *v;

	enum q6voice_path_type type;
	int tx_port, rx_port;
	/* Serialize access to voice path session */
	struct mutex lock;
	struct q6voice_path_runtime *runtime;
};

struct q6voice {
	struct device *dev;
	/* cached so the version is queried once, not per call */
	char cvd_version[32];
	struct q6voice_path paths[Q6VOICE_PATH_COUNT];
};

/*
 * Which of the two create commands the vocproc expects. They carry the same
 * payload and differ only in opcode, but the choice is not cosmetic: a CVD
 * from 2.2 on refuses VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT for a session that was
 * created with the older V2 command.
 */
static bool q6voice_cvp_create_v3(const char *cvd_version)
{
	unsigned int major, minor;

	if (sscanf(cvd_version, "%u.%u", &major, &minor) != 2)
		return false;

	return major > 2 || (major == 2 && minor >= 2);
}

static int q6voice_path_start(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm, *cvp, *cvs;
	int ret;

	dev_dbg(dev, "start path %d\n", p->type);

	mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	if (!mvm) {
		mvm = q6mvm_session_create(p->type);
		if (IS_ERR(mvm))
			return PTR_ERR(mvm);
		p->runtime->sessions[Q6VOICE_SERVICE_MVM] = mvm;
	}

	/*
	 * The stream session carries the voice packets themselves. It has to
	 * exist and be attached before the vocproc is started, otherwise the
	 * ADSP ends up with a fully configured but silent call.
	 */
	cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
	if (!cvs) {
		cvs = q6cvs_session_create(p->type);
		if (IS_ERR(cvs))
			return PTR_ERR(cvs);
		p->runtime->sessions[Q6VOICE_SERVICE_CVS] = cvs;
	}

	ret = q6mvm_attach_stream(mvm, cvs, true);
	if (ret) {
		dev_err(dev, "failed to attach stream to mvm: %d\n", ret);
		return ret;
	}

	/*
	 * Which CVD the ADSP runs decides how the vocproc below is created, so
	 * ask before creating it. The vendor gates the topology commit on
	 * >= 2.2 and the per-path media format commands on >= 2.3 as well.
	 */
	if (!p->v->cvd_version[0] &&
	    !q6mvm_get_cvd_version(p->v->cvd_version, sizeof(p->v->cvd_version)))
		dev_info(dev, "ADSP CVD version: '%s'\n", p->v->cvd_version);

	cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	if (!cvp) {
		cvp = q6cvp_session_create(p->type,
					   q6afe_get_port_id(p->tx_port),
					   q6afe_get_port_id(p->rx_port),
					   q6voice_cvp_create_v3(p->v->cvd_version));
		if (IS_ERR(cvp)) {
			ret = PTR_ERR(cvp);
			goto stream_err;
		}
		p->runtime->sessions[Q6VOICE_SERVICE_CVP] = cvp;
	}

	/*
	 * Describe both directions before committing: the ADSP refuses the
	 * commit until it knows the channel layout of each side.
	 */
	ret = q6cvp_set_channel_info(cvp);
	if (ret)
		dev_warn(dev, "set channel info failed: %d\n", ret);

	/*
	 * CVD 2.3 and later also want the media format of each endpoint before
	 * the topology can be committed.
	 */
	ret = q6cvp_set_media_format(cvp, q6afe_get_port_id(p->tx_port),
				     q6afe_get_port_id(p->rx_port));
	if (ret)
		dev_warn(dev, "set media format failed: %d\n", ret);

	/*
	 * The commit below publishes topology modules that must already be
	 * loaded. Ask the core to load the ones this vocproc was created with;
	 * on the vendor these ids come from calibration, so the stock defaults
	 * may or may not be present in the ADSP.
	 */
	ret = q6core_load_topo_modules(VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT);
	if (ret)
		dev_warn(dev, "load rx topo modules failed: %d\n", ret);
	ret = q6core_load_topo_modules(VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS);
	if (ret)
		dev_warn(dev, "load tx topo modules failed: %d\n", ret);

	/*
	 * Commit the topology before enabling. Not fatal if the ADSP rejects
	 * it -- older CVD versions do not implement the command at all -- but
	 * newer ones need it or the vocproc never actually runs.
	 */
	ret = q6cvp_topology_commit(cvp);
	if (ret)
		dev_warn(dev, "topology commit failed: %d\n", ret);

	ret = q6cvp_enable(cvp, true);
	if (ret) {
		dev_err(dev, "failed to enable cvp: %d\n", ret);
		goto cvp_err;
	}

	ret = q6mvm_attach(mvm, cvp, true);
	if (ret) {
		dev_err(dev, "failed to attach cvp to mvm: %d\n", ret);
		goto attach_err;
	}

	ret = q6mvm_start(mvm, true);
	if (ret) {
		dev_err(dev, "failed to start voice: %d\n", ret);
		goto start_err;
	}

	return ret;

start_err:
	q6mvm_start(mvm, false);
attach_err:
	q6mvm_attach(mvm, cvp, false);
cvp_err:
	q6cvp_enable(cvp, false);
stream_err:
	q6mvm_attach_stream(mvm, cvs, false);
	return ret;
}

int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime) {
		p->runtime = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p->runtime) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (p->runtime->started & BIT(capture)) {
		ret = -EALREADY;
		goto out;
	}

	p->runtime->started |= BIT(capture);

	/* FIXME: For now we only start if both RX/TX are active */
	if (p->runtime->started != 3)
		goto out;

	ret = q6voice_path_start(p);
	if (ret) {
		p->runtime->started &= ~BIT(capture);
		dev_err(v->dev, "failed to start path %d: %d\n", path, ret);
		goto out;
	}

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_start);

static void q6voice_path_stop(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	struct q6voice_session *cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
	int ret;

	dev_dbg(dev, "stop path %d\n", p->type);

	ret = q6mvm_start(mvm, false);
	if (ret)
		dev_err(dev, "failed to stop voice: %d\n", ret);

	ret = q6mvm_attach(mvm, cvp, false);
	if (ret)
		dev_err(dev, "failed to detach cvp from mvm: %d\n", ret);

	ret = q6cvp_enable(cvp, false);
	if (ret)
		dev_err(dev, "failed to disable cvp: %d\n", ret);

	if (cvs) {
		ret = q6mvm_attach_stream(mvm, cvs, false);
		if (ret)
			dev_err(dev, "failed to detach stream from mvm: %d\n", ret);
	}
}

static void q6voice_path_destroy(struct q6voice_path *p)
{
	struct q6voice_path_runtime *runtime = p->runtime;
	enum q6voice_service_type svc;

	for (svc = 0; svc < Q6VOICE_SERVICE_COUNT; ++svc) {
		if (runtime->sessions[svc])
			q6voice_session_release(runtime->sessions[svc]);
	}

	p->runtime = NULL;
	kfree(runtime);
}

int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime || !(p->runtime->started & BIT(capture)))
		goto out;

	if (p->runtime->started == 3)
		q6voice_path_stop(p);

	p->runtime->started &= ~BIT(capture);

	if (p->runtime->started == 0)
		q6voice_path_destroy(p);

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_stop);

static void q6voice_free(void *data)
{
	struct q6voice *v = data;
	enum q6voice_path_type path;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		mutex_lock(&p->lock);
		if (p->runtime) {
			dev_warn(v->dev,
				 "q6voice_remove() called while path %d is active\n",
				 path);

			if (p->runtime->started == 3)
				q6voice_path_stop(p);
			q6voice_path_destroy(p);
		}
		mutex_unlock(&p->lock);
		mutex_destroy(&p->lock);
	}
}

struct q6voice *q6voice_create(struct device *dev)
{
	struct q6voice *v;
	enum q6voice_path_type path;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);

	v->dev = dev;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		p->v = v;
		p->type = path;
		mutex_init(&p->lock);
	}

	ret = devm_add_action(dev, q6voice_free, v);
	if (ret)
		return ERR_PTR(ret);

	return v;
}
EXPORT_SYMBOL_GPL(q6voice_create);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		return p->tx_port;
	else
		return p->rx_port;
}
EXPORT_SYMBOL_GPL(q6voice_get_port);

void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		p->tx_port = index;
	else
		p->rx_port = index;
}
EXPORT_SYMBOL_GPL(q6voice_set_port);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice driver");
MODULE_LICENSE("GPL v2");
