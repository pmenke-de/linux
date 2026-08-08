// SPDX-License-Identifier: GPL-2.0
/*
 * lv2_alsa_host - live guitar rig: ALSA duplex through the Hexagon NAM plugin
 *
 * Runs the LV2 plugin against an ALSA device directly, with no PipeWire or JACK
 * in the path. For a guitar amp that is the right trade: the graph buys nothing
 * here, and going straight to the hardware keeps the period size and hence the
 * latency under our control. It also sidesteps the fact that WirePlumber is not
 * currently exposing ALSA devices on this system at all.
 *
 * Must run inside a CDSP domain so the plugin inherits the FastRPC fd:
 *
 *   cdsp-run "lv2_alsa_host BUNDLE model.nqw hw:1,0 [period] [in_ch] [out_ch]"
 *
 * No sudo needed once /dev/fastrpc-* is group-readable and the user is in the
 * fastrpc group.
 *
 * The interface is opened in its native S32_LE at 48 kHz. Only one input
 * channel is processed (a guitar is mono); the result is written to the first
 * two output channels so it lands on both sides of a pair of headphones.
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/log/log.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#define MAX_URIS 256
#define SEQ_BYTES 8192
#define RATE 48000

static char *uris[MAX_URIS];
static uint32_t n_uris = 1;
static volatile sig_atomic_t running = 1;

static void on_sigint(int s) { (void)s; running = 0; }

static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri)
{
	(void)h;
	for (uint32_t i = 1; i < n_uris; i++)
		if (!strcmp(uris[i], uri))
			return i;
	if (n_uris >= MAX_URIS)
		return 0;
	uris[n_uris] = strdup(uri);
	return n_uris++;
}

static const char *unmap_uri(LV2_URID_Unmap_Handle h, LV2_URID u)
{
	(void)h;
	return (u && u < n_uris) ? uris[u] : NULL;
}

static int vprintf_cb(LV2_Log_Handle h, LV2_URID t, const char *fmt, va_list ap)
{
	(void)h; (void)t;
	fprintf(stderr, "  [plugin] ");
	return vfprintf(stderr, fmt, ap);
}

static int printf_cb(LV2_Log_Handle h, LV2_URID t, const char *fmt, ...)
{
	va_list ap; int r;
	va_start(ap, fmt); r = vprintf_cb(h, t, fmt, ap); va_end(ap);
	return r;
}

static const LV2_Descriptor *desc;
static LV2_Handle instance;

static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size,
				 const void *data)
{
	(void)h;
	const LV2_Worker_Interface *wi = desc->extension_data(LV2_WORKER__interface);

	if (wi && wi->work_response)
		wi->work_response(instance, size, data);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h,
				       uint32_t size, const void *data)
{
	(void)h;
	const LV2_Worker_Interface *wi = desc->extension_data(LV2_WORKER__interface);

	if (wi && wi->work)
		return wi->work(instance, respond, NULL, size, data);
	return LV2_WORKER_ERR_UNKNOWN;
}

static int setup_pcm(snd_pcm_t *pcm, unsigned channels, snd_pcm_uframes_t period,
		     const char *what)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	unsigned rate = RATE, periods = 4;   /* 4 periods of slack for a non-RT kernel */
	int err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, channels);
	snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
	snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, 0);

	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0) {
		fprintf(stderr, "%s: hw_params: %s\n", what, snd_strerror(err));
		return err;
	}
	if (rate != RATE)
		fprintf(stderr, "%s: got %u Hz, models expect %d\n", what, rate, RATE);

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period);
	snd_pcm_sw_params(pcm, sw);
	return 0;
}

