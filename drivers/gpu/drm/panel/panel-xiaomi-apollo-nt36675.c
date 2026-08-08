// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM panel driver for the Xiaomi Apollo / Xiaomi 10T Pro LCD panel
 * (Novatek NT36675 based, vendor panel: mdss_dsi_j3s_37_02_0a_dsc_video).
 *
 * Mainline port based on the vendor Android DT command tables.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct apollo_dsi_cmd {
	u8 data[16];
	u8 len;
	u16 delay_ms;
};

/*
 * Only vddio (1.8 V digital I/O) and vci (3.0 V analog) are listed.
 * The LCDB lab/ibb regulators are left out because the upstream LCDB
 * driver cannot probe (qcom_pmic_get() dependency). The bootloader
 * leaves LCDB powered, which is sufficient for initial bring-up.
 */
#define APOLLO_NUM_SUPPLIES 2

struct apollo_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[APOLLO_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	struct drm_dsc_config dsc;
};

static inline struct apollo_panel *to_apollo_panel(struct drm_panel *panel)
{
	return container_of(panel, struct apollo_panel, panel);
}

static const char *const apollo_supply_names[APOLLO_NUM_SUPPLIES] = {
	"vddio",
	"vci",
};

static const struct apollo_dsi_cmd apollo_pre_on_cmds[] = {
	{ .data = { 0xff, 0xe0 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x35, 0x82 }, .len = 2 },
	{ .data = { 0xff, 0xf0 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x5a, 0x00 }, .len = 2 },
	{ .data = { 0x9f, 0x19 }, .len = 2 },
	{ .data = { 0x9c, 0x00 }, .len = 2 },
	{ .data = { 0xff, 0xd0 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0xff, 0xc0 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x9c, 0x11 }, .len = 2 },
	{ .data = { 0x9d, 0x11 }, .len = 2 },
	{ .data = { 0xff, 0x23 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x00, 0x80 }, .len = 2 },
	{ .data = { 0x01, 0x84 }, .len = 2 },
	{ .data = { 0x05, 0x2d }, .len = 2 },
	{ .data = { 0x06, 0x00 }, .len = 2 },
	{ .data = { 0x07, 0x00 }, .len = 2 },
	{ .data = { 0x08, 0x01 }, .len = 2 },
	{ .data = { 0x09, 0x45 }, .len = 2 },
	{ .data = { 0x11, 0x01 }, .len = 2 },
	{ .data = { 0x12, 0x95 }, .len = 2 },
	{ .data = { 0x15, 0x68 }, .len = 2 },
	{ .data = { 0x16, 0x0b }, .len = 2 },
	{ .data = { 0x29, 0x0a }, .len = 2 },
	{ .data = { 0x30, 0xff }, .len = 2 },
	{ .data = { 0x31, 0xfe }, .len = 2 },
	{ .data = { 0x32, 0xfd }, .len = 2 },
	{ .data = { 0x33, 0xfb }, .len = 2 },
	{ .data = { 0x34, 0xf8 }, .len = 2 },
	{ .data = { 0x35, 0xf5 }, .len = 2 },
	{ .data = { 0x36, 0xf3 }, .len = 2 },
	{ .data = { 0x37, 0xf2 }, .len = 2 },
	{ .data = { 0x38, 0xf2 }, .len = 2 },
	{ .data = { 0x39, 0xf2 }, .len = 2 },
	{ .data = { 0x3a, 0xef }, .len = 2 },
	{ .data = { 0x3b, 0xec }, .len = 2 },
	{ .data = { 0x3d, 0xe9 }, .len = 2 },
	{ .data = { 0x3f, 0xe5 }, .len = 2 },
	{ .data = { 0x40, 0xe5 }, .len = 2 },
	{ .data = { 0x41, 0xe5 }, .len = 2 },
	{ .data = { 0x2a, 0x13 }, .len = 2 },
	{ .data = { 0x45, 0xff }, .len = 2 },
	{ .data = { 0x46, 0xf4 }, .len = 2 },
	{ .data = { 0x47, 0xe7 }, .len = 2 },
	{ .data = { 0x48, 0xda }, .len = 2 },
	{ .data = { 0x49, 0xcd }, .len = 2 },
	{ .data = { 0x4a, 0xc0 }, .len = 2 },
	{ .data = { 0x4b, 0xb3 }, .len = 2 },
	{ .data = { 0x4c, 0xb2 }, .len = 2 },
	{ .data = { 0x4d, 0xb2 }, .len = 2 },
	{ .data = { 0x4e, 0xb2 }, .len = 2 },
	{ .data = { 0x4f, 0x99 }, .len = 2 },
	{ .data = { 0x50, 0x80 }, .len = 2 },
	{ .data = { 0x51, 0x68 }, .len = 2 },
	{ .data = { 0x52, 0x66 }, .len = 2 },
	{ .data = { 0x53, 0x66 }, .len = 2 },
	{ .data = { 0x54, 0x66 }, .len = 2 },
	{ .data = { 0x2b, 0x0e }, .len = 2 },
	{ .data = { 0x58, 0xff }, .len = 2 },
	{ .data = { 0x59, 0xfb }, .len = 2 },
	{ .data = { 0x5a, 0xf7 }, .len = 2 },
	{ .data = { 0x5b, 0xf3 }, .len = 2 },
	{ .data = { 0x5c, 0xef }, .len = 2 },
	{ .data = { 0x5d, 0xe3 }, .len = 2 },
	{ .data = { 0x5e, 0xda }, .len = 2 },
	{ .data = { 0x5f, 0xd8 }, .len = 2 },
	{ .data = { 0x60, 0xd8 }, .len = 2 },
	{ .data = { 0x61, 0xd8 }, .len = 2 },
	{ .data = { 0x62, 0xcb }, .len = 2 },
	{ .data = { 0x63, 0xbf }, .len = 2 },
	{ .data = { 0x64, 0xb3 }, .len = 2 },
	{ .data = { 0x65, 0xb2 }, .len = 2 },
	{ .data = { 0x66, 0xb2 }, .len = 2 },
	{ .data = { 0x67, 0xb2 }, .len = 2 },
	{ .data = { 0xff, 0x27 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x40, 0x20 }, .len = 2 },
	{ .data = { 0xff, 0x10 }, .len = 2 },
	{ .data = { 0x51, 0x0b, 0xbb }, .len = 3 },
	{ .data = { 0x53, 0x24 }, .len = 2 },
	{ .data = { 0xff, 0x10 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x35, 0x00 }, .len = 2 },
};

