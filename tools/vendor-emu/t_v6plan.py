#!/usr/bin/env python3
"""<What will the driver compute and write for this IPv6 flow?>

The driver's add path is a port of tm.ko's sbrg_add_ipv6table(): pack
sip[16] + dip[16] + vlan[u16le] into a 34-byte key, hash it backwards with
CRC-16/CCITT (init 0), keep 10 bits, pick the block from vendor reg 29, build
the 48-byte entry and write it as four blocks of three words to table 0.  The
key packing, the hash and the slot formula were all checked against the
vendor module in the emulator (t_v6slot.py), and the entry assembler in
t_v6entry.py matches it on random records.  This script prints the same
numbers the driver logs, so a hardware run can be compared line by line.

    python3 t_v6plan.py fd99::2 fd88::2 1 0xff
    python3 t_v6plan.py fd99::2 fd88::2 1          # pt_mask defaults to 0xff
"""
import sys

from t_v6entry import build
from t_v6slot import sbrg_hash, sbrg_key, slot


def parse_addr(text):
    """Parse an IPv6 address into 16 bytes (no compression corner cases)."""
    import ipaddress
    return ipaddress.IPv6Address(text).packed


def plan(sip, dip, vlan, pt=0xff):
    key = sbrg_key(sip, dip, vlan)
    h = sbrg_hash(key)
    rec = (b'\x01\x00\x00\x00' + b'\x00' * 4 + b'\x01\x00\x00\x00'
           + vlan.to_bytes(4, 'little') + dip + pt.to_bytes(4, 'little') + sip)
    return h, slot(h, 1), build(rec)


def main():
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    sip = parse_addr(sys.argv[1])
    dip = parse_addr(sys.argv[2])
    vlan = int(sys.argv[3], 0)
    pt = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0xff
    h, idx, words = plan(sip, dip, vlan, pt)
    print('sip %s dip %s vlan %d pt %#04x' % (sip.hex(), dip.hex(), vlan, pt))
    print('hash %#05x  slot %#05x  (blocks %#05x..%#05x)' % (h, idx, idx, idx + 3))
    for i in range(0, 12, 3):
        print('  w%02d %08x %08x %08x' % (i, words[i], words[i + 1], words[i + 2]))
    print('driver: echo "h %s %s %d %#x" > /sys/module/zx279128s_ppe/parameters/sbrg'
          % (sip.hex(), dip.hex(), vlan, pt))
    return 0


if __name__ == '__main__':
    sys.exit(main())
