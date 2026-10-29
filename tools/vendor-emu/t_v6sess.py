#!/usr/bin/env python3
"""zte_api_fast_l3_session_add with a *working* memory boundary.

The first harness run stopped in kmem_cache_alloc/memcmp (both undefined in a
.ko and stubbed to return 0), so the vendor function gave up halfway with
"malloc memory fail" and all we ever saw was a template hash entry.  This
script gives the module a real allocator on the emulator heap and real
memcmp/memcpy/strlen, then records every table write of a complete run.

    python3 t_v6sess.py [family ...]        # default: 0 1 2 3
"""
import struct
import sys

from km import REG0_12
from unicorn.arm_const import UC_ARM_REG_R0
from flowadd import Case

SRC = bytes.fromhex('fd990000000000000000000000000002')
DST = bytes.fromhex('fd880000000000000000000000000002')
SPORT, DPORT = 48211, 9999

ALLOC_LOG = []
PRM = {'regw': [], 'regr': []}


def cstr(m, a):
    try:
        return m.rd(a, 200).split(b'\x00')[0].decode('utf-8', 'replace')
    except Exception:
        return '<%#x>' % a


def extra_handlers(case, alloc_size=0x800):
    m = case.m
    UC = m.uc

    def reg():
        return [UC.reg_read(REG0_12[i]) for i in range(4)]

    def setret(v):
        UC.reg_write(UC_ARM_REG_R0, v & 0xffffffff)

    def h_alloc(_n):
        r = reg()
        a = m.calloc(alloc_size, 16)
        ALLOC_LOG.append((a, r[0]))
        m.log('   alloc %#x (cache %#x size %#x)' % (a, r[0], alloc_size))
        setret(a)

    def h_kfree(_n):
        setret(0)

    def h_memcmp(_n):
        r = reg()
        a = m.rd(r[0], r[2]); b = m.rd(r[1], r[2])
        setret(0 if a == b else (-1 if a < b else 1))

    def h_memcpy(_n):
        r = reg()
        m.wr(r[0], r[2], m.rd(r[1], r[2]))
        setret(r[0])

    def h_strlen(_n):
        r = reg()
        s = m.rd(r[0], 400).split(b'\x00')[0]
        setret(len(s))

    def h_strcmp(_n):
        r = reg()
        a = m.rd(r[0], 200).split(b'\x00')[0]; b = m.rd(r[1], 200).split(b'\x00')[0]
        setret(0 if a == b else (-1 if a < b else 1))

    def h_strncpy(_n):
        r = reg()
        setret(r[0])

    def h_fmt(_n):
        r = reg()
        try:
            s = cstr(m, r[0])
            out = s % tuple() if '%' not in s else s
        except Exception:
            out = cstr(m, r[0])
        try:
            m.wr(r[1] if _n != 'sprintf' else r[0], min(len(out) + 1, 200), out.encode() + b'\x00')
        except Exception:
            pass
        setret(len(out))

    def h_onu_mac(_n):
        r = reg()
        idx, buf = r[0], r[1]
        mac = bytes.fromhex('5054aabbccdd') if idx == 1 else b'\x00' * 6
        m.wr(buf, 6, mac)
        setret(0)

    def h_done(_n):
        r = reg()
        try:
            m.wr(r[0], 4, (1).to_bytes(4, 'little'))
        except Exception:
            pass
        setret(0)

    def h_regw(_n):
        r = reg()
        PRM['regw'].append((r[0], r[1], r[2], r[3]))
        setret(0)

    def h_regr(_n):
        r = reg()
        PRM['regr'].append((r[0], r[1], r[2], r[3]))
        # reg 1 is the indirect-access status: bit 0 = "done", the board reads
        # 0x92388018 bit 0 as 1 (fifth-round probe), so hand the code a 1.
        try:
            m.wr(r[1], 4, (1).to_bytes(4, 'little'))
        except Exception:
            pass
        setret(0)

    def h_fpgar(_n):
        r = reg()
        PRM['regr'].append((r[0], r[1], r[2], r[3]))
        # fpga_read_reg(addr): the indirect-access status word reads back as 1 on
        # the board (0x92388018 bit0), everything else is "no data".
        setret(0xffffffff)

    names = {
        'fpga_read_reg': h_fpgar,
        'pp_pm_ind_acc_done_get': h_done,
        'tmOnuRegWrite': h_regw,
        'tmOnuRegRead': h_regr,
        'pp_pm_get_onu_mac_ram_info': h_onu_mac,
        'kmem_cache_alloc': h_alloc, '__kmalloc': h_alloc, 'kmalloc_order': h_alloc,
        'kfree': h_kfree, 'memcmp': h_memcmp, 'memcpy': h_memcpy,
        'strlen': h_strlen, 'strcmp': h_strcmp, 'strncpy': h_strncpy,
        'snprintf': h_fmt, 'sprintf': h_fmt,
    }
    return names


