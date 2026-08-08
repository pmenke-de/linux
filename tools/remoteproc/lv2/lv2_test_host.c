// SPDX-License-Identifier: GPL-2.0
/*
 * lv2_test_host - run the Hexagon NAM LV2 plugin over a WAV, offline
 *
 * Enough of an LV2 host to exercise the parts that can actually break in this
 * fork - URID mapping, the worker thread that loads the model, a patch:Set to
 * point at a .nqw blob, and run() with the host's own block size - without
 * needing JACK, PipeWire or an audio interface. Being offline also means the
 * output is comparable against nam_dsp_run's, so the plugin can be checked
 * against the known-good path rather than just "it made a noise".
 *
 * Must run inside a CDSP domain, same as any other client:
 *   sudo cdsp-run "/usr/local/bin/lv2_test_host BUNDLE model.nqw in.wav out.wav [block]"
 *
 * The worker is called synchronously here; a real host runs it off the audio
 * thread, but the plugin's contract is the same either way.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
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

static char *uris[MAX_URIS];
static uint32_t n_uris = 1;             /* 0 is not a valid URID */

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

static const char *unmap_uri(LV2_URID_Unmap_Handle h, LV2_URID urid)
{
	(void)h;
	return (urid && urid < n_uris) ? uris[urid] : NULL;
}

static int vprintf_cb(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
{
	(void)h; (void)type;
	fprintf(stderr, "  [plugin] ");
	return vfprintf(stderr, fmt, ap);
}

static int printf_cb(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vprintf_cb(h, type, fmt, ap);
	va_end(ap);
	return r;
}

/* The plugin hands work back through respond(); apply it immediately. */
static const LV2_Descriptor *desc;
static LV2_Handle instance;

static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size,
				 const void *data)
{
	(void)h;
	if (desc->extension_data) {
		const LV2_Worker_Interface *wi =
			desc->extension_data(LV2_WORKER__interface);

		if (wi && wi->work_response)
			wi->work_response(instance, size, data);
	}
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h,
				       uint32_t size, const void *data)
{
	(void)h;
	if (desc->extension_data) {
		const LV2_Worker_Interface *wi =
			desc->extension_data(LV2_WORKER__interface);

		if (wi && wi->work)
			return wi->work(instance, respond, NULL, size, data);
	}
	return LV2_WORKER_ERR_UNKNOWN;
}

/* Minimal RIFF: mono 16/24-bit in, 16-bit out. */
static float *read_wav(const char *path, unsigned *count, unsigned *rate)
{
	unsigned char *d;
	long size;
	FILE *f = fopen(path, "rb");
	unsigned pos = 12, bits = 0, ch = 1, dlen = 0;
	unsigned char *data = NULL;
	float *out;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(size);
	if (!d || fread(d, 1, size, f) != (size_t)size) {
		fclose(f); free(d); return NULL;
	}
	fclose(f);

	while (pos + 8 <= (unsigned long)size) {
		unsigned sz;

		memcpy(&sz, d + pos + 4, 4);
		if (!memcmp(d + pos, "fmt ", 4)) {
			memcpy(&ch, d + pos + 10, 2); ch &= 0xffff;
			memcpy(rate, d + pos + 12, 4);
			memcpy(&bits, d + pos + 22, 2); bits &= 0xffff;
		} else if (!memcmp(d + pos, "data", 4)) {
			data = d + pos + 8; dlen = sz;
		}
		pos += 8 + sz + (sz & 1);
	}
	if (!data || (bits != 16 && bits != 24)) { free(d); return NULL; }

	*count = dlen / (bits / 8) / ch;
	out = malloc(*count * sizeof(float));
	for (unsigned i = 0; i < *count; i++) {
		if (bits == 16) {
			int16_t v;

			memcpy(&v, data + (size_t)i * 2 * ch, 2);
			out[i] = v / 32768.0f;
		} else {
			const unsigned char *p = data + (size_t)i * 3 * ch;
			int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);

			if (v & 0x800000)
				v -= 1 << 24;
			out[i] = v / 8388608.0f;
		}
	}
	free(d);
	return out;
}

