"""喂真实表内容，看原厂 v6 流添加到底把索引表第 4 字改成什么。

2026-10-28 真机回读（驱动 dump 参数）拿到了这些值：
  index  9 = 93929190 97969594 9b9a9998 9f9e9d9c 00150151
  index 10 = 03020100 07060504 0b0a0908 0f0e0d0c 00150154
  rule 152 = 22038608 000058a1 0 0 f00ff000 ffffffff ffffffff 0fffffff
             0 0 0 0 0 0 00700000 00092492 00000000
  rule 153 = e1828406 16284880 01e3868c 94264889 fff0000a ffffffff x8
             0fffffff 01300000 db6db6db 00db6db6
  CLA+0x90（多项式配置，ONU reg 12 掩码 0x00ff00ff）= 00e400e4

之前这些 get_* 都返回全零/桩值，所以 EN_V4FAST_EXTIDXTBL /
EN_V6FAST_EXTIDXTBL 那种「读出来、改几位、写回去」写回去的是假内容。现在让它们
读真值，于是写回来的第 4 字就是原厂打算让硬件看到的最终值。
"""
import struct

from flowadd import Case
from km import REG0_12
from unicorn.arm_const import UC_ARM_REG_SP

W = lambda *v: b''.join(struct.pack('<I', x) for x in v)

REAL_INDEX = {
    9: W(0x93929190, 0x97969594, 0x9b9a9998, 0x9f9e9d9c, 0x00150151),
    10: W(0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c, 0x00150154),
}
REAL_RULE = {
    152: W(0x22038608, 0x000058a1, 0, 0, 0xf00ff000, 0xffffffff, 0xffffffff,
           0x0fffffff, 0, 0, 0, 0, 0, 0, 0x00700000, 0x00092492, 0),
    153: W(0xe1828406, 0x16284880, 0x01e3868c, 0x94264889, 0xfff0000a,
           0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
           0xffffffff, 0xffffffff, 0xffffffff, 0x0fffffff, 0x01300000,
           0xdb6db6db, 0x00db6db6),
}
# cla_get_hash_poly_config 不是把寄存器原样拷出来：它把 ONU reg 12 拆成 8 个
# 2 位字段，寄存器位 0/2/4/6 进 out[0..3]，16/18/20/22 进 out[4..7]。
# reg 12 真机回读 0x00e400e4 = 0xe4:e4，每半 1110 0100 -> 字段 0,1,0,3。
POLY = W(0, 1, 0, 3, 0, 1, 0, 3)


def handlers(m, log):
    def h_index(name):
        r = [m.uc.reg_read(REG0_12[i]) for i in range(2)]
        n, buf = (r[0], r[1]) if r[0] < 64 else (r[1], r[0])
        data = REAL_INDEX.get(n, bytes(20))
        m.wr(buf, 20, data)
        log.append('     <- get_index(%d) -> %s' % (n, data.hex()))

    def h_rule(name):
        r = [m.uc.reg_read(REG0_12[i]) for i in range(2)]
        n, buf = (r[0], r[1]) if r[0] < 64 else (r[1], r[0])
        data = REAL_RULE.get(n, bytes(68))
        m.wr(buf, 68, data)
        log.append('     <- get_rule(%d) -> %s' % (n, data[:28].hex() + '...'))

    def h_poly(name):
        r0 = m.uc.reg_read(REG0_12[0])
        m.wr(r0, 32, POLY)
        log.append('     <- get_hash_poly_config -> %s' % POLY.hex())

    return {'cla_get_extra_index_table': h_index,
            'cla_get_extra_rule_table': h_rule,
            'cla_get_hash_poly_config': h_poly}


def build(typ, idx, real=True):
    log = []

    def pre(c):
        m = c.m
        text = m.sec_base[m.symtab[m.sym_index('tm_acl_fast_add_v4v6')]['st_shndx']]

        def poke(mm, a, idx=idx):
            sp = mm.uc.reg_read(UC_ARM_REG_SP)
            mm.wr(sp + 16, 4, idx)

        m.pokes[text + 0x4d350] = poke
        if real:
            m.handlers.update(handlers(m, log))

    return Case(typ=typ, pre_run=pre), log


def show(c, log, label):
    print('== %s ret %r' % (label, c.ret))
    for line in log:
        print(line)
    for kind, slot, data in c.got:
        if kind == 'index':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(5)]
            print('   W index[%-3d] %s' % (slot, ' '.join('%08x' % x for x in w)))
        elif kind == 'rule':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(17)]
            print('   W rule [%-3d] %s' % (slot, ' '.join('%08x' % x for x in w)))
        elif kind == 'hash':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(15)]
            print('   W hash [%-3d] %s' % (slot, ' '.join('%08x' % x for x in w)))
            print('          bytes %s' % data.hex())
        elif kind == 'xhash':
            print('   W xhash args %s struct %s' % (slot, data.hex()[:64]))
        else:
            print('   W %s[%s] %s' % (kind, slot, data.hex()[:120]))


if __name__ == '__main__':
    for typ in (3, 5):
        for idx in (9, 10):
            c, log = build(typ, idx)
            show(c, log, 'type %d extract %d' % (typ, idx))
            print()
