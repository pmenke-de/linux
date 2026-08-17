// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX682 64 Mpixel camera sensor driver
 *
 * Copyright (c) 2026 Roy Kaandorp
 *
 * The register sequences below were recovered from the vendor camera stack
 * with tools/remoteproc/qti_sensormodule.py; see that file for the format.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define IMX682_MCLK_FREQ		(19200 * HZ_PER_KHZ)
#define IMX682_DATA_LANES		3
#define IMX682_BITS_PER_SAMPLE		10

/* Register map follows MIPI CCS, as on the other Samsung parts. */
#define IMX682_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX682_CHIP_ID			0x0682

#define IMX682_REG_CTRL_MODE		CCI_REG8(0x0100)
#define IMX682_MODE_STREAMING		BIT(0)

#define IMX682_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX682_HFLIP			BIT(0)
#define IMX682_VFLIP			BIT(1)

#define IMX682_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX682_EXPOSURE_MIN		8
#define IMX682_EXPOSURE_STEP		1
/*
 * Exposure is expressed in lines and has to stay below the frame length, with
 * room for the sensor's own readout overhead.
 */
#define IMX682_EXPOSURE_MARGIN		48

#define IMX682_REG_AGAIN		CCI_REG16(0x0204)
#define IMX682_AGAIN_MIN		0
#define IMX682_AGAIN_MAX		1008
#define IMX682_AGAIN_STEP		1
#define IMX682_AGAIN_DEFAULT		0

#define IMX682_REG_VTS			CCI_REG16(0x0340)
#define IMX682_VTS_MAX			0xfffc

#define IMX682_REG_HTS			CCI_REG16(0x0342)

#define IMX682_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define to_imx682(_sd)			container_of(_sd, struct imx682, sd)

/*
 * The two implemented modes run the link at different rates, so the mode
 * carries an index into this menu rather than there being a single frequency.
 */
enum {
	IMX682_LINK_FREQ_998MHZ,
};

static const s64 imx682_link_freq_menu[] = {
	[IMX682_LINK_FREQ_998MHZ] = 998400000,
};

/* Ordered so the flip controls can pick a code by index. */
static const u32 imx682_mbus_formats[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
};

static const char * const imx682_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Color bars",
	"Fade to grey color bars",
	"PN9",
};

struct imx682_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct imx682_mode {
	u32 width;
	u32 height;
	u32 hts;			/* Line length in pixels */
	u32 vts;			/* Default frame length in lines */
	u32 link_freq_idx;
	struct imx682_reg_list reg_list;
};

struct imx682 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator *avdd;		/* Analog */
	struct regulator *dvdd;		/* Digital core */
	struct regulator *dovdd;	/* Digital I/O */

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	const struct imx682_mode *mode;
};

