#!/usr/bin/env python3
"""Descriptor -> 60-byte hash-rule mapping, by calling tm_acl_get_fastHashRule
straight out of tm.ko with a descriptor we control.

tm_acl_fast_add_v4v6() calls
    tm_acl_get_fastHashRule(isL3, session, operInfo, buf60)
and then copies the extract rule's windows out of buf60 into the 45-byte key it
hands to aclGetAvailableHashAddr().  Calling it on its own makes the mapping
observable one descriptor byte at a time:

  * baseline: descriptor all zero  -> which entry bytes are written at all
  * per byte: descriptor byte = 0xff -> which entry bytes change, and to what

    python3 t_gfhr.py map      # summarise the per-byte mapping
    python3 t_gfhr.py dump N   # full entry for one experiment
"""
import sys

from km import Mod
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1

KO = '/ko/tm.ko'
INTERCEPT = ('printk', '__memzero', 'memset', 'cla_get_hash_poly_config')

FN = 'tm_acl_get_fastHashRule'


class G:
    def __init__(self, typ, desc=None, oper=None, size=0x200, fill=0x00):
        self.log = []
        m = self.m = Mod(KO, intercept=INTERCEPT, log=self.log.append)
        m.handlers['printk'] = lambda n: 0

        def memzero(_n):
            dst = m.uc.reg_read(UC_ARM_REG_R0)
            n = m.uc.reg_read(UC_ARM_REG_R1)
            if 0 < n < 0x1000:
                m.wr(dst, n, b'\x00' * n)
        m.handlers['__memzero'] = memzero
        self.desc = m.calloc(size)
        m.wr(self.desc, size, bytes([fill]) * size)
        m.wr(self.desc + 12, 4, typ)          # [session+12] = rule type
        for off, val in (desc or {}).items():
            if off == 12:
                continue
            m.wr(self.desc + off, len(val), val)
        self.oper = m.calloc(0x80)
        m.wr(self.oper, 0x80, bytes([fill]) * 0x80)
        for off, val in (oper or {}).items():
            m.wr(self.oper + off, len(val), val)
        self.buf = m.calloc(0x40)
        m.wr(self.buf, 0x40, b'\xaa' * 0x40)
        self.ret = m.run(FN, (1, self.desc, self.oper, self.buf),
                         max_steps=200000)
        self.entry = m.rd(self.buf, 60)


def baseline(typ):
    return G(typ)


def sweep(typ):
    base = baseline(typ)
    print('type %d ret %r entry %s' % (typ, base.ret, base.entry.hex()))
    print('\ndesc byte -> entry byte mapping (desc[o]=0xff against all-zero)')
    hits = {}
    for o in range(0, 0x140):
        if o == 12:
            continue                      # rule type, must stay 3/5
        g = G(typ, desc={o: b'\xff'})
        diff = [(i, base.entry[i], g.entry[i])
                for i in range(60) if base.entry[i] != g.entry[i]]
        if diff:
            hits[o] = diff
            print('  desc[%#04x] -> %s' % (
                o, ' '.join('e%d:%02x->%02x' % d for d in diff)))
    return hits


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'map'
    types = [int(a) for a in sys.argv[2:]] or [3, 5]
    if cmd == 'map':
        for t in types:
            sweep(t)
    else:
        for t in types:
            g = baseline(t)
            print(t, g.ret, g.entry.hex(), g.log[:6])
