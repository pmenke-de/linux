// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8697 haptic driver
 *
 * Copyright (c) 2026 Philipp Menke <pmenke@pmenke.de>
 *
 * The chip drives a linear resonant actuator from a waveform it keeps in its
 * own SRAM. This driver stores a single sine period there and loops it for as
 * long as the rumble effect lasts; the magnitude becomes the output gain.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define AW8697_ID_REG				0x00
#define AW8697_ID				0x97
/* The ID register doubles as the software reset: this value restarts the chip */
#define AW8697_ID_SOFTRST			0xaa

#define AW8697_SYSINT_REG			0x02
#define AW8697_SYSINT_BSTERRI			BIT(7)
#define AW8697_SYSINT_OVI			BIT(6)
#define AW8697_SYSINT_UVLI			BIT(5)
#define AW8697_SYSINT_FF_AEI			BIT(4)
#define AW8697_SYSINT_FF_AFI			BIT(3)
#define AW8697_SYSINT_OCDI			BIT(2)
#define AW8697_SYSINT_OTI			BIT(1)
#define AW8697_SYSINT_DONEI			BIT(0)

/*
 * Interrupt masks: a set bit silences that source. The boost converter
 * reports an error on every cycle it spends idle, which is most of them, so
 * it stays silenced; the undervoltage lockout is only meaningful while the
 * converter runs and is silenced in standby (aw8697_standby).
 */
#define AW8697_SYSINTM_REG			0x03
#define AW8697_SYSINTM_BSTERRM			BIT(7)
#define AW8697_SYSINTM_OVM			BIT(6)
#define AW8697_SYSINTM_UVLOM			BIT(5)
#define AW8697_SYSINTM_FF_AEM			BIT(4)
#define AW8697_SYSINTM_FF_AFM			BIT(3)
#define AW8697_SYSINTM_OCDM			BIT(2)
#define AW8697_SYSINTM_OTM			BIT(1)
#define AW8697_SYSINTM_DONEM			BIT(0)

#define AW8697_SYSCTRL_REG			0x04
#define AW8697_SYSCTRL_RAMINIT_MASK		GENMASK(5, 5)
#define AW8697_SYSCTRL_RAMINIT_ON		1
#define AW8697_SYSCTRL_RAMINIT_OFF		0
#define AW8697_SYSCTRL_PLAY_MODE_MASK		GENMASK(3, 2)
#define AW8697_SYSCTRL_PLAY_MODE_RAM		0
#define AW8697_SYSCTRL_BST_MODE_MASK		GENMASK(1, 1)
#define AW8697_SYSCTRL_BST_MODE_BOOST		1
#define AW8697_SYSCTRL_WORK_MODE_MASK		GENMASK(0, 0)
#define AW8697_SYSCTRL_WORK_MODE_STANDBY	1
#define AW8697_SYSCTRL_WORK_MODE_ACTIVE		0

#define AW8697_GO_REG				0x05
#define AW8697_GO_GO				BIT(0)

#define AW8697_WAVSEQ1_REG			0x07
#define AW8697_WAVSEQ1_WAV_MASK			GENMASK(6, 0)

#define AW8697_WAVLOOP1_REG			0x0f
#define AW8697_WAVLOOP1_SEQ1_MASK		GENMASK(7, 4)
#define AW8697_WAVLOOP1_SEQ1_INFINITELY		15

#define AW8697_DBGCTRL_REG			0x20
#define AW8697_DBGCTRL_INT_MODE_MASK		GENMASK(2, 2)
#define AW8697_DBGCTRL_INT_MODE_EDGE		1

#define AW8697_BASE_ADDRH_REG			0x21
#define AW8697_BASE_ADDRL_REG			0x22
#define AW8697_FIFO_AEH_REG			0x23
#define AW8697_FIFO_AEL_REG			0x24
#define AW8697_FIFO_AFH_REG			0x25
#define AW8697_FIFO_AFL_REG			0x26

