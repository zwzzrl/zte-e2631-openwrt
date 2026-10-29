#!/usr/bin/env python3
"""ACL L3 hardfast 条目（厂商 struct zteHffEntry）的离线取证：
switch.ko 造条目 -> tm.ko 落硬件。

两段分开跑，中间用一份**录下来的 168 字节条目**传递，所以每一步都能对得上：

  1) `sess`：在 switch.ko 里跑 sw_acl_l3_hardfast_session_add(desc)，拦
     zte_api_fast_l3_session_add，把 168 字节条目整个录下来（`tag` 模式下把输入
     描述符每个字节写成它自己的偏移标记，于是条目的每个字节"是谁写的"可读）。
  2) `hw`：把那份条目交给 tm.ko 的 zte_api_fast_l3_session_add，记录 CLA 表项
     （15 字 hash entry / 68 字节 rule / 20 字节 index）以及每一次 tmOnuRegWrite
     （寄存器号 + 值 + 下标）——"基址+偏移+值"是被拦到的，不是推的。
  3) `ab`：只改条目里的一个字节（默认 +0x9e = inport），重跑并 diff 全部硬件写，
     用来判定"该字段有没有进硬件"。

用法（tools/vendor-emu，镜像 e2631-uni:py312；kmodule 目录挂到 /ko）：
    python3 t_hfsess.py sess            # 造条目 -> /tmp/ztehff.bin
    python3 t_hfsess.py sess tag        # 同上，但输入描述符按偏移打标记
    python3 t_hfsess.py hw              # 条目 -> 硬件写流水
    python3 t_hfsess.py ab 0x9e 5       # 只改这个字节，看硬件写变不变
    python3 t_hfsess.py ab 0x00 1
"""
import os
import struct
import sys

import km
from km import Mod, REG0_12

SW = '/ko/switch.ko'
TM = '/ko/tm.ko'
SESS = os.environ.get('HFSESS', '/o/ztehff.bin')   # 挂在 /o 的工作目录里，跨容器保留
SIZE = 0xa8

SIP = bytes.fromhex('fd990000000000000000000000000002')
DIP = bytes.fromhex('fd880000000000000000000000000002')
SPORT, DPORT, PROTO = 48211, 9999, 0x11


def r0v(m):
    return [m.uc.reg_read(REG0_12[k]) for k in range(4)]


def set_r0(m, v):
    m.uc.reg_write(REG0_12[0], v & 0xffffffff)


def patch_sym_ptr(m, name, addr):
    """把 .text 里所有指向某个数据符号的 movw/movt 立即数改到 addr。

    km.py 已经把 g_sw_cap 之类的未定义符号重定位到 stub 区，而 stub 区每个符号
    只占 16 字节，往那里写一整张表会踩坏后面别的 stub（踩过一次：症状是
    __memzero 跳飞）。所以改成"挪到自己 map 的一页上"。
    """
    ti = None
    for i, s in enumerate(m.elf.iter_sections()):
        if s.name == '.text':
            ti = i
    n = 0
    for sec in m.elf.iter_sections():
        if not str(sec['sh_type']).endswith('SHT_REL') or sec['sh_info'] != ti:
            continue
        for rel in sec.iter_relocations():
            if m.symtab[rel['r_info_sym']].name != name:
                continue
            P = m.sec_base[ti] + rel['r_offset']
            insn = struct.unpack('<I', m.rd(P, 4))[0]
            typ = rel['r_info_type']
            if typ == km.R_ARM_MOVW_ABS_NC:
                m.wr(P, 4, struct.pack('<I', km.movw_patch(insn, addr & 0xffff)))
                n += 1
            elif typ == km.R_ARM_MOVT_ABS:
                m.wr(P, 4, struct.pack('<I', km.movw_patch(insn, (addr >> 16) & 0xffff)))
                n += 1
    return n


