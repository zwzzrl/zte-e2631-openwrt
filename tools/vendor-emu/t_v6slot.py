#!/usr/bin/env python3
"""<Does the re-implemented hash pick the same table slot as the vendor code?>

t_v6seq.py shows one sequence; this script closes the loop.  sbrg_add_ipv6table
does not hash the 52-byte record: it first packs a 34-byte key of
sip[16] + dip[16] + vlan[2], then sbrg_hash reverses those 34 bytes and runs
CRC-16/CCITT (init 0) over them, returning crc & 0x3ff; sbrg_get_table_sel
turns that into the block index.  That key packing was read out of the
emulator by hooking sbrg_hash's entry and dumping the buffer at its r0
(see README "第八轮").  This script reproduces key + hash + slot and compares
the prediction against the index the module's own code writes, over random
records.

    python3 t_v6slot.py             # 8 random records
    python3 t_v6slot.py <n>         # n random records

The CRC table is the 256 u16 words at .rodata:0x108 in tm.ko, i.e. the array
.LANCHOR1+0x18 that sbrg_hash indexes; it is reproduced here so the check
does not depend on the ELF file.
"""
import random
import sys

from km import Mod, REG0_12

KO = '/ko/tm.ko'
BOUNDARY = ('tmOnuRegWrite', 'tmOnuRegRead', 'printk', '__memzero', 'memset',
            'sbrg_get_multi_vlan_mode', 'sbrg_set_table_sel')
REGVAL = {20: 1, 25: 0, 29: 1}

# CRC-16/CCITT, polynomial 0x1021, non-reflected: table[i] built from i << 8.
def _gen():
    out = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
        out.append(crc)
    return tuple(out)

TAB = _gen()


def sbrg_key(sip, dip, vlan):
    """The 34 bytes sbrg_hash actually sees: sip, dip, then vlan as u16le."""
    return bytes(sip) + bytes(dip) + (vlan & 0xffff).to_bytes(2, 'little')


def sbrg_hash(key, ln=34):
    crc = 0
    for b in bytes(key[:ln])[::-1]:
        crc = (TAB[(b ^ (crc >> 8)) & 0xff] ^ ((crc << 8) & 0xffff)) & 0xffff
    return crc & 0x3ff


def slot(h, sel):
    if sel == 0:
        return h          # 12-bit index, 8 words per entry (vendor tables 1/2)
    if sel == 1:
        return ((h & 0x7f) << 2) | 0x200
    return ((h << 2) & 0xff) | 0x300


def rand_rec(rnd):
    vlan = rnd.randrange(1, 4095)
    dip = bytes([0xfd, rnd.randrange(256)]) + bytes(rnd.randrange(256) for _ in range(14))
    sip = bytes([0xfd, rnd.randrange(256)]) + bytes(rnd.randrange(256) for _ in range(14))
    rec = (b'\x01\x00\x00\x00' + b'\x00' * 4 + b'\x01\x00\x00\x00'
           + vlan.to_bytes(4, 'little') + dip + b'\xff\x00\x00\x00' + sip)
    return rec, vlan, dip, sip


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    rnd = random.Random(20261029)
    bad = 0
    for i in range(n):
        rec, vlan, dip, sip = rand_rec(rnd)
        seq = []
        sel = []

        msgs = []
        m = Mod(KO, intercept=BOUNDARY, log=msgs.append)

        def handler(name):
            def f(_n):
                r = [m.uc.reg_read(REG0_12[k]) for k in range(4)]
                if name == 'tmOnuRegWrite':
                    seq.append(('w', r[0], r[1] & 0xffffffff))
                elif name == 'tmOnuRegRead':
                    v = REGVAL.get(r[0], 0)
                    m.wr(r[1], 4, v.to_bytes(4, 'little'))
                    seq.append(('r', r[0], v))
                m.uc.reg_write(REG0_12[0], 0)
            return f

        for nm in BOUNDARY:
            m.handlers[nm] = handler(nm)
        entry, _ = m.func('sbrg_get_table_sel')
        def poke(mm, _a):
            sel.append([mm.uc.reg_read(REG0_12[k]) for k in range(4)])
        m.pokes[entry] = poke

        buf = m.calloc(len(rec), 16)
        m.wr(buf, len(rec), rec)
        ret = m.run('sbrg_add_ipv6table', (buf,), max_steps=2000000)
        writes = [v for (k, reg, v) in seq if k == 'w' and reg == 19]
        first = writes[0] if writes else None
        got = first & 0xfff if first is not None else None
        got_tbl = (first >> 22) & 0x1f if first is not None else None
        h = sbrg_hash(sbrg_key(sip, dip, vlan))
        s = slot(h, 1)
        ok = (got == s)
        bad += 0 if ok else 1
        print('rec %d vlan %-4d dip %s sip %s' % (i, vlan, dip[:6].hex(), sip[:6].hex()))
        print('   emu ret %r table %s index %s (%d cmds) | predicted h=%#05x slot=%#05x [%s]'
              % (ret, got_tbl, None if got is None else hex(got), len(writes), h, s,
                 'OK' if ok else 'MISMATCH'))
    print('mismatches: %d/%d' % (bad, n))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
