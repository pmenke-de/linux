// SPDX-License-Identifier: GPL-2.0
/*
 * MHI satellite driver: lets a remote subsystem drive its own MHI channels.
 *
 * Ported from the Qualcomm downstream mhi_satellite driver
 * (Copyright (c) 2019, The Linux Foundation).
 *
 * On boards where the modem hangs off PCIe, the audio DSP needs to talk to it
 * directly -- call audio is negotiated between the two with the AP nowhere in
 * the data path. The DSP has no PCIe access of its own, so instead the AP hosts
 * the MHI controller and *lends* a few channels to the DSP: the DSP builds the
 * channel and event ring contexts in its own memory, sends them here over a
 * glink/rpmsg link, and this driver installs them into the MHI core and starts
 * the channels. From then on the DSP rings doorbells and consumes events
 * itself.
 *
 * So this is a proxy for MHI control operations, not a data path. The messages
 * are MHI transfer-ring elements wrapped in a small header; we execute the ones
 * the remote cannot (IOMMU mapping, context install, channel start/stop) and
 * reply with completion events.
 *
 * The remote also has to be told when the device dies, hence the sys_err
 * plumbing: without it the DSP would keep using rings that no longer exist.
 */

#include <linux/async.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/rpmsg.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "internal.h"

#define MHI_SAT_DRIVER_NAME "mhi_satellite"

/* sys_err command */
#define MHI_TRE_CMD_SYS_ERR_PTR		0
#define MHI_TRE_CMD_SYS_ERR_D0		0
#define MHI_TRE_CMD_SYS_ERR_D1		(MHI_PKT_TYPE_SYS_ERR_CMD << 16)

/* state change event */
#define MHI_TRE_EVT_MHI_STATE_PTR	0
#define MHI_TRE_EVT_MHI_STATE_D0(state)	((state) << 24)
#define MHI_TRE_EVT_MHI_STATE_D1	(MHI_PKT_TYPE_STATE_CHANGE_EVENT << 16)

/* exec env change event */
#define MHI_TRE_EVT_EE_PTR		0
#define MHI_TRE_EVT_EE_D0(ee)		((ee) << 24)
#define MHI_TRE_EVT_EE_D1		(MHI_PKT_TYPE_EE_EVENT << 16)

/* config (hello) event */
#define MHI_TRE_EVT_CFG_PTR(base)	(base)
#define MHI_TRE_EVT_CFG_D0(er_base, n)	(((er_base) << 16) | ((n) & 0xFFFF))
#define MHI_TRE_EVT_CFG_D1		(MHI_PKT_TYPE_CFG_EVENT << 16)

/* command completion event */
#define MHI_TRE_EVT_CMD_COMPLETION_PTR(ptr)	(ptr)
#define MHI_TRE_EVT_CMD_COMPLETION_D0(code)	((code) << 24)
#define MHI_TRE_EVT_CMD_COMPLETION_D1	(MHI_PKT_TYPE_CMD_COMPLETION_EVENT << 16)

/* packet parsers */
#define MHI_TRE_GET_PTR(tre)		((tre)->ptr)
#define MHI_TRE_GET_SIZE(tre)		((tre)->dword[0])
#define MHI_TRE_GET_CCS(tre)		(((tre)->dword[0] >> 24) & 0xFF)
#define MHI_TRE_GET_ID(tre)		(((tre)->dword[1] >> 24) & 0xFF)
#define MHI_TRE_GET_TYPE(tre)		(((tre)->dword[1] >> 16) & 0xFF)
#define MHI_TRE_IS_ER_CTXT_TYPE(tre)	((tre)->dword[1] & 0x1)

#define MHI_CTXT_TYPE_GENERIC		0xA

struct __packed mhi_generic_ctxt {
	u32 reserved0;
	u32 type;
	u32 reserved1;
	u64 ctxt_base;
	u64 ctxt_size;
	u64 reserved[2];
};

/*
 * Packet types that only exist on the satellite link. The rest of the
 * MHI_PKT_TYPE_* and all of enum mhi_ev_ccs come from common.h, which
 * internal.h already pulls in.
 */
#define MHI_PKT_TYPE_CTXT_UPDATE_CMD	0x64
#define MHI_PKT_TYPE_IOMMU_MAP_CMD	0x65
#define MHI_PKT_TYPE_CFG_EVENT		0x6E
#define MHI_PKT_TYPE_SYS_ERR_CMD	0xFF

enum subsys_id {
	SUBSYS_ADSP,
	SUBSYS_SLPI,
	SUBSYS_MAX,
};

static const char * const subsys_names[SUBSYS_MAX] = {
	[SUBSYS_ADSP] = "lpass",
	[SUBSYS_SLPI] = "slpi",
};

/* IPC definitions */
#define SAT_MAJOR_VERSION	1
#define SAT_MINOR_VERSION	0
#define SAT_RESERVED_SEQ_NUM	0xFFFF
#define SAT_MSG_SIZE(n)		(sizeof(struct sat_header) + \
				 ((n) * sizeof(struct sat_tre)))
#define SAT_TRE_SIZE(msg_size)	((msg_size) - sizeof(struct sat_header))
#define SAT_TRE_OFFSET(msg)	((msg) + sizeof(struct sat_header))
#define SAT_TRE_NUM_PKTS(size)	((size) / sizeof(struct sat_tre))

enum sat_msg_id {
	SAT_MSG_ID_ACK = 0xA,
	SAT_MSG_ID_CMD = 0xC,
	SAT_MSG_ID_EVT = 0xE,
};

enum sat_ctxt_type {
	SAT_CTXT_TYPE_CHAN	= 0x0,
	SAT_CTXT_TYPE_EVENT	= 0x1,
	SAT_CTXT_TYPE_MAX,
};

static const char * const sat_ctxt_str[SAT_CTXT_TYPE_MAX] = {
	[SAT_CTXT_TYPE_CHAN] = "CCA",
	[SAT_CTXT_TYPE_EVENT] = "ECA",
};

#define TO_SAT_CTXT_TYPE_STR(t) \
	((t) >= SAT_CTXT_TYPE_MAX ? "INVALID" : sat_ctxt_str[t])

struct __packed sat_tre {
	u64 ptr;
	u32 dword[2];
};

struct __packed sat_header {
	u16 major_ver;
	u16 minor_ver;
	u16 msg_id;
	u16 seq;
	u16 reply_seq;
	u16 payload_size;
	u32 dev_id;
	u8 reserved[8];
};

/*
 * Addresses the remote asked us to map on its behalf. Kept so they can be
 * released when the link or the device goes away; mhi_buf has no list node.
 */
struct mhi_sat_addr_map {
	struct list_head node;
	phys_addr_t phys_addr;
	dma_addr_t dma_addr;
	size_t len;
};

struct mhi_sat_packet {
	struct list_head node;
	struct mhi_sat_cntrl *cntrl;
	void *msg;
};

enum mhi_sat_state {
	SAT_READY,		/* device presented, no link yet */
	SAT_RUNNING,		/* remote can talk to the device */
	SAT_DISCONNECTED,	/* rpmsg link down */
	SAT_FATAL_DETECT,	/* device asserted */
	SAT_ERROR,		/* device down after error or shutdown */
	SAT_DISABLED,		/* wait for device removal */
	SAT_STATE_MAX,
};

static const char * const sat_state_str[SAT_STATE_MAX] = {
	[SAT_READY]		= "ready",
	[SAT_RUNNING]		= "running",
	[SAT_DISCONNECTED]	= "disconnected",
	[SAT_FATAL_DETECT]	= "fatal detected",
	[SAT_ERROR]		= "error",
	[SAT_DISABLED]		= "disabled",
};

#define MHI_SAT_ACTIVE(c)	((c)->state == SAT_RUNNING)
#define MHI_SAT_IN_ERROR(c)	((c)->state >= SAT_FATAL_DETECT)
#define MHI_SAT_ALLOW_CONN(c)	((c)->state == SAT_READY || \
				 (c)->state == SAT_DISCONNECTED)
