#!/usr/bin/env python3
"""The 48-byte entry the vendor's IPv6 table add builds.

sbrg_add_ipv6table() takes the 52-byte user record, reads the four tables
(table 0..3, chunk 0x248..0x24b, three words each) and writes one entry back
(4 rounds x 3 words, ONU registers 78/77/76 = words 3i+2, 3i+1, 3i).  The
read-back turns out not to influence the entry at all: words 8 and 11 are
stored as a hard zero (`mov r2, #0` then two `str`).

Record layout (52 bytes), from the function's own debug prints plus a
one-byte-at-a-time sweep:

    0x00 u32 valid (0/1)      0x04 u32 age (0/1)     0x08 u32 mode (0/1)
    0x0c u32 vlan_id (<0x1000)  0x10 dip[16] (network order)
    0x20 u32 pt_mask (<=0xff, the three high bytes must be zero)
    0x24 sip[16] (network order)

Entry layout (12 words, verified against the emulator on 60 random records by
build()): the entry is a 384-bit little-endian bit stream, bit 0 first:

    0..2      valid, age, mode          3..14     vlan_id
    15..78    dip bytes 8..15, LE u64   96..103   pt_mask
    104..167  dip bytes 0..7, LE u64    192..255  sip bytes 8..15
    288..351  sip bytes 0..7            everything else 0 (w2[31:15],
    w5[31:8], w8 and w11 are hard zero)

    python3 t_v6entry.py run            # one zero record: what the code builds
    python3 t_v6entry.py layout 40      # assembler vs emulator, random records
    python3 t_v6entry.py proto          # register/cmd sequence of one add
    python3 t_v6entry.py prints         # field names from the debug prints
    python3 t_v6entry.py readmap        # is the read-back entry used at all?
    python3 t_v6entry.py raw            # run the real cfg helpers (pokes reg 20)
    python3 t_v6entry.py sweep          # poke each record byte, diff the entry
"""
import sys

from km import Mod, REG0_12

KO = '/ko/tm.ko'
STUBS = ('tmOnuRegWrite', 'tmOnuRegRead', 'printk', '__memzero', 'memset',
         'sbrg_set_indreg_wr_cfg', 'sbrg_get_indreg_wr_cfg',
         'sbrg_set_indreg_cmd', 'sbrg_hash', 'sbrg_get_table_sel',
         'sbrg_get_multi_vlan_mode', 'sbrg_set_table_sel')
TABLE_SEL = 1        # 0 and 3 are rejected by the function itself
REG20 = None         # value the "table access done" bit (reg 20) returns
HASH = 0x12          # an arbitrary slot number


def addr(m, name):
    i = m.sym_index(name)
    if i is None:
        raise KeyError(name)
    s = m.symtab[i]
    return m.sec_base[s['st_shndx']] + s['st_value']


