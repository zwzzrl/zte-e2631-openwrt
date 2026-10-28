#!/usr/bin/env python3
"""What exactly does the vendor hand the hardware for one flow, byte by byte?

The 13-byte-window puzzle: with the real polynomial config fed in, a type-5
(IPv6 up) flow-add still writes only e19..e32 of the 60-byte hash entry - the
same 13 bytes an IPv4 flow fills - and nothing from the descriptor's IPv6
address area lands there, while our driver writes 19 halfwords (38 bytes) of
key for IPv6.  This script walks the descriptor 4 bytes at a time, pokes a
distinct marker into each block and reports which output byte each marker
lands in, for every buffer the vendor produces: the 45-byte key, the 60-byte
hash rule, the 60-byte table entry and any external-hash entry.

    python3 t_v6win.py                 # types 3 and 5, block sweep
    python3 t_v6win.py 5               # one type
    python3 t_v6win.py 5 real          # one plausible IPv6 5-tuple, no sweep
"""
import struct
import sys

from t_poly import POLY, Q

MARK = (0xd1, 0xd2, 0xd3, 0xd4)


def outputs(p):
    """Every buffer the run produced, keyed by a label."""
    out = {}
    slot, entry = p.entry()
    out['entry[%s]' % slot] = entry or b''
    for kind, idx, d in p.got:
        out['%s[%s]' % (kind, idx)] = d
    for name, bank, key, buf in p.keys:
        out[name + '.key45/b%d' % bank] = key
        out[name + '.rule60/b%d' % bank] = buf
    return out


def run(typ, rs):
    return Q(typ, rs=rs, idx=10 if typ in (5, 6) else 9)


def block_sweep(typ, lo=0x10, hi=0x124, step=4):
    base = run(typ, {})
    b0 = outputs(base)
    print('### type %d: descriptor block -> output byte' % typ)
    for label, data in b0.items():
        print('   base %-22s %s' % (label, data.hex()))
    for o in range(lo, hi, step):
        rs = {o + k: bytes([MARK[k]]) for k in range(min(step, hi - o))}
        p = run(typ, rs)
        got = outputs(p)
        for label, data in got.items():
            old = b0.get(label, b'')
            for i, b in enumerate(data):
                if b in MARK and (i >= len(old) or old[i] != b):
                    src = o + MARK.index(b)
                    print('   desc[%#04x] -> %s byte %d = %#04x'
                          % (src, label, i, b))


def real(typ):
    src = bytes.fromhex('fd990000000000000000000000000002')
    dst = bytes.fromhex('fd880000000000000000000000000002')
    rs = {
        0x10: bytes([0x01, 0x00, 0x00, 0x00]),
        0x62: bytes([0x11]),
        0x64: src,
        0x74: struct.pack('>HH', 48211, 9999),
    }
    p = run(typ, rs)
    print('== type %d, plausible IPv6 descriptor at +0x64/+0x74' % typ)
    for label, data in outputs(p).items():
        print('   %-22s %s' % (label, data.hex()))
    for l in p.log[:6]:
        print('   log %s' % l)


if __name__ == '__main__':
    types = [int(a) for a in sys.argv[1:] if a.isdigit()] or [3, 5]
    if 'real' in sys.argv:
        for t in types:
            real(t)
    else:
        for t in types:
            block_sweep(t)
