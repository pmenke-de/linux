// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K3T2 20 Mpixel camera sensor driver
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

#define S5K3T2_MCLK_FREQ		(19200 * HZ_PER_KHZ)
#define S5K3T2_DATA_LANES		4
#define S5K3T2_BITS_PER_SAMPLE		10

/* Register map follows MIPI CCS, as on the other Samsung parts. */
#define S5K3T2_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K3T2_CHIP_ID			0x3142

#define S5K3T2_REG_CTRL_MODE		CCI_REG8(0x0100)
#define S5K3T2_MODE_STREAMING		BIT(0)

#define S5K3T2_REG_ORIENTATION		CCI_REG8(0x0101)
#define S5K3T2_HFLIP			BIT(0)
#define S5K3T2_VFLIP			BIT(1)

#define S5K3T2_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K3T2_EXPOSURE_MIN		8
#define S5K3T2_EXPOSURE_STEP		1
/*
 * Exposure is expressed in lines and has to stay below the frame length, with
 * room for the sensor's own readout overhead.
 */
#define S5K3T2_EXPOSURE_MARGIN		22

#define S5K3T2_REG_AGAIN		CCI_REG16(0x0204)
#define S5K3T2_AGAIN_MIN		32
#define S5K3T2_AGAIN_MAX		1024
#define S5K3T2_AGAIN_STEP		1
#define S5K3T2_AGAIN_DEFAULT		32

#define S5K3T2_REG_VTS			CCI_REG16(0x0340)
#define S5K3T2_VTS_MAX			0xfffc

#define S5K3T2_REG_HTS			CCI_REG16(0x0342)

#define S5K3T2_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define to_s5k3t2(_sd)			container_of(_sd, struct s5k3t2, sd)

/*
 * The two implemented modes run the link at different rates, so the mode
 * carries an index into this menu rather than there being a single frequency.
 */
enum {
	S5K3T2_LINK_FREQ_248MHZ,
	S5K3T2_LINK_FREQ_716MHZ,
	S5K3T2_LINK_FREQ_240MHZ,
};

static const s64 s5k3t2_link_freq_menu[] = {
	[S5K3T2_LINK_FREQ_248MHZ] = 248320000,
	[S5K3T2_LINK_FREQ_716MHZ] = 716800000,
	[S5K3T2_LINK_FREQ_240MHZ] = 240000000,
};

/* Ordered so the flip controls can pick a code by index. */
static const u32 s5k3t2_mbus_formats[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
};

static const char * const s5k3t2_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Color bars",
	"Fade to grey color bars",
	"PN9",
};

struct s5k3t2_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct s5k3t2_mode {
	u32 width;
	u32 height;
	u32 hts;			/* Line length in pixels */
	u32 vts;			/* Default frame length in lines */
	u32 link_freq_idx;
	struct s5k3t2_reg_list reg_list;
};

struct s5k3t2 {
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

	const struct s5k3t2_mode *mode;
};

/*
 * Unlock sequence: page pointer, firmware version, chip id, then a soft
 * reset. The sensor needs time to settle before it accepts the rest.
 */
static const struct cci_reg_sequence s5k3t2_unlock_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0000), 0x0005 },
	{ CCI_REG16(0x0000), 0x3142 },
	{ CCI_REG16(0x6010), 0x0001 },
};

