// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power-loss breadcrumbs in PMIC SDAM scratch memory.
 *
 * The SDAM contents survive everything short of battery removal,
 * including a cold power-off that loses DRAM (and with it ramoops).
 * A small lifecycle record is kept up to date while the system runs;
 * whatever is found there at the next boot tells how far the previous
 * kernel got before power was lost.
 *
 * Layout inside the SDAM (nvmem offsets):
 *   0xB0  lifecycle record: last of BOOT/SUSPEND/RESUME/SHUTDOWN/REBOOT
 *   0xA0  sticky record, written only from the thermal-critical path
 *         and cleared only after being reported at boot
 *
 * The nvmem window this driver can address is 0x40..0xBF (the SDAM
 * driver folds the 0x40 hardware offset into the nvmem address); the
 * vendor fuel gauge uses 0x80..0x92 of the same SDAM, and both records
 * deliberately sit above that.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/suspend.h>
#include <linux/timekeeping.h>
#include <soc/qcom/apollo_breadcrumb.h>

#define CRUMB_LIFECYCLE_OFF	0xb0
#define CRUMB_THERMAL_OFF	0xa0
#define CRUMB_MAGIC		0x42

#define CRUMB_BOOT		0x01
#define CRUMB_SUSPEND		0x02
#define CRUMB_RESUME		0x03
#define CRUMB_SHUTDOWN		0x04
#define CRUMB_REBOOT		0x05
#define CRUMB_THERMAL		0x07

struct crumb_record {
	u8 magic;
	u8 event;
	__le16 seq;
	__le32 rtc_sec;
	u8 detail0;
	__le16 detail1;
} __packed;

struct crumb_chip {
	struct device *dev;
	struct nvmem_device *nvmem;
	struct mutex lock;
	u16 seq;
	struct notifier_block pm_nb;
	struct notifier_block reboot_nb;
};

static struct crumb_chip *crumb;

static const char *crumb_event_name(u8 event)
{
	switch (event) {
	case CRUMB_BOOT:	return "boot";
	case CRUMB_SUSPEND:	return "suspend-entry";
	case CRUMB_RESUME:	return "resume";
	case CRUMB_SHUTDOWN:	return "orderly-poweroff";
	case CRUMB_REBOOT:	return "orderly-reboot";
	case CRUMB_THERMAL:	return "thermal-critical";
	default:		return "unknown";
	}
}

static void crumb_write(struct crumb_chip *chip, unsigned int off, u8 event,
			u8 detail0, u16 detail1)
{
	struct crumb_record rec = {
		.magic = CRUMB_MAGIC,
		.event = event,
		.detail0 = detail0,
		.detail1 = cpu_to_le16(detail1),
	};
	int rc;

	mutex_lock(&chip->lock);
	rec.seq = cpu_to_le16(++chip->seq);
	rec.rtc_sec = cpu_to_le32((u32)ktime_get_real_seconds());
	rc = nvmem_device_write(chip->nvmem, off, sizeof(rec), &rec);
	mutex_unlock(&chip->lock);

	if (rc < 0)
		dev_warn(chip->dev, "crumb %#x write failed: %d\n", event, rc);
}

void apollo_breadcrumb_thermal(int tz_id, int temp_mC)
{
	struct crumb_chip *chip = READ_ONCE(crumb);

	if (!chip)
		return;

	/* temperature in deci-degrees so it fits the 16-bit detail */
	crumb_write(chip, CRUMB_THERMAL_OFF, CRUMB_THERMAL,
		    (u8)tz_id, (u16)(temp_mC / 100));
	crumb_write(chip, CRUMB_LIFECYCLE_OFF, CRUMB_THERMAL,
		    (u8)tz_id, (u16)(temp_mC / 100));
}
EXPORT_SYMBOL_GPL(apollo_breadcrumb_thermal);