#define AW8697_PWMDBG_REG			0x2e
#define AW8697_PWMDBG_PWM_MODE_MASK		GENMASK(6, 5)
#define AW8697_PWMDBG_PWM_24K			2

#define AW8697_BSTDBG1_REG			0x31
#define AW8697_BSTDBG2_REG			0x32
#define AW8697_BSTDBG3_REG			0x33

#define AW8697_ANADBG_REG			0x35
#define AW8697_ANADBG_IOC_MASK			GENMASK(3, 2)
#define AW8697_ANADBG_IOC_4P65A			3

/* Output gain, 0x80 being the nominal full scale of the driver stage */
#define AW8697_DATDBG_REG			0x39
#define AW8697_DATDBG_GAIN_FULL			0x80

#define AW8697_BSTDBG4_REG			0x3a
#define AW8697_BSTDBG4_BSTVOL_MASK		GENMASK(5, 1)
#define AW8697_BSTDBG4_BSTVOL_DEFAULT		0x10

#define AW8697_RAMADDRH_REG			0x40
#define AW8697_RAMADDRL_REG			0x41
#define AW8697_RAMDATA_REG			0x42

#define AW8697_GLB_STATE_REG			0x46
#define AW8697_GLB_STATE_MASK			GENMASK(3, 0)
#define AW8697_GLB_STATE_STANDBY		0

#define AW8697_TSET_REG				0x4d
#define AW8697_TSET_DEFAULT			0x12

#define AW8697_R_SPARE_REG			0x5d
#define AW8697_R_SPARE_DEFAULT			0x68

/*
 * The 8 KiB of SRAM is split at this address: the RTP FIFO lives below it,
 * the waveforms above. Nothing here plays from the FIFO, so the split only
 * has to leave room for the waveform.
 */
#define AW8697_RAM_BASE_ADDR			0x0800

/* The waveform the sequencer plays, numbered from one */
#define AW8697_WAVEFORM_SINE			1

/*
 * One period of a sine, in two's complement, as the chip reads it at the
 * 24 kHz the PWM register is set to:
 *   round(84 * sin(2 * pi * i / 141))
 * 141 samples put the period at 170.2 Hz, which is where the actuators these
 * chips are paired with resonate. An actuator with a markedly different
 * resonance needs a table of its own length.
 */
static const u8 aw8697_waveform[] = {
	0x00, 0x04, 0x07, 0x0b, 0x0f, 0x13, 0x16, 0x1a, 0x1d, 0x21, 0x24, 0x28,
	0x2b, 0x2e, 0x31, 0x34, 0x37, 0x3a, 0x3c, 0x3f, 0x41, 0x44, 0x46, 0x48,
	0x4a, 0x4b, 0x4d, 0x4e, 0x50, 0x51, 0x52, 0x52, 0x53, 0x54, 0x54, 0x54,
	0x54, 0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4c, 0x4b, 0x49,
	0x47, 0x45, 0x42, 0x40, 0x3e, 0x3b, 0x38, 0x36, 0x33, 0x30, 0x2c, 0x29,
	0x26, 0x23, 0x1f, 0x1c, 0x18, 0x14, 0x11, 0x0d, 0x09, 0x06, 0x02, 0xfe,
	0xfa, 0xf7, 0xf3, 0xef, 0xec, 0xe8, 0xe4, 0xe1, 0xdd, 0xda, 0xd7, 0xd4,
	0xd0, 0xcd, 0xca, 0xc8, 0xc5, 0xc2, 0xc0, 0xbe, 0xbb, 0xb9, 0xb7, 0xb5,
	0xb4, 0xb2, 0xb1, 0xb0, 0xaf, 0xae, 0xad, 0xad, 0xac, 0xac, 0xac, 0xac,
	0xac, 0xad, 0xae, 0xae, 0xaf, 0xb0, 0xb2, 0xb3, 0xb5, 0xb6, 0xb8, 0xba,
	0xbc, 0xbf, 0xc1, 0xc4, 0xc6, 0xc9, 0xcc, 0xcf, 0xd2, 0xd5, 0xd8, 0xdc,
	0xdf, 0xe3, 0xe6, 0xea, 0xed, 0xf1, 0xf5, 0xf9, 0xfc
};

