#!/usr/bin/env python3
"""Where does the vendor IPv6 session add give up?  Rerun t_v6sess's flow and
record the caller PCs of every printk, so a failing check can be read off the
disassembly instead of guessed."""
import sys
from flowadd import Case
from t_v6sess import entry_bytes, extra_handlers, cstr
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

FAM = int(sys.argv[1]) if len(sys.argv) > 1 else 3


def main():
    e = entry_bytes(FAM)
    rs = {i: e[i:i+1] for i in range(len(e)) if e[i]}
    rs[0x04] = e[0x04:0x14]; rs[0x14] = e[0x14:0x24]; rs[0x84] = e[0x84:0x94]
    c = Case.__new__(Case)
    c.log = []; c.got = []; c.quiet = True; c.extidx = 0
    from flowadd import INTERCEPT
    c.m = m = Mod('/ko/tm.ko', intercept=INTERCEPT, log=c.note)
    for n in INTERCEPT:
        m.handlers[n] = c._h(n)
    m.handlers.update(extra_handlers(c))
    printks = []
    base_printk = m.handlers['printk']

    def pk(name):
        r0 = m.uc.reg_read(UC_ARM_REG_R0)
        try:
            raw = m.rd(r0, 48).split(b'\x00')[0]
        except Exception:
            raw = b''
        pcs = list(m.recent)
        printks.append(('r0=%#x %r' % (r0, raw), pcs))
        base_printk(name)
    m.handlers['printk'] = pk
    flow = m.calloc(0xa8); m.wr(flow, 0xa8, b'\x00' * 0xa8)
    for off, val in rs.items():
        m.wr(flow + off, len(val), val)
    l1 = c.sym('.LANCHOR1'); m.wr(l1 + 0x148, 4, 1)
    scratch = m.calloc(0x40); m.wr(scratch, 0x40, b'\x00' * 0x40)
    ret = m.run('zte_api_fast_l3_session_add', (flow, flow + 192, scratch), max_steps=3000000)
    print('family %d ret %r steps %d' % (FAM, ret, m.steps))
    base = m.sec_base[1]
    for s, pcs in printks:
        print('printk %r' % s.strip())
        print('   recent: %s' % ' '.join('%#x' % (p - base) for p in pcs))
    for kind, idx, data in c.got:
        print('  %s[%d] %s' % (kind, idx, data.hex()))


main()
