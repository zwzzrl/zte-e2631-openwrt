#!/usr/bin/env python3
"""Load an ARM32 .ko into unicorn: sections laid out at our own addresses,
relocations applied, undefined (or intercepted) symbols pointed at stubs.

A stub is six bytes of real ARM code -- ``mov r0, r0 ; bx lr`` -- preceded by
nothing else, so a code hook fires at the stub's first instruction, the handler
does whatever the symbol is supposed to do and leaves the return value in r0,
and unicorn then executes the two instructions and returns to the caller.  No
PC poking, which unicorn does not always honour.
"""
import struct
from io import BytesIO

from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UcError)
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                               UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

REG0_12 = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
           UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
           UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
           UC_ARM_REG_R12]

SHF_ALLOC = 0x2
SHT_NOBITS = 8
SHT_REL = 9

R_ARM_NONE, R_ARM_PC24, R_ARM_ABS32, R_ARM_REL32 = 0, 1, 2, 3
R_ARM_PLT32, R_ARM_CALL, R_ARM_JUMP24 = 27, 28, 29
R_ARM_V4BX, R_ARM_TARGET1, R_ARM_PREL31 = 40, 38, 42
R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS = 43, 44
R_ARM_MOVW_PREL_NC, R_ARM_MOVT_PREL = 45, 46
R_ARM_TARGET2, R_ARM_ABS32_NOI = 41, 55

RET_ADDR = 0x0ffffff0        # even, so bx lr cannot switch to Thumb

STUB_CODE = struct.pack('<II', 0xe1a00000, 0xe12fff1e)   # mov r0,r0 ; bx lr


def _sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def movw_patch(insn, imm16):
    return (insn & 0xfff0f000) | (((imm16 >> 12) & 0xf) << 16) | (imm16 & 0xfff)


def movw_get(insn):
    return (((insn >> 16) & 0xf) << 12) | (insn & 0xfff)


