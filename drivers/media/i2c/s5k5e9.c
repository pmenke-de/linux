// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K5E9 5 Mpixel camera sensor driver
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

#define S5K5E9_MCLK_FREQ		(19200 * HZ_PER_KHZ)
#define S5K5E9_DATA_LANES		2
#define S5K5E9_BITS_PER_SAMPLE		10

/* Register map follows MIPI CCS, as on the other Samsung parts. */
#define S5K5E9_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K5E9_CHIP_ID			0x559b

#define S5K5E9_REG_CTRL_MODE		CCI_REG8(0x0100)
#define S5K5E9_MODE_STREAMING		BIT(0)

#define S5K5E9_REG_ORIENTATION		CCI_REG8(0x0101)
#define S5K5E9_HFLIP			BIT(0)
#define S5K5E9_VFLIP			BIT(1)

#define S5K5E9_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K5E9_EXPOSURE_MIN		8
#define S5K5E9_EXPOSURE_STEP		1
/*
 * Exposure is expressed in lines and has to stay below the frame length, with
 * room for the sensor's own readout overhead.
 */
#define S5K5E9_EXPOSURE_MARGIN		22

#define S5K5E9_REG_AGAIN		CCI_REG16(0x0204)
#define S5K5E9_AGAIN_MIN		32
#define S5K5E9_AGAIN_MAX		1024
#define S5K5E9_AGAIN_STEP		1
#define S5K5E9_AGAIN_DEFAULT		32

#define S5K5E9_REG_VTS			CCI_REG16(0x0340)
#define S5K5E9_VTS_MAX			0xfffc

#define S5K5E9_REG_HTS			CCI_REG16(0x0342)

#define S5K5E9_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define to_s5k5e9(_sd)			container_of(_sd, struct s5k5e9, sd)

/*
 * The two implemented modes run the link at different rates, so the mode
 * carries an index into this menu rather than there being a single frequency.
 */
enum {
	S5K5E9_LINK_FREQ_438MHZ,
};

static const s64 s5k5e9_link_freq_menu[] = {
	[S5K5E9_LINK_FREQ_438MHZ] = 438000000,
};

/* Ordered so the flip controls can pick a code by index. */
static const u32 s5k5e9_mbus_formats[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
};

static const char * const s5k5e9_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Color bars",
	"Fade to grey color bars",
	"PN9",
};

struct s5k5e9_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct s5k5e9_mode {
	u32 width;
	u32 height;
	u32 hts;			/* Line length in pixels */
	u32 vts;			/* Default frame length in lines */
	u32 link_freq_idx;
	struct s5k5e9_reg_list reg_list;
};

struct s5k5e9 {
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

	const struct s5k5e9_mode *mode;
};

/* Global setup. */
static const struct cci_reg_sequence s5k5e9_init_regs[] = {
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x3b45), 0x01 },
	{ CCI_REG8(0x0b05), 0x01 },
	{ CCI_REG8(0x392f), 0x01 },
	{ CCI_REG8(0x3930), 0x00 },
	{ CCI_REG8(0x3924), 0x7f },
	{ CCI_REG8(0x3925), 0xfd },
	{ CCI_REG8(0x3c08), 0xff },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c0a), 0x05 },
	{ CCI_REG8(0x3c31), 0xff },
	{ CCI_REG8(0x3c32), 0xff },
	{ CCI_REG8(0x3290), 0x10 },
	{ CCI_REG8(0x3200), 0x01 },
	{ CCI_REG8(0x3074), 0x06 },
	{ CCI_REG8(0x3075), 0x2f },
	{ CCI_REG8(0x308a), 0x20 },
	{ CCI_REG8(0x308b), 0x08 },
	{ CCI_REG8(0x308c), 0x0b },
	{ CCI_REG8(0x3081), 0x07 },
	{ CCI_REG8(0x307b), 0x85 },
	{ CCI_REG8(0x307a), 0x0a },
	{ CCI_REG8(0x3079), 0x0a },
	{ CCI_REG8(0x306e), 0x71 },
	{ CCI_REG8(0x306f), 0x28 },
	{ CCI_REG8(0x301f), 0x20 },
	{ CCI_REG8(0x3012), 0x4e },
	{ CCI_REG8(0x306b), 0x9a },
	{ CCI_REG8(0x3091), 0x16 },
	{ CCI_REG8(0x30c4), 0x06 },
	{ CCI_REG8(0x306a), 0x79 },
	{ CCI_REG8(0x30b0), 0xff },
	{ CCI_REG8(0x306d), 0x08 },
	{ CCI_REG8(0x3084), 0x16 },
	{ CCI_REG8(0x3070), 0x0f },
	{ CCI_REG8(0x30c2), 0x05 },
	{ CCI_REG8(0x3069), 0x87 },
	{ CCI_REG8(0x3c0f), 0x00 },
	{ CCI_REG8(0x3083), 0x14 },
	{ CCI_REG8(0x3080), 0x08 },
	{ CCI_REG8(0x3c34), 0xea },
	{ CCI_REG8(0x3c35), 0x5c },
};