/* Global setup, applied once the reset has settled. */
static const struct cci_reg_sequence s5k3t2_init_regs[] = {
	{ CCI_REG16(0x6214), 0xff7d },
	{ CCI_REG16(0x6218), 0x0000 },
	{ CCI_REG16(0x0a02), 0x003f },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x3aec },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0549 },
	{ CCI_REG16(0x6f12), 0x0448 },
	{ CCI_REG16(0x6f12), 0x054a },
	{ CCI_REG16(0x6f12), 0xc1f8 },
	{ CCI_REG16(0x6f12), 0x2c05 },
	{ CCI_REG16(0x6f12), 0x101a },
	{ CCI_REG16(0x6f12), 0xa1f8 },
	{ CCI_REG16(0x6f12), 0x3005 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x7bb8 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3ca0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2670 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x9c00 },
	{ CCI_REG16(0x6f12), 0x70b5 },
	{ CCI_REG16(0x6f12), 0x0646 },
	{ CCI_REG16(0x6f12), 0x4348 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x0168 },
	{ CCI_REG16(0x6f12), 0x0c0c },
	{ CCI_REG16(0x6f12), 0x8db2 },
	{ CCI_REG16(0x6f12), 0x2946 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x9bf8 },
	{ CCI_REG16(0x6f12), 0x3046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x9df8 },
	{ CCI_REG16(0x6f12), 0x3d48 },
	{ CCI_REG16(0x6f12), 0x3e4a },
	{ CCI_REG16(0x6f12), 0x0830 },
	{ CCI_REG16(0x6f12), 0x0188 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x911c },
	{ CCI_REG16(0x6f12), 0x4088 },
	{ CCI_REG16(0x6f12), 0x0880 },
	{ CCI_REG16(0x6f12), 0x2946 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0x7040 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x89b8 },
	{ CCI_REG16(0x6f12), 0x70b5 },
	{ CCI_REG16(0x6f12), 0x0646 },
	{ CCI_REG16(0x6f12), 0x3548 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x4068 },
	{ CCI_REG16(0x6f12), 0x84b2 },
	{ CCI_REG16(0x6f12), 0x050c },
	{ CCI_REG16(0x6f12), 0x2146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x7ef8 },
	{ CCI_REG16(0x6f12), 0x3046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x85f8 },
	{ CCI_REG16(0x6f12), 0x3149 },
	{ CCI_REG16(0x6f12), 0x314b },
	{ CCI_REG16(0x6f12), 0x0120 },
	{ CCI_REG16(0x6f12), 0x0988 },
	{ CCI_REG16(0x6f12), 0x40ea },
	{ CCI_REG16(0x6f12), 0x0110 },
	{ CCI_REG16(0x6f12), 0x5881 },
	{ CCI_REG16(0x6f12), 0x2f48 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x6f12), 0x68b1 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x2e48 },
	{ CCI_REG16(0x6f12), 0x2f49 },
	{ CCI_REG16(0x6f12), 0x80f8 },
	{ CCI_REG16(0x6f12), 0xc428 },
	{ CCI_REG16(0x6f12), 0x0a80 },
	{ CCI_REG16(0x6f12), 0x2e49 },
	{ CCI_REG16(0x6f12), 0x0e78 },
	{ CCI_REG16(0x6f12), 0x36b1 },
	{ CCI_REG16(0x6f12), 0x90f8 },
	{ CCI_REG16(0x6f12), 0xe803 },
	{ CCI_REG16(0x6f12), 0x18b1 },
	{ CCI_REG16(0x6f12), 0x0120 },
	{ CCI_REG16(0x6f12), 0x02e0 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0xf0e7 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x6f12), 0x4978 },
	{ CCI_REG16(0x6f12), 0x01b1 },
	{ CCI_REG16(0x6f12), 0x42b1 },
	{ CCI_REG16(0x6f12), 0x0021 },
	{ CCI_REG16(0x6f12), 0x40ea },
	{ CCI_REG16(0x6f12), 0x0110 },
	{ CCI_REG16(0x6f12), 0x5880 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x66f8 },
	{ CCI_REG16(0x6f12), 0x0128 },
	{ CCI_REG16(0x6f12), 0x02d0 },
	{ CCI_REG16(0x6f12), 0x1be0 },
	{ CCI_REG16(0x6f12), 0x0121 },
	{ CCI_REG16(0x6f12), 0xf5e7 },
	{ CCI_REG16(0x6f12), 0x2248 },
	{ CCI_REG16(0x6f12), 0x234a },
	{ CCI_REG16(0x6f12), 0x234e },
	{ CCI_REG16(0x6f12), 0xb0f8 },
	{ CCI_REG16(0x6f12), 0x7211 },
	{ CCI_REG16(0x6f12), 0x92f8 },
	{ CCI_REG16(0x6f12), 0x9420 },
	{ CCI_REG16(0x6f12), 0x90f8 },
	{ CCI_REG16(0x6f12), 0x7401 },
	{ CCI_REG16(0x6f12), 0xd140 },
	{ CCI_REG16(0x6f12), 0xd040 },
	{ CCI_REG16(0x6f12), 0x4318 },
	{ CCI_REG16(0x6f12), 0x96f8 },
	{ CCI_REG16(0x6f12), 0x7f63 },
	{ CCI_REG16(0x6f12), 0x581e },
	{ CCI_REG16(0x6f12), 0x022e },
	{ CCI_REG16(0x6f12), 0x03d9 },
	{ CCI_REG16(0x6f12), 0x0220 },
	{ CCI_REG16(0x6f12), 0x9040 },
	{ CCI_REG16(0x6f12), 0x181a },
	{ CCI_REG16(0x6f12), 0x401c },
	{ CCI_REG16(0x6f12), 0x164a },
	{ CCI_REG16(0x6f12), 0x703a },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x911c },
	{ CCI_REG16(0x6f12), 0x0880 },
	{ CCI_REG16(0x6f12), 0x2146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0x7040 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x31b8 },
	{ CCI_REG16(0x6f12), 0x10b5 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xb501 },
	{ CCI_REG16(0x6f12), 0x1348 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x3ef8 },
	{ CCI_REG16(0x6f12), 0x064c },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xff01 },
	{ CCI_REG16(0x6f12), 0x6060 },
	{ CCI_REG16(0x6f12), 0x1048 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x36f8 },
	{ CCI_REG16(0x6f12), 0x0f49 },
	{ CCI_REG16(0x6f12), 0x2060 },
	{ CCI_REG16(0x6f12), 0x7a20 },
	{ CCI_REG16(0x6f12), 0x0968 },
	{ CCI_REG16(0x6f12), 0x4883 },
	{ CCI_REG16(0x6f12), 0x10bd },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3c90 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0x950c },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x16f0 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0xd000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x19a0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x30c0 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0x9800 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x1dd0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x17c0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2210 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2670 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xf45f },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xd957 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x08c0 },
	{ CCI_REG16(0x6f12), 0x49f6 },
	{ CCI_REG16(0x6f12), 0x213c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4df6 },
	{ CCI_REG16(0x6f12), 0x571c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4ff2 },
	{ CCI_REG16(0x6f12), 0x5f4c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x44f2 },
	{ CCI_REG16(0x6f12), 0x9b0c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4bf2 },
	{ CCI_REG16(0x6f12), 0xed0c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1014 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x7000 },
	{ CCI_REG16(0x602a), 0x108a },
	{ CCI_REG16(0x6f12), 0x0006 },
	{ CCI_REG16(0x602a), 0x1092 },
	{ CCI_REG16(0x6f12), 0x0005 },
	{ CCI_REG16(0x602a), 0x1096 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x6f12), 0x001a },
	{ CCI_REG16(0x602a), 0x109c },
	{ CCI_REG16(0x6f12), 0x0014 },
	{ CCI_REG16(0x602a), 0x10a2 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x602a), 0x10ae },
	{ CCI_REG16(0x6f12), 0x0007 },
	{ CCI_REG16(0x602a), 0x10c2 },
	{ CCI_REG16(0x6f12), 0x001e },
	{ CCI_REG16(0x602a), 0x10f4 },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x602a), 0x110a },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x113e },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x014e },
	{ CCI_REG16(0x602a), 0x13ea },
	{ CCI_REG16(0x6f12), 0x160f },
	{ CCI_REG16(0x6f12), 0x0d00 },
	{ CCI_REG16(0x602a), 0x13fa },
	{ CCI_REG16(0x6f12), 0x009d },
	{ CCI_REG16(0x6f12), 0x0107 },
	{ CCI_REG16(0x602a), 0x14e2 },
	{ CCI_REG16(0x6f12), 0x04c2 },
	{ CCI_REG16(0x6f12), 0x02ae },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0xf44a), 0x0010 },
	{ CCI_REG16(0xf46a), 0xb6a0 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0f5e },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x0f90 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x17c0 },
	{ CCI_REG16(0x6f12), 0x0010 },
	{ CCI_REG16(0x6f12), 0x0201 },
	{ CCI_REG16(0x602a), 0x1da2 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x0203 },
	{ CCI_REG16(0x6f12), 0x0405 },
	{ CCI_REG16(0x6f12), 0x0607 },
	{ CCI_REG16(0x6f12), 0x0809 },
	{ CCI_REG16(0x6f12), 0x0a0b },
	{ CCI_REG16(0x6f12), 0x0c0d },
	{ CCI_REG16(0x6f12), 0x0e0f },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x020c), 0x0001 },
	{ CCI_REG16(0x0bc6), 0x0000 },
	{ CCI_REG16(0x0d00), 0x0000 },
	{ CCI_REG16(0xb13c), 0x0800 },
};

