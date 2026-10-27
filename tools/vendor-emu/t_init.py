#!/usr/bin/env python3
"""Independent re-run of tm_acl_fast_init under unicorn, to cross-check the
68-byte rule and 20-byte index entries the asm interpreter produced."""
import sys

from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2

KO = '/ko/tm.ko'

INTERCEPT = ('cla_set_extra_index_table', 'cla_set_extra_rule_table',
             'cla_get_extra_index_table', 'cla_get_extra_rule_table',
             'cla_get_hash_poly_config', 'cla_set_hash_table', 'printk',
             '__memzero', 'memset', 'cla_get_indirect_rw_data',
             'cla_get_indirect_rw_status', 'cla_set_external_hash_table',
             'cla_get_external_hash_table', 'fpga_read_reg', 'fpga_write_reg')
m = Mod(KO, intercept=INTERCEPT)

got = []


def H(name):
    def h(_name):
        r = [m.uc.reg_read(REG0_12[i]) for i in range(4)]
        if name == '__memzero':
            m.wr(r[0], r[1], b'\x00' * r[1])
        elif name == 'memset':
            m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
        elif name == 'cla_set_extra_index_table':
            got.append(('index', r[0], m.rd(r[1], 20)))
        elif name == 'cla_set_extra_rule_table':
            got.append(('rule', r[0], m.rd(r[1], 68)))
        elif name == 'cla_set_hash_table':
            got.append(('hash', r[0], m.rd(r[1], 60)))
        elif name == 'cla_set_external_hash_table':
            got.append(('xhash', r[0], m.rd(r[1], 40)))
        elif name == 'cla_get_extra_index_table':
            m.wr(r[1], 20, b'\x00' * 20)
        elif name == 'cla_get_extra_rule_table':
            m.wr(r[1], 68, b'\x00' * 68)
        elif name == 'cla_get_hash_poly_config':
            m.wr(r[0], 32, b'\x00' * 32)
        elif name == 'printk':
            try:
                raw = m.rd(r[0], 128)
                txt = raw.split(b'\x00')[0].decode('utf-8', 'replace')
            except Exception:
                txt = '<bad fmt %#x>' % r[0]
            print('   printk: %r  args %#x %#x %#x' % (txt, r[1], r[2], r[3]))
        m.uc.reg_write(UC_ARM_REG_R0, 0)
    return h


for n in list(m.intercept):
    m.handlers[n] = H(n)

name = sys.argv[1] if len(sys.argv) > 1 else 'tm_acl_fast_init'
arg = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
print('== %s, reloc types %s, sections %d, size %#x'
      % (name, m.relcount, len(m.sec_base), m.modsize))
print('   entry %#x  stub count %d  unhandled-reloc %d'
      % (m.func(name)[0], m.nstub, m.badpc))
r = m.run(name, (arg,), max_steps=3000000)
print('   returned %r, steps %d, calls %s' % (r, m.steps, m.calls[:20]))
for kind, idx, data in got:
    if kind == 'index':
        w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(5)]
        print('   index[%-3d] %s' % (idx, ' '.join('%08x' % x for x in w)))
    elif kind == 'rule':
        w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(17)]
        print('   rule [%-3d] %s' % (idx, ' '.join('%08x' % x for x in w)))
    else:
        print('   %s[%d] %s' % (kind, idx, data.hex()))