/* Full 5 Mpixel readout. */
static const struct cci_reg_sequence s5k5e9_2592x1944_regs[] = {
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x59 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0x89 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x3c17), 0x00 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0820), 0x03 },
	{ CCI_REG8(0x0821), 0x6c },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0x3929), 0x0f },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x27 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x9f },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x98 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x00 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x0340), 0x07 },
	{ CCI_REG8(0x0341), 0xee },
	{ CCI_REG8(0x0342), 0x0c },
	{ CCI_REG8(0x0343), 0x28 },
	{ CCI_REG8(0x0200), 0x0b },
	{ CCI_REG8(0x0201), 0x9c },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x02 },
	{ CCI_REG8(0x30b8), 0x2e },
	{ CCI_REG8(0x30ba), 0x36 },
};

static const struct s5k5e9_mode s5k5e9_supported_modes[] = {
	{
		/* Full 5 Mpixel readout. */
		.width = 2592,
		.height = 1944,
		.hts = 3112,
		.vts = 2030,
		.link_freq_idx = S5K5E9_LINK_FREQ_438MHZ,
		.reg_list = {
			.regs = s5k5e9_2592x1944_regs,
			.num_regs = ARRAY_SIZE(s5k5e9_2592x1944_regs),
		},
	},
};

static u64 s5k5e9_pixel_rate(const struct s5k5e9_mode *mode)
{
	return div_u64(s5k5e9_link_freq_menu[mode->link_freq_idx] * 2 *
		       S5K5E9_DATA_LANES, S5K5E9_BITS_PER_SAMPLE);
}

