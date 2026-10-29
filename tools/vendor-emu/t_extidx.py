#!/usr/bin/env python3
"""让厂商 tm.ko 自己跑出 "extract index 表 + 规则表" 的 provision 流水，
以及 **入端口 -> extract index -> 表项 byte16 / key 规则字节** 这条链。

背景（上一轮 `docs/vendor-acl-hardfast-re-2026-10-30.md` 的未解决项）：
v6 的 extract index 不写死在 9/10，而是走 `aclGetExtIdxRuleRamAddrByPort()`
的端口映射表（1..8）。上一轮只证明了 `tm_acl_fast_init` 没 provision 1..8，
没回答"1..8 该写什么、写哪张表、硬件按哪一侧取键"。

本脚本的四个子命令，都是让**厂商代码真身**跑、只在 `fpga_*` 这一级记流水：

    map    # 端口映射表的原始字节 + 解码（谁写的、每个端口->几）
    init   # aclRamInit()（会连带调 tm_acl_fast_init）写下的每一张表
    init9  # 只跑 tm_acl_fast_init，看它对 index 9/10 做的是 RMW 还是全量写
    prov   # 按厂商的 aclRamInit 形状，把 index 1..8 + 对应规则写下去
    flow   # 同一股 v6 流，只改入端口：表项 byte16 / key 里的规则号各是多少

用法（tools/vendor-emu，镜像 e2631-uni:py312）：
    python3 t_extidx.py map
    python3 t_extidx.py init
    python3 t_extidx.py init9
    python3 t_extidx.py prov
    python3 t_extidx.py flow
    python3 t_extidx.py all

新增脚本，不改动已有脚本与结论。
"""
import struct
import sys

import km
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP

TM = '/ko/tm.ko'
SW = '/ko/switch.ko'

SW_PHYS = 0x92000000            # switch 窗口（fpga_module_init 里的 0xF4000000）
CLA_OFF = 0x38c000              # CLA 在 switch 里的字节偏移
CLA_PHYS = SW_PHYS + CLA_OFF    # 0x9238c000
CLA_CMD = CLA_PHYS + 0x14       # 间接命令字
CLA_STATUS = CLA_PHYS + 0x18    # 间接状态字
CLA_DATA = CLA_PHYS + 0x1c      # 数据字 0

BOUNDARY = ('printk', '__memzero', 'memset',
            'fpga_read_reg', 'fpga_write_reg')

# 驱动里那份 17 字 v6 规则（与厂商 rule 2 逐字节相同，见
# docs/vendor-cla-extra-rule-2026-10-30.md §2.2）
V6_RULE = [
    0xe1828406, 0x16284880, 0x01e3868c, 0x94264889, 0xfff0000a,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0x0fffffff, 0x01300000,
    0xdb6db6db, 0x00db6db6,
]

# 板级端口（overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts）：
#   gmac_port0 = lan3, port1 = lan2, port2 = lan1, port3 = wan, port4 未接
PORT_LABEL = {0: 'lan3', 1: 'lan2', 2: 'lan1', 3: 'wan', 4: '(未接)'}


# ---------------------------------------------------------------- 小工具
def r0v(m):
    return [m.uc.reg_read(REG0_12[k]) for k in range(4)]


def set_r0(m, v):
    m.uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)


def cstr(m, a, n=200):
    out = bytearray()
    for i in range(n):
        try:
            b = m.rd(a + i, 1)
        except Exception:
            break
        if b == b'\x00':
            break
        out += b
    return out.decode('utf-8', 'replace')


