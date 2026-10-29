#!/usr/bin/env python3
"""switch.ko / tm.ko 的带符号标注反汇编器 + 静态/仿真小工具（离线）。

为什么需要它：`objdump -dr` 在这种 `ld -r` 合并出来的模块上会把多个 .o 里
同名的 `.LC43` 打成一个名字，字符串对不上；`bl` 也只有重定位项才准。
这里复用 `km.py` 的布局与重定位（模块被整体搬到 base 上、重定位按符号下标
真解一遍），所以每条 `bl`、每对 `movw/movt`、每个 `ldr rX,[pc,#n]` 都能落到
唯一一个值 / 唯一的字符串上。

用法（在 tools/vendor-emu 里跑，镜像 e2631-uni:py312）：
    python3 t_hfset.py dis sw_acl_l3_hardfast_session_add
    python3 t_hfset.py dis hf_set_l3_entry hf_del_l3_entry hf_query_l3_status hf_entry_police2
    python3 t_hfset.py struct sw_acl_l3_hardfast_session_add   # 只列 str* 到栈上的字段
    python3 t_hfset.py data npu_cap_array 0x60
    python3 t_hfset.py str 0x16a0
    python3 t_hfset.py xref hf_set_l3_entry
"""
import struct
import sys

import km
from km import Mod

KO = '/ko/switch.ko'


class View:
    def __init__(self, path=KO, base=0x10000000):
        self.m = Mod(path, base=base, log=lambda *a: None)
        self.base = self.m.base
        self.modsize = self.m.modsize
        # 地址 -> 符号名
        self.by_name = {}
        for i, s in enumerate(self.m.symtab):
            if not s.name or s['st_shndx'] in ('SHN_UNDEF',) or isinstance(s['st_shndx'], str):
                self.by_name.setdefault(s.name, (None, None))
                continue
            a = self.m.sec_base.get(s['st_shndx'])
            if a is None:
                continue
            self.by_name.setdefault(s.name, (a + s['st_value'], s['st_size']))
        self.at = {}
        for n, (a, sz) in self.by_name.items():
            if a is not None and not n.startswith('.LC'):
                self.at[a] = (n, sz)

    # --- 地址 -> 名字 ---
    def name_at(self, addr):
        if addr in self.at:
            return self.at[addr][0]
        if self.m.stub_base <= addr < self.m.stub_base + 0x10000:
            return self.m.stub_by_addr.get(addr, '?stub')
        return None

    def loc(self, addr):
        """返回 ('sec', off) 便于打印。"""
        for i, b in self.m.sec_base.items():
            sz = self.m.sec_size.get(i, 0)
            if b <= addr < b + max(sz, 1):
                sec = self.m.elf.get_section(i)
                return (sec.name, addr - b)
        return None

    def str_at(self, addr):
        l = self.loc(addr)
        if not l:
            return None
        sec, off = l
        if not sec or not sec.startswith('.rodata.str'):
            return None
        b = self.m.elf.get_section_by_name(sec).data()
        end = b.find(b'\x00', off)
        if end < 0:
            return None
        s = b[off:end]
        try:
            t = s.decode('utf-8')
        except UnicodeDecodeError:
            return None
        return t if all(32 <= ord(c) < 127 or c in '\n\t' for c in t) else None

    def word(self, addr):
        try:
            return struct.unpack('<I', self.m.rd(addr, 4))[0]
        except Exception:
            return None


def _pair_mov(v, insns):
    """movw/movt 配对：返回 {movt 地址: (值, 说明)}，并把 movw 的地址记下来。"""
    notes, pend = {}, {}
    for ins in insns:
        if ins.mnemonic not in ('movw', 'movt') or not ins.operands:
            continue
        reg = ins.op_str.split(',')[0].strip()
        if ins.mnemonic == 'movw':
            pend[reg] = (ins.address, int(ins.op_str.split('#')[-1], 0))
        else:
            if reg not in pend:
                continue
            lo = pend.pop(reg)
            val = (int(ins.op_str.split('#')[-1], 0) << 16) | lo[1]
            notes[ins.address] = val
    return notes