def run(record, size=64, steps=2000000, readback=None, debug=0, prlog=None,
        real_cfg=False, regval=None):
    """Run one add and return (12 entry words, register-write log).

    readback: 12 words the "old entry" reads return, in read order; None
    means zeros (a fresh table).
    """
    msgs = []
    iv = tuple(n for n in STUBS
               if not (real_cfg and n in ('sbrg_set_indreg_wr_cfg',
                                          'sbrg_get_indreg_wr_cfg')))
    m = Mod(KO, intercept=iv, log=msgs.append)
    writes = []
    reads = []
    calls = []

    def out_params(name):
        def f(_n):
            r = [m.uc.reg_read(REG0_12[i]) for i in range(4)]
            calls.append((name, r))
            if name == 'sbrg_get_table_sel':
                m.wr(r[0], 4, TABLE_SEL.to_bytes(4, 'little'))
                m.uc.reg_write(REG0_12[0], 0)
            elif name == 'sbrg_get_multi_vlan_mode':
                m.wr(r[0], 4, b'\x00\x00\x00\x00')
                m.uc.reg_write(REG0_12[0], 0)
            elif name == 'sbrg_hash':
                m.uc.reg_write(REG0_12[0], HASH)
            elif name == 'tmOnuRegRead':
                i = len(reads)
                if regval and r[0] in regval:
                    val = regval[r[0]]
                else:
                    val = readback[i] if readback and i < len(readback) else 0
                reads.append((r[0], val))
                m.wr(r[1], 4, val.to_bytes(4, 'little'))
                m.uc.reg_write(REG0_12[0], val & 0xffffffff)
            elif name == 'printk':
                try:
                    fmt = m.rd(r[0], 160).split(b'\x00')[0].decode('latin1')
                except Exception:
                    fmt = '?'
                if prlog is not None:
                    prlog.append((fmt, r[1], r[2], r[3]))
                m.uc.reg_write(REG0_12[0], 0)
            elif name == 'tmOnuRegWrite':
                writes.append((r[0], r[1] & 0xffffffff))
                m.uc.reg_write(REG0_12[0], 0)
            else:
                m.uc.reg_write(REG0_12[0], 0)
        return f

    for n in STUBS:
        if real_cfg and n in ('sbrg_set_indreg_wr_cfg', 'sbrg_get_indreg_wr_cfg'):
            continue
        m.handlers[n] = out_params(n)
    if debug:
        m.wr(addr(m, 'g_tm_debug_level'), 4, debug.to_bytes(4, 'little'))
    buf = m.calloc(size)
    m.wr(buf, size, bytes(record) + b'\x00' * (size - len(record)))
    ret = m.run('sbrg_add_ipv6table', (buf,), max_steps=steps)
    words = [0] * 12
    order = {78: 2, 77: 1, 76: 0}
    for reg, val in writes:
        if reg in order:
            words.append(val)
    # the writes come as 4 rounds of (78,77,76); rebuild in entry order
    entry = [None] * 12
    i = 0
    for reg, val in writes:
        if reg not in order:
            continue
        entry[3 * (i // 3) + order[reg]] = val
        i += 1
    return dict(ret=ret, words=entry, writes=writes, steps=m.steps,
                reads=reads, calls=calls, log=msgs[:4])


def show(tag, r):
    print('== %s ret %r steps %d writes %d'
          % (tag, r['ret'], r['steps'], len(r['writes'])))
    print('   entry %s' % ' '.join('%08x' % w if w is not None else '--------'
                                   for w in r['words']))
    print('   writes %s' % ' '.join('%d:%08x' % (reg, val)
                                    for reg, val in r['writes']))


def sweep(lo, hi):
    base = run(bytes(64))['words']
    print('baseline %s' % ' '.join('%08x' % w if w is not None else '--------'
                                   for w in base))
    for o in range(lo, hi):
        rec = bytearray(64)
        rec[o] = 0xa5
        w = run(bytes(rec))['words']
        diff = ['w%d=%08x' % (i, v) for i, v in enumerate(w)
                if v is not None and base[i] != v]
        if diff:
            print('rec[%#04x] = 0xa5 -> %s' % (o, ' '.join(diff)))


def prints():
    """<Turn the debug prints on; each arg reveals the field's record offset.>"""
    out = []
    rec = bytes(range(64))
    r = run(rec, debug=7, prlog=out)
    print('== ret %r steps %d prints %d' % (r['ret'], r['steps'], len(out)))
    print('   entry %s' % ' '.join('%08x' % w if w is not None else '--------'
                                   for w in r['words']))
    for fmt, a, b, c in out:
        t = fmt.strip()
        if any(k in t for k in ('input', 'vlan', 'sip', 'dip', 'valid', 'mode',
                                'age', 'mask', 'table', 'hash')):
            print('   %-56s %#x %#x %#x' % (t[:56], a, b, c))


def proto():
    """<Print the exact register/cmd sequence one add performs.>"""
    rec = bytearray(64)
    rec[0] = 1                                  # valid
    rec[4] = 0                                  # age
    rec[8] = 1                                  # mode
    rec[12] = 0x34                              # vlan
    rec[0x10:0x20] = bytes(range(0xa0, 0xb0))    # dip
    rec[0x20] = 0x7                             # pt_mask
    rec[0x24:0x34] = bytes(range(0xc0, 0xd0))    # sip
    r = run(bytes(rec))
    print('== ret %r steps %d' % (r['ret'], r['steps']))
    print('   entry %s' % ' '.join('%08x' % w if w is not None else '----'
                                   for w in r['words']))
    print('   sequence:')
    ri = 0
    for i, (name, a) in enumerate(r['calls']):
        if name == 'tmOnuRegRead':
            reg, val = r['reads'][ri]
            ri += 1
            print('   %3d %-22s reg %-3d -> %#010x' % (i, name, reg, val & 0xffffffff))
        elif name == 'tmOnuRegWrite':
            print('   %3d %-22s reg %-3d <- %#010x' % (i, name, a[0], a[1] & 0xffffffff))
        else:
            print('   %3d %-22s %s' % (i, name, ' '.join('%#x' % (x & 0xffffffff) for x in a[:4])))


def build(rec):
    """<Assemble the 12 words from the 52-byte record, per the measured map.>"""
    def le(off, n):
        return int.from_bytes(bytes(rec[off:off + n]), 'little')
    w = [0] * 12
    w[0] = (rec[0] & 1) | ((rec[4] & 1) << 1) | ((rec[8] & 1) << 2) \
        | ((le(12, 4) & 0xfff) << 3) | ((le(0x18, 8) & 0x1ffff) << 15)
    w[1] = (le(0x18, 8) >> 17) & 0xffffffff
    w[2] = (le(0x18, 8) >> 49) & 0x7fff
    w[3] = (le(0x20, 4) & 0xff) | ((le(0x10, 8) & 0xffffff) << 8)
    w[4] = (le(0x10, 8) >> 24) & 0xffffffff
    w[5] = (le(0x10, 8) >> 56) & 0xff
    v = le(0x24, 16)
    w[9] = v & 0xffffffff
    w[10] = (v >> 32) & 0xffffffff
    w[6] = (v >> 64) & 0xffffffff
    w[7] = (v >> 96) & 0xffffffff
    return w


def layout(n=40):
    """<Check the assembler against the emulator on random records.>"""
    import random
    rnd = random.Random(20261029)
    bad = 0
    for i in range(n):
        rec = bytearray(rnd.randrange(256) for _ in range(64))
        rec[0] = rnd.randrange(2)
        rec[1:4] = b'\x00' * 3
        rec[4] = rnd.randrange(2)
        rec[5:8] = b'\x00' * 3
        rec[8] = rnd.randrange(2)
        rec[9:12] = b'\x00' * 3
        rec[12:16] = bytes([rnd.randrange(256), rnd.randrange(0x10), 0, 0])
        rec[0x20:0x24] = bytes([rnd.randrange(0x80), 0, 0, 0])
        want = build(rec)
        got = run(bytes(rec))['words']
        if want != got:
            bad += 1
            print('record %d MISMATCH' % i)
            print('   want %s' % ' '.join('%08x' % w for w in want))
            print('   got  %s' % ' '.join('%08x' % w if w is not None else '----'
                                          for w in got))
            if bad > 3:
                break
    print('%d/%d records match' % (n - bad, n))
    if bad:
        return
    rec = bytearray(64)
    print('layout of the 12 words (G = 32*w + bit):')
    print('  w0   bit0 valid, bit1 age, bit2 mode, bits3..14 vlan_id, '
          'bits15..31 dip[64..80]')
    print('  w1..w2 dip[81..127] (w2 bits15..31 = 0)')
    print('  w3   bits0..7 pt_mask, bits8..31 dip[0..23]')
    print('  w4..w5 dip[24..63] (w5 bits8..31 = 0)')
    print('  w6   sip[64..95]      w7 sip[96..127]')
    print('  w8   0')
    print('  w9   sip[0..31]       w10 sip[32..63]')
    print('  w11  0')
    print('  nibble: the address halves past byte 8 come first (they sit at w0/w3),')
    print('  the first 8 bytes come last (w6/w9), both as little-endian u64 halves.')


def raw():
    """<Run with the real cfg helpers, to see the registers they touch.>"""
    rec = bytearray(64)
    rec[0] = 1
    rec[8] = 1
    rec[12] = 0x34
    rec[0x10:0x20] = bytes(range(0xa0, 0xb0))
    rec[0x20] = 0x7
    rec[0x24:0x34] = bytes(range(0xc0, 0xd0))
    rv = {20: REG20} if REG20 is not None else None
    pr = []
    r = run(bytes(rec), real_cfg=True, regval=rv, prlog=pr)
    print('== ret %r steps %d' % (r['ret'], r['steps']))
    print('   entry %s' % ' '.join('%08x' % w if w is not None else '----'
                                   for w in r['words']))
    ri = 0
    for i, (name, a) in enumerate(r['calls']):
        if name == 'tmOnuRegRead':
            reg, val = r['reads'][ri]
            ri += 1
            print('   %3d read  reg %-3d -> %#010x' % (i, reg, val & 0xffffffff))
        elif name == 'tmOnuRegWrite':
            print('   %3d write reg %-3d <- %#010x' % (i, a[0], a[1] & 0xffffffff))
        else:
            print('   %3d %-24s %s' % (i, name, ' '.join('%#x' % (x & 0xffffffff)
                                                         for x in a[:4])))
    print('   prints:')
    for fmt, a, b, c in pr:
        print('     %-52s %#x %#x %#x' % (fmt.strip()[:52], a, b, c))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'run'
    if cmd == 'sweep':
        lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
        hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
        sweep(lo, hi)
    elif cmd == 'readmap':
        rb = [0x100000 + i for i in range(64)]
        r = run(bytes(64), readback=rb)
        print('ret %r steps %d reads %d' % (r['ret'], r['steps'], len(r['reads'])))
        print('entry %s' % ' '.join('%08x' % w if w is not None else '----'
                                    for w in r['words']))
        print('read slots: %s' % ' '.join('%d:%06x' % (i, v) for i, (reg, v)
                                          in enumerate(r['reads'])))
    elif cmd == 'raw20':
        REG20 = int(sys.argv[2], 0)
        raw()
    elif cmd == 'raw':
        raw()
    elif cmd == 'layout':
        layout(int(sys.argv[2], 0) if len(sys.argv) > 2 else 40)
    elif cmd == 'proto':
        proto()
    elif cmd == 'prints':
        prints()
    elif cmd == 'readback':
        rb = [0x10000000 + i for i in range(12)]
        r = run(bytes(64), readback=rb)
        show('zero record, old entry = 0x10000000+i', r)
    else:
        show('zero record', run(bytes(64)))