#define MHI_SAT_ALLOW_SYS_ERR(c) ((c)->state == SAT_RUNNING || \
				  (c)->state == SAT_FATAL_DETECT)

/*
 * Everything the remote asked for this boot, and what it was told in reply.
 *
 * The exchange happens once, early, and is over before anyone can arm dynamic
 * debug -- and a request the host refuses is answered with an error code the
 * remote accepts without complaint, so a rejected channel and a granted one
 * look the same from the outside. Recording it is the only way to tell.
 */
#define MHI_SAT_LOG_ENTRIES	64

struct mhi_sat_log_entry {
	u64 ts;
	u64 ptr;
	u32 size;
	u8 type;
	u8 ccs;
	u8 id;
	bool er;	/* the id names an event ring, not a channel */
};

struct mhi_sat_subsys {
	const char *name;
	struct rpmsg_device *rpdev;

	struct list_head cntrl_list;
	struct mutex cntrl_mutex;
	spinlock_t cntrl_lock;
};

struct mhi_sat_cntrl {
	struct list_head node;

	struct mhi_controller *mhi_cntrl;
	struct mhi_sat_subsys *subsys;

	struct list_head dev_list;
	struct list_head addr_map_list;
	struct mutex list_mutex;

	struct list_head packet_list;
	spinlock_t pkt_lock;

	struct work_struct connect_work;
	struct work_struct process_work;
	async_cookie_t error_cookie;

	u32 dev_id;
	phys_addr_t base_addr;	/* physical MHI register base, for the remote */
	int er_base;
	int er_max;
	int num_er;

	int num_devices;
	int max_devices;
	u16 seq;
	enum mhi_sat_state state;
	spinlock_t state_lock;

	u16 last_cmd_seq;
	enum mhi_ev_ccs last_cmd_ccs;
	struct completion completion;
	struct mutex cmd_wait_mutex;

	/* diagnostics; see mhi_sat_status_show() and mhi_sat_log_show() */
	struct dentry *dbgfs;
	unsigned int hellos;
	unsigned int rx_cmds;
	unsigned int rx_evts;

	struct mhi_sat_log_entry log[MHI_SAT_LOG_ENTRIES];
	unsigned int log_next;
	spinlock_t log_lock;
};

struct mhi_sat_device {
	struct list_head node;
	struct mhi_device *mhi_dev;
	struct mhi_sat_cntrl *cntrl;
	bool chan_started;
};

static struct mhi_sat_subsys *mhi_sat_subsys_array;

/*
 * How many satellite channels the remote expects to share. The downstream
 * driver reads this from each MHI device's DT node; mainline MHI devices have
 * none, so it is fixed here. It must match the number of channels declared for
 * the subsystem in the controller config, otherwise the link never comes up.
 */
#define MHI_SAT_MAX_DEVICES 4

static struct mhi_sat_subsys *find_subsys_by_name(const char *name)
{
	struct mhi_sat_subsys *subsys = mhi_sat_subsys_array;
	int i;

	for (i = 0; i < SUBSYS_MAX; i++, subsys++) {
		if (!strcmp(name, subsys->name))
			return subsys;
	}

	return NULL;
}

static struct mhi_sat_cntrl *find_sat_cntrl_by_id(struct mhi_sat_subsys *subsys,
						  u32 dev_id)
{
	struct mhi_sat_cntrl *sat_cntrl;
	unsigned long flags;

	spin_lock_irqsave(&subsys->cntrl_lock, flags);
	list_for_each_entry(sat_cntrl, &subsys->cntrl_list, node) {
		if (sat_cntrl->dev_id == dev_id) {
			spin_unlock_irqrestore(&subsys->cntrl_lock, flags);
			return sat_cntrl;
		}
	}
	spin_unlock_irqrestore(&subsys->cntrl_lock, flags);

	return NULL;
}

/* event ring index of a device's channel; not exposed outside the MHI core */
static int mhi_sat_dev_er_index(struct mhi_device *mhi_dev)
{
	struct mhi_chan *mhi_chan = mhi_dev->dl_chan ?: mhi_dev->ul_chan;

	return mhi_chan ? mhi_chan->er_index : 0;
}

static struct mhi_sat_device *find_sat_dev_by_id(struct mhi_sat_cntrl *sat_cntrl,
						 int id, enum sat_ctxt_type evt)
{
	struct mhi_sat_device *sat_dev;
	int compare_id;

	mutex_lock(&sat_cntrl->list_mutex);
	list_for_each_entry(sat_dev, &sat_cntrl->dev_list, node) {
		compare_id = (evt == SAT_CTXT_TYPE_EVENT) ?
				mhi_sat_dev_er_index(sat_dev->mhi_dev) :
				sat_dev->mhi_dev->dl_chan_id;

		if (compare_id == id) {
			mutex_unlock(&sat_cntrl->list_mutex);
			return sat_dev;
		}
	}
	mutex_unlock(&sat_cntrl->list_mutex);

	return NULL;
}

static bool mhi_sat_isvalid_header(struct sat_header *hdr, int len)
{
	if (len < (int)sizeof(*hdr))
		return false;

	if (len != (int)(hdr->payload_size + sizeof(*hdr)))
		return false;

	if (hdr->major_ver != SAT_MAJOR_VERSION ||
	    hdr->minor_ver != SAT_MINOR_VERSION)
		return false;

	if (hdr->msg_id != SAT_MSG_ID_CMD && hdr->msg_id != SAT_MSG_ID_EVT)
		return false;

	return true;
}

static int mhi_sat_wait_cmd_completion(struct mhi_sat_cntrl *sat_cntrl)
{
	struct device *dev = sat_cntrl->mhi_cntrl->cntrl_dev;
	int ret;

	ret = wait_for_completion_timeout(&sat_cntrl->completion,
			msecs_to_jiffies(sat_cntrl->mhi_cntrl->timeout_ms));
	if (!ret || sat_cntrl->last_cmd_ccs != MHI_EV_CC_SUCCESS) {
		dev_err(dev, "sat: command failed seq:%u ret:%d ccs:%d\n",
			sat_cntrl->last_cmd_seq, ret, sat_cntrl->last_cmd_ccs);
		return -EIO;
	}

	return 0;
}

static int mhi_sat_send_msg(struct mhi_sat_cntrl *sat_cntrl,
			    enum sat_msg_id type, u16 reply_seq,
			    void *msg, u16 msg_size)
{
	struct mhi_sat_subsys *subsys = sat_cntrl->subsys;
	struct sat_header *hdr = msg;

	if (!subsys->rpdev)
		return -ENODEV;

	sat_cntrl->seq++;
	if (sat_cntrl->seq == SAT_RESERVED_SEQ_NUM)
		sat_cntrl->seq = 0;

	hdr->major_ver = SAT_MAJOR_VERSION;
	hdr->minor_ver = SAT_MINOR_VERSION;
	hdr->msg_id = type;
	hdr->seq = sat_cntrl->seq;
	hdr->reply_seq = reply_seq;
	hdr->payload_size = SAT_TRE_SIZE(msg_size);
	hdr->dev_id = sat_cntrl->dev_id;

	if (type == SAT_MSG_ID_CMD)
		sat_cntrl->last_cmd_seq = sat_cntrl->seq;

	return rpmsg_send(subsys->rpdev->ept, msg, msg_size);
}

static void mhi_sat_log(struct mhi_sat_cntrl *sat_cntrl, u8 type, u8 id,
			bool er, u64 ptr, u32 size, u8 ccs)
{
	struct mhi_sat_log_entry *e;
	unsigned long flags;

	spin_lock_irqsave(&sat_cntrl->log_lock, flags);

	e = &sat_cntrl->log[sat_cntrl->log_next % MHI_SAT_LOG_ENTRIES];
	sat_cntrl->log_next++;

	e->ts = local_clock();
	e->ptr = ptr;
	e->size = size;
	e->type = type;
	e->ccs = ccs;
	e->id = id;
	e->er = er;

	spin_unlock_irqrestore(&sat_cntrl->log_lock, flags);
}

