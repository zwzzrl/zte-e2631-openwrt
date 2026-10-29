#!/usr/bin/env python3
"""cla_set_extra_rule_table() 到底往哪儿写了什么？

本目录以前的脚本（flowadd.py / t_init.py / t_v6chain.py / t_v6rule.py）都把
`cla_set_extra_rule_table` 当成"黑盒符号"拦截掉（它就在 INTERCEPT 列表里），
所以谁也没见过它内部的硬件动作，只知道它的参数是 (rule_id, 68 字节缓冲区)。

这一轮只拦**真正的边界** —— `fpga_read_reg` / `fpga_write_reg` / `printk` ——
让 tm.ko 自己的

    cla_set_extra_rule_table
      -> cla_get_indirect_rw_status / cla_set_indirect_rw_cmd / cla_set_indirect_rw_data
        -> tmOnuRegWrite / tmOnuRegRead
          -> fpga_write_reg / fpga_read_reg

整条链跑起来。fpga_* 的入参是"描述符字偏移"，物理地址 = 0x92000000 + 4 * off
（docs/vendor-cla-hash-recovered-2026-10-30.md、README 第三轮），所以拦在 fpga_*
这一层就能直接报出 基址+偏移。

子命令：

    python3 t_v6extra.py init      # 真 init 传给这个函数的是哪几号规则、什么内容
    python3 t_v6extra.py trace     # 对该函数逐次"碰硬件"的读写流水
    python3 t_v6extra.py decode    # 打开厂商自己的调试打印，看 17 个字的字段名
    python3 t_v6extra.py flow      # IPv6/IPv4 的流添加路径到底调不调它
    python3 t_v6extra.py bits      # 逐位扫描：17 个字里每一位是哪本字段
    python3 t_v6extra.py all       # 依次跑上面几个

新增脚本，不改动已有结论/脚本。
"""
import struct
import sys
import types

from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP

TM = '/ko/tm.ko'
SW_PHYS = 0x92000000          # 交换芯片窗口（fpga module_init 存的 0xF4000000 对应）
CLA_OFF = 0x38c000            # CLA 窗口在 switch 里的字节偏移（0x92000000 + 0x38c000）
CLA_PHYS = SW_PHYS + CLA_OFF  # 0x9238c000

# 只拦边界。cla_* / tmOnu* 全部让它自己跑。
#
# tmOnuGlbRegValidation 不能拦：它是 tm.ko 自己的函数，除了校验还把"寄存器描述符
# 地址"写到第 2 个实参指向的位置；拦掉它（像 t_init3.py 那样直接返回 0）会让
# tmOnuRegWrite/Read 拿到 NULL 描述符，偏移全算成 0。这是本轮踩到的坑。
BOUNDARY = ('printk', '__memzero', 'memset',
            'fpga_read_reg', 'fpga_write_reg')

# init 的参数捕获：这两个要拦住，否则看不到实参
CAPTURE = BOUNDARY + ('cla_set_extra_rule_table', 'cla_set_extra_index_table',
                      'cla_get_extra_index_table', 'cla_get_extra_rule_table',
                      'cla_get_hash_poly_config', 'cla_set_hash_table',
                      'cla_set_external_hash_table', 'cla_get_external_hash_table')

CLA_STATUS = CLA_PHYS + 0x18   # 间接访问状态字（reg 1）
CLA_CMD = CLA_PHYS + 0x14      # 间接访问命令字（reg 0）
CLA_DATA = CLA_PHYS + 0x1c     # 数据字 0（reg 2），共 17 个字


def cstr(m, a, n=200):
    """逐字节读，撞到未映射的页就停 —— .rodata.str1.4 末尾的字符串
    紧贴模块映像边界，一次 rd(200) 会踩到未映射页。"""
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


def regs(m):
    return [m.uc.reg_read(REG0_12[i]) for i in range(4)]


def setret(m, v):
    m.uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)