int main(int argc, char **argv)
{
	LV2_URID_Map map = { NULL, map_uri };
	LV2_URID_Unmap unmap = { NULL, unmap_uri };
	LV2_Log_Log log = { NULL, printf_cb, vprintf_cb };
	LV2_Worker_Schedule sched = { NULL, schedule_work };
	LV2_Feature f_map = { LV2_URID__map, &map };
	LV2_Feature f_unmap = { LV2_URID__unmap, &unmap };
	LV2_Feature f_log = { LV2_LOG__log, &log };
	LV2_Feature f_sched = { LV2_WORKER__schedule, &sched };
	const LV2_Feature *features[] = { &f_map, &f_unmap, &f_log, &f_sched, NULL };

	LV2_Atom_Forge forge;
	LV2_Atom_Sequence *ctl, *notify;
	float in_level = 0, out_level = 0, quality = 1.0f;
	float *fin, *fout;
	int32_t *cbuf, *pbuf;
	snd_pcm_t *cap = NULL, *play = NULL;
	const char *dev;
	unsigned period, in_ch, out_ch;
	char so[1024];
	void *lib;
	const LV2_Descriptor *(*entry)(uint32_t);
	unsigned long xruns = 0, blocks = 0, consec = 0;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc < 4) {
		fprintf(stderr,
			"usage: %s BUNDLE model.nqw DEVICE [period] [in_ch] [out_ch]\n"
			"  e.g. %s ~/.lv2/neural_amp_modeler.lv2 model.nqw hw:1,0 128 4 4\n",
			argv[0], argv[0]);
		return 1;
	}
	dev = argv[3];
	/*
	 * 256 frames (5.33 ms) runs clean here; 128 collapses into a recovery
	 * loop once timing gets tight, because a linked capture/playback pair
	 * cannot always be restarted from the error path. Fixing that means
	 * driving the two streams unlinked.
	 */
	period = argc > 4 ? (unsigned)atoi(argv[4]) : 256;
	in_ch = argc > 5 ? (unsigned)atoi(argv[5]) : 2;
	out_ch = argc > 6 ? (unsigned)atoi(argv[6]) : 2;

	if (period % 64) {
		fprintf(stderr, "period must be a multiple of 64 (DSP granularity)\n");
		return 1;
	}

	snprintf(so, sizeof(so), "%s/neural_amp_modeler.so", argv[1]);
	lib = dlopen(so, RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen %s: %s\n", so, dlerror());
		return 1;
	}
	entry = (const LV2_Descriptor *(*)(uint32_t))dlsym(lib, "lv2_descriptor");
	desc = entry(0);

	instance = desc->instantiate(desc, RATE, argv[1], features);
	if (!instance) {
		fprintf(stderr, "instantiate failed\n");
		return 1;
	}

	ctl = calloc(1, SEQ_BYTES);
	notify = calloc(1, SEQ_BYTES);
	fin = calloc(period, sizeof(float));
	fout = calloc(period, sizeof(float));
	cbuf = calloc(period * in_ch, sizeof(int32_t));
	pbuf = calloc(period * out_ch, sizeof(int32_t));

	desc->connect_port(instance, 0, ctl);
	desc->connect_port(instance, 1, notify);
	desc->connect_port(instance, 2, fin);
	desc->connect_port(instance, 3, fout);
	desc->connect_port(instance, 4, &in_level);
	desc->connect_port(instance, 5, &out_level);
	desc->connect_port(instance, 6, &quality);

	if (desc->activate)
		desc->activate(instance);

	/* load the model exactly the way a host would */
	lv2_atom_forge_init(&forge, &map);
	lv2_atom_forge_set_buffer(&forge, (uint8_t *)ctl, SEQ_BYTES);
	{
		LV2_Atom_Forge_Frame seq, obj;

		lv2_atom_forge_sequence_head(&forge, &seq, 0);
		lv2_atom_forge_frame_time(&forge, 0);
		lv2_atom_forge_object(&forge, &obj, 0, map_uri(NULL, LV2_PATCH__Set));
		lv2_atom_forge_key(&forge, map_uri(NULL, LV2_PATCH__property));
		lv2_atom_forge_urid(&forge, map_uri(NULL,
			"http://github.com/mikeoliphant/neural-amp-modeler-lv2#model"));
		lv2_atom_forge_key(&forge, map_uri(NULL, LV2_PATCH__value));
		lv2_atom_forge_path(&forge, argv[2], (uint32_t)strlen(argv[2]));
		lv2_atom_forge_pop(&forge, &obj);
		lv2_atom_forge_pop(&forge, &seq);
	}
	notify->atom.size = SEQ_BYTES - sizeof(LV2_Atom);
	notify->atom.type = map_uri(NULL, LV2_ATOM__Sequence);
	desc->run(instance, 0);

	lv2_atom_forge_set_buffer(&forge, (uint8_t *)ctl, SEQ_BYTES);
	{
		LV2_Atom_Forge_Frame seq;

		lv2_atom_forge_sequence_head(&forge, &seq, 0);
		lv2_atom_forge_pop(&forge, &seq);
	}

	if (snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0) < 0 ||
	    snd_pcm_open(&play, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		fprintf(stderr, "cannot open %s for duplex\n", dev);
		return 1;
	}
	if (setup_pcm(cap, in_ch, period, "capture") < 0 ||
	    setup_pcm(play, out_ch, period, "playback") < 0)
		return 1;

	/*
	 * Prepare both before linking: a linked pair starts together, but each
	 * still has to be brought out of SETUP itself, and writing to an
	 * unprepared stream just errors straight back and spins the loop.
	 */
	/*
	 * Deliberately NOT snd_pcm_link()ed. Linking starts the pair together,
	 * but it also means an error on one leaves the other in a state the
	 * recovery path cannot always restart, which showed up as capture
	 * wedged in PREPARED and every later read failing. Driving them
	 * independently costs a little startup alignment and recovers cleanly.
	 */
	snd_pcm_prepare(cap);
	snd_pcm_prepare(play);

	/* prime playback so the first capture period has somewhere to go */
	memset(pbuf, 0, period * out_ch * sizeof(int32_t));
	for (int i = 0; i < 2; i++) {
		snd_pcm_sframes_t w = snd_pcm_writei(play, pbuf, period);

		if (w < 0)
			fprintf(stderr, "prime: %s\n", snd_strerror((int)w));
	}

	{
		struct sched_param sp = { .sched_priority = 70 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
			printf("scheduling: SCHED_FIFO prio 70\n");
		else
			printf("scheduling: normal (no rtprio; expect xruns "
			       "on a PREEMPT_NONE kernel)\n");
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	printf("live: %s, %u frames/period (%.2f ms), in %u ch, out %u ch\n",
	       dev, period, period * 1000.0 / RATE, in_ch, out_ch);
	printf("guitar on input 1; ctrl-C to stop\n");

	if (snd_pcm_state(cap) == SND_PCM_STATE_PREPARED)
		snd_pcm_start(cap);

	while (running) {
		snd_pcm_sframes_t got = snd_pcm_readi(cap, cbuf, period);

		if (got < 0) {
			if (xruns < 5)
				fprintf(stderr, "capture: %s (state %s)\n",
					snd_strerror((int)got),
					snd_pcm_state_name(snd_pcm_state(cap)));
			xruns++;
			if (snd_pcm_recover(cap, (int)got, 1) < 0)
				break;
			/* recover() leaves it PREPARED; capture will not
			 * self-start, so kick it or every later read fails. */
			if (snd_pcm_state(cap) == SND_PCM_STATE_PREPARED)
				snd_pcm_start(cap);
			/* Bail rather than spin: a stream we cannot restart
			 * would otherwise burn a core and log millions of
			 * "xruns" that are really one stuck device. */
			if (++consec > 50) {
				fprintf(stderr, "capture stuck; giving up\n");
				break;
			}
			continue;
		}

		for (snd_pcm_sframes_t i = 0; i < got; i++)
			fin[i] = (float)cbuf[i * in_ch] * (1.0f / 2147483648.0f);

		desc->run(instance, (uint32_t)got);

		for (snd_pcm_sframes_t i = 0; i < got; i++) {
			float v = fout[i];
			int32_t s;

			if (v > 1.0f)
				v = 1.0f;
			else if (v < -1.0f)
				v = -1.0f;
			s = (int32_t)(v * 2147483000.0f);
			for (unsigned c = 0; c < out_ch; c++)
				pbuf[i * out_ch + c] = (c < 2) ? s : 0;
		}

		snd_pcm_sframes_t put = snd_pcm_writei(play, pbuf, got);

		if (put < 0) {
			if (xruns < 5)
				fprintf(stderr, "playback: %s (state %s)\n",
					snd_strerror((int)put),
					snd_pcm_state_name(snd_pcm_state(play)));
			xruns++;
			if (snd_pcm_recover(play, (int)put, 1) < 0)
				break;
			if (snd_pcm_state(play) == SND_PCM_STATE_PREPARED) {
				memset(pbuf, 0, period * out_ch * sizeof(int32_t));
				snd_pcm_writei(play, pbuf, period);
			}
		}
		consec = 0;
		blocks++;
	}

	printf("\nstopped after %lu blocks, %lu xruns\n", blocks, xruns);

	if (desc->deactivate)
		desc->deactivate(instance);
	desc->cleanup(instance);
	snd_pcm_close(cap);
	snd_pcm_close(play);
	return 0;
}
