#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate FM-4's original 100-voice preset bank.

Every voice here is an original design for FM-4 (no Yamaha factory data).
Output: firmware/app/fm4/presets_bank.c (VMEM records) and
webapp/presets/FM-4_PRESETS.syx (TQ5 100-voice bank format).

Voices are described per TX81Z operator OP1..OP4 with these keys:
  r   ratio index CRS (0..63)  e.g. 4 = 1.00, 8 = 2.00, 13 = 4.00
  o   output level 0..99       a/d1/l1/d2/rr  AR, D1R, D1L, D2R, RR
  v   KVS 0..7   w  waveform 1..8   dt detune -3..3   fine 0..15
  fix fixed frequency (fix=(rg, crs, fine))   ame AM enable   ls level scaling
"""
import os, random

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPBLOCK = {1: 3, 2: 1, 3: 2, 4: 0}   # VCED/VMEM block for TX81Z operator n

def op(r=4, o=0, a=31, d1=10, l1=12, d2=2, rr=7, v=2, w=1, dt=0, fine=0, rs=1, ls=0, ame=0, fix=None, sh=0):
    return dict(r=r, o=o, a=a, d1=d1, l1=l1, d2=d2, rr=rr, v=v, w=w, dt=dt, fine=fine, rs=rs, ls=ls, ame=ame, fix=fix, sh=sh)

def voice(name, alg, fb, ops, lfs=30, lfd=0, pmd=0, amd=0, lfw=2, pms=0, ams=0, sync=0,
          mono=0, pbr=2, porta=0, mw_pitch=40, mw_amp=0, fx=(1, 18, 25), trps=24, rev=0):
    return dict(name=name, alg=alg, fb=fb, ops=ops, lfs=lfs, lfd=lfd, pmd=pmd, amd=amd, lfw=lfw,
                pms=pms, ams=ams, sync=sync, mono=mono, pbr=pbr, porta=porta, mw_pitch=mw_pitch,
                mw_amp=mw_amp, fx=fx, trps=trps, rev=rev)

def pack(vc):
    rec = [0] * 128
    for n in (1, 2, 3, 4):
        k = OPBLOCK[n]; p = vc['ops'][n - 1]; b = 10 * k
        rec[b + 0] = p['a'] & 31; rec[b + 1] = p['d1'] & 31; rec[b + 2] = p['d2'] & 31
        rec[b + 3] = max(1, p['rr']) & 15; rec[b + 4] = p['l1'] & 15; rec[b + 5] = p['ls']
        rec[b + 6] = (p['ame'] << 6) | (0 << 3) | (p['v'] & 7)
        rec[b + 7] = max(0, min(99, p['o']))
        fixed = p['fix'] is not None
        crs = (p['fix'][1] if fixed else p['r']) & 63
        fine = (p['fix'][2] if fixed else p['fine']) & 15
        if crs < 4: fine = min(fine, 7)
        rec[b + 8] = crs
        rec[b + 9] = ((p['rs'] & 3) << 3) | ((p['dt'] + 3) & 7)
        rec[73 + 2 * k] = ((0 if n == 1 else p['sh']) << 4) | ((1 if fixed else 0) << 3) | ((p['fix'][0] if fixed else 0) & 7)
        rec[74 + 2 * k] = ((p['w'] - 1) << 4) | fine
    rec[40] = (vc['sync'] << 6) | ((vc['fb'] & 7) << 3) | ((vc['alg'] - 1) & 7)
    rec[41] = vc['lfs']; rec[42] = vc['lfd']; rec[43] = vc['pmd']; rec[44] = vc['amd']
    rec[45] = ((vc['pms'] & 7) << 4) | ((vc['ams'] & 3) << 2) | (vc['lfw'] & 3)
    rec[46] = vc['trps']; rec[47] = vc['pbr']
    rec[48] = (vc['mono'] << 3) | (1 if vc['porta'] else 0)
    rec[49] = vc['porta']; rec[50] = 0
    rec[51] = vc['mw_pitch']; rec[52] = vc['mw_amp']; rec[53] = 0; rec[54] = 0; rec[55] = 50; rec[56] = 0
    nm = (vc['name'] + ' ' * 10)[:10]
    for i, c in enumerate(nm): rec[57 + i] = ord(c) & 127
    rec[81] = vc['rev'] & 7
    rec[91], rec[92], rec[93] = vc['fx']
    assert all(0 <= x < 128 for x in rec), vc['name']
    return rec

V = []
# ---- Electric & acoustic keys (10) ---------------------------------------
def epiano(name, tine_r, tine_o, body_d, w1=1, fx=(3, 16, 30), detune=1):
    return voice(name, 5, 0, [
        op(4, 93, 31, body_d, 8, 3, 7, 3, w1, dt=0, rs=2),
        op(4, 72, 31, 10, 6, 4, 7, 4, 1, dt=detune, rs=2),
        op(4, 86, 31, body_d + 2, 7, 3, 7, 3, 1, dt=-detune, rs=2),
        op(tine_r, tine_o, 31, 16, 2, 6, 8, 5, 1, rs=3)], lfs=33, pmd=0, amd=0, fx=fx)
V += [epiano('Glass Tine', 42, 58, 8), epiano('Soft Rhodo', 13, 52, 7, fx=(2, 14, 28)),
      epiano('Bell Keys', 49, 62, 9, fx=(1, 20, 32)), epiano('Mellow EP', 8, 46, 6, w1=2),
      epiano('Bright EP', 45, 66, 9, detune=2), epiano('Suitcase', 31, 55, 7, fx=(6, 30, 22))]
V += [voice('Clavinette', 1, 6, [op(4, 92, 31, 15, 4, 8, 9, 4, 3), op(13, 74, 31, 18, 3, 10, 9, 5, 1),
            op(8, 62, 31, 20, 2, 10, 9, 5, 1), op(4, 60, 31, 22, 1, 12, 9, 6, 2)], fx=(0, 0, 0)),
      voice('Toy Piano', 5, 2, [op(4, 92, 31, 12, 3, 6, 8, 3), op(17, 60, 31, 18, 1, 8, 8, 4),
            op(8, 82, 31, 14, 3, 6, 8, 3), op(21, 58, 31, 20, 1, 9, 8, 5)], fx=(2, 12, 25)),
      voice('Harpsy', 1, 5, [op(4, 92, 31, 11, 4, 5, 9, 2, 2), op(8, 76, 31, 13, 4, 6, 9, 3),
            op(13, 62, 31, 15, 3, 7, 9, 3, 3), op(4, 55, 31, 17, 2, 8, 9, 3, 1)], fx=(2, 16, 25)),
      voice('Vibrafonix', 5, 0, [op(4, 92, 31, 9, 6, 3, 6, 3, ame=1), op(4, 68, 31, 14, 2, 6, 6, 3),
            op(4, 84, 31, 9, 6, 3, 6, 3, ame=1), op(36, 54, 31, 18, 1, 8, 6, 4)],
            lfs=38, amd=22, lfw=2, ams=2, mw_amp=30, fx=(1, 22, 30))]
# ---- Basses (12) ------------------------------------------------------------
def bass(name, alg, fb, m1, m1o, m2, m2o, m3, m3o, w=1, d=12, mono=1, porta=0, fx=(0, 0, 0)):
    return voice(name, alg, fb, [op(4, 96, 31, 6, 13, 2, 9, 2, w), op(m1, m1o, 31, d, 6, 6, 9, 4),
                                 op(m2, m2o, 31, d + 3, 4, 7, 9, 4), op(m3, m3o, 31, d + 5, 3, 8, 9, 5)],
                 mono=mono, porta=porta, pbr=2, mw_pitch=20, fx=fx, trps=12)
V += [bass('Solid Bass', 1, 5, 4, 78, 4, 60, 8, 52), bass('Pick Bass', 4, 6, 8, 70, 4, 64, 13, 50, d=14),
      bass('Synbass 1', 1, 7, 4, 82, 4, 70, 4, 66, w=2), bass('Synbass 2', 2, 6, 4, 80, 8, 64, 0, 62, w=4),
      bass('Wood Bass', 4, 3, 4, 66, 8, 55, 13, 40, d=10), bass('Slap Pop', 1, 6, 13, 70, 8, 62, 22, 55, d=16),
      bass('Sub Rubber', 1, 2, 0, 74, 4, 60, 4, 50, w=1, d=8), bass('Acid Line', 1, 7, 4, 86, 4, 74, 4, 70, w=5, d=10, porta=18),
      bass('Fretless', 3, 4, 4, 70, 8, 58, 4, 50, d=8, porta=24), bass('Hollow Bas', 2, 5, 8, 72, 13, 60, 4, 58, w=3),
      bass('Steel Bass', 1, 6, 10, 74, 4, 64, 17, 56, d=15), bass('FM Wobble', 1, 7, 4, 80, 0, 70, 4, 64, w=2, d=6, fx=(4, 40, 18))]
V[-1]['pmd'] = 0; V[-1]['amd'] = 0
# ---- Brass & winds (12) ---------------------------------------------------
def brass(name, attack, bright, alg=4, w=1, fx=(1, 18, 22), vib=0):
    return voice(name, alg, 6, [op(4, 94, attack, 6, 13, 2, 7, 2, w),
                                op(4, 70 + bright // 3, attack - 2, 8, 10, 3, 7, 3, 1, dt=1),
                                op(4, bright, attack - 3, 9, 9, 3, 7, 4, 1, dt=-1),
                                op(8, bright - 8, attack - 4, 10, 8, 3, 7, 4, 1)],
                 lfs=34, lfd=40, pmd=vib, pms=3 if vib else 0, mw_pitch=45, fx=fx)
V += [brass('Brass Sect', 20, 78), brass('Soft Horns', 16, 66, w=2), brass('Trumpetish', 22, 82, vib=6),
      brass('Big Synbrs', 21, 84, alg=2), brass('Brass Fall', 24, 80), brass('Stab Brass', 26, 86, fx=(2, 12, 18))]
V += [voice('Flute Air', 5, 0, [op(4, 94, 16, 4, 14, 1, 7, 2), op(4, 56, 18, 6, 10, 2, 7, 3, 1, dt=1),
            op(13, 40, 24, 14, 4, 6, 7, 2, 1), op(0, 30, 31, 31, 0, 0, 7, 0, 1, fix=(7, 60, 15))],
            lfs=36, lfd=50, pmd=8, pms=3, fx=(1, 20, 28)),
      voice('Reedy Lead', 1, 5, [op(4, 92, 20, 5, 13, 1, 7, 2, 1), op(4, 76, 20, 6, 11, 2, 7, 3, 4),
            op(8, 60, 22, 8, 9, 2, 7, 3), op(4, 54, 24, 10, 8, 2, 7, 3)], lfs=36, lfd=45, pmd=7, pms=3, fx=(2, 14, 20)),
      voice('Clarinetto', 1, 3, [op(4, 92, 20, 3, 14, 1, 7, 2, 1), op(8, 62, 20, 6, 12, 1, 7, 2, 3),
            op(4, 58, 20, 8, 11, 2, 7, 3), op(4, 40, 22, 12, 6, 2, 7, 3)], lfs=36, lfd=50, pmd=6, pms=3),
      voice('Pan Pipe', 5, 0, [op(4, 92, 14, 6, 13, 2, 6, 2), op(8, 50, 17, 8, 8, 3, 6, 2),
            op(4, 70, 18, 10, 9, 3, 6, 2), op(0, 38, 31, 18, 2, 6, 6, 1, fix=(6, 63, 15))],
            lfs=34, lfd=40, pmd=5, pms=3, fx=(1, 26, 34)),
      voice('Harmonica', 1, 6, [op(4, 92, 22, 5, 13, 1, 7, 2, 2), op(4, 74, 22, 6, 12, 2, 7, 3, 2),
            op(10, 56, 22, 8, 10, 2, 7, 3), op(4, 50, 22, 8, 9, 2, 7, 3)], lfs=50, pmd=4, pms=4, mono=1),
      voice('Oboe Tone', 1, 4, [op(4, 92, 21, 4, 14, 1, 7, 2, 4), op(8, 72, 21, 6, 12, 1, 7, 2),
            op(13, 54, 22, 8, 10, 2, 7, 2), op(4, 48, 22, 10, 9, 2, 7, 3)], lfs=36, lfd=48, pmd=6, pms=3)]
# ---- Bells & mallets (10) ------------------------------------------------------
def bell(name, r_mod, mod_o, r_car2, w=1, decay=4, fx=(1, 28, 34), alg=5):
    return voice(name, alg, 0, [op(4, 94, 31, decay, 0, 2, 4, 2, w, rs=2), op(r_mod, mod_o, 31, decay + 2, 0, 3, 4, 3, rs=2),
                                op(r_car2, 82, 31, decay + 1, 0, 2, 4, 2, 1, dt=2, rs=2),
                                op(r_mod + 3 if r_mod < 60 else r_mod, mod_o - 6, 31, decay + 3, 0, 3, 4, 3, rs=2)], fx=fx)
V += [bell('Tubular', 17, 62, 12), bell('Chime Tree', 27, 66, 41), bell('Glockenfm', 45, 54, 25, decay=6),
      bell('Gamelan', 21, 70, 14, w=2), bell('Crystal', 49, 58, 36, w=1, decay=3), bell('Marimboid', 13, 58, 25, decay=10, fx=(2, 14, 22)),
      bell('Kalimbak', 32, 52, 8, decay=12, fx=(2, 12, 24)), bell('Steel Drum', 10, 64, 21, decay=8, w=2, fx=(2, 16, 26)),
      bell('Church Bel', 12, 72, 17, decay=3, fx=(1, 38, 40)), bell('Music Box', 42, 50, 36, decay=7, fx=(3, 22, 34))]
# ---- Organs (8) ----------------------------------------------------------------
def organ(name, ratios, levels, w=(1, 1, 1, 1), perc=0, fx=(4, 20, 18), am=0):
    ops = [op(ratios[i], levels[i], 31, 0, 15, 0, 9, 0, w[i], ame=1 if am else 0) for i in range(4)]
    if perc:
        ops[3] = op(ratios[3], levels[3], 31, 14, 0, 6, 9, 1, w[3])
    return voice(name, 8, 0, ops, lfs=72, amd=am, ams=1 if am else 0, lfw=2, mw_pitch=10, fx=fx)
V += [organ('Drawbar 1', (4, 8, 0, 13), (90, 80, 76, 60)), organ('Jazz Perc', (4, 8, 0, 10), (90, 74, 70, 82), perc=1),
      organ('Full Organ', (0, 4, 8, 13), (84, 88, 80, 74), w=(1, 1, 2, 2)), organ('Church Pipe', (0, 4, 8, 25), (88, 84, 72, 56), fx=(1, 40, 40)),
      organ('Reed Organ', (4, 8, 10, 13), (88, 70, 60, 50), w=(3, 3, 1, 1)), organ('Rotary Org', (4, 8, 0, 13), (88, 80, 74, 62), am=18),
      organ('Combo Org', (4, 8, 13, 25), (90, 76, 62, 50), w=(4, 4, 1, 1)), organ('Flute Stop', (4, 8, 0, 4), (90, 60, 50, 0))]
# ---- Strings & pads (14) ------------------------------------------------------
def pad(name, attack, release, alg=6, w=(1, 1, 1, 1), bright=58, det=2, lfo=(30, 6, 3), fx=(1, 30, 40), d2=1):
    return voice(name, alg, 3, [op(4, 92, attack, 4, 13, d2, release, 1, w[0], dt=det),
                                op(4, 88, attack, 4, 13, d2, release, 1, w[1], dt=-det),
                                op(8 if alg != 8 else 4, 84, attack, 4, 13, d2, release, 1, w[2], dt=det - 1),
                                op(4, bright, attack + 1, 6, 10, 2, release, 2, w[3])],
                 lfs=lfo[0], pmd=lfo[1], pms=lfo[2], mw_pitch=40, fx=fx)
V += [pad('Str Ensmbl', 14, 6), pad('Slow Strng', 10, 5, w=(2, 2, 1, 1)), pad('Warm Pad', 11, 5, alg=5, bright=52),
      pad('Glass Pad', 12, 5, w=(5, 5, 1, 1), bright=60), pad('Choir Ahh', 12, 5, alg=7, w=(3, 1, 3, 1), bright=50, lfo=(34, 10, 3)),
      pad('Sweep Pad', 9, 4, alg=5, w=(2, 2, 2, 7), bright=66, lfo=(14, 20, 4)), pad('Space Pad', 8, 3, w=(1, 6, 1, 1), fx=(9, 40, 44)),
      pad('Hollow Pad', 10, 5, w=(3, 3, 3, 1), alg=8), pad('Pizz Strng', 31, 7, alg=6, d2=10, bright=56, fx=(1, 18, 30)),
      pad('Synth Strs', 15, 6, w=(2, 2, 2, 1), det=3), pad('Dark Pad', 9, 4, alg=5, bright=44, w=(1, 1, 1, 3)),
      pad('Air Voices', 11, 4, alg=7, w=(3, 3, 1, 1), lfo=(40, 12, 3), fx=(1, 36, 44)),
      pad('Shimmer', 11, 5, w=(5, 7, 5, 1), bright=60, fx=(6, 50, 36)), pad('Cello Ens', 14, 6, bright=62, det=1)]
for p in V[-14:]: p['trps'] = 24
V[-1]['trps'] = 12
# ---- Leads (10) -----------------------------------------------------------------
def lead(name, alg, fb, w=(1, 2, 1, 1), bright=74, porta=0, fx=(4, 30, 24), vib=(36, 40, 7, 3)):
    return voice(name, alg, fb, [op(4, 94, 31, 4, 13, 1, 8, 2, w[0]), op(4, bright, 31, 6, 11, 2, 8, 3, w[1], dt=1),
                                 op(8, bright - 8, 31, 7, 10, 2, 8, 3, w[2]), op(4, bright - 12, 31, 8, 9, 2, 8, 3, w[3])],
                 mono=1, porta=porta, lfs=vib[0], lfd=vib[1], pmd=vib[2], pms=vib[3], mw_pitch=50, fx=fx)
V += [lead('Sync Lead', 1, 7, bright=80), lead('Square Ld', 2, 6, w=(4, 4, 1, 1)), lead('Saw Lead', 1, 7, w=(1, 2, 2, 1), bright=84),
      lead('Glide Lead', 1, 6, porta=30), lead('Whistle', 5, 0, w=(1, 1, 1, 1), bright=40, vib=(40, 30, 10, 3), fx=(1, 24, 30)),
      lead('Fuzz Lead', 1, 7, w=(5, 2, 2, 2), bright=86, fx=(8, 30, 30)), lead('Soft Lead', 4, 4, w=(2, 1, 1, 1), bright=64),
      lead('Chiplead', 1, 5, w=(4, 3, 1, 1), bright=70, vib=(60, 0, 10, 4), fx=(0, 0, 0)),
      lead('Ocarinaish', 5, 0, w=(1, 3, 1, 1), bright=48, fx=(1, 20, 26)), lead('Dist Guitr', 1, 7, w=(2, 2, 1, 1), bright=88, fx=(7, 30, 40))]
# ---- Plucked & guitar (8) ----------------------------------------------------------
def pluck(name, alg, r2, o2, d, w=1, fx=(2, 16, 24), fb=4):
    return voice(name, alg, fb, [op(4, 94, 31, d, 2, 6, 8, 3, w, rs=2), op(r2, o2, 31, d + 3, 1, 8, 8, 4, rs=2),
                                 op(4, 70, 31, d + 4, 1, 8, 8, 4, rs=2), op(8, 60, 31, d + 6, 0, 9, 8, 5, rs=2)], fx=fx)
V += [pluck('Nylon Gtr', 4, 8, 66, 10), pluck('Steel Gtr', 1, 13, 70, 9, fx=(1, 18, 26)), pluck('Harp Pluck', 5, 8, 58, 8, fx=(1, 26, 34)),
      pluck('Koto Taste', 4, 17, 66, 9, w=2), pluck('Banjoesque', 1, 22, 72, 13, w=3), pluck('Muted Gtr', 1, 8, 70, 18, fx=(0, 0, 0)),
      pluck('Sitar Buzz', 1, 18, 74, 6, w=4, fb=7), pluck('Pluck Synt', 1, 4, 76, 12, w=2, fx=(6, 34, 28))]
# ---- Percussion (8) ---------------------------------------------------------------
def perc(name, alg, fb, ops, fx=(0, 0, 0)):
    return voice(name, alg, fb, ops, fx=fx)
V += [perc('Kick Drum', 1, 0, [op(0, 96, 31, 20, 0, 12, 12, 2, fix=(3, 32, 0)), op(0, 70, 31, 24, 0, 14, 12, 2, fix=(4, 40, 0)),
                                op(4, 0, 31, 31, 0, 0, 12), op(4, 0, 31, 31, 0, 0, 12)]),
      perc('Snare FM', 7, 7, [op(0, 90, 31, 20, 0, 13, 11, 3, fix=(4, 52, 0)), op(0, 84, 31, 24, 0, 14, 11, 3, fix=(6, 60, 8)),
                               op(0, 70, 31, 22, 0, 14, 11, 3, fix=(7, 63, 15)), op(0, 92, 31, 18, 0, 14, 11, 3, fix=(7, 63, 15))], fx=(2, 10, 20)),
      perc('Hi Hat', 7, 7, [op(0, 76, 31, 26, 0, 16, 13, 3, fix=(7, 63, 15)), op(0, 70, 31, 26, 0, 16, 13, 3, fix=(7, 50, 7)),
                             op(0, 62, 31, 26, 0, 16, 13, 3, fix=(7, 44, 3)), op(0, 99, 31, 31, 0, 31, 13, 3, fix=(7, 63, 15))]),
      perc('Tom Tom', 1, 3, [op(0, 94, 31, 16, 0, 10, 10, 2, fix=(4, 36, 0)), op(0, 60, 31, 20, 0, 12, 10, 3, fix=(5, 44, 0)),
                              op(4, 0, 31, 31, 0, 0, 10), op(4, 0, 31, 31, 0, 0, 10)], fx=(1, 12, 22)),
      perc('Cowbelloid', 5, 0, [op(0, 90, 31, 14, 0, 9, 10, 2, fix=(6, 32, 4)), op(18, 66, 31, 18, 0, 10, 10, 2),
                                 op(0, 84, 31, 14, 0, 9, 10, 2, fix=(6, 47, 2)), op(25, 60, 31, 18, 0, 10, 10, 2)]),
      perc('Clap Gate', 8, 7, [op(0, 86, 31, 18, 0, 16, 12, 3, fix=(6, 63, 15)), op(0, 82, 31, 19, 0, 16, 12, 3, fix=(7, 60, 9)),
                                op(0, 80, 31, 20, 0, 16, 12, 3, fix=(7, 55, 5)), op(0, 90, 31, 17, 0, 16, 12, 3, fix=(7, 63, 15))], fx=(9, 12, 50)),
      perc('Timbale', 5, 2, [op(0, 92, 31, 15, 0, 10, 10, 2, fix=(5, 48, 0)), op(13, 60, 31, 19, 0, 12, 10, 3),
                              op(0, 80, 31, 16, 0, 10, 10, 2, fix=(5, 60, 0)), op(10, 54, 31, 20, 0, 12, 10, 3)], fx=(1, 14, 22)),
      perc('Woodblock', 1, 0, [op(0, 94, 31, 24, 0, 16, 12, 2, fix=(5, 60, 8)), op(0, 70, 31, 26, 0, 16, 12, 2, fix=(6, 44, 0)),
                                op(4, 0, 31, 31, 0, 0, 12), op(4, 0, 31, 31, 0, 0, 12)])]
# ---- Synth & effects (8) ----------------------------------------------------------
V += [voice('Poly Synth', 4, 5, [op(4, 94, 31, 6, 11, 2, 7, 2, 2), op(4, 76, 31, 8, 9, 3, 7, 3, 1, dt=2),
            op(4, 72, 31, 9, 8, 3, 7, 3, 1, dt=-2), op(8, 60, 31, 10, 7, 3, 7, 3)], fx=(6, 34, 30)),
      voice('Hoover Fm', 1, 7, [op(4, 92, 26, 4, 12, 2, 6, 2, 2), op(1, 80, 26, 6, 10, 2, 6, 3, 2, dt=3),
            op(4, 74, 26, 6, 10, 2, 6, 3, 1, dt=-3), op(0, 70, 24, 8, 9, 2, 6, 3, 1)], lfs=40, pmd=14, pms=4, mono=1, porta=20, fx=(7, 30, 30)),
      voice('Laser Zap', 1, 7, [op(4, 92, 31, 14, 0, 10, 10, 1), op(63, 90, 31, 12, 0, 10, 10, 1),
            op(4, 80, 31, 16, 0, 10, 10, 1), op(4, 70, 31, 18, 0, 10, 10, 1)], lfs=90, pmd=99, pms=7, lfw=0, sync=1, fx=(6, 40, 30)),
      voice('Wind Noise', 7, 7, [op(0, 70, 8, 2, 12, 2, 4, 0, 1, fix=(7, 63, 15)), op(0, 80, 8, 2, 12, 2, 4, 0, 1, fix=(7, 60, 9)),
            op(0, 60, 8, 2, 12, 2, 4, 0, 1, fix=(6, 50, 3)), op(0, 99, 31, 0, 15, 0, 4, 0, 1, fix=(7, 63, 15))],
            lfs=12, amd=40, ams=2, lfw=3, fx=(1, 40, 50)),
      voice('Sci-Fi Arp', 5, 4, [op(4, 92, 31, 12, 3, 8, 7, 2, 5), op(30, 70, 31, 14, 2, 8, 7, 3),
            op(13, 84, 31, 12, 3, 8, 7, 2, 7), op(41, 60, 31, 16, 2, 8, 7, 3)], fx=(6, 45, 40)),
      voice('Drone Bed', 6, 5, [op(0, 86, 6, 2, 14, 1, 3, 0, 1, dt=2), op(4, 84, 6, 2, 14, 1, 3, 0, 3, dt=-2),
            op(8, 80, 6, 2, 14, 1, 3, 0, 1, dt=1), op(5, 62, 4, 2, 13, 1, 3, 0, 2)],
            lfs=8, pmd=20, pms=2, fx=(1, 40, 50)),
      voice('Metallic', 5, 6, [op(4, 92, 31, 8, 4, 4, 6, 2), op(37, 72, 31, 10, 3, 5, 6, 3, 2),
            op(17, 82, 31, 9, 4, 4, 6, 2), op(55, 66, 31, 11, 3, 5, 6, 3)], fx=(10, 30, 40)),
      voice('Init Voice', 1, 0, [op(4, 90, 31, 31, 15, 0, 15, 0), op(4, 0, 31, 31, 15, 0, 15, 0),
            op(4, 0, 31, 31, 15, 0, 15, 0), op(4, 0, 31, 31, 15, 0, 15, 0)], lfs=35, pms=6, sync=1, mw_pitch=50, fx=(1, 20, 0))]

assert len(V) == 100, len(V)
names = [v['name'] for v in V]
assert len(set(names)) == 100, [n for n in names if names.count(n) > 1]
records = [pack(v) for v in V]

out_c = os.path.join(ROOT, 'firmware/app/fm4/presets_bank.c')
with open(out_c, 'w') as f:
    f.write('/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception\n'
            ' * FM-4 original preset bank (100 voices, VMEM records).\n'
            ' * Generated by tools/gen_presets.py - do not edit by hand. */\n'
            '#include "presets_bank.h"\n\n'
            'const uint8_t fm4_preset_bank[FM4_PRESETS][128] = {\n')
    for v, rec in zip(V, records):
        f.write('    /* %-10s */ {' % v['name'] + ','.join(str(x) for x in rec) + '},\n')
    f.write('};\n')
with open(os.path.join(ROOT, 'firmware/app/fm4/presets_bank.h'), 'w') as f:
    f.write('/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */\n'
            '#ifndef FM4_PRESETS_BANK_H\n#define FM4_PRESETS_BANK_H\n#include <stdint.h>\n'
            '#define FM4_PRESETS 100\nextern const uint8_t fm4_preset_bank[FM4_PRESETS][128];\n#endif\n')

def checksum(data):
    return (-sum(data)) & 0x7f

syx = bytearray()
for b in range(4):
    syx += bytes([0xF0, 0x43, 0x10, 0x24, 0x07, b + 1, 0xF7])
    block = records[25 * b: 25 * b + 25]
    block += [records[25 * b]] * 7
    data = bytes(x for rec in block for x in rec)
    syx += bytes([0xF0, 0x43, 0x00, 0x04, 0x20, 0x00]) + data + bytes([checksum(data), 0xF7])
os.makedirs(os.path.join(ROOT, 'webapp/presets'), exist_ok=True)
with open(os.path.join(ROOT, 'webapp/presets/FM-4_PRESETS.syx'), 'wb') as f:
    f.write(syx)
print('wrote', out_c, 'and FM-4_PRESETS.syx (%d bytes)' % len(syx))