def sym_addr(m, name):
    i = m.sym_index(name)
    s = m.symtab[i]
    return m.sec_base[s['st_shndx']] + s['st_value']


def words_of(data):
    return [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(len(data) // 4)]


# ---------------------------------------------------------------- 硬件模型
class Hw:
    """最小硬件模型：状态字恒为 1（访问门是开的，第五轮真机实测），
    其余寄存器读回"我们写进去的值"，初值 0。"""

    def __init__(self):
        self.mem = {}
        self.events = []          # ('r'|'w', phys, value)

    def read(self, phys):
        if phys == CLA_STATUS:
            self.events.append(('r', phys, 1))
            return 1
        v = self.mem.get(phys, 0)
        self.events.append(('r', phys, v))
        return v

    def write(self, phys, val):
        self.mem[phys] = val
        self.events.append(('w', phys, val))


def hw_handlers(m, hw, trace=True):
    def h_read(_n):
        off = regs(m)[0]
        phys = SW_PHYS + 4 * off
        setret(m, hw.read(phys))

    def h_write(_n):
        off, val = regs(m)[0], regs(m)[1]
        hw.write(SW_PHYS + 4 * off, val)
        setret(m, 0)

    m.handlers['fpga_read_reg'] = h_read
    m.handlers['fpga_write_reg'] = h_write


# ---------------------------------------------------------------- 打印解码
def fmt_printk(m, fmt, args):
    """把一条 printk 的格式串和实参渲染成文本（够用即可：%d %u %x %s %p）。"""
    out, ai, i = [], 0, 0
    while i < len(fmt):
        c = fmt[i]
        if c != '%':
            out.append(c)
            i += 1
            continue
        j = i + 1
        while j < len(fmt) and fmt[j] in '-+ #0.0123456789lh':
            j += 1
        if j >= len(fmt):
            out.append(fmt[i:])
            break
        conv = fmt[j]
        spec = fmt[i:j + 1]
        a = args[ai] if ai < len(args) else 0
        ai += 1
        if conv in 'di':
            # 厂商打印里有些是 %d 打的无符号量
            sv = a - (1 << 32) if a & 0x80000000 else a
            out.append(spec.replace('l', '') % sv)
        elif conv in 'uxX':
            out.append(spec.replace('l', '') % a)
        elif conv == 's':
            out.append(cstr(m, a, 80))
        elif conv == 'c':
            out.append(chr(a & 0xff))
        else:
            out.append(spec.replace('l', '') % a)
        i = j + 1
    return ''.join(out)


def install_printk(m, sink):
    """sink 收到 (fmt, args) 或已经渲染好的文本。"""
    def h(_n):
        r = regs(m)
        sp = m.uc.reg_read(UC_ARM_REG_SP)
        extra = [int.from_bytes(m.rd(sp + 4 * i, 4), 'little') for i in range(8)]
        args = [r[1], r[2], r[3]] + extra
        sink(m, cstr(m, r[0]), args)
        setret(m, 0)
    m.handlers['printk'] = h


def install_mem(m):
    def h(_n):
        r = regs(m)
        if _n == '__memzero':
            m.wr(r[0], r[1], b'\x00' * r[1])
        else:
            m.wr(r[0], r[2], bytes([r[1] & 0xff]) * r[2])
        setret(m, 0)
    m.handlers['__memzero'] = h
    m.handlers['memset'] = h


# ---------------------------------------------------------------- init 捕获
def capture_init(entry='tm_acl_fast_init'):
    """把 cla_set_extra_rule_table / _index_table 拦住，看真 init 传了什么。"""
    logs = []
    m = Mod(TM, intercept=CAPTURE, log=lambda s: None)
    hw = Hw()
    hw_handlers(m, hw)
    install_printk(m, lambda mm, f, a: None)
    install_mem(m)
    got = []

    def h_rule(_n):
        r = regs(m)
        got.append(('rule', r[0], m.rd(r[1], 68)))
        setret(m, 0)

    def h_index(_n):
        r = regs(m)
        got.append(('index', r[0], m.rd(r[1], 20)))
        setret(m, 0)

    def h_get_rule(_n):
        m.wr(regs(m)[1], 68, b'\x00' * 68)
        setret(m, 0)

    def h_get_index(_n):
        m.wr(regs(m)[1], 20, b'\x00' * 20)
        setret(m, 0)

    def h_poly(_n):
        m.wr(regs(m)[0], 32, struct.pack('<8I', 0, 1, 0, 3, 0, 1, 0, 3))
        setret(m, 0)

    def h_hash_set(_n):
        got.append(('hash', regs(m)[0], m.rd(regs(m)[1], 60)))
        setret(m, 0)

    def h_xhash(_n):
        got.append(('xhash', regs(m)[0], m.rd(regs(m)[2], 60)))
        setret(m, 0)

    m.handlers['cla_set_extra_rule_table'] = h_rule
    m.handlers['cla_set_extra_index_table'] = h_index
    m.handlers['cla_get_extra_rule_table'] = h_get_rule
    m.handlers['cla_get_extra_index_table'] = h_get_index
    m.handlers['cla_get_hash_poly_config'] = h_poly
    m.handlers['cla_set_hash_table'] = h_hash_set
    m.handlers['cla_set_external_hash_table'] = h_xhash
    ret = m.run(entry, (0,), max_steps=4000000)
    return ret, got, m


# ---------------------------------------------------------------- 单函数追踪
def mod_for_trace(debug=0, printk_sink=None):
    m = Mod(TM, intercept=BOUNDARY, log=lambda s: None)
    hw = Hw()
    hw_handlers(m, hw)
    install_mem(m)
    if printk_sink is not None:
        install_printk(m, printk_sink)
    else:
        install_printk(m, lambda mm, f, a: None)
    if debug:
        m.wr(sym_addr(m, 'g_tm_debug_level'), 4, struct.pack('<I', debug))
    return m, hw


def call_extra_rule(m, rule_id, words):
    buf = m.calloc(0x44)
    m.wr(buf, 0x44, b''.join(struct.pack('<I', w & 0xffffffff) for w in words))
    return m.run('cla_set_extra_rule_table', (rule_id, buf), max_steps=2000000)


def decode_cmd(v):
    return 'index=%d table=%d rw=%d' % (v & 0x3fffff, (v >> 22) & 0xf, (v >> 27) & 1)


def show_events(hw, rule_id, words):
    print('   -- rule %d 的 17 个字:' % rule_id)
    for i in range(0, 17, 3):
        print('      w%02d %s' % (i, ' '.join('%08x' % w for w in words[i:i + 3])))
    print('   -- 硬件访问流水（phys 相对 CLA 窗口 0x9238c000 的偏移）:')
    for n, (rw, phys, val) in enumerate(hw.events):
        off = phys - SW_PHYS
        tag = ''
        if phys == CLA_CMD:
            tag = '  <- 间接命令字: ' + decode_cmd(val)
        elif CLA_DATA <= phys < CLA_DATA + 0x44:
            tag = '  <- 数据字 %d' % ((phys - CLA_DATA) // 4)
        elif phys == CLA_STATUS:
            tag = '  <- 间接状态字'
        name = 'read ' if rw == 'r' else 'write'
        print('   %02d %s  phys %#010x (switch+%#07x, CLA+%#04x) = %#010x%s'
              % (n, name, phys, off, off - CLA_OFF, val, tag))


# ---------------------------------------------------------------- 各子命令
def cmd_init():
    ret, got, m = capture_init()
    print('== tm_acl_fast_init -> ret %r, %d 次表写入' % (ret, len(got)))
    for kind, idx, data in got:
        w = words_of(data)
        if kind == 'rule':
            print('   extra_rule[%3d] %s' % (idx, ' '.join('%08x' % x for x in w)))
        elif kind == 'index':
            print('   extra_index[%2d] %s' % (idx, ' '.join('%08x' % x for x in w)))
        elif kind == 'hash':
            print('   hash[%3d] %d 字' % (idx, len(w)))
        else:
            print('   %-6s %s %d 字' % (kind, idx, len(w)))
    return got


def cmd_trace(got=None):
    if got is None:
        _, got, _ = capture_init()
    rules = [(i, words_of(d)) for k, i, d in got if k == 'rule']
    if not rules:
        print('没有捕获到 extra rule 写入')
        return
    for idx, words in rules:
        m, hw = mod_for_trace(debug=0)
        ret = call_extra_rule(m, idx, words)
        print('== cla_set_extra_rule_table(%d, buf) -> ret %r, %d 步'
              % (idx, ret, m.steps))
        show_events(hw, idx, words)
        print()


def cmd_decode(got=None):
    if got is None:
        _, got, _ = capture_init()
    rules = [(i, words_of(d)) for k, i, d in got if k == 'rule']
    for idx, words in rules:
        lines = []

        def sink(mm, f, a, _l=lines):
            _l.append(fmt_printk(mm, f, a))

        m, hw = mod_for_trace(debug=7, printk_sink=sink)
        ret = call_extra_rule(m, idx, words)
        print('== cla_set_extra_rule_table(%d, ...) ret %r，厂商自己打印的字段：' % (idx, ret))
        for l in lines:
            print('   ' + l.rstrip())
        print()


def cmd_flow():
    """IPv6 / IPv4 的 zte_api_fast_l3_session_add 用真身跑。

    和 trace 一样只拦边界：tmOnuRegWrite/Read、cla_* 全部跑真代码，于是
    "这条流到底写了哪些表、写到哪个物理地址"是代码自己走出来的，不是我们猜的。
    """
    import t_v6sess

    cases = ((2, True, 'IPv6 family 2'),
             (0, True, 'IPv6 family 0'),
             (3, True, 'IPv6 family 3'),
             (2, False, 'IPv4 family 2'),
             (3, False, 'IPv4 family 3'))
    for fam, v6, name in cases:
        e = t_v6sess.entry_bytes(fam, v6=v6)
        rs = {i: e[i:i + 1] for i in range(len(e)) if e[i]}
        rs[0x04] = e[0x04:0x14]
        rs[0x14] = e[0x14:0x24]
        rs[0x84] = e[0x84:0x94]

        m, hw = mod_for_trace(debug=0)
        fake = types.SimpleNamespace(m=m, log=[])
        helpers = dict(t_v6sess.extra_handlers(fake))
        # 这些是边界，必须让真函数跑（否则看不到物理地址）
        for k in ('tmOnuRegWrite', 'tmOnuRegRead', 'fpga_read_reg',
                  'fpga_write_reg'):
            helpers.pop(k, None)
        m.handlers.update(helpers)
        t_v6sess.patch_onu_mac(m)

        flow = m.calloc(0xa8)
        m.wr(flow, 0xa8, b'\x00' * 0xa8)
        for off, val in rs.items():
            m.wr(flow + off, len(val), val)
        l1 = sym_addr(m, '.LANCHOR1')
        m.wr(l1 + 0x148, 4, 1)
        scratch = m.calloc(0x40)
        m.wr(scratch, 0x40, b'\x00' * 0x40)
        ret = m.run('zte_api_fast_l3_session_add', (flow, flow + 192, scratch),
                    max_steps=3000000)
        print('== %-14s ret %r steps %d' % (name, ret, m.steps))
        # 按窗口归类
        cla = [ev for ev in hw.events if CLA_PHYS <= ev[1] < CLA_PHYS + 0x400]
        other = [ev for ev in hw.events if not (CLA_PHYS <= ev[1] < CLA_PHYS + 0x400)]
        print('   CLA 窗口访问 %d 次，窗口外 %d 次' % (len(cla), len(other)))
        for rw, phys, val in cla:
            tag = ''
            if phys == CLA_CMD:
                tag = '  ' + decode_cmd(val)
            elif CLA_DATA <= phys < CLA_DATA + 0x44:
                tag = '  data%d' % ((phys - CLA_DATA) // 4)
            print('   %s CLA+%#04x = %#010x%s'
                  % ('w' if rw == 'w' else 'r', phys - CLA_PHYS, val, tag))
        seen = {}
        for rw, phys, val in other:
            if rw == 'w':
                seen[phys] = val
        for phys, val in sorted(seen.items()):
            print('   w 窗口外 phys %#010x = %#010x' % (phys, val))
        print()


def cmd_bits(got=None):
    """逐位扫描 17 个字：用厂商自己的字段打印，定位每一位属于哪个字段。

    一个 Mod 复用到三次扫描结束（每次只换那 68 字节缓冲区），544 次大约几秒。
    判定方式是"和全零基线比，哪个字段的值变了"，不是"这行打印出现了没有"。
    """
    if got is None:
        _, got, _ = capture_init()
    rules = [(i, words_of(d)) for k, i, d in got if k == 'rule']
    if not rules:
        print('没有捕获到 extra rule 写入')
        return
    idx, words = rules[0]
    sink_lines = []

    def sink(mm, f, a):
        # 跳过 cla_set_indirect_rw_* 自己的打印，只留字段解码
        if a and not f.startswith('[TM]'):
            sink_lines.append(fmt_printk(mm, f, a))

    m, hw = mod_for_trace(debug=7, printk_sink=sink)
    buf = m.calloc(0x44)

    def run(w):
        m.wr(buf, 0x44, b''.join(struct.pack('<I', x & 0xffffffff) for x in w))
        del sink_lines[:]
        ret = m.run('cla_set_extra_rule_table', (idx, buf), max_steps=2000000)
        fields = {}
        for txt in sink_lines:
            for part in txt.split(','):
                if '=' in part:
                    k, v = part.split('=', 1)
                    fields[k.strip()] = v.strip()
                elif ':' in part and part.strip().endswith(('0x0', '0x1', '0x2',
                                                           '0x3')):
                    k, v = part.split(':', 1)
                    fields[k.strip()] = v.strip()
        return ret, fields

    ret, base = run([0] * 17)
    print('== 零基线: rule_id=%s hash_len=%s mem_ctrl=%s (ret %r)'
          % (base.get('rule_id'), base.get('hash_len'),
             base.get('mem_ctrl'), ret))

    ret, real = run(words)
    print('== 厂商对 rule %d 的解码（17 个字里非零的字段）:' % idx)
    for k, v in real.items():
        if v not in ('0', '0x0'):
            print('   %-16s %s' % (k, v))

    field_bits = {}
    for bit in range(17 * 32):
        w = [0] * 17
        w[bit // 32] = 1 << (bit % 32)
        _, f = run(w)
        for k, v in f.items():
            if v != base.get(k):
                field_bits.setdefault(k, []).append(bit)

    print('\n== 字段 -> 位（相对零基线变化）')
    for k in sorted(field_bits):
        bits = field_bits[k]
        lo, hi = min(bits), max(bits)
        print('   %-16s bit %3d..%-3d  %2d 位  (w%d.%d..w%d.%d)'
              % (k, lo, hi, len(bits), lo // 32, lo % 32, hi // 32, hi % 32))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if cmd == 'init':
        cmd_init()
    elif cmd == 'trace':
        cmd_trace()
    elif cmd == 'decode':
        cmd_decode()
    elif cmd == 'flow':
        cmd_flow()
    elif cmd == 'bits':
        cmd_bits()
    else:
        got = cmd_init()
        cmd_trace(got)
        cmd_decode(got)
        cmd_flow()


if __name__ == '__main__':
    main()