static const struct apollo_dsi_cmd apollo_post_on_cmds[] = {
	{ .data = { 0xff, 0x27 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x3f, 0x01 }, .len = 2 },
	{ .data = { 0x43, 0x08 }, .len = 2 },
	{ .data = { 0x40, 0x25 }, .len = 2 },
	{ .data = { 0xff, 0x10 }, .len = 2 },
};

static const struct apollo_dsi_cmd apollo_off_cmds[] = {
	{ .data = { 0xff, 0x27 }, .len = 2 },
	{ .data = { 0xfb, 0x01 }, .len = 2 },
	{ .data = { 0x3f, 0x00 }, .len = 2 },
	{ .data = { 0xff, 0x10 }, .len = 2 },
	{ .data = { 0x28, 0x00 }, .len = 2 },
	{ .data = { 0x10, 0x00 }, .len = 2, .delay_ms = 70 },
};

static const struct drm_display_mode apollo_mode = {
	.clock = 415642, /* 1175 * 2456 * 144Hz = 415.64 MHz */

	.hdisplay = 1080,
	.hsync_start = 1080 + 40,
	.hsync_end = 1080 + 40 + 12,
	.htotal = 1080 + 40 + 12 + 43,

	.vdisplay = 2400,
	.vsync_start = 2400 + 28,
	.vsync_end = 2400 + 28 + 2,
	.vtotal = 2400 + 28 + 2 + 26,

	.width_mm = 71,
	.height_mm = 158,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int apollo_write_cmds(struct mipi_dsi_multi_context *ctx,
			     const struct apollo_dsi_cmd *cmds, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		mipi_dsi_dcs_write_buffer_multi(ctx, cmds[i].data, cmds[i].len);
		if (cmds[i].delay_ms)
			mipi_dsi_msleep(ctx, cmds[i].delay_ms);
	}

	return ctx->accum_err;
}

