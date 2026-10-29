#!/usr/bin/env python3
"""<Is the driver's sbrg_hash() the vendor's function, bit for bit?>

The driver hashes a 34-byte key (sip[16] + dip[16] + vlan u16le) in reverse
with CRC-16/CCITT, init 0, masked to ten bits.  This script takes the table
(and nothing else) from tm.ko: the 256 u16 words at .LANCHOR1+0x18 in
.rodata, i.e. the array sbrg_hash indexes.  It then runs the module's own
sbrg_hash() over random 60-byte buffers and compares with the port, so the
"the driver's copy is right" claim has a number behind it.

    python3 t_hash.py              # 200 random keys
    python3 t_hash.py 1000         # more

Only sbrg_hash runs; no register access is needed, so no hardware values are
stubbed beyond the log sink.
"""
import hashlib
import random
import struct
import sys

from elftools.elf.elffile import ELFFile

from km import Mod

KO = '/ko/tm.ko'
KEY_LEN = 34          # sbrg_hash(r0, 9, 0x10c, 0) hashes 34 bytes


def table():
    """The CRC table sbrg_hash reads at .LANCHOR1+0x18."""
    f = ELFFile(open(KO, 'rb'))
    for s in f.get_section_by_name('.symtab').iter_symbols():
        if s.name == '.LANCHOR1' and s['st_value'] == 0xf0:
            sec = f.get_section(s['st_shndx'])
            raw = sec.data()[0xf0 + 0x18:0xf0 + 0x18 + 512]
            return struct.unpack('<256H', raw)
    raise SystemExit('no .LANCHOR1 at 0xf0 in %s' % KO)


TAB = table()


def port(key, init=0):
    """The driver's algorithm: reverse, CRC-16/CCITT, keep ten bits."""
    crc = init
    for b in bytes(key[:KEY_LEN])[::-1]:
        crc = (TAB[(b ^ (crc >> 8)) & 0xff] ^ ((crc << 8) & 0xffff)) & 0xffff
    return crc & 0x3ff


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    print('crc table sha256 %s' % hashlib.sha256(
        struct.pack('<256H', *TAB)).hexdigest()[:16])
    m = Mod(KO, log=lambda _s: None)
    rnd = random.Random(20261029)
    bad = {0: 0, 0xffff: 0}
    for i in range(n):
        key = bytes(rnd.randrange(256) for _ in range(60))
        buf = m.calloc(len(key), 16)
        m.wr(buf, len(key), key)
        got = m.run('sbrg_hash', (buf, 9, 0x10c, 0))
        if got is None:
            print('key %d: emulator did not return' % i)
            return 1
        for init in bad:
            if port(key, init) != got:
                bad[init] += 1
    print('keys %d  mismatches: init 0 -> %d, init 0xffff -> %d'
          % (n, bad[0], bad[0xffff]))
    return 0 if bad[0] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