/*
 * Execute the commands the remote cannot issue itself and rewrite each packet
 * in place into its completion event -- the caller ships the same buffer back.
 */
static void mhi_sat_process_cmds(struct mhi_sat_cntrl *sat_cntrl,
				 struct sat_header *hdr, struct sat_tre *pkt)
{
	struct mhi_controller *mhi_cntrl = sat_cntrl->mhi_cntrl;
	struct device *dev = mhi_cntrl->cntrl_dev;
	int num_pkts = SAT_TRE_NUM_PKTS(hdr->payload_size);
	int i;

	for (i = 0; i < num_pkts; i++, pkt++) {
		enum mhi_ev_ccs code = MHI_EV_CC_INVALID;
		u64 completion_ptr = 0;

		/* the packet is rewritten below, so keep what it asked for */
		u8 log_type = MHI_TRE_GET_TYPE(pkt);
		u8 log_id = MHI_TRE_GET_ID(pkt);
		bool log_er = MHI_TRE_IS_ER_CTXT_TYPE(pkt);
		u64 log_ptr = MHI_TRE_GET_PTR(pkt);
		u32 log_size = MHI_TRE_GET_SIZE(pkt);

		switch (MHI_TRE_GET_TYPE(pkt)) {
		case MHI_PKT_TYPE_IOMMU_MAP_CMD: {
			struct mhi_sat_addr_map *map;
			dma_addr_t iova;

			map = kzalloc(sizeof(*map), GFP_KERNEL);
			if (!map)
				break;

			map->phys_addr = MHI_TRE_GET_PTR(pkt);
			map->len = MHI_TRE_GET_SIZE(pkt);

			iova = dma_map_resource(dev, map->phys_addr, map->len,
						DMA_BIDIRECTIONAL, 0);
			if (dma_mapping_error(dev, iova)) {
				kfree(map);
				break;
			}

			map->dma_addr = iova;
			completion_ptr = iova;

			mutex_lock(&sat_cntrl->list_mutex);
			list_add_tail(&map->node, &sat_cntrl->addr_map_list);
			mutex_unlock(&sat_cntrl->list_mutex);

			code = MHI_EV_CC_SUCCESS;
			dev_dbg(dev, "sat: IOMMU MAP %pap len:%zu -> %pad\n",
				&map->phys_addr, map->len, &iova);
			break;
		}
		case MHI_PKT_TYPE_CTXT_UPDATE_CMD: {
			enum sat_ctxt_type evt = MHI_TRE_IS_ER_CTXT_TYPE(pkt);
			int id = MHI_TRE_GET_ID(pkt);
			struct mhi_generic_ctxt gen_ctxt = {0};
			struct mhi_sat_device *sat_dev;
			struct mhi_buf buf = {0};

			sat_dev = find_sat_dev_by_id(sat_cntrl, id, evt);
			if (!sat_dev) {
				dev_err(dev, "sat: no device for chan/evt %d\n", id);
				break;
			}

			/*
			 * A context update for a channel we still believe is
			 * running means the remote has started over without
			 * telling us: it re-maps its buffers and pushes fresh
			 * ring addresses, but never sends RESET_CHAN first, so
			 * nothing here clears chan_started. Left alone, the
			 * START that follows is refused, and the channel we
			 * kept instead points at rings the remote has already
			 * abandoned -- the link stays down until the next
			 * reboot. Tear it down so the start can succeed.
			 */
			if (evt == SAT_CTXT_TYPE_CHAN && sat_dev->chan_started) {
				dev_info(dev, "sat: chan %d restarted by %s\n",
					 id, sat_cntrl->subsys->name);
				mhi_unprepare_from_transfer(sat_dev->mhi_dev);
				sat_dev->chan_started = false;
			}

			gen_ctxt.type = MHI_CTXT_TYPE_GENERIC;
			gen_ctxt.ctxt_base = MHI_TRE_GET_PTR(pkt);
			gen_ctxt.ctxt_size = MHI_TRE_GET_SIZE(pkt);

			buf.buf = &gen_ctxt;
			buf.len = sizeof(gen_ctxt);
			buf.name = TO_SAT_CTXT_TYPE_STR(evt);

			if (!mhi_device_configure(sat_dev->mhi_dev,
						  DMA_BIDIRECTIONAL, &buf, 1))
				code = MHI_EV_CC_SUCCESS;

			dev_dbg(dev, "sat: CTXT UPDATE %s:%d %s\n", buf.name, id,
				code == MHI_EV_CC_SUCCESS ? "ok" : "failed");
			break;
		}
		case MHI_PKT_TYPE_START_CHAN_CMD: {
			int id = MHI_TRE_GET_ID(pkt);
			struct mhi_sat_device *sat_dev;

			sat_dev = find_sat_dev_by_id(sat_cntrl, id,
						     SAT_CTXT_TYPE_CHAN);
			if (!sat_dev || sat_dev->chan_started) {
				dev_err(dev, "sat: bad START for chan %d\n", id);
				break;
			}

			if (!mhi_prepare_for_transfer(sat_dev->mhi_dev)) {
				sat_dev->chan_started = true;
				code = MHI_EV_CC_SUCCESS;
			}

			dev_dbg(dev, "sat: START chan %d %s\n", id,
				code == MHI_EV_CC_SUCCESS ? "ok" : "failed");
			break;
		}
		case MHI_PKT_TYPE_RESET_CHAN_CMD: {
			int id = MHI_TRE_GET_ID(pkt);
			struct mhi_sat_device *sat_dev;

			sat_dev = find_sat_dev_by_id(sat_cntrl, id,
						     SAT_CTXT_TYPE_CHAN);
			if (!sat_dev || !sat_dev->chan_started) {
				dev_err(dev, "sat: bad RESET for chan %d\n", id);
				break;
			}

			mhi_unprepare_from_transfer(sat_dev->mhi_dev);
			sat_dev->chan_started = false;
			code = MHI_EV_CC_SUCCESS;

			dev_dbg(dev, "sat: RESET chan %d ok\n", id);
			break;
		}
		default:
			dev_err(dev, "sat: unhandled command type %u\n",
				(u32)MHI_TRE_GET_TYPE(pkt));
			break;
		}

		mhi_sat_log(sat_cntrl, log_type, log_id, log_er, log_ptr,
			    log_size, code);

		pkt->ptr = MHI_TRE_EVT_CMD_COMPLETION_PTR(completion_ptr);
		pkt->dword[0] = MHI_TRE_EVT_CMD_COMPLETION_D0(code);
		pkt->dword[1] = MHI_TRE_EVT_CMD_COMPLETION_D1;
	}
}

/*
 * Tell the remote the device is gone so it stops using the shared rings. This
 * is the only message in the protocol the remote has to acknowledge; everything
 * else we send is an event it may silently ignore.
 *
 * The caller holds cmd_wait_mutex and does the waiting, so that a probe can
 * wait for the reply on its own terms.
 */
