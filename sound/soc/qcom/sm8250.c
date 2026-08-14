// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include <linux/uaccess.h>
#include <sound/soc-card.h>
#include "qdsp6/q6afe.h"
#include "common.h"
#include "usb_offload_utils.h"
#include "sdw.h"

#define MI2S_BCLK_RATE		1536000

static unsigned int tdm_slot_offset[8] = {0, 4, 8, 12, 16, 20, 24, 28};
/*
 * Eight 32-bit slots at 96 kHz: 96000 * 8 * 32, matching the rate a live stock
 * mixer dump shows on this port (TERT_TDM_RX_0 SampleRate = KHZ_96).
 */
#define TDM_BCLK_RATE		24576000

/*
 * The two CS35L41 are the loudspeaker and the receiver. Which is which was
 * settled by powering one down during a call: with 0x42 off only the bottom
 * speaker played. So "RCV" at 0x40 is the loudspeaker and "LCV" at 0x42 the
 * receiver -- what both device trees say, even though the name prefixes read
 * the other way round.
 */
enum {
	SM8250_SPK_EARPIECE,
	SM8250_SPK_LOUDSPEAKER,
	SM8250_SPK_COUNT
};

struct sm8250_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	unsigned int spk_volume;
	bool spk_off[SM8250_SPK_COUNT];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack usb_offload_jack;
	bool usb_offload_jack_setup;
	struct snd_soc_jack dp_jack;
	bool jack_setup;
};

static int sm8250_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case DISPLAY_PORT_RX:
		return qcom_snd_dp_jack_setup(rtd, &data->dp_jack, 0);
	case USB_RX:
		return qcom_snd_usb_offload_jack_setup(rtd, &data->usb_offload_jack,
						       &data->usb_offload_jack_setup);
	default:
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
	}
}

static void sm8250_snd_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == USB_RX)
		qcom_snd_usb_offload_jack_remove(rtd,
						 &data->usb_offload_jack_setup);

}

static int sm8250_tdm_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret = 0;
	int channels, slots, slot_width;

	channels = params_channels(params);

	/*
	 * Follow the vendor machine driver exactly (kona_tdm_snd_hw_params):
	 * eight 32-bit slots, and a slot mask covering all of them rather than
	 * just the two that carry audio. Its tertiary RX_0 entry is
	 * { {0, 4, 0xFFFF} }, i.e. the two channels sit at byte offsets 0 and 4
	 * of a 32-bit-slot frame - which is what tdm_slot_offset[] already says.
	 */
	slots = 8;
	slot_width = 32;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/*
		 * One mask bit per channel actually carried. The vendor driver
		 * opens every slot in the frame (0xFF for eight), but its port
		 * config carries a matching channel count; asking the ADSP to
		 * enable eight slots for a two channel port makes
		 * AFE_PORT_CMD_DEVICE_START (0x100e5) fail with error 1 and the
		 * card never appears.
		 */
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, (1 << channels) - 1,
					       slots, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
				__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
					  channels, tdm_slot_offset);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
				__func__, ret);
			goto end;
		}

		/*
		 * Give each amplifier its own slot. Without this both parts keep
		 * the reset default and read slot 0, so at best the two speakers
		 * play the same channel.
		 *
		 * Kept the same way round as the MI2S path above, which is the
		 * one this board actually uses and where the assignment was
		 * settled by ear.
		 */
		if (cpu_dai->id == TERTIARY_TDM_RX_0) {
			struct snd_soc_dai *codec_dai;
			int j;

			for_each_rtd_codec_dais(rtd, j, codec_dai) {
				/*
				 * Both receive slots have to be programmed, the
				 * way the vendor codec driver does it from
				 * cirrus,right-channel-amp:
				 *
				 *   RX1 = right ? 1 : 0
				 *   RX2 = right ? 0 : 1
				 *
				 * Setting only RX1 leaves RX2 wherever it reset
				 * to - the same slot on both amplifiers - and
				 * the part mixes what it finds there.
				 */
				unsigned int codec_slot[2] = { !j, j };

				ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL,
								  2, codec_slot);
				if (ret < 0) {
					dev_err(rtd->dev, "%s: codec channel map err:%d\n",
						__func__, ret);
					goto end;
				}
			}
		}

		ret = 0;
	} else {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0xf, 0, slots, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
				__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, channels,
					  tdm_slot_offset, 0, NULL);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
				__func__, ret);
			goto end;
		}
	}

end:
	return ret;
}

