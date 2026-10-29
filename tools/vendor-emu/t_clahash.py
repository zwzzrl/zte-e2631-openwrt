#!/usr/bin/env python3
"""<Is the CLA hash we recovered really the vendor's cla_acl_hash_addr_gen?>

docs/vendor-cla-hash-recovered-2026-10-30.md claims:

    slot_hash = CRC32_msb_first(poly[type], 0x00 || reverse(key[0..43])) & 0xffff

This runs the vendor's own cla_acl_hash_addr_gen() under unicorn over random
44-byte keys for all four types and compares with that formula, re-deriving each
poly from the .data table itself rather than from the document.

    python3 t_clahash.py              # 200 random keys x 4 types
    python3 t_clahash.py 1000

Nothing is stubbed beyond the log sink: the function's own code decides the byte
order, the truncation and the table choice.
"""
import random
import struct
import sys

from elftools.elf.elffile import ELFFile

from km import Mod

KO = '/ko/tm.ko'
KEY_LEN = 44

# .data offsets of the four 1 KiB tables, and the type each one belongs to.
# Derivation: the R_ARM_MOVW/MOVT relocations at 0x6d74 (.LANCHOR0) and
# 0x6df8/0x6e2c (.LANCHOR2) plus the ldr offsets in each branch.
TABLES = {
    0: ('.LANCHOR0', 0x70, 0xa7c),
    1: ('.LANCHOR0', 0x70, 0xe7c),
    2: ('.LANCHOR2', 0x2068, -0xd7c),
    3: ('.LANCHOR2', 0x2068, -0x97c),
}


def sections():
    f = ELFFile(open(KO, 'rb'))
    syms = {}
    for s in f.get_section_by_name('.symtab').iter_symbols():
        if s.name in ('.LANCHOR0', '.LANCHOR2'):
            syms.setdefault((s.name, s['st_value']), f.get_section(s['st_shndx']))
    return syms


def table(typ):
    name, base, off = TABLES[typ]
    sec = sections()[(name, base)]
    start = base + off
    return struct.unpack('<256I', sec.data()[start:start + 1024])


TABS = {t: table(t) for t in TABLES}


def port(key, typ):
    """The recovered algorithm: leading zero, reversed key, MSB-first CRC32."""
    crc = 0
    for b in b'\x00' + bytes(key)[::-1]:
        crc = (TABS[typ][(crc >> 24) ^ b] ^ ((crc << 8) & 0xffffffff)) & 0xffffffff
    return crc & 0xffff


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    for typ in TABLES:
        print('type %d poly %#010x (table[1])' % (typ, TABS[typ][1]))
    m = Mod(KO, log=lambda _s: None)
    rnd = random.Random(20261030)
    bad = dict.fromkeys(TABLES, 0)
    for i in range(n):
        key = bytes(rnd.randrange(256) for _ in range(KEY_LEN))
        kbuf = m.calloc(KEY_LEN, 16)
        m.wr(kbuf, KEY_LEN, key)
        for typ in TABLES:
            obuf = m.calloc(4, 16)
            got = m.run('cla_acl_hash_addr_gen', (typ, kbuf, obuf))
            if got is None:
                print('key %d type %d: emulator did not return' % (i, typ))
                return 1
            want = int.from_bytes(m.rd(obuf, 4), 'little') & 0xffff
            if port(key, typ) != want:
                bad[typ] += 1
                if bad[typ] == 1:
                    print('MISMATCH key %d type %d: vendor %#06x port %#06x'
                          % (i, typ, want, port(key, typ)))
    print('keys %d: mismatches %s' % (n, bad))
    return 0 if not any(bad.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
