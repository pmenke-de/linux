/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_APOLLO_BREADCRUMB_H__
#define __SOC_QCOM_APOLLO_BREADCRUMB_H__

#if IS_ENABLED(CONFIG_QCOM_APOLLO_BREADCRUMB)
void apollo_breadcrumb_thermal(int tz_id, int temp_mC);
#else
static inline void apollo_breadcrumb_thermal(int tz_id, int temp_mC) {}
#endif

#endif