/*
 * What the sequencer finds at the base address: a version byte and, for each
 * waveform in turn, the first and last address of its samples.
 */
struct aw8697_sram_waveform_header {
	u8 version;
	__be16 start_address;
	__be16 end_address;
} __packed;

static const struct aw8697_sram_waveform_header sram_waveform_header = {
	.version = 0x01,
	.start_address = cpu_to_be16(AW8697_RAM_BASE_ADDR +
			sizeof(struct aw8697_sram_waveform_header)),
	.end_address = cpu_to_be16(AW8697_RAM_BASE_ADDR +
			sizeof(struct aw8697_sram_waveform_header) +
			ARRAY_SIZE(aw8697_waveform) - 1),
};

struct aw8697_data {
	struct work_struct play_work;
	struct device *dev;
	struct input_dev *input_dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	u16 level;
};

static const struct regmap_config aw8697_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
	.max_register = 0x7f,
};

static int aw8697_wait_enter_standby(struct aw8697_data *haptics)
{
	unsigned int reg_val;
	int err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8697_GLB_STATE_REG, reg_val,
				       (FIELD_GET(AW8697_GLB_STATE_MASK, reg_val) ==
						AW8697_GLB_STATE_STANDBY),
				       2000, 2000 * 100);
	if (err)
		dev_err(haptics->dev, "did not enter standby: %d\n", err);

	return err;
}

static int aw8697_standby(struct aw8697_data *haptics)
{
	int err;

	err = regmap_set_bits(haptics->regmap, AW8697_SYSINTM_REG, AW8697_SYSINTM_UVLOM);
	if (err)
		return err;

	return regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				  AW8697_SYSCTRL_WORK_MODE_MASK,
				  FIELD_PREP(AW8697_SYSCTRL_WORK_MODE_MASK,
					     AW8697_SYSCTRL_WORK_MODE_STANDBY));
}

static int aw8697_stop(struct aw8697_data *haptics)
{
	int err;

	err = regmap_write(haptics->regmap, AW8697_GO_REG, 0);
	if (err) {
		dev_err(haptics->dev, "Failed to stop playback: %d\n", err);
		return err;
	}

	/* Playback ends on the current period, so give the chip that long */
	err = aw8697_wait_enter_standby(haptics);
	if (err)
		dev_err(haptics->dev, "Failed to enter standby, forcing it\n");

	return aw8697_standby(haptics);
}

static int aw8697_play_sine(struct aw8697_data *haptics)
{
	unsigned int sysint;
	int err;

	err = regmap_write(haptics->regmap, AW8697_DATDBG_REG,
			   haptics->level * AW8697_DATDBG_GAIN_FULL / 0xffff);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_WAVSEQ1_REG,
				 AW8697_WAVSEQ1_WAV_MASK,
				 FIELD_PREP(AW8697_WAVSEQ1_WAV_MASK,
					    AW8697_WAVEFORM_SINE));
	if (err)
		return err;

	/* The effect lasts until the rumble is turned off, so loop the period */
	err = regmap_update_bits(haptics->regmap, AW8697_WAVLOOP1_REG,
				 AW8697_WAVLOOP1_SEQ1_MASK,
				 FIELD_PREP(AW8697_WAVLOOP1_SEQ1_MASK,
					    AW8697_WAVLOOP1_SEQ1_INFINITELY));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				 AW8697_SYSCTRL_PLAY_MODE_MASK |
					AW8697_SYSCTRL_BST_MODE_MASK,
				 FIELD_PREP(AW8697_SYSCTRL_PLAY_MODE_MASK,
					    AW8697_SYSCTRL_PLAY_MODE_RAM) |
					FIELD_PREP(AW8697_SYSCTRL_BST_MODE_MASK,
						   AW8697_SYSCTRL_BST_MODE_BOOST));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				 AW8697_SYSCTRL_WORK_MODE_MASK,
				 FIELD_PREP(AW8697_SYSCTRL_WORK_MODE_MASK,
					    AW8697_SYSCTRL_WORK_MODE_ACTIVE));
	if (err)
		return err;

	/*
	 * The boost converter comes up with the chip, and reports the
	 * undervoltage it starts from. Reading the register drops that, so
	 * only a dip under load is left to report.
	 */
	err = regmap_read(haptics->regmap, AW8697_SYSINT_REG, &sysint);
	if (err)
		return err;

	err = regmap_clear_bits(haptics->regmap, AW8697_SYSINTM_REG, AW8697_SYSINTM_UVLOM);
	if (err)
		return err;

	return regmap_write(haptics->regmap, AW8697_GO_REG, AW8697_GO_GO);
}

