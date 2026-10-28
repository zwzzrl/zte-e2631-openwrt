#!/usr/bin/env python3
"""除了表项，原厂 init 还写了哪些 CLA 配置寄存器？

t_init2 只看了索引/规则表。真机上 CLA 低窗口 0x00..0x7c 全是 0（除了间接寄存器），
而模块里有一整套 cla_set_config / cla_set_outspace_cfg / cla_set_trap_acl_en_config /
cla_set_hash_poly_config / cla_set_local_ipv6_addr，这些正是「表之外」的配置。
"""
import struct, sys

from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

KO = '/ko/tm.ko'
NAMES = ('cla_set_config', 'cla_set_outspace_cfg', 'cla_set_trap_acl_en_config',
         'cla_set_hash_poly_config', 'cla_set_local_ipv6_addr', 'cla_set_ipv6_fast',
         'cla_set_extra_index_table', 'cla_set_extra_rule_table',
         'cla_get_extra_index_table', 'cla_get_extra_rule_table',
         'cla_get_hash_poly_config', 'cla_set_hash_table',
         'cla_set_external_hash_table', 'printk', '__memzero', 'memset',
         'fpga_read_reg', 'fpga_write_reg', 'tmOnuRegWrite', 'tmOnuRegRead')

m = Mod(KO, intercept=NAMES)


def dump_words(ptr, n):
    return ' '.join('%08x' % int.from_bytes(m.rd(ptr + 4 * i, 4), 'little') for i in range(n))


def H(name):
    def h(_n):
        r = [m.uc.reg_read(REG0_12[i]) for i in range(4)]
        if name == '__memzero':
            m.wr(r[0], r[1], b'\x00' * r[1])
        elif name == 'memset':
            m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
        elif name == 'cla_set_config':
            print('   cla_set_config  ptr %#x  struct[14] %s' % (r[0], dump_words(r[0], 14)))
        elif name == 'cla_set_outspace_cfg':
            print('   cla_set_outspace_cfg  outer_hash_num=%d outer_space_sel=%d' % (r[0], r[1]))
        elif name == 'cla_set_trap_acl_en_config':
            print('   cla_set_trap_acl_en_config  %d' % r[0])
        elif name == 'cla_set_hash_poly_config':
            print('   cla_set_hash_poly_config  ptr %#x  %s' % (r[0], dump_words(r[0], 8)))
        elif name == 'cla_set_local_ipv6_addr':
            print('   cla_set_local_ipv6_addr  ptr %#x  %s' % (r[0], dump_words(r[0], 4)))
        elif name == 'cla_set_extra_index_table':
            print('   cla_set_extra_index[%d] %s' % (r[0], dump_words(r[1], 5)))
        elif name == 'cla_set_extra_rule_table':
            print('   cla_set_extra_rule[%d] %s' % (r[0], dump_words(r[1], 17)))
        elif name == 'cla_set_external_hash_table':
            print('   cla_set_external_hash(args %d,%d) %s' % (r[0], r[1], dump_words(r[2], 10)))
        elif name == 'cla_set_hash_table':
            print('   cla_set_hash_table(%d) %s' % (r[0], dump_words(r[1], 15)))
        elif name == 'cla_get_extra_index_table':
            m.wr(r[1], 20, bytes(20))
        elif name == 'cla_get_extra_rule_table':
            m.wr(r[1], 68, bytes(68))
        elif name == 'cla_get_hash_poly_config':
            m.wr(r[0], 32, struct.pack('<8I', 0, 1, 0, 3, 0, 1, 0, 3))
        elif name == 'fpga_write_reg':
            print('   fpga_write_reg(reg %d, val %#x)' % (r[0], r[1]))
        elif name == 'tmOnuRegWrite':
            print('   tmOnuRegWrite(reg %d, val %#x)' % (r[0], r[1]))
        elif name == 'printk':
            try:
                txt = m.rd(r[0], 128).split(b'\x00')[0].decode('utf-8', 'replace')
            except Exception:
                txt = '<bad %#x>' % r[0]
            print('   printk: %r' % txt)
        m.uc.reg_write(UC_ARM_REG_R0, 0)
    return h


for n in NAMES:
    m.handlers[n] = H(n)

name = sys.argv[1] if len(sys.argv) > 1 else 'tm_acl_fast_init'
print('== %s entry %#x' % (name, m.func(name)[0]))
r = m.run(name, (0,), max_steps=3000000)
print('== returned %r steps %d calls %s' % (r, m.steps, m.calls[:24]))
