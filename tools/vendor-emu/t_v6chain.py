#!/usr/bin/env python3
"""The vendor's IPv6 session add, end to end, in two emulators.

Round ten.  Everything before this jumped into tm.ko one layer too low: it
called tm_add_acl_flow_rule() (or zte_api_fast_l3_session_add()) with a hand
made "descriptor".  switch.ko's side had never been run, so the 168-byte entry
tm.ko actually receives was never seen.  This script runs both halves in order:

    switch.ko  sw_acl_l3_hardfast_session_add(session)
      -> sw_acl_setMtchInfo()  builds the 168-byte hffEntry
      -> zte_api_fast_l3_session_add(hffEntry, out)      [tm.ko]

Part 1 loads switch.ko, fills its session struct the way the switch driver does
(+0x00 -> 16-byte source address, +0x04 -> destination, +0x08/+0x0a the ports,
+0x0c the type, +0x10 the protocol, +0x14 a flag, +0x18 the input device name)
and records the entry it would hand to tm.ko.  Part 2 feeds that entry to tm.ko
under the same Unicorn harness the other scripts use and prints every
cla_set_hash_table()/cla_set_extra_rule_table()/cla_set_extra_index_table()
call.  The point: see what a *real* IPv6 session add writes, with the caller's
own entry rather than ours.

    python3 t_v6chain.py            # IPv6 UDP flow, type 6, entry as built
    python3 t_v6chain.py sweep      # also re-run tm.ko with entry+0x2e = 0..3
"""
import struct
import sys

from flowadd import INTERCEPT as TM_INTERCEPT
from flowadd import cstr
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0

SWITCH = '/ko/switch.ko'
TM = '/ko/tm.ko'
SRC = bytes.fromhex('fd990000000000000000000000000002')
DST = bytes.fromhex('fd880000000000000000000000000002')
SPORT, DPORT = 48211, 9999

SW_INTER = ('printk', '__memzero', 'memset', 'get_sw_port_from_devname', 'TO_WAN',
            'zte_api_fast_l3_session_add', '_raw_spin_lock_bh',
            '_raw_spin_unlock_bh', '__printk_ratelimit', '___ratelimit',
            'strcmp', 'strncmp', 'dev_get_by_name', 'kmem_cache_alloc', 'kfree')


def build_session(m, typ=6, flag=1, proto=0x11, dev=b'eth0\x00'):
    sess = m.calloc(0x100)
    a = m.calloc(16)
    b = m.calloc(16)
    nm = m.calloc(32)
    m.wr(a, 16, SRC)
    m.wr(b, 16, DST)
    m.wr(nm, 32, dev + b'\x00' * 20)
    m.wr(sess, 0x100, b'\x00' * 0x100)
    m.wr(sess + 0x00, 4, struct.pack('<I', a))
    m.wr(sess + 0x04, 4, struct.pack('<I', b))
    m.wr(sess + 0x08, 2, struct.pack('<H', SPORT))
    m.wr(sess + 0x0a, 2, struct.pack('<H', DPORT))
    m.wr(sess + 0x0c, 4, struct.pack('<I', typ))
    m.wr(sess + 0x10, 1, bytes([proto]))
    m.wr(sess + 0x14, 1, bytes([flag]))
    m.wr(sess + 0x18, 4, struct.pack('<I', nm))
    return sess


def switch_entry(typ=6, flag=1, proto=0x11, dev=b'eth0\x00', quiet=True):
    """Run switch.ko's session add; return the 168 bytes handed to tm.ko."""
    got = []
    m = Mod(SWITCH, intercept=SW_INTER)

    def cstr0(a):
        try:
            return m.rd(a, 80).split(b'\x00')[0].decode('utf-8', 'replace')
        except Exception:
            return '<?>'

    def H(name):
        def h(_n):
            r = [m.uc.reg_read(REG0_12[i]) for i in range(4)]
            if name == 'printk':
                if not quiet:
                    print('   sw printk %r' % cstr0(r[0]))
            elif name == '__memzero':
                m.wr(r[0], r[1], b'\x00' * r[1])
            elif name == 'memset':
                m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
            elif name == 'get_sw_port_from_devname':
                m.uc.reg_write(REG0_12[0], 0)
                m.wr(r[1], 4, struct.pack('<I', 3))
                return
            elif name == 'zte_api_fast_l3_session_add':
                got.append(m.rd(r[0], 0xa8))
                m.uc.reg_write(REG0_12[0], 0)
                return
            elif name in ('strcmp', 'strncmp'):
                a, b = cstr0(r[0]), cstr0(r[1])
                m.uc.reg_write(REG0_12[0], 0 if a == b else (1 if a > b else -1))
                return
            m.uc.reg_write(REG0_12[0], 0)
        return h

    for n in SW_INTER:
        m.handlers[n] = H(n)
    sess = build_session(m, typ=typ, flag=flag, proto=proto, dev=dev)
    ret = m.run('sw_acl_l3_hardfast_session_add', (sess,), max_steps=2000000)
    if not got:
        raise SystemExit('switch.ko never reached tm.ko (ret %r, calls %s)'
                         % (ret, m.calls[:12]))
    return got[0]


