#!/usr/bin/env python3
"""Full ordered trace of the vendor's IPv6 flow-add.

Earlier rounds only looked at the three table writers (hash / extra rule /
extra index).  This one pokes *every* cla_*/acl_*/fpga_* function entry and
prints the argument registers, so the whole sequence the vendor's own code
executes for one IPv6 flow becomes visible - including the register writes
that the internal/external-hash decision at 0x4e728 in aclBuildHashAndAclRule
depends on.

    python3 t_v6trace.py                # default tuple, inport 2 = lan1
    python3 t_v6trace.py <sip> <dip> [sport] [dport] [inport]
"""
import struct
import sys

import t_hfsess
import t_v6real
import flowadd
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

TM = '/ko/tm.ko'


def r0v(m, n=4):
    return [m.uc.reg_read(REG0_12[i]) for i in range(n)]


def run(sip, dip, sport, dport, inport, want_keys=False):
    t_hfsess.SIP, t_hfsess.DIP = sip, dip
    t_hfsess.SPORT, t_hfsess.DPORT = sport, dport
    d = bytearray(t_hfsess.build_desc(d14=1, type=10, port=inport))
    d[0x10] = 0x11
    sess = t_hfsess.run_switch(bytes(d), log=lambda *a: None)
    e = bytearray(sess[0] if isinstance(sess, tuple) else sess)
    e[0x9e] = inport

    c = flowadd.Case.__new__(flowadd.Case)
    c.log, c.got, c.quiet, c.extidx = [], [], True, 0
    ic = tuple(n for n in flowadd.INTERCEPT
               if n != 'aclGetExtIdxRuleRamAddrByPort')
    c.m = m = Mod(TM, intercept=ic, log=lambda *a: None)
    for n in ic:
        m.handlers[n] = c._h(n)

    trace = []
    keys = []

    def mk(nm):
        def h(mm, _a):
            r = r0v(mm)
            note = ''
            if nm in ('cla_set_hash_table', 'cla_set_external_hash_table'):
                note = ' key45=' + mm.rd(r[1], 45).hex()
            elif nm in ('cla_set_extra_index_table', 'cla_set_extra_rule_table'):
                note = ' data=' + mm.rd(r[1], 8).hex()
            trace.append((nm, r, note))
        return h

    seen = set()
    for sym in m.symtab:
        nm = sym.name
        if not nm or nm in seen:
            continue
        if not (nm.startswith(('cla_', 'acl', 'sw_acl', 'tm_acl', 'fpga_'))):
            continue
        a = m.sym_addr(sym)
        if a is None:
            continue
        seen.add(nm)
        try:
            m.pokes[a] = mk(nm)
        except Exception:                                # noqa: BLE001
            pass

    def h_status(_n):
        r = r0v(m)
        m.wr(r[0], 4, struct.pack('<I', 0))
        m.uc.reg_write(UC_ARM_REG_R0, 0)

    def h_alloc(_n):
        m.uc.reg_write(UC_ARM_REG_R0, m.calloc(0x200, 16))

    def h_memcmp(_n):
        r = r0v(m)
        same = 0 if m.rd(r[0], r[2]) == m.rd(r[1], r[2]) else 1
        m.uc.reg_write(UC_ARM_REG_R0, same)

    def h_memcpy(_n):
        r = r0v(m)
        m.wr(r[0], r[2], m.rd(r[1], r[2]))
        m.uc.reg_write(UC_ARM_REG_R0, r[0])

    def h_strlen(_n):
        r = r0v(m)
        m.uc.reg_write(UC_ARM_REG_R0, len(m.rd(r[0], 200).split(b'\x00')[0]))

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
    if want_keys:
        def _pk(mm, _a):
            keys.append(mm.rd(mm.uc.reg_read(REG0_12[1]), 48))
        i2 = m.sym_index('cla_acl_hash_addr_gen')
        if i2 is not None:
            m.pokes[m.sym_addr(m.symtab[i2])] = _pk

    flow = m.calloc(0xa8)
    m.wr(flow, 0xa8, bytes(e))
    out = m.calloc(0x40)
    l1 = c.sym('.LANCHOR1')
    m.wr(l1 + 0x148, 4, struct.pack('<I', 1))
    ret = m.run('zte_api_fast_l3_session_add', (flow, out, out),
                max_steps=2000000)
    return ret, trace, c.got, keys, m


def main(argv):
    sip = bytes.fromhex(argv[1]) if len(argv) > 1 else bytes.fromhex(
        'fdee70dfd02e00000000000000000002')
    dip = bytes.fromhex(argv[2]) if len(argv) > 2 else bytes.fromhex(
        'fd880000000000000000000000000002')
    sport = int(argv[3], 0) if len(argv) > 3 else 40000
    dport = int(argv[4], 0) if len(argv) > 4 else 9999
    inport = int(argv[5], 0) if len(argv) > 5 else 2
    ret, trace, got, _, _m = run(sip, dip, sport, dport, inport)
    print('== ret %r, %d calls' % (ret, len(trace)))
    for nm, r, note in trace:
        print('   %-34s r0..r3=%s%s'
              % (nm, ' '.join('%#010x' % x for x in r), note))
    print('== table writes')
    for kind, a, data in got:
        print('   %-6s %r %s' % (kind, a, data.hex()))


if __name__ == '__main__':
    main(sys.argv)
