#!/usr/bin/env python3
"""Where does the 45-byte v6 hash key sit on tm_acl_fast_add_v4v6's stack, and
what does the frame look like around it?

    python3 t_v6stk.py [typ]
"""
import sys

from unicorn.arm_const import UC_ARM_REG_SP

from t_key import P
from km import REG0_12


class S(P):
    def __init__(self, typ=5, rs=None, fast=1, idx=10):
        self.shots = []
        super().__init__(typ=typ, rs=rs, fast=fast, idx=idx)

    def _rerun(self):
        def poke(mm, addr):
            sp = mm.uc.reg_read(UC_ARM_REG_SP)
            keyp = mm.uc.reg_read(REG0_12[1])
            self.shots.append((sp, keyp, mm.rd(sp, 0xb0)))
        self.m.pokes[self.text + 0x4d604] = poke
        super()._rerun()


def main(typ):
    s = S(typ)
    sp, keyp, data = s.shots[-1]
    print('== typ %d ret %r sp %#x keyptr %#x  (key - sp = %#x)'
          % (typ, s.ret, sp, keyp, keyp - sp))
    for i in range(0, len(data), 16):
        print('  %#04x  %s' % (i, ' '.join('%02x' % b for b in data[i:i + 16])))


if __name__ == '__main__':
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 5)
