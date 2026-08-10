/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_VOICE_CAL_H
#define _Q6_VOICE_CAL_H

#include "q6cvp.h"

struct device;
struct q6voice_cal;
struct q6voice_session;

/*
 * Returns NULL when no calibration is installed for the board, which callers
 * should treat as "carry on without it" rather than as a failure.
 */
struct q6voice_cal *q6voice_cal_load(struct device *dev, const char *name,
				     struct q6voice_session *mvm);
void q6voice_cal_free(struct q6voice_cal *cal);

/*
 * @instance says which era of registration command the vocproc expects; it
 * goes with the vocproc's version, the same way the create command does.
 */
int q6voice_cal_register_vol(struct q6voice_cal *cal,
			     struct q6voice_session *cvp, bool instance);
int q6voice_cal_register_cal(struct q6voice_cal *cal,
			     struct q6voice_session *cvp, bool instance);
int q6voice_cal_register_dev_cfg(struct q6voice_cal *cal,
				 struct q6voice_session *cvp);

#endif /*_Q6_VOICE_CAL_H */