static void aw8697_haptics_play_work(struct work_struct *work)
{
	struct aw8697_data *haptics =
		container_of(work, struct aw8697_data, play_work);
	int err;

	if (haptics->level)
		err = aw8697_play_sine(haptics);
	else
		err = aw8697_stop(haptics);

	if (err)
		dev_err(haptics->dev, "Failed to execute work command: %d\n", err);
}

static int aw8697_haptics_play(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct aw8697_data *haptics = input_get_drvdata(dev);
	int level;

	level = effect->u.rumble.strong_magnitude;
	if (!level)
		level = effect->u.rumble.weak_magnitude;

	/* If level does not change, don't restart playback */
	if (haptics->level == level)
		return 0;

	haptics->level = level;

	schedule_work(&haptics->play_work);

	return 0;
}

static void aw8697_close(struct input_dev *input)
{
	struct aw8697_data *haptics = input_get_drvdata(input);
	int err;

	cancel_work_sync(&haptics->play_work);

	err = aw8697_stop(haptics);
	if (err)
		dev_err(haptics->dev, "Failed to close the driver: %d\n", err);
}

static void aw8697_hw_reset(struct aw8697_data *haptics)
{
	/* Assert reset */
	gpiod_set_value_cansleep(haptics->reset_gpio, 1);
	/* Wait ~1ms */
	usleep_range(1000, 2000);
	/* Deassert reset */
	gpiod_set_value_cansleep(haptics->reset_gpio, 0);
	/* Wait ~2ms until I2C is accessible */
	usleep_range(2000, 2500);
}

static int aw8697_haptic_init(struct aw8697_data *haptics)
{
	int err;

	/* Sample rate of both the waveform in SRAM and the RTP FIFO */
	err = regmap_update_bits(haptics->regmap, AW8697_PWMDBG_REG,
				 AW8697_PWMDBG_PWM_MODE_MASK,
				 FIELD_PREP(AW8697_PWMDBG_PWM_MODE_MASK,
					    AW8697_PWMDBG_PWM_24K));
	if (err)
		return err;

	/* Boost converter: compensation, output voltage and current limit */
	err = regmap_write(haptics->regmap, AW8697_BSTDBG1_REG, 0x30);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BSTDBG2_REG, 0xeb);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BSTDBG3_REG, 0xd4);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_BSTDBG4_REG,
				 AW8697_BSTDBG4_BSTVOL_MASK,
				 FIELD_PREP(AW8697_BSTDBG4_BSTVOL_MASK,
					    AW8697_BSTDBG4_BSTVOL_DEFAULT));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_ANADBG_REG,
				 AW8697_ANADBG_IOC_MASK,
				 FIELD_PREP(AW8697_ANADBG_IOC_MASK,
					    AW8697_ANADBG_IOC_4P65A));
	if (err)
		return err;

	/* Driver stage timing and trim, at the values the chip is specified for */
	err = regmap_write(haptics->regmap, AW8697_TSET_REG, AW8697_TSET_DEFAULT);
	if (err)
		return err;

	return regmap_write(haptics->regmap, AW8697_R_SPARE_REG, AW8697_R_SPARE_DEFAULT);
}