def annotate(v, name, only_mem=False):
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    entry, size = v.m.func(name)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    insns = list(md.disasm(v.m.rd(entry, size), entry))
    pairs = _pair_mov(v, insns)
    out = []
    for ins in insns:
        extra = []
        if ins.mnemonic == 'movw':
            reg = ins.op_str.split(',')[0].strip()
            lo = int(ins.op_str.split('#')[-1], 0)
            # 后面紧跟同寄存器的 movt 才配对
            for nxt in insns[insns.index(ins) + 1:insns.index(ins) + 3]:
                if nxt.mnemonic == 'movt' and nxt.op_str.split(',')[0].strip() == reg:
                    val = (int(nxt.op_str.split('#')[-1], 0) << 16) | lo
                    s = v.str_at(val) if v.loc(val) else None
                    n = v.name_at(val)
                    extra.append('; &%#x %s' % (val, ('"%s"' % s[:90]) if s else (('&' + n) if n else '')))
                    break
        if ins.address in pairs:
            val = pairs[ins.address]
            s = v.str_at(val) if v.loc(val) else None
            n = v.name_at(val)
            if s:
                extra.append('; %#x "%s"' % (val, s[:110]))
            elif n:
                extra.append('; %#x &%s' % (val, n))
            else:
                extra.append('; %#x' % val)
        ops = ins.op_str
        # bl / b -> 名字
        if ins.mnemonic in ('bl', 'blx') and ops.startswith('#'):
            tgt = int(ops[1:], 0)
            n = v.name_at(tgt)
            if n:
                extra.append('; %s' % n)
        # ldr/str 到 pc 相对或绝对字
        if ins.mnemonic in ('ldr', 'str') and '[pc' in ops:
            try:
                imm = int(ops.split('#')[-1].rstrip(']'), 0)
                tgt = (ins.address + 8 + imm) & 0xffffffff
                w = v.word(tgt)
                if w is not None:
                    s = v.str_at(w) if v.loc(w) else None
                    n = v.name_at(w)
                    if s:
                        extra.append('; "%s"' % s[:90])
                    elif n:
                        extra.append('; ->%s (%#x)' % (n, w))
                    else:
                        extra.append('; ->%#x' % w)
            except Exception:
                pass
        # movw/movt 的绝对地址
        if ins.mnemonic in ('movw', 'movt') and '#' in ops:
            try:
                imm = int(ops.split('#')[-1], 0)
                n = v.name_at(imm)
                if n and ins.mnemonic == 'movt':
                    extra.append('; hi(%s)' % n)
                elif n:
                    extra.append('; &%s' % n)
                else:
                    s = v.str_at(imm)
                    if s:
                        extra.append('; "%s"' % s[:90])
            except Exception:
                pass
        if only_mem and not extra:
            continue
        out.append('  %08x  %-8s %-30s %s' % (ins.address, ins.mnemonic, ops, ' '.join(extra)))
    return '\n'.join(out)


def main(argv):
    cmd = argv[1]
    ko = KO
    if cmd.startswith('/') or cmd.endswith('.ko'):
        ko = argv[1]
        cmd = argv[2]
        argv = argv[1:]
    v = View(ko)
    args = argv[2:]
    if cmd == 'dis':
        for name in args:
            entry, size = v.m.func(name)
            print('\n=== %s  file+%#x  size %#x (%d) ===' % (name, entry - v.base, size, size))
            print(annotate(v, name))
    elif cmd == 'mem':
        for name in args:
            entry, size = v.m.func(name)
            print('\n=== %s  file+%#x  size %#x ===' % (name, entry - v.base, size))
            print(annotate(v, name, only_mem=True))
    elif cmd == 'data':
        name = args[0]
        n = int(args[1], 0) if len(args) > 1 else 0x40
        a, sz = v.by_name[name]
        n = min(n, sz or n)
        b = v.m.rd(a, n)
        print('%s @%#x size %#x' % (name, a, sz))
        for i in range(0, n, 16):
            print('  +%#04x  %s  |%s|' % (i, ' '.join('%02x' % c for c in b[i:i + 16]),
                                          ''.join(chr(c) if 32 <= c < 127 else '.' for c in b[i:i + 16])))
    elif cmd == 'str':
        off = int(args[0], 0)
        for sec in ('.rodata.str1.4', '.rodata.str', '.rodata'):
            try:
                s = v.m.elf.get_section_by_name(sec).data()
            except Exception:
                continue
            if off < len(s):
                end = s.find(b'\x00', off)
                print('%s+%#x: %r' % (sec, off, s[off:end]))
    elif cmd == 'xref':
        for name in args:
            print('== xref', name)
            for off, rel in _rels(v).items():
                if rel[1] == name:
                    who = '?'
                    for s in v.m.symtab:
                        if s['st_info']['type'] == 'STT_FUNC' and s['st_value'] <= off - v.m.sec_base[s['st_shndx']] < s['st_value'] + max(s['st_size'], 4):
                            who = s.name
                            break
                    print('   file+%#06x  type %d  in %s' % (off, rel[0], who))
    elif cmd == 'calls':
        # 谁 BL 到它：只能看"重定位之后"的指令，局部函数的重定位符号是 .text 段
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        want = {}
        for name in args:
            want[v.m.func(name)[0]] = name
        ti = [i for i, s in enumerate(v.m.elf.iter_sections()) if s.name == '.text'][0]
        code = v.m.rd(v.m.sec_base[ti], v.m.sec_size[ti])
        for ins in md.disasm(code, v.m.sec_base[ti]):
            if ins.mnemonic not in ('bl', 'b') or not ins.op_str.startswith('#'):
                continue
            t = int(ins.op_str[1:], 0)
            if t in want:
                who = '?'
                off = ins.address - v.m.sec_base[ti]
                for t2 in v.m.symtab:
                    if t2['st_info']['type'] == 'STT_FUNC' and t2['st_shndx'] == ti \
                            and t2['st_value'] <= off < t2['st_value'] + max(t2['st_size'], 4):
                        who = t2.name
                        break
                print('%s <- %s at +%#x' % (want[t], who, off))
    else:
        print(__doc__)


def _rels(v):
    """把 .text 的 (偏移 -> (type, symname)) 缓存下来，键是文件内偏移。"""
    if getattr(v, '_relcache', None) is not None:
        return v._relcache
    d = {}
    for sec in v.m.elf.iter_sections():
        if not str(sec['sh_type']).endswith('SHT_REL'):
            continue
        ti = sec['sh_info']
        if ti not in v.m.sec_base:
            continue
        tb = v.m.sec_base[ti]
        for r in sec.iter_relocations():
            nm = v.m.symtab[r['r_info_sym']].name
            d[tb + r['r_offset'] - v.base] = (r['r_info_type'], nm)
    v._relcache = d
    return d


if __name__ == '__main__':
    main(sys.argv)
