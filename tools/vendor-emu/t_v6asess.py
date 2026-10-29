#!/usr/bin/env python3
"""Session-byte -> entry-byte scan on the *real* (AF_INET6) v6 session.

t_v6sessscan.py scanned the AF_INET4-copy session, where s08.. holds flags; here
desc[0x0c]=10 so session+4..0x13 really is the 16-byte sip and +0x14..0x23 the
dip, and the scan answers "which session byte feeds which entry/key byte".

    python3 t_v6asess.py base
    python3 t_v6asess.py scan [lo] [hi]
"""
import sys

import t_v6real


def ent_key(sess, inport=2):
    e = bytearray(sess)
    e[0x9e] = inport
    ret, got, keys = t_v6real.t_v6flowdump.run_tm_with_keys(bytes(e))
    ent = None
    for kind, a, data in got:
        if kind == 'hash':
            ent = (a, data)
    return ent, (keys[0] if keys else b''), ret


def base_sess():
    d = t_v6real.build(bytes.fromhex('fd990000000000000000000000000002'),
                       bytes.fromhex('fd880000000000000000000000000002'),
                       48211, 9999, 0x11, 2)
    s = t_v6real.t_hfsess.run_switch(bytes(d), log=lambda *a: None)
    return bytes(s[0] if isinstance(s, tuple) else s)


def show(tag, sess, r):
    ent, key, ret = r
    print('== %s ret %r' % (tag, ret))
    print('   sess %s' % sess.hex())
    if ent:
        print('   slot %d' % ent[0])
        print('   entry %s' % ent[1].hex())
    print('   key   %s' % key.hex())


def scan(lo, hi):
    s0 = base_sess()
    e0, k0, _ = ent_key(s0)
    print('### base slot %d' % e0[0])
    print('### base entry %s' % e0[1].hex())
    print('### base key   %s' % k0.hex())
    for off in range(lo, hi):
        s = bytearray(s0)
        s[off] ^= 0xff
        ent, key, ret = ent_key(bytes(s))
        if ent is None:
            print('  s%02x: no entry (ret %r)' % (off, ret))
            continue
        de = ['e%d=%02x' % (i, ent[1][i]) for i in range(60)
              if ent[1][i] != e0[1][i]]
        dk = ['k%d=%02x' % (i, key[i]) for i in range(45)
              if i < len(k0) and key[i] != k0[i]]
        if not de and not dk and ent[0] == e0[0]:
            continue
        print('  s%02x slot %d: %s | %s'
              % (off, ent[0], ' '.join(de) or '-', ' '.join(dk) or '-'))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'base'
    if cmd == 'scan':
        scan(int(sys.argv[2], 0) if len(sys.argv) > 2 else 0,
             int(sys.argv[3], 0) if len(sys.argv) > 3 else 0xa8)
    else:
        s = base_sess()
        show('base', s, ent_key(s))