# ---------------------------------------------------------------- switch 侧
def build_desc(tag=False, **kw):
    """按 sw_acl_l3_hardfast_session_add / sw_acl_setMtchInfo 真读的偏移造输入描述符。

    每个 +0x.. 都能在 `t_hfset.py dis sw_acl_l3_hardfast_session_add` 里对上一条 ldr。
    """
    d = bytearray(0x124)
    if tag:
        for i in range(len(d)):
            d[i] = (i * 7 + 1) & 0xff
        return bytes(d)
    d[0x00:0x04] = b'\x01\x00\x00\x00'
    # +0x14 != 0 -> 会话 type 2（tm.ko 里 rule type 5 = IPv6 TCP 的那条）
    # +0x14 == 0 且 +0x15 != 0 -> 会话 type 由 +0x0c 派生（2 -> type 3，IPv4 那条）
    d[0x0c:0x10] = struct.pack('<I', kw.get('type', 2))
    d[0x14] = kw.get('d14', 0)
    d[0x15] = 1
    d[0x10] = PROTO
    d[0x30:0x32] = struct.pack('>H', SPORT)
    d[0x32:0x34] = struct.pack('>H', DPORT)
    d[0x38:0x3a] = struct.pack('<H', kw.get('vlan', 1))
    d[0x50] = kw.get('port', 2)
    d[0x51] = 0x10
    d[0x55] = 0x01
    d[0x40] = 0x01
    return bytes(d)


def fill_desc(m, p, desc, v6=True):
    """把描述符里那几个"指针字段"指向真的地址/端口结构。

    实测（t_hfset.py dis sw_acl_setMtchInfo）：desc+0x00 -> sip[16]、desc+0x04 ->
    dip[16]（r5 的两个 ldr 后各拷 16 字节到条目 +4/+0x14），desc+0x24/+0x28 ->
    {u32 addr, u16 port} 两个小结构（拷贝到条目 +0x36/+0x3c）。
    """
    sip = m.calloc(16)
    m.wr(sip, 16, SIP)
    dip = m.calloc(16)
    m.wr(dip, 16, DIP)
    # desc+0x24 / +0x28 各指向 6 字节 MAC：tm.ko 会把它们原样拷到条目 +0x3c / +0x36，
    # 并在那边做 "invalid source mac addr" 检查（t_hfsess.py hw 里能看到这条 printk）。
    da = m.calloc(8)
    m.wr(da, 8, bytes.fromhex('5054aabbccdd'))    # -> 条目 +0x36
    sa = m.calloc(8)
    m.wr(sa, 8, bytes.fromhex('d843ae681828'))    # -> 条目 +0x3c
    for off, val in ((0x00, sip), (0x04, dip),
                     (0x24, sa), (0x28, da),
                     (0x48, sip), (0x4c, dip)):   # +0x48/+0x4c 是 IPv6 的 sip/dip 指针
        m.wr(p + off, 4, struct.pack('<I', val))


def run_switch(desc, tag=False, log=None):
    """跑一次 switch.ko 的 session_add，返回它交给 tm.ko 的 168 字节条目。"""
    m = Mod(SW, intercept=('zte_api_fast_l3_session_add',), log=(log or (lambda *a: None)))
    cap = {}
    # g_sw_cap 定义在厂商 vmlinux 里；这里伪造 session_add 会读到的几个字段：
    #   +0x004 = eth 口个数, +0x00c.. = 每个以太口的交换端口号,
    #   +0x190 = pon/ptm0/CPU 的端口号, +0x1a4 = "条目带端口" 的能力开关
    gsc = 0x61000000
    m.uc.mem_map(gsc, 0x10000)
    m.uc.mem_write(gsc, b'\x00' * 0x10000)
    patch_sym_ptr(m, 'g_sw_cap', gsc)
    m.wr(gsc + 0x004, 4, struct.pack('<I', 4))
    m.wr(gsc + 0x00c, 8, bytes([5, 6, 7, 8, 9, 10, 11, 12]))
    m.wr(gsc + 0x1a4, 4, struct.pack('<I', 2))    # >1 -> 走"带端口"分支

    def h_tmadd(_n):
        r = r0v(m)
        cap['entry'] = m.rd(r[0], SIZE)
        cap['entry_ptr'] = r[0]
        # 输出 6 个字（session_add 会拷回 desc+0x64）
        m.wr(r[1], 24, bytes([0x11, 0, 0, 0, 0x12, 0, 0, 0, 0x13, 0, 0, 0,
                              0x14, 0, 0, 0, 0x15, 0, 0, 0, 0x16, 0, 0, 0]))
        set_r0(m, 0)
        m.log('   [hook] zte_api_fast_l3_session_add(entry=%#x, out=%#x)' % (r[0], r[1]))

    def h_memzero(_n):
        r = r0v(m)
        m.wr(r[0], r[1], b'\x00' * r[1])
        set_r0(m, r[0])

    m.handlers['zte_api_fast_l3_session_add'] = h_tmadd
    m.handlers['__memzero'] = h_memzero

    names = {}
    for nm in ('wan', 'pon', 'ptm0', 'eth0', 'eth1', 'br-lan', 'CPU', 'ppp0'):
        names[nm] = m.cstr(nm)
    p = m.calloc(len(desc))
    m.wr(p, len(desc), bytes(desc))
    for off, nm in ((0x18, 'eth1'), (0x1c, 'eth1')):
        m.wr(p + off, 4, struct.pack('<I', names[nm]))
    if tag:
        # 打标记之后，只把"必须是指针/枚举"的那几个字节改回合法值，其余保持标记
        m.wr(p + 0x0c, 4, struct.pack('<I', 2))
        m.wr(p + 0x14, 2, b'\x00\x01')     # +0x14=0 (+0x15=1) -> 会话 type 3
        fill_desc(m, p, desc)
    else:
        fill_desc(m, p, desc)
    m.run('sw_acl_l3_hardfast_session_add', (p,), max_steps=400000)
    return cap.get('entry'), cap


