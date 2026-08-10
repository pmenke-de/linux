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
#include "q6voice-cal.h"
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
	/* lent to the ADSP once and referred to by handle thereafter */
	struct q6voice_cal *dev_cfg;	/* what the endpoints are */
	struct q6voice_cal *cal;	/* the vocproc, other than by step */
	struct q6voice_cal *vol_cal;	/* the vocproc, per volume step */
	bool cal_tried;
	struct q6voice_path paths[Q6VOICE_PATH_COUNT];
};

/*
 * Calibration is per board, and the ADSP has to be lent it before it will
 * resolve a volume step. Loaded on the first call rather than at probe: the
 * MVM service has to be up to accept the memory, and it is not when q6voice
 * is created.
 */
/*
 * The three tables, in the order the vendor registers them: what the endpoints
 * are, then everything about the vocproc that does not vary with the volume
 * step, then what does. A volume table is an overlay on a vocproc that has
 * already been told the first two, which is why the order is not incidental.
 */
#define Q6VOICE_DEV_CFG_FIRMWARE	"qcom/q6voice-devcfg.bin"
#define Q6VOICE_CAL_FIRMWARE		"qcom/q6voice-cal.bin"
#define Q6VOICE_VOL_CAL_FIRMWARE	"qcom/q6voice-vol-cal.bin"

/*
 * Downlink volume for a call. Android's HAL maps the user-facing volume onto
 * steps 0..5 and pushes the result down a mixer control; with no such control
 * here, ask for the top of that range so calls are audible, and leave making
 * it adjustable to whoever adds the control.
 */
#define Q6VOICE_RX_VOLUME_STEP	5

/* Vendor defaults: a short ramp for volume, a long one for mute. */
#define Q6VOICE_VOLUME_RAMP_MS	20
#define Q6VOICE_MUTE_RAMP_MS	500

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

/*
 * Set once, by a driver that probes long before any call can start, and only
 * ever read from a path that is already serialized by its own mutex.
 */
static const struct q6voice_modem_link *q6voice_modem_link;

void q6voice_set_modem_link(const struct q6voice_modem_link *link)
{
	q6voice_modem_link = link;
}
EXPORT_SYMBOL_GPL(q6voice_set_modem_link);

static int q6voice_modem_link_start(void)
{
	if (!q6voice_modem_link)
		return 0;

	return q6voice_modem_link->start();
}

static void q6voice_modem_link_end(void)
{
	if (q6voice_modem_link)
		q6voice_modem_link->end();
}

/*
 * How much of the vocproc to configure before starting a call.
 *
 * The msm8916 driver this one grew from creates a vocproc, enables it, attaches
 * it and starts -- five commands, and calls are audible. Everything beyond that
 * here was added for a modem on another chip, at a time when the session name
 * and the create opcode were both still wrong, and has never been re-tested
 * since those were fixed. A configuration command the ADSP accepts but does not
 * want is indistinguishable from one it needs, so make it possible to take them
 * back out and hear the difference.
 *
 *   0  everything (default)
 *   1  no vocproc configuration: no channel info, media format, topology or
 *      volume, but still a stream session
 *   2  as msm8916 does it: no stream session either -- this ADSP refuses to
 *      enable a vocproc without one (VSS_IVOCPROC_CMD_ENABLE returns an
 *      error), so it is a comparison point rather than a usable setting
 *
 * Writable at runtime, so the three can be compared within one boot rather than
 * one reboot each.
 */
/*
 * How far to take the calibration handoff. Lending the memory and registering
 * it are separate conversations with the DSP and either can be the one that
 * kills it, so they can be reached one at a time: whichever level first takes
 * the board down is the one at fault. Off by default because a board that
 * resets on every call is worse than one that is merely quiet.
 */
enum {
	Q6VOICE_CAL_NONE = 0,	/* do not touch calibration at all */
	Q6VOICE_CAL_LEND,	/* load it and lend the DSP the memory */
	Q6VOICE_CAL_REGISTER,	/* also point the vocproc at it */
};

/*
 * Re-read the calibration files at the start of every call rather than once.
 * Only useful while working out what the DSP will accept: it makes trying a
 * different file a matter of copying it, and the memory the last attempt was
 * lent went away with the session that lent it.
 */
static bool cal_reload;
module_param(cal_reload, bool, 0644);
MODULE_PARM_DESC(cal_reload, "Re-read the calibration files on every call.");

static int cal_level = Q6VOICE_CAL_NONE;
module_param(cal_level, int, 0644);
MODULE_PARM_DESC(cal_level,
		 "0 to leave calibration alone, 1 to lend the DSP the memory, 2 to also register it.");

static int setup_level;
module_param(setup_level, int, 0644);
MODULE_PARM_DESC(setup_level,
		 "0 = full setup, 1 = skip vocproc config, 2 = msm8916 minimal");