/* 2x2 binned; the default for preview and video calls. */
static const struct cci_reg_sequence s5k3t2_2592x1940_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0136), 0x1300 },
	{ CCI_REG16(0x013e), 0x00c8 },
	{ CCI_REG16(0x0304), 0x0003 },
	{ CCI_REG16(0x0306), 0x00b9 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0e0e },
	{ CCI_REG16(0x6f12), 0x0105 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0300), 0x0007 },
	{ CCI_REG16(0x030e), 0x0003 },
	{ CCI_REG16(0x0310), 0x00c2 },
	{ CCI_REG16(0x0312), 0x0002 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x0344), 0x0008 },
	{ CCI_REG16(0x0346), 0x0008 },
	{ CCI_REG16(0x0348), 0x1447 },
	{ CCI_REG16(0x034a), 0x0f2f },
	{ CCI_REG16(0x034c), 0x0a20 },
	{ CCI_REG16(0x034e), 0x0794 },
	{ CCI_REG16(0x0350), 0x0000 },
	{ CCI_REG16(0x0352), 0x0000 },
	{ CCI_REG16(0x0900), 0x0122 },
	{ CCI_REG16(0x0404), 0x1000 },
	{ CCI_REG16(0x0380), 0x0002 },
	{ CCI_REG16(0x0382), 0x0002 },
	{ CCI_REG16(0x0384), 0x0002 },
	{ CCI_REG16(0x0386), 0x0002 },
	{ CCI_REG16(0x0342), 0x2a80 },
	{ CCI_REG16(0x0340), 0x0810 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x010c), 0x0000 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0116), 0x3000 },
	{ CCI_REG16(0x011a), 0x0001 },
	{ CCI_REG16(0x0118), 0x0002 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0e92 },
	{ CCI_REG16(0x6f12), 0xffff },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0b04), 0x0001 },
	{ CCI_REG16(0x0b06), 0x0101 },
	{ CCI_REG16(0x0fea), 0x04a0 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x3c98 },
	{ CCI_REG16(0x6f12), 0x052c },
	{ CCI_REG16(0x6f12), 0x0a3b },
	{ CCI_REG16(0x602a), 0x1da0 },
	{ CCI_REG16(0x6f12), 0x0010 },
	{ CCI_REG16(0x602a), 0x10ac },
	{ CCI_REG16(0x6f12), 0x0014 },
	{ CCI_REG16(0x602a), 0x1110 },
	{ CCI_REG16(0x6f12), 0x001d },
	{ CCI_REG16(0x6f12), 0x004d },
	{ CCI_REG16(0x602a), 0x13e8 },
	{ CCI_REG16(0x6f12), 0x080f },
	{ CCI_REG16(0x602a), 0x13f8 },
	{ CCI_REG16(0x6f12), 0x38c8 },
};

