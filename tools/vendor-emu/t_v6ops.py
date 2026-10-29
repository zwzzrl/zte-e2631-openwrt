#!/usr/bin/env python3
"""The vendor's IPv6 fast-path write sequence, one indirect command at a time.

Every earlier emulator run stubbed `cla_set_hash_table` / `cla_set_extra_*`
out and only looked at their arguments.  That hides the half of the sequence
that matters on hardware: which table id and which index the vendor really
programs.  This script lets every table writer in tm.ko run for real and
stubs only the bottom of the stack (`cla_set_indirect_rw_cmd/data`,
`cla_set_indirect_rw_status`, `fpga_*`), recording the command stream into a
small model of the classifier RAM:

    cla_set_indirect_rw_cmd(sel, table, index)
    cla_set_indirect_rw_data(i, value)        -> model[table][index + i] = value

The model is printed at the end, so the result is the exact list of
(table, index, words) the vendor's own code writes for one IPv6 flow - and
`cla_set_external_hash_table` runs for real too, so its odd direct-store
addressing shows up as an access outside the module's own memory.

    python3 t_v6ops.py [sip] [dip] [sport] [dport] [inport]
"""
import struct
import sys

import flowadd
import t_hfsess
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

TM = '/ko/tm.ko'

LOW = ('cla_set_indirect_rw_cmd', 'cla_set_indirect_rw_data',
       'cla_get_indirect_rw_data', 'cla_get_indirect_rw_status',
       'fpga_read_reg', 'fpga_write_reg', 'printk', '__memzero', 'memset',
       'cla_get_hash_poly_config', 'aclGetExtIdxRuleRamAddrByPort',
       'tmOnuRegWrite', 'tmOnuGlbRegValidation', '__copy_from_user',
       'copy_from_user', '_raw_spin_lock_bh', '_raw_spin_unlock_bh',
       'mutex_lock', 'mutex_unlock', 'kmem_cache_alloc', '__kmalloc',
       'kmalloc_order', 'memcmp', 'memcpy', 'strlen')


def regs(m, n=4):
    return [m.uc.reg_read(REG0_12[i]) for i in range(n)]


def setr(m, v, i=0):
    m.uc.reg_write(REG0_12[i], v)


def build_session(sip, dip, sport, dport, inport, proto=0x11):
    import os
    if os.environ.get('LEGACY'):
        # the shape t_v6flowdump.py used: default desc type, no addresses in the
        # session, inport patched in by hand
        r = t_hfsess.run_switch(t_hfsess.build_desc(d14=1), log=lambda *a: None)
        e = bytearray(r[0] if isinstance(r, tuple) else r)
        e[0x9e] = inport
        return bytes(e)
    t_hfsess.SIP, t_hfsess.DIP = sip, dip
    t_hfsess.SPORT, t_hfsess.DPORT = sport, dport
    d = bytearray(t_hfsess.build_desc(d14=1, type=10, port=inport))
    d[0x10] = proto & 0xff
    sess = t_hfsess.run_switch(bytes(d), log=lambda *a: None)
    e = bytearray(sess[0] if isinstance(sess, tuple) else sess)
    e[0x9e] = inport
    return bytes(e)


class Ram:
    """(table, index) -> list of words, filled from the cmd/data stream."""

    def __init__(self):
        self.tab = {}
        self.cur = None
        self.ops = []

    def cmd(self, table, index):
        self.cur = (table, index)
        self.ops.append(('cmd', table, index, None))

    def data(self, i, value):
        if self.cur is None:
            return
        table, index = self.cur
        base = self.tab.setdefault((table, index), {})
        base[i] = value
        self.ops.append(('data', table, index, (i, value)))

    def words(self, table, index):
        d = self.tab.get((table, index), {})
        if not d:
            return []
        return [d.get(i, 0) for i in range(max(d) + 1)]

    def dump(self):
        for (table, index) in self.tab:
            w = self.words(table, index)
            print('   ram[%d][%d] =' % (table, index),
                  ' '.join('%08x' % x for x in w))