static int sm8250_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	switch (cpu_dai->id) {
	case TERTIARY_TDM_RX_0:
		/*
		 * Two channels of 24-bit at 96 kHz. A live mixer dump taken from
		 * the stock system has this port - the only RX mixer enabled for
		 * MultiMedia1 - set to KHZ_96 / S24_LE / Two. mixer_paths.xml
		 * lists KHZ_48 for its speaker paths, but that is the HAL's
		 * config file rather than the state the hardware ends up in.
		 *
		 * The rate has to agree with TDM_BCLK_RATE, the bit clock the
		 * AFE is asked to generate: eight 32-bit slots at 96 kHz is
		 * exactly 24.576 MHz.
		 */
		rate->min = rate->max = 96000;
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S24_LE);
		break;
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
		/*
		 * Qualcomm codec TX backends may be driven in mono or stereo.
		 */
		channels->min = 1;
		break;
	case TX_CODEC_DMA_TX_3:
		/*
		 * Two channels, always: the top mic carries speech and the
		 * bottom one is the echo reference, which is how the stock
		 * system runs this port for a call (0xb037, 48 kHz, two
		 * channels) and what q6cvp tells the vocproc to expect.
		 *
		 * Leaving the minimum at one meant the port followed whichever
		 * front end opened it first -- PulseAudio keeps a mono source
		 * open permanently -- so the vocproc was told two channels and
		 * given one, and the uplink was silent while capture itself
		 * worked. A capture front end that wants mono still gets it;
		 * the conversion happens in the DSP.
		 */
		channels->min = channels->max = 2;
		break;
	case VA_CODEC_DMA_TX_0:
		/*
		 * Vendor config uses 8 channels for the VA backend; allow a wider
		 * range here instead of clamping it to stereo.
		 */
		channels->min = 1;
		channels->max = 8;
		break;
	default:
		break;
	}

	return 0;
}

static int sm8250_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	int ret,j;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SECONDARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case TERTIARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);

		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			/*
			 * Both receive slots have to be programmed, the way
			 * the vendor codec driver does it from
			 * cirrus,right-channel-amp:
			 *
			 *   RX1 = right ? 1 : 0
			 *   RX2 = right ? 0 : 1
			 *
			 * Codec 0 is the loudspeaker at 0x40 and takes the
			 * right slot, codec 1 the receiver and the left, so
			 * the left channel comes out of the earpiece. That
			 * agrees with the vendor marking 0x40, and only 0x40,
			 * cirrus,right-channel-amp. Left at the reset default
			 * both parts read slot 0 and play the same channel.
			 *
			 * The TDM path does this too, but this board runs the
			 * amplifiers on MI2S, so doing it only there left the
			 * slots unprogrammed on the path actually in use.
			 */
			unsigned int codec_slot[2] = { !j, j };

			ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL,
							  2, codec_slot);
			if (ret < 0) {
				dev_err(rtd->dev, "MI2S channel map err:%d\n",
					ret);
				return ret;
			}

			ret = snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
			/* CS35L41_CLKID_SCLK=0: configure PLL to lock on BCLK */
			snd_soc_dai_set_sysclk(codec_dai, 0, MI2S_BCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);
			snd_soc_component_set_sysclk(codec_dai->component,
						     0, 0, MI2S_BCLK_RATE,
						     SND_SOC_CLOCK_IN);
			if (ret < 0) {
				dev_err(rtd->dev, "MI2S fmt err:%d\n", ret);
				return ret;
			}
		}
		break;
	case QUINARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case TERTIARY_TDM_RX_0:
		/*
		 * This port is pinned to 96 kHz (see be_hw_params_fixup) and the
		 * frame is eight 32-bit slots, so the AFE clocks 24.576 MHz -
		 * not the 12.288 MHz that a 48 kHz frame would need. Telling the
		 * amplifiers the wrong reference makes their PLL lock on a clock
		 * running at twice what they were configured for: every register
		 * reads back healthy, PLL_LOCK included, and the part emits
		 * silence.
		 */
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_DSP_A;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT,
			TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);

		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			ret = snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
			snd_soc_dai_set_sysclk(codec_dai, 0, TDM_BCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);
			/* CS35L41_CLKID_SCLK=0: configure PLL to lock on BCLK at TDM rate */
			snd_soc_component_set_sysclk(codec_dai->component,
						     0, 0, TDM_BCLK_RATE,
						     SND_SOC_CLOCK_IN);
			if (ret < 0) {
				dev_err(rtd->dev, "TDM fmt err:%d\n", ret);
				return ret;
			}
		}
		break;
	case PRIMARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_DSP_A;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT,
			TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);

		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			ret = snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
			snd_soc_dai_set_sysclk(codec_dai, 0, TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
			/* CS35L41_CLKID_SCLK=0: configure PLL to lock on BCLK at TDM rate */
			snd_soc_component_set_sysclk(codec_dai->component,
						     0, 0, TDM_BCLK_RATE,
						     SND_SOC_CLOCK_IN);
			if (ret < 0) {
				dev_err(rtd->dev, "TDM fmt err:%d\n", ret);
				return ret;
			}
		}
		break;

	default:
		break;
	}

	return qcom_snd_sdw_startup(substream);
}