/* Full 20 Mpixel readout. */
static const struct cci_reg_sequence s5k3t2_5184x3880_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0136), 0x1300 },
	{ CCI_REG16(0x013e), 0x00c8 },
	{ CCI_REG16(0x0304), 0x0003 },
	{ CCI_REG16(0x0306), 0x00b9 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0e0e },
	{ CCI_REG16(0x6f12), 0x0103 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1078 },
	{ CCI_REG16(0x6f12), 0x04a6 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0300), 0x0007 },
	{ CCI_REG16(0x030e), 0x0003 },
	{ CCI_REG16(0x0310), 0x008c },
	{ CCI_REG16(0x0312), 0x0000 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x0344), 0x0008 },
	{ CCI_REG16(0x0346), 0x0008 },
	{ CCI_REG16(0x0348), 0x1447 },
	{ CCI_REG16(0x034a), 0x0f2f },
	{ CCI_REG16(0x034c), 0x1440 },
	{ CCI_REG16(0x034e), 0x0f28 },
	{ CCI_REG16(0x0350), 0x0000 },
	{ CCI_REG16(0x0352), 0x0000 },
	{ CCI_REG16(0x0900), 0x0111 },
	{ CCI_REG16(0x0404), 0x1000 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0001 },
	{ CCI_REG16(0x0342), 0x1540 },
	{ CCI_REG16(0x0340), 0x1000 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x010c), 0x0000 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0116), 0x3000 },
	{ CCI_REG16(0x011a), 0x0001 },
	{ CCI_REG16(0x0118), 0x0002 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0e92 },
	{ CCI_REG16(0x6f12), 0xffff },
	{ CCI_REG16(0x602a), 0x165a },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0b04), 0x0001 },
	{ CCI_REG16(0x0b06), 0x0101 },
	{ CCI_REG16(0x0fea), 0x04a0 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x3c98 },
	{ CCI_REG16(0x6f12), 0x0a58 },
	{ CCI_REG16(0x6f12), 0x1477 },
	{ CCI_REG16(0x602a), 0x1da0 },
	{ CCI_REG16(0x6f12), 0x0010 },
	{ CCI_REG16(0x602a), 0x10ac },
	{ CCI_REG16(0x6f12), 0x000a },
	{ CCI_REG16(0x602a), 0x1110 },
	{ CCI_REG16(0x6f12), 0x001d },
	{ CCI_REG16(0x6f12), 0x003f },
	{ CCI_REG16(0x602a), 0x13e8 },
	{ CCI_REG16(0x6f12), 0x0804 },
	{ CCI_REG16(0x602a), 0x13f8 },
	{ CCI_REG16(0x6f12), 0x38c8 },
};

