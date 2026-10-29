#!/usr/bin/env python3
"""Exact arguments of tm.ko's cla_set_hash_table calls for the v6 session path.

The fast path and the session path both end in cla_set_hash_table, but with
different argument shapes, so guessing which buffer is "the key" from the
disassembly is error prone.  This records, for every call: r0..r3, the caller's
LR and SP, the 0xc0 bytes at SP (so both stack buffers are visible), and the
bytes at r1.

    python3 t_v6spy.py [inport]
"""
import struct
import sys

import flowadd
import t_v6flowdump
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_SP


def spy(entry):
    sh = []
    c = flowadd.Case.__new__(flowadd.Case)
    c.log, c.got, c.quiet, c.extidx = [], [], True, 0
    ic = tuple(n for n in flowadd.INTERCEPT
               if n != 'aclGetExtIdxRuleRamAddrByPort')
    c.m = m = Mod('/ko/tm.ko', intercept=ic, log=lambda *a: None)
    for n in ic:
        m.handlers[n] = c._h(n)

    def h_hash(_n):
        r = [m.uc.reg_read(x) for x in REG0_12]
        sp = m.uc.reg_read(UC_ARM_REG_SP)
        lr = m.uc.reg_read(UC_ARM_REG_LR)
        try:
            at_r1 = m.rd(r[1], 96).hex()
        except Exception:                                # noqa: BLE001
            at_r1 = '?'
        sh.append((lr, sp, r[:4], at_r1, m.rd(sp, 0xc0)))
        m.uc.reg_write(UC_ARM_REG_R0, 0)

    m.handlers['cla_set_hash_table'] = h_hash

    def h_status(_n):
        r = [m.uc.reg_read(x) for x in REG0_12]
        m.wr(r[0], 4, struct.pack('<I', 0))
        m.uc.reg_write(UC_ARM_REG_R0, 0)

    def h_alloc(_n):
        m.uc.reg_write(UC_ARM_REG_R0, m.calloc(0x200, 16))

    def h_memcmp(_n):
        r = [m.uc.reg_read(x) for x in REG0_12]
        m.uc.reg_write(UC_ARM_REG_R0,
                       0 if m.rd(r[0], r[2]) == m.rd(r[1], r[2]) else 1)

    def h_memcpy(_n):
        r = [m.uc.reg_read(x) for x in REG0_12]
        m.wr(r[0], r[2], m.rd(r[1], r[2]))
        m.uc.reg_write(UC_ARM_REG_R0, r[0])

    def h_strlen(_n):
        r = [m.uc.reg_read(x) for x in REG0_12]
        m.uc.reg_write(UC_ARM_REG_R0,
                       len(m.rd(r[0], 200).split(b'\x00')[0]))

    def h_fpgar(_n):
        m.uc.reg_write(UC_ARM_REG_R0, 0)

    m.handlers.update({'cla_get_indirect_rw_status': h_status,
                       'kmem_cache_alloc': h_alloc, '__kmalloc': h_alloc,
                       'kmalloc_order': h_alloc, 'memcmp': h_memcmp,
                       'memcpy': h_memcpy, 'strlen': h_strlen,
                       'fpga_read_reg': h_fpgar})
    for nm in ('findSubnetIdBySrcMac', 'operInfoAdd'):
        try:
            fe, _ = m.func(nm)
        except KeyError:
            continue
        m.wr(fe, 8, struct.pack('<II', 0xe3a00000, 0xe12fff1e))
    flow = m.calloc(0xa8)
    m.wr(flow, 0xa8, bytes(entry))
    out = m.calloc(0x40)
    l1 = c.sym('.LANCHOR1')
    m.wr(l1 + 0x148, 4, struct.pack('<I', 1))
    ret = m.run('zte_api_fast_l3_session_add', (flow, out, out),
                max_steps=2000000)
    return ret, sh


def dump(tag, sh):
    print('== %s: %d cla_set_hash_table call(s)' % (tag, len(sh)))
    for i, (lr, sp, r, at_r1, frame) in enumerate(sh):
        print('  call %d lr %#x sp %#x r0 %#x r1 %#x r2 %#x r3 %#x'
              % (i, lr, sp, r[0], r[1], r[2], r[3]))
        print('     @r1  %s' % at_r1)
        base = r[1] - sp
        for j in range(0, len(frame), 16):
            mark = ''
            if 0 <= base - j < 16:
                mark = '   <-- r1 (off %+#x)' % base
            print('     sp+%#04x %s%s' % (j, frame[j:j + 16].hex(), mark))


def main(argv):
    inport = int(argv[1], 0) if len(argv) > 1 else 2
    e = t_v6flowdump.build_entry(inport)
    ret, sh = spy(e)
    print('session[0x9e]=%d ret %r' % (inport, ret))
    dump('inport %d' % inport, sh)


if __name__ == '__main__':
    main(sys.argv)
