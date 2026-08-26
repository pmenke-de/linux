// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/firmware/qcom/qcom_scm.h>

static void __iomem *msm_ps_hold;

static int do_msm_poweroff(struct sys_off_data *data)
{
	/*
	 * Quiesce the SPMI arbiter first: lowering PS_HOLD mid-transaction
	 * hangs the PMIC's PBS sequencer, whose watchdog then turns the
	 * reset into a power-off (observed on sm8250 Xiaomi as a phone
	 * that never comes back from a battery-powered reboot). Then let
	 * TZ drop PS_HOLD if it knows how, with the direct write as the
	 * fallback, mirroring the vendor sequence.
	 */
	if (qcom_scm_is_available()) {
		qcom_scm_spmi_pmic_arbiter_halt();
		qcom_scm_deassert_ps_hold();
	}

	writel(0, msm_ps_hold);
	mdelay(10000);

	return NOTIFY_DONE;
}

static int msm_restart_probe(struct platform_device *pdev)
{
	msm_ps_hold = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	/*
	 * Above SYS_OFF_PRIO_FIRMWARE would be dishonest, but this must
	 * outrank PSCI's restart notifier (129): on sm8250 Xiaomi boards
	 * the TZ SYSTEM_RESET implementation powers the board off instead
	 * of resetting it, and the direct PS_HOLD drop - the path the
	 * vendor kernel uses for every reboot - is the one that works.
	 */
	devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
				      SYS_OFF_PRIO_HIGH, do_msm_poweroff, NULL);

	devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
				      SYS_OFF_PRIO_DEFAULT, do_msm_poweroff,
				      NULL);

	return 0;
}

static const struct of_device_id of_msm_restart_match[] = {
	{ .compatible = "qcom,pshold", },
	{},
};
MODULE_DEVICE_TABLE(of, of_msm_restart_match);

static struct platform_driver msm_restart_driver = {
	.probe = msm_restart_probe,
	.driver = {
		.name = "msm-restart",
		.of_match_table = of_match_ptr(of_msm_restart_match),
	},
};
builtin_platform_driver(msm_restart_driver);
