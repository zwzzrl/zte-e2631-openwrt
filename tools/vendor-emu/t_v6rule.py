#!/usr/bin/env python3
"""What rule/index table does the vendor's IPv6 add path write?

t_v6win.py already showed that a type-5 add fills only 13 bytes of the hash
entry.  This script prints the other half of that picture: the extract rule and
the index entry the same run hands to the hardware, so the entry bytes can be
compared against the rule that is supposed to describe them.

    python3 t_v6rule.py [type]
"""
import struct
import sys

from t_poly import Q

SRC = bytes.fromhex('fd990000000000000000000000000002')
DST = bytes.fromhex('fd880000000000000000000000000002')


def run(typ):
    rs = {
        0x10: bytes([0x01, 0x00, 0x00, 0x00]),
        0x62: bytes([0x11]),
        0x64: SRC,
        0x6c: DST,
        0x74: struct.pack('>HH', 48211, 9999),
    }
    p = Q(typ, rs=rs, idx=10 if typ in (5, 6) else 9)
    print('== type %d ret %r' % (typ, p.ret))
    for kind, idx, data in p.got:
        print('   %-6s %-4s %s' % (kind, idx, data.hex()))
    for l in p.log[:8]:
        print('   log %s' % l)


if __name__ == '__main__':
    for t in [int(a) for a in sys.argv[1:] if a.isdigit()] or [5]:
        run(t)
