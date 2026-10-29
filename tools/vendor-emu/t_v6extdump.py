#!/usr/bin/env python3
"""Where does the vendor's IPv6 flow-add actually put the entry?

`cla_set_external_hash_table()` does not go through the indirect CLA window:
it computes a destination address itself (the 0x3c40800 / <<6 arithmetic) and
stores the 60-byte entry there byte by byte.  Decoding that by hand is
error-prone, so run the vendor path in the emulator and dump every byte it
touched outside its own data/stack/heap - that gives both the destination
range and the entry bytes as the vendor's code really writes them.

    python3 t_v6extdump.py [sip] [dip] [sport] [dport] [inport]
"""
import sys

import t_v6trace

MINE = ((0x10000000, 0x1000000), (0x30000000, 0x400000),
        (0x40000000, 0x400000), (0x50000000, 0x1000000))


def owned(addr):
    for b, n in MINE:
        if b <= addr < b + n:
            return True
    return False


def main(argv):
    sip = bytes.fromhex(argv[1]) if len(argv) > 1 else bytes.fromhex(
        'fdee70dfd02e00000000000000000002')
    dip = bytes.fromhex(argv[2]) if len(argv) > 2 else bytes.fromhex(
        'fd880000000000000000000000000002')
    sport = int(argv[3], 0) if len(argv) > 3 else 40000
    dport = int(argv[4], 0) if len(argv) > 4 else 9999
    inport = int(argv[5], 0) if len(argv) > 5 else 2

    ret, trace, got, keys, m = t_v6trace.run(sip, dip, sport, dport, inport)
    print('== ret %r' % ret)
    for kind, a, data in got:
        print('   table write %-6s %r %s' % (kind, a, data.hex()))
    print('== pages the run touched outside the module/stack/heap')
    for page in sorted(m.unmapped):
        if owned(page):
            continue
        try:
            data = m.uc.mem_read(page, 0x1000)
        except Exception as exc:                        # noqa: BLE001
            print('   %#010x  <%s>' % (page, exc))
            continue
        for off in range(0, 0x1000, 16):
            row = data[off:off + 16]
            if any(row):
                print('   %#010x  %s' % (page + off, row.hex(' ')))


if __name__ == '__main__':
    main(sys.argv)
