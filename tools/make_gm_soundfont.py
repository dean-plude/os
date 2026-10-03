#!/usr/bin/env python3
"""Write NovaOS's General MIDI soundfont (an SF2 file) from synthesized waves.

winmm's MIDI synthesizer (TinySoundFont) plays General MIDI with it: all
128 programs of bank 0, grouped by GM family onto a handful of looped
single-cycle waveforms with their own envelopes, filters and vibrato, and
a drum kit (bank 128) built from tones with falling pitch and filtered
noise.  Everything is generated here, so the file is NovaOS's own (MIT);
it is small and sounds like an early wavetable card.  A better .sf2 can
replace it at C:\\Windows\\System32\\drivers\\gm.sf2.

    make_gm_soundfont.py OUT.sf2
"""
import math, random, struct, sys

RATE_CYCLE = 64                      # frames per cycle at the root key
ROOT = 60                            # middle C
CYCLES = 16                          # cycles per looped sample


def tc(seconds):
    """seconds as SF2 timecents"""
    return int(round(1200 * math.log2(max(seconds, 0.001))))


def cycle_rate():
    return round(440 * 2 ** ((ROOT - 69) / 12) * RATE_CYCLE)


# --------------------------------------------------------------------------
# Waveforms: one period, sampled at RATE_CYCLE points, as additive partials
# --------------------------------------------------------------------------
def additive(amps, phases=None):
    out = []
    for i in range(RATE_CYCLE):
        t = i / RATE_CYCLE
        out.append(sum(a * math.sin(2 * math.pi * (h + 1) * t + (phases[h] if phases else 0))
                       for h, a in enumerate(amps) if a))
    return out