# ---------------------------------------------------------------- tm 侧
def run_tm(entry, quiet=True, trace=()):
    """把 168 字节条目喂给 tm.ko，记录所有硬件边界写。"""
    import flowadd
    from flowadd import INTERCEPT
    c = flowadd.Case.__new__(flowadd.Case)
    c.log, c.got, c.quiet, c.extidx = [], [], quiet, 0
    # aclGetExtIdxRuleRamAddrByPort 保持真身：要看端口到底影响什么
    ic = tuple(n for n in INTERCEPT if n != 'aclGetExtIdxRuleRamAddrByPort')
    c.m = m = Mod(TM, intercept=ic, log=(lambda *a: None))
    for n in ic:
        m.handlers[n] = c._h(n)
    regw, regr = [], []

    def h_regw(_n):
        r = r0v(m)
        regw.append((r[0], r[1], r[2]))
        set_r0(m, 0)

    def h_regr(_n):
        r = r0v(m)
        regr.append((r[0], r[1], r[2]))
        if r[0] == 1:
            m.wr(r[1], 4, struct.pack('<I', 1))
        set_r0(m, 0)

    def h_status(_n):
        r = r0v(m)
        m.wr(r[0], 4, struct.pack('<I', 0))
        set_r0(m, 0)

    def h_alloc(_n):
        set_r0(m, m.calloc(0x200, 16))

    def h_memcmp(_n):
        r = r0v(m)
        a, b = m.rd(r[0], r[2]), m.rd(r[1], r[2])
        set_r0(m, 0 if a == b else 1)

    def h_memcpy(_n):
        r = r0v(m)
        m.wr(r[0], r[2], m.rd(r[1], r[2]))
        set_r0(m, r[0])

    def h_strlen(_n):
        r = r0v(m)
        set_r0(m, len(m.rd(r[0], 200).split(b'\x00')[0]))

    def h_fpgar(_n):
        r = r0v(m)
        regr.append(('fpga_read', r[0], 0))
        set_r0(m, 0)

    m.handlers.update({'tmOnuRegWrite': h_regw, 'tmOnuRegRead': h_regr,
                       'fpga_read_reg': h_fpgar, 'fpga_write_reg': h_regw,
                       'cla_get_indirect_rw_status': h_status,
                       'kmem_cache_alloc': h_alloc, '__kmalloc': h_alloc,
                       'kmalloc_order': h_alloc, 'memcmp': h_memcmp,
                       'memcpy': h_memcpy, 'strlen': h_strlen})
    # 三个"环境依赖"的辅助函数：查源 MAC 的子网表、PP 的会话/下一跳 RAM 写。
    # 它们的表/引擎由原厂别处初始化，这里没有；像前几轮 stub 掉 cla_* 一样，把入口改成
    # "mov r0,#0 ; bx lr"（返回成功），只放过它们、其余代码仍然真跑。
    skip = ['findSubnetIdBySrcMac', 'operInfoAdd']
    skip += [s.name for s in m.symtab
             if s['st_info']['type'] == 'STT_FUNC'
             and (s.name.startswith('pp_pm_') or s.name.startswith('pon_pp_set')
                  or s.name.startswith('tm_ipv6table_') or s.name.startswith('sbrg_add'))]
    for nm in skip:
        try:
            fe, _fsz = m.func(nm)
        except KeyError:
            continue
        m.wr(fe, 8, struct.pack('<II', 0xe3a00000, 0xe12fff1e))
    seen = []

    def mk(nm):
        def h(_m, _a):
            seen.append(nm)
        return h
    for nm in trace:
        try:
            fe, _ = m.func(nm)
        except KeyError:
            continue
        m.pokes[fe] = mk(nm)
    flow = m.calloc(SIZE)
    m.wr(flow, SIZE, bytes(entry))
    out = m.calloc(0x40)
    m.wr(out, 0x40, b'\x00' * 0x40)
    l1 = c.sym('.LANCHOR1')
    m.wr(l1 + 0x148, 4, struct.pack('<I', 1))
    ret = m.run('zte_api_fast_l3_session_add', (flow, out, out), max_steps=2000000)
    return {'ret': ret, 'got': c.got, 'regw': regw, 'regr': regr,
            'calls': list(m.calls), 'steps': m.steps, 'flow': flow,
            'log': c.log, 'seen': seen}


