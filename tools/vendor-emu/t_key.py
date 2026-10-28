#!/usr/bin/env python3
"""Which key does tm.ko build for a flow, and from which descriptor bytes?

tm_add_acl_flow_rule() routes a flow to one of two builders, depending on a
global flag (.LANCHOR1+0x148 in tm.ko) and on the rule type at [session+12]:

    type 3/4 (IPv4), 5/6 (IPv6), 7/8 (L2)
    flag set, type-3 <= 5 -> tm_acl_fast_add()      -> tm_acl_fast_add_v4v6()
    otherwise             -> addAclRule()           -> aclBuildHashAndAclRule()

This script runs either one with a descriptor whose bytes we control, and
records every cla_set_hash_table()/cla_set_external_hash_table() call plus the
45-byte key that is handed to aclGetAvailableHashAddr().

    python3 t_key.py paths                 # both routes, all six types
    python3 t_key.py map 5                 # descriptor byte -> entry byte
    python3 t_key.py key 5                 # the 45-byte key, banks 0/1/2
"""
import sys

from flowadd import Case
from km import REG0_12
from unicorn.arm_const import UC_ARM_REG_SP

BK = dict(path_up=0x4d604, path_dn=0x4d7ec, patch1=0x4d61c,
          call_b0=0x4d648, patch2=0x4d67c, call_b1=0x4d6b0, call_b2=0x4d6dc)
CALLS = ('call_b0', 'call_b1', 'call_b2')


class P(Case):
    """Case + hooks, where the hooks need the module's own section base."""

    def __init__(self, typ, rs=None, fast=1, idx=None, **kw):
        self.hits = []
        self.keys = []
        self.fast = fast
        self.idx = idx
        super().__init__(typ=typ, rs=rs, fast=fast, **kw)
        m = self.m
        text = m.sec_base[m.symtab[m.sym_index('tm_acl_fast_add_v4v6')]['st_shndx']]
        self.text = text
        # force [sp+16] (the extract index the real lookup returned) right
        # after the call that fills it, as t_xh.py does
        if idx is not None:
            def _poke(mm, addr, v=idx):
                mm.wr(mm.uc.reg_read(UC_ARM_REG_SP) + 16, 4, v)
            m.pokes[text + 0x4d350] = _poke
        for name, off in BK.items():
            if name in CALLS:
                m.pokes[text + off] = self._keyhook(name)
            else:
                m.pokes[text + off] = self._hit(name)
        self._rerun()

    def _rerun(self):
        self.got = []
        self.ret = self.m.run('tm_add_acl_flow_rule',
                              (self.flow, self.flow + 192, self.scratch),
                              max_steps=4000000)

    def _hit(self, name):
        def h(mm, addr):
            self.hits.append(name)
        return h

    def _keyhook(self, name):
        def h(mm, addr):
            r = [mm.uc.reg_read(x) for x in REG0_12[:4]]
            sp = mm.uc.reg_read(UC_ARM_REG_SP)
            self.keys.append((name, r[0], mm.rd(r[1], 45),
                              mm.rd(sp + 100, 60)))
        return h

    def entry(self):
        for kind, idx, data in self.got:
            if kind == 'hash':
                return idx, data
        return None, None

    def report(self, tag=''):
        slot, data = self.entry()
        print('== %s type %d fast %d ret %r slot %r paths %s'
              % (tag, self.typ, self.fast, self.ret, slot, sorted(set(self.hits))))
        for kind, idx, d in self.got:
            if kind == 'hash':
                print('   hash[%-4d] %s' % (idx, d.hex()))
            elif kind == 'xhash':
                print('   xhash   %-4s %s' % (idx, d.hex()))
        for name, bank, key, e in self.keys:
            print('   %-7s bank %d key45 %s' % (name, bank, key.hex()))
        for l in self.log[:4]:
            print('   log: %s' % l)


def paths(types):
    for fast in (1, 0):
        for t in types:
            P(t, fast=fast).report()


def maptag(typ, fast=1, lo=0x10, hi=0x124, step=32):
    print('### type %d fast %d: descriptor byte -> entry byte' % (typ, fast))
    base = P(typ, fast=fast)
    slot0, e0 = base.entry()
    print('   baseline (desc zero) entry %s' % (e0.hex() if e0 else None))
    for b in range(lo, hi, step):
        p = P(typ, fast=fast,
              rs={o: bytes([0x80 | ((o - b) & 0x3f)])
                  for o in range(b, min(b + step, hi))})
        slot, d = p.entry()
        if d is None:
            print('   block %#04x: no entry (%s)' % (b, p.log[:2]))
            continue
        diffs = ['e%d=%02x' % (i, d[i]) for i in range(60)
                 if e0 is None or d[i] != e0[i]]
        print('   block %#04x: %s' % (b, ' '.join(diffs)))


def keys(typ, fast=1, idx=None):
    p = P(typ, fast=fast, idx=idx)
    p.report()


def default_idx(typ):
    return 10 if typ in (5, 6) else 9


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'paths'
    rest = sys.argv[2:]
    if cmd == 'paths':
        paths([int(a) for a in rest] or [3, 4, 5, 6, 7, 8])
    elif cmd == 'map':
        maptag(int(rest[0]), fast=int(rest[1]) if len(rest) > 1 else 1)
    elif cmd == 'key':
        t = int(rest[0])
        keys(t, fast=int(rest[1]) if len(rest) > 1 else 1,
             idx=int(rest[2]) if len(rest) > 2 else default_idx(t))