static int mhi_sat_send_sys_err_cmd(struct mhi_sat_cntrl *sat_cntrl)
{
	struct sat_tre *pkt;
	void *msg;
	int ret;

	msg = kzalloc(SAT_MSG_SIZE(1), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	pkt = SAT_TRE_OFFSET(msg);
	pkt->ptr = MHI_TRE_CMD_SYS_ERR_PTR;
	pkt->dword[0] = MHI_TRE_CMD_SYS_ERR_D0;
	pkt->dword[1] = MHI_TRE_CMD_SYS_ERR_D1;

	/* arm before sending: the reply may beat us back here */
	reinit_completion(&sat_cntrl->completion);

	ret = mhi_sat_send_msg(sat_cntrl, SAT_MSG_ID_CMD, SAT_RESERVED_SEQ_NUM,
			       msg, SAT_MSG_SIZE(1));
	kfree(msg);

	return ret;
}

static void mhi_sat_send_sys_err(struct mhi_sat_cntrl *sat_cntrl)
{
	struct device *dev = sat_cntrl->mhi_cntrl->cntrl_dev;
	int ret;

	flush_work(&sat_cntrl->connect_work);
	flush_work(&sat_cntrl->process_work);

	mutex_lock(&sat_cntrl->cmd_wait_mutex);

	ret = mhi_sat_send_sys_err_cmd(sat_cntrl);
	if (ret)
		dev_err(dev, "sat: failed to send SYS_ERR: %d\n", ret);
	else
		mhi_sat_wait_cmd_completion(sat_cntrl);

	mutex_unlock(&sat_cntrl->cmd_wait_mutex);
}

static void mhi_sat_error_worker(void *data, async_cookie_t cookie)
{
	struct mhi_sat_cntrl *sat_cntrl = data;
	struct device *dev = sat_cntrl->mhi_cntrl->cntrl_dev;
	struct sat_tre *pkt;
	void *msg;
	int ret;

	flush_work(&sat_cntrl->connect_work);
	flush_work(&sat_cntrl->process_work);

	msg = kzalloc(SAT_MSG_SIZE(1), GFP_KERNEL);
	if (!msg)
		return;

	pkt = SAT_TRE_OFFSET(msg);
	pkt->ptr = MHI_TRE_EVT_MHI_STATE_PTR;
	pkt->dword[0] = MHI_TRE_EVT_MHI_STATE_D0(MHI_STATE_SYS_ERR);
	pkt->dword[1] = MHI_TRE_EVT_MHI_STATE_D1;

	ret = mhi_sat_send_msg(sat_cntrl, SAT_MSG_ID_EVT, SAT_RESERVED_SEQ_NUM,
			       msg, SAT_MSG_SIZE(1));
	kfree(msg);

	dev_dbg(dev, "sat: SYS_ERR event send %s\n", ret ? "failed" : "ok");
}

static void mhi_sat_process_worker(struct work_struct *work)
{
	struct mhi_sat_cntrl *sat_cntrl = container_of(work,
					struct mhi_sat_cntrl, process_work);
	struct mhi_sat_packet *packet, *tmp;
	struct sat_header *hdr;
	struct sat_tre *pkt;
	LIST_HEAD(head);

	spin_lock_irq(&sat_cntrl->pkt_lock);
	list_splice_tail_init(&sat_cntrl->packet_list, &head);
	spin_unlock_irq(&sat_cntrl->pkt_lock);

	list_for_each_entry_safe(packet, tmp, &head, node) {
		hdr = packet->msg;
		pkt = SAT_TRE_OFFSET(packet->msg);

		list_del(&packet->node);

		if (!MHI_SAT_ACTIVE(sat_cntrl))
			goto next;

		mhi_sat_process_cmds(sat_cntrl, hdr, pkt);

		/* the rewritten packets are the response events */
		mhi_sat_send_msg(sat_cntrl, SAT_MSG_ID_EVT, hdr->seq,
				 packet->msg,
				 SAT_MSG_SIZE(SAT_TRE_NUM_PKTS(hdr->payload_size)));
next:
		kfree(packet);
	}
}

/*
 * Tell the remote where the MHI registers live, which event rings it owns, and
 * that the device is already in M0/AMSS (the AP brought it up long before).
 * The remote is not expected to answer, only to start asking for its channels.
 */
static int mhi_sat_send_hello(struct mhi_sat_cntrl *sat_cntrl)
{
	struct sat_tre *pkt;
	void *msg;
	int ret;

	msg = kzalloc(SAT_MSG_SIZE(3), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	pkt = SAT_TRE_OFFSET(msg);

	pkt->ptr = MHI_TRE_EVT_CFG_PTR(sat_cntrl->base_addr);
	pkt->dword[0] = MHI_TRE_EVT_CFG_D0(sat_cntrl->er_base,
					   sat_cntrl->num_er);
	pkt->dword[1] = MHI_TRE_EVT_CFG_D1;
	pkt++;

	pkt->ptr = MHI_TRE_EVT_MHI_STATE_PTR;
	pkt->dword[0] = MHI_TRE_EVT_MHI_STATE_D0(MHI_STATE_M0);
	pkt->dword[1] = MHI_TRE_EVT_MHI_STATE_D1;
	pkt++;

	pkt->ptr = MHI_TRE_EVT_EE_PTR;
	pkt->dword[0] = MHI_TRE_EVT_EE_D0(MHI_EE_AMSS);
	pkt->dword[1] = MHI_TRE_EVT_EE_D1;

	ret = mhi_sat_send_msg(sat_cntrl, SAT_MSG_ID_EVT, SAT_RESERVED_SEQ_NUM,
			       msg, SAT_MSG_SIZE(3));
	kfree(msg);

	if (!ret)
		sat_cntrl->hellos++;

	return ret;
}

/* Greet the remote once the link is up and every shared channel has probed. */
static void mhi_sat_connect_worker(struct work_struct *work)
{
	struct mhi_sat_cntrl *sat_cntrl = container_of(work,
					struct mhi_sat_cntrl, connect_work);
	struct device *dev = sat_cntrl->mhi_cntrl->cntrl_dev;
	struct mhi_sat_subsys *subsys = sat_cntrl->subsys;
	enum mhi_sat_state prev_state;
	int ret;

	spin_lock_irq(&sat_cntrl->state_lock);
	if (!subsys->rpdev ||
	    sat_cntrl->max_devices != sat_cntrl->num_devices ||
	    !MHI_SAT_ALLOW_CONN(sat_cntrl)) {
		spin_unlock_irq(&sat_cntrl->state_lock);
		return;
	}
	prev_state = sat_cntrl->state;
	sat_cntrl->state = SAT_RUNNING;
	spin_unlock_irq(&sat_cntrl->state_lock);

	ret = mhi_sat_send_hello(sat_cntrl);
	if (ret) {
		dev_err(dev, "sat: failed to send hello: %d\n", ret);
		goto err;
	}

	dev_info(dev, "sat: %s link up, device 0x%x greeted (er %d..%d)\n",
		 subsys->name, sat_cntrl->dev_id, sat_cntrl->er_base,
		 sat_cntrl->er_max);
	return;

err:
	spin_lock_irq(&sat_cntrl->state_lock);
	if (MHI_SAT_ACTIVE(sat_cntrl))
		sat_cntrl->state = prev_state;
	spin_unlock_irq(&sat_cntrl->state_lock);
}

static struct dentry *mhi_sat_debugfs_root;

/*
 * Everything the host knows about one satellite link, so a silent remote can be
 * told apart from a link that was never offered anything: whether the rpmsg
 * channel exists, whether all the lent channels probed (the hello is only sent
 * once they have), what the hello claimed, and how much has been said in each
 * direction since.
 */
static int mhi_sat_status_show(struct seq_file *s, void *unused)
{
	struct mhi_sat_cntrl *sat_cntrl = s->private;
	enum mhi_sat_state state = sat_cntrl->state;

	seq_printf(s, "subsys:     %s\n", sat_cntrl->subsys->name);
	seq_printf(s, "link:       %s\n",
		   sat_cntrl->subsys->rpdev ? "up" : "down");
	seq_printf(s, "state:      %s\n",
		   state < SAT_STATE_MAX ? sat_state_str[state] : "invalid");
	seq_printf(s, "dev id:     0x%08x\n", sat_cntrl->dev_id);
	seq_printf(s, "mmio base:  %pap\n", &sat_cntrl->base_addr);
	seq_printf(s, "devices:    %d of %d\n", sat_cntrl->num_devices,
		   sat_cntrl->max_devices);
	seq_printf(s, "event ring: %d..%d (%d)\n", sat_cntrl->er_base,
		   sat_cntrl->er_max, sat_cntrl->num_er);
	seq_printf(s, "tx:         %u messages, %u hellos, last cmd seq %u\n",
		   sat_cntrl->seq, sat_cntrl->hellos, sat_cntrl->last_cmd_seq);
	seq_printf(s, "rx:         %u commands, %u events\n",
		   sat_cntrl->rx_cmds, sat_cntrl->rx_evts);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mhi_sat_status);

static const char *mhi_sat_cmd_str(u8 type)
{
	switch (type) {
	case MHI_PKT_TYPE_IOMMU_MAP_CMD:
		return "IOMMU_MAP";
	case MHI_PKT_TYPE_CTXT_UPDATE_CMD:
		return "CTXT_UPDATE";
	case MHI_PKT_TYPE_START_CHAN_CMD:
		return "START_CHAN";
	case MHI_PKT_TYPE_RESET_CHAN_CMD:
		return "RESET_CHAN";
	default:
		return "UNKNOWN";
	}
}

static int mhi_sat_log_show(struct seq_file *s, void *unused)
{
	struct mhi_sat_cntrl *sat_cntrl = s->private;
	struct mhi_sat_log_entry *log;
	unsigned int next, i, n;
	unsigned long flags;

	log = kmalloc_array(MHI_SAT_LOG_ENTRIES, sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;

	spin_lock_irqsave(&sat_cntrl->log_lock, flags);
	memcpy(log, sat_cntrl->log, MHI_SAT_LOG_ENTRIES * sizeof(*log));
	next = sat_cntrl->log_next;
	spin_unlock_irqrestore(&sat_cntrl->log_lock, flags);

	if (!next) {
		seq_puts(s, "(the remote has asked for nothing)\n");
		goto out;
	}

	n = min_t(unsigned int, next, MHI_SAT_LOG_ENTRIES);

	for (i = next - n; i < next; i++) {
		struct mhi_sat_log_entry *e = &log[i % MHI_SAT_LOG_ENTRIES];

		seq_printf(s, "[%5llu.%06llu] %-11s ", e->ts / NSEC_PER_SEC,
			   (e->ts % NSEC_PER_SEC) / 1000, mhi_sat_cmd_str(e->type));

		switch (e->type) {
		case MHI_PKT_TYPE_IOMMU_MAP_CMD:
			seq_printf(s, "phys 0x%016llx size %u", e->ptr, e->size);
			break;
		case MHI_PKT_TYPE_CTXT_UPDATE_CMD:
			seq_printf(s, "%s %-3u base 0x%016llx size %u",
				   e->er ? "ring" : "chan", e->id, e->ptr,
				   e->size);
			break;
		default:
			seq_printf(s, "chan %-3u", e->id);
			break;
		}

		seq_printf(s, "  -> %s\n",
			   e->ccs == MHI_EV_CC_SUCCESS ? "granted" : "REFUSED");
	}

out:
	kfree(log);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mhi_sat_log);

/* Send the hello again. Reading this file is the trigger. */
static int mhi_sat_hello_show(struct seq_file *s, void *unused)
{
	struct mhi_sat_cntrl *sat_cntrl = s->private;
	int ret;

	if (!sat_cntrl->subsys->rpdev) {
		seq_puts(s, "no link\n");
		return 0;
	}

	ret = mhi_sat_send_hello(sat_cntrl);
	seq_printf(s, "hello seq %u: %s (%d)\n", sat_cntrl->seq,
		   ret ? "failed" : "sent", ret);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mhi_sat_hello);

/*
 * Is the remote's satellite client listening at all?
 *
 * Nothing it is normally sent requires an answer, so a client that is loaded
 * but uninterested looks exactly like one that was never started. SYS_ERR is
 * the exception: the remote must acknowledge it. An ack therefore means the
 * client is alive and reading our messages, and the reason it never asks for
 * its channels lies inside it; silence means our messages are not reaching it
 * at all, which is a problem on this side of the link.
 *
 * Note this tells the remote the device has died. A client that believes it
 * will not come back for the rest of this boot, so probe last.
 */
#define MHI_SAT_PROBE_TIMEOUT_MS	2000

static int mhi_sat_probe_show(struct seq_file *s, void *unused)
{
	struct mhi_sat_cntrl *sat_cntrl = s->private;
	unsigned long left;
	int ret;

	if (!sat_cntrl->subsys->rpdev) {
		seq_puts(s, "no link\n");
		return 0;
	}

	mutex_lock(&sat_cntrl->cmd_wait_mutex);

	ret = mhi_sat_send_sys_err_cmd(sat_cntrl);
	if (ret) {
		seq_printf(s, "SYS_ERR: send failed (%d)\n", ret);
		goto out;
	}

	seq_printf(s, "SYS_ERR: sent, seq %u\n", sat_cntrl->last_cmd_seq);

	left = wait_for_completion_timeout(&sat_cntrl->completion,
				msecs_to_jiffies(MHI_SAT_PROBE_TIMEOUT_MS));
	if (left)
		seq_printf(s, "reply:   acked, ccs %d -- remote is listening\n",
			   sat_cntrl->last_cmd_ccs);
	else
		seq_printf(s, "reply:   none after %u ms -- remote is silent\n",
			   MHI_SAT_PROBE_TIMEOUT_MS);

out:
	mutex_unlock(&sat_cntrl->cmd_wait_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mhi_sat_probe);

/*
 * Is the remote actually talking to the device?
 *
 * Once its channels are started the remote drives them itself: it writes the
 * doorbell registers in the device's own address space and reads its events
 * straight out of a ring the host must not touch. None of that is visible from
 * here, so a remote that has been given everything and does nothing looks
 * exactly like one that is working -- which is the state this board is in, with
 * a voice call established and not one byte crossing the mailbox.
 *
 * The doorbells themselves are readable, though. They live in the device's
 * MMIO, which the host maps, and they hold the last value written. Read them
 * twice around something the remote should be doing, and a value that has
 * moved says it is sending; one that has not says it never tried. That splits
 * the fault cleanly in two, which nothing on the host side can.
 */
/*
 * How often the remote rings, rather than what it rings with.
 *
 * The rings themselves are in memory the remote owns and the host does not
 * map, and on this SoC reading that memory from here risks a permission fault
 * that takes the whole board down -- so the buffers stay untouched. The
 * doorbells alone still separate the two things worth telling apart: a stream
 * moves them steadily for as long as it runs, while session and control
 * traffic moves them in bursts around whatever provoked it.
 */
#define MHI_SAT_WATCH_INTERVAL_MS	20
#define MHI_SAT_WATCH_SAMPLES		250	/* five seconds of them */

struct mhi_sat_watch {
	u32 off;
	u64 last;
	unsigned int changes;
	unsigned int first_ms;
	unsigned int last_ms;
	int id;
	bool ring;
};

static bool mhi_sat_watch_read(struct mhi_controller *mhi_cntrl, u32 off,
			       u64 *val)
{
	u32 lo, hi;

	if (mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, off, &lo) ||
	    mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, off + 4, &hi))
		return false;

	*val = ((u64)hi << 32) | lo;
	return true;
}

static int mhi_sat_cadence_show(struct seq_file *s, void *unused)
{
	struct mhi_sat_cntrl *sat_cntrl = s->private;
	struct mhi_controller *mhi_cntrl = sat_cntrl->mhi_cntrl;
	struct mhi_sat_device *sat_dev;
	struct mhi_sat_watch *w;
	unsigned int n = 0, i, j;
	u32 chdb_off, er_off;
	int ret;

	ret = mhi_get_channel_doorbell_offset(mhi_cntrl, &chdb_off);
	if (ret)
		return ret;

	ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, ERDBOFF, &er_off);
	if (ret)
		return ret;

	w = kcalloc(MHI_SAT_MAX_DEVICES + sat_cntrl->er_max + 1, sizeof(*w),
		    GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	mutex_lock(&sat_cntrl->list_mutex);
	list_for_each_entry(sat_dev, &sat_cntrl->dev_list, node) {
		struct mhi_device *mhi_dev = sat_dev->mhi_dev;

		w[n].id = mhi_dev->dl_chan_id ?: mhi_dev->ul_chan_id;
		w[n].off = chdb_off + (8 * w[n].id);
		n++;
	}
	mutex_unlock(&sat_cntrl->list_mutex);

	for (i = sat_cntrl->er_base; i <= sat_cntrl->er_max; i++) {
		w[n].id = i;
		w[n].off = er_off + (8 * i);
		w[n].ring = true;
		n++;
	}

	for (i = 0; i < n; i++)
		mhi_sat_watch_read(mhi_cntrl, w[i].off, &w[i].last);

	for (j = 0; j < MHI_SAT_WATCH_SAMPLES; j++) {
		unsigned int ms = j * MHI_SAT_WATCH_INTERVAL_MS;

		msleep(MHI_SAT_WATCH_INTERVAL_MS);

		for (i = 0; i < n; i++) {
			u64 val;

			if (!mhi_sat_watch_read(mhi_cntrl, w[i].off, &val))
				continue;
			if (val == w[i].last)
				continue;

			if (!w[i].changes)
				w[i].first_ms = ms;
			w[i].last_ms = ms;
			w[i].changes++;
			w[i].last = val;
		}
	}

	seq_printf(s, "%u samples over %u ms\n\n", MHI_SAT_WATCH_SAMPLES,
		   MHI_SAT_WATCH_SAMPLES * MHI_SAT_WATCH_INTERVAL_MS);
	seq_printf(s, "%-12s %-8s %-9s %-9s %s\n",
		   "doorbell", "moves", "first ms", "last ms", "mean gap ms");

	for (i = 0; i < n; i++) {
		unsigned int gap = 0;

		if (w[i].changes > 1)
			gap = (w[i].last_ms - w[i].first_ms) / (w[i].changes - 1);

		seq_printf(s, "%-5s %-6d %-8u %-9u %-9u %u\n",
			   w[i].ring ? "ring" : "chan", w[i].id, w[i].changes,
			   w[i].first_ms, w[i].last_ms, gap);
	}

	kfree(w);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mhi_sat_cadence);

static int mhi_sat_doorbells_show(struct seq_file *s, void *unused)
{
	struct mhi_sat_cntrl *sat_cntrl = s->private;
	struct mhi_controller *mhi_cntrl = sat_cntrl->mhi_cntrl;
	struct mhi_sat_device *sat_dev;
	u32 chdb_off, er_off;
	int ret;

	ret = mhi_get_channel_doorbell_offset(mhi_cntrl, &chdb_off);
	if (ret) {
		seq_printf(s, "cannot read the doorbell offset: %d\n", ret);
		return 0;
	}

	ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, ERDBOFF, &er_off);
	if (ret) {
		seq_printf(s, "cannot read the event doorbell offset: %d\n", ret);
		return 0;
	}

	seq_printf(s, "%-14s %-10s %s\n", "doorbell", "offset", "value");

	mutex_lock(&sat_cntrl->list_mutex);
	list_for_each_entry(sat_dev, &sat_cntrl->dev_list, node) {
		struct mhi_device *mhi_dev = sat_dev->mhi_dev;
		int chan = mhi_dev->dl_chan_id ?: mhi_dev->ul_chan_id;
		u32 off = chdb_off + (8 * chan);
		u32 lo, hi;

		if (mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, off, &lo) ||
		    mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, off + 4, &hi)) {
			seq_printf(s, "chan %-9d 0x%08x unreadable\n", chan, off);
			continue;
		}

		seq_printf(s, "chan %-9d 0x%08x 0x%08x%08x%s\n", chan, off, hi, lo,
			   sat_dev->chan_started ? "" : " (not started)");
	}
	mutex_unlock(&sat_cntrl->list_mutex);

	/* The rings the remote was handed, so its own consumption shows too. */
	for (ret = sat_cntrl->er_base; ret <= sat_cntrl->er_max; ret++) {
		u32 off = er_off + (8 * ret);
		u32 lo, hi;

		if (mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, off, &lo) ||
		    mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, off + 4, &hi)) {
			seq_printf(s, "ring %-9d 0x%08x unreadable\n", ret, off);
			continue;
		}

		seq_printf(s, "ring %-9d 0x%08x 0x%08x%08x\n", ret, off, hi, lo);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mhi_sat_doorbells);

