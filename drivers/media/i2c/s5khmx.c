// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5KHMX (ISOCELL Bright HMX) 108 Mpixel camera sensor driver
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

#define S5KHMX_MCLK_FREQ		(19200 * HZ_PER_KHZ)
#define S5KHMX_DATA_LANES		3
#define S5KHMX_BITS_PER_SAMPLE		10

/* Register map follows MIPI CCS, as on the other Samsung parts. */
#define S5KHMX_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5KHMX_CHIP_ID			0x1ad0

#define S5KHMX_REG_CTRL_MODE		CCI_REG8(0x0100)
#define S5KHMX_MODE_STREAMING		BIT(0)

#define S5KHMX_REG_ORIENTATION		CCI_REG8(0x0101)
#define S5KHMX_HFLIP			BIT(0)
#define S5KHMX_VFLIP			BIT(1)

#define S5KHMX_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5KHMX_EXPOSURE_MIN		8
#define S5KHMX_EXPOSURE_STEP		1
/*
 * Exposure is expressed in lines and has to stay below the frame length, with
 * room for the sensor's own readout overhead.
 */
#define S5KHMX_EXPOSURE_MARGIN		22

#define S5KHMX_REG_AGAIN		CCI_REG16(0x0204)
#define S5KHMX_AGAIN_MIN		32
#define S5KHMX_AGAIN_MAX		1024
#define S5KHMX_AGAIN_STEP		1
#define S5KHMX_AGAIN_DEFAULT		32

#define S5KHMX_REG_VTS			CCI_REG16(0x0340)
#define S5KHMX_VTS_MAX			0xfffc

#define S5KHMX_REG_HTS			CCI_REG16(0x0342)

#define S5KHMX_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define to_s5khmx(_sd)			container_of(_sd, struct s5khmx, sd)

/*
 * The two implemented modes run the link at different rates, so the mode
 * carries an index into this menu rather than there being a single frequency.
 */
enum {
	S5KHMX_LINK_FREQ_603MHZ,
};

static const s64 s5khmx_link_freq_menu[] = {
	[S5KHMX_LINK_FREQ_603MHZ] = 603114035,
};

/* Ordered so the flip controls can pick a code by index. */
static const u32 s5khmx_mbus_formats[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
};

static const char * const s5khmx_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Color bars",
	"Fade to grey color bars",
	"PN9",
};

struct s5khmx_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct s5khmx_mode {
	u32 width;
	u32 height;
	u32 hts;			/* Line length in pixels */
	u32 vts;			/* Default frame length in lines */
	u32 link_freq_idx;
	struct s5khmx_reg_list reg_list;
};

struct s5khmx {
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

	const struct s5khmx_mode *mode;
};

/*
 * Unlock sequence: page pointer, firmware version, chip id, then a soft
 * reset. The sensor needs time to settle before it accepts the rest.
 */
static const struct cci_reg_sequence s5khmx_unlock_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0000), 0x0002 },
	{ CCI_REG16(0x0000), 0x1ad0 },
	{ CCI_REG16(0x6010), 0x0001 },
};

