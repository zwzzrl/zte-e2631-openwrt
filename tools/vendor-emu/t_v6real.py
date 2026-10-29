#!/usr/bin/env python3
"""The vendor's real v6 entry, for addresses *we* choose, and the slot it lands in.

Why the earlier rounds could not do this: switch.ko's sw_acl_setMtchInfo() picks
its address copy path on `desc[0x0c] & ~2`:

    0  ->  4-byte copy (AF_INET), session+4 holds one u32
    !=0 -> 16-byte copy (AF_INET6), session+4..0x13 = sip, +0x14..0x23 = dip

build_desc() wrote 2 there, so every "v6" session actually carried the IPv4
4-byte copy and the encrypted-looking `40 03 00 50` we chased was the constant
that path leaves behind.  With desc[0x0c] = 10 the session carries the real
16-byte addresses, and tm.ko builds a real entry and a real hash key from them.

    python3 t_v6real.py base
    python3 t_v6real.py one <sip> <dip> [sport] [dport] [proto] [inport]
    python3 t_v6real.py scan
"""
import struct
import sys

import t_hfsess
import t_v6flowdump


def build(sip, dip, sport, dport, proto, inport):
    t_hfsess.SIP, t_hfsess.DIP = sip, dip
    t_hfsess.SPORT, t_hfsess.DPORT = sport, dport
    d = bytearray(t_hfsess.build_desc(d14=1, type=10, port=inport))
    d[0x10] = proto & 0xff
    return d


def run(sip, dip, sport=48211, dport=9999, proto=0x11, inport=2):
    d = build(sip, dip, sport, dport, proto, inport)
    sess = t_hfsess.run_switch(bytes(d), log=lambda *a: None)
    if sess is None:
        return None
    e = bytearray(sess[0] if isinstance(sess, tuple) else sess)
    e[0x9e] = inport
    ret, got, keys = t_v6flowdump.run_tm_with_keys(bytes(e))
    ent = None
    for kind, a, data in got:
        if kind == 'hash':
            ent = (a, data)
    return bytes(e), ent, (keys[0] if keys else b''), ret


def hexby(tag, data):
    print('   %s (%d B)' % (tag, len(data)))
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        print('     %3d  %s' % (i, ' '.join('%02x' % b for b in row)))


def show(tag, r):
    if r is None:
        print('== %s: no session' % tag)
        return
    sess, ent, key, ret = r
    print('== %s ret %r' % (tag, ret))
    if ent:
        print('   slot %d' % ent[0])
        hexby('entry', ent[1])
    if key:
        hexby('key48', key[:45])


def one(argv):
    sip = bytes.fromhex(argv[0])
    dip = bytes.fromhex(argv[1])
    sport = int(argv[2], 0) if len(argv) > 2 else 48211
    dport = int(argv[3], 0) if len(argv) > 3 else 9999
    proto = int(argv[4], 0) if len(argv) > 4 else 0x11
    inport = int(argv[5], 0) if len(argv) > 5 else 2
    show('one', run(sip, dip, sport, dport, proto, inport))


def scan():
    b = run(bytes.fromhex('fd990000000000000000000000000002'),
            bytes.fromhex('fd880000000000000000000000000002'))
    show('base', b)
    cases = [
        ('sip-last', bytes.fromhex('fd990000000000000000000000000007'),
         bytes.fromhex('fd880000000000000000000000000002'), 48211, 9999, 0x11),
        ('dip-last', bytes.fromhex('fd990000000000000000000000000002'),
         bytes.fromhex('fd880000000000000000000000000009'), 48211, 9999, 0x11),
        ('sip-hi', bytes.fromhex('fe800000000000000000000000000002'),
         bytes.fromhex('fd880000000000000000000000000002'), 48211, 9999, 0x11),
        ('sport', bytes.fromhex('fd990000000000000000000000000002'),
         bytes.fromhex('fd880000000000000000000000000002'), 0x1234, 9999, 0x11),
        ('dport', bytes.fromhex('fd990000000000000000000000000002'),
         bytes.fromhex('fd880000000000000000000000000002'), 48211, 0x5678, 0x11),
        ('tcp', bytes.fromhex('fd990000000000000000000000000002'),
         bytes.fromhex('fd880000000000000000000000000002'), 48211, 9999, 0x06),
        ('inport0', bytes.fromhex('fd990000000000000000000000000002'),
         bytes.fromhex('fd880000000000000000000000000002'), 48211, 9999, 0x11, 0),
    ]
    for c in cases:
        show(c[0], run(*c[1:]))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'base'
    if cmd == 'base':
        show('base', run(bytes.fromhex('fd990000000000000000000000000002'),
                         bytes.fromhex('fd880000000000000000000000000002')))
    elif cmd == 'one':
        one(sys.argv[2:])
    else:
        scan()
