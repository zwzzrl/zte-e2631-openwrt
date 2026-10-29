#!/usr/bin/env python3
"""What does tm.ko decode from a v6 flow descriptor, and how does that become
the 13-byte hash-key head?

tm_acl_fast_add_v4v6() first calls 0x48ec8(port, desc, ?, &blk) where blk is a
52-byte (0x34) field block on the stack at sp+0x64; a straight-line run of
bfi/strb then packs the 45-byte hash key at sp+0x14 out of that block.  An
faulting header (0x4d378) dumps both, so one run shows what the decoder made of
the descriptor and what reached the hash walker.

    python3 t_v6key.py base [typ]        # one descriptor, block + key
    python3 t_v6key.py scan <lo> <hi>    # descriptor byte -> block/key byte
"""
import sys

from unicorn.arm_const import UC_ARM_REG_SP

from t_key import P

BLK, KEY = 0x64, 0x14
NBLK, NKEY = 0x34, 0x2d  # block, 45-byte key


class K(P):
    def __init__(self, typ=5, rs=None, fast=1, idx=10, **kw):
        self.blocks = []
        super().__init__(typ=typ, rs=rs, fast=fast, idx=idx, **kw)

    def _rerun(self):
        def poke(mm, addr):
            sp = mm.uc.reg_read(UC_ARM_REG_SP)
            self.blocks.append(mm.rd(sp + BLK, NBLK))
        self.m.pokes[self.text + 0x4d378] = poke
        super()._rerun()

    def last(self):
        return self.blocks[-1] if self.blocks else b''

    def key45(self):
        for _n, _b, k, _e in self.keys:
            return k
        return b''

    def show(self, tag=''):
        blk, key = self.last(), self.key45()
        slot, _e = self.entry()
        print('== %s typ %d fast %d ret %r slot %r' %
              (tag or 'base', self.typ, self.fast, self.ret, slot))
        print('   blk %s' % blk.hex())
        print('   key %s' % key.hex())


def base(typ):
    K(typ).show()


def scan(typ, lo, hi):
    """One descriptor byte at a time: which block/key byte follows it."""
    b0 = K(typ)
    blk0, key0 = b0.last(), b0.key45()
    print('### typ %d baseline blk %s' % (typ, blk0.hex()))
    print('###                      key %s' % key0.hex())
    for off in range(lo, hi):
        v = 0xa0 | (off & 0x1f)
        p = K(typ, rs={off: bytes([v])})
        blk, key = p.last(), p.key45()
        if not blk:
            print('  desc %#04x: no block (ret %r) %s' %
                  (off, p.ret, p.log[:2]))
            continue
        if len(key) < NKEY:
            print('  desc %#04x: block only (ret %r) %s' %
                  (off, p.ret, p.log[:2]))
            continue
        db = ['b%d=%02x' % (i, blk[i]) for i in range(NBLK) if blk[i] != blk0[i]]
        dk = ['k%d=%02x' % (i, key[i]) for i in range(NKEY) if key[i] != key0[i]]
        print('  desc %#04x: %s | %s' % (off, ' '.join(db), ' '.join(dk)))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'base'
    if cmd == 'base':
        base(int(sys.argv[2]) if len(sys.argv) > 2 else 5)
    elif cmd == 'scan':
        scan(int(sys.argv[4]) if len(sys.argv) > 4 else 5,
             int(sys.argv[2], 0), int(sys.argv[3], 0))