/* Global setup, applied once the reset has settled. */
static const struct cci_reg_sequence s5khmx_init_regs[] = {
	{ CCI_REG16(0x6214), 0xff7d },
	{ CCI_REG16(0x6218), 0x0000 },
	{ CCI_REG16(0x0a02), 0x01f4 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0xbc90 },
	{ CCI_REG16(0x6f12), 0x0a49 },
	{ CCI_REG16(0x6f12), 0x10b5 },
	{ CCI_REG16(0x6f12), 0x0848 },
	{ CCI_REG16(0x6f12), 0x0a4a },
	{ CCI_REG16(0x6f12), 0xc1f8 },
	{ CCI_REG16(0x6f12), 0xec06 },
	{ CCI_REG16(0x6f12), 0x101a },
	{ CCI_REG16(0x6f12), 0xa1f8 },
	{ CCI_REG16(0x6f12), 0xf006 },
	{ CCI_REG16(0x6f12), 0xfff7 },
	{ CCI_REG16(0x6f12), 0x2ffe },
	{ CCI_REG16(0x6f12), 0x0748 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x0749 },
	{ CCI_REG16(0x6f12), 0x436f },
	{ CCI_REG16(0x6f12), 0x0748 },
	{ CCI_REG16(0x6f12), 0x9847 },
	{ CCI_REG16(0x6f12), 0x0749 },
	{ CCI_REG16(0x6f12), 0x0860 },
	{ CCI_REG16(0x6f12), 0x10bd },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0xbd54 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x6a70 },
	{ CCI_REG16(0x6f12), 0x2001 },
	{ CCI_REG16(0x6f12), 0xfa00 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x5d20 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0xbce1 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0xb427 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0xbd50 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x70b5 },
	{ CCI_REG16(0x6f12), 0x0d4c },
	{ CCI_REG16(0x6f12), 0x0d4e },
	{ CCI_REG16(0x6f12), 0x2089 },
	{ CCI_REG16(0x6f12), 0x3080 },
	{ CCI_REG16(0x6f12), 0x0d4d },
	{ CCI_REG16(0x6f12), 0x0b49 },
	{ CCI_REG16(0x6f12), 0x4e39 },
	{ CCI_REG16(0x6f12), 0xb5f8 },
	{ CCI_REG16(0x6f12), 0x8803 },
	{ CCI_REG16(0x6f12), 0x0880 },
	{ CCI_REG16(0x6f12), 0x0120 },
	{ CCI_REG16(0x6f12), 0x891e },
	{ CCI_REG16(0x6f12), 0x0880 },
	{ CCI_REG16(0x6f12), 0x2068 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x11f8 },
	{ CCI_REG16(0x6f12), 0x6089 },
	{ CCI_REG16(0x6f12), 0x3080 },
	{ CCI_REG16(0x6f12), 0x6068 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x0cf8 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x0ff8 },
	{ CCI_REG16(0x6f12), 0xc5f8 },
	{ CCI_REG16(0x6f12), 0x8003 },
	{ CCI_REG16(0x6f12), 0x70bd },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0xbd40 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0xf450 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x8fb0 },
	{ CCI_REG16(0x6f12), 0x4bf2 },
	{ CCI_REG16(0x6f12), 0x6b6c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4bf2 },
	{ CCI_REG16(0x6f12), 0x0f6c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x3800 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0600 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0011 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1fc8 },
	{ CCI_REG16(0x6f12), 0x0067 },
	{ CCI_REG16(0x602a), 0x1fd0 },
	{ CCI_REG16(0x6f12), 0x0056 },
	{ CCI_REG16(0x602a), 0x1ffa },
	{ CCI_REG16(0x6f12), 0x0082 },
	{ CCI_REG16(0x602a), 0x2036 },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x6f12), 0x05ea },
	{ CCI_REG16(0x6f12), 0x05fe },
	{ CCI_REG16(0x6f12), 0x0608 },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x6f12), 0x05ea },
	{ CCI_REG16(0x6f12), 0x0608 },
	{ CCI_REG16(0x6f12), 0x05fe },
	{ CCI_REG16(0x6f12), 0x060a },
	{ CCI_REG16(0x6f12), 0x060a },
	{ CCI_REG16(0x6f12), 0x0004 },
	{ CCI_REG16(0x602a), 0x207a },
	{ CCI_REG16(0x6f12), 0x01a8 },
	{ CCI_REG16(0x602a), 0x2138 },
	{ CCI_REG16(0x6f12), 0x005e },
	{ CCI_REG16(0x602a), 0x23c6 },
	{ CCI_REG16(0x6f12), 0x00fc },
	{ CCI_REG16(0x602a), 0x23d8 },
	{ CCI_REG16(0x6f12), 0x035e },
	{ CCI_REG16(0x602a), 0x23b8 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x4480 },
	{ CCI_REG16(0x6f12), 0x530d },
	{ CCI_REG16(0x602a), 0x273c },
	{ CCI_REG16(0x6f12), 0x04d2 },
	{ CCI_REG16(0x602a), 0x3338 },
	{ CCI_REG16(0x6f12), 0x0027 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x602a), 0x363a },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0xf44a), 0x0014 },
	{ CCI_REG16(0xf446), 0x000d },
	{ CCI_REG16(0xf450), 0x0011 },
	{ CCI_REG16(0xf452), 0x002e },
	{ CCI_REG16(0xf65e), 0x3f00 },
	{ CCI_REG16(0x6b76), 0xb000 },
	{ CCI_REG16(0x0bc6), 0x0000 },
	{ CCI_REG16(0x602a), 0x125a },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x3680 },
	{ CCI_REG16(0x6f12), 0x193c },
	{ CCI_REG16(0x602a), 0xba2e },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0xba44 },
	{ CCI_REG16(0x6f12), 0x7ac0 },
	{ CCI_REG16(0x6f12), 0x7ad0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x602a), 0xba54 },
	{ CCI_REG16(0x6f12), 0x0dfb },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x04ae },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6028), 0x2001 },
	{ CCI_REG16(0x602a), 0xe660 },
	{ CCI_REG16(0x6f12), 0x7abd },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0xe666 },
	{ CCI_REG16(0x6f12), 0x0006 },
	{ CCI_REG16(0x6f12), 0x04ba },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x008f },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x6f12), 0x200a },
	{ CCI_REG16(0x6f12), 0x0090 },
	{ CCI_REG16(0x602a), 0xe690 },
	{ CCI_REG16(0x6f12), 0x0090 },
	{ CCI_REG16(0x6f12), 0x0091 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x602a), 0xe6b4 },
	{ CCI_REG16(0x6f12), 0x0092 },
	{ CCI_REG16(0x6f12), 0x2279 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x200a },
	{ CCI_REG16(0x6f12), 0x0134 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x6f12), 0x200a },
	{ CCI_REG16(0x6f12), 0x0134 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x602a), 0xe6d8 },
	{ CCI_REG16(0x6f12), 0x227a },
	{ CCI_REG16(0x6f12), 0x233f },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x200a },
	{ CCI_REG16(0x6f12), 0x00c6 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x3e5c },
	{ CCI_REG16(0x6f12), 0x0141 },
	{ CCI_REG16(0x602a), 0x49ee },
	{ CCI_REG16(0x6f12), 0x0004 },
	{ CCI_REG16(0x602a), 0x49e2 },
	{ CCI_REG16(0x6f12), 0x0008 },
	{ CCI_REG16(0x602a), 0x4b2a },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x602a), 0x4b42 },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x602a), 0x4b5a },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x602a), 0x4b72 },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x602a), 0x4b36 },
	{ CCI_REG16(0x6f12), 0x001c },
	{ CCI_REG16(0x602a), 0x4b4e },
	{ CCI_REG16(0x6f12), 0x001c },
	{ CCI_REG16(0x602a), 0x4b66 },
	{ CCI_REG16(0x6f12), 0x001c },
	{ CCI_REG16(0x602a), 0x4b7e },
	{ CCI_REG16(0x6f12), 0x001c },
	{ CCI_REG16(0x602a), 0x4b96 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x4bae },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x4bc6 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x4bde },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x4ba2 },
	{ CCI_REG16(0x6f12), 0x0014 },
	{ CCI_REG16(0x602a), 0x4bba },
	{ CCI_REG16(0x6f12), 0x0014 },
	{ CCI_REG16(0x602a), 0x4bd2 },
	{ CCI_REG16(0x6f12), 0x0014 },
	{ CCI_REG16(0x602a), 0x4bea },
	{ CCI_REG16(0x6f12), 0x0014 },
};

