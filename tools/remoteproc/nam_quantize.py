#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Turn a NAM WaveNet model into the int16 blob namwn_skel.c consumes.

Handles both shapes seen in the wild:
  legacy (v0.5)  one kernel_size per group, Tanh, 1x1 head rechannel
  A2 (v0.7)      per-layer kernel_sizes, LeakyReLU(0.01), and a head that is a
                 Conv1D(channels -> 1, kernel 16, bias). A2 captures arrive
                 wrapped in a SlimmableContainer; the widest submodel is the one
                 NAM itself renders with, so that is the one taken here.

Calibrates every tensor's dynamic range on a piece of audio, picks a
power-of-two shift per tensor, quantises weights and biases into the
accumulator domain each consumer expects, and writes a blob whose layout
mirrors the structs in namwn_skel.c exactly.

Weight order (NAM/wavenet/model.cpp): per group rechannel, then
[conv, input_mixin, layer1x1] per layer, then head_rechannel; the final scalar
of the whole array is head_scale (config's head_scale is a constructor default
and is NOT what the model uses).  Conv1D: for o, for i, for k, then biases.

Usage: nam_quantize.py model.nam calib.wav out.nqw [headroom_bits]
"""

import json
import struct
import sys

import numpy as np

TANH_N = 4096
TANH_RANGE = 8.0          # table spans [-8, 8), matching apply_tanh()
NQ_MAGIC = 0x3257514E     # "NQW2"
Q15 = -15                 # shift of tanh outputs and of the input signal
LEAKY = 0.01

ACT_TANH, ACT_LEAKY = 0, 1

# Products accumulate in int32 on the DSP. An output that uses the full int16
# range sits at acc = out * 2^rshift, so rshift must stay small enough that
# 32767 * 2^rshift fits with room for partial-sum excursions. 15 gives a peak
# of 2^30 against the 2^31 limit. The cap is enforced by coarsening the weight
# scale, which is why small weight tensors (notably the mixins) lose bits.
MAX_RSHIFT = 15


def fit_shift(s_w_ideal, s_in, s_out):
    """Weight shift and requantise shift with the accumulator kept in range."""
    s_w = max(s_w_ideal, s_out - s_in - MAX_RSHIFT)
    return s_w, s_out - (s_w + s_in)


def load_wav(path):
    d = open(path, "rb").read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        sz, = struct.unpack_from("<I", d, pos + 4)
        body = d[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif cid == b"data":
            data = body
        pos += 8 + sz + (sz & 1)
    af, ch, rate, _, _, bits = fmt
    if bits == 16:
        x = np.frombuffer(data, "<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        raw = np.frombuffer(data, np.uint8).reshape(-1, 3).astype(np.int32)
        v = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        x = np.where(v & 0x800000, v - (1 << 24), v).astype(np.float64) / (1 << 23)
    elif bits == 32 and af == 3:
        x = np.frombuffer(data, "<f4").astype(np.float64)
    else:
        raise SystemExit(f"unsupported wav ({bits} bits, format {af})")
    return x.reshape(-1, ch)[:, 0], rate


class Taker:
    def __init__(self, w):
        self.w, self.i = w, 0

    def take(self, n):
        out = self.w[self.i:self.i + n]
        assert len(out) == n, "ran off the end of the weights array"
        self.i += n
        return out

    def conv(self, out_ch, in_ch, ksize, bias=True):
        w = self.take(out_ch * in_ch * ksize).reshape(out_ch, in_ch, ksize)
        b = self.take(out_ch) if bias else np.zeros(out_ch)
        return w, b


def shift_for(peak, bits=15, headroom=0):
    """Smallest power-of-two shift with peak inside the signed range."""
    if peak <= 0:
        return -bits
    return int(np.ceil(np.log2(peak / (2.0 ** bits - 1)))) + headroom


def qi(x, shift, lo, hi):
    return np.clip(np.round(np.asarray(x) / (2.0 ** shift)), lo, hi).astype(np.int64)


def dilated_conv(x, w, b, dilation):
    out_ch, in_ch, k = w.shape
    hist = (k - 1) * dilation
    T = x.shape[1] - hist
    y = np.repeat(np.asarray(b)[:, None], T, axis=1)
    for kk in range(k):
        y += w[:, :, kk] @ x[:, kk * dilation: kk * dilation + T]
    return y


def activate(c, act):
    return np.tanh(c) if act == ACT_TANH else np.where(c > 0, c, LEAKY * c)


def pick_model(path):
    d = json.load(open(path))
    if d["architecture"] == "WaveNet":
        return d
    if d["architecture"] == "SlimmableContainer":
        subs = d["config"]["submodels"]
        best = max(subs, key=lambda s: s["model"]["config"]["layers"][0]["channels"])
        return best["model"]
    raise SystemExit(f"unsupported architecture {d['architecture']}")


def parse(model_path):
    m = pick_model(model_path)
    cfg = m["config"]
    if cfg.get("head") is not None:
        raise SystemExit("post-stack head is not supported")
    t = Taker(np.asarray(m["weights"], dtype=np.float64))

    groups = []
    for la in cfg["layers"]:
        ch = la["channels"]
        dils = la["dilations"]
        if "kernel_sizes" in la:
            ks_list = la["kernel_sizes"]
            act = ACT_LEAKY if "LeakyReLU" in json.dumps(la.get("activation")) \
                else ACT_TANH
            head = la["head"]
            head_size, head_k = head["out_channels"], head["kernel_size"]
            head_bias = bool(head.get("bias", True))
        else:
            ks_list = [la["kernel_size"]] * len(dils)
            act = ACT_TANH
            head_size, head_k = la["head_size"], 1
            head_bias = bool(la["head_bias"])

        g = dict(channels=ch, in_ch=la["input_size"], cond_ch=la["condition_size"],
                 head_size=head_size, head_ksize=head_k, head_bias=head_bias,
                 act=act)
        g["rechannel"] = t.conv(ch, la["input_size"], 1, bias=False)
        g["layers"] = [
            (d, k, t.conv(ch, ch, k, True),
             t.conv(ch, la["condition_size"], 1, False),
             t.conv(ch, ch, 1, True))
            for d, k in zip(dils, ks_list)
        ]
        g["head_conv"] = t.conv(head_size, ch, head_k, bias=head_bias)
        groups.append(g)

    head_scale = float(t.take(1)[0])
    assert t.i == len(t.w), f"weight layout mismatch: {t.i} of {len(t.w)}"
    return groups, head_scale


def calibrate(groups, x):
    """Float pass recording the peak magnitude of every activation tensor."""
    N = len(x)
    cond = x[None, :]
    layer_in = x[None, :]
    head = None
    peaks = []

    for g in groups:
        ch = g["channels"]
        rw, _ = g["rechannel"]
        z = rw[:, :, 0] @ layer_in
        gp = {"z0": np.max(np.abs(z)), "layers": []}
        head_acc = np.zeros((ch, N)) if head is None else head.copy()

        for (d, k, (cw, cb), (mw, _), (ow, ob)) in g["layers"]:
            hist = (k - 1) * d
            padded = np.concatenate([np.zeros((ch, hist)), z], axis=1)
            c = dilated_conv(padded, cw, cb, d) + mw[:, :, 0] @ cond
            a = activate(c, g["act"])
            head_acc += a
            z = z + (ow[:, :, 0] @ a + ob[:, None])
            gp["layers"].append({"c": np.max(np.abs(c)), "z": np.max(np.abs(z))})

        gp["head_acc"] = np.max(np.abs(head_acc))
        hw, hb = g["head_conv"]
        hk = g["head_ksize"]
        padded = np.concatenate([np.zeros((ch, hk - 1)), head_acc], axis=1)
        head = dilated_conv(padded, hw, hb, 1)
        gp["head"] = np.max(np.abs(head))
        peaks.append(gp)
        layer_in = z
    return peaks


def build(model_path, wav_path, out_path, headroom=0):
    groups, head_scale = parse(model_path)
    x, _ = load_wav(wav_path)
    peaks = calibrate(groups, x)

    grp_parts, lay_parts, w_parts = [], [], []
    s_cond = Q15
    prev_head_shift = None
    prev_z_shift = None

    for gi, (g, gp) in enumerate(zip(groups, peaks)):
        ch, act = g["channels"], g["act"]
        s_layer_in = Q15 if gi == 0 else prev_z_shift

        rw, _ = g["rechannel"]
        s_z = shift_for(gp["z0"], 15, headroom)
        s_wr, rshift_rechan = fit_shift(
            shift_for(np.max(np.abs(rw)), 15, headroom), s_layer_in, s_z)
        assert rshift_rechan >= 0, "rechannel needs a left shift"
        w_parts.append(qi(rw[:, :, 0], s_wr, -32768, 32767).astype("<i2").tobytes())

        # tanh always lands in Q15, LeakyReLU keeps its input's shift, so the
        # int32 skip accumulator runs at the finest activation shift in the group
        s_c_list = [shift_for(lp["c"], 15, headroom) for lp in gp["layers"]]
        s_a_list = [Q15 if act == ACT_TANH else sc for sc in s_c_list]
        s_ha_dom = min(s_a_list)

        layer_records = []
        for li, ((d, k, (cw, cb), (mw, _), (ow, ob)), lp) in enumerate(
                zip(g["layers"], gp["layers"])):
            s_c, s_a = s_c_list[li], s_a_list[li]
            s_wc, rshift_conv = fit_shift(
                shift_for(np.max(np.abs(cw)), 15, headroom), s_z, s_c)
            acc_conv = s_wc + s_z
            assert rshift_conv >= 0, f"g{gi} l{li}: conv needs a left shift"

            s_wm, rshift_mix = fit_shift(
                shift_for(np.max(np.abs(mw)), 15, headroom), s_cond, s_c)
            assert rshift_mix >= 0, f"g{gi} l{li}: mixin needs a left shift"

            s_z_next = max(shift_for(lp["z"], 15, headroom), s_z)
            s_w1, rshift_1x1 = fit_shift(
                shift_for(np.max(np.abs(ow)), 15, headroom), s_a, s_z_next)
            assert rshift_1x1 >= 0, f"g{gi} l{li}: 1x1 needs a left shift"

            w_parts.append(qi(cw, s_wc, -32768, 32767).astype("<i2").tobytes())
            w_parts.append(qi(cb, acc_conv, -(1 << 31), (1 << 31) - 1)
                           .astype("<i4").tobytes())
            w_parts.append(qi(mw[:, :, 0], s_wm, -32768, 32767).astype("<i2").tobytes())
            w_parts.append(qi(ow[:, :, 0], s_w1, -32768, 32767).astype("<i2").tobytes())
            w_parts.append(qi(ob, s_w1 + s_a, -(1 << 31), (1 << 31) - 1)
                           .astype("<i4").tobytes())

            layer_records.append(struct.pack("<IIiiiiii", d, k, rshift_conv,
                                             rshift_1x1, s_c, s_z_next - s_z,
                                             rshift_mix, s_a - s_ha_dom))
            s_z = s_z_next

        s_ha = shift_for(gp["head_acc"], 15, headroom)
        head_acc_rshift = s_ha - s_ha_dom
        assert head_acc_rshift >= 0

        hw, hb = g["head_conv"]
        s_head = shift_for(gp["head"], 15, headroom)
        s_wh, rshift_head = fit_shift(
            shift_for(np.max(np.abs(hw)), 15, headroom), s_ha, s_head)
        assert rshift_head >= 0, f"g{gi}: head needs a left shift"

        w_parts.append(qi(hw, s_wh, -32768, 32767).astype("<i2").tobytes())
        if g["head_bias"]:
            w_parts.append(qi(hb, s_wh + s_ha, -(1 << 31), (1 << 31) - 1)
                           .astype("<i4").tobytes())

        head_seed_lshift = 0 if gi == 0 else (prev_head_shift - s_ha_dom)
        assert head_seed_lshift >= 0

        grp_parts.append(struct.pack("<IIIIIIIIiiii", ch, g["in_ch"], g["cond_ch"],
                                     g["head_size"], len(g["layers"]),
                                     int(g["head_bias"]), g["head_ksize"], act,
                                     rshift_rechan, rshift_head,
                                     head_acc_rshift, head_seed_lshift))
        lay_parts.append(b"".join(layer_records))
        prev_head_shift = s_head
        prev_z_shift = s_z

    head_scale_q = int(round(head_scale * (2.0 ** (prev_head_shift - Q15)) * (1 << 30)))
    if not -(1 << 31) <= head_scale_q < (1 << 31):
        raise SystemExit("head_scale does not fit Q30")

    header = struct.pack("<IIiiiI", NQ_MAGIC, len(groups), s_cond,
                         prev_head_shift, head_scale_q, 0)

    idx = np.arange(TANH_N + 1)
    xs = (idx - TANH_N // 2) * (2.0 * TANH_RANGE / TANH_N)
    base = np.clip(np.round(np.tanh(xs) * 32767), -32768, 32767).astype("<i2")
    slope = (base[1:].astype(np.int32) - base[:-1].astype(np.int32)).astype("<i2")

    blob = bytearray()
    blob += header
    blob += b"".join(grp_parts)
    blob += b"".join(lay_parts)
    if len(blob) & 1:
        blob += b"\0"
    blob += base.tobytes()
    blob += slope.tobytes()
    while len(blob) & 3:
        blob += b"\0"
    blob += b"".join(w_parts)

    open(out_path, "wb").write(bytes(blob))
    print(f"wrote {out_path}: {len(blob)} bytes, {len(groups)} group(s), "
          f"{sum(len(g['layers']) for g in groups)} layers, "
          f"act={'leaky' if groups[0]['act'] else 'tanh'}, "
          f"head_k={groups[-1]['head_ksize']}, head_scale={head_scale:.6g}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    build(sys.argv[1], sys.argv[2], sys.argv[3],
          int(sys.argv[4]) if len(sys.argv) > 4 else 0)