def words_of(data):
    return [int.from_bytes(data[4 * i:4 * i + 4], 'little')
            for i in range(len(data) // 4)]


def pack_words(ws):
    return b''.join(struct.pack('<I', w & 0xffffffff) for w in ws)


def decode_cmd(v):
    return 'ram_id=%d index=%d rw=%d' % ((v >> 22) & 0xf, v & 0x3fffff,
                                        (v >> 27) & 1)


# ---------------------------------------------------------------- 硬件模型
class Hw:
    """最小模型：CLA 状态字恒 1（真机实测访问门是开的）。

    间接接口按 (ram_id, index) 存表，读命令把整项搬进数据寄存器，写命令的
    数据字落回表里 —— 这样 tm_acl_fast_init 的 "读-改-写" 才是真的 RMW，
    而不是读到上一条命令留下的脏数据。"""

    def __init__(self):
        self.mem = {}               # 原始寄存器（含 CLA_CMD / CLA_DATA）
        self.tab = {}               # (ram_id, index) -> [words]
        self.cur = None             # 当前写目标
        self.events = []            # ('r'|'w', phys, value)

    def nwords(self, rid):
        return 5 if rid == 0 else 17

    def read(self, phys):
        if phys == CLA_STATUS:
            self.events.append(('r', phys, 1))
            return 1
        v = self.mem.get(phys, 0)
        self.events.append(('r', phys, v))
        return v

    def write(self, phys, val):
        self.events.append(('w', phys, val))
        if phys == CLA_CMD:
            rid, idx, rw = (val >> 22) & 0xf, val & 0x3fffff, (val >> 27) & 1
            self.mem[CLA_CMD] = val
            if rw:
                ws = self.tab.get((rid, idx), [0] * self.nwords(rid))
                for k in range(self.nwords(rid)):
                    self.mem[CLA_DATA + 4 * k] = ws[k]
                self.cur = None
            else:
                self.cur = (rid, idx)
            return
        if CLA_DATA <= phys < CLA_DATA + 0x44:
            self.mem[phys] = val
            if self.cur:
                rid, idx = self.cur
                ws = self.tab.setdefault((rid, idx),
                                         [0] * self.nwords(rid))
                k = (phys - CLA_DATA) // 4
                if k < len(ws):
                    ws[k] = val
            return
        self.mem[phys] = val


def zero_bss(m):
    """km.py 不保证 .bss 是 0（实测 g_aclRamInited 读出来是 0x7b6），
    这里显式清零，否则 aclRamInit 会以为"已经初始化过"直接返回。"""
    for i, s in enumerate(m.elf.iter_sections()):
        if s.name != '.bss':
            continue
        base = m.sec_base[i]
        try:
            m.wr(base, s['sh_size'], b'\x00' * s['sh_size'])
        except Exception:
            pass


def install(m, hw, printk_sink=None):
    def h_read(_n):
        r = r0v(m)
        set_r0(m, hw.read(SW_PHYS + 4 * r[0]))

    def h_write(_n):
        r = r0v(m)
        hw.write(SW_PHYS + 4 * r[0], r[1])
        set_r0(m, 0)

    def h_memzero(_n):
        r = r0v(m)
        try:
            m.wr(r[0], r[1], b'\x00' * r[1])
        except Exception:
            pass        # aclRamInit 会 __memzero 0xF1020000/0xF1420000（外部窗口）

    def h_printk(_n):
        if printk_sink is None:
            set_r0(m, 0)
            return
        r = r0v(m)
        sp = m.uc.reg_read(UC_ARM_REG_SP)
        extra = [int.from_bytes(m.rd(sp + 4 * i, 4), 'little') for i in range(8)]
        printk_sink(cstr(m, r[0]), [r[1], r[2], r[3]] + extra)
        set_r0(m, 0)

    m.handlers['fpga_read_reg'] = h_read
    m.handlers['fpga_write_reg'] = h_write
    m.handlers['__memzero'] = h_memzero
    m.handlers['memset'] = h_memzero
    m.handlers['printk'] = h_printk


def groups(hw):
    """把 fpga 流水按间接命令字切成 [(cmd, [data...]), ...]。"""
    out = []
    for rw, phys, val in hw.events:
        if rw == 'w' and phys == CLA_CMD:
            out.append((val, []))
        elif rw == 'w' and CLA_DATA <= phys < CLA_DATA + 0x44 and out:
            out[-1][1].append(((phys - CLA_DATA) // 4, val))
    return out


def show_groups(hw, title='', reads=True):
    print('== %s' % title)
    for cmd, ds in groups(hw):
        rid = (cmd >> 22) & 0xf
        idx = cmd & 0x3fffff
        rw = (cmd >> 27) & 1
        if rw:
            if reads:
                print('   read  [%2d] %s（把整项搬进数据寄存器）'
                      % (idx, {0: 'EXTRA_INDEX', 1: 'EXTRA_RULE'}.get(rid, rid)))
            continue
        vals = dict(ds)
        ws = [vals.get(i, 0) for i in range(5 if rid == 0 else 17)]
        print('   write [%2d] %s  words: %s'
              % (idx, {0: 'EXTRA_INDEX', 1: 'EXTRA_RULE'}.get(rid, rid),
                 ' '.join('%08x' % w for w in ws)))


# ---------------------------------------------------------------- 端口映射表
def read_port_map(m):
    i = m.sym_index('s_aclPortExtIdxTblMap')
    s = m.symtab[i]
    a = m.sec_base[s['st_shndx']] + s['st_value']
    sz = s['st_size']
    raw = m.rd(a, sz)
    sec = m.elf.get_section(s['st_shndx']).name
    pairs = [(raw[2 * k], raw[2 * k + 1]) for k in range(len(raw) // 2)]
    return a, sec, sz, raw, pairs


def cmd_map():
    m = Mod(TM, intercept=(), log=lambda *a: None)
    a, sec, sz, raw, pairs = read_port_map(m)
    print('== s_aclPortExtIdxTblMap  %s+%#x  size %#x (%d 项 x 2 B)'
          % (sec, a - m.sec_base[[i for i, s in enumerate(m.elf.iter_sections())
                                  if s.name == sec][0]], sz, len(pairs)))
    print('   原始字节: ' + ' '.join('%02x%02x' % p for p in pairs))
    print('   解码（第一匹配生效）:')
    seen = {}
    for port, idx in pairs:
        if port not in seen:
            seen[port] = idx
    for port, idx in seen.items():
        label = PORT_LABEL.get(port, '')
        print('     端口 %#04x %-8s -> extract index %d' % (port, label, idx))
    tail = pairs[len(seen):]
    print('   尾部（首次匹配已命中，不可达）: ' +
          ' '.join('%#x->%d' % p for p in tail))
    print('   -> 这是一张 .rodata 里的 static const；tm.ko 里没有写它的代码。')


# ---------------------------------------------------------------- aclRamInit
def run_acl_init(seq=True):
    m = Mod(TM, intercept=BOUNDARY, log=lambda *a: None)
    zero_bss(m)
    hw = Hw()
    install(m, hw)
    # 让 aclRamInit 走 "有 wlan idm 表" 的分支，从而连带调用 tm_acl_fast_init
    i = m.sym_index('s_aclWlanToIdmEssidMap')
    s = m.symtab[i]
    m.wr(m.sec_base[s['st_shndx']] + s['st_value'] + 0x148, 4,
         struct.pack('<I', 1))
    ret = m.run('aclRamInit', (), max_steps=6000000)
    return m, hw, ret


def cmd_init():
    m, hw, ret = run_acl_init()
    print('== aclRamInit() -> ret %r, steps %d' % (ret, m.steps))
    for cmd, ds in groups(hw):
        rid = (cmd >> 22) & 0xf
        idx = cmd & 0x3fffff
        vals = dict(ds)
        n = 5 if rid == 0 else 17
        ws = [vals.get(i, 0) for i in range(n)]
        kind = {0: 'EXTRA_INDEX', 1: 'EXTRA_RULE'}.get(rid, 'ram_id%d' % rid)
        print('   %-11s [%3d] %s' % (kind, idx, ' '.join('%08x' % w for w in ws)))
    print('   -- 观察：index 1..10 是**全量 5 字写**；9/10 的 word4 基线是 '
          '0x00150000，index_valid 低 16 位为 0。')


def cmd_init9():
    """只跑 tm_acl_fast_init，并用真机 dump 到的 9/10 内容做 RMW 基线。"""
    m = Mod(TM, intercept=BOUNDARY, log=lambda *a: None)
    hw = Hw()
    install(m, hw)
    # 真机 dump（known-issues PPE-IPV6-TCP）：
    #   index[9]  = 93929190 97969594 9b9a9998 9f9e9d9c 00150151
    #   index[10] = 03020100 07060504 0b0a0908 0f0e0d0c 00150154
    for idx, ws in ((9, [0x93929190, 0x97969594, 0x9b9a9998, 0x9f9e9d9c,
                         0x00150151]),
                    (10, [0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
                          0x00150154])):
        hw.tab[(0, idx)] = list(ws)
    ret = m.run('tm_acl_fast_init', (), max_steps=3000000)
    print('== tm_acl_fast_init() -> ret %r, steps %d' % (ret, m.steps))
    show_groups(hw, 'tm_acl_fast_init 的硬件流水（index 9/10 是 RMW）')
    print('   -- 观察：它先 cla_get_extra_index_table(9/10) 读回 5 字，'
          '只把 word4 的低 16 位 OR 上 1 / 4，再整条写回；')
    print('      words 0..3（16 个 extract field id）原样保留——也就是'
          ' 9/10 之外一个字都没 provision。')


# ---------------------------------------------------------------- provision 1..8
def aclraminit_words(n):
    """aclRamInit 对 index n 算出来的 5 个字（含 10 号的特例）。"""
    if n == 10:
        ids = list(range(0x00, 0x10))
    else:
        ids = list(range((n << 4) & 0xff, ((n << 4) & 0xff) + 0x10))
    w = [0, 0, 0, 0, 0]
    for k, fid in enumerate(ids):
        w[k // 4] |= (fid & 0xff) << (8 * (k % 4))
    w[4] = 0x00190000 if n < 9 else 0x00150000
    return w


def cmd_prov():
    """按厂商 aclRamInit 的形状把 1..8 写全，并把 v6 规则写进每个组。"""
    m = Mod(TM, intercept=BOUNDARY, log=lambda *a: None)
    hw = Hw()
    install(m, hw)
    for n in range(1, 9):
        ws = aclraminit_words(n)
        ws[4] |= 0x1 << 8            # index_valid bit8：组内第 8 项（*<<4|8）
        buf = m.calloc(0x14)
        m.wr(buf, 0x14, pack_words(ws))
        r1 = m.run('cla_set_extra_index_table', (n, buf), max_steps=400000)
        rid = (n << 4) | 8
        rb = m.calloc(0x44)
        m.wr(rb, 0x44, pack_words(V6_RULE))
        r2 = m.run('cla_set_extra_rule_table', (rid, rb), max_steps=800000)
        print('   index[%d] <- %s   ret %r ; rule[%d] <- 19 窗口 v6 模板 ret %r'
              % (n, ' '.join('%08x' % w for w in ws), r1, rid, r2))
    show_groups(hw, 'index 1..8 + rule (n<<4|8) 的完整硬件流水')
    print('   -- 这是"可执行级别"的序列：每项 = 状态轮询 + 一条命令字 + 5/17 个数据字，'
          '物理地址见上表（CLA+0x14 / +0x1c+4i）。')


def cmd_trace(n=3):
    """index n 与 rule (n<<4)|8 的逐次物理访问（含状态轮询/读写）。"""
    m = Mod(TM, intercept=BOUNDARY, log=lambda *a: None)
    zero_bss(m)
    hw = Hw()
    install(m, hw)
    ws = aclraminit_words(n)
    ws[4] |= 0x1 << 8
    buf = m.calloc(0x14)
    m.wr(buf, 0x14, pack_words(ws))
    r1 = m.run('cla_set_extra_index_table', (n, buf), max_steps=400000)
    rb = m.calloc(0x44)
    m.wr(rb, 0x44, pack_words(V6_RULE))
    rid = (n << 4) | 8
    r2 = m.run('cla_set_extra_rule_table', (rid, rb), max_steps=800000)
    print('== index[%d] ret %r, rule[%d] ret %r' % (n, r1, rid, r2))
    print('   #   方向 phys          CLA+off   值          说明')
    for k, (rw, phys, val) in enumerate(hw.events):
        off = phys - CLA_PHYS
        note = ''
        if phys == CLA_CMD:
            note = decode_cmd(val)
        elif phys == CLA_STATUS:
            note = 'status bit0=1 放行'
        elif CLA_DATA <= phys < CLA_DATA + 0x44:
            note = 'data%d' % ((phys - CLA_DATA) // 4)
        print('   %02d  %s  %#010x  +%#06x  %#010x  %s'
              % (k, 'r' if rw == 'r' else 'w', phys, off, val, note))


# ---------------------------------------------------------------- 流实验
def build_desc(etype, d14):
    d = bytearray(0x124)
    d[0x00:0x04] = b'\x01\x00\x00\x00'
    d[0x0c:0x10] = struct.pack('<I', 2)
    d[0x14] = d14
    d[0x15] = 1
    d[0x10] = 0x11
    d[0x30:0x32] = struct.pack('>H', 48211)
    d[0x32:0x34] = struct.pack('>H', 9999)
    d[0x38:0x3a] = struct.pack('<H', 1)
    d[0x50] = etype
    d[0x51] = 0x10
    d[0x55] = 0x01
    d[0x40] = 0x01
    return bytes(d)


def fill_desc(m, p):
    sip = m.calloc(16)
    m.wr(sip, 16, bytes.fromhex('fd990000000000000000000000000002'))
    dip = m.calloc(16)
    m.wr(dip, 16, bytes.fromhex('fd880000000000000000000000000002'))
    da = m.calloc(8)
    m.wr(da, 8, bytes.fromhex('5054aabbccdd'))
    sa = m.calloc(8)
    m.wr(sa, 8, bytes.fromhex('d843ae681828'))
    for off, val in ((0x00, sip), (0x04, dip), (0x24, sa), (0x28, da),
                     (0x48, sip), (0x4c, dip)):
        m.wr(p + off, 4, struct.pack('<I', val))


def run_switch(inport, etype):
    """switch.ko 造 168 B zteHffEntry；g_sw_cap 用板级真实端口号 0..3。"""
    m = Mod(SW, intercept=('zte_api_fast_l3_session_add',), log=lambda *a: None)
    cap = {}
    gsc = 0x61000000
    m.uc.mem_map(gsc, 0x10000)
    m.uc.mem_write(gsc, b'\x00' * 0x10000)
    # 把 movw/movt 里的 g_sw_cap 地址挪到自建页（同 t_hfsess.py 的做法）
    ti = [i for i, s in enumerate(m.elf.iter_sections()) if s.name == '.text'][0]
    for sec in m.elf.iter_sections():
        if not str(sec['sh_type']).endswith('SHT_REL') or sec['sh_info'] != ti:
            continue
        for rel in sec.iter_relocations():
            if m.symtab[rel['r_info_sym']].name != 'g_sw_cap':
                continue
            P = m.sec_base[ti] + rel['r_offset']
            insn = struct.unpack('<I', m.rd(P, 4))[0]
            if rel['r_info_type'] == km.R_ARM_MOVW_ABS_NC:
                m.wr(P, 4, struct.pack('<I', km.movw_patch(insn, gsc & 0xffff)))
            elif rel['r_info_type'] == km.R_ARM_MOVT_ABS:
                m.wr(P, 4, struct.pack('<I',
                                       km.movw_patch(insn, (gsc >> 16) & 0xffff)))
    m.wr(gsc + 0x004, 4, struct.pack('<I', 4))
    m.wr(gsc + 0x00c, 8, bytes([0, 1, 2, 3, 4, 5, 6, 7]))
    m.wr(gsc + 0x190, 4, struct.pack('<I', 5))
    m.wr(gsc + 0x1a4, 4, struct.pack('<I', 2))

    def h_tmadd(_n):
        r = r0v(m)
        cap['entry'] = m.rd(r[0], 0xa8)
        m.wr(r[1], 24, bytes(24))
        set_r0(m, 0)

    m.handlers['zte_api_fast_l3_session_add'] = h_tmadd
    names = {}
    for nm in ('wan', 'pon', 'ptm0', 'eth0', 'eth1', 'eth2', 'eth3', 'br-lan'):
        names[nm] = m.cstr(nm)
    p = m.calloc(0x124)
    m.wr(p, 0x124, build_desc(etype, 1))
    m.wr(p + 0x18, 4, struct.pack('<I', names['eth%d' % inport]))
    m.wr(p + 0x1c, 4, struct.pack('<I', names['eth%d' % inport]))
    fill_desc(m, p)
    m.run('sw_acl_l3_hardfast_session_add', (p,), max_steps=500000)
    return cap.get('entry')


def run_tm(entry):
    """tm.ko 吃 168 B 条目；只拦边界，记录 60 B hash 表项。"""
    import flowadd
    c = flowadd.Case.__new__(flowadd.Case)
    c.log, c.got, c.quiet, c.extidx = [], [], True, 0
    ic = tuple(n for n in flowadd.INTERCEPT
               if n != 'aclGetExtIdxRuleRamAddrByPort')
    c.m = m = Mod(TM, intercept=ic, log=lambda *a: None)
    for n in ic:
        m.handlers[n] = c._h(n)

    def h_status(_n):
        r = r0v(m)
        m.wr(r[0], 4, struct.pack('<I', 0))
        set_r0(m, 0)

    def h_alloc(_n):
        set_r0(m, m.calloc(0x200, 16))

    def h_memcmp(_n):
        r = r0v(m)
        set_r0(m, 0 if m.rd(r[0], r[2]) == m.rd(r[1], r[2]) else 1)

    def h_memcpy(_n):
        r = r0v(m)
        m.wr(r[0], r[2], m.rd(r[1], r[2]))
        set_r0(m, r[0])

    def h_strlen(_n):
        r = r0v(m)
        set_r0(m, len(m.rd(r[0], 200).split(b'\x00')[0]))

    def h_fpgar(_n):
        set_r0(m, 0)

    m.handlers.update({'cla_get_indirect_rw_status': h_status,
                       'kmem_cache_alloc': h_alloc, '__kmalloc': h_alloc,
                       'kmalloc_order': h_alloc, 'memcmp': h_memcmp,
                       'memcpy': h_memcpy, 'strlen': h_strlen,
                       'fpga_read_reg': h_fpgar})
    skip = ['findSubnetIdBySrcMac', 'operInfoAdd']
    skip += [s.name for s in m.symtab
             if s['st_info']['type'] == 'STT_FUNC'
             and (s.name.startswith('pp_pm_') or s.name.startswith('pon_pp_set')
                  or s.name.startswith('tm_ipv6table_')
                  or s.name.startswith('sbrg_add'))]
    for nm in skip:
        try:
            fe, _ = m.func(nm)
        except KeyError:
            continue
        m.wr(fe, 8, struct.pack('<II', 0xe3a00000, 0xe12fff1e))
    flow = m.calloc(0xa8)
    m.wr(flow, 0xa8, bytes(entry))
    out = m.calloc(0x40)
    l1 = c.sym('.LANCHOR1')
    m.wr(l1 + 0x148, 4, struct.pack('<I', 1))
    ret = m.run('zte_api_fast_l3_session_add', (flow, out, out),
                max_steps=2000000)
    return ret, c.got, m


def entry_fields(data):
    """60 B 表项 -> (byte16, key 里的规则字节, 前 4 个 key 字节)。"""
    b16 = data[16]
    key = data[16:60]
    rule = ((key[2] >> 7) & 1) | ((key[3] & 0x7f) << 1)
    return b16, rule, key[:4]


def cmd_flow():
    """同一股 v6 流，只改条目 +0x9e（inport）：表项 byte16 与 hash 槽怎么变。

    这里直接改 168 B 条目的 +0x9e，而不是靠 switch.ko 从设备名查端口：
    get_sw_port_from_devname 读的是厂商 vmlinux 里的 g_sw_cap，离线拿不到；
    上一轮已用 A/B 证明 +0x9e 就是 inport（只影响 extract index，不进条目）。
    """
    import t_hfsess
    m0 = Mod(TM, intercept=(), log=lambda *a: None)
    _, _, _, _, pairs = read_port_map(m0)
    seen = {}
    for port, idx in pairs:
        seen.setdefault(port, idx)
    e0, _ = t_hfsess.run_switch(t_hfsess.build_desc(d14=1),
                                log=lambda *a: None)
    if e0 is None:
        print('switch.ko 没走到 session_add')
        return
    print('== 同一股 v6 流（fd99::2:48211 -> fd88::2:9999，会话 type 2），'
          '只改条目 +0x9e（inport）')
    print('   条目 +0x9e  板级口  端口表 idx  表项 byte16  slot')
    for p in (0, 1, 2, 3, 5):
        e = bytearray(e0)
        e[0x9e] = p
        res = t_hfsess.run_tm(bytes(e))
        hs = [(i, d) for k, i, d in res['got'] if k == 'hash']
        idx = seen.get(p, -1)
        if not hs:
            print('   %-11d %-7s %-11d (无 hash 写入, ret=%r)'
                  % (p, PORT_LABEL.get(p, '?'), idx, res['ret']))
            continue
        for slot, d in hs:
            print('   %-11d %-7s %-11d %#04x         %d'
                  % (p, PORT_LABEL.get(p, '?'), idx, d[16], slot))
    print('   -- 观察：入口同一五元组，只有 +0x9e 变，表项 byte16 = 0x40|idx，'
          'hash 槽也跟着变。')
    print('      这说明 idx 在厂商自己的路径上就是'
          ' aclGetExtIdxRuleRamAddrByPort(inport) 的返回值，')
    print('      而且它参与 hash 键，所以"槽"和"硬件取键用的 idx"必须是同一个。')


def main(argv):
    cmd = argv[1] if len(argv) > 1 else 'all'
    if cmd == 'map':
        cmd_map()
    elif cmd == 'init':
        cmd_init()
    elif cmd == 'init9':
        cmd_init9()
    elif cmd == 'prov':
        cmd_prov()
    elif cmd == 'trace':
        cmd_trace(int(argv[2], 0) if len(argv) > 2 else 3)
    elif cmd == 'flow':
        cmd_flow()
    else:
        cmd_map()
        cmd_init()
        cmd_init9()
        cmd_prov()
        cmd_flow()


if __name__ == '__main__':
    main(sys.argv)