/* 4x4 binned; the sensible default for preview and stills. */
static const struct cci_reg_sequence s5khmx_3008x2256_regs[] = {
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x0136), 0x1300 },
	{ CCI_REG16(0x013e), 0x00c8 },
	{ CCI_REG16(0x0304), 0x0003 },
	{ CCI_REG16(0x0306), 0x010c },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0302), 0x0003 },
	{ CCI_REG16(0x0300), 0x0002 },
	{ CCI_REG16(0x030e), 0x0003 },
	{ CCI_REG16(0x0310), 0x00bc },
	{ CCI_REG16(0x0312), 0x0002 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0002 },
	{ CCI_REG16(0x602a), 0x23be },
	{ CCI_REG16(0x6f12), 0x0096 },
	{ CCI_REG16(0x0344), 0x0008 },
	{ CCI_REG16(0x0346), 0x0008 },
	{ CCI_REG16(0x0348), 0x2f07 },
	{ CCI_REG16(0x034a), 0x2347 },
	{ CCI_REG16(0x0350), 0x0000 },
	{ CCI_REG16(0x0352), 0x0000 },
	{ CCI_REG16(0x034c), 0x0bc0 },
	{ CCI_REG16(0x034e), 0x08d0 },
	{ CCI_REG16(0x0900), 0x0124 },
	{ CCI_REG16(0x0400), 0x1010 },
	{ CCI_REG16(0x0404), 0x2000 },
	{ CCI_REG16(0x0380), 0x0002 },
	{ CCI_REG16(0x0382), 0x0002 },
	{ CCI_REG16(0x0384), 0x0004 },
	{ CCI_REG16(0x0386), 0x0004 },
	{ CCI_REG16(0x602a), 0x38e6 },
	{ CCI_REG16(0x6f12), 0x0108 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x120e },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x602a), 0x1216 },
	{ CCI_REG16(0x6f12), 0x0e15 },
	{ CCI_REG16(0x602a), 0x4630 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x03b8 },
	{ CCI_REG16(0x0342), 0x3ea0 },
	{ CCI_REG16(0x0340), 0x0945 },
	{ CCI_REG16(0x0114), 0x0201 },
	{ CCI_REG16(0x0118), 0x0001 },
	{ CCI_REG16(0x011c), 0x0101 },
	{ CCI_REG16(0x081c), 0x000c },
	{ CCI_REG16(0x081e), 0x0c00 },
	{ CCI_REG16(0x602a), 0x127a },
	{ CCI_REG16(0x6f12), 0x0300 },
	{ CCI_REG16(0x602a), 0x1f2e },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x602a), 0x39a2 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x602a), 0x3902 },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x0d00), 0x0101 },
	{ CCI_REG16(0x0d02), 0x0101 },
	{ CCI_REG16(0x0d04), 0x0102 },
	{ CCI_REG16(0x602a), 0x4afe },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x4310 },
	{ CCI_REG16(0x6f12), 0x0008 },
	{ CCI_REG16(0x6f12), 0x0402 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x4318 },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x602a), 0x4396 },
	{ CCI_REG16(0x6f12), 0x0077 },
	{ CCI_REG16(0x602a), 0x43c0 },
	{ CCI_REG16(0x6f12), 0x0204 },
	{ CCI_REG16(0x602a), 0x434e },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x2920 },
	{ CCI_REG16(0x6f12), 0x03c0 },
	{ CCI_REG16(0x6f12), 0x06c8 },
	{ CCI_REG16(0x6f12), 0x03d0 },
	{ CCI_REG16(0x6f12), 0x06d8 },
	{ CCI_REG16(0x6f12), 0x03e0 },
	{ CCI_REG16(0x6f12), 0x06e8 },
	{ CCI_REG16(0x6f12), 0x03f0 },
	{ CCI_REG16(0x6f12), 0x06f8 },
	{ CCI_REG16(0x602a), 0x23ce },
	{ CCI_REG16(0x6f12), 0x009c },
	{ CCI_REG16(0x602a), 0x275a },
	{ CCI_REG16(0x6f12), 0x0700 },
	{ CCI_REG16(0x602a), 0x44ec },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x3e36 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x5dd0 },
	{ CCI_REG16(0x6f12), 0x8370 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x8372 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x8374 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x8318 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x831c },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x839e },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x83a2 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x8418 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x841c },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x5f60 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x5f66 },
	{ CCI_REG16(0x6f12), 0x0009 },
	{ CCI_REG16(0x602a), 0x44b2 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x44e4 },
	{ CCI_REG16(0x6f12), 0x0433 },
	{ CCI_REG16(0x6f12), 0x0433 },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x602a), 0x44dc },
	{ CCI_REG16(0x6f12), 0x0040 },
	{ CCI_REG16(0x6f12), 0x0040 },
	{ CCI_REG16(0x6f12), 0x07c0 },
	{ CCI_REG16(0x6f12), 0x07c0 },
	{ CCI_REG16(0x602a), 0x43f2 },
	{ CCI_REG16(0x6f12), 0x0123 },
	{ CCI_REG16(0x602a), 0x43fa },
	{ CCI_REG16(0x6f12), 0x3210 },
	{ CCI_REG16(0x602a), 0x4412 },
	{ CCI_REG16(0x6f12), 0x0123 },
	{ CCI_REG16(0x602a), 0x441a },
	{ CCI_REG16(0x6f12), 0x3210 },
	{ CCI_REG16(0x602a), 0x3f82 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x3af0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x3fff },
	{ CCI_REG16(0x6f12), 0x3fff },
	{ CCI_REG16(0x602a), 0x3972 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x3fff },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x3fff },
	{ CCI_REG16(0x0b08), 0x0101 },
	{ CCI_REG16(0x602a), 0x3954 },
	{ CCI_REG16(0x6f12), 0x0bc0 },
	{ CCI_REG16(0x602a), 0x3958 },
	{ CCI_REG16(0x6f12), 0x08d0 },
	{ CCI_REG16(0x602a), 0xba2c },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0xba42 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0xba30 },
	{ CCI_REG16(0x6f12), 0xf880 },
	{ CCI_REG16(0x6f12), 0xfd80 },
	{ CCI_REG16(0x6028), 0x2001 },
	{ CCI_REG16(0x602a), 0xe664 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0xf472), 0x0010 },
};