/* 720p, cropped and subsampled. */
static const struct cci_reg_sequence s5k3t2_1280x720_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0136), 0x1300 },
	{ CCI_REG16(0x013e), 0x00c8 },
	{ CCI_REG16(0x0304), 0x0003 },
	{ CCI_REG16(0x0306), 0x00b9 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0e0e },
	{ CCI_REG16(0x6f12), 0x0104 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1078 },
	{ CCI_REG16(0x6f12), 0x04a6 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0300), 0x0007 },
	{ CCI_REG16(0x030e), 0x0002 },
	{ CCI_REG16(0x0310), 0x007d },
	{ CCI_REG16(0x0312), 0x0002 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x0344), 0x0028 },
	{ CCI_REG16(0x0346), 0x01fc },
	{ CCI_REG16(0x0348), 0x1427 },
	{ CCI_REG16(0x034a), 0x0d3b },
	{ CCI_REG16(0x034c), 0x0500 },
	{ CCI_REG16(0x034e), 0x02d0 },
	{ CCI_REG16(0x0350), 0x0000 },
	{ CCI_REG16(0x0352), 0x0000 },
	{ CCI_REG16(0x0900), 0x0124 },
	{ CCI_REG16(0x0404), 0x2000 },
	{ CCI_REG16(0x0380), 0x0002 },
	{ CCI_REG16(0x0382), 0x0002 },
	{ CCI_REG16(0x0384), 0x0002 },
	{ CCI_REG16(0x0386), 0x0006 },
	{ CCI_REG16(0x0342), 0x1620 },
	{ CCI_REG16(0x0340), 0x03e0 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x010c), 0x0000 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0116), 0x3000 },
	{ CCI_REG16(0x011a), 0x0001 },
	{ CCI_REG16(0x0118), 0x0002 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x0e92 },
	{ CCI_REG16(0x6f12), 0xffff },
	{ CCI_REG16(0x602a), 0x165a },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0b04), 0x0001 },
	{ CCI_REG16(0x0b06), 0x0101 },
	{ CCI_REG16(0x0fea), 0x04a0 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x3c98 },
	{ CCI_REG16(0x6f12), 0x0296 },
	{ CCI_REG16(0x6f12), 0x0515 },
	{ CCI_REG16(0x602a), 0x1da0 },
	{ CCI_REG16(0x6f12), 0x0110 },
	{ CCI_REG16(0x602a), 0x10ac },
	{ CCI_REG16(0x6f12), 0x0014 },
	{ CCI_REG16(0x602a), 0x1110 },
	{ CCI_REG16(0x6f12), 0x001d },
	{ CCI_REG16(0x6f12), 0x004d },
	{ CCI_REG16(0x602a), 0x13e8 },
	{ CCI_REG16(0x6f12), 0x080f },
	{ CCI_REG16(0x602a), 0x13f8 },
	{ CCI_REG16(0x6f12), 0x38c8 },
};

static const struct s5k3t2_mode s5k3t2_supported_modes[] = {
	{
		/* 2x2 binned; the default for preview and video calls. */
		.width = 2592,
		.height = 1940,
		.hts = 10880,
		.vts = 2064,
		.link_freq_idx = S5K3T2_LINK_FREQ_248MHZ,
		.reg_list = {
			.regs = s5k3t2_2592x1940_regs,
			.num_regs = ARRAY_SIZE(s5k3t2_2592x1940_regs),
		},
	},
	{
		/* Full 20 Mpixel readout. */
		.width = 5184,
		.height = 3880,
		.hts = 5440,
		.vts = 4096,
		.link_freq_idx = S5K3T2_LINK_FREQ_716MHZ,
		.reg_list = {
			.regs = s5k3t2_5184x3880_regs,
			.num_regs = ARRAY_SIZE(s5k3t2_5184x3880_regs),
		},
	},
	{
		/* 720p, cropped and subsampled. */
		.width = 1280,
		.height = 720,
		.hts = 5664,
		.vts = 992,
		.link_freq_idx = S5K3T2_LINK_FREQ_240MHZ,
		.reg_list = {
			.regs = s5k3t2_1280x720_regs,
			.num_regs = ARRAY_SIZE(s5k3t2_1280x720_regs),
		},
	},
};

static u64 s5k3t2_pixel_rate(const struct s5k3t2_mode *mode)
{
	return div_u64(s5k3t2_link_freq_menu[mode->link_freq_idx] * 2 *
		       S5K3T2_DATA_LANES, S5K3T2_BITS_PER_SAMPLE);
}

