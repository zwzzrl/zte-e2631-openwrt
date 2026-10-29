#!/usr/bin/env python3
"""The entry point switch.ko really uses: zte_api_fast_l3_session_add(hffEntry).

Every earlier harness (t_key, t_poly, t_v6rule, t_v6win) jumps into
tm_add_acl_flow_rule() with a hand-made "flow descriptor", which is one layer
below what the switch driver calls.  The real call is

    sw_acl_l3_hardfast_session_add(session)
      -> sw_acl_setMtchInfo(session, hffEntry)     switch.ko 0x1e24, 168 bytes
      -> zte_api_fast_l3_session_add(hffEntry, out) tm.ko 0x5ce74, 5520 bytes

and the 168-byte hffEntry is described by switch.ko's own stores (0x1e24):

    +0x04  16 bytes  source address  (4 bytes in the IPv4 case)
    +0x14  16 bytes  destination address
    +0x24  u16       source port, network order (setMtchInfo rev16's it)
    +0x26  u16       destination port
    +0x28  u8        protocol
    +0x2e  u16       family/type - tm.ko dispatches on it: 0, 1, 2, 3, other
    +0x35  6 bytes   source MAC      (copied by tm.ko 0x5d018)
    +0x3b  6 bytes   destination MAC
    +0x6c, +0x70, +0x74, +0x84  the rest of what tm.ko reads

This script fills that entry for one IPv6 UDP flow and runs the vendor function
in the Unicorn harness, recording every cla_set_* call.  The point is to see
what a *real* IPv6 session add hands to the hardware - rule, index and hash -
without the synthetic descriptor in between.

    python3 t_v6hff.py [family ...]        # default: 0 1 2 3
"""
import struct
import sys

from flowadd import Case

SRC = bytes.fromhex('fd990000000000000000000000000002')
DST = bytes.fromhex('fd880000000000000000000000000002')
SPORT, DPORT = 48211, 9999


def entry_bytes(family, proto=0x11, v6=True):
    e = bytearray(0xa8)
    e[0x04:0x14] = SRC if v6 else SRC[12:] + b'\x00' * 12
    e[0x14:0x24] = DST if v6 else DST[12:] + b'\x00' * 12
    e[0x24:0x28] = struct.pack('>HH', SPORT, DPORT)
    e[0x28] = proto
    e[0x2E:0x30] = struct.pack('<H', family)
    e[0x35:0x3B] = bytes.fromhex('5054aabbccdd')      # source MAC
    e[0x3B:0x41] = bytes.fromhex('d843ae681828')      # destination MAC
    e[0x6C] = 0x01                                     # vlan?
    e[0x70:0x74] = struct.pack('<I', 0x00000001)       # input dev id?
    e[0x74] = 0x02                                     # ?
    e[0x84:0x94] = DST                                 # second address slot
    e[0x98:0x9A] = struct.pack('<H', 1)                # ?
    e[0x9A:0x9C] = struct.pack('<H', 1)                # ?
    e[0x9C] = 0x01                                     # ?
    e[0x9D] = 0xFF                                     # "unset" sentinel
    e[0x9E] = 0x05                                     # output port
    e[0x9F] = 0x00                                     # ?
    return bytes(e)


def run(family, v6=True):
    e = entry_bytes(family, v6=v6)
    rs = {i: e[i:i + 1] for i in range(len(e)) if e[i]}
    rs[0x04] = e[0x04:0x14]
    rs[0x14] = e[0x14:0x24]
    rs[0x84] = e[0x84:0x94]
    c = Case(typ=0, rs=rs, entry='zte_api_fast_l3_session_add', size=0xa8)
    print('== family %d v6 %s: ret %r steps %d' % (family, v6, c.ret, c.steps))
    for kind, idx, data in c.got:
        if kind == 'rule':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(17)]
            print('   rule [%s] %s' % (idx, ' '.join('%08x' % x for x in w)))
        elif kind == 'index':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(5)]
            print('   index[%s] %s' % (idx, ' '.join('%08x' % x for x in w)))
        else:
            print('   %-5s %s %s' % (kind, idx, data.hex()))
    for l in c.log[:6]:
        print('   log %s' % l)
    if not c.got:
        print('   (no table calls; calls: %s)' % (c.calls[:12],))


if __name__ == '__main__':
    fams = [int(a) for a in sys.argv[1:] if a.isdigit()] or [0, 1, 2, 3]
    for f in fams:
        run(f)
