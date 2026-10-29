#!/usr/bin/env python3
"""Disassemble a range of a .ko's .text, with relocation targets in comments.

    python3 disr.py /ko/tm.ko 0x5d640 0x5d720
"""
import sys

from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

KO = sys.argv[1]
LO, HI = int(sys.argv[2], 16), int(sys.argv[3], 16)
SEC = sys.argv[4] if len(sys.argv) > 4 else '.text'
f = ELFFile(open(KO, 'rb'))
st = f.get_section_by_name('.symtab')
rels = {}
for sec in f.iter_sections():
    if sec.header.sh_type in ('SHT_REL', 'SHT_RELA'):
        for r in sec.iter_relocations():
            sn = st.get_symbol(r['r_info_sym']).name
            if sn:
                rels[(sec['sh_info'], r['r_offset'])] = sn
sec = f.get_section_by_name(SEC)
idx = [i for i in range(f.num_sections()) if f.get_section(i).name == SEC][0]
code = sec.data()
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for i in md.disasm(code[LO:HI], LO):
    note = rels.get((idx, i.address))
    print('%05x %-8s %-30s%s' % (i.address, i.mnemonic, i.op_str,
                                 ('   ; ' + note) if note else ''))