static int sm8250_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	int j;

	switch (cpu_dai->id) {
	case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:
		return sm8250_tdm_snd_hw_params(substream, params);
	case TERTIARY_MI2S_RX:
		/* Assign I2S channel slots: codec 0 = L (slot 0), codec 1 = R (slot 1) */
		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			unsigned int codec_slot[1] = {j};

			snd_soc_dai_set_channel_map(codec_dai, 0, NULL,
						    1, codec_slot);
		}
		return 0;
	}

	return 0;
}

static int sm8250_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sm8250_be_ops = {
	.startup = sm8250_snd_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_params = sm8250_snd_hw_params,
	.hw_free = sm8250_snd_hw_free,
	.prepare = sm8250_snd_prepare,
};

static void sm8250_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sm8250_snd_init;
			link->exit = sm8250_snd_exit;
			link->be_hw_params_fixup = sm8250_be_hw_params_fixup;
			link->ops = &sm8250_be_ops;
		}
	}
}

/*
 * Both amplifiers are mono components with their own name prefix, so there is
 * nothing single for userspace to move -- a UCM profile names one
 * PlaybackVolume control -- and no way to play out of one of them alone, which
 * is what an earpiece call needs.
 *
 * The volume is more than a convenience. Call audio is mixed in the ADSP and
 * never passes through userspace, so a software volume cannot reach it; the
 * amplifier gain is the only gain sitting in both the music and the call path.
 *
 * So present the pair as one volume and a switch each. The muting has to go
 * through the digital volume, which has a mute at zero: the analog gain looks
 * like the obvious candidate and is not one, because it bottoms out at
 * +0.5 dB and a "muted" speaker is still perfectly audible. That puts the mute
 * and the volume on the same register, so the switches are kept here as state
 * and a muted amplifier is simply one the volume is not written to.
 */
static const char * const sm8250_spk_volume_name[SM8250_SPK_COUNT] = {
	[SM8250_SPK_EARPIECE]	 = "LCV Digital PCM Volume",
	[SM8250_SPK_LOUDSPEAKER] = "RCV Digital PCM Volume",
};

/*
 * The amplifiers' output widgets, for powering one down on its own. Both parts
 * share tlmm 114 as their reset, so neither can be held in reset separately;
 * this is the only per amplifier off the board has.
 */
static const char * const sm8250_spk_widget_name[SM8250_SPK_COUNT] = {
	[SM8250_SPK_EARPIECE]	 = "LCV SPK",
	[SM8250_SPK_LOUDSPEAKER] = "RCV SPK",
};

static struct snd_kcontrol *sm8250_spk_kcontrol(struct snd_soc_card *card,
						unsigned int i)
{
	return snd_soc_card_get_kcontrol(card, sm8250_spk_volume_name[i]);
}

static int sm8250_spk_write(struct snd_soc_card *card, unsigned int i,
			    unsigned int val)
{
	struct snd_kcontrol *k = sm8250_spk_kcontrol(card, i);
	struct snd_ctl_elem_value *ev;
	int ret;

	if (!k)
		return -ENODEV;

	/* Far too big for the stack, and this is never on a fast path. */
	ev = kzalloc(sizeof(*ev), GFP_KERNEL);
	if (!ev)
		return -ENOMEM;

	ev->value.integer.value[0] = val;
	ret = k->put(k, ev);
	kfree(ev);

	/*
	 * The amplifier's own control moved, and the core only reports the
	 * control it was asked to write. Anything watching the pair --
	 * alsamixer, a profile saving mixer state -- would otherwise keep
	 * showing the value from before.
	 */
	if (ret > 0)
		snd_ctl_notify(card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &k->id);

	return ret;
}

static int sm8250_spk_apply(struct snd_soc_card *card)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	int changed = 0;
	unsigned int i;

	for (i = 0; i < SM8250_SPK_COUNT; i++) {
		int ret;

		/*
		 * Turning the volume down is not an off switch. Muting the
		 * digital volume leaves the amplifier running and this board
		 * was still audible through it, so the output is powered down
		 * as well; the mute goes with it so nothing leaks back on the
		 * way through.
		 */
		if (data->spk_off[i])
			snd_soc_dapm_disable_pin(card->dapm,
						 sm8250_spk_widget_name[i]);
		else
			snd_soc_dapm_enable_pin(card->dapm,
						sm8250_spk_widget_name[i]);

		ret = sm8250_spk_write(card, i, data->spk_off[i] ?
				       0 : data->spk_volume);
		if (ret < 0)
			return ret;
		if (ret)
			changed = 1;
	}

	snd_soc_dapm_sync(card->dapm);

	return changed;
}