static void mhi_sat_debugfs_create(struct mhi_sat_cntrl *sat_cntrl)
{
	char name[32];

	snprintf(name, sizeof(name), "%s-%08x", sat_cntrl->subsys->name,
		 sat_cntrl->dev_id);

	sat_cntrl->dbgfs = debugfs_create_dir(name, mhi_sat_debugfs_root);

	debugfs_create_file("status", 0444, sat_cntrl->dbgfs, sat_cntrl,
			    &mhi_sat_status_fops);
	debugfs_create_file("log", 0444, sat_cntrl->dbgfs, sat_cntrl,
			    &mhi_sat_log_fops);
	debugfs_create_file("doorbells", 0444, sat_cntrl->dbgfs, sat_cntrl,
			    &mhi_sat_doorbells_fops);
	debugfs_create_file("cadence", 0444, sat_cntrl->dbgfs, sat_cntrl,
			    &mhi_sat_cadence_fops);
	debugfs_create_file("hello", 0400, sat_cntrl->dbgfs, sat_cntrl,
			    &mhi_sat_hello_fops);
	debugfs_create_file("probe", 0400, sat_cntrl->dbgfs, sat_cntrl,
			    &mhi_sat_probe_fops);
}

static void mhi_sat_process_events(struct mhi_sat_cntrl *sat_cntrl,
				   struct sat_header *hdr, struct sat_tre *pkt)
{
	int num_pkts = SAT_TRE_NUM_PKTS(hdr->payload_size);
	int i;