/* Global setup. */
static const struct cci_reg_sequence imx682_init_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x33f0), 0x02 },
	{ CCI_REG8(0x33f1), 0x03 },
	{ CCI_REG8(0x0111), 0x03 },
	{ CCI_REG8(0x3076), 0x00 },
	{ CCI_REG8(0x3077), 0x30 },
	{ CCI_REG8(0x1f06), 0x06 },
	{ CCI_REG8(0x1f07), 0x82 },
	{ CCI_REG8(0x1f04), 0x71 },
	{ CCI_REG8(0x1f05), 0x01 },
	{ CCI_REG8(0x1f08), 0x01 },
	{ CCI_REG8(0x5bfe), 0x14 },
	{ CCI_REG8(0x5c0d), 0x2d },
	{ CCI_REG8(0x5c1c), 0x30 },
	{ CCI_REG8(0x5c2b), 0x32 },
	{ CCI_REG8(0x5c37), 0x2e },
	{ CCI_REG8(0x5c40), 0x30 },
	{ CCI_REG8(0x5c50), 0x14 },
	{ CCI_REG8(0x5c5f), 0x28 },
	{ CCI_REG8(0x5c6e), 0x28 },
	{ CCI_REG8(0x5c7d), 0x32 },
	{ CCI_REG8(0x5c89), 0x37 },
	{ CCI_REG8(0x5c92), 0x56 },
	{ CCI_REG8(0x5bfc), 0x12 },
	{ CCI_REG8(0x5c0b), 0x2a },
	{ CCI_REG8(0x5c1a), 0x2c },
	{ CCI_REG8(0x5c29), 0x2f },
	{ CCI_REG8(0x5c36), 0x2e },
	{ CCI_REG8(0x5c3f), 0x2e },
	{ CCI_REG8(0x5c4e), 0x06 },
	{ CCI_REG8(0x5c5d), 0x1e },
	{ CCI_REG8(0x5c6c), 0x20 },
	{ CCI_REG8(0x5c7b), 0x1e },
	{ CCI_REG8(0x5c88), 0x32 },
	{ CCI_REG8(0x5c91), 0x32 },
	{ CCI_REG8(0x5c02), 0x14 },
	{ CCI_REG8(0x5c11), 0x2f },
	{ CCI_REG8(0x5c20), 0x32 },
	{ CCI_REG8(0x5c2f), 0x34 },
	{ CCI_REG8(0x5c39), 0x31 },
	{ CCI_REG8(0x5c42), 0x31 },
	{ CCI_REG8(0x5c8b), 0x28 },
	{ CCI_REG8(0x5c94), 0x28 },
	{ CCI_REG8(0x5c00), 0x10 },
	{ CCI_REG8(0x5c0f), 0x2c },
	{ CCI_REG8(0x5c1e), 0x2e },
	{ CCI_REG8(0x5c2d), 0x32 },
	{ CCI_REG8(0x5c38), 0x2e },
	{ CCI_REG8(0x5c41), 0x2b },
	{ CCI_REG8(0x5c61), 0x0a },
	{ CCI_REG8(0x5c70), 0x0a },
	{ CCI_REG8(0x5c7f), 0x0a },
	{ CCI_REG8(0x5c8a), 0x1e },
	{ CCI_REG8(0x5c93), 0x2a },
	{ CCI_REG8(0x5bfa), 0x2b },
	{ CCI_REG8(0x5c09), 0x2d },
	{ CCI_REG8(0x5c18), 0x2e },
	{ CCI_REG8(0x5c27), 0x30 },
	{ CCI_REG8(0x5c5b), 0x28 },
	{ CCI_REG8(0x5c6a), 0x22 },
	{ CCI_REG8(0x5c79), 0x42 },
	{ CCI_REG8(0x5bfb), 0x2c },
	{ CCI_REG8(0x5c0a), 0x2f },
	{ CCI_REG8(0x5c19), 0x2e },
	{ CCI_REG8(0x5c28), 0x2e },
	{ CCI_REG8(0x5c4d), 0x20 },
	{ CCI_REG8(0x5c5c), 0x1e },
	{ CCI_REG8(0x5c6b), 0x32 },
	{ CCI_REG8(0x5c7a), 0x32 },
	{ CCI_REG8(0x5bfd), 0x30 },
	{ CCI_REG8(0x5c0c), 0x32 },
	{ CCI_REG8(0x5c1b), 0x2e },
	{ CCI_REG8(0x5c2a), 0x30 },
	{ CCI_REG8(0x5c4f), 0x28 },
	{ CCI_REG8(0x5c5e), 0x32 },
	{ CCI_REG8(0x5c6d), 0x37 },
	{ CCI_REG8(0x5c7c), 0x56 },
	{ CCI_REG8(0x5bff), 0x2e },
	{ CCI_REG8(0x5c0e), 0x32 },
	{ CCI_REG8(0x5c1d), 0x2e },
	{ CCI_REG8(0x5c2c), 0x2b },
	{ CCI_REG8(0x5c51), 0x0a },
	{ CCI_REG8(0x5c60), 0x0a },
	{ CCI_REG8(0x5c6f), 0x1e },
	{ CCI_REG8(0x5c7e), 0x2a },
	{ CCI_REG8(0x5c01), 0x32 },
	{ CCI_REG8(0x5c10), 0x34 },
	{ CCI_REG8(0x5c1f), 0x31 },
	{ CCI_REG8(0x5c2e), 0x31 },
	{ CCI_REG8(0x5c71), 0x28 },
	{ CCI_REG8(0x5c80), 0x28 },
	{ CCI_REG8(0x5c4c), 0x2a },
	{ CCI_REG8(0x33f2), 0x01 },
	{ CCI_REG8(0x1f04), 0x73 },
	{ CCI_REG8(0x1f05), 0x01 },
	{ CCI_REG8(0x5bfa), 0x35 },
	{ CCI_REG8(0x5c09), 0x38 },
	{ CCI_REG8(0x5c18), 0x3a },
	{ CCI_REG8(0x5c27), 0x38 },
	{ CCI_REG8(0x5c5b), 0x25 },
	{ CCI_REG8(0x5c6a), 0x24 },
	{ CCI_REG8(0x5c79), 0x47 },
	{ CCI_REG8(0x5bfc), 0x15 },
	{ CCI_REG8(0x5c0b), 0x2e },
	{ CCI_REG8(0x5c1a), 0x36 },
	{ CCI_REG8(0x5c29), 0x38 },
	{ CCI_REG8(0x5c36), 0x36 },
	{ CCI_REG8(0x5c3f), 0x36 },
	{ CCI_REG8(0x5c4e), 0x0b },
	{ CCI_REG8(0x5c5d), 0x20 },
	{ CCI_REG8(0x5c6c), 0x2a },
	{ CCI_REG8(0x5c7b), 0x25 },
	{ CCI_REG8(0x5c88), 0x25 },
	{ CCI_REG8(0x5c91), 0x22 },
	{ CCI_REG8(0x5bfe), 0x15 },
	{ CCI_REG8(0x5c0d), 0x32 },
	{ CCI_REG8(0x5c1c), 0x36 },
	{ CCI_REG8(0x5c2b), 0x36 },
	{ CCI_REG8(0x5c37), 0x3a },
	{ CCI_REG8(0x5c40), 0x39 },
	{ CCI_REG8(0x5c50), 0x06 },
	{ CCI_REG8(0x5c5f), 0x22 },
	{ CCI_REG8(0x5c6e), 0x23 },
	{ CCI_REG8(0x5c7d), 0x2e },
	{ CCI_REG8(0x5c89), 0x44 },
	{ CCI_REG8(0x5c92), 0x51 },
	{ CCI_REG8(0x5d7f), 0x0a },
	{ CCI_REG8(0x5c00), 0x17 },
	{ CCI_REG8(0x5c0f), 0x36 },
	{ CCI_REG8(0x5c1e), 0x38 },
	{ CCI_REG8(0x5c2d), 0x3c },
	{ CCI_REG8(0x5c38), 0x38 },
	{ CCI_REG8(0x5c41), 0x36 },
	{ CCI_REG8(0x5c52), 0x0a },
	{ CCI_REG8(0x5c61), 0x21 },
	{ CCI_REG8(0x5c70), 0x23 },
	{ CCI_REG8(0x5c7f), 0x1b },
	{ CCI_REG8(0x5c8a), 0x22 },
	{ CCI_REG8(0x5c93), 0x20 },
	{ CCI_REG8(0x5c02), 0x1a },
	{ CCI_REG8(0x5c11), 0x3e },
	{ CCI_REG8(0x5c20), 0x3f },
	{ CCI_REG8(0x5c2f), 0x3d },
	{ CCI_REG8(0x5c39), 0x3e },
	{ CCI_REG8(0x5c42), 0x3c },
	{ CCI_REG8(0x5c54), 0x02 },
	{ CCI_REG8(0x5c63), 0x12 },
	{ CCI_REG8(0x5c72), 0x14 },
	{ CCI_REG8(0x5c81), 0x24 },
	{ CCI_REG8(0x5c8b), 0x1c },
	{ CCI_REG8(0x5c94), 0x4e },
	{ CCI_REG8(0x5d8a), 0x09 },
	{ CCI_REG8(0x5bfb), 0x36 },
	{ CCI_REG8(0x5c0a), 0x38 },
	{ CCI_REG8(0x5c19), 0x36 },
	{ CCI_REG8(0x5c28), 0x36 },
	{ CCI_REG8(0x5c4d), 0x2a },
	{ CCI_REG8(0x5c5c), 0x25 },
	{ CCI_REG8(0x5c6b), 0x25 },
	{ CCI_REG8(0x5c7a), 0x22 },
	{ CCI_REG8(0x5bfd), 0x36 },
	{ CCI_REG8(0x5c0c), 0x36 },
	{ CCI_REG8(0x5c1b), 0x3a },
	{ CCI_REG8(0x5c2a), 0x39 },
	{ CCI_REG8(0x5c4f), 0x23 },
	{ CCI_REG8(0x5c5e), 0x2e },
	{ CCI_REG8(0x5c6d), 0x44 },
	{ CCI_REG8(0x5c7c), 0x51 },
	{ CCI_REG8(0x5d63), 0x0a },
	{ CCI_REG8(0x5bff), 0x38 },
	{ CCI_REG8(0x5c0e), 0x3c },
	{ CCI_REG8(0x5c1d), 0x38 },
	{ CCI_REG8(0x5c2c), 0x36 },
	{ CCI_REG8(0x5c51), 0x23 },
	{ CCI_REG8(0x5c60), 0x1b },
	{ CCI_REG8(0x5c6f), 0x22 },
	{ CCI_REG8(0x5c7e), 0x20 },
	{ CCI_REG8(0x5c01), 0x3f },
	{ CCI_REG8(0x5c10), 0x3d },
	{ CCI_REG8(0x5c1f), 0x3e },
	{ CCI_REG8(0x5c2e), 0x3c },
	{ CCI_REG8(0x5c53), 0x14 },
	{ CCI_REG8(0x5c62), 0x24 },
	{ CCI_REG8(0x5c71), 0x1c },
	{ CCI_REG8(0x5c80), 0x4e },
	{ CCI_REG8(0x5d76), 0x09 },
	{ CCI_REG8(0x5c4c), 0x2a },
	{ CCI_REG8(0x33f2), 0x02 },
	{ CCI_REG8(0x1f04), 0x78 },
	{ CCI_REG8(0x1f05), 0x01 },
	{ CCI_REG8(0x5bfa), 0x37 },
	{ CCI_REG8(0x5c09), 0x36 },
	{ CCI_REG8(0x5c18), 0x39 },
	{ CCI_REG8(0x5c27), 0x38 },
	{ CCI_REG8(0x5c5b), 0x27 },
	{ CCI_REG8(0x5c6a), 0x2b },
	{ CCI_REG8(0x5c79), 0x48 },
	{ CCI_REG8(0x5bfc), 0x16 },
	{ CCI_REG8(0x5c0b), 0x32 },
	{ CCI_REG8(0x5c1a), 0x33 },
	{ CCI_REG8(0x5c29), 0x37 },
	{ CCI_REG8(0x5c36), 0x36 },
	{ CCI_REG8(0x5c3f), 0x35 },
	{ CCI_REG8(0x5c4e), 0x0d },
	{ CCI_REG8(0x5c5d), 0x2d },
	{ CCI_REG8(0x5c6c), 0x23 },
	{ CCI_REG8(0x5c7b), 0x25 },
	{ CCI_REG8(0x5c88), 0x31 },
	{ CCI_REG8(0x5c91), 0x2e },
	{ CCI_REG8(0x5bfe), 0x15 },
	{ CCI_REG8(0x5c0d), 0x31 },
	{ CCI_REG8(0x5c1c), 0x35 },
	{ CCI_REG8(0x5c2b), 0x36 },
	{ CCI_REG8(0x5c37), 0x35 },
	{ CCI_REG8(0x5c40), 0x37 },
	{ CCI_REG8(0x5c50), 0x0f },
	{ CCI_REG8(0x5c5f), 0x31 },
	{ CCI_REG8(0x5c6e), 0x30 },
	{ CCI_REG8(0x5c7d), 0x33 },
	{ CCI_REG8(0x5c89), 0x36 },
	{ CCI_REG8(0x5c92), 0x5b },
	{ CCI_REG8(0x5c00), 0x13 },
	{ CCI_REG8(0x5c0f), 0x2f },
	{ CCI_REG8(0x5c1e), 0x2e },
	{ CCI_REG8(0x5c2d), 0x34 },
	{ CCI_REG8(0x5c38), 0x33 },
	{ CCI_REG8(0x5c41), 0x32 },
	{ CCI_REG8(0x5c52), 0x0d },
	{ CCI_REG8(0x5c61), 0x27 },
	{ CCI_REG8(0x5c70), 0x28 },
	{ CCI_REG8(0x5c7f), 0x1f },
	{ CCI_REG8(0x5c8a), 0x25 },
	{ CCI_REG8(0x5c93), 0x2c },
	{ CCI_REG8(0x5c02), 0x15 },
	{ CCI_REG8(0x5c11), 0x36 },
	{ CCI_REG8(0x5c20), 0x39 },
	{ CCI_REG8(0x5c2f), 0x3a },
	{ CCI_REG8(0x5c39), 0x37 },
	{ CCI_REG8(0x5c42), 0x37 },
	{ CCI_REG8(0x5c54), 0x04 },
	{ CCI_REG8(0x5c63), 0x1c },
	{ CCI_REG8(0x5c72), 0x1c },
	{ CCI_REG8(0x5c81), 0x1c },
	{ CCI_REG8(0x5c8b), 0x28 },
	{ CCI_REG8(0x5c94), 0x24 },
	{ CCI_REG8(0x5bfb), 0x33 },
	{ CCI_REG8(0x5c0a), 0x37 },
	{ CCI_REG8(0x5c19), 0x36 },
	{ CCI_REG8(0x5c28), 0x35 },
	{ CCI_REG8(0x5c4d), 0x23 },
	{ CCI_REG8(0x5c5c), 0x25 },
	{ CCI_REG8(0x5c6b), 0x31 },
	{ CCI_REG8(0x5c7a), 0x2e },
	{ CCI_REG8(0x5bfd), 0x35 },
	{ CCI_REG8(0x5c0c), 0x36 },
	{ CCI_REG8(0x5c1b), 0x35 },
	{ CCI_REG8(0x5c2a), 0x37 },
	{ CCI_REG8(0x5c4f), 0x30 },
	{ CCI_REG8(0x5c5e), 0x33 },
	{ CCI_REG8(0x5c6d), 0x36 },
	{ CCI_REG8(0x5c7c), 0x5b },
	{ CCI_REG8(0x5bff), 0x2e },
	{ CCI_REG8(0x5c0e), 0x34 },
	{ CCI_REG8(0x5c1d), 0x33 },
	{ CCI_REG8(0x5c2c), 0x32 },
	{ CCI_REG8(0x5c51), 0x28 },
	{ CCI_REG8(0x5c60), 0x1f },
	{ CCI_REG8(0x5c6f), 0x25 },
	{ CCI_REG8(0x5c7e), 0x2c },
	{ CCI_REG8(0x5c01), 0x39 },
	{ CCI_REG8(0x5c10), 0x3a },
	{ CCI_REG8(0x5c1f), 0x37 },
	{ CCI_REG8(0x5c2e), 0x37 },
	{ CCI_REG8(0x5c53), 0x1c },
	{ CCI_REG8(0x5c62), 0x1c },
	{ CCI_REG8(0x5c71), 0x28 },
	{ CCI_REG8(0x5c80), 0x24 },
	{ CCI_REG8(0x5c4c), 0x2c },
	{ CCI_REG8(0x33f2), 0x03 },
	{ CCI_REG8(0x1f08), 0x00 },
	{ CCI_REG8(0x32c8), 0x00 },
	{ CCI_REG8(0x4017), 0x40 },
	{ CCI_REG8(0x40a2), 0x01 },
	{ CCI_REG8(0x40ac), 0x01 },
	{ CCI_REG8(0x4328), 0x00 },
	{ CCI_REG8(0x4329), 0xb3 },
	{ CCI_REG8(0x4e15), 0x10 },
	{ CCI_REG8(0x4e19), 0x2f },
	{ CCI_REG8(0x4e21), 0x0f },
	{ CCI_REG8(0x4e2f), 0x10 },
	{ CCI_REG8(0x4e3d), 0x10 },
	{ CCI_REG8(0x4e41), 0x2f },
	{ CCI_REG8(0x4e57), 0x29 },
	{ CCI_REG8(0x4ffb), 0x2f },
	{ CCI_REG8(0x5011), 0x24 },
	{ CCI_REG8(0x501d), 0x03 },
	{ CCI_REG8(0x505f), 0x41 },
	{ CCI_REG8(0x5060), 0xdf },
	{ CCI_REG8(0x5065), 0xdf },
	{ CCI_REG8(0x5066), 0x37 },
	{ CCI_REG8(0x506e), 0x57 },
	{ CCI_REG8(0x5070), 0xc5 },
	{ CCI_REG8(0x5072), 0x57 },
	{ CCI_REG8(0x5075), 0x53 },
	{ CCI_REG8(0x5076), 0x55 },
	{ CCI_REG8(0x5077), 0xc1 },
	{ CCI_REG8(0x5078), 0xc3 },
	{ CCI_REG8(0x5079), 0x53 },
	{ CCI_REG8(0x507a), 0x55 },
	{ CCI_REG8(0x507d), 0x57 },
	{ CCI_REG8(0x507e), 0xdf },
	{ CCI_REG8(0x507f), 0xc5 },
	{ CCI_REG8(0x5081), 0x57 },
	{ CCI_REG8(0x53c8), 0x01 },
	{ CCI_REG8(0x53c9), 0xe2 },
	{ CCI_REG8(0x53ca), 0x03 },
	{ CCI_REG8(0x5422), 0x7a },
	{ CCI_REG8(0x548e), 0x40 },
	{ CCI_REG8(0x5497), 0x5e },
	{ CCI_REG8(0x54a1), 0x40 },
	{ CCI_REG8(0x54a9), 0x40 },
	{ CCI_REG8(0x54b2), 0x5e },
	{ CCI_REG8(0x54bc), 0x40 },
	{ CCI_REG8(0x57c6), 0x00 },
	{ CCI_REG8(0x583d), 0x0e },
	{ CCI_REG8(0x583e), 0x0e },
	{ CCI_REG8(0x583f), 0x0e },
	{ CCI_REG8(0x5840), 0x0e },
	{ CCI_REG8(0x5841), 0x0e },
	{ CCI_REG8(0x5842), 0x0e },
	{ CCI_REG8(0x5900), 0x12 },
	{ CCI_REG8(0x5901), 0x12 },
	{ CCI_REG8(0x5902), 0x14 },
	{ CCI_REG8(0x5903), 0x12 },
	{ CCI_REG8(0x5904), 0x14 },
	{ CCI_REG8(0x5905), 0x12 },
	{ CCI_REG8(0x5906), 0x14 },
	{ CCI_REG8(0x5907), 0x12 },
	{ CCI_REG8(0x590f), 0x12 },
	{ CCI_REG8(0x5911), 0x12 },
	{ CCI_REG8(0x5913), 0x12 },
	{ CCI_REG8(0x591c), 0x12 },
	{ CCI_REG8(0x591e), 0x12 },
	{ CCI_REG8(0x5920), 0x12 },
	{ CCI_REG8(0x5948), 0x08 },
	{ CCI_REG8(0x5949), 0x08 },
	{ CCI_REG8(0x594a), 0x08 },
	{ CCI_REG8(0x594b), 0x08 },
	{ CCI_REG8(0x594c), 0x08 },
	{ CCI_REG8(0x594d), 0x08 },
	{ CCI_REG8(0x594e), 0x08 },
	{ CCI_REG8(0x594f), 0x08 },
	{ CCI_REG8(0x595c), 0x08 },
	{ CCI_REG8(0x595e), 0x08 },
	{ CCI_REG8(0x5960), 0x08 },
	{ CCI_REG8(0x596e), 0x08 },
	{ CCI_REG8(0x5970), 0x08 },
	{ CCI_REG8(0x5972), 0x08 },
	{ CCI_REG8(0x597e), 0x0f },
	{ CCI_REG8(0x597f), 0x0f },
	{ CCI_REG8(0x599a), 0x0f },
	{ CCI_REG8(0x59de), 0x08 },
	{ CCI_REG8(0x59df), 0x08 },
	{ CCI_REG8(0x59fa), 0x08 },
	{ CCI_REG8(0x5a59), 0x22 },
	{ CCI_REG8(0x5a5b), 0x22 },
	{ CCI_REG8(0x5a5d), 0x1a },
	{ CCI_REG8(0x5a5f), 0x22 },
	{ CCI_REG8(0x5a61), 0x1a },
	{ CCI_REG8(0x5a63), 0x22 },
	{ CCI_REG8(0x5a65), 0x1a },
	{ CCI_REG8(0x5a67), 0x22 },
	{ CCI_REG8(0x5a77), 0x22 },
	{ CCI_REG8(0x5a7b), 0x22 },
	{ CCI_REG8(0x5a7f), 0x22 },
	{ CCI_REG8(0x5a91), 0x22 },
	{ CCI_REG8(0x5a95), 0x22 },
	{ CCI_REG8(0x5a99), 0x22 },
	{ CCI_REG8(0x5ae9), 0x66 },
	{ CCI_REG8(0x5aeb), 0x66 },
	{ CCI_REG8(0x5aed), 0x5e },
	{ CCI_REG8(0x5aef), 0x66 },
	{ CCI_REG8(0x5af1), 0x5e },
	{ CCI_REG8(0x5af3), 0x66 },
	{ CCI_REG8(0x5af5), 0x5e },
	{ CCI_REG8(0x5af7), 0x66 },
	{ CCI_REG8(0x5b07), 0x66 },
	{ CCI_REG8(0x5b0b), 0x66 },
	{ CCI_REG8(0x5b0f), 0x66 },
	{ CCI_REG8(0x5b21), 0x66 },
	{ CCI_REG8(0x5b25), 0x66 },
	{ CCI_REG8(0x5b29), 0x66 },
	{ CCI_REG8(0x5b79), 0x46 },
	{ CCI_REG8(0x5b7b), 0x3e },
	{ CCI_REG8(0x5b7d), 0x3e },
	{ CCI_REG8(0x5b89), 0x46 },
	{ CCI_REG8(0x5b8b), 0x46 },
	{ CCI_REG8(0x5b97), 0x46 },
	{ CCI_REG8(0x5b99), 0x46 },
	{ CCI_REG8(0x5c9e), 0x0a },
	{ CCI_REG8(0x5c9f), 0x08 },
	{ CCI_REG8(0x5ca0), 0x0a },
	{ CCI_REG8(0x5ca1), 0x0a },
	{ CCI_REG8(0x5ca2), 0x0b },
	{ CCI_REG8(0x5ca3), 0x06 },
	{ CCI_REG8(0x5ca4), 0x04 },
	{ CCI_REG8(0x5ca5), 0x06 },
	{ CCI_REG8(0x5ca6), 0x04 },
	{ CCI_REG8(0x5cad), 0x0b },
	{ CCI_REG8(0x5cae), 0x0a },
	{ CCI_REG8(0x5caf), 0x0c },
	{ CCI_REG8(0x5cb0), 0x0a },
	{ CCI_REG8(0x5cb1), 0x0b },
	{ CCI_REG8(0x5cb2), 0x08 },
	{ CCI_REG8(0x5cb3), 0x06 },
	{ CCI_REG8(0x5cb4), 0x08 },
	{ CCI_REG8(0x5cb5), 0x04 },
	{ CCI_REG8(0x5cbc), 0x0b },
	{ CCI_REG8(0x5cbd), 0x09 },
	{ CCI_REG8(0x5cbe), 0x08 },
	{ CCI_REG8(0x5cbf), 0x09 },
	{ CCI_REG8(0x5cc0), 0x0a },
	{ CCI_REG8(0x5cc1), 0x08 },
	{ CCI_REG8(0x5cc2), 0x06 },
	{ CCI_REG8(0x5cc3), 0x08 },
	{ CCI_REG8(0x5cc4), 0x06 },
	{ CCI_REG8(0x5ccb), 0x0a },
	{ CCI_REG8(0x5ccc), 0x09 },
	{ CCI_REG8(0x5ccd), 0x0a },
	{ CCI_REG8(0x5cce), 0x08 },
	{ CCI_REG8(0x5ccf), 0x0a },
	{ CCI_REG8(0x5cd0), 0x08 },
	{ CCI_REG8(0x5cd1), 0x08 },
	{ CCI_REG8(0x5cd2), 0x08 },
	{ CCI_REG8(0x5cd3), 0x08 },
	{ CCI_REG8(0x5cda), 0x09 },
	{ CCI_REG8(0x5cdb), 0x09 },
	{ CCI_REG8(0x5cdc), 0x08 },
	{ CCI_REG8(0x5cdd), 0x08 },
	{ CCI_REG8(0x5ce3), 0x09 },
	{ CCI_REG8(0x5ce4), 0x08 },
	{ CCI_REG8(0x5ce5), 0x08 },
	{ CCI_REG8(0x5ce6), 0x08 },
	{ CCI_REG8(0x5cf4), 0x04 },
	{ CCI_REG8(0x5d04), 0x04 },
	{ CCI_REG8(0x5d13), 0x06 },
	{ CCI_REG8(0x5d22), 0x06 },
	{ CCI_REG8(0x5d23), 0x04 },
	{ CCI_REG8(0x5d2e), 0x06 },
	{ CCI_REG8(0x5d37), 0x06 },
	{ CCI_REG8(0x5d6f), 0x09 },
	{ CCI_REG8(0x5d72), 0x0f },
	{ CCI_REG8(0x5d88), 0x0f },
	{ CCI_REG8(0x5de6), 0x01 },
	{ CCI_REG8(0x5de7), 0x01 },
	{ CCI_REG8(0x5de8), 0x01 },
	{ CCI_REG8(0x5de9), 0x01 },
	{ CCI_REG8(0x5dea), 0x01 },
	{ CCI_REG8(0x5deb), 0x01 },
	{ CCI_REG8(0x5dec), 0x01 },
	{ CCI_REG8(0x5df2), 0x01 },
	{ CCI_REG8(0x5df3), 0x01 },
	{ CCI_REG8(0x5df4), 0x01 },
	{ CCI_REG8(0x5df5), 0x01 },
	{ CCI_REG8(0x5df6), 0x01 },
	{ CCI_REG8(0x5df7), 0x01 },
	{ CCI_REG8(0x5df8), 0x01 },
	{ CCI_REG8(0x5dfe), 0x01 },
	{ CCI_REG8(0x5dff), 0x01 },
	{ CCI_REG8(0x5e00), 0x01 },
	{ CCI_REG8(0x5e01), 0x01 },
	{ CCI_REG8(0x5e02), 0x01 },
	{ CCI_REG8(0x5e03), 0x01 },
	{ CCI_REG8(0x5e04), 0x01 },
	{ CCI_REG8(0x5e0a), 0x01 },
	{ CCI_REG8(0x5e0b), 0x01 },
	{ CCI_REG8(0x5e0c), 0x01 },
	{ CCI_REG8(0x5e0d), 0x01 },
	{ CCI_REG8(0x5e0e), 0x01 },
	{ CCI_REG8(0x5e0f), 0x01 },
	{ CCI_REG8(0x5e10), 0x01 },
	{ CCI_REG8(0x5e16), 0x01 },
	{ CCI_REG8(0x5e17), 0x01 },
	{ CCI_REG8(0x5e18), 0x01 },
	{ CCI_REG8(0x5e1e), 0x01 },
	{ CCI_REG8(0x5e1f), 0x01 },
	{ CCI_REG8(0x5e20), 0x01 },
	{ CCI_REG8(0x5e6e), 0x5a },
	{ CCI_REG8(0x5e6f), 0x46 },
	{ CCI_REG8(0x5e70), 0x46 },
	{ CCI_REG8(0x5e71), 0x3c },
	{ CCI_REG8(0x5e72), 0x3c },
	{ CCI_REG8(0x5e73), 0x28 },
	{ CCI_REG8(0x5e74), 0x28 },
	{ CCI_REG8(0x5e75), 0x6e },
	{ CCI_REG8(0x5e76), 0x6e },
	{ CCI_REG8(0x5e81), 0x46 },
	{ CCI_REG8(0x5e83), 0x3c },
	{ CCI_REG8(0x5e85), 0x28 },
	{ CCI_REG8(0x5e87), 0x6e },
	{ CCI_REG8(0x5e92), 0x46 },
	{ CCI_REG8(0x5e94), 0x3c },
	{ CCI_REG8(0x5e96), 0x28 },
	{ CCI_REG8(0x5e98), 0x6e },
	{ CCI_REG8(0x5ecb), 0x26 },
	{ CCI_REG8(0x5ecc), 0x26 },
	{ CCI_REG8(0x5ecd), 0x26 },
	{ CCI_REG8(0x5ece), 0x26 },
	{ CCI_REG8(0x5ed2), 0x26 },
	{ CCI_REG8(0x5ed3), 0x26 },
	{ CCI_REG8(0x5ed4), 0x26 },
	{ CCI_REG8(0x5ed5), 0x26 },
	{ CCI_REG8(0x5ed9), 0x26 },
	{ CCI_REG8(0x5eda), 0x26 },
	{ CCI_REG8(0x5ee5), 0x08 },
	{ CCI_REG8(0x5ee6), 0x08 },
	{ CCI_REG8(0x5ee7), 0x08 },
	{ CCI_REG8(0x6006), 0x14 },
	{ CCI_REG8(0x6007), 0x14 },
	{ CCI_REG8(0x6008), 0x14 },
	{ CCI_REG8(0x6009), 0x14 },
	{ CCI_REG8(0x600a), 0x14 },
	{ CCI_REG8(0x600b), 0x14 },
	{ CCI_REG8(0x600c), 0x14 },
	{ CCI_REG8(0x600d), 0x22 },
	{ CCI_REG8(0x600e), 0x22 },
	{ CCI_REG8(0x600f), 0x14 },
	{ CCI_REG8(0x601a), 0x14 },
	{ CCI_REG8(0x601b), 0x14 },
	{ CCI_REG8(0x601c), 0x14 },
	{ CCI_REG8(0x601d), 0x14 },
	{ CCI_REG8(0x601e), 0x14 },
	{ CCI_REG8(0x601f), 0x14 },
	{ CCI_REG8(0x6020), 0x14 },
	{ CCI_REG8(0x6021), 0x22 },
	{ CCI_REG8(0x6022), 0x22 },
	{ CCI_REG8(0x6023), 0x14 },
	{ CCI_REG8(0x602e), 0x14 },
	{ CCI_REG8(0x602f), 0x14 },
	{ CCI_REG8(0x6030), 0x14 },
	{ CCI_REG8(0x6031), 0x22 },
	{ CCI_REG8(0x6039), 0x14 },
	{ CCI_REG8(0x603a), 0x14 },
	{ CCI_REG8(0x603b), 0x14 },
	{ CCI_REG8(0x603c), 0x22 },
	{ CCI_REG8(0x6132), 0x0f },
	{ CCI_REG8(0x6133), 0x0f },
	{ CCI_REG8(0x6134), 0x0f },
	{ CCI_REG8(0x6135), 0x0f },
	{ CCI_REG8(0x6136), 0x0f },
	{ CCI_REG8(0x6137), 0x0f },
	{ CCI_REG8(0x6138), 0x0f },
	{ CCI_REG8(0x613e), 0x0f },
	{ CCI_REG8(0x613f), 0x0f },
	{ CCI_REG8(0x6140), 0x0f },
	{ CCI_REG8(0x6141), 0x0f },
	{ CCI_REG8(0x6142), 0x0f },
	{ CCI_REG8(0x6143), 0x0f },
	{ CCI_REG8(0x6144), 0x0f },
	{ CCI_REG8(0x614a), 0x0f },
	{ CCI_REG8(0x614b), 0x0f },
	{ CCI_REG8(0x614c), 0x0f },
	{ CCI_REG8(0x614d), 0x0f },
	{ CCI_REG8(0x614e), 0x0f },
	{ CCI_REG8(0x614f), 0x0f },
	{ CCI_REG8(0x6150), 0x0f },
	{ CCI_REG8(0x6156), 0x0f },
	{ CCI_REG8(0x6157), 0x0f },
	{ CCI_REG8(0x6158), 0x0f },
	{ CCI_REG8(0x6159), 0x0f },
	{ CCI_REG8(0x615a), 0x0f },
	{ CCI_REG8(0x615b), 0x0f },
	{ CCI_REG8(0x615c), 0x0f },
	{ CCI_REG8(0x6162), 0x0f },
	{ CCI_REG8(0x6163), 0x0f },
	{ CCI_REG8(0x6164), 0x0f },
	{ CCI_REG8(0x616a), 0x0f },
	{ CCI_REG8(0x616b), 0x0f },
	{ CCI_REG8(0x616c), 0x0f },
	{ CCI_REG8(0x6226), 0x00 },
	{ CCI_REG8(0x84f8), 0x01 },
	{ CCI_REG8(0x8501), 0x00 },
	{ CCI_REG8(0x8502), 0x01 },
	{ CCI_REG8(0x8505), 0x00 },
	{ CCI_REG8(0x8744), 0x00 },
	{ CCI_REG8(0x883c), 0x01 },
	{ CCI_REG8(0x8845), 0x00 },
	{ CCI_REG8(0x8846), 0x01 },
	{ CCI_REG8(0x8849), 0x00 },
	{ CCI_REG8(0x9004), 0x1f },
	{ CCI_REG8(0x9064), 0x4d },
	{ CCI_REG8(0x9065), 0x3d },
	{ CCI_REG8(0x922e), 0x91 },
	{ CCI_REG8(0x922f), 0x2a },
	{ CCI_REG8(0x9230), 0xe2 },
	{ CCI_REG8(0x9231), 0xc0 },
	{ CCI_REG8(0x9232), 0xe2 },
	{ CCI_REG8(0x9233), 0xc1 },
	{ CCI_REG8(0x9234), 0xe2 },
	{ CCI_REG8(0x9235), 0xc2 },
	{ CCI_REG8(0x9236), 0xe2 },
	{ CCI_REG8(0x9237), 0xc3 },
	{ CCI_REG8(0x9238), 0xe2 },
	{ CCI_REG8(0x9239), 0xd4 },
	{ CCI_REG8(0x923a), 0xe2 },
	{ CCI_REG8(0x923b), 0xd5 },
	{ CCI_REG8(0x923c), 0x90 },
	{ CCI_REG8(0x923d), 0x64 },
	{ CCI_REG8(0xb0b9), 0x10 },
	{ CCI_REG8(0xbc76), 0x00 },
	{ CCI_REG8(0xbc77), 0x00 },
	{ CCI_REG8(0xbc78), 0x00 },
	{ CCI_REG8(0xbc79), 0x00 },
	{ CCI_REG8(0xbc7b), 0x28 },
	{ CCI_REG8(0xbc7c), 0x00 },
	{ CCI_REG8(0xbc7d), 0x00 },
	{ CCI_REG8(0xbc7f), 0xc0 },
	{ CCI_REG8(0xc6b9), 0x01 },
	{ CCI_REG8(0xecb5), 0x04 },
	{ CCI_REG8(0xecbf), 0x04 },
};