static int aw8697_ram_init(struct aw8697_data *haptics)
{
	int err;

	err = aw8697_wait_enter_standby(haptics);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				 AW8697_SYSCTRL_RAMINIT_MASK,
				 FIELD_PREP(AW8697_SYSCTRL_RAMINIT_MASK,
					    AW8697_SYSCTRL_RAMINIT_ON));
	if (err)
		return err;

	/* Where the waveforms begin, and with it how large the RTP FIFO is */
	err = regmap_write(haptics->regmap, AW8697_BASE_ADDRH_REG,
			   AW8697_RAM_BASE_ADDR >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BASE_ADDRL_REG,
			   AW8697_RAM_BASE_ADDR & 0xff);
	if (err)
		return err;

	/* The FIFO's watermarks, a quarter in from either end of it */
	err = regmap_write(haptics->regmap, AW8697_FIFO_AEH_REG,
			   (AW8697_RAM_BASE_ADDR >> 2) >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_FIFO_AEL_REG,
			   (AW8697_RAM_BASE_ADDR >> 2) & 0xff);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_FIFO_AFH_REG,
			   (AW8697_RAM_BASE_ADDR - (AW8697_RAM_BASE_ADDR >> 2)) >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_FIFO_AFL_REG,
			   (AW8697_RAM_BASE_ADDR - (AW8697_RAM_BASE_ADDR >> 2)) & 0xff);
	if (err)
		return err;

	/* Writes to the data register walk on from here */
	err = regmap_write(haptics->regmap, AW8697_RAMADDRH_REG,
			   AW8697_RAM_BASE_ADDR >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_RAMADDRL_REG,
			   AW8697_RAM_BASE_ADDR & 0xff);
	if (err)
		return err;

	err = regmap_noinc_write(haptics->regmap, AW8697_RAMDATA_REG,
				 &sram_waveform_header, sizeof(sram_waveform_header));
	if (err)
		return err;

	err = regmap_noinc_write(haptics->regmap, AW8697_RAMDATA_REG,
				 aw8697_waveform, ARRAY_SIZE(aw8697_waveform));
	if (err)
		return err;

	return regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				  AW8697_SYSCTRL_RAMINIT_MASK,
				  FIELD_PREP(AW8697_SYSCTRL_RAMINIT_MASK,
					     AW8697_SYSCTRL_RAMINIT_OFF));
}

static irqreturn_t aw8697_irq(int irq, void *data)
{
	struct aw8697_data *haptics = data;
	struct device *dev = haptics->dev;
	unsigned int reg_val;
	int err;

	/* Reading the register clears it */
	err = regmap_read(haptics->regmap, AW8697_SYSINT_REG, &reg_val);
	if (err) {
		dev_err(dev, "Failed to read SYSINT register: %d\n", err);
		return IRQ_NONE;
	}

	if (reg_val & AW8697_SYSINT_BSTERRI)
		dev_err(dev, "Received a boost converter error interrupt\n");
	if (reg_val & AW8697_SYSINT_OVI)
		dev_err(dev, "Received an Over Voltage Protection interrupt\n");
	if (reg_val & AW8697_SYSINT_UVLI)
		dev_err(dev, "Received an Under Voltage Lock Out interrupt\n");
	if (reg_val & AW8697_SYSINT_OCDI)
		dev_err(dev, "Received an Over Current interrupt\n");
	if (reg_val & AW8697_SYSINT_OTI)
		dev_err(dev, "Received an Over Temperature interrupt\n");

	if (reg_val & AW8697_SYSINT_DONEI)
		dev_dbg(dev, "Chip playback done!\n");
	if (reg_val & AW8697_SYSINT_FF_AFI)
		dev_dbg(dev, "The RTP mode FIFO is almost full!\n");
	if (reg_val & AW8697_SYSINT_FF_AEI)
		dev_dbg(dev, "The RTP mode FIFO is almost empty!\n");

	return IRQ_HANDLED;
}