def show(res, title=''):
    print('== %s  ret=%r steps=%d' % (title, res['ret'], res['steps']))
    for kind, idx, data in res['got']:
        if kind == 'hash':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(15)]
            print('   hash[%d]  %s' % (idx, ' '.join('%08x' % x for x in w)))
            print('             bytes %s' % data.hex())
        elif kind == 'rule':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(17)]
            print('   rule[%d]  %s' % (idx, ' '.join('%08x' % x for x in w)))
        elif kind == 'index':
            w = [int.from_bytes(data[4 * i:4 * i + 4], 'little') for i in range(5)]
            print('   index[%d] %s' % (idx, ' '.join('%08x' % x for x in w)))
        else:
            print('   %-6s %s %s' % (kind, idx, data.hex()))
    if res.get('seen'):
        print('   调用顺序: ' + ' -> '.join(res['seen']))
    print('   tmOnuRegWrite %d 次:' % len(res['regw']))
    for r in res['regw']:
        print('      reg%d <- %#010x   (idx %d)' % (r[0], r[1], r[2]))


def main(argv):
    cmd = argv[1] if len(argv) > 1 else 'sess'
    if cmd == 'sess':
        tag = len(argv) > 2 and argv[2] == 'tag'
        e, cap = run_switch(build_desc(tag=tag), tag=tag, log=print)
        if e is None:
            print('!! switch.ko 没走到 zte_api_fast_l3_session_add')
            return
        if not tag:
            open(SESS, 'wb').write(e)
        print('== zteHffEntry (%d B)%s' % (len(e), (' 已存 ' + SESS) if not tag else ''))
        for i in range(0, len(e), 16):
            print('   +%#04x  %s' % (i, ' '.join('%02x' % b for b in e[i:i + 16])))
        if tag:
            print('   -- 非零字节的值 = (输入偏移*7+1)&0xff')
    elif cmd == 'hw':
        show(run_tm(open(SESS, 'rb').read(),
                    trace=('tm_acl_fast_add_v4v6', 'tm_acl_fast_add',
                           'aclGetAvailableHashAddr.constprop.16',
                           'tm_acl_get_fastHashRule', 'cla_set_hash_table',
                           'cla_set_extra_index_table',
                           'cla_set_extra_rule_table', 'addFlowOperInfo',
                           'tm_acl_fast_init', 'sbrg_add_ipv6table')),
             'tm.ko 吃下 168 字节条目')
    elif cmd == 'ab':
        off, val = int(argv[2], 0), int(argv[3], 0)
        e = bytearray(open(SESS, 'rb').read())
        e[off] = val
        show(run_tm(bytes(e)), 'A/B entry+%#x = %#x' % (off, val))
    else:
        print(__doc__)


if __name__ == '__main__':
    main(sys.argv)