	for (i = 0; i < num_pkts; i++, pkt++) {
		if (MHI_TRE_GET_TYPE(pkt) != MHI_PKT_TYPE_CMD_COMPLETION_EVENT)
			continue;

		if (hdr->reply_seq != sat_cntrl->last_cmd_seq)
			continue;

		sat_cntrl->last_cmd_ccs = MHI_TRE_GET_CCS(pkt);
		complete(&sat_cntrl->completion);
	}
}

static int mhi_sat_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			    void *priv, u32 src)
{
	struct mhi_sat_subsys *subsys = dev_get_drvdata(&rpdev->dev);
	struct sat_header *hdr = data;
	struct sat_tre *pkt = SAT_TRE_OFFSET(data);
	struct mhi_sat_cntrl *sat_cntrl;
	struct mhi_sat_packet *packet;
	unsigned long flags;

	if (!mhi_sat_isvalid_header(hdr, len)) {
		dev_err(&rpdev->dev, "sat: invalid header\n");
		return 0;
	}

	dev_dbg(&rpdev->dev,
		"sat: rx dev 0x%x msg 0x%x seq %u reply_seq %u payload %u\n",
		hdr->dev_id, hdr->msg_id, hdr->seq, hdr->reply_seq,
		hdr->payload_size);

	sat_cntrl = find_sat_cntrl_by_id(subsys, hdr->dev_id);
	if (!sat_cntrl) {
		/*
		 * The remote is talking about a device we never announced. Say
		 * so: dropping this silently makes an id mismatch look exactly
		 * like a remote that never said anything at all.
		 */
		dev_warn_once(&rpdev->dev,
			      "sat: message for unknown device 0x%x, dropping\n",
			      hdr->dev_id);
		return 0;
	}

	/*
	 * The remote has never been heard from on some boards, so say when it
	 * first is -- without needing dynamic debug armed beforehand to catch it.
	 */
	if (!sat_cntrl->rx_cmds && !sat_cntrl->rx_evts)
		dev_info(&rpdev->dev, "sat: first message from %s, msg 0x%x\n",
			 subsys->name, hdr->msg_id);

	/* completions are handled inline regardless of state */
	if (hdr->msg_id == SAT_MSG_ID_EVT) {
		sat_cntrl->rx_evts++;
		mhi_sat_process_events(sat_cntrl, hdr, pkt);
		return 0;
	}

	sat_cntrl->rx_cmds++;

	if (unlikely(!MHI_SAT_ACTIVE(sat_cntrl))) {
		dev_warn_once(&rpdev->dev,
			      "sat: msg 0x%x for device 0x%x while state %d, dropping\n",
			      hdr->msg_id, hdr->dev_id, sat_cntrl->state);
		return 0;
	}

	/* commands may sleep (DMA mapping, channel start): defer to a worker */
	packet = kmalloc(sizeof(*packet) + len, GFP_ATOMIC);
	if (!packet)
		return 0;

	packet->cntrl = sat_cntrl;
	packet->msg = packet + 1;
	memcpy(packet->msg, data, len);

	spin_lock_irqsave(&sat_cntrl->pkt_lock, flags);
	list_add_tail(&packet->node, &sat_cntrl->packet_list);
	spin_unlock_irqrestore(&sat_cntrl->pkt_lock, flags);

	schedule_work(&sat_cntrl->process_work);

	return 0;
}