class Mod:
    def __init__(self, path, base=0x10000000, stub_base=None,
                 intercept=(), data_base=0x30000000, data_size=0x400000,
                 stack_size=0x400000, log=print):
        self.log = log
        self.d = open(path, 'rb').read()
        self.elf = ELFFile(BytesIO(self.d))
        self.base = base
        self.stub_base = stub_base
        self.data_base = data_base
        self.data_size = data_size
        self.stack_size = stack_size
        self.sec_base = {}
        self.sec_size = {}
        self._layout()
        if stub_base is None:
            # keep stubs inside the +-32 MB reach of an ARM branch
            stub_base = (base + self.modsize + 0xfff) & ~0xfff
        self.stub_base = stub_base
        self._syms()
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map(base, self.modsize, UC_PROT_ALL)
        self._copy_sections()
        self.uc.mem_map(self.stub_base, 0x10000, UC_PROT_ALL)
        self.uc.mem_write(self.stub_base, b'\x00' * 0x10000)
        self.nstub = 0
        self.stub_by_name = {}
        self.stub_by_addr = {}
        self.intercept = set(intercept)
        self.handlers = {}
        self._relocate()
        self.uc.mem_map(data_base, data_size, UC_PROT_ALL)
        self.uc.mem_write(data_base, b'\x00' * data_size)
        self.stack_top = data_base + data_size
        self.uc.mem_map(0x40000000, stack_size, UC_PROT_ALL)
        self.uc.mem_write(0x40000000, b'\x00' * stack_size)
        self.stack_base = 0x40000000
        self.heap = 0x50000000
        self.heap_end = self.heap
        self.heap_max = 0x5f000000
        self.uc.mem_map(self.heap, 0x1000000, UC_PROT_ALL)
        self.calls = []
        self.pokes = {}
        self.recent = []
        self.steps = 0
        self.max_steps = 4000000
        self.badpc = 0
        self.unmapped = {}
        self.uc.hook_add(UC_HOOK_CODE, self._hook_code)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._hook_unmapped)

    # ---------- layout ----------
    def _layout(self):
        cur = self.base
        for i, s in enumerate(self.elf.iter_sections()):
            if not (s['sh_flags'] & SHF_ALLOC):
                continue
            size = s['sh_size']
            if size == 0:
                self.sec_base[i] = cur
                self.sec_size[i] = 0
                continue
            cur = (cur + 15) & ~15
            self.sec_base[i] = cur
            self.sec_size[i] = size
            cur = (cur + size + 15) & ~15
        self.modsize = ((cur - self.base) + 0xfff) & ~0xfff

    def _syms(self):
        st = self.elf.get_section_by_name('.symtab')
        self.symtab = list(st.iter_symbols())

    def sym_index(self, name):
        for i, s in enumerate(self.symtab):
            if s.name == name:
                return i
        return None

    def _copy_sections(self):
        for i, s in enumerate(self.elf.iter_sections()):
            if i not in self.sec_base or s['sh_size'] == 0:
                continue
            sb = self.sec_base[i]
            if s['sh_type'] == SHT_NOBITS:
                self.uc.mem_write(sb, b'\x00' * s['sh_size'])
            else:
                off, size = s['sh_offset'], s['sh_size']
                data = self.d[off:off + size]
                if len(data) < size:
                    data += b'\x00' * (size - len(data))
                self.uc.mem_write(sb, data)

    # ---------- symbols ----------
    def stub(self, name):
        if name in self.stub_by_name:
            return self.stub_by_name[name]
        addr = self.stub_base + 16 * self.nstub
        self.nstub += 1
        self.uc.mem_write(addr, STUB_CODE)
        self.stub_by_name[name] = addr
        self.stub_by_addr[addr] = name
        return addr

    def sym_addr(self, sym):
        shndx = sym['st_shndx']
        if isinstance(shndx, str) or shndx == 'SHN_UNDEF':
            return self.stub(sym.name)
        if shndx == 'SHN_ABS':
            return sym['st_value']
        if sym.name in self.intercept:
            return self.stub(sym.name)
        return self.sec_base[shndx] + sym['st_value']

    def func(self, name):
        i = self.sym_index(name)
        if i is None:
            raise KeyError(name)
        s = self.symtab[i]
        return self.sec_base[s['st_shndx']] + s['st_value'], s['st_size']

    # ---------- relocations ----------
    def _relocate(self):
        self.relcount = {}
        for sec in self.elf.iter_sections():
            # pyelftools reports known section types as names, not numbers
            if not str(sec['sh_type']).endswith('SHT_REL'):
                continue
            tgt = sec['sh_info']
            if tgt not in self.sec_base:
                continue
            for rel in sec.iter_relocations():
                self._do_reloc(rel, self.sec_base[tgt], sec.name)

    def _do_reloc(self, rel, tgt_base, secname):
        P = tgt_base + rel['r_offset']
        sym = self.symtab[rel['r_info_sym']]
        typ = rel['r_info_type']
        self.relcount[typ] = self.relcount.get(typ, 0) + 1
        S = self.sym_addr(sym)
        if typ in (R_ARM_CALL, R_ARM_JUMP24, R_ARM_PC24, R_ARM_PLT32):
            insn = struct.unpack('<I', self.uc.mem_read(P, 4))[0]
            A = _sx((insn & 0xffffff) << 2, 26)
            if (insn >> 28) == 0xf:
                raise RuntimeError('BLX to %s at %#x' % (sym.name, P))
            tgt = (S + A) & 0xffffffff
            # the ABI absorbs the pipeline's +8 into the addend
            rel_off = (tgt - P) >> 2
            if not -0x800000 <= rel_off < 0x800000:
                raise RuntimeError('branch out of range to %s at %#x' % (sym.name, P))
            insn = (insn & 0xff000000) | (rel_off & 0xffffff)
            self.uc.mem_write(P, struct.pack('<I', insn))
            self._check_target(sym.name, tgt, P)
        elif typ in (R_ARM_ABS32, R_ARM_TARGET1, R_ARM_TARGET2, R_ARM_ABS32_NOI,
                     R_ARM_PREL31):
            A = struct.unpack('<I', self.uc.mem_read(P, 4))[0]
            if typ == R_ARM_PREL31:
                val = (S + (A & 0x7fffffff) - P) & 0x7fffffff
            else:
                val = S + A
            self.uc.mem_write(P, struct.pack('<I', val & 0xffffffff))
            self._check_target(sym.name, val & 0xffffffff, P)
        elif typ == R_ARM_REL32:
            A = struct.unpack('<I', self.uc.mem_read(P, 4))[0]
            self.uc.mem_write(P, struct.pack('<I', (S + A - P) & 0xffffffff))
        elif typ in (R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS, R_ARM_MOVW_PREL_NC,
                     R_ARM_MOVT_PREL):
            insn = struct.unpack('<I', self.uc.mem_read(P, 4))[0]
            A = movw_get(insn)
            if typ in (R_ARM_MOVW_PREL_NC, R_ARM_MOVT_PREL):
                val = S + A - P
            else:
                val = S + A
            imm = (val >> 16) & 0xffff if typ in (R_ARM_MOVT_ABS,
                                                  R_ARM_MOVT_PREL) else val & 0xffff
            self.uc.mem_write(P, struct.pack('<I', movw_patch(insn, imm)))
        elif typ == R_ARM_V4BX:
            pass
        elif typ == R_ARM_NONE:
            pass
        else:
            self.badpc += 1
            self.log('   reloc type %d not handled (%s at %#x, sym %s)'
                     % (typ, secname, P, sym.name))

    def _check_target(self, name, tgt, P):
        if self.base <= tgt < self.base + self.modsize:
            return
        if self.stub_base <= tgt < self.stub_base + 0x10000:
            return

    # ---------- memory helpers ----------
    def rd(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def wr(self, addr, n, val):
        if isinstance(val, int):
            val = val.to_bytes(n, 'little')
        self.uc.mem_write(addr, val)

    def calloc(self, n, align=16):
        a = (self.heap_end + align - 1) & ~(align - 1)
        self.heap_end = a + ((n + 63) & ~63)
        if self.heap_end >= self.heap_max:
            raise RuntimeError('emu heap exhausted')
        return a

    def cstr(self, s):
        a = self.calloc(len(s) + 1, 4)
        self.wr(a, len(s) + 1, s.encode() + b'\x00')
        return a

    # ---------- hooks ----------
    def _hook_code(self, uc, addr, size, _):
        self.steps += 1
        self.recent.append(addr)
        if len(self.recent) > 12:
            self.recent.pop(0)
        if self.steps > self.max_steps:
            self.log('   !! step limit')
            uc.emu_stop()
            self.badpc = -1
            return
        hook = self.pokes.get(addr)
        if hook is not None:
            hook(self, addr)
        name = self.stub_by_addr.get(addr)
        if name is not None:
            h = self.handlers.get(name, self.default_stub)
            self.calls.append(name)
            try:
                h(name)
            except Exception as exc:                    # noqa: BLE001
                self.log('   !! handler %s: %s' % (name, exc))
                uc.emu_stop()
            return

    def _hook_unmapped(self, uc, access, addr, size, value, _):
        page = addr & ~0xfff
        if addr in self.unmapped:
            return False
        self.unmapped[addr] = access
        try:
            uc.mem_map(page, 0x10000, UC_PROT_ALL)
            uc.mem_write(page, b'\x00' * 0x10000)
        except UcError:
            return False
        if len(self.unmapped) < 12:
            self.log('   .. zero page mapped at %#x (access %d)' % (page, access))
        return True

    def default_stub(self, name):
        if not name.startswith('..'):
            self.log('   ?? unstubbed call: %s' % name)
        self.uc.reg_write(UC_ARM_REG_R0, 0)

    # ---------- run ----------
    def run(self, name, args=(), max_steps=None, sp=None):
        entry, _ = self.func(name)
        return self.run_at(entry, args, max_steps, sp, name)

    def run_at(self, entry, args=(), max_steps=None, sp=None, label='?'):
        uc = self.uc
        for i, a in enumerate(args):
            uc.reg_write(REG0_12[i], a)
        sp = sp if sp is not None else (self.stack_base + self.stack_size - 0x1000)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET_ADDR)
        self.steps = 0
        self.badpc = 0
        if max_steps:
            self.max_steps = max_steps
        try:
            uc.emu_start(entry, RET_ADDR)
        except UcError as exc:
            self.log('   !! unicorn stopped in %s: %s (pc %#x)'
                     % (label, exc, uc.reg_read(UC_ARM_REG_PC)))
            self.log('   .. last pcs %s' % ' '.join('%#x' % a for a in self.recent))
            return None
        if self.badpc == -1:
            self.log('   !! %s hit the step limit' % label)
            return None
        pc = uc.reg_read(UC_ARM_REG_PC)
        if pc != RET_ADDR:
            self.log('   .. %s returned through pc %#x' % (label, pc))
        return uc.reg_read(UC_ARM_REG_R0)
