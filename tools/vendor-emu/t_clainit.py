#!/usr/bin/env python3
"""The vendor's classifier init, as a stream of indirect writes.

`tm_pon_pp_cla_initial()` is what leaves the CLA in the state the rest of tm.ko
expects: extract index entries, ACL rules, the "outspace" (external hash)
configuration and the hash polynomial.  Our driver hand-copied the stock
tables long before any of this was known, so dump the vendor's own init and
diff it against ours.

Only the bottom of the stack is stubbed (indirect rw / fpga / mem / alloc);
everything above it runs for real, so the table ids and indices are the
vendor's.

    python3 t_clainit.py [symbol]
"""
import struct
import sys

from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0
from unicorn import UC_HOOK_MEM_WRITE

TM = '/ko/tm.ko'

LOW = ('cla_set_indirect_rw_cmd', 'cla_set_indirect_rw_data',
       'cla_get_indirect_rw_data', 'cla_get_indirect_rw_status',
       'fpga_read_reg', 'fpga_write_reg', 'printk', '__memzero', 'memset',
       'kmem_cache_alloc', '__kmalloc', 'kmalloc_order', 'memcmp', 'memcpy',
       'strlen', '__copy_from_user', 'copy_from_user', '_raw_spin_lock_bh',
       '_raw_spin_unlock_bh', 'mutex_lock', 'mutex_unlock', 'udelay',
       'mdelay', 'msleep', 'schedule_timeout', 'jiffies')


def regs(m, n=4):
    return [m.uc.reg_read(REG0_12[i]) for i in range(n)]


def setr(m, v, i=0):
    m.uc.reg_write(REG0_12[i], v)


class Ram:
    def __init__(self):
        self.tab = {}
        self.cur = None
        self.order = []

    def cmd(self, table, index):
        self.cur = (table, index)
        if (table, index) not in self.order:
            self.order.append((table, index))

    def data(self, i, value):
        if self.cur is None:
            return
        table, index = self.cur
        self.tab.setdefault((table, index), {})[i] = value

    def words(self, table, index):
        d = self.tab.get((table, index), {})
        if not d:
            return []
        return [d.get(i, 0) for i in range(max(d) + 1)]

    def dump(self):
        for k in self.order:
            w = self.words(*k)
            print('   ram[%d][%d] =' % k, ' '.join('%08x' % x for x in w))


def run(sym='tm_pon_pp_cla_initial', args=(), max_steps=8000000):
    ram = Ram()
    prints = []
    stores = []
    m = Mod(TM, intercept=LOW, log=lambda *a: None)

    def h_cmd(_n):
        r = regs(m)
        ram.cmd(r[1], r[2])
        setr(m, 0)

    def h_data(_n):
        r = regs(m)
        ram.data(r[0], r[1])
        setr(m, 0)

    def h_status(_n):
        r = regs(m)
        m.wr(r[0], 4, struct.pack('<I', 0))
        setr(m, 0)

    def h_getdata(_n):
        r = regs(m)
        m.wr(r[1], 4, struct.pack('<I', 0))
        setr(m, 0)

    def h_fpgar(_n):
        setr(m, 0)

    def h_fpgaw(_n):
        r = regs(m)
        stores.append((r[0], r[1]))
        setr(m, 0)

    def h_printk(_n):
        r = regs(m)
        try:
            txt = m.rd(r[0], 160).split(b'\x00')[0].decode('utf-8', 'replace')
        except Exception:                                # noqa: BLE001
            txt = '<%#x>' % r[0]
        prints.append((txt, r[1], r[2], r[3]))
        setr(m, 0)

    def h_memzero(_n):
        r = regs(m)
        m.wr(r[0], r[1], b'\x00' * r[1])
        setr(m, 0)

    def h_memset(_n):
        r = regs(m)
        m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
        setr(m, 0)

    def h_alloc(_n):
        setr(m, m.calloc(0x400, 16))

    def h_memcmp(_n):
        r = regs(m)
        setr(m, 0 if m.rd(r[0], r[2]) == m.rd(r[1], r[2]) else 1)

    def h_memcpy(_n):
        r = regs(m)
        m.wr(r[0], r[2], m.rd(r[1], r[2]))
        setr(m, r[0])

    def h_strlen(_n):
        r = regs(m)
        setr(m, len(m.rd(r[0], 200).split(b'\x00')[0]))

    m.handlers.update({
        'cla_set_indirect_rw_cmd': h_cmd,
        'cla_set_indirect_rw_data': h_data,
        'cla_get_indirect_rw_status': h_status,
        'cla_get_indirect_rw_data': h_getdata,
        'fpga_read_reg': h_fpgar,
        'fpga_write_reg': h_fpgaw,
        'printk': h_printk,
        '__memzero': h_memzero,
        'memset': h_memset,
        'kmem_cache_alloc': h_alloc,
        '__kmalloc': h_alloc,
        'kmalloc_order': h_alloc,
        'memcmp': h_memcmp,
        'memcpy': h_memcpy,
        'strlen': h_strlen,
    })

    def mem_hook(uc, access, addr, size, value, _):
        if addr >= 0xf0000000:
            stores.append((addr, value))
    m.uc.hook_add(UC_HOOK_MEM_WRITE, mem_hook)

    ret = m.run(sym, tuple(args), max_steps=max_steps)
    return ret, ram, prints, stores, m


def main(argv):
    sym = argv[1] if len(argv) > 1 else 'tm_pon_pp_cla_initial'
    ret, ram, prints, stores, m = run(sym)
    print('== %s -> %r (steps %d)' % (sym, ret, m.steps))
    print('== ram writes (%d tables)' % len(ram.order))
    ram.dump()
    if prints:
        print('== printk')
        for t, a, b, c in prints[:40]:
            print('   %r %#x %#x %#x' % (t, a, b, c))
    ext = [(a, v) for a, v in stores if a >= 0xf0000000]
    if ext:
        print('== raw stores above 0xf0000000 (%d)' % len(ext))
        for a, v in ext[:40]:
            print('   %#010x = %#x' % (a, v))


if __name__ == '__main__':
    main(sys.argv)