def tm_run(entry, extidx=0):
    """Feed one 168-byte entry to tm.ko; return the recorded table writes."""
    m = Mod(TM, intercept=TM_INTERCEPT)
    got = []

    def H(name):
        def h(_n):
            r = [m.uc.reg_read(REG0_12[i]) for i in range(4)]
            if name == '__memzero':
                m.wr(r[0], r[1], b'\x00' * r[1])
            elif name == 'memset':
                m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
            elif name == 'printk':
                pass
            elif name == 'cla_set_extra_index_table':
                got.append(('index', r[0], m.rd(r[1], 20)))
            elif name == 'cla_set_extra_rule_table':
                got.append(('rule', r[0], m.rd(r[1], 68)))
            elif name == 'cla_set_hash_table':
                got.append(('hash', r[0], m.rd(r[1], 60)))
            elif name == 'cla_set_external_hash_table':
                got.append(('xhash', r[0], m.rd(r[2], 60)))
            elif name in ('cla_get_extra_index_table', 'cla_get_extra_rule_table',
                          'cla_get_hash_table'):
                pass
            elif name == 'cla_get_hash_poly_config':
                m.wr(r[0], 32, struct.pack('<8I', 0, 1, 0, 3, 0, 1, 0, 3))
            elif name == 'aclGetExtIdxRuleRamAddrByPort':
                m.uc.reg_write(REG0_12[0], 0)
                m.wr(r[1], 4, struct.pack('<I', extidx))
                return
            elif name in ('cla_get_indirect_rw_status', 'pp_pm_ind_acc_done_get'):
                m.wr(r[0], 4, b'\x01\x00\x00\x00')
            m.uc.reg_write(REG0_12[0], 0)
        return h

    for n in TM_INTERCEPT:
        m.handlers[n] = H(n)
    # the SubRam/QoS managers this kernel would have set up at boot: the add
    # path only asks them for a rewrite slot, which is not what we are chasing
    for name in ('operInfoAdd', 'pp_pm_set_b4_ipv6_ram_info',
                 'pp_pm_set_next_hop_ram_info', 'pp_pm_set_modify_ram_info',
                 'pp_pm_set_sub_ram_info', 'pp_pm_set_cmd_ram_info',
                 'pp_pm_set_pri_ram_info', 'pp_pm_set_vlan_modify_ram_info'):
        try:
            m.uc.mem_write(m.func(name)[0], struct.pack('<II', 0xe3a00000, 0xe12fff1e))
        except KeyError:
            pass
    buf = m.calloc(0xa8)
    m.wr(buf, 0xa8, entry)
    out = m.calloc(0x40)
    ret = m.run('zte_api_fast_l3_session_add', (buf, out), max_steps=3000000)
    return ret, got


def show(tag, entry, ret, got):
    print('== %s: ret %r writes %d' % (tag, ret, len(got)))
    for kind, idx, data in got:
        if kind == 'rule':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(17)]
            print('   rule [%-3s] %s' % (idx, ' '.join('%08x' % x for x in w)))
        elif kind == 'index':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(5)]
            print('   index[%-3s] %s' % (idx, ' '.join('%08x' % x for x in w)))
        else:
            print('   %-5s %-4s %s' % (kind, idx, data.hex()))


def baseline(entry):
    ret, got = tm_run(entry)
    for kind, idx, data in got:
        if kind == 'hash':
            return data
    return None


def mark(entry, lo=0x00, hi=0xa8, stride=1):
    """Which entry byte lands in which byte of the hash entry tm.ko writes."""
    b0 = baseline(entry)
    if b0 is None:
        print('no hash entry in the baseline'); return
    print('baseline %s' % b0.hex())
    for off in range(lo, hi, stride):
        for k in range(0, min(stride, hi - off)):
            b = bytearray(entry)
            b[off + k] = 0x5A
            ret, got = tm_run(bytes(b))
            h = None
            for kind, idx, data in got:
                if kind == 'hash':
                    h = data
            if h is None:
                print('e[%#04x] = no hash write (ret %r)' % (off + k, ret))
                continue
            diffs = ['h%d=%02x' % (i, h[i]) for i in range(len(h))
                     if 0 <= i < len(b0) and h[i] != b0[i]]
            print('e[%#04x] -> slot %s  %s' % (off + k,
                  [idx for kind, idx, _ in got if kind == 'hash'],
                  ' '.join(diffs) if diffs else '(no change)'))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'map':
        lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x00
        hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0xa8
        st = int(sys.argv[4], 0) if len(sys.argv) > 4 else 1
        mark(switch_entry(), lo, hi, st)
        return
    if len(sys.argv) > 1 and sys.argv[1] == 'sweep':
        for typ in (3, 5, 6, 7):
            e = switch_entry(typ=typ)
            for fam in (0, 1, 2, 3):
                b = bytearray(e)
                b[0x2e:0x30] = struct.pack('<H', fam)
                ret, got = tm_run(bytes(b))
                show('type %d, entry+0x2e = %d' % (typ, fam), bytes(b), ret, got)
        return
    e = switch_entry()
    print('entry from switch.ko: %s' % e.hex())
    ret, got = tm_run(e)
    show('entry as built', e, ret, got)


if __name__ == '__main__':
    main()