static int q6voice_path_start(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm, *cvp, *cvs = NULL;
	int ret;

	dev_info(dev, "start path %d, setup level %d\n", p->type, setup_level);

	/*
	 * Where the modem is a separate chip it DMAs the call audio into system
	 * memory for as long as the call lasts, so the link to it must be held
	 * awake throughout -- an idle link suspends after a couple of seconds
	 * and the audio simply stops arriving. Vote first and release on the way
	 * out, as the vendor does.
	 */
	ret = q6voice_modem_link_start();
	if (ret) {
		dev_err(dev, "failed to wake the modem link: %d\n", ret);
		return ret;
	}

	mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	if (!mvm) {
		mvm = q6mvm_session_create(p->type);
		if (IS_ERR(mvm)) {
			ret = PTR_ERR(mvm);
			goto link_err;
		}
		p->runtime->sessions[Q6VOICE_SERVICE_MVM] = mvm;
	}

	/*
	 * The stream session carries the voice packets themselves. msm8916 does
	 * without one -- its modem is on the same die -- so it is the first
	 * thing to drop when reproducing that driver exactly.
	 */
	if (setup_level < 2) {
		cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
		if (!cvs) {
			cvs = q6cvs_session_create(p->type);
			if (IS_ERR(cvs)) {
				ret = PTR_ERR(cvs);
				goto link_err;
			}
			p->runtime->sessions[Q6VOICE_SERVICE_CVS] = cvs;
		}

		ret = q6mvm_attach_stream(mvm, cvs, true);
		if (ret) {
			dev_err(dev, "failed to attach stream to mvm: %d\n", ret);
			goto link_err;
		}
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

	if (setup_level > 0)
		goto configured;

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

	/*
	 * A render port with more than one channel needs the converter set up
	 * behind it, and only after the topology is committed.
	 */
	ret = q6cvp_set_mfc_config(cvp);
	if (ret)
		dev_warn(dev, "media format converter setup failed: %d\n", ret);

	/*
	 * Calibration goes in while the vocproc is still being described.
	 * Once it has been enabled and attached it is running, and a running
	 * vocproc will not take a new table: the command is well formed and
	 * simply fails. Absent calibration is not an error -- the call
	 * proceeds, and only the volume command notices.
	 */
	if (cal_level >= Q6VOICE_CAL_LEND && (!p->v->cal_tried || cal_reload)) {
		q6voice_cal_free(p->v->dev_cfg);
		q6voice_cal_free(p->v->cal);
		q6voice_cal_free(p->v->vol_cal);

		p->v->cal_tried = true;
		p->v->dev_cfg = q6voice_cal_load(dev, Q6VOICE_DEV_CFG_FIRMWARE,
						 mvm);
		p->v->cal = q6voice_cal_load(dev, Q6VOICE_CAL_FIRMWARE, mvm);
		p->v->vol_cal = q6voice_cal_load(dev, Q6VOICE_VOL_CAL_FIRMWARE,
						 mvm);
	}

	if (cal_level >= Q6VOICE_CAL_REGISTER) {
		bool instance = q6voice_cvp_create_v3(p->v->cvd_version);

		if (!p->v->dev_cfg)
			dev_warn(dev, "no device configuration to register\n");

		ret = q6voice_cal_register_dev_cfg(p->v->dev_cfg, cvp);
		if (ret)
			dev_warn(dev, "failed to register device config: %d\n",
				 ret);

		ret = q6voice_cal_register_cal(p->v->cal, cvp, instance);
		if (ret)
			dev_warn(dev, "failed to register calibration: %d\n",
				 ret);

		ret = q6voice_cal_register_vol(p->v->vol_cal, cvp, instance);
		if (ret)
			dev_warn(dev, "failed to register volume calibration: %d\n",
				 ret);
	}

configured:
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

	if (setup_level > 0)
		goto started;


	/*
	 * Volume and mute, before the call runs and in that order, as the
	 * vendor does. Neither is fatal: the call is still established without
	 * them, just inaudible, and saying so beats refusing to start.
	 */
	ret = q6cvp_set_rx_volume(cvp, Q6VOICE_RX_VOLUME_STEP,
				  Q6VOICE_VOLUME_RAMP_MS);
	if (ret)
		dev_warn(dev, "failed to set rx volume: %d\n", ret);

	ret = q6cvs_set_mute(cvs, VSS_IVOLUME_DIRECTION_TX, false,
			     Q6VOICE_MUTE_RAMP_MS);
	if (ret)
		dev_warn(dev, "failed to unmute tx stream: %d\n", ret);

	/*
	 * The device mute is a second gate, on the vocproc rather than the
	 * stream, and it applies to both directions. Unlike the stream mute the
	 * vendor clears it for uplink and downlink both.
	 */
	ret = q6cvp_set_device_mute(cvp, VSS_IVOLUME_DIRECTION_RX, false,
				    Q6VOICE_MUTE_RAMP_MS);
	if (ret)
		dev_warn(dev, "failed to unmute rx device: %d\n", ret);

	ret = q6cvp_set_device_mute(cvp, VSS_IVOLUME_DIRECTION_TX, false,
				    Q6VOICE_MUTE_RAMP_MS);
	if (ret)
		dev_warn(dev, "failed to unmute tx device: %d\n", ret);

started:
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
	/* There is no stream to detach when the setup level left it out. */
	if (cvs)
		q6mvm_attach_stream(mvm, cvs, false);
link_err:
	q6voice_modem_link_end();
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

	q6voice_modem_link_end();
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

	q6voice_cal_free(v->dev_cfg);
	q6voice_cal_free(v->cal);
	q6voice_cal_free(v->vol_cal);
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