static void apollo_reset(struct apollo_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(2000, 3000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int apollo_prepare(struct drm_panel *panel)
{
	struct apollo_panel *ctx = to_apollo_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct drm_dsc_picture_parameter_set pps;
	unsigned long mode_flags;
	int ret;

	dev_dbg(panel->dev, "apollo_prepare\n");

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret) {
		dev_err(panel->dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 1);

	apollo_reset(ctx);

	mode_flags = ctx->dsi->mode_flags;
	ctx->dsi->mode_flags = mode_flags | MIPI_DSI_MODE_LPM;

	ret = apollo_write_cmds(&dsi_ctx, apollo_pre_on_cmds,
				ARRAY_SIZE(apollo_pre_on_cmds));
	if (ret < 0)
		goto err_disable;

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	ret = apollo_write_cmds(&dsi_ctx, apollo_post_on_cmds,
				ARRAY_SIZE(apollo_post_on_cmds));
	if (ret < 0)
		goto err_disable;

	mipi_dsi_msleep(&dsi_ctx, 80);

	/* PPS and compression enable LAST, matching vendor order */
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 0);
	mipi_dsi_msleep(&dsi_ctx, 28);

	if (dsi_ctx.accum_err < 0) {
		ret = dsi_ctx.accum_err;
		goto err_disable;
	}

	ctx->dsi->mode_flags = mode_flags;
	return 0;

err_disable:
	ctx->dsi->mode_flags = mode_flags;
	dev_err(panel->dev, "prepare failed: %d\n", ret);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	return ret;
}

static int apollo_disable(struct drm_panel *panel)
{
	struct apollo_panel *ctx = to_apollo_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	unsigned long mode_flags;

	dev_dbg(panel->dev, "apollo_disable\n");

	mode_flags = ctx->dsi->mode_flags;
	ctx->dsi->mode_flags = mode_flags | MIPI_DSI_MODE_LPM;

	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, false,
					    MIPI_DSI_COMPRESSION_DSC, 0);
	apollo_write_cmds(&dsi_ctx, apollo_off_cmds,
			  ARRAY_SIZE(apollo_off_cmds));

	ctx->dsi->mode_flags = mode_flags;
	return dsi_ctx.accum_err;
}

static int apollo_unprepare(struct drm_panel *panel)
{
	struct apollo_panel *ctx = to_apollo_panel(panel);

	dev_dbg(panel->dev, "apollo_unprepare\n");

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);

	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	return 0;
}

static const struct drm_display_mode apollo_modes[] = {
	{
		/* 144 Hz (Preferred) */
		.clock = 415555,
		.hdisplay = 1080,
		.hsync_start = 1080 + 40,
		.hsync_end = 1080 + 40 + 12,
		.htotal = 1080 + 40 + 12 + 43,
		.vdisplay = 2400,
		.vsync_start = 2400 + 28,
		.vsync_end = 2400 + 28 + 2,
		.vtotal = 2400 + 28 + 2 + 26,
		.width_mm = 71,
		.height_mm = 158,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	{
		/* 120 Hz (VFP Padded) */
		.clock = 415555,
		.hdisplay = 1080,
		.hsync_start = 1080 + 40,
		.hsync_end = 1080 + 40 + 12,
		.htotal = 1080 + 40 + 12 + 43,
		.vdisplay = 2400,
		.vsync_start = 2400 + 519,
		.vsync_end = 2400 + 519 + 2,
		.vtotal = 2400 + 519 + 2 + 26,
		.width_mm = 71,
		.height_mm = 158,
		.type = DRM_MODE_TYPE_DRIVER,
	},
	{
		/* 90 Hz (VFP Padded) */
		.clock = 415555,
		.hdisplay = 1080,
		.hsync_start = 1080 + 40,
		.hsync_end = 1080 + 40 + 12,
		.htotal = 1080 + 40 + 12 + 43,
		.vdisplay = 2400,
		.vsync_start = 2400 + 1502,
		.vsync_end = 2400 + 1502 + 2,
		.vtotal = 2400 + 1502 + 2 + 26,
		.width_mm = 71,
		.height_mm = 158,
		.type = DRM_MODE_TYPE_DRIVER,
	},
	{
		/* 60 Hz (VFP Padded) */
		.clock = 415555,
		.hdisplay = 1080,
		.hsync_start = 1080 + 40,
		.hsync_end = 1080 + 40 + 12,
		.htotal = 1080 + 40 + 12 + 43,
		.vdisplay = 2400,
		.vsync_start = 2400 + 3466,
		.vsync_end = 2400 + 3466 + 2,
		.vtotal = 2400 + 3466 + 2 + 26,
		.width_mm = 71,
		.height_mm = 158,
		.type = DRM_MODE_TYPE_DRIVER,
	},
};

static int apollo_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(apollo_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &apollo_modes[i]);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = apollo_modes[0].width_mm;
	connector->display_info.height_mm = apollo_modes[0].height_mm;
	connector->display_info.bpc = 8;

	return ARRAY_SIZE(apollo_modes);
}

static const struct drm_panel_funcs apollo_panel_funcs = {
	.prepare = apollo_prepare,
	.disable = apollo_disable,
	.unprepare = apollo_unprepare,
	.get_modes = apollo_get_modes,
};