/* 2x2 binned; the sensible default for preview and stills. */
static const struct cci_reg_sequence imx682_4624x3472_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x02 },
	{ CCI_REG8(0x0342), 0x24 },
	{ CCI_REG8(0x0343), 0xd8 },
	{ CCI_REG8(0x0340), 0x0d },
	{ CCI_REG8(0x0341), 0xf5 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x24 },
	{ CCI_REG8(0x0349), 0x1f },
	{ CCI_REG8(0x034a), 0x1b },
	{ CCI_REG8(0x034b), 0x1f },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x30d8), 0x04 },
	{ CCI_REG8(0x3200), 0x41 },
	{ CCI_REG8(0x3201), 0x41 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x12 },
	{ CCI_REG8(0x040d), 0x10 },
	{ CCI_REG8(0x040e), 0x0d },
	{ CCI_REG8(0x040f), 0x90 },
	{ CCI_REG8(0x034c), 0x12 },
	{ CCI_REG8(0x034d), 0x10 },
	{ CCI_REG8(0x034e), 0x0d },
	{ CCI_REG8(0x034f), 0x90 },
	{ CCI_REG8(0x0301), 0x08 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x3c },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0x38 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x30d9), 0x00 },
	{ CCI_REG8(0x32d5), 0x00 },
	{ CCI_REG8(0x32d6), 0x00 },
	{ CCI_REG8(0x401e), 0x00 },
	{ CCI_REG8(0x40b8), 0x01 },
	{ CCI_REG8(0x40b9), 0x2c },
	{ CCI_REG8(0x40bc), 0x01 },
	{ CCI_REG8(0x40bd), 0x18 },
	{ CCI_REG8(0x40be), 0x00 },
	{ CCI_REG8(0x40bf), 0x00 },
	{ CCI_REG8(0x41a4), 0x00 },
	{ CCI_REG8(0x5a09), 0x01 },
	{ CCI_REG8(0x5a17), 0x01 },
	{ CCI_REG8(0x5a25), 0x01 },
	{ CCI_REG8(0x5a33), 0x01 },
	{ CCI_REG8(0x98d7), 0xb4 },
	{ CCI_REG8(0x98d8), 0x8c },
	{ CCI_REG8(0x98d9), 0x0a },
	{ CCI_REG8(0x99c4), 0x16 },
	{ CCI_REG8(0x0202), 0x0d },
	{ CCI_REG8(0x0203), 0xc5 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x4018), 0x04 },
	{ CCI_REG8(0x4019), 0x80 },
	{ CCI_REG8(0x401a), 0x00 },
	{ CCI_REG8(0x401b), 0x01 },
	{ CCI_REG8(0x3400), 0x02 },
	{ CCI_REG8(0x3093), 0x01 },
};

