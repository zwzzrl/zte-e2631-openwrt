#!/usr/bin/env python3
"""A/B the vendor v6 session path on **raw** addresses, not on session bytes.

t_v6flowdump.py proved the 168-byte session entry holds mask-encoded prefixes,
so editing the session could not move the address fields.  The encoder lives in
switch.ko (sw_acl_setMtchInfo), and it is fed by t_hfsess.fill_desc(), which
points the descriptor at raw 16-byte sip/dip buffers.  So: change the raw
address here -> switch.ko re-encodes -> tm.ko rebuilds the 45-byte hash key and
the 60-byte entry.  That is the A/B the earlier round could not do.

    python3 t_v6ab.py                      # baseline (fd99::2 -> fd88::2)
    python3 t_v6ab.py sip  <32 hex>        # one field at a time
    python3 t_v6ab.py dip  <32 hex>
    python3 t_v6ab.py sport <dec|0x..>
    python3 t_v6ab.py dport <dec|0x..>
    python3 t_v6ab.py proto <dec|0x..>
    python3 t_v6ab.py inport <n>
    python3 t_v6ab.py scan                # run the whole set, diff vs baseline
"""
import struct
import sys

import t_hfsess
import t_v6flowdump

BASE = dict(sip=t_hfsess.SIP, dip=t_hfsess.DIP, sport=t_hfsess.SPORT,
            dport=t_hfsess.DPORT, proto=0x11, inport=2)


def build(f):
    """Raw descriptor -> switch.ko's 168-byte session entry."""
    t_hfsess.SIP = f['sip']
    t_hfsess.DIP = f['dip']
    t_hfsess.SPORT = f['sport']
    t_hfsess.DPORT = f['dport']
    d = bytearray(t_hfsess.build_desc(d14=1, port=f['inport']))
    d[0x10] = f['proto'] & 0xff
    return t_hfsess.run_switch(bytes(d), log=lambda *a: None)


def run(f):
    sess = build(f)
    if sess is None:
        return None
    e = bytearray(sess[0] if isinstance(sess, tuple) else sess)
    e[0x9e] = f['inport']
    ret, got, keys = t_v6flowdump.run_tm_with_keys(bytes(e))
    ent = None
    for kind, a, data in got:
        if kind == 'hash':
            ent = (a, data)
    return bytes(e), ent, (keys[0] if keys else b''), ret


def show(tag, f, r):
    if r is None:
        print('== %s: switch.ko did not reach session_add' % tag)
        return
    sess, ent, key, ret = r
    print('== %s sip %s dip %s sport %d dport %d proto %#x inport %d ret %r'
          % (tag, f['sip'].hex(), f['dip'].hex(), f['sport'], f['dport'],
             f['proto'], f['inport'], ret))
    print('   sess %s' % sess.hex())
    if ent is None:
        print('   no entry written')
        return
    print('   slot %d entry %s' % (ent[0], ent[1].hex()))
    print('   key48 %s' % key.hex())


def diff(tag, r0, r1):
    if not (r0 and r1 and r0[1] and r1[1]):
        print('  %-10s no comparable entry' % tag)
        return
    s0, (slot0, e0), k0, _ = r0
    s1, (slot1, e1), k1, _ = r1
    ds = ['s%d=%02x' % (i, s1[i]) for i in range(0xa8) if s1[i] != s0[i]]
    de = ['e%d=%02x' % (i, e1[i]) for i in range(60) if e1[i] != e0[i]]
    dk = ['k%d=%02x' % (i, k1[i]) for i in range(48) if k1[i] != k0[i]]
    print('  %-10s slot %d->%d' % (tag, slot0, slot1))
    print('     sess  %s' % (' '.join(ds) or '-'))
    print('     entry %s' % (' '.join(de) or '-'))
    print('     key   %s' % (' '.join(dk) or '-'))


def scan():
    b = run(BASE)
    show('base', BASE, b)
    cases = [
        ('sip-b0', dict(BASE, sip=bytes([ord('f')]) * 0)),
        ('sip', dict(BASE, sip=bytes.fromhex(
            'fd770000000000000000000000000002'))),
        ('sip-last', dict(BASE, sip=bytes.fromhex(
            'fd990000000000000000000000000002'.replace('02', '02')[:-2]
            + '07'))),
        ('dip', dict(BASE, dip=bytes.fromhex(
            'fd880000000000000000000000000003'))),
        ('dip-id', dict(BASE, dip=bytes.fromhex(
            'fdac1e00000000000000000000000002'))),
        ('sport', dict(BASE, sport=0x1234)),
        ('dport', dict(BASE, dport=0x5678)),
        ('proto', dict(BASE, proto=0x06)),
        ('inport', dict(BASE, inport=0)),
    ]
    for tag, f in cases:
        if tag == 'sip-b0':
            continue
        r = run(f)
        show(tag, f, r)
        diff(tag, b, r)


def main(argv):
    if len(argv) < 2 or argv[1] == 'scan':
        scan()
        return
    k = argv[1]
    v = argv[2]
    f = dict(BASE)
    if k in ('sip', 'dip'):
        f[k] = bytes.fromhex(v)
    elif k == 'inport':
        f[k] = int(v, 0)
        f['sport'] = f['sport']
    else:
        f[k] = int(v, 0)
    show('%s=%s' % (k, v), f, run(f))


if __name__ == '__main__':
    main(sys.argv)
