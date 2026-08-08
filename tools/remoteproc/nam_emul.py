#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Emulate namwn_skel.c's integer arithmetic from an NQW2 blob, in numpy.

Splits the two possible causes of a bad DSP result, which is what made the
original port debuggable:

  emulator matches the float render  -> the blob is fine, the C is wrong
  emulator is also wrong             -> nam_quantize.py is wrong

It therefore has to mirror the skel *exactly*, including where it truncates.
Notably every requantise is a plain arithmetic shift with no rounding, and the
dilated conv and the condition mixin share one accumulator before a single
requantise - splitting them costs about 14 dB, because the mixin partially
cancels the conv so the conv alone swings wider than their sum.

Usage: nam_emul.py model.nqw in.wav ref.wav [dsp_out.wav]
"""

import struct
import sys

import numpy as np

HDR = "<IIiiiI"
GRP = "<IIIIIIIIiiii"
LAY = "<IIiiiiii"

NQ_MAGIC = 0x3257514E     # "NQW2"
TANH_N = 4096
TANH_FRAC = 6             # one segment is exactly 2^TANH_FRAC in Q(TANH_FRAC+8)
TANH_Q = 14
LEAKY_Q15 = 328           # 0.01 in Q15

ACT_TANH, ACT_LEAKY = 0, 1

GRP_FIELDS = ("channels in_ch cond_ch head_size n_layers head_bias head_ksize "
              "act_type rshift_rechan rshift_head head_acc_rshift "
              "head_seed_lshift").split()
LAY_FIELDS = ("dilation ksize rshift_conv rshift_1x1 s_c rshift_z rshift_mix "
              "lshift_a").split()


def sat16(v):
    return np.clip(v, -32768, 32767).astype(np.int64)


def load_blob(path):
    b = open(path, "rb").read()
    off = 0
    magic, n_groups, cond_shift, head_shift, head_scale_q, _ = \
        struct.unpack_from(HDR, b, off)
    off += struct.calcsize(HDR)
    if magic != NQ_MAGIC:
        raise SystemExit(f"bad magic {magic:#x}; this reads NQW2 blobs only")

    groups = []
    for _ in range(n_groups):
        groups.append(dict(zip(GRP_FIELDS, struct.unpack_from(GRP, b, off))))
        off += struct.calcsize(GRP)

    for g in groups:
        g["layers"] = []
        for _ in range(g["n_layers"]):
            g["layers"].append(dict(zip(LAY_FIELDS,
                                        struct.unpack_from(LAY, b, off))))
            off += struct.calcsize(LAY)

    if off & 1:
        off += 1
    base = np.frombuffer(b, "<i2", count=TANH_N + 1, offset=off).astype(np.int64)
    off += (TANH_N + 1) * 2
    slope = np.frombuffer(b, "<i2", count=TANH_N, offset=off).astype(np.int64)
    off += TANH_N * 2
    off = (off + 3) & ~3        # int32 biases live in the int16 weight stream

    return b, off, groups, cond_shift, head_shift, head_scale_q, base, slope


def tanh_lut(c, s_c, base, slope):
    to_q = s_c + TANH_Q
    v = c.astype(np.int64)
    v = v << to_q if to_q >= 0 else v >> (-to_q)
    lim = (TANH_N // 2) << TANH_FRAC
    out = np.zeros_like(v)
    hi, lo = v >= lim - 1, v <= -lim
    mid = ~(hi | lo)
    out[hi], out[lo] = 32767, -32767
    vm = v[mid]
    idx = (vm >> TANH_FRAC) + TANH_N // 2
    frac = vm & ((1 << TANH_FRAC) - 1)
    out[mid] = sat16(base[idx] + ((slope[idx] * frac) >> TANH_FRAC))
    return out


def leaky(c):
    """LeakyReLU keeping the input's shift, as apply_leaky() does."""
    v = c.astype(np.int64)
    return np.where(v >= 0, v, sat16((v * LEAKY_Q15 + (1 << 14)) >> 15))


def conv_acc(w, bias, x, ksize, dil, out_ch, in_ch, n):
    """Raw int32-domain accumulator, before any requantise."""
    acc = np.zeros((out_ch, n), dtype=np.int64)
    for o in range(out_ch):
        for i in range(in_ch):
            wr = w[(o * in_ch + i) * ksize:(o * in_ch + i + 1) * ksize]
            for k in range(ksize):
                acc[o] += int(wr[k]) * x[i, k * dil: k * dil + n]
        if bias is not None:
            acc[o] += int(bias[o])
    return acc


