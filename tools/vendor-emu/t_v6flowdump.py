#!/usr/bin/env python3
"""把原厂 v6 快转会话真正写进硬件的 60 字节表项原样 dump 出来。

为什么要有这个脚本：`t_extidx.py flow` 只打印槽号，`t_v6extra.py flow` 走的不是会话
添加那条路（那条路对 IPv6 只读 CLA+0x90/0x94 两个寄存器），所以"原厂自己的 v6 快转
表项长什么样"一直没有逐字证据。这里让 switch.ko 造 168 B 会话条目（入端口写 +0x9e），
再让 tm.ko 真身跑 `zte_api_fast_l3_session_add`，只在 `cla_set_hash_table` /
`cla_set_extra_rule_table` / `cla_set_extra_index_table` 处记录参数。

已知局限（2026-10-31，别把它当结论用）：

* 168 字节会话条目里的 IPv6 地址是**掩码编码**（`00 40 03 00 50 ...`），不是 `fd99...`
  原始字节，所以"换目的地址看哪几个字变"这种 A/B 在仿真里做不成——改不到那 16 字节。
* `t_extidx.py flow` 打出来的槽号只能当"idx 参与键"的证据：那条路把 switch.ko 内存里的
  sip/dip 指针喂给另一个 Unicorn 实例，地址读成 0，仿真出来的 key 内容是退化的。

用法（tools/vendor-emu，镜像 e2631-uni:py312，挂 /ko = 原厂 kmodule-unsquashfs）：
    python3 t_v6flowdump.py 2          # 入端口 2 = lan1
    python3 t_v6flowdump.py 0          # 入端口 0 = lan3，用来看 byte16 / 槽怎么跟着变
"""
import os
import struct
import sys

import t_hfsess
import flowadd
from km import Mod, REG0_12
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1

TM = '/ko/tm.ko'


def w16(data):
    return ' '.join('%08x' % v
                    for v in struct.unpack('<%dI' % (len(data) // 4), data))


def r0v(m):
    return [m.uc.reg_read(r) for r in REG0_12]


def set_r0(m, v):
    m.uc.reg_write(UC_ARM_REG_R0, v)


def build_entry(inport):
    res = t_hfsess.run_switch(t_hfsess.build_desc(d14=1), log=lambda *a: None)
    entry = res[0] if isinstance(res, tuple) else res
    if entry is None:
        return None
    e = bytearray(entry)
    e[0x9e] = inport
    return bytes(e)


def run_tm_with_keys(entry):
    return _run_tm(entry, want_keys=True)


def run_tm(entry):
    return _run_tm(entry, want_keys=False)


def _run_tm(entry, want_keys=False):
    c = flowadd.Case.__new__(flowadd.Case)
    c.log, c.got, c.quiet, c.extidx = [], [], True, 0
    ic = tuple(n for n in flowadd.INTERCEPT
               if n != 'aclGetExtIdxRuleRamAddrByPort')
    c.m = m = Mod(TM, intercept=ic, log=lambda *a: None)
    for n in ic:
        m.handlers[n] = c._h(n)
    if os.environ.get('TRACE_HASH'):
        hits = []

        def mk(nm):
            def h(mm, _a):
                hits.append(nm)
            return h

        for idx2, sym in enumerate(m.symtab):
            nm = sym.name
            if not nm or 'ash' not in nm:
                continue
            try:
                m.pokes[m.sym_addr(sym)] = mk(nm)
            except Exception:                            # noqa: BLE001
                pass
        m._hash_hits = hits
    if os.environ.get('TRACE_ARGS'):
        want = ('cla_acl_hash_addr_gen', 'aclGetAvailableHashAddr.constprop.16',
                'tm_acl_get_fastHashRule', 'cla_list_hash_addr_gen',
                'cla_get_hash_poly_config')
        for nm in want:
            i = m.sym_index(nm)
            if i is None:
                continue

            def mk(nm2):
                def h(mm, _a):
                    r = r0v(mm)
                    try:
                        b0 = mm.rd(r[0], 48).hex()
                    except Exception:                    # noqa: BLE001
                        b0 = '?'
                    try:
                        b1 = mm.rd(r[1], 128).hex()
                    except Exception:                    # noqa: BLE001
                        b1 = '?'
                    print('   [%s] r0..r3=%s' % (nm2, ' '.join('%#x' % x for x in r[:4])))
                    print('      @r0: %s' % b0)
                    print('      @r1: %s' % b1)
                return h
            m.pokes[m.sym_addr(m.symtab[i])] = mk(nm)
    if os.environ.get('DUMP_LR'):
        from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_SP
        base = m.handlers['cla_set_hash_table']

        def hooked(_n):
            lr = m.uc.reg_read(UC_ARM_REG_LR)
            r = r0v(m)
            sp = m.uc.reg_read(UC_ARM_REG_SP)
            print('   call cla_set_hash_table lr=%#x r0..r3=%s sp=%#x'
                  % (lr, ' '.join('%#x' % x for x in r[:4]), sp))
            print('     key48 @r1: %s' % m.rd(r[1], 48).hex())
            return base(_n)

        m.handlers['cla_set_hash_table'] = hooked

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
    for nm in ('findSubnetIdBySrcMac', 'operInfoAdd'):
        try:
            fe, _ = m.func(nm)
        except KeyError:
            continue
        m.wr(fe, 8, struct.pack('<II', 0xe3a00000, 0xe12fff1e))
    keys = []
    if want_keys:
        import struct as _st

        def _pk(mm, _a):
            keys.append(mm.rd(mm.uc.reg_read(UC_ARM_REG_R1), 48))
        i2 = m.sym_index('cla_acl_hash_addr_gen')
        if i2 is not None:
            m.pokes[m.sym_addr(m.symtab[i2])] = _pk
    flow = m.calloc(0xa8)
    m.wr(flow, 0xa8, bytes(entry))
    out = m.calloc(0x40)
    l1 = c.sym('.LANCHOR1')
    m.wr(l1 + 0x148, 4, struct.pack('<I', 1))
    ret = m.run('zte_api_fast_l3_session_add', (flow, out, out),
                max_steps=2000000)
    if os.environ.get('TRACE_HASH'):
        seen = []
        for x in getattr(m, '_hash_hits', []):
            if x not in seen:
                seen.append(x)
        print('   hash 相关函数命中:', seen)
    if want_keys:
        return ret, c.got, keys
    return ret, c.got


def main(argv):
    inport = int(argv[1], 0) if len(argv) > 1 else 2
    e = build_entry(inport)
    if e is None:
        print('switch.ko 没走到 session_add')
        return
    ret, got = run_tm(e)
    print('== inport %d 会话添加 ret %r' % (inport, ret))
    if not got:
        print('   （这次会话添加没有写任何表：规则表/索引表/hash 都是空的）')
    for kind, a, data in got:
        if kind == 'hash':
            print('   hash slot %d, %d B:' % (a, len(data)))
            print('     ', w16(data))
            print('      byte16 = %#04x' % data[16])
        elif kind in ('rule', 'index', 'xhash'):
            print('   %s[%r]:' % (kind, a))
            print('     ', w16(data))


if __name__ == '__main__':
    main(sys.argv)