static int apollo_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct apollo_panel *ctx;
	static const struct drm_dsc_rc_range_parameters rc_params[] = {
		{ .range_min_qp =  0, .range_max_qp =  4, .range_bpg_offset =  2 },
		{ .range_min_qp =  0, .range_max_qp =  4, .range_bpg_offset =  0 },
		{ .range_min_qp =  1, .range_max_qp =  5, .range_bpg_offset =  0 },
		{ .range_min_qp =  1, .range_max_qp =  6, .range_bpg_offset = -2 },
		{ .range_min_qp =  3, .range_max_qp =  7, .range_bpg_offset = -4 },
		{ .range_min_qp =  3, .range_max_qp =  7, .range_bpg_offset = -6 },
		{ .range_min_qp =  3, .range_max_qp =  7, .range_bpg_offset = -8 },
		{ .range_min_qp =  3, .range_max_qp =  8, .range_bpg_offset = -8 },
		{ .range_min_qp =  3, .range_max_qp =  9, .range_bpg_offset = -8 },
		{ .range_min_qp =  3, .range_max_qp = 10, .range_bpg_offset = -10 },
		{ .range_min_qp =  5, .range_max_qp = 11, .range_bpg_offset = -10 },
		{ .range_min_qp =  5, .range_max_qp = 12, .range_bpg_offset = -12 },
		{ .range_min_qp =  5, .range_max_qp = 13, .range_bpg_offset = -12 },
		{ .range_min_qp =  7, .range_max_qp = 13, .range_bpg_offset = -12 },
		{ .range_min_qp = 12, .range_max_qp = 15, .range_bpg_offset = -12 },
	};
	int ret;
	size_t i;

	dev_info(dev, "apollo panel probe\n");

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	for (i = 0; i < APOLLO_NUM_SUPPLIES; i++)
		ctx->supplies[i].supply = apollo_supply_names[i];

	ret = devm_regulator_bulk_get(dev, APOLLO_NUM_SUPPLIES, ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get panel regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset gpio\n");

	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "failed to get enable gpio\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	drm_panel_init(&ctx->panel, dev, &apollo_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret == -EPROBE_DEFER)
		return dev_err_probe(dev, ret, "backlight not ready\n");
	if (ret)
		dev_warn(dev, "no backlight, continuing without: %d\n", ret);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;

	/* Added MIPI_DSI_MODE_LPM as specified by DTSI BLLP power modes */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_VIDEO_HSE |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_LPM;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.pic_width = 1080;
	ctx->dsc.pic_height = 2400;
	ctx->dsc.slice_height = 20;
	ctx->dsc.slice_width = 540;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;
	ctx->dsc.convert_rgb = true;

	/* * 1. Allow DRM helper to compute baseline standard RC parameters first 
	 */
	drm_dsc_set_rc_buf_thresh(&ctx->dsc);
	memcpy(ctx->dsc.rc_range_params, rc_params, sizeof(rc_params));
	drm_dsc_compute_rc_parameters(&ctx->dsc);

	/* * 2. Override explicitly with vendor-specific parameters so they 
	 * don't get destroyed by drm_dsc_compute_rc_parameters()
	 */
	ctx->dsc.rc_model_size = 8192;
	ctx->dsc.initial_xmit_delay = 512;
	ctx->dsc.initial_offset = 6144;
	ctx->dsc.first_line_bpg_offset = 12;
	ctx->dsc.line_buf_depth = 9;
	ctx->dsc.flatness_min_qp = 3;
	ctx->dsc.flatness_max_qp = 12;
	ctx->dsc.rc_edge_factor = 6;
	ctx->dsc.rc_quant_incr_limit0 = 11;
	ctx->dsc.rc_quant_incr_limit1 = 11;
	ctx->dsc.rc_tgt_offset_high = 3;
	ctx->dsc.rc_tgt_offset_low = 3;
	ctx->dsc.mux_word_size = DSC_MUX_WORD_SIZE_8_10_BPC;

	dsi->dsc = &ctx->dsc;

	ctx->panel.prepare_prev_first = true;
	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	dev_info(dev, "apollo panel probed successfully\n");
	return 0;
}

static void apollo_remove(struct mipi_dsi_device *dsi)
{
	struct apollo_panel *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id apollo_of_match[] = {
	{ .compatible = "xiaomi,apollo-nt36675" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, apollo_of_match);

static struct mipi_dsi_driver apollo_driver = {
	.probe = apollo_probe,
	.remove = apollo_remove,
	.driver = {
		.name = "panel-xiaomi-apollo-nt36675",
		.of_match_table = apollo_of_match,
	},
};
module_mipi_dsi_driver(apollo_driver);

MODULE_DESCRIPTION("DRM panel driver for Xiaomi Apollo NT36675 panel");
MODULE_LICENSE("GPL");
