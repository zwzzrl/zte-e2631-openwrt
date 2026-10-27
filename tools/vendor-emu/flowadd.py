#!/usr/bin/env python3
"""One run of tm.ko's flow-add path with a synthetic flow descriptor.

Kept importable so that a sweep can build a fresh module image per run: the
path writes globals (rule bookkeeping, ports) and those must not leak from one
experiment into the next.
"""
import struct

from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

KO = '/ko/tm.ko'
INTERCEPT = ('cla_set_extra_index_table', 'cla_set_extra_rule_table',
             'cla_get_extra_index_table', 'cla_get_extra_rule_table',
             'cla_get_hash_poly_config', 'cla_get_hash_table',
             'cla_set_hash_table', 'cla_get_external_hash_table',
             'cla_set_external_hash_table', 'printk', '__memzero', 'memset',
             'cla_get_indirect_rw_data', 'cla_get_indirect_rw_status',
             'cla_set_indirect_rw_cmd', 'fpga_read_reg', 'fpga_write_reg',
             '__copy_from_user', 'copy_from_user', '_raw_spin_lock_bh',
             '_raw_spin_unlock_bh', 'mutex_lock', 'mutex_unlock',
             'aclGetExtIdxRuleRamAddrByPort', 'tmOnuRegWrite',
             'tmOnuGlbRegValidation')


def cstr(m, a):
    try:
        return m.rd(a, 160).split(b'\x00')[0].decode('utf-8', 'replace')
    except Exception:
        return '<%#x>' % a


class Case:
    """rs: {offset: bytes} written into the flow descriptor before the run."""

    def __init__(self, typ=3, rs=None, fast=1, tail=None, quiet=True,
                 entry='tm_add_acl_flow_rule', size=0x124, extidx=0):
        self.log = []
        self.got = []
        self.extidx = extidx
        self.typ = typ
        m = self.m = Mod(KO, intercept=INTERCEPT, log=self.note)
        self.quiet = quiet
        for n in INTERCEPT:
            m.handlers[n] = self._h(n)
        flow = m.calloc(size)
        m.wr(flow, size, b'\x00' * size)
        m.wr(flow + 12, 4, typ)
        for off, val in (rs or {}).items():
            m.wr(flow + off, len(val), val)
        l1 = self.sym('.LANCHOR1')
        m.wr(l1 + 0x148, 4, fast)
        scratch = m.calloc(0x40)
        m.wr(scratch, 0x40, b'\x00' * 0x40)
        self.flow, self.scratch = flow, scratch
        self.ret = m.run(entry, (flow, flow + 192, scratch), max_steps=3000000)
        self.calls = list(m.calls)
        self.steps = m.steps

    def note(self, msg):
        self.log.append(msg)

    def sym(self, name):
        i = self.m.sym_index(name)
        s = self.m.symtab[i]
        return self.m.sec_base[s['st_shndx']] + s['st_value']

    def _h(self, name):
        m = self.m
        UC = m.uc

        def h(_n):
            UC_reg = REG0_12
            r = [UC.reg_read(UC_reg[i]) for i in range(4)]
            if name == '__memzero':
                m.wr(r[0], r[1], b'\x00' * r[1])
            elif name == 'memset':
                m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
            elif name == 'printk':
                if not self.quiet:
                    print('   printk %r %#x %#x' % (cstr(m, r[0]), r[1], r[2]))
                self.log.append('printk:' + cstr(m, r[0]))
            elif name == 'cla_set_extra_index_table':
                self.got.append(('index', r[0], m.rd(r[1], 20)))
            elif name == 'cla_set_extra_rule_table':
                self.got.append(('rule', r[0], m.rd(r[1], 68)))
            elif name == 'cla_set_hash_table':
                self.got.append(('hash', r[0], m.rd(r[1], 60)))
            elif name == 'cla_set_external_hash_table':
                # (index, arg1, struct at r2)
                self.got.append(('xhash', (r[0], r[1]), m.rd(r[2], 60)))
            elif name == 'cla_get_extra_index_table':
                m.wr(r[1], 20, b'\x00' * 20)
            elif name == 'cla_get_extra_rule_table':
                m.wr(r[1], 68, b'\x00' * 68)
            elif name == 'cla_get_hash_poly_config':
                m.wr(r[0], 32, b'\x00' * 32)
            elif name == 'cla_get_hash_table':
                m.wr(r[1], 60, b'\x00' * 60)
            elif name in ('aclGetExtIdxRuleRamAddrByPort',):
                m.wr(r[1], 4, self.extidx)
                UC.reg_write(UC_ARM_REG_R0, 0)
                return
            elif name in ('__copy_from_user', 'copy_from_user'):
                UC.reg_write(UC_ARM_REG_R0, 0)
                return
            UC.reg_write(UC_ARM_REG_R0, 0)
        return h

    def hash_entry(self):
        for kind, idx, data in self.got:
            if kind == 'hash':
                return idx, data
        return None, None

    def dump(self):
        print('  returned %r steps %d calls %s'
              % (self.ret, self.steps, self.calls[:14]))
        for kind, idx, data in self.got:
            if kind == 'index':
                w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(5)]
                print('   index[%-3d] %s' % (idx, ' '.join('%08x' % x for x in w)))
            elif kind == 'rule':
                w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(17)]
                print('   rule [%-3d] %s' % (idx, ' '.join('%08x' % x for x in w)))
            elif kind == 'hash':
                w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(15)]
                print('   hash [%-3d] %s' % (idx, ' '.join('%08x' % x for x in w)))
                print('        bytes %s' % data.hex())
            else:
                print('   %s[%d] %s' % (kind, idx, data.hex()))
