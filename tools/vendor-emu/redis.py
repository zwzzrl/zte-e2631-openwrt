#!/usr/bin/env python3
"""Disassemble a range of an ARM module, with relocation notes.

    python3 redis.py <ko> <start-hex> <end-hex>

(The file cannot be called dis.py - that shadows the stdlib module of the same
name and pyelftools' own import of `inspect` then fails.)
"""
import sys

from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

KO = sys.argv[1]
LO = int(sys.argv[2], 16)
HI = int(sys.argv[3], 16)
f = ELFFile(open(KO, 'rb'))
st = f.get_section_by_name('.symtab')
syms = {s.name: s for s in st.iter_symbols() if s.name}
rels = {}
for sec in f.iter_sections():
    if sec.header.sh_type in ('SHT_REL', 'SHT_RELA'):
        for r in sec.iter_relocations():
            sn = st.get_symbol(r['r_info_sym']).name
            if sn:
                rels[(sec['sh_info'], r['r_offset'])] = (sn, r['r_info_type'])
sec = f.get_section_by_name('.text')
code = sec.data()
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for i in md.disasm(code[LO:HI], LO):
    note = ''
    r = rels.get((1, i.address))
    if r:
        note = '   ; %s (t%d)' % (r[0], r[1])
    print('%05x %-8s %-28s%s' % (i.address, i.mnemonic, i.op_str, note))