def band_limited(kind, harmonics=24):
    if kind == 'saw':
        amps = [1 / n for n in range(1, harmonics + 1)]
    elif kind == 'square':
        amps = [1 / n if n % 2 else 0 for n in range(1, harmonics + 1)]
    elif kind == 'triangle':
        amps = [(1 / n ** 2) * (-1) ** ((n - 1) // 2) if n % 2 else 0 for n in range(1, harmonics + 1)]
    elif kind == 'pulse':          # 25 % duty
        amps = [abs(math.sin(math.pi * n * 0.25)) / n for n in range(1, harmonics + 1)]
    else:
        amps = [1]
    return additive(amps)


WAVES = {
    'sine':     additive([1]),
    'triangle': band_limited('triangle'),
    'square':   band_limited('square'),
    'saw':      band_limited('saw'),
    'pulse':    band_limited('pulse'),
    'organ':    additive([1, 0.6, 0.3, 0.45, 0, 0.25, 0, 0.2]),
    'reed':     additive([0.5, 1, 0.8, 0.5, 0.45, 0.3, 0.2, 0.15, 0.1]),
    'brass':    additive([1, 0.85, 0.7, 0.55, 0.45, 0.35, 0.28, 0.22, 0.17, 0.13, 0.1]),
    'piano':    additive([1, 0.45, 0.25, 0.18, 0.1, 0.07, 0.05, 0.03]),
    'pluck':    additive([1, 0.7, 0.45, 0.35, 0.25, 0.2, 0.15, 0.1, 0.08, 0.06]),
    'bell':     additive([1, 0, 0.6, 0, 0, 0.4, 0, 0, 0, 0.25]),
    'flute':    additive([1, 0.2, 0.08, 0.03]),
}


def normalize(w, peak=0.9):
    m = max(abs(x) for x in w) or 1
    return [x / m * peak for x in w]


def looped(wave):
    w = normalize(wave)
    return [int(32767 * w[i % RATE_CYCLE]) for i in range(RATE_CYCLE * CYCLES)]


def noise(n, seed=7):
    r = random.Random(seed)
    return [int(r.uniform(-1, 1) * 29000) for _ in range(n)]


# --------------------------------------------------------------------------
# Instruments
# --------------------------------------------------------------------------
# GM families (8 programs each): wave, attack, decay, sustain (dB down),
# release (seconds), filter cutoff (Hz, 0 = open), vibrato depth (cents)
FAMILIES = [
    ('piano',    0.002, 2.5, 30, 0.4, 5000, 0),    # piano
    ('bell',     0.002, 1.6, 40, 0.6, 0, 0),       # chromatic percussion
    ('organ',    0.01, 0.1, 1, 0.08, 0, 6),        # organ
    ('pluck',    0.002, 1.8, 36, 0.3, 4000, 0),    # guitar
    ('triangle', 0.004, 1.2, 24, 0.15, 1500, 0),   # bass
    ('saw',      0.12, 0.5, 4, 0.4, 3000, 12),     # strings
    ('saw',      0.25, 0.8, 6, 0.6, 2200, 8),      # ensemble
    ('brass',    0.04, 0.4, 6, 0.15, 3500, 6),     # brass
    ('reed',     0.03, 0.4, 4, 0.12, 3000, 8),     # reed
    ('flute',    0.04, 0.4, 3, 0.15, 0, 10),       # pipe
    ('square',   0.01, 0.5, 8, 0.2, 4000, 0),      # synth lead
    ('saw',      0.3, 1.0, 6, 0.8, 1500, 10),      # synth pad
    ('pulse',    0.05, 1.0, 12, 0.5, 2500, 15),    # synth effects
    ('pluck',    0.002, 1.2, 40, 0.25, 3500, 0),   # ethnic
    ('sine',     0.001, 0.4, 60, 0.15, 0, 0),      # percussive
    ('noise',    0.05, 1.0, 20, 0.5, 3000, 0),     # sound effects
]
# a few programs differ from their family
OVERRIDES = {
    6: ('pluck', 0.002, 1.2, 40, 0.3, 4500, 0),    # harpsichord
    8: ('bell', 0.002, 1.0, 40, 0.5, 0, 0),        # celesta
    11: ('sine', 0.002, 1.5, 40, 0.5, 0, 0),       # vibraphone
    12: ('sine', 0.002, 0.6, 60, 0.3, 0, 0),       # marimba
    13: ('triangle', 0.002, 0.5, 60, 0.2, 0, 0),   # xylophone
    16: ('organ', 0.005, 0.1, 1, 0.05, 0, 0),      # drawbar organ
    19: ('organ', 0.05, 0.1, 1, 0.3, 0, 4),        # church organ
    24: ('pluck', 0.002, 2.0, 40, 0.3, 3000, 0),   # nylon guitar
    29: ('square', 0.005, 0.8, 10, 0.2, 2500, 0),  # overdriven guitar
    30: ('saw', 0.005, 0.8, 8, 0.2, 2500, 0),      # distortion guitar
    32: ('triangle', 0.005, 1.5, 30, 0.2, 0, 0),   # acoustic bass
    38: ('saw', 0.002, 0.6, 10, 0.1, 1200, 0),     # synth bass 1
    45: ('pluck', 0.002, 0.4, 60, 0.15, 3000, 0),  # pizzicato
    46: ('pluck', 0.002, 2.0, 40, 0.6, 5000, 0),   # harp
    47: ('sine', 0.002, 0.8, 60, 0.3, 0, 0),       # timpani
    56: ('brass', 0.02, 0.3, 4, 0.1, 4500, 4),     # trumpet
    73: ('flute', 0.05, 0.3, 2, 0.15, 0, 12),      # flute
    80: ('square', 0.005, 0.2, 2, 0.1, 0, 0),      # square lead
    81: ('saw', 0.005, 0.2, 2, 0.1, 0, 0),         # saw lead
    88: ('saw', 0.4, 1.5, 4, 1.0, 1800, 6),        # new age pad
}


def hz_to_cents(hz):
    return int(round(1200 * math.log2(hz / 8.176)))


def inst_gens(wave_sample, attack, decay, sustain_db, release, cutoff, vibrato, extra=()):
    g = [(34, tc(attack)), (36, tc(decay)), (37, int(sustain_db * 10)), (38, tc(release))]
    if cutoff:
        g.append((8, hz_to_cents(cutoff)))
    if vibrato:
        g += [(24, -2000), (23, tc(0.25)), (6, vibrato)]      # ~4.4 Hz after a quarter second
    g += list(extra)
    g += [(54, 1), (53, wave_sample)]                         # looped; the sample comes last
    return g


# Drum kit: key -> (sample, pitch key, decay s, filter Hz, pitch drop cents, attenuation cB)
def drum_map():
    d = {}
    for k in (35, 36):                         # kicks
        d[k] = ('sine', 31 if k == 35 else 33, 0.35, 0, -2400, 0)
    d[37] = ('noise', 84, 0.08, 6000, 0, 40)   # side stick
    for k in (38, 40):                         # snares
        d[k] = ('noise', 72, 0.22, 5000, 0, 20)
    d[39] = ('noise', 76, 0.2, 7000, 0, 30)    # clap
    for k, p in ((41, 40), (43, 43), (45, 46), (47, 49), (48, 52), (50, 55)):   # toms
        d[k] = ('sine', p, 0.45, 0, -1200, 10)
    for k in (42, 44):                         # closed and pedal hi-hat
        d[k] = ('noise', 96, 0.06, 0, 0, 50)
    d[46] = ('noise', 96, 0.5, 0, 0, 50)       # open hi-hat
    for k in (49, 52, 55, 57):                 # crash, chinese, splash
        d[k] = ('noise', 90, 1.6, 0, 0, 40)
    for k in (51, 59):                         # rides
        d[k] = ('noise', 100, 1.0, 0, 0, 60)
    d[53] = ('bell', 84, 0.8, 0, 0, 40)        # ride bell
    d[54] = ('noise', 100, 0.25, 0, 0, 50)     # tambourine
    d[56] = ('bell', 79, 0.3, 0, 0, 40)        # cowbell
    d[58] = ('noise', 70, 0.6, 3000, 0, 40)    # vibraslap
    for k in range(60, 69):                    # bongos, congas, timbales
        d[k] = ('sine', 64 + (k - 60) * 2, 0.25, 0, -400, 20)
    for k in (69, 70):                         # cabasa, maracas
        d[k] = ('noise', 100, 0.08, 0, 0, 50)
    for k in (71, 72):                         # whistles
        d[k] = ('sine', 96, 0.3 if k == 71 else 0.8, 0, 0, 40)
    for k in (73, 74):                         # guiro
        d[k] = ('noise', 80, 0.2, 4000, 0, 40)
    d[75] = ('sine', 96, 0.1, 0, 0, 40)        # claves
    for k in (76, 77):                         # wood blocks
        d[k] = ('triangle', 84 if k == 76 else 79, 0.1, 0, 0, 30)
    for k in (78, 79):                         # cuica
        d[k] = ('sine', 76 if k == 78 else 70, 0.3, 0, 600, 30)
    for k in (80, 81):                         # triangle
        d[k] = ('bell', 100, 0.2 if k == 80 else 1.2, 0, 0, 50)
    return d


# --------------------------------------------------------------------------
# SF2 writer
# --------------------------------------------------------------------------
def chunk(tag, data):
    pad = b'\0' if len(data) % 2 else b''
    return tag + struct.pack('<I', len(data)) + data + pad


def lst(tag, *chunks):
    body = tag + b''.join(chunks)
    return b'LIST' + struct.pack('<I', len(body)) + body


def name20(s):
    return s.encode('ascii')[:19].ljust(20, b'\0')


def build():
    rate = cycle_rate()
    samples = []                       # (name, frames, loop, rate, root)
    data = []
    index = {}

    def add_sample(name, frames, loop, srate, root):
        index[name] = len(samples)
        start = sum(len(d) + 46 for d in data)     # 46 zero frames after each sample, as SF2 asks
        samples.append((name, start, start + len(frames), loop, srate, root))
        data.append(frames)

    for name, w in WAVES.items():
        add_sample(name, looped(w), True, rate, ROOT)
    add_sample('noise', noise(16384), True, 44100, 60)

    sdata = b''.join(struct.pack('<%dh' % len(d), *d) + b'\0' * 92 for d in data)

    insts = []                         # (name, [zone gens])
    for prog in range(128):
        wave, a, d, s, r, fc, vib = OVERRIDES.get(prog, FAMILIES[prog // 8])
        extra = []
        if wave == 'noise':
            extra = [(58, 60)]
        insts.append(('GM %d' % prog, [inst_gens(index[wave], a, d, s, r, fc, vib, extra)]))
    zones = []
    for key, (wave, pitch, decay, fc, drop, att) in sorted(drum_map().items()):
        g = [(43, key | key << 8), (34, tc(0.001)), (36, tc(decay)), (37, 1440), (38, tc(decay * 0.6)),
             (48, att), (56, 0 if wave == 'noise' else 100)]
        if wave == 'noise':
            g.append((51, (pitch - 60)))       # coarse tune: brighter noise for higher "pitches"
        else:
            g.append((58, ROOT - (pitch - key)))
        if fc:
            g.append((8, hz_to_cents(fc)))
        if drop:
            g += [(26, tc(0.001)), (28, tc(decay * 0.4)), (29, 1000), (7, drop)]
        if key in (42, 44, 46):
            g.append((57, 1))                  # hi-hats cut each other off
        g += [(54, 1), (53, index[wave])]
        zones.append(g)
    insts.append(('Standard Kit', zones))

    # pdta
    phdr, pbag, pgen = b'', b'', b''
    ibag_n = igen_n = pbag_n = pgen_n = 0
    inst, ibag, igen = b'', b'', b''
    for i, (name, zs) in enumerate(insts):
        inst += name20(name) + struct.pack('<H', ibag_n)
        for z in zs:
            ibag += struct.pack('<HH', igen_n, 0)
            ibag_n += 1
            for op, amt in z:
                igen += struct.pack('<Hh' if amt < 0 else '<HH', op, amt)
                igen_n += 1
    inst += name20('EOI') + struct.pack('<H', ibag_n)
    ibag += struct.pack('<HH', igen_n, 0)
    igen += struct.pack('<HH', 0, 0)

    presets = [(name20(insts[p][0].replace('GM', 'Program')), p, 0, p) for p in range(128)]
    presets.append((name20('Standard Kit'), 0, 128, 128))
    for pname, prog, bank, ii in presets:
        phdr += pname + struct.pack('<HHHIII', prog, bank, pbag_n, 0, 0, 0)
        pbag += struct.pack('<HH', pgen_n, 0)
        pbag_n += 1
        pgen += struct.pack('<HH', 41, ii)
        pgen_n += 1
    phdr += name20('EOP') + struct.pack('<HHHIII', 0, 0, pbag_n, 0, 0, 0)
    pbag += struct.pack('<HH', pgen_n, 0)
    pgen += struct.pack('<HH', 0, 0)
    pmod = imod = b'\0' * 10

    shdr = b''
    for name, start, end, loop, srate, root in samples:
        ls, le = (start, end) if loop else (start, end)
        shdr += name20(name) + struct.pack('<IIIIIBbHH', start, end, ls, le, srate, root, 0, 0, 1)
    shdr += name20('EOS') + b'\0' * 26

    info = lst(b'INFO', chunk(b'ifil', struct.pack('<HH', 2, 1)), chunk(b'isng', b'EMU8000\0'),
               chunk(b'INAM', b'NovaOS General MIDI\0'),
               chunk(b'ICOP', b'NovaOS contributors, MIT licence\0'))
    sdta = lst(b'sdta', chunk(b'smpl', sdata))
    pdta = lst(b'pdta', chunk(b'phdr', phdr), chunk(b'pbag', pbag), chunk(b'pmod', pmod), chunk(b'pgen', pgen),
               chunk(b'inst', inst), chunk(b'ibag', ibag), chunk(b'imod', imod), chunk(b'igen', igen),
               chunk(b'shdr', shdr))
    body = b'sfbk' + info + sdta + pdta
    return b'RIFF' + struct.pack('<I', len(body)) + body


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    open(sys.argv[1], 'wb').write(build())
