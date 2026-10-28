#!/usr/bin/env python3
"""What actually carries the vendor's IPv6 forwarding entry?

tm.ko has two unrelated IPv6 mechanisms:

  1. the ACL hash entry (60 bytes) built by tm_acl_fast_add_v4v6() - what our
     driver's zx_ppe_hash_entry() reproduces.  For a type-5 (IPv6 up) flow it
     fills only e19..e32, the same 13 bytes an IPv4 flow gets (t_v6win.py), and
     tm_acl_fast_add_v4v6 has no family branch at all (static disassembly).
  2. an IPv6 *table*: tm_ipv6table_add_set() -> sbrg_add_ipv6table(), exposed to
     userland through dev_attr_v6Table, with its own globals (g_ipv6FastEn,
     g_fast_entry_v6, g_v6_total_entry, g_IPv6_{Snat,Dnat}_Ram_Length,
     g_nptv6_{s,d}ipv6).  It read-modify-writes a **48-byte** entry through the
     switch's indirect-register interface: 4 rounds of sbrg_*_indreg_*_cfg(i)
     plus tmOnuRegWrite/Read on ONU registers 78/77/76, which the module's
     descriptor table maps to bases 0x000e2007/8/9, and a status bit at
     register 25 (base 0x000e2060, mask 1, shift 2).

This script decodes that descriptor table out of the ELF (no hardware, no
emulation) and can emulate sbrg_add_ipv6table() for a synthetic record.

    python3 t_v6tbl.py regs           # the ONU registers this path uses
    python3 t_v6tbl.py call           # emulate with a 64-byte tagged record
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

from km import Mod, REG0_12

KO = '/ko/tm.ko'
# movw/movt (R_ARM_MOVW_ABS_NC) inside sbrg_add_ipv6table that loads the
# register-descriptor table; the first one is enough
SITE = 0xf3f4
REGS = (25, 76, 77, 78)
# 28-byte descriptor: u16 id, u16 pad, u32 kind, u32 base, u32 mask,
# u16 shift, u16 pad, u32 stride, u32 counter
DESC = '<HHIIIHHII'


def table():
    """The descriptor array the function uses, read from the ELF relocations."""
    f = ELFFile(open(KO, 'rb'))
    syms = list(f.get_section_by_name('.symtab').iter_symbols())
    for r in f.get_section_by_name('.rel.text').iter_relocations():
        if r['r_offset'] == SITE:
            sym = syms[r['r_info_sym']]
            sec = f.get_section(sym['st_shndx'])
            print('reloc %#x -> %s in %s at %#x'
                  % (SITE, sym.name, sec.name, sym['st_value']))
            return sec.data()[sym['st_value']:]
    raise SystemExit('no relocation at %#x' % SITE)


SWITCH_PHYS = 0x92000000     # what zx_ponreg.ko's init stores: virt 0xF4000000


def regs():
    d = table()
    print('phys = %#x + 4 * (descriptor base + stride * index)'
          % SWITCH_PHYS)
    for reg in REGS:
        w = struct.unpack(DESC, d[28 * reg:28 * reg + 28])
        print('reg %-3d base %#010x mask %#010x shift %-2d stride %#x '
              'kind %d  -> phys %#010x'
              % (w[0], w[3], w[4], w[5], w[7], w[2],
                 SWITCH_PHYS + 4 * (w[3] + w[7] * reg)))


def emu(rec_hex=None, size=64, steps=2000000):
    msgs = []
    m = Mod(KO, intercept=('tmOnuRegWrite', 'tmOnuRegRead', 'printk',
                           '__memzero', 'memset', 'sbrg_set_indreg_wr_cfg',
                           'sbrg_get_indreg_wr_cfg', 'sbrg_set_indreg_cmd',
                           'sbrg_hash', 'sbrg_get_table_sel'),
            log=msgs.append)
    rec = []

    def h(name, nargs):
        def f(_n):
            r = [m.uc.reg_read(REG0_12[i]) for i in range(nargs)]
            rec.append((name,) + tuple(r))
            m.uc.reg_write(REG0_12[0], 0)
        return f

    for n, a in (('tmOnuRegWrite', 4), ('tmOnuRegRead', 4),
                 ('sbrg_set_indreg_wr_cfg', 4), ('sbrg_get_indreg_wr_cfg', 4),
                 ('sbrg_set_indreg_cmd', 4), ('sbrg_hash', 4),
                 ('sbrg_get_table_sel', 4), ('__memzero', 2), ('printk', 2)):
        m.handlers[n] = h(n, a)
    buf = m.calloc(size)
    raw = bytes.fromhex(rec_hex) if rec_hex else bytes(range(size))
    m.wr(buf, size, raw + b'\x00' * (size - len(raw)))
    ret = m.run('sbrg_add_ipv6table', (buf,), max_steps=steps)
    print('== ret %r steps %d calls %d' % (ret, m.steps, len(rec)))
    for line in rec:
        print('   ' + ' '.join(str(x) if isinstance(x, str) else '%#x' % x
                               for x in line))
    for l in msgs[:6]:
        print('   log %s' % l)


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'regs'
    if cmd == 'regs':
        regs()
    elif cmd == 'call':
        emu(sys.argv[2] if len(sys.argv) > 2 else None)