static int crumb_pm_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct crumb_chip *chip = container_of(nb, struct crumb_chip, pm_nb);

	switch (action) {
	case PM_SUSPEND_PREPARE:
		crumb_write(chip, CRUMB_LIFECYCLE_OFF, CRUMB_SUSPEND, 0, 0);
		break;
	case PM_POST_SUSPEND:
		crumb_write(chip, CRUMB_LIFECYCLE_OFF, CRUMB_RESUME, 0, 0);
		break;
	}
	return NOTIFY_DONE;
}

static int crumb_reboot_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct crumb_chip *chip = container_of(nb, struct crumb_chip, reboot_nb);

	crumb_write(chip, CRUMB_LIFECYCLE_OFF,
		    action == SYS_POWER_OFF ? CRUMB_SHUTDOWN : CRUMB_REBOOT,
		    0, 0);
	return NOTIFY_DONE;
}

static void crumb_report(struct crumb_chip *chip, unsigned int off,
			 const char *what)
{
	struct crumb_record rec;
	int rc;

	rc = nvmem_device_read(chip->nvmem, off, sizeof(rec), &rec);
	if (rc < 0) {
		dev_warn(chip->dev, "%s record read failed: %d\n", what, rc);
		return;
	}

	if (rec.magic != CRUMB_MAGIC) {
		dev_info(chip->dev, "%s record: none (magic %#x)\n",
			 what, rec.magic);
		return;
	}

	dev_info(chip->dev,
		 "%s record: %s seq=%u rtc=%u detail=%u/%u\n",
		 what, crumb_event_name(rec.event), le16_to_cpu(rec.seq),
		 le32_to_cpu(rec.rtc_sec), rec.detail0,
		 le16_to_cpu(rec.detail1));

	chip->seq = le16_to_cpu(rec.seq);
}

static int crumb_probe(struct platform_device *pdev)
{
	struct crumb_chip *chip;
	u8 zero = 0;
	int rc;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	mutex_init(&chip->lock);

	chip->nvmem = devm_nvmem_device_get(&pdev->dev, "sdam");
	if (IS_ERR(chip->nvmem))
		return dev_err_probe(&pdev->dev, PTR_ERR(chip->nvmem),
				     "no sdam nvmem\n");

	crumb_report(chip, CRUMB_LIFECYCLE_OFF, "last-life");
	crumb_report(chip, CRUMB_THERMAL_OFF, "sticky-thermal");

	/* evidence delivered to the log; rearm the sticky record */
	nvmem_device_write(chip->nvmem, CRUMB_THERMAL_OFF, 1, &zero);

	chip->pm_nb.notifier_call = crumb_pm_notify;
	rc = register_pm_notifier(&chip->pm_nb);
	if (rc)
		return rc;

	chip->reboot_nb.notifier_call = crumb_reboot_notify;
	rc = register_reboot_notifier(&chip->reboot_nb);
	if (rc) {
		unregister_pm_notifier(&chip->pm_nb);
		return rc;
	}

	platform_set_drvdata(pdev, chip);
	WRITE_ONCE(crumb, chip);

	crumb_write(chip, CRUMB_LIFECYCLE_OFF, CRUMB_BOOT, 0, 0);

	return 0;
}

static void crumb_remove(struct platform_device *pdev)
{
	struct crumb_chip *chip = platform_get_drvdata(pdev);

	WRITE_ONCE(crumb, NULL);
	unregister_reboot_notifier(&chip->reboot_nb);
	unregister_pm_notifier(&chip->pm_nb);
}

static const struct of_device_id crumb_match_table[] = {
	{ .compatible = "xiaomi,apollo-breadcrumb" },
	{ }
};
MODULE_DEVICE_TABLE(of, crumb_match_table);

static struct platform_driver crumb_driver = {
	.probe = crumb_probe,
	.remove = crumb_remove,
	.driver = {
		.name = "apollo-breadcrumb",
		.of_match_table = crumb_match_table,
	},
};
module_platform_driver(crumb_driver);

MODULE_DESCRIPTION("PMIC SDAM power-loss breadcrumbs");
MODULE_LICENSE("GPL");