static int s5k3t2_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k3t2 *s5k3t2 = container_of(ctrl->handler, struct s5k3t2,
					     ctrl_handler);
	const struct s5k3t2_mode *mode = s5k3t2->mode;
	s64 exposure_max;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Keep the exposure range consistent with the new blanking. */
		exposure_max = mode->height + ctrl->val - S5K3T2_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(s5k3t2->exposure,
					 s5k3t2->exposure->minimum,
					 exposure_max, s5k3t2->exposure->step,
					 min(s5k3t2->exposure->val,
					     exposure_max));
	}

	if (!pm_runtime_get_if_in_use(s5k3t2->dev))
		return 0;

	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5k3t2->regmap, S5K3T2_REG_AGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(s5k3t2->regmap, S5K3T2_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(s5k3t2->regmap, S5K3T2_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(s5k3t2->regmap, S5K3T2_REG_ORIENTATION,
			  (s5k3t2->hflip->val ? S5K3T2_HFLIP : 0) |
			  (s5k3t2->vflip->val ? S5K3T2_VFLIP : 0), &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(s5k3t2->regmap, S5K3T2_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(s5k3t2->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k3t2_ctrl_ops = {
	.s_ctrl = s5k3t2_set_ctrl,
};

static int s5k3t2_init_controls(struct s5k3t2 *s5k3t2)
{
	const struct s5k3t2_mode *mode = s5k3t2->mode;
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5k3t2->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max, hblank, vblank;
	u64 pixel_rate;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 12);
	if (ret)
		return ret;

	s5k3t2->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5k3t2_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(s5k3t2_link_freq_menu) - 1,
					mode->link_freq_idx,
					s5k3t2_link_freq_menu);
	if (s5k3t2->link_freq)
		s5k3t2->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = s5k3t2_pixel_rate(mode);
	s5k3t2->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       pixel_rate, 1, pixel_rate);
	if (s5k3t2->pixel_rate)
		s5k3t2->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank = mode->hts - mode->width;
	s5k3t2->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (s5k3t2->hblank)
		s5k3t2->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s5k3t2->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   S5K3T2_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K3T2_AGAIN_MIN, S5K3T2_AGAIN_MAX,
			  S5K3T2_AGAIN_STEP, S5K3T2_AGAIN_DEFAULT);

	exposure_max = mode->vts - S5K3T2_EXPOSURE_MARGIN;
	s5k3t2->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K3T2_EXPOSURE_MIN, exposure_max,
					     S5K3T2_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k3t2_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k3t2_test_pattern_menu) - 1,
				     0, 0, s5k3t2_test_pattern_menu);

	s5k3t2->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s5k3t2->hflip)
		s5k3t2->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	s5k3t2->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k3t2_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s5k3t2->vflip)
		s5k3t2->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(s5k3t2->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k3t2_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	s5k3t2->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int s5k3t2_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);
	const struct s5k3t2_reg_list *reg_list = &s5k3t2->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(s5k3t2->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(s5k3t2->regmap, s5k3t2_unlock_regs,
			    ARRAY_SIZE(s5k3t2_unlock_regs), &ret);
	if (ret)
		goto error;

	/* The soft reset in the unlock sequence needs time to settle. */
	usleep_range(15 * USEC_PER_MSEC, 16 * USEC_PER_MSEC);

	cci_multi_reg_write(s5k3t2->regmap, s5k3t2_init_regs,
			    ARRAY_SIZE(s5k3t2_init_regs), &ret);
	cci_multi_reg_write(s5k3t2->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s5k3t2->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(s5k3t2->regmap, S5K3T2_REG_CTRL_MODE,
		  S5K3T2_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s5k3t2->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5k3t2->dev);

	return ret;
}

static int s5k3t2_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);
	int ret;

	ret = cci_write(s5k3t2->regmap, S5K3T2_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(s5k3t2->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s5k3t2->dev);

	return ret;
}

static u32 s5k3t2_get_format_code(struct s5k3t2 *s5k3t2)
{
	unsigned int i;

	i = (s5k3t2->vflip->val ? 2 : 0) | (s5k3t2->hflip->val ? 1 : 0);

	return s5k3t2_mbus_formats[i];
}