def run_full(sip, dip, sport, dport, inport, extra_intercept=()):
    entry = build_session(sip, dip, sport, dport, inport)
    ram = Ram()
    raw = []
    m = Mod(TM, intercept=tuple(LOW) + tuple(extra_intercept),
            log=lambda *a: None)

    def h_cmd(nm):
        def h(_n):
            r = regs(m)
            ram.cmd(r[1], r[2])
            setr(m, 0)
        return h

    def h_data(nm):
        def h(_n):
            r = regs(m)
            ram.data(r[0], r[1])
            setr(m, 0)
        return h

    def h_status(_n):
        r = regs(m)
        m.wr(r[0], 4, struct.pack('<I', 0))
        setr(m, 0)

    def h_getdata(_n):
        r = regs(m)
        m.wr(r[1], 4, struct.pack('<I', 0))
        setr(m, 0)

    def h_poly(_n):
        r = regs(m)
        m.wr(r[0], 32, b'\x00' * 32)
        setr(m, 0)

    def h_extidx(_n):
        r = regs(m)
        # lan1 = switch port 2 -> extract index 3
        m.wr(r[1], 4, struct.pack('<I', 3))
        setr(m, 0)

    def h_firstarg(_n):
        # index the incoming value is taken from (no direct-call variant here)
        setr(m, 0)

    def h_mem(_n):
        r = regs(m)
        if _n == 'printk':
            try:
                txt = m.rd(r[0], 160).split(b'\x00')[0].decode('utf-8', 'replace')
            except Exception:                            # noqa: BLE001
                txt = '<%#x>' % r[0]
            raw.append(('printk', r, txt))
            print('   printk %r  %#x %#x' % (txt, r[1], r[2]))
        else:
            if _n == '__memzero':
                m.wr(r[0], r[1], b'\x00' * r[1])
            elif _n == 'memset':
                m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
            setr(m, 0)

    def h_unmapped(uc, access, addr, size, value, _):
        raw.append(('touch', addr, size))
        return False

    def h_alloc(_n):
        setr(m, m.calloc(0x200, 16))

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
        'kmem_cache_alloc': h_alloc,
        '__kmalloc': h_alloc,
        'kmalloc_order': h_alloc,
        'memcmp': h_memcmp,
        'memcpy': h_memcpy,
        'strlen': h_strlen,
        'cla_set_indirect_rw_cmd': h_cmd('cmd'),
        'cla_set_indirect_rw_data': h_data('data'),
        'cla_get_indirect_rw_status': h_status,
        'cla_get_indirect_rw_data': h_getdata,
        'cla_get_hash_poly_config': h_poly,
        'aclGetExtIdxRuleRamAddrByPort': h_extidx,
        'fpga_read_reg': h_firstarg,
        'fpga_write_reg': h_firstarg,
        '__memzero': h_mem,
        'memset': h_mem,
        'printk': h_mem,
    })
    for nm in ('findSubnetIdBySrcMac', 'operInfoAdd'):
        try:
            fe, _ = m.func(nm)
        except KeyError:
            continue
        m.wr(fe, 8, struct.pack('<II', 0xe3a00000, 0xe12fff1e))

    flow = m.calloc(0xa8)
    m.wr(flow, 0xa8, entry)
    out = m.calloc(0x40)
    i = m.sym_index('zte_api_fast_l3_session_add')
    l1 = m.sec_base[m.symtab[m.sym_index('.LANCHOR1')]['st_shndx']] + \
        m.symtab[m.sym_index('.LANCHOR1')]['st_value']
    m.wr(l1 + 0x148, 4, struct.pack('<I', 1))
    srcbox = []

    def poke_x(mm, _a):
        r = regs(mm)
        try:
            srcbox.append((r[0], mm.rd(r[2], 60)))
        except Exception:                                # noqa: BLE001
            pass
    try:
        i3 = m.sym_index('cla_set_external_hash_table')
        m.pokes[m.sym_addr(m.symtab[i3])] = poke_x
    except Exception:                                    # noqa: BLE001
        pass

    def mem_hook(uc, access, addr, size, value, _):
        if addr >= 0xf0000000 or addr < 0x30000000:
            raw.append(('store', addr, value))
    from unicorn import UC_HOOK_MEM_WRITE
    m.uc.hook_add(UC_HOOK_MEM_WRITE, mem_hook)
    ret = m.run('zte_api_fast_l3_session_add', (flow, out, out),
                max_steps=4000000)
    for a, d in srcbox:
        print('== source entry handed to cla_set_external_hash_table'
              ' (addr arg %#x)' % a)
        for off in range(0, 60, 16):
            print('   +%03d  %s' % (off, d[off:off + 16].hex(' ')))
    return ret, ram, m, raw


def main(argv):
    sip = bytes.fromhex(argv[1]) if len(argv) > 1 else bytes.fromhex(
        'fdee70dfd02e00000000000000000002')
    dip = bytes.fromhex(argv[2]) if len(argv) > 2 else bytes.fromhex(
        'fd880000000000000000000000000002')
    sport = int(argv[3], 0) if len(argv) > 3 else 40000
    dport = int(argv[4], 0) if len(argv) > 4 else 9999
    inport = int(argv[5], 0) if len(argv) > 5 else 2
    ret, ram, m, raw = run_full(sip, dip, sport, dport, inport)
    print('== ret %r steps %d' % (ret, m.steps))
    print('== ram writes')
    ram.dump()
    print('== calls')
    seen = []
    for c in m.calls:
        if not seen or seen[-1][0] != c:
            seen.append([c, 1])
        else:
            seen[-1][1] += 1
    print('   ' + ', '.join('%s%s' % (c, '' if n == 1 else 'x%d' % n)
                            for c, n in seen))
    hook_w = [x for x in raw if x[0] == 'store']
    print('== stores outside module memory (%d)' % len(hook_w))
    for _, a, v in hook_w[:80]:
        print('   %#010x = %#x' % (a, v))
    ext = [x for x in raw if x[0] == 'store' and x[1] >= 0xf0000000]
    if ext:
        base = min(a for _, a, _ in ext) & ~0x3f
        try:
            img = m.uc.mem_read(base, 0x80)
        except Exception as exc:                        # noqa: BLE001
            img = b''
            print('   (read back failed: %s)' % exc)
        print('== external table image at %#x' % base)
        for off in range(0, len(img), 16):
            row = img[off:off + 16]
            print('   +%03d  %s' % (off, row.hex(' ')))
        print('== page of the source entry')
    for a, d in raw and (getattr(m, 'srcbox_dump', []) or []):
        pass
    print('== pages touched outside the module')
    for k in sorted(m.unmapped):
        if k < 0x30000000 or k >= 0x50100000:
            print('   %#010x access %s' % (k, m.unmapped[k]))


if __name__ == '__main__':
    main(sys.argv)
