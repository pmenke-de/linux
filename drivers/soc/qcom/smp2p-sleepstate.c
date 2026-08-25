// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tell interested remote processors whether the application processor is
 * awake, over an SMP2P state entry.
 *
 * Qualcomm DSP firmware watches this bit to adjust its own behaviour
 * around the AP suspending: on the SM8250 the sensor DSP (SLPI) expects
 * it, and without the notification its services fault across a suspend
 * cycle and the whole DSP is restarted on resume, taking every sensor
 * session with it. Ported from the downstream smp2p_sleepstate driver.
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/suspend.h>

#define SLEEPSTATE_PROC_AWAKE_BIT	BIT(12)

struct smp2p_sleepstate {
	struct qcom_smem_state *state;
	struct notifier_block nb;
	struct wakeup_source *ws;
};

static int sleepstate_pm_notifier(struct notifier_block *nb,
				  unsigned long event, void *unused)
{
	struct smp2p_sleepstate *ss = container_of(nb, struct smp2p_sleepstate, nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		qcom_smem_state_update_bits(ss->state,
					    SLEEPSTATE_PROC_AWAKE_BIT, 0);
		break;
	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(ss->state,
					    SLEEPSTATE_PROC_AWAKE_BIT,
					    SLEEPSTATE_PROC_AWAKE_BIT);
		break;
	}

	return NOTIFY_DONE;
}

static irqreturn_t smp2p_sleepstate_handler(int irq, void *data)
{
	struct smp2p_sleepstate *ss = data;

	/* The remote side wants the AP up for a moment; hold it awake. */
	__pm_wakeup_event(ss->ws, 200);
	return IRQ_HANDLED;
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct smp2p_sleepstate *ss;
	int irq, ret;

	ss = devm_kzalloc(dev, sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	ss->state = devm_qcom_smem_state_get(dev, NULL, &ret);
	if (IS_ERR(ss->state))
		return dev_err_probe(dev, PTR_ERR(ss->state),
				     "cannot get smem state\n");

	qcom_smem_state_update_bits(ss->state, SLEEPSTATE_PROC_AWAKE_BIT,
				    SLEEPSTATE_PROC_AWAKE_BIT);

	ss->ws = wakeup_source_register(dev, "smp2p-sleepstate");
	if (!ss->ws)
		return -ENOMEM;

	/* The inbound direction is optional. */
	irq = platform_get_irq(pdev, 0);
	if (irq > 0) {
		ret = devm_request_threaded_irq(dev, irq, NULL,
						smp2p_sleepstate_handler,
						IRQF_ONESHOT | IRQF_TRIGGER_RISING,
						"smp2p_sleepstate", ss);
		if (ret) {
			wakeup_source_unregister(ss->ws);
			return dev_err_probe(dev, ret, "cannot request irq\n");
		}
		enable_irq_wake(irq);
	}

	ss->nb.notifier_call = sleepstate_pm_notifier;
	ss->nb.priority = INT_MAX;
	ret = register_pm_notifier(&ss->nb);
	if (ret) {
		wakeup_source_unregister(ss->ws);
		return dev_err_probe(dev, ret, "cannot register pm notifier\n");
	}

	platform_set_drvdata(pdev, ss);
	return 0;
}

static void smp2p_sleepstate_remove(struct platform_device *pdev)
{
	struct smp2p_sleepstate *ss = platform_get_drvdata(pdev);

	unregister_pm_notifier(&ss->nb);
	wakeup_source_unregister(ss->ws);
}

static const struct of_device_id smp2p_sleepstate_of_match[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{ }
};
MODULE_DEVICE_TABLE(of, smp2p_sleepstate_of_match);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.remove = smp2p_sleepstate_remove,
	.driver = {
		.name = "smp2p-sleepstate",
		.of_match_table = smp2p_sleepstate_of_match,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("Qualcomm SMP2P sleep state notifier");
MODULE_LICENSE("GPL");