static void write_wav(const char *path, const float *x, unsigned n, unsigned rate)
{
	unsigned char hdr[44];
	unsigned bytes = n * 2;
	FILE *f = fopen(path, "wb");
	int16_t *pcm = malloc(n * sizeof(int16_t));

	for (unsigned i = 0; i < n; i++) {
		float s = x[i] * 32768.0f;

		pcm[i] = s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768
			 : (int16_t)(s >= 0 ? s + 0.5f : s - 0.5f);
	}
	memcpy(hdr, "RIFF", 4);
	*(uint32_t *)(hdr + 4) = 36 + bytes;
	memcpy(hdr + 8, "WAVEfmt ", 8);
	*(uint32_t *)(hdr + 16) = 16;
	*(uint16_t *)(hdr + 20) = 1;
	*(uint16_t *)(hdr + 22) = 1;
	*(uint32_t *)(hdr + 24) = rate;
	*(uint32_t *)(hdr + 28) = rate * 2;
	*(uint16_t *)(hdr + 32) = 2;
	*(uint16_t *)(hdr + 34) = 16;
	memcpy(hdr + 36, "data", 4);
	*(uint32_t *)(hdr + 40) = bytes;
	fwrite(hdr, 1, 44, f);
	fwrite(pcm, 2, n, f);
	fclose(f);
	free(pcm);
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
	float *in, *out;
	unsigned n = 0, rate = 48000, block;
	char so[1024];
	void *lib;
	const LV2_Descriptor *(*entry)(uint32_t);

	if (argc < 5) {
		fprintf(stderr, "usage: %s BUNDLE model.nqw in.wav out.wav [block]\n",
			argv[0]);
		return 1;
	}
	block = argc > 5 ? (unsigned)atoi(argv[5]) : 128;

	snprintf(so, sizeof(so), "%s/neural_amp_modeler.so", argv[1]);
	lib = dlopen(so, RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen %s: %s\n", so, dlerror());
		return 1;
	}
	entry = (const LV2_Descriptor *(*)(uint32_t))dlsym(lib, "lv2_descriptor");
	if (!entry) {
		fprintf(stderr, "no lv2_descriptor\n");
		return 1;
	}
	desc = entry(0);
	printf("plugin: %s\n", desc->URI);

	in = read_wav(argv[3], &n, &rate);
	if (!in) {
		fprintf(stderr, "cannot read %s\n", argv[3]);
		return 1;
	}
	out = calloc(n + block, sizeof(float));

	instance = desc->instantiate(desc, rate, argv[1], features);
	if (!instance) {
		fprintf(stderr, "instantiate failed\n");
		return 1;
	}

	ctl = calloc(1, SEQ_BYTES);
	notify = calloc(1, SEQ_BYTES);

	desc->connect_port(instance, 0, ctl);
	desc->connect_port(instance, 1, notify);
	desc->connect_port(instance, 4, &in_level);
	desc->connect_port(instance, 5, &out_level);
	desc->connect_port(instance, 6, &quality);

	if (desc->activate)
		desc->activate(instance);

	/* patch:Set the model path, exactly as a real host would */
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

	desc->connect_port(instance, 2, in);
	desc->connect_port(instance, 3, out);
	desc->run(instance, 0);         /* deliver the patch:Set, no audio */

	/* now stream the file */
	lv2_atom_forge_set_buffer(&forge, (uint8_t *)ctl, SEQ_BYTES);
	{
		LV2_Atom_Forge_Frame seq;

		lv2_atom_forge_sequence_head(&forge, &seq, 0);
		lv2_atom_forge_pop(&forge, &seq);
	}

	unsigned i;

	for (i = 0; i + block <= n; i += block) {
		notify->atom.size = SEQ_BYTES - sizeof(LV2_Atom);
		desc->connect_port(instance, 2, in + i);
		desc->connect_port(instance, 3, out + i);
		desc->run(instance, block);
	}

	if (desc->deactivate)
		desc->deactivate(instance);
	desc->cleanup(instance);

	write_wav(argv[4], out, i, rate);
	printf("wrote %s: %u samples, block %u\n", argv[4], i, block);
	return 0;
}
