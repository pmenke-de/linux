/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_CVP_H
#define _Q6_CVP_H

#include "q6voice.h"

/* Stock voice topologies; the vendor takes these from calibration instead. */
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

struct q6voice_session;

struct q6voice_session *q6cvp_session_create(enum q6voice_path_type path,
					     u16 tx_port, u16 rx_port,
					     bool create_v3);
int q6cvp_set_channel_info(struct q6voice_session *cvp);
int q6cvp_set_media_format(struct q6voice_session *cvp, u16 tx_port, u16 rx_port);
int q6cvp_topology_commit(struct q6voice_session *cvp);
int q6cvp_set_rx_volume(struct q6voice_session *cvp, u32 step, u16 ramp_ms);
int q6cvp_enable(struct q6voice_session *cvp, bool enable);

#endif /*_Q6_CVP_H */