static const struct imx682_mode imx682_supported_modes[] = {
	{
		/* 2x2 binned; the sensible default for preview and stills. */
		.width = 4624,
		.height = 3472,
		.hts = 9432,
		.vts = 3573,
		.link_freq_idx = IMX682_LINK_FREQ_998MHZ,
		.reg_list = {
			.regs = imx682_4624x3472_regs,
			.num_regs = ARRAY_SIZE(imx682_4624x3472_regs),
		},
	},
};

static u64 imx682_pixel_rate(const struct imx682_mode *mode)
{
	/*
	 * C-PHY moves 16 bits per 7 symbols on each trio, which works out as
	 * 2.28 bits per symbol.
	 */
	return div_u64(imx682_link_freq_menu[mode->link_freq_idx] * 228 *
		       IMX682_DATA_LANES, 100 * IMX682_BITS_PER_SAMPLE);
}

static int imx682_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx682 *imx682 = container_of(ctrl->handler, struct imx682,
					     ctrl_handler);
	const struct imx682_mode *mode = imx682->mode;
	s64 exposure_max;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Keep the exposure range consistent with the new blanking. */
		exposure_max = mode->height + ctrl->val - IMX682_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx682->exposure,
					 imx682->exposure->minimum,
					 exposure_max, imx682->exposure->step,
					 min(imx682->exposure->val,
					     exposure_max));
	}

	if (!pm_runtime_get_if_in_use(imx682->dev))
		return 0;

	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(imx682->regmap, IMX682_REG_AGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(imx682->regmap, IMX682_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(imx682->regmap, IMX682_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(imx682->regmap, IMX682_REG_ORIENTATION,
			  (imx682->hflip->val ? IMX682_HFLIP : 0) |
			  (imx682->vflip->val ? IMX682_VFLIP : 0), &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(imx682->regmap, IMX682_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(imx682->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx682_ctrl_ops = {
	.s_ctrl = imx682_set_ctrl,
};

static int imx682_init_controls(struct imx682 *imx682)
{
	const struct imx682_mode *mode = imx682->mode;
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx682->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max, hblank, vblank;
	u64 pixel_rate;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 12);
	if (ret)
		return ret;

	imx682->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx682_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(imx682_link_freq_menu) - 1,
					mode->link_freq_idx,
					imx682_link_freq_menu);
	if (imx682->link_freq)
		imx682->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = imx682_pixel_rate(mode);
	imx682->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       pixel_rate, 1, pixel_rate);
	if (imx682->pixel_rate)
		imx682->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank = mode->hts - mode->width;
	imx682->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (imx682->hblank)
		imx682->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	imx682->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   IMX682_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX682_AGAIN_MIN, IMX682_AGAIN_MAX,
			  IMX682_AGAIN_STEP, IMX682_AGAIN_DEFAULT);

	exposure_max = mode->vts - IMX682_EXPOSURE_MARGIN;
	imx682->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX682_EXPOSURE_MIN, exposure_max,
					     IMX682_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx682_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx682_test_pattern_menu) - 1,
				     0, 0, imx682_test_pattern_menu);

	imx682->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx682->hflip)
		imx682->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx682->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx682_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx682->vflip)
		imx682->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(imx682->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx682_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	imx682->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int imx682_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx682 *imx682 = to_imx682(sd);
	const struct imx682_reg_list *reg_list = &imx682->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(imx682->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(imx682->regmap, imx682_init_regs,
			    ARRAY_SIZE(imx682_init_regs), &ret);
	cci_multi_reg_write(imx682->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(imx682->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(imx682->regmap, IMX682_REG_CTRL_MODE,
		  IMX682_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(imx682->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(imx682->dev);

	return ret;
}

static int imx682_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx682 *imx682 = to_imx682(sd);
	int ret;

	ret = cci_write(imx682->regmap, IMX682_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(imx682->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(imx682->dev);

	return ret;
}

static u32 imx682_get_format_code(struct imx682 *imx682)
{
	unsigned int i;

	i = (imx682->vflip->val ? 2 : 0) | (imx682->hflip->val ? 1 : 0);

	return imx682_mbus_formats[i];
}

static void imx682_update_pad_format(struct imx682 *imx682,
				     const struct imx682_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = imx682_get_format_code(imx682);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx682_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx682 *imx682 = to_imx682(sd);
	const struct imx682_mode *mode;
	struct v4l2_mbus_framefmt *format;
	s64 exposure_max, hblank, vblank;

	mode = v4l2_find_nearest_size(imx682_supported_modes,
				      ARRAY_SIZE(imx682_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	imx682_update_pad_format(imx682, mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	imx682->mode = mode;

	__v4l2_ctrl_s_ctrl(imx682->link_freq, mode->link_freq_idx);
	__v4l2_ctrl_s_ctrl_int64(imx682->pixel_rate, imx682_pixel_rate(mode));

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(imx682->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(imx682->vblank, vblank,
				 IMX682_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(imx682->vblank, vblank);

	exposure_max = mode->vts - IMX682_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(imx682->exposure, IMX682_EXPOSURE_MIN,
				 exposure_max, IMX682_EXPOSURE_STEP,
				 exposure_max);

	return 0;
}

static int imx682_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx682 *imx682 = to_imx682(sd);

	if (code->index)
		return -EINVAL;

	code->code = imx682_get_format_code(imx682);

	return 0;
}

static int imx682_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx682 *imx682 = to_imx682(sd);

	if (fse->index >= ARRAY_SIZE(imx682_supported_modes))
		return -EINVAL;

	if (fse->code != imx682_get_format_code(imx682))
		return -EINVAL;

	fse->min_width = imx682_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = imx682_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int imx682_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = imx682_supported_modes[1 - 1].width;
		sel->r.height = imx682_supported_modes[1 - 1].height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx682_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx682 *imx682 = to_imx682(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = imx682->mode->width,
			.height = imx682->mode->height,
		},
	};

	return imx682_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops imx682_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx682_pad_ops = {
	.set_fmt = imx682_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = imx682_get_selection,
	.enum_mbus_code = imx682_enum_mbus_code,
	.enum_frame_size = imx682_enum_frame_size,
	.enable_streams = imx682_enable_streams,
	.disable_streams = imx682_disable_streams,
};

static const struct v4l2_subdev_ops imx682_subdev_ops = {
	.video = &imx682_video_ops,
	.pad = &imx682_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx682_internal_ops = {
	.init_state = imx682_init_state,
};

static const struct media_entity_operations imx682_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx682_identify_sensor(struct imx682 *imx682)
{
	u64 val;
	int ret;

	ret = cci_read(imx682->regmap, IMX682_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(imx682->dev, ret, "failed to read chip id\n");

	if (val != IMX682_CHIP_ID)
		return dev_err_probe(imx682->dev, -ENODEV,
				     "chip id mismatch: %#x != %#llx\n",
				     IMX682_CHIP_ID, val);

	return 0;
}

static int imx682_check_hwcfg(struct imx682 *imx682)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx682->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_CPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EPROBE_DEFER;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX682_DATA_LANES) {
		dev_err(imx682->dev, "invalid number of data trios: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx682->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx682_link_freq_menu,
				       ARRAY_SIZE(imx682_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx682_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx682 *imx682 = to_imx682(sd);
	int ret;

	if (imx682->dvdd) {
		ret = regulator_enable(imx682->dvdd);
		if (ret)
			return ret;

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
	}

	if (imx682->avdd) {
		ret = regulator_enable(imx682->avdd);
		if (ret)
			goto disable_dvdd;
	}

	if (imx682->dovdd) {
		ret = regulator_enable(imx682->dovdd);
		if (ret)
			goto disable_avdd;
	}

	ret = clk_prepare_enable(imx682->mclk);
	if (ret)
		goto disable_dovdd;

	gpiod_set_value_cansleep(imx682->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	return 0;

disable_dovdd:
	if (imx682->dovdd)
		regulator_disable(imx682->dovdd);
disable_avdd:
	if (imx682->avdd)
		regulator_disable(imx682->avdd);
disable_dvdd:
	if (imx682->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(imx682->dvdd);
	}

	return ret;
}

static int imx682_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx682 *imx682 = to_imx682(sd);

	gpiod_set_value_cansleep(imx682->reset_gpio, 1);
	clk_disable_unprepare(imx682->mclk);

	if (imx682->dovdd)
		regulator_disable(imx682->dovdd);
	if (imx682->avdd)
		regulator_disable(imx682->avdd);
	if (imx682->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(imx682->dvdd);
	}

	return 0;
}

static int imx682_get_regulators(struct imx682 *imx682)
{
	static const char * const names[] = { "avdd", "dvdd", "dovdd" };
	struct regulator **targets[] = {
		&imx682->avdd, &imx682->dvdd, &imx682->dovdd,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct regulator *reg;

		reg = devm_regulator_get_optional(imx682->dev, names[i]);
		if (IS_ERR(reg)) {
			if (PTR_ERR(reg) != -ENODEV)
				return dev_err_probe(imx682->dev, PTR_ERR(reg),
						     "failed to get %s\n",
						     names[i]);
			reg = NULL;
		}
		*targets[i] = reg;
	}

	return 0;
}

static int imx682_probe(struct i2c_client *client)
{
	struct imx682 *imx682;
	unsigned long freq;
	int ret;

	imx682 = devm_kzalloc(&client->dev, sizeof(*imx682), GFP_KERNEL);
	if (!imx682)
		return -ENOMEM;

	imx682->dev = &client->dev;
	imx682->mode = &imx682_supported_modes[0];
	v4l2_i2c_subdev_init(&imx682->sd, client, &imx682_subdev_ops);

	imx682->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx682->regmap))
		return dev_err_probe(imx682->dev, PTR_ERR(imx682->regmap),
				     "failed to init CCI\n");

	imx682->mclk = devm_v4l2_sensor_clk_get(imx682->dev, NULL);
	if (IS_ERR(imx682->mclk))
		return dev_err_probe(imx682->dev, PTR_ERR(imx682->mclk),
				     "failed to get MCLK\n");

	freq = clk_get_rate(imx682->mclk);
	if (freq != IMX682_MCLK_FREQ)
		return dev_err_probe(imx682->dev, -EINVAL,
				     "MCLK frequency %lu not supported\n", freq);

	ret = imx682_check_hwcfg(imx682);
	if (ret)
		return ret;

	imx682->reset_gpio = devm_gpiod_get_optional(imx682->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx682->reset_gpio))
		return dev_err_probe(imx682->dev, PTR_ERR(imx682->reset_gpio),
				     "cannot get reset GPIO\n");

	ret = imx682_get_regulators(imx682);
	if (ret)
		return ret;

	ret = imx682_power_on(imx682->dev);
	if (ret)
		return ret;

	ret = imx682_identify_sensor(imx682);
	if (ret)
		goto error_power_off;

	ret = imx682_init_controls(imx682);
	if (ret)
		goto error_power_off;

	imx682->sd.internal_ops = &imx682_internal_ops;
	imx682->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx682->sd.entity.ops = &imx682_subdev_entity_ops;
	imx682->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	imx682->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx682->sd.entity, 1, &imx682->pad);
	if (ret)
		goto error_free_handler;

	imx682->sd.state_lock = imx682->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&imx682->sd);
	if (ret)
		goto error_media_entity;

	pm_runtime_set_active(imx682->dev);
	pm_runtime_enable(imx682->dev);
	pm_runtime_set_autosuspend_delay(imx682->dev, 1000);
	pm_runtime_use_autosuspend(imx682->dev);

	ret = v4l2_async_register_subdev_sensor(&imx682->sd);
	if (ret)
		goto error_pm;

	pm_runtime_idle(imx682->dev);

	return 0;

error_pm:
	pm_runtime_disable(imx682->dev);
	pm_runtime_set_suspended(imx682->dev);
	v4l2_subdev_cleanup(&imx682->sd);
error_media_entity:
	media_entity_cleanup(&imx682->sd.entity);
error_free_handler:
	v4l2_ctrl_handler_free(&imx682->ctrl_handler);
error_power_off:
	imx682_power_off(imx682->dev);

	return ret;
}

static void imx682_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx682 *imx682 = to_imx682(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx682->ctrl_handler);

	pm_runtime_disable(imx682->dev);
	if (!pm_runtime_status_suspended(imx682->dev))
		imx682_power_off(imx682->dev);
	pm_runtime_set_suspended(imx682->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx682_pm_ops, imx682_power_off,
				 imx682_power_on, NULL);

static const struct of_device_id imx682_of_match[] = {
	{ .compatible = "sony,imx682" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx682_of_match);

static struct i2c_driver imx682_i2c_driver = {
	.driver = {
		.name = "imx682",
		.pm = pm_ptr(&imx682_pm_ops),
		.of_match_table = imx682_of_match,
	},
	.probe = imx682_probe,
	.remove = imx682_remove,
};
module_i2c_driver(imx682_i2c_driver);

MODULE_AUTHOR("Roy Kaandorp <roykaandorp@gmail.com>");
MODULE_DESCRIPTION("Sony IMX682 64 Mpixel camera sensor driver");
MODULE_LICENSE("GPL");
