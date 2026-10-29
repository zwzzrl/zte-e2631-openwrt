#!/usr/bin/env python3
"""Which 168-byte session field lands in which byte of tm.ko's v6 hash key and
60-byte ACL entry?

switch.ko's own address encoder cannot be exercised offline (it reads a prefix
table we do not model), but tm.ko's side can: hand it one session entry, flip a
single session byte, and diff everything it pushes to CLA.  That is the bit
packing we have to reproduce, independent of how the address got encoded.

    python3 t_v6sessscan.py base
    python3 t_v6sessscan.py scan [lo] [hi]
"""
import sys

import t_v6flowdump


def base(inport=2):
    e = t_v6flowdump.build_entry(inport)
    return e


def run(e):
    ret, got, keys = t_v6flowdump.run_tm_with_keys(e)
    ent = None
    for kind, a, data in got:
        if kind == 'hash':
            ent = (a, data)
    return ent, (keys[0] if keys else b''), ret, got


def show(tag, e, r):
    ent, key, ret, got = r
    print('== %s ret %r' % (tag, ret))
    print('   sess %s' % e.hex())
    if ent:
        print('   slot %d entry %s' % (ent[0], ent[1].hex()))
    print('   key  %s' % key.hex())
    for kind, a, data in got:
        if kind != 'hash':
            print('   %s %r %s' % (kind, a, data.hex()))


def scan(lo, hi):
    e0 = base()
    ent0, key0, _ret, _g = run(e0)
    print('### base entry %s' % (ent0[1].hex() if ent0 else None))
    print('### base key   %s' % key0.hex())
    for off in range(lo, hi):
        e = bytearray(e0)
        e[off] ^= 0xff
        ent, key, ret, _g = run(bytes(e))
        if ent is None:
            print('  s%02x: no entry (ret %r)' % (off, ret))
            continue
        de = ['e%d=%02x' % (i, ent[1][i]) for i in range(60)
              if ent0 is None or ent[1][i] != ent0[1][i]]
        dk = ['k%d=%02x' % (i, key[i]) for i in range(45)
              if i < len(key0) and key[i] != key0[i]]
        if not de and not dk and (ent0 and ent[0] == ent[0]):
            continue
        print('  s%02x slot %d: %s | %s'
              % (off, ent[0], ' '.join(de) or '-', ' '.join(dk) or '-'))


def main(argv):
    if len(argv) > 1 and argv[1] == 'scan':
        scan(int(argv[2], 0) if len(argv) > 2 else 0,
             int(argv[3], 0) if len(argv) > 3 else 0xa8)
    else:
        show('base', base(), run(base()))


if __name__ == '__main__':
    main(sys.argv)