static int aw8697_detect(struct aw8697_data *haptics)
{
	unsigned int chip_id;
	int err;

	err = regmap_read(haptics->regmap, AW8697_ID_REG, &chip_id);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to read ID register\n");

	if (chip_id != AW8697_ID) {
		dev_err(haptics->dev, "Unexpected ID value 0x%x\n", chip_id);
		return -ENODEV;
	}

	return 0;
}

static int aw8697_probe(struct i2c_client *client)
{
	struct aw8697_data *haptics;
	int err;

	haptics = devm_kzalloc(&client->dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->dev = &client->dev;
	haptics->client = client;

	i2c_set_clientdata(client, haptics);

	haptics->regmap = devm_regmap_init_i2c(client, &aw8697_regmap_config);
	if (IS_ERR(haptics->regmap))
		return dev_err_probe(haptics->dev, PTR_ERR(haptics->regmap),
				     "Failed to allocate register map\n");

	haptics->input_dev = devm_input_allocate_device(haptics->dev);
	if (!haptics->input_dev)
		return -ENOMEM;

	haptics->reset_gpio = devm_gpiod_get(haptics->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(haptics->reset_gpio))
		return dev_err_probe(haptics->dev, PTR_ERR(haptics->reset_gpio),
				     "Failed to get reset gpio\n");

	aw8697_hw_reset(haptics);

	err = regmap_write(haptics->regmap, AW8697_ID_REG, AW8697_ID_SOFTRST);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed software reset\n");

	/* Wait ~2ms until I2C is accessible */
	usleep_range(2000, 2500);

	err = aw8697_detect(haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to find chip\n");

	err = regmap_update_bits(haptics->regmap, AW8697_DBGCTRL_REG,
				 AW8697_DBGCTRL_INT_MODE_MASK,
				 FIELD_PREP(AW8697_DBGCTRL_INT_MODE_MASK,
					    AW8697_DBGCTRL_INT_MODE_EDGE));
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to configure interrupt mode\n");

	/* Report the faults that can damage the actuator, and nothing else */
	err = regmap_write(haptics->regmap, AW8697_SYSINTM_REG,
			   AW8697_SYSINTM_BSTERRM | AW8697_SYSINTM_UVLOM |
				AW8697_SYSINTM_FF_AEM | AW8697_SYSINTM_FF_AFM |
				AW8697_SYSINTM_DONEM);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to configure interrupt masks\n");

	err = devm_request_threaded_irq(haptics->dev, client->irq, NULL,
					aw8697_irq, IRQF_ONESHOT, NULL, haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to request threaded irq\n");

	INIT_WORK(&haptics->play_work, aw8697_haptics_play_work);

	haptics->input_dev->name = "aw8697-haptics";
	haptics->input_dev->close = aw8697_close;

	input_set_drvdata(haptics->input_dev, haptics);
	input_set_capability(haptics->input_dev, EV_FF, FF_RUMBLE);

	err = input_ff_create_memless(haptics->input_dev, NULL, aw8697_haptics_play);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to create FF dev\n");

	err = aw8697_standby(haptics);
	if (err)
		return dev_err_probe(haptics->dev, err,
				     "Failed to enter standby for haptic init\n");

	err = aw8697_haptic_init(haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Haptic init failed\n");

	err = aw8697_ram_init(haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to init aw8697 sram\n");

	err = input_register_device(haptics->input_dev);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to register input device\n");

	return 0;
}

static const struct of_device_id aw8697_of_id[] = {
	{ .compatible = "awinic,aw8697" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, aw8697_of_id);

static struct i2c_driver aw8697_driver = {
	.driver = {
		.name = "aw8697-haptics",
		.of_match_table = aw8697_of_id,
	},
	.probe = aw8697_probe,
};

module_i2c_driver(aw8697_driver);

MODULE_AUTHOR("Philipp Menke <pmenke@pmenke.de>");
MODULE_DESCRIPTION("AWINIC AW8697 LRA Haptic Driver");
MODULE_LICENSE("GPL");