static void mhi_sat_free_addr_map(struct mhi_sat_cntrl *sat_cntrl)
{
	struct mhi_sat_addr_map *map, *tmp;

	list_for_each_entry_safe(map, tmp, &sat_cntrl->addr_map_list, node) {
		dma_unmap_resource(sat_cntrl->mhi_cntrl->cntrl_dev,
				   map->dma_addr, map->len,
				   DMA_BIDIRECTIONAL, 0);
		list_del(&map->node);
		kfree(map);
	}
}

static void mhi_sat_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct mhi_sat_subsys *subsys = dev_get_drvdata(&rpdev->dev);
	struct mhi_sat_cntrl *sat_cntrl;
	struct mhi_sat_device *sat_dev;

	mutex_lock(&subsys->cntrl_mutex);
	list_for_each_entry(sat_cntrl, &subsys->cntrl_list, node) {
		async_synchronize_cookie(sat_cntrl->error_cookie + 1);

		spin_lock_irq(&sat_cntrl->state_lock);
		if (MHI_SAT_IN_ERROR(sat_cntrl)) {
			sat_cntrl->state = SAT_DISABLED;
			spin_unlock_irq(&sat_cntrl->state_lock);
			continue;
		}
		sat_cntrl->state = SAT_DISCONNECTED;
		spin_unlock_irq(&sat_cntrl->state_lock);

		flush_work(&sat_cntrl->connect_work);
		flush_work(&sat_cntrl->process_work);

		mutex_lock(&sat_cntrl->list_mutex);
		list_for_each_entry(sat_dev, &sat_cntrl->dev_list, node) {
			if (sat_dev->chan_started) {
				mhi_unprepare_from_transfer(sat_dev->mhi_dev);
				sat_dev->chan_started = false;
			}
		}
		mhi_sat_free_addr_map(sat_cntrl);
		mutex_unlock(&sat_cntrl->list_mutex);
	}
	subsys->rpdev = NULL;
	mutex_unlock(&subsys->cntrl_mutex);
}

static int mhi_sat_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct mhi_sat_subsys *subsys;
	struct mhi_sat_cntrl *sat_cntrl;
	const char *subsys_name;
	int ret;

	/*
	 * The glink edge node's label names the remote ("lpass", "dsps"),
	 * which is how we tell the ADSP link from the sensor DSP one.
	 */
	ret = of_property_read_string(rpdev->dev.parent->of_node, "label",
				      &subsys_name);
	if (ret)
		return ret;

	subsys = find_subsys_by_name(subsys_name);
	if (!subsys)
		return -EINVAL;

	mutex_lock(&subsys->cntrl_mutex);

	dev_set_drvdata(&rpdev->dev, subsys);
	subsys->rpdev = rpdev;

	spin_lock_irq(&subsys->cntrl_lock);
	list_for_each_entry(sat_cntrl, &subsys->cntrl_list, node)
		schedule_work(&sat_cntrl->connect_work);
	spin_unlock_irq(&subsys->cntrl_lock);

	mutex_unlock(&subsys->cntrl_mutex);

	dev_info(&rpdev->dev, "sat: %s satellite link ready\n", subsys->name);

	return 0;
}

static struct rpmsg_device_id mhi_sat_rpmsg_match_table[] = {
	{ .name = "mhi_sat" },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, mhi_sat_rpmsg_match_table);

static struct rpmsg_driver mhi_sat_rpmsg_driver = {
	.id_table = mhi_sat_rpmsg_match_table,
	.probe = mhi_sat_rpmsg_probe,
	.remove = mhi_sat_rpmsg_remove,
	.callback = mhi_sat_rpmsg_cb,
	.drv = {
		.name = "mhi,sat_rpmsg",
	},
};

static void mhi_sat_dev_status_cb(struct mhi_device *mhi_dev,
				  enum mhi_callback mhi_cb)
{
	struct mhi_sat_device *sat_dev = dev_get_drvdata(&mhi_dev->dev);
	struct mhi_sat_cntrl *sat_cntrl;
	unsigned long flags;

	if (mhi_cb != MHI_CB_FATAL_ERROR || !sat_dev)
		return;

	sat_cntrl = sat_dev->cntrl;

	spin_lock_irqsave(&sat_cntrl->state_lock, flags);
	if (MHI_SAT_ACTIVE(sat_cntrl)) {
		sat_cntrl->error_cookie = async_schedule(mhi_sat_error_worker,
							 sat_cntrl);
		sat_cntrl->state = SAT_FATAL_DETECT;
	} else {
		sat_cntrl->state = SAT_DISABLED;
	}
	spin_unlock_irqrestore(&sat_cntrl->state_lock, flags);
}

static void mhi_sat_dev_remove(struct mhi_device *mhi_dev)
{
	struct mhi_sat_device *sat_dev = dev_get_drvdata(&mhi_dev->dev);
	struct mhi_sat_cntrl *sat_cntrl = sat_dev->cntrl;
	struct mhi_sat_subsys *subsys = sat_cntrl->subsys;
	bool send_sys_err = false;

	mutex_lock(&sat_cntrl->list_mutex);
	list_del(&sat_dev->node);
	mutex_unlock(&sat_cntrl->list_mutex);

	sat_cntrl->num_devices--;

	mutex_lock(&subsys->cntrl_mutex);

	async_synchronize_cookie(sat_cntrl->error_cookie + 1);

	spin_lock_irq(&sat_cntrl->state_lock);
	if (MHI_SAT_ALLOW_SYS_ERR(sat_cntrl))
		send_sys_err = true;
	sat_cntrl->state = SAT_ERROR;
	spin_unlock_irq(&sat_cntrl->state_lock);

	if (send_sys_err)
		mhi_sat_send_sys_err(sat_cntrl);

	if (sat_cntrl->num_devices) {
		mutex_unlock(&subsys->cntrl_mutex);
		return;
	}

	debugfs_remove_recursive(sat_cntrl->dbgfs);

	cancel_work_sync(&sat_cntrl->connect_work);
	cancel_work_sync(&sat_cntrl->process_work);

	mutex_lock(&sat_cntrl->list_mutex);
	mhi_sat_free_addr_map(sat_cntrl);
	mutex_unlock(&sat_cntrl->list_mutex);

	spin_lock_irq(&subsys->cntrl_lock);
	list_del(&sat_cntrl->node);
	spin_unlock_irq(&subsys->cntrl_lock);

	mutex_destroy(&sat_cntrl->cmd_wait_mutex);
	mutex_destroy(&sat_cntrl->list_mutex);
	kfree(sat_cntrl);

	mutex_unlock(&subsys->cntrl_mutex);
}

