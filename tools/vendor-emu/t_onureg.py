#!/usr/bin/env python3
"""Dump the vendor's xdpi/tm "ONU register" descriptors.

cla_set_outspace_cfg() writes ONU register 0xd, so its address/mask/shift
matter to us.  tmOnuGlbRegValidation() hands back a descriptor struct; run it
in the emulator for a range of register ids and print the fields the writers
use: +0x08 base, +0x0c mask, +0x10 shift, +0x14 stride.

    python3 t_onureg.py [first] [last]
"""
import struct
import sys

from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

TM = '/ko/tm.ko'


def one(m, regid):
    out = m.calloc(0x20)
    m.wr(out, 0x20, b'\x00' * 0x20)
    tmp = m.calloc(0x20)
    m.wr(tmp, 0x20, b'\x00' * 0x20)
    rc = m.run('tmOnuGlbRegValidation', (out, tmp, regid, 1), max_steps=200000)
    if rc:
        return None
    ptr = struct.unpack('<I', m.rd(tmp, 4))[0]
    if ptr == 0:
        return None
    return m.rd(ptr, 0x20)


def main(argv):
    lo = int(argv[1], 0) if len(argv) > 1 else 0
    hi = int(argv[2], 0) if len(argv) > 2 else 0x20
    m = Mod(TM, intercept=('printk', 'fpga_read_reg', 'fpga_write_reg'),
            log=lambda *a: None)

    def h_p(_n):
        m.uc.reg_write(UC_ARM_REG_R0, 0)
    m.handlers.update({'printk': h_p, 'fpga_read_reg': h_p,
                       'fpga_write_reg': h_p})
    for r in range(lo, hi):
        d = one(m, r)
        if d is None:
            print('reg %#04x: validation failed' % r)
            continue
        w = struct.unpack('<8I', d)
        print('reg %#04x: base %#010x mask %#x shift %u stride %#x  raw %s'
              % (r, w[2], w[3], w[4] & 0xffff, w[5],
                 ' '.join('%08x' % x for x in w)))


if __name__ == '__main__':
    main(sys.argv)