static void s5k3t2_update_pad_format(struct s5k3t2 *s5k3t2,
				     const struct s5k3t2_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = s5k3t2_get_format_code(s5k3t2);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k3t2_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);
	const struct s5k3t2_mode *mode;
	struct v4l2_mbus_framefmt *format;
	s64 exposure_max, hblank, vblank;

	mode = v4l2_find_nearest_size(s5k3t2_supported_modes,
				      ARRAY_SIZE(s5k3t2_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	s5k3t2_update_pad_format(s5k3t2, mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	s5k3t2->mode = mode;

	__v4l2_ctrl_s_ctrl(s5k3t2->link_freq, mode->link_freq_idx);
	__v4l2_ctrl_s_ctrl_int64(s5k3t2->pixel_rate, s5k3t2_pixel_rate(mode));

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(s5k3t2->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(s5k3t2->vblank, vblank,
				 S5K3T2_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(s5k3t2->vblank, vblank);

	exposure_max = mode->vts - S5K3T2_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(s5k3t2->exposure, S5K3T2_EXPOSURE_MIN,
				 exposure_max, S5K3T2_EXPOSURE_STEP,
				 exposure_max);

	return 0;
}

static int s5k3t2_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);

	if (code->index)
		return -EINVAL;

	code->code = s5k3t2_get_format_code(s5k3t2);

	return 0;
}

static int s5k3t2_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);

	if (fse->index >= ARRAY_SIZE(s5k3t2_supported_modes))
		return -EINVAL;

	if (fse->code != s5k3t2_get_format_code(s5k3t2))
		return -EINVAL;

	fse->min_width = s5k3t2_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k3t2_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k3t2_get_selection(struct v4l2_subdev *sd,
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
		sel->r.width = s5k3t2_supported_modes[3 - 1].width;
		sel->r.height = s5k3t2_supported_modes[3 - 1].height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5k3t2_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s5k3t2->mode->width,
			.height = s5k3t2->mode->height,
		},
	};

	return s5k3t2_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k3t2_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k3t2_pad_ops = {
	.set_fmt = s5k3t2_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5k3t2_get_selection,
	.enum_mbus_code = s5k3t2_enum_mbus_code,
	.enum_frame_size = s5k3t2_enum_frame_size,
	.enable_streams = s5k3t2_enable_streams,
	.disable_streams = s5k3t2_disable_streams,
};

static const struct v4l2_subdev_ops s5k3t2_subdev_ops = {
	.video = &s5k3t2_video_ops,
	.pad = &s5k3t2_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k3t2_internal_ops = {
	.init_state = s5k3t2_init_state,
};

static const struct media_entity_operations s5k3t2_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k3t2_identify_sensor(struct s5k3t2 *s5k3t2)
{
	u64 val;
	int ret;

	/* The chip id lives on page 0x4000. */
	ret = cci_write(s5k3t2->regmap, CCI_REG16(0x6028), 0x4000, NULL);
	if (ret)
		return ret;

	ret = cci_read(s5k3t2->regmap, S5K3T2_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s5k3t2->dev, ret, "failed to read chip id\n");

	if (val != S5K3T2_CHIP_ID)
		return dev_err_probe(s5k3t2->dev, -ENODEV,
				     "chip id mismatch: %#x != %#llx\n",
				     S5K3T2_CHIP_ID, val);

	return 0;
}

static int s5k3t2_check_hwcfg(struct s5k3t2 *s5k3t2)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5k3t2->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
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

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K3T2_DATA_LANES) {
		dev_err(s5k3t2->dev, "invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5k3t2->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k3t2_link_freq_menu,
				       ARRAY_SIZE(s5k3t2_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5k3t2_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);
	int ret;

	if (s5k3t2->dvdd) {
		ret = regulator_enable(s5k3t2->dvdd);
		if (ret)
			return ret;

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
	}

	if (s5k3t2->avdd) {
		ret = regulator_enable(s5k3t2->avdd);
		if (ret)
			goto disable_dvdd;
	}

	if (s5k3t2->dovdd) {
		ret = regulator_enable(s5k3t2->dovdd);
		if (ret)
			goto disable_avdd;
	}

	ret = clk_prepare_enable(s5k3t2->mclk);
	if (ret)
		goto disable_dovdd;

	gpiod_set_value_cansleep(s5k3t2->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	return 0;

disable_dovdd:
	if (s5k3t2->dovdd)
		regulator_disable(s5k3t2->dovdd);
disable_avdd:
	if (s5k3t2->avdd)
		regulator_disable(s5k3t2->avdd);
disable_dvdd:
	if (s5k3t2->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(s5k3t2->dvdd);
	}

	return ret;
}

static int s5k3t2_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);

	gpiod_set_value_cansleep(s5k3t2->reset_gpio, 1);
	clk_disable_unprepare(s5k3t2->mclk);

	if (s5k3t2->dovdd)
		regulator_disable(s5k3t2->dovdd);
	if (s5k3t2->avdd)
		regulator_disable(s5k3t2->avdd);
	if (s5k3t2->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(s5k3t2->dvdd);
	}

	return 0;
}

static int s5k3t2_get_regulators(struct s5k3t2 *s5k3t2)
{
	static const char * const names[] = { "avdd", "dvdd", "dovdd" };
	struct regulator **targets[] = {
		&s5k3t2->avdd, &s5k3t2->dvdd, &s5k3t2->dovdd,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct regulator *reg;

		reg = devm_regulator_get_optional(s5k3t2->dev, names[i]);
		if (IS_ERR(reg)) {
			if (PTR_ERR(reg) != -ENODEV)
				return dev_err_probe(s5k3t2->dev, PTR_ERR(reg),
						     "failed to get %s\n",
						     names[i]);
			reg = NULL;
		}
		*targets[i] = reg;
	}

	return 0;
}