static int sm8250_spk_volume_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_kcontrol *k = sm8250_spk_kcontrol(card, 0);

	if (!k)
		return -ENODEV;

	return k->info(k, uinfo);
}

static int sm8250_spk_volume_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);

	/*
	 * Answered from here rather than from an amplifier: a muted one reads
	 * back zero, and the volume the user set has to survive the mute.
	 */
	ucontrol->value.integer.value[0] = data->spk_volume;
	return 0;
}

static int sm8250_spk_volume_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	unsigned int val = ucontrol->value.integer.value[0];

	if (val == data->spk_volume)
		return 0;

	data->spk_volume = val;
	return sm8250_spk_apply(card) < 0 ? -EIO : 1;
}

static int sm8250_spk_switch_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = !data->spk_off[kcontrol->private_value];
	return 0;
}

static int sm8250_spk_switch_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	unsigned int i = kcontrol->private_value;
	bool off = !ucontrol->value.integer.value[0];

	if (off == data->spk_off[i])
		return 0;

	data->spk_off[i] = off;
	return sm8250_spk_apply(card) < 0 ? -EIO : 1;
}

static int sm8250_spk_volume_tlv(struct snd_kcontrol *kcontrol, int op_flag,
				 unsigned int size, unsigned int __user *tlv)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_kcontrol *k = sm8250_spk_kcontrol(card, 0);
	unsigned int len;

	if (!k)
		return -ENODEV;

	/*
	 * Hand back whatever the amplifier describes rather than restating its
	 * range here: userspace turns this into dB, and a copy of the scale
	 * that drifts from the real one is worse than none.
	 */
	if (k->vd[0].access & SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK)
		return k->tlv.c(k, op_flag, size, tlv);

	if (!k->tlv.p)
		return -ENXIO;

	len = 2 * sizeof(unsigned int) + k->tlv.p[1];
	if (size < len)
		return -ENOMEM;
	if (copy_to_user(tlv, k->tlv.p, len))
		return -EFAULT;

	return 0;
}

static const struct snd_kcontrol_new sm8250_spk_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			  SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.info = sm8250_spk_volume_info,
		.get = sm8250_spk_volume_get,
		.put = sm8250_spk_volume_put,
		.tlv.c = sm8250_spk_volume_tlv,
	},
	SOC_SINGLE_BOOL_EXT("Speaker Playback Switch", SM8250_SPK_LOUDSPEAKER,
			    sm8250_spk_switch_get, sm8250_spk_switch_put),
	SOC_SINGLE_BOOL_EXT("Earpiece Playback Switch", SM8250_SPK_EARPIECE,
			    sm8250_spk_switch_get, sm8250_spk_switch_put),
};

static int sm8250_late_probe(struct snd_soc_card *card)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_ctl_elem_value *ev;
	struct snd_kcontrol *k;
	unsigned int i;

	/* Boards without the pair get nothing; the names would be a lie. */
	for (i = 0; i < SM8250_SPK_COUNT; i++)
		if (!sm8250_spk_kcontrol(card, i))
			return 0;

	/* Start from whatever the amplifiers already carry. */
	ev = kzalloc(sizeof(*ev), GFP_KERNEL);
	if (!ev)
		return -ENOMEM;

	k = sm8250_spk_kcontrol(card, 0);
	if (!k->get(k, ev))
		data->spk_volume = ev->value.integer.value[0];
	kfree(ev);

	return snd_soc_add_card_controls(card, sm8250_spk_controls,
					 ARRAY_SIZE(sm8250_spk_controls));
}

static int sm8250_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8250_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	card->late_probe = sm8250_late_probe;
	sm8250_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sm8250_dt_match[] = {
	{ .compatible = "fairphone,fp4-sndcard", .data = "sm7225" },
	{ .compatible = "fairphone,fp5-sndcard", .data = "qcm6490" },
	{ .compatible = "qcom,qrb2210-sndcard", .data = "qcm2290" },
	{ .compatible = "qcom,qrb4210-rb2-sndcard", .data = "sm4250" },
	{ .compatible = "qcom,qrb5165-rb5-sndcard", .data = "sm8250" },
	{ .compatible = "qcom,sm8250-sndcard", .data = "sm8250" },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8250_dt_match);

static struct platform_driver snd_sm8250_driver = {
	.probe  = sm8250_platform_probe,
	.driver = {
		.name = "snd-sm8250",
		.of_match_table = snd_sm8250_dt_match,
	},
};
module_platform_driver(snd_sm8250_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SM8250 ASoC Machine Driver");
MODULE_LICENSE("GPL");