/*
 * A unique id for the controller, as the remote knows it. Downstream builds it
 * from the MHI device's PCI topology fields; mainline keeps that on the PCI
 * device itself, so derive it from there.
 */
static u32 mhi_sat_dev_id(struct mhi_controller *mhi_cntrl)
{
	struct device *cntrl_dev = mhi_cntrl->cntrl_dev;
	struct pci_dev *pdev;

	if (!dev_is_pci(cntrl_dev))
		return 0;

	pdev = to_pci_dev(cntrl_dev);

	return (pdev->device & 0xFFFF) << 16 |
	       (pci_domain_nr(pdev->bus) & 0xF) << 12 |
	       (pdev->bus->number & 0xFF) << 4 |
	       (PCI_SLOT(pdev->devfn) & 0xF);
}

static phys_addr_t mhi_sat_base_addr(struct mhi_controller *mhi_cntrl)
{
	struct device *cntrl_dev = mhi_cntrl->cntrl_dev;

	if (!dev_is_pci(cntrl_dev))
		return 0;

	/* MHI MMIO lives in BAR 0 on every controller this driver serves */
	return pci_resource_start(to_pci_dev(cntrl_dev), 0);
}

static int mhi_sat_dev_probe(struct mhi_device *mhi_dev,
			     const struct mhi_device_id *id)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_sat_subsys *subsys = &mhi_sat_subsys_array[id->driver_data];
	struct mhi_sat_cntrl *sat_cntrl;
	struct mhi_sat_device *sat_dev;
	u32 dev_id = mhi_sat_dev_id(mhi_cntrl);
	int er_index = mhi_sat_dev_er_index(mhi_dev);

	sat_cntrl = find_sat_cntrl_by_id(subsys, dev_id);
	if (!sat_cntrl) {
		sat_cntrl = kzalloc(sizeof(*sat_cntrl), GFP_KERNEL);
		if (!sat_cntrl)
			return -ENOMEM;

		sat_cntrl->max_devices = MHI_SAT_MAX_DEVICES;
		sat_cntrl->dev_id = dev_id;
		sat_cntrl->base_addr = mhi_sat_base_addr(mhi_cntrl);
		sat_cntrl->er_base = er_index;
		sat_cntrl->mhi_cntrl = mhi_cntrl;
		sat_cntrl->last_cmd_seq = SAT_RESERVED_SEQ_NUM;
		sat_cntrl->subsys = subsys;
		init_completion(&sat_cntrl->completion);
		mutex_init(&sat_cntrl->list_mutex);
		mutex_init(&sat_cntrl->cmd_wait_mutex);
		spin_lock_init(&sat_cntrl->pkt_lock);
		spin_lock_init(&sat_cntrl->state_lock);
		spin_lock_init(&sat_cntrl->log_lock);
		INIT_WORK(&sat_cntrl->connect_work, mhi_sat_connect_worker);
		INIT_WORK(&sat_cntrl->process_work, mhi_sat_process_worker);
		INIT_LIST_HEAD(&sat_cntrl->dev_list);
		INIT_LIST_HEAD(&sat_cntrl->addr_map_list);
		INIT_LIST_HEAD(&sat_cntrl->packet_list);

		mutex_lock(&subsys->cntrl_mutex);
		spin_lock_irq(&subsys->cntrl_lock);
		list_add(&sat_cntrl->node, &subsys->cntrl_list);
		spin_unlock_irq(&subsys->cntrl_lock);
		mutex_unlock(&subsys->cntrl_mutex);

		mhi_sat_debugfs_create(sat_cntrl);
	}

	sat_cntrl->er_base = min(sat_cntrl->er_base, er_index);
	sat_cntrl->er_max = max(sat_cntrl->er_max, er_index);

	sat_dev = devm_kzalloc(&mhi_dev->dev, sizeof(*sat_dev), GFP_KERNEL);
	if (!sat_dev)
		return -ENOMEM;

	sat_dev->mhi_dev = mhi_dev;
	sat_dev->cntrl = sat_cntrl;

	mutex_lock(&sat_cntrl->list_mutex);
	list_add(&sat_dev->node, &sat_cntrl->dev_list);
	mutex_unlock(&sat_cntrl->list_mutex);

	dev_set_drvdata(&mhi_dev->dev, sat_dev);

	sat_cntrl->num_devices++;

	if (sat_cntrl->num_devices == sat_cntrl->max_devices) {
		sat_cntrl->num_er = (sat_cntrl->er_max - sat_cntrl->er_base) + 1;
		schedule_work(&sat_cntrl->connect_work);
	}

	return 0;
}

/* .driver_data holds the subsystem id */
static const struct mhi_device_id mhi_sat_dev_match_table[] = {
	{ .chan = "ADSP_0", .driver_data = SUBSYS_ADSP },
	{ .chan = "ADSP_1", .driver_data = SUBSYS_ADSP },
	{ .chan = "ADSP_2", .driver_data = SUBSYS_ADSP },
	{ .chan = "ADSP_3", .driver_data = SUBSYS_ADSP },
	{ .chan = "SLPI_0", .driver_data = SUBSYS_SLPI },
	{ .chan = "SLPI_1", .driver_data = SUBSYS_SLPI },
	{ .chan = "SLPI_2", .driver_data = SUBSYS_SLPI },
	{ .chan = "SLPI_3", .driver_data = SUBSYS_SLPI },
	{},
};
MODULE_DEVICE_TABLE(mhi, mhi_sat_dev_match_table);

static struct mhi_driver mhi_sat_dev_driver = {
	.id_table = mhi_sat_dev_match_table,
	.probe = mhi_sat_dev_probe,
	.remove = mhi_sat_dev_remove,
	.status_cb = mhi_sat_dev_status_cb,
	.driver = {
		.name = MHI_SAT_DRIVER_NAME,
	},
};

static int __init mhi_sat_init(void)
{
	struct mhi_sat_subsys *subsys;
	int i, ret;

	subsys = kcalloc(SUBSYS_MAX, sizeof(*subsys), GFP_KERNEL);
	if (!subsys)
		return -ENOMEM;

	mhi_sat_subsys_array = subsys;

	for (i = 0; i < SUBSYS_MAX; i++, subsys++) {
		subsys->name = subsys_names[i];
		mutex_init(&subsys->cntrl_mutex);
		spin_lock_init(&subsys->cntrl_lock);
		INIT_LIST_HEAD(&subsys->cntrl_list);
	}

	mhi_sat_debugfs_root = debugfs_create_dir(MHI_SAT_DRIVER_NAME, NULL);

	ret = register_rpmsg_driver(&mhi_sat_rpmsg_driver);
	if (ret)
		goto err_free;

	ret = mhi_driver_register(&mhi_sat_dev_driver);
	if (ret)
		goto err_rpmsg;

	return 0;

err_rpmsg:
	unregister_rpmsg_driver(&mhi_sat_rpmsg_driver);
err_free:
	debugfs_remove_recursive(mhi_sat_debugfs_root);
	subsys = mhi_sat_subsys_array;
	for (i = 0; i < SUBSYS_MAX; i++, subsys++)
		mutex_destroy(&subsys->cntrl_mutex);
	kfree(mhi_sat_subsys_array);
	mhi_sat_subsys_array = NULL;

	return ret;
}

static void __exit mhi_sat_exit(void)
{
	struct mhi_sat_subsys *subsys = mhi_sat_subsys_array;
	int i;

	mhi_driver_unregister(&mhi_sat_dev_driver);
	unregister_rpmsg_driver(&mhi_sat_rpmsg_driver);

	debugfs_remove_recursive(mhi_sat_debugfs_root);

	for (i = 0; i < SUBSYS_MAX; i++, subsys++)
		mutex_destroy(&subsys->cntrl_mutex);

	kfree(mhi_sat_subsys_array);
	mhi_sat_subsys_array = NULL;
}

module_init(mhi_sat_init);
module_exit(mhi_sat_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI satellite driver");