def conv(w, bias, x, ksize, dil, rsh, out_ch, in_ch, n):
    return sat16(conv_acc(w, bias, x, ksize, dil, out_ch, in_ch, n) >> rsh)


def pad(x, hist, ch):
    return np.concatenate([np.zeros((ch, hist), dtype=np.int64), x], axis=1)


def run(blob_path, wav_path):
    b, woff, groups, _cs, _hs, head_scale_q, tb, ts = load_blob(blob_path)
    w16 = np.frombuffer(b, "<i2", offset=woff)
    w32 = np.frombuffer(b, "<i4", offset=woff)
    p = 0                       # cursor in int16 units

    import importlib.util
    spec = importlib.util.spec_from_file_location("nam_ref", "nam_ref.py")
    R = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(R)
    x, _ = R.load_wav(wav_path)
    n = len(x)

    # Match nam_dsp_run.c's (v + 128) >> 8 exactly: round half *up*, not
    # numpy's half-to-even, or ties disagree and the comparison stops being
    # bit-exact for no useful reason.
    cond = sat16(np.floor(x * 32768.0 + 0.5))[None, :]
    layer_in = cond
    head_out = None

    for gi, G in enumerate(groups):
        ch, in_ch = G["channels"], G["in_ch"]

        rw = w16[p:p + ch * in_ch]
        p += ch * in_ch
        z = conv(rw, None, layer_in, 1, 1, G["rshift_rechan"], ch, in_ch, n)

        head_acc = np.zeros((ch, n), dtype=np.int64)
        if gi > 0:
            k = min(ch, groups[gi - 1]["head_size"])
            head_acc[:k] = head_out[:k] << G["head_seed_lshift"]

        for L in G["layers"]:
            ks, d = L["ksize"], L["dilation"]

            cw = w16[p:p + ch * ch * ks]; p += ch * ch * ks
            cb = w32[p // 2: p // 2 + ch]; p += ch * 2
            mw = w16[p:p + ch * G["cond_ch"]]; p += ch * G["cond_ch"]
            ow = w16[p:p + ch * ch]; p += ch * ch
            ob = w32[p // 2: p // 2 + ch]; p += ch * 2

            # conv and mixin share one accumulator, brought to the coarser of
            # the two domains (the smaller requantise shift) before adding
            rs_out = min(L["rshift_conv"], L["rshift_mix"])
            ac = conv_acc(cw, cb, pad(z, (ks - 1) * d, ch), ks, d, ch, ch, n)
            am = conv_acc(mw, None, cond, 1, 1, ch, G["cond_ch"], n)
            ac >>= L["rshift_conv"] - rs_out
            am >>= L["rshift_mix"] - rs_out
            c = sat16((ac + am) >> rs_out)

            a = leaky(c) if G["act_type"] == ACT_LEAKY \
                else tanh_lut(c, L["s_c"], tb, ts)

            head_acc += a << L["lshift_a"]
            z = sat16((z >> L["rshift_z"])
                      + conv(ow, ob, a, 1, 1, L["rshift_1x1"], ch, ch, n))

        hin = sat16(head_acc >> G["head_acc_rshift"])
        hk, hs = G["head_ksize"], G["head_size"]
        hw = w16[p:p + hs * ch * hk]; p += hs * ch * hk
        hb = None
        if G["head_bias"]:
            hb = w32[p // 2: p // 2 + hs]; p += hs * 2
        head_out = conv(hw, hb, pad(hin, hk - 1, ch), hk, 1,
                        G["rshift_head"], hs, ch, n)
        layer_in = z

    out = sat16((head_out[0] * head_scale_q) >> 30)
    return out.astype(np.float64) / 32768.0


if __name__ == "__main__":
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)

    out = run(sys.argv[1], sys.argv[2])

    import importlib.util
    spec = importlib.util.spec_from_file_location("nam_ref", "nam_ref.py")
    R = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(R)

    ref, _ = R.load_wav(sys.argv[3])
    m = min(len(out), len(ref))
    a, b_ = out[12000:m], ref[12000:m]
    e = a - b_
    print(f"emulator vs float: SNR "
          f"{10 * np.log10(np.sum(b_ ** 2) / max(np.sum(e ** 2), 1e-30)):.1f} dB   "
          f"corr {np.dot(a, b_) / np.sqrt(np.dot(a, a) * np.dot(b_, b_) + 1e-30):.4f}")

    if len(sys.argv) > 4:
        d, _ = R.load_wav(sys.argv[4])
        m2 = min(len(out), len(d))
        diff = out[:m2] - d[:m2]
        print(f"emulator vs DSP:   max|diff| {np.max(np.abs(diff)):.6f}  "
              f"exact match: {np.max(np.abs(diff)) == 0}")