static int s5k3t2_probe(struct i2c_client *client)
{
	struct s5k3t2 *s5k3t2;
	unsigned long freq;
	int ret;

	s5k3t2 = devm_kzalloc(&client->dev, sizeof(*s5k3t2), GFP_KERNEL);
	if (!s5k3t2)
		return -ENOMEM;

	s5k3t2->dev = &client->dev;
	s5k3t2->mode = &s5k3t2_supported_modes[0];
	v4l2_i2c_subdev_init(&s5k3t2->sd, client, &s5k3t2_subdev_ops);

	s5k3t2->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k3t2->regmap))
		return dev_err_probe(s5k3t2->dev, PTR_ERR(s5k3t2->regmap),
				     "failed to init CCI\n");

	s5k3t2->mclk = devm_v4l2_sensor_clk_get(s5k3t2->dev, NULL);
	if (IS_ERR(s5k3t2->mclk))
		return dev_err_probe(s5k3t2->dev, PTR_ERR(s5k3t2->mclk),
				     "failed to get MCLK\n");

	freq = clk_get_rate(s5k3t2->mclk);
	if (freq != S5K3T2_MCLK_FREQ)
		return dev_err_probe(s5k3t2->dev, -EINVAL,
				     "MCLK frequency %lu not supported\n", freq);

	ret = s5k3t2_check_hwcfg(s5k3t2);
	if (ret)
		return ret;

	s5k3t2->reset_gpio = devm_gpiod_get_optional(s5k3t2->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5k3t2->reset_gpio))
		return dev_err_probe(s5k3t2->dev, PTR_ERR(s5k3t2->reset_gpio),
				     "cannot get reset GPIO\n");

	ret = s5k3t2_get_regulators(s5k3t2);
	if (ret)
		return ret;

	ret = s5k3t2_power_on(s5k3t2->dev);
	if (ret)
		return ret;

	ret = s5k3t2_identify_sensor(s5k3t2);
	if (ret)
		goto error_power_off;

	ret = s5k3t2_init_controls(s5k3t2);
	if (ret)
		goto error_power_off;

	s5k3t2->sd.internal_ops = &s5k3t2_internal_ops;
	s5k3t2->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k3t2->sd.entity.ops = &s5k3t2_subdev_entity_ops;
	s5k3t2->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	s5k3t2->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s5k3t2->sd.entity, 1, &s5k3t2->pad);
	if (ret)
		goto error_free_handler;

	s5k3t2->sd.state_lock = s5k3t2->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&s5k3t2->sd);
	if (ret)
		goto error_media_entity;

	pm_runtime_set_active(s5k3t2->dev);
	pm_runtime_enable(s5k3t2->dev);
	pm_runtime_set_autosuspend_delay(s5k3t2->dev, 1000);
	pm_runtime_use_autosuspend(s5k3t2->dev);

	ret = v4l2_async_register_subdev_sensor(&s5k3t2->sd);
	if (ret)
		goto error_pm;

	pm_runtime_idle(s5k3t2->dev);

	return 0;

error_pm:
	pm_runtime_disable(s5k3t2->dev);
	pm_runtime_set_suspended(s5k3t2->dev);
	v4l2_subdev_cleanup(&s5k3t2->sd);
error_media_entity:
	media_entity_cleanup(&s5k3t2->sd.entity);
error_free_handler:
	v4l2_ctrl_handler_free(&s5k3t2->ctrl_handler);
error_power_off:
	s5k3t2_power_off(s5k3t2->dev);

	return ret;
}

static void s5k3t2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3t2 *s5k3t2 = to_s5k3t2(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s5k3t2->ctrl_handler);

	pm_runtime_disable(s5k3t2->dev);
	if (!pm_runtime_status_suspended(s5k3t2->dev))
		s5k3t2_power_off(s5k3t2->dev);
	pm_runtime_set_suspended(s5k3t2->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(s5k3t2_pm_ops, s5k3t2_power_off,
				 s5k3t2_power_on, NULL);

static const struct of_device_id s5k3t2_of_match[] = {
	{ .compatible = "samsung,s5k3t2" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k3t2_of_match);

static struct i2c_driver s5k3t2_i2c_driver = {
	.driver = {
		.name = "s5k3t2",
		.pm = pm_ptr(&s5k3t2_pm_ops),
		.of_match_table = s5k3t2_of_match,
	},
	.probe = s5k3t2_probe,
	.remove = s5k3t2_remove,
};
module_i2c_driver(s5k3t2_i2c_driver);

MODULE_AUTHOR("Roy Kaandorp <roykaandorp@gmail.com>");
MODULE_DESCRIPTION("Samsung S5K3T2 20 Mpixel camera sensor driver");
MODULE_LICENSE("GPL");
