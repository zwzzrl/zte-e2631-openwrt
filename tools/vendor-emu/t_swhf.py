#!/usr/bin/env python3
"""<What does the vendor's session-add entry point write for an IPv6 flow?>

switch.ko's sw_acl_l3_hardfast_session_add() builds a 168-byte session struct
and hands it to tm.ko's zte_api_fast_l3_session_add(session, out).  That is the
vendor's real "hardfast" add path - it ends in tm_add_acl_flow_rule(), i.e. the
CLA tables, which is why the sbrg/MLD table was a dead end for unicast.

This script calls zte_api_fast_l3_session_add() directly with a struct we
control and records every CLA boundary write:

    python3 t_swhf.py                 # zero struct: what comes out, ret
    python3 t_swhf.py sweep           # which struct byte lands in which output
    python3 t_swhf.py run <hex>       # one struct (first bytes)

The session layout is being mapped right now (that is what `sweep` is for);
nothing here is assumed about the caller.
"""
import sys

import flowadd
from flowadd import Case

# The PP "next hop ram" write is the neighbour/TTL-rewrite half of the fast
# path; it needs the PP indirect-access engine, which this harness does not
# model.  Stub it out for now: it has nothing to do with *matching* a CLA
# entry, which is what we are chasing here.
flowadd.INTERCEPT = tuple(flowadd.INTERCEPT) + ('pp_pm_set_next_hop_ram_info',
                                  'operInfoAdd')

ENTRY = 'zte_api_fast_l3_session_add'
SIZE = 0x100


def calls_in(m, name):
    """Every BL inside one function, as (site, target, symbol-or-None)."""
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    entry, size = m.func(name)[:2]
    code = m.rd(entry, size)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    out = []
    for ins in md.disasm(code, entry):
        if ins.mnemonic != 'bl' or not ins.operands:
            continue
        op = ins.operands[0]
        tgt = getattr(op, 'imm', None)
        if tgt is None:
            continue
        sym = None
        i = m.sym_index_by_addr(tgt) if hasattr(m, 'sym_index_by_addr') else None
        out.append((ins.address, tgt, sym))
    return out


def skip_calls(m, name, wanted, log):
    """Turn the BLs to @wanted inside @name into "return 0" by skipping them."""
    wanted = set(wanted)
    for site, tgt, _sym in calls_in(m, name):
        if tgt not in wanted:
            continue
        log('   skip call at %#x -> %#x' % (site, tgt))

        def h(mm, addr):
            from km import REG0_12
            mm.uc.reg_write(REG0_12[0], 0)
            mm.uc.reg_write(15, addr + 4)      # r15 = pc

        m.pokes[site] = h


def session(rs=None, typ=5, size=SIZE):
    """One run: the struct at r0 comes from Case (rs overrides its bytes)."""
    from km import REG0_12
    holder = {}

    def h_alloc(_n):
        m = holder['m']
        m.uc.reg_write(REG0_12[0], m.calloc(0x4000, 64))

    def h_free(_n):
        holder['m'].uc.reg_write(REG0_12[0], 0)

    def setup(case):
        holder['m'] = case.m
        case.prelude = []
        for name in ('operListInit', 'tm_db_init', 'aclRamInit', 'tm_sdk_init'):
            try:
                r = case.m.run(name, (), max_steps=3000000)
            except KeyError:
                continue
            case.prelude.append((name, r))
        try:
            skip_calls(case.m, ENTRY,
                       (case.m.func('operInfoAdd')[0],
                        case.m.func('pp_pm_set_next_hop_ram_info')[0]),
                       case.prelude.append)
        except KeyError as exc:
            case.prelude.append(('skip failed', exc))

    def h_status(_n):
        # cla_get_indirect_rw_status(dst): the hardware sets bit 0 when the
        # indirect access has completed; without it the PP path times out.
        m = holder['m']
        m.wr(m.uc.reg_read(REG0_12[0]), 4, b'\x01\x00\x00\x00')
        m.uc.reg_write(REG0_12[0], 0)

    def h_data(_n):
        m = holder['m']
        m.wr(m.uc.reg_read(REG0_12[0]), 4, b'\x00' * 4)
        m.uc.reg_write(REG0_12[0], 0)

    def h_ok(_n):
        holder['m'].uc.reg_write(REG0_12[0], 0)

    handlers = {'kmem_cache_alloc': h_alloc, 'kmem_cache_free': h_free,
                'kfree': h_free,
                'cla_get_indirect_rw_status': h_status,
                'cla_get_indirect_rw_data': h_data,
                'pp_pm_ind_acc_done_get': h_status,
                'pp_pm_set_next_hop_ram_info': h_ok,
                'operInfoAdd': h_ok}
    return Case(typ=typ, rs=rs, entry=ENTRY, size=size, pre_run=setup,
                handlers=handlers)


def outputs(p):
    out = {}
    for kind, idx, data in p.got:
        out['%s[%s]' % (kind, idx)] = data
    return out


def show(rs=None):
    p = session(rs)
    print('ret %r steps %s' % (p.ret, p.steps))
    for kind, idx, data in p.got:
        print('   %-6s %-10s %s' % (kind, idx, data.hex()))
    for item in p.prelude:
        print('   prelude', item)
    for l in p.log[:10]:
        print('   log', l)


def sweep():
    base = session()
    b0 = outputs(base)
    for k, v in b0.items():
        print('base %-14s %s' % (k, v.hex()[:120]))
    for off in range(0, 0xa8, 4):
        rs = {off + k: bytes([0x80 | (k & 0x3f)]) for k in range(4)}
        p = session(rs)
        got = outputs(p)
        for label, data in got.items():
            old = b0.get(label, b'')
            for i, b in enumerate(data):
                if b & 0x80 and b != 0x80 and (i >= len(old) or old[i] != b):
                    print('   sess[%#04x] -> %s byte %d = %#04x'
                          % (off + (b & 0x3f), label, i, b))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'sweep':
        sweep()
        return
    if len(sys.argv) > 1 and sys.argv[1] == 'run':
        raw = bytes.fromhex(sys.argv[2]) if len(sys.argv) > 2 else b''
        rs = {i: raw[i:i + 1] for i in range(len(raw))}
        show(rs)
        return
    show()


if __name__ == '__main__':
    main()
