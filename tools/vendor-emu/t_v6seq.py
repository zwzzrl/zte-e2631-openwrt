#!/usr/bin/env python3
"""<What command sequence does the vendor's IPv6 table add really issue?>

t_v6tbl.py answers "which registers does it touch" by stubbing the wrappers;
this script does the opposite: it lets the module's own code run and only
intercepts the boundary (the ONU register accessors), so the *values* it
writes come out of the vendor's arithmetic instead of a stub.

Two register values are not invented - they were read on this board through
the driver's `xprobe` parameter (tools/vendor-emu README, "第五轮"):

    reg 20 (0x92388018) bit0 = 1   the access handshake is granted
    reg 25 (0x92388180) bits0..5 = 0   no table access is in flight
    reg 29 (0x92388184) mask 3     = 1   the multicast-table selector

so sbrg_access_timeout() passes on the first poll, exactly as it would on the
box, and the sequence that follows is the sequence the hardware would see.

    python3 t_v6seq.py            # one valid record, full sequence
    python3 t_v6seq.py <52-byte-record-as-hex>
"""
import struct
import sys

from km import Mod, REG0_12

KO = '/ko/tm.ko'

# Only the boundary is stubbed.  sbrg_hash, sbrg_get_table_sel,
# sbrg_set_indreg_cmd and both cfg wrappers run for real: the point of this
# script is the numbers they compute.
BOUNDARY = ('tmOnuRegWrite', 'tmOnuRegRead', 'printk', '__memzero', 'memset',
            'sbrg_get_multi_vlan_mode', 'sbrg_set_table_sel')

# Measured on the board, see the docstring.
REGVAL = {20: 1, 25: 0, 29: 1}   # 29 = 0x92388184, reads 1 on the box

# valid/age/mode, vlan 1, dip fd88::2, pt_mask 0xff, sip fd99::2
RECORD = bytes.fromhex(
    '01000000' '00000000' '01000000' '01000000'      # valid, age, mode, vlan
    'fd88' + '00' * 13 + '02' 'ff000000'             # dip fd88::2, pt_mask
    'fd99' + '00' * 13 + '02')                       # sip fd99::2


def decode_cmd(cmd):
    return 'rw=%d start=%d table=%d index=%#05x' % (
        (cmd >> 31) & 1, (cmd >> 27) & 1, (cmd >> 22) & 0x1f, cmd & 0xfff)


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    want_hash = '--hash' in sys.argv
    want_calls = '--calls' in sys.argv
    rec = bytes.fromhex(argv[0]) if argv else RECORD
    msgs = []
    m = Mod(KO, intercept=BOUNDARY, log=msgs.append)
    seq = []

    def handler(name):
        def f(_n):
            r = [m.uc.reg_read(REG0_12[i]) for i in range(4)]
            if name == 'tmOnuRegWrite':
                seq.append(('w', r[0], r[1] & 0xffffffff, None))
            elif name == 'tmOnuRegRead':
                val = REGVAL.get(r[0], 0)
                m.wr(r[1], 4, val.to_bytes(4, 'little'))
                seq.append(('r', r[0], val, None))
            elif name == 'printk':
                try:
                    fmt = m.rd(r[0], 200).split(b'\x00')[0].decode('latin1')
                except Exception:                       # noqa: BLE001
                    fmt = '?'
                seq.append(('p', 0, 0, fmt))
            m.uc.reg_write(REG0_12[0], 0)
        return f

    for n in BOUNDARY:
        m.handlers[n] = handler(n)

    seen = []
    if want_calls:
        # The real code runs; these hooks only report where it goes.  Without
        # this, "which function produced the slot number" stays a guess.
        for n in ('sbrg_hash', 'sbrg_get_table_sel', 'sbrg_set_indreg_cmd'):
            entry, _size = m.func(n)
            def poke(mm, _a, n=n):
                seen.append((n, [mm.uc.reg_read(REG0_12[i]) for i in range(4)]))
                if n == 'sbrg_hash' and len(mm.recent) >= 2:
                    # read r0 at the instruction the BL returns to: that is the
                    # slot number the caller then uses as the block index base
                    ret = mm.recent[-2] + 4
                    def after(m2, _b):
                        seen.append((n + ' -> r0', [m2.uc.reg_read(REG0_12[0])]))
                    mm.pokes[ret] = after
            m.pokes[entry] = poke

    buf = m.calloc(len(rec), 16)
    m.wr(buf, len(rec), rec)
    ret = m.run('sbrg_add_ipv6table', (buf,), max_steps=2000000)
    print('== ret %r steps %d record %s' % (ret, m.steps, rec.hex()))
    print('   sequence (reg numbers are the vendor\'s, phys = 0x92000000 + 4*base):')
    data = []
    for i, (kind, reg, val, txt) in enumerate(seq):
        if kind == 'p':
            print('   %3d printk %s' % (i, txt.strip()[:90]))
        elif kind == 'r':
            extra = ''
            if reg in (19,):
                extra = '  [' + decode_cmd(val) + ']'
            print('   %3d read  reg %-3d -> %#010x%s' % (i, reg, val, extra))
        else:
            extra = ''
            if reg == 19:
                extra = '  [' + decode_cmd(val) + ']'
            elif reg in (76, 77, 78):
                data.append(val)
                extra = '  [entry word %d]' % len(data)
            print('   %3d write reg %-3d <- %#010x%s' % (i, reg, val, extra))
    if data:
        print('   entry words, in write order:')
        for i in range(0, len(data), 3):
            print('     ' + ' '.join('%08x' % w for w in data[i:i + 3]))
    for line in msgs[:6]:
        print('   log %s' % line)

    if want_calls:
        print('   calls into the vendor helpers (r0..r3 as the caller passed them):')
        for n, a in seen:
            print('     %-22s %s' % (n, ' '.join('%#x' % (x & 0xffffffff) for x in a)))

    if want_hash:
        # Same call the caller makes (r0=buf, r1=9, r2=0x10c, r3=0).  Run the
        # module's own function and compare with the index the sequence used:
        # that is the answer to "where does the entry slot come from".
        h = m.run('sbrg_hash', (buf, 9, 0x10c, 0), max_steps=400000)
        idx = None
        for kind, reg, val, _ in seq:
            if kind == 'w' and reg == 19:
                idx = val & 0xfff
                break
        print('   sbrg_hash(buf, 9, 0x10c, 0) = %s ; first cmd index = 0x%03x'
              % ('?' if h is None else '0x%03x' % h, idx if idx is not None else 0))


if __name__ == '__main__':
    main()
