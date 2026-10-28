#!/usr/bin/env python3
"""The flow-add path with the polynomial config the hardware really holds.

flowadd.Case stubs cla_get_hash_poly_config() to zero, and a zero config is what
the vendor code rejects ("[cla_acl_hash_addr_gen]input invalid parameter"): the
window list it builds then falls back to whatever a zeroed rule says, which is
why an IPv6 descriptor came out looking exactly like an IPv4 one.  The real
register 12 reads 0x00e400e4, whose eight 2-bit fields are 0,1,0,3,0,1,0,3 (see
docs/known-issues.md, PPE-IPV6-TCP, second round), so this script feeds those.

    python3 t_poly.py [type ...]
"""
import struct

from unicorn.arm_const import UC_ARM_REG_R0

import t_key

# ONU register 12 = 0x00e400e4 split into its eight 2-bit fields.  out[0..3]
# come from bits 0/2/4/6 of the low half, out[4..7] from 16/18/20/22.
POLY = (0, 1, 0, 3, 0, 1, 0, 3)


class Q(t_key.P):
    """t_key.P with the polynomial config answered as the hardware answers it."""

    def _h(self, name):
        if name != 'cla_get_hash_poly_config':
            return super()._h(name)

        def h(_n):
            m = self.m
            m.wr(m.uc.reg_read(UC_ARM_REG_R0), 32,
                 struct.pack('<8I', *POLY))
            m.uc.reg_write(UC_ARM_REG_R0, 0)
        return h


def show(typ, fast=1):
    p = Q(typ, fast=fast, idx=10 if typ in (5, 6) else 9)
    slot, entry = p.entry()
    print('== type %d fast %d ret %r slot %r steps %s'
          % (typ, fast, p.ret, slot, p.m.steps))
    print('   entry %s' % (entry.hex() if entry else None))
    for name, bank, key, buf in p.keys:
        print('   %-7s bank %d key45 %s' % (name, bank, key.hex()))
    for l in p.log[:6]:
        print('   log %s' % l)


if __name__ == '__main__':
    import sys
    for t in [int(a) for a in sys.argv[1:]] or [3, 5]:
        show(t)