static const struct s5khmx_mode s5khmx_supported_modes[] = {
	{
		/* 4x4 binned; the sensible default for preview and stills. */
		.width = 3008,
		.height = 2256,
		.hts = 16032,
		.vts = 2373,
		.link_freq_idx = S5KHMX_LINK_FREQ_603MHZ,
		.reg_list = {
			.regs = s5khmx_3008x2256_regs,
			.num_regs = ARRAY_SIZE(s5khmx_3008x2256_regs),
		},
	},
};

static u64 s5khmx_pixel_rate(const struct s5khmx_mode *mode)
{
	/*
	 * C-PHY moves 16 bits per 7 symbols on each trio, which works out as
	 * 2.28 bits per symbol.
	 */
	return div_u64(s5khmx_link_freq_menu[mode->link_freq_idx] * 228 *
		       S5KHMX_DATA_LANES, 100 * S5KHMX_BITS_PER_SAMPLE);
}

static int s5khmx_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5khmx *s5khmx = container_of(ctrl->handler, struct s5khmx,
					     ctrl_handler);
	const struct s5khmx_mode *mode = s5khmx->mode;
	s64 exposure_max;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Keep the exposure range consistent with the new blanking. */
		exposure_max = mode->height + ctrl->val - S5KHMX_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(s5khmx->exposure,
					 s5khmx->exposure->minimum,
					 exposure_max, s5khmx->exposure->step,
					 min(s5khmx->exposure->val,
					     exposure_max));
	}

	if (!pm_runtime_get_if_in_use(s5khmx->dev))
		return 0;

	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5khmx->regmap, S5KHMX_REG_AGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(s5khmx->regmap, S5KHMX_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(s5khmx->regmap, S5KHMX_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(s5khmx->regmap, S5KHMX_REG_ORIENTATION,
			  (s5khmx->hflip->val ? S5KHMX_HFLIP : 0) |
			  (s5khmx->vflip->val ? S5KHMX_VFLIP : 0), &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(s5khmx->regmap, S5KHMX_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(s5khmx->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5khmx_ctrl_ops = {
	.s_ctrl = s5khmx_set_ctrl,
};

static int s5khmx_init_controls(struct s5khmx *s5khmx)
{
	const struct s5khmx_mode *mode = s5khmx->mode;
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5khmx->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max, hblank, vblank;
	u64 pixel_rate;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 12);
	if (ret)
		return ret;

	s5khmx->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5khmx_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(s5khmx_link_freq_menu) - 1,
					mode->link_freq_idx,
					s5khmx_link_freq_menu);
	if (s5khmx->link_freq)
		s5khmx->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = s5khmx_pixel_rate(mode);
	s5khmx->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       pixel_rate, 1, pixel_rate);
	if (s5khmx->pixel_rate)
		s5khmx->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank = mode->hts - mode->width;
	s5khmx->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (s5khmx->hblank)
		s5khmx->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s5khmx->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   S5KHMX_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5KHMX_AGAIN_MIN, S5KHMX_AGAIN_MAX,
			  S5KHMX_AGAIN_STEP, S5KHMX_AGAIN_DEFAULT);

	exposure_max = mode->vts - S5KHMX_EXPOSURE_MARGIN;
	s5khmx->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5KHMX_EXPOSURE_MIN, exposure_max,
					     S5KHMX_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5khmx_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5khmx_test_pattern_menu) - 1,
				     0, 0, s5khmx_test_pattern_menu);

	s5khmx->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s5khmx->hflip)
		s5khmx->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	s5khmx->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5khmx_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s5khmx->vflip)
		s5khmx->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(s5khmx->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5khmx_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	s5khmx->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int s5khmx_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5khmx *s5khmx = to_s5khmx(sd);
	const struct s5khmx_reg_list *reg_list = &s5khmx->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(s5khmx->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(s5khmx->regmap, s5khmx_unlock_regs,
			    ARRAY_SIZE(s5khmx_unlock_regs), &ret);
	if (ret)
		goto error;

	/* The soft reset in the unlock sequence needs time to settle. */
	usleep_range(15 * USEC_PER_MSEC, 16 * USEC_PER_MSEC);

	cci_multi_reg_write(s5khmx->regmap, s5khmx_init_regs,
			    ARRAY_SIZE(s5khmx_init_regs), &ret);
	cci_multi_reg_write(s5khmx->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s5khmx->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(s5khmx->regmap, S5KHMX_REG_CTRL_MODE,
		  S5KHMX_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s5khmx->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5khmx->dev);

	return ret;
}

static int s5khmx_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5khmx *s5khmx = to_s5khmx(sd);
	int ret;

	ret = cci_write(s5khmx->regmap, S5KHMX_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(s5khmx->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s5khmx->dev);

	return ret;
}

static u32 s5khmx_get_format_code(struct s5khmx *s5khmx)
{
	unsigned int i;

	i = (s5khmx->vflip->val ? 2 : 0) | (s5khmx->hflip->val ? 1 : 0);

	return s5khmx_mbus_formats[i];
}

static void s5khmx_update_pad_format(struct s5khmx *s5khmx,
				     const struct s5khmx_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = s5khmx_get_format_code(s5khmx);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5khmx_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5khmx *s5khmx = to_s5khmx(sd);
	const struct s5khmx_mode *mode;
	struct v4l2_mbus_framefmt *format;
	s64 exposure_max, hblank, vblank;

	mode = v4l2_find_nearest_size(s5khmx_supported_modes,
				      ARRAY_SIZE(s5khmx_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	s5khmx_update_pad_format(s5khmx, mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	s5khmx->mode = mode;

	__v4l2_ctrl_s_ctrl(s5khmx->link_freq, mode->link_freq_idx);
	__v4l2_ctrl_s_ctrl_int64(s5khmx->pixel_rate, s5khmx_pixel_rate(mode));

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(s5khmx->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(s5khmx->vblank, vblank,
				 S5KHMX_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(s5khmx->vblank, vblank);

	exposure_max = mode->vts - S5KHMX_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(s5khmx->exposure, S5KHMX_EXPOSURE_MIN,
				 exposure_max, S5KHMX_EXPOSURE_STEP,
				 exposure_max);

	return 0;
}

static int s5khmx_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct s5khmx *s5khmx = to_s5khmx(sd);

	if (code->index)
		return -EINVAL;

	code->code = s5khmx_get_format_code(s5khmx);

	return 0;
}

static int s5khmx_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5khmx *s5khmx = to_s5khmx(sd);

	if (fse->index >= ARRAY_SIZE(s5khmx_supported_modes))
		return -EINVAL;

	if (fse->code != s5khmx_get_format_code(s5khmx))
		return -EINVAL;

	fse->min_width = s5khmx_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5khmx_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5khmx_get_selection(struct v4l2_subdev *sd,
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
		sel->r.width = s5khmx_supported_modes[1 - 1].width;
		sel->r.height = s5khmx_supported_modes[1 - 1].height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5khmx_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct s5khmx *s5khmx = to_s5khmx(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s5khmx->mode->width,
			.height = s5khmx->mode->height,
		},
	};

	return s5khmx_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5khmx_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5khmx_pad_ops = {
	.set_fmt = s5khmx_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5khmx_get_selection,
	.enum_mbus_code = s5khmx_enum_mbus_code,
	.enum_frame_size = s5khmx_enum_frame_size,
	.enable_streams = s5khmx_enable_streams,
	.disable_streams = s5khmx_disable_streams,
};

static const struct v4l2_subdev_ops s5khmx_subdev_ops = {
	.video = &s5khmx_video_ops,
	.pad = &s5khmx_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5khmx_internal_ops = {
	.init_state = s5khmx_init_state,
};

static const struct media_entity_operations s5khmx_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5khmx_identify_sensor(struct s5khmx *s5khmx)
{
	u64 val;
	int ret;

	/* The chip id lives on page 0x4000. */
	ret = cci_write(s5khmx->regmap, CCI_REG16(0x6028), 0x4000, NULL);
	if (ret)
		return ret;

	ret = cci_read(s5khmx->regmap, S5KHMX_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s5khmx->dev, ret, "failed to read chip id\n");

	if (val != S5KHMX_CHIP_ID)
		return dev_err_probe(s5khmx->dev, -ENODEV,
				     "chip id mismatch: %#x != %#llx\n",
				     S5KHMX_CHIP_ID, val);

	return 0;
}

static int s5khmx_check_hwcfg(struct s5khmx *s5khmx)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5khmx->dev), *ep;
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

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5KHMX_DATA_LANES) {
		dev_err(s5khmx->dev, "invalid number of data trios: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5khmx->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5khmx_link_freq_menu,
				       ARRAY_SIZE(s5khmx_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5khmx_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5khmx *s5khmx = to_s5khmx(sd);
	int ret;

	if (s5khmx->dvdd) {
		ret = regulator_enable(s5khmx->dvdd);
		if (ret)
			return ret;

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
	}

	if (s5khmx->avdd) {
		ret = regulator_enable(s5khmx->avdd);
		if (ret)
			goto disable_dvdd;
	}

	if (s5khmx->dovdd) {
		ret = regulator_enable(s5khmx->dovdd);
		if (ret)
			goto disable_avdd;
	}

	ret = clk_prepare_enable(s5khmx->mclk);
	if (ret)
		goto disable_dovdd;

	gpiod_set_value_cansleep(s5khmx->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	return 0;

disable_dovdd:
	if (s5khmx->dovdd)
		regulator_disable(s5khmx->dovdd);
disable_avdd:
	if (s5khmx->avdd)
		regulator_disable(s5khmx->avdd);
disable_dvdd:
	if (s5khmx->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(s5khmx->dvdd);
	}

	return ret;
}

static int s5khmx_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5khmx *s5khmx = to_s5khmx(sd);

	gpiod_set_value_cansleep(s5khmx->reset_gpio, 1);
	clk_disable_unprepare(s5khmx->mclk);

	if (s5khmx->dovdd)
		regulator_disable(s5khmx->dovdd);
	if (s5khmx->avdd)
		regulator_disable(s5khmx->avdd);
	if (s5khmx->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(s5khmx->dvdd);
	}

	return 0;
}

static int s5khmx_get_regulators(struct s5khmx *s5khmx)
{
	static const char * const names[] = { "avdd", "dvdd", "dovdd" };
	struct regulator **targets[] = {
		&s5khmx->avdd, &s5khmx->dvdd, &s5khmx->dovdd,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct regulator *reg;

		reg = devm_regulator_get_optional(s5khmx->dev, names[i]);
		if (IS_ERR(reg)) {
			if (PTR_ERR(reg) != -ENODEV)
				return dev_err_probe(s5khmx->dev, PTR_ERR(reg),
						     "failed to get %s\n",
						     names[i]);
			reg = NULL;
		}
		*targets[i] = reg;
	}

	return 0;
}

static int s5khmx_probe(struct i2c_client *client)
{
	struct s5khmx *s5khmx;
	unsigned long freq;
	int ret;

	s5khmx = devm_kzalloc(&client->dev, sizeof(*s5khmx), GFP_KERNEL);
	if (!s5khmx)
		return -ENOMEM;

	s5khmx->dev = &client->dev;
	s5khmx->mode = &s5khmx_supported_modes[0];
	v4l2_i2c_subdev_init(&s5khmx->sd, client, &s5khmx_subdev_ops);

	s5khmx->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5khmx->regmap))
		return dev_err_probe(s5khmx->dev, PTR_ERR(s5khmx->regmap),
				     "failed to init CCI\n");

	s5khmx->mclk = devm_v4l2_sensor_clk_get(s5khmx->dev, NULL);
	if (IS_ERR(s5khmx->mclk))
		return dev_err_probe(s5khmx->dev, PTR_ERR(s5khmx->mclk),
				     "failed to get MCLK\n");

	freq = clk_get_rate(s5khmx->mclk);
	if (freq != S5KHMX_MCLK_FREQ)
		return dev_err_probe(s5khmx->dev, -EINVAL,
				     "MCLK frequency %lu not supported\n", freq);

	ret = s5khmx_check_hwcfg(s5khmx);
	if (ret)
		return ret;

	s5khmx->reset_gpio = devm_gpiod_get_optional(s5khmx->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5khmx->reset_gpio))
		return dev_err_probe(s5khmx->dev, PTR_ERR(s5khmx->reset_gpio),
				     "cannot get reset GPIO\n");

	ret = s5khmx_get_regulators(s5khmx);
	if (ret)
		return ret;

	ret = s5khmx_power_on(s5khmx->dev);
	if (ret)
		return ret;

	ret = s5khmx_identify_sensor(s5khmx);
	if (ret)
		goto error_power_off;

	ret = s5khmx_init_controls(s5khmx);
	if (ret)
		goto error_power_off;

	s5khmx->sd.internal_ops = &s5khmx_internal_ops;
	s5khmx->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5khmx->sd.entity.ops = &s5khmx_subdev_entity_ops;
	s5khmx->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	s5khmx->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s5khmx->sd.entity, 1, &s5khmx->pad);
	if (ret)
		goto error_free_handler;

	s5khmx->sd.state_lock = s5khmx->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&s5khmx->sd);
	if (ret)
		goto error_media_entity;

	pm_runtime_set_active(s5khmx->dev);
	pm_runtime_enable(s5khmx->dev);
	pm_runtime_set_autosuspend_delay(s5khmx->dev, 1000);
	pm_runtime_use_autosuspend(s5khmx->dev);

	ret = v4l2_async_register_subdev_sensor(&s5khmx->sd);
	if (ret)
		goto error_pm;

	pm_runtime_idle(s5khmx->dev);

	return 0;

error_pm:
	pm_runtime_disable(s5khmx->dev);
	pm_runtime_set_suspended(s5khmx->dev);
	v4l2_subdev_cleanup(&s5khmx->sd);
error_media_entity:
	media_entity_cleanup(&s5khmx->sd.entity);
error_free_handler:
	v4l2_ctrl_handler_free(&s5khmx->ctrl_handler);
error_power_off:
	s5khmx_power_off(s5khmx->dev);

	return ret;
}

static void s5khmx_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5khmx *s5khmx = to_s5khmx(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s5khmx->ctrl_handler);

	pm_runtime_disable(s5khmx->dev);
	if (!pm_runtime_status_suspended(s5khmx->dev))
		s5khmx_power_off(s5khmx->dev);
	pm_runtime_set_suspended(s5khmx->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(s5khmx_pm_ops, s5khmx_power_off,
				 s5khmx_power_on, NULL);

static const struct of_device_id s5khmx_of_match[] = {
	{ .compatible = "samsung,s5khmx" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5khmx_of_match);

static struct i2c_driver s5khmx_i2c_driver = {
	.driver = {
		.name = "s5khmx",
		.pm = pm_ptr(&s5khmx_pm_ops),
		.of_match_table = s5khmx_of_match,
	},
	.probe = s5khmx_probe,
	.remove = s5khmx_remove,
};
module_i2c_driver(s5khmx_i2c_driver);

MODULE_AUTHOR("Roy Kaandorp <roykaandorp@gmail.com>");
MODULE_DESCRIPTION("Samsung S5KHMX (ISOCELL Bright HMX) 108 Mpixel camera sensor driver");
MODULE_LICENSE("GPL");