def entry_bytes(family, proto=0x11, v6=True, sport=SPORT, dport=DPORT,
                src=SRC, dst=DST, outport=5, vlan=1, inport=1):
    e = bytearray(0xa8)
    e[0x04:0x14] = src if v6 else src[12:] + b'\x00' * 12
    e[0x14:0x24] = dst if v6 else dst[12:] + b'\x00' * 12
    e[0x24:0x28] = struct.pack('>HH', sport, dport)
    e[0x28] = proto
    e[0x2E:0x30] = struct.pack('<H', family)
    # tm.ko copies them with pre-increment stores from +0x35 / +0x3b, so the
    # first byte lands on +0x36 / +0x3c: the MACs really are at +0x36 and +0x3c.
    e[0x36:0x3C] = bytes.fromhex('5054aabbccdd')
    e[0x3C:0x42] = bytes.fromhex('d843ae681828')
    e[0x6C] = vlan
    e[0x70:0x74] = struct.pack('<I', inport)
    e[0x74] = 0x02
    e[0x84:0x94] = dst
    e[0x98:0x9A] = struct.pack('<H', 1)
    e[0x9A:0x9C] = struct.pack('<H', 1)
    e[0x9C] = 0x01
    e[0x9D] = 0xFF
    e[0x9E] = outport
    e[0x9F] = 0x00
    return bytes(e)


def run(family, **kw):
    e = entry_bytes(family, **kw)
    rs = {i: e[i:i + 1] for i in range(len(e)) if e[i]}
    rs[0x04] = e[0x04:0x14]
    rs[0x14] = e[0x14:0x24]
    rs[0x84] = e[0x84:0x94]
    c = Case(typ=0, rs=rs, entry='zte_api_fast_l3_session_add', size=0xa8)
    c.m.handlers.update(extra_handlers(c))
    # re-run with the extra handlers installed
    c = Case(typ=0, rs=rs, entry='zte_api_fast_l3_session_add', size=0xa8,
             handlers=extra_handlers(None) if False else None)
    return c


def main(fams):
    for fam in fams:
        e = entry_bytes(fam)
        rs = {i: e[i:i + 1] for i in range(len(e)) if e[i]}
        rs[0x04] = e[0x04:0x14]
        rs[0x14] = e[0x14:0x24]
        rs[0x84] = e[0x84:0x94]
        c = Case.__new__(Case)
        # build manually so the extra handlers are in place before the run
        c.log = []; c.got = []; c.quiet = True; c.extidx = 0
        from flowadd import INTERCEPT
        from km import Mod
        c.m = m = Mod('/ko/tm.ko', intercept=INTERCEPT, log=c.note)
        for n in INTERCEPT:
            m.handlers[n] = c._h(n)
        m.handlers.update(extra_handlers(c))
        m.handlers['tmOnuRegRead'] = extra_handlers(c)['tmOnuRegRead']
        m.handlers['fpga_read_reg'] = extra_handlers(c)['fpga_read_reg']
        patch_onu_mac(m)
        flow = m.calloc(0xa8); m.wr(flow, 0xa8, b'\x00' * 0xa8)
        m.wr(flow + 12, 4, 0)
        for off, val in rs.items():
            m.wr(flow + off, len(val), val)
        l1 = c.sym('.LANCHOR1'); m.wr(l1 + 0x148, 4, 1)
        scratch = m.calloc(0x40); m.wr(scratch, 0x40, b'\x00' * 0x40)
        c.ret = m.run('zte_api_fast_l3_session_add', (flow, flow + 192, scratch),
                      max_steps=3000000)
        c.calls = list(m.calls); c.steps = m.steps
        print('== family %d ret %r steps %d' % (fam, c.ret, c.steps))
        for kind, idx, data in c.got:
            if kind == 'hash':
                w = [int.from_bytes(data[4*i:4*i+4], 'little') for i in range(15)]
                print('   hash [%d] %s' % (idx, ' '.join('%08x' % x for x in w)))
            elif kind == 'rule':
                w = [int.from_bytes(data[4*i:4*i+4], 'little') for i in range(17)]
                print('   rule [%d] %s' % (idx, ' '.join('%08x' % x for x in w)))
            elif kind == 'index':
                w = [int.from_bytes(data[4*i:4*i+4], 'little') for i in range(5)]
                print('   index[%d] %s' % (idx, ' '.join('%08x' % x for x in w)))
            else:
                print('   %-5s %s %s' % (kind, idx, data.hex()))
        for l in c.log[:10]:
            print('   log %s' % l)
        print('   calls %s' % (c.calls[:16],))
        print('   tmOnuRegWrite %d calls: %s' % (len(PRM['regw']), PRM['regw'][:40]))
        print('   tmOnuRegRead  %d calls: %s' % (len(PRM['regr']), PRM['regr'][:20]))




def patch_onu_mac(m):
    """pp_pm_get_onu_mac_ram_info() lives in tm.ko and reads a table the vendor
    fills at init; give it our MAC for ONU 1 and make it return 0."""
    import struct as _s
    addr, _size = m.func('pp_pm_get_onu_mac_ram_info')

    def fill(_m, a):
        idx = _m.uc.reg_read(REG0_12[0])
        buf = _m.uc.reg_read(REG0_12[1])
        mac = bytes.fromhex('5054aabbccdd') if idx == 1 else b'\x00' * 6
        _m.wr(buf, 6, mac)
    m.pokes[addr] = fill
    m.wr(addr, 8, _s.pack('<II', 0xe3a00000, 0xe12fff1e))


if __name__ == '__main__':
    main([int(a) for a in sys.argv[1:] if a.isdigit()] or [0, 1, 2, 3])