static int s5k5e9_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k5e9 *s5k5e9 = container_of(ctrl->handler, struct s5k5e9,
					     ctrl_handler);
	const struct s5k5e9_mode *mode = s5k5e9->mode;
	s64 exposure_max;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Keep the exposure range consistent with the new blanking. */
		exposure_max = mode->height + ctrl->val - S5K5E9_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(s5k5e9->exposure,
					 s5k5e9->exposure->minimum,
					 exposure_max, s5k5e9->exposure->step,
					 min(s5k5e9->exposure->val,
					     exposure_max));
	}

	if (!pm_runtime_get_if_in_use(s5k5e9->dev))
		return 0;

	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5k5e9->regmap, S5K5E9_REG_AGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(s5k5e9->regmap, S5K5E9_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(s5k5e9->regmap, S5K5E9_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(s5k5e9->regmap, S5K5E9_REG_ORIENTATION,
			  (s5k5e9->hflip->val ? S5K5E9_HFLIP : 0) |
			  (s5k5e9->vflip->val ? S5K5E9_VFLIP : 0), &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(s5k5e9->regmap, S5K5E9_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(s5k5e9->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k5e9_ctrl_ops = {
	.s_ctrl = s5k5e9_set_ctrl,
};

static int s5k5e9_init_controls(struct s5k5e9 *s5k5e9)
{
	const struct s5k5e9_mode *mode = s5k5e9->mode;
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5k5e9->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max, hblank, vblank;
	u64 pixel_rate;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 12);
	if (ret)
		return ret;

	s5k5e9->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5k5e9_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(s5k5e9_link_freq_menu) - 1,
					mode->link_freq_idx,
					s5k5e9_link_freq_menu);
	if (s5k5e9->link_freq)
		s5k5e9->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = s5k5e9_pixel_rate(mode);
	s5k5e9->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       pixel_rate, 1, pixel_rate);
	if (s5k5e9->pixel_rate)
		s5k5e9->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank = mode->hts - mode->width;
	s5k5e9->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (s5k5e9->hblank)
		s5k5e9->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s5k5e9->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   S5K5E9_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K5E9_AGAIN_MIN, S5K5E9_AGAIN_MAX,
			  S5K5E9_AGAIN_STEP, S5K5E9_AGAIN_DEFAULT);

	exposure_max = mode->vts - S5K5E9_EXPOSURE_MARGIN;
	s5k5e9->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K5E9_EXPOSURE_MIN, exposure_max,
					     S5K5E9_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k5e9_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k5e9_test_pattern_menu) - 1,
				     0, 0, s5k5e9_test_pattern_menu);

	s5k5e9->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s5k5e9->hflip)
		s5k5e9->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	s5k5e9->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e9_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s5k5e9->vflip)
		s5k5e9->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(s5k5e9->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k5e9_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	s5k5e9->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int s5k5e9_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	const struct s5k5e9_reg_list *reg_list = &s5k5e9->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(s5k5e9->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(s5k5e9->regmap, s5k5e9_init_regs,
			    ARRAY_SIZE(s5k5e9_init_regs), &ret);
	cci_multi_reg_write(s5k5e9->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s5k5e9->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(s5k5e9->regmap, S5K5E9_REG_CTRL_MODE,
		  S5K5E9_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s5k5e9->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5k5e9->dev);

	return ret;
}

static int s5k5e9_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	int ret;

	ret = cci_write(s5k5e9->regmap, S5K5E9_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(s5k5e9->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s5k5e9->dev);

	return ret;
}

static u32 s5k5e9_get_format_code(struct s5k5e9 *s5k5e9)
{
	unsigned int i;

	i = (s5k5e9->vflip->val ? 2 : 0) | (s5k5e9->hflip->val ? 1 : 0);

	return s5k5e9_mbus_formats[i];
}

static void s5k5e9_update_pad_format(struct s5k5e9 *s5k5e9,
				     const struct s5k5e9_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = s5k5e9_get_format_code(s5k5e9);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k5e9_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	const struct s5k5e9_mode *mode;
	struct v4l2_mbus_framefmt *format;
	s64 exposure_max, hblank, vblank;

	mode = v4l2_find_nearest_size(s5k5e9_supported_modes,
				      ARRAY_SIZE(s5k5e9_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	s5k5e9_update_pad_format(s5k5e9, mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	s5k5e9->mode = mode;

	__v4l2_ctrl_s_ctrl(s5k5e9->link_freq, mode->link_freq_idx);
	__v4l2_ctrl_s_ctrl_int64(s5k5e9->pixel_rate, s5k5e9_pixel_rate(mode));

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(s5k5e9->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(s5k5e9->vblank, vblank,
				 S5K5E9_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(s5k5e9->vblank, vblank);

	exposure_max = mode->vts - S5K5E9_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(s5k5e9->exposure, S5K5E9_EXPOSURE_MIN,
				 exposure_max, S5K5E9_EXPOSURE_STEP,
				 exposure_max);

	return 0;
}

static int s5k5e9_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	if (code->index)
		return -EINVAL;

	code->code = s5k5e9_get_format_code(s5k5e9);

	return 0;
}

static int s5k5e9_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	if (fse->index >= ARRAY_SIZE(s5k5e9_supported_modes))
		return -EINVAL;

	if (fse->code != s5k5e9_get_format_code(s5k5e9))
		return -EINVAL;

	fse->min_width = s5k5e9_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k5e9_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k5e9_get_selection(struct v4l2_subdev *sd,
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
		sel->r.width = s5k5e9_supported_modes[1 - 1].width;
		sel->r.height = s5k5e9_supported_modes[1 - 1].height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5k5e9_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s5k5e9->mode->width,
			.height = s5k5e9->mode->height,
		},
	};

	return s5k5e9_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k5e9_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k5e9_pad_ops = {
	.set_fmt = s5k5e9_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5k5e9_get_selection,
	.enum_mbus_code = s5k5e9_enum_mbus_code,
	.enum_frame_size = s5k5e9_enum_frame_size,
	.enable_streams = s5k5e9_enable_streams,
	.disable_streams = s5k5e9_disable_streams,
};

static const struct v4l2_subdev_ops s5k5e9_subdev_ops = {
	.video = &s5k5e9_video_ops,
	.pad = &s5k5e9_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k5e9_internal_ops = {
	.init_state = s5k5e9_init_state,
};

static const struct media_entity_operations s5k5e9_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k5e9_identify_sensor(struct s5k5e9 *s5k5e9)
{
	u64 val;
	int ret;

	ret = cci_read(s5k5e9->regmap, S5K5E9_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s5k5e9->dev, ret, "failed to read chip id\n");

	if (val != S5K5E9_CHIP_ID)
		return dev_err_probe(s5k5e9->dev, -ENODEV,
				     "chip id mismatch: %#x != %#llx\n",
				     S5K5E9_CHIP_ID, val);

	return 0;
}

static int s5k5e9_check_hwcfg(struct s5k5e9 *s5k5e9)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5k5e9->dev), *ep;
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

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K5E9_DATA_LANES) {
		dev_err(s5k5e9->dev, "invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5k5e9->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k5e9_link_freq_menu,
				       ARRAY_SIZE(s5k5e9_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5k5e9_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	int ret;

	if (s5k5e9->dvdd) {
		ret = regulator_enable(s5k5e9->dvdd);
		if (ret)
			return ret;

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
	}

	if (s5k5e9->avdd) {
		ret = regulator_enable(s5k5e9->avdd);
		if (ret)
			goto disable_dvdd;
	}

	if (s5k5e9->dovdd) {
		ret = regulator_enable(s5k5e9->dovdd);
		if (ret)
			goto disable_avdd;
	}

	ret = clk_prepare_enable(s5k5e9->mclk);
	if (ret)
		goto disable_dovdd;

	gpiod_set_value_cansleep(s5k5e9->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	return 0;

disable_dovdd:
	if (s5k5e9->dovdd)
		regulator_disable(s5k5e9->dovdd);
disable_avdd:
	if (s5k5e9->avdd)
		regulator_disable(s5k5e9->avdd);
disable_dvdd:
	if (s5k5e9->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(s5k5e9->dvdd);
	}

	return ret;
}

static int s5k5e9_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	gpiod_set_value_cansleep(s5k5e9->reset_gpio, 1);
	clk_disable_unprepare(s5k5e9->mclk);

	if (s5k5e9->dovdd)
		regulator_disable(s5k5e9->dovdd);
	if (s5k5e9->avdd)
		regulator_disable(s5k5e9->avdd);
	if (s5k5e9->dvdd) {
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		regulator_disable(s5k5e9->dvdd);
	}

	return 0;
}

static int s5k5e9_get_regulators(struct s5k5e9 *s5k5e9)
{
	static const char * const names[] = { "avdd", "dvdd", "dovdd" };
	struct regulator **targets[] = {
		&s5k5e9->avdd, &s5k5e9->dvdd, &s5k5e9->dovdd,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct regulator *reg;

		reg = devm_regulator_get_optional(s5k5e9->dev, names[i]);
		if (IS_ERR(reg)) {
			if (PTR_ERR(reg) != -ENODEV)
				return dev_err_probe(s5k5e9->dev, PTR_ERR(reg),
						     "failed to get %s\n",
						     names[i]);
			reg = NULL;
		}
		*targets[i] = reg;
	}

	return 0;
}

static int s5k5e9_probe(struct i2c_client *client)
{
	struct s5k5e9 *s5k5e9;
	unsigned long freq;
	int ret;

	s5k5e9 = devm_kzalloc(&client->dev, sizeof(*s5k5e9), GFP_KERNEL);
	if (!s5k5e9)
		return -ENOMEM;

	s5k5e9->dev = &client->dev;
	s5k5e9->mode = &s5k5e9_supported_modes[0];
	v4l2_i2c_subdev_init(&s5k5e9->sd, client, &s5k5e9_subdev_ops);

	s5k5e9->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k5e9->regmap))
		return dev_err_probe(s5k5e9->dev, PTR_ERR(s5k5e9->regmap),
				     "failed to init CCI\n");

	s5k5e9->mclk = devm_v4l2_sensor_clk_get(s5k5e9->dev, NULL);
	if (IS_ERR(s5k5e9->mclk))
		return dev_err_probe(s5k5e9->dev, PTR_ERR(s5k5e9->mclk),
				     "failed to get MCLK\n");

	freq = clk_get_rate(s5k5e9->mclk);
	if (freq != S5K5E9_MCLK_FREQ)
		return dev_err_probe(s5k5e9->dev, -EINVAL,
				     "MCLK frequency %lu not supported\n", freq);

	ret = s5k5e9_check_hwcfg(s5k5e9);
	if (ret)
		return ret;

	s5k5e9->reset_gpio = devm_gpiod_get_optional(s5k5e9->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5k5e9->reset_gpio))
		return dev_err_probe(s5k5e9->dev, PTR_ERR(s5k5e9->reset_gpio),
				     "cannot get reset GPIO\n");

	ret = s5k5e9_get_regulators(s5k5e9);
	if (ret)
		return ret;

	ret = s5k5e9_power_on(s5k5e9->dev);
	if (ret)
		return ret;

	ret = s5k5e9_identify_sensor(s5k5e9);
	if (ret)
		goto error_power_off;

	ret = s5k5e9_init_controls(s5k5e9);
	if (ret)
		goto error_power_off;

	s5k5e9->sd.internal_ops = &s5k5e9_internal_ops;
	s5k5e9->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k5e9->sd.entity.ops = &s5k5e9_subdev_entity_ops;
	s5k5e9->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	s5k5e9->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s5k5e9->sd.entity, 1, &s5k5e9->pad);
	if (ret)
		goto error_free_handler;

	s5k5e9->sd.state_lock = s5k5e9->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&s5k5e9->sd);
	if (ret)
		goto error_media_entity;

	pm_runtime_set_active(s5k5e9->dev);
	pm_runtime_enable(s5k5e9->dev);
	pm_runtime_set_autosuspend_delay(s5k5e9->dev, 1000);
	pm_runtime_use_autosuspend(s5k5e9->dev);

	ret = v4l2_async_register_subdev_sensor(&s5k5e9->sd);
	if (ret)
		goto error_pm;

	pm_runtime_idle(s5k5e9->dev);

	return 0;

error_pm:
	pm_runtime_disable(s5k5e9->dev);
	pm_runtime_set_suspended(s5k5e9->dev);
	v4l2_subdev_cleanup(&s5k5e9->sd);
error_media_entity:
	media_entity_cleanup(&s5k5e9->sd.entity);
error_free_handler:
	v4l2_ctrl_handler_free(&s5k5e9->ctrl_handler);
error_power_off:
	s5k5e9_power_off(s5k5e9->dev);

	return ret;
}

static void s5k5e9_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s5k5e9->ctrl_handler);

	pm_runtime_disable(s5k5e9->dev);
	if (!pm_runtime_status_suspended(s5k5e9->dev))
		s5k5e9_power_off(s5k5e9->dev);
	pm_runtime_set_suspended(s5k5e9->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(s5k5e9_pm_ops, s5k5e9_power_off,
				 s5k5e9_power_on, NULL);

static const struct of_device_id s5k5e9_of_match[] = {
	{ .compatible = "samsung,s5k5e9" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k5e9_of_match);

static struct i2c_driver s5k5e9_i2c_driver = {
	.driver = {
		.name = "s5k5e9",
		.pm = pm_ptr(&s5k5e9_pm_ops),
		.of_match_table = s5k5e9_of_match,
	},
	.probe = s5k5e9_probe,
	.remove = s5k5e9_remove,
};
module_i2c_driver(s5k5e9_i2c_driver);

MODULE_AUTHOR("Roy Kaandorp <roykaandorp@gmail.com>");
MODULE_DESCRIPTION("Samsung S5K5E9 5 Mpixel camera sensor driver");
MODULE_LICENSE("GPL");
