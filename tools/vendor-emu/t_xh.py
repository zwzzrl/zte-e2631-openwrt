from flowadd import Case
from unicorn.arm_const import UC_ARM_REG_SP

def run(typ, idx, tags=True):
    rs = {o: bytes([o & 0x7f]) for o in range(0x60, 0x80)} if tags else {}
    c = Case(typ=typ, rs=rs)
    m = c.m
    text = m.sec_base[m.symtab[m.sym_index('tm_acl_fast_add_v4v6')]['st_shndx']]
    m.pokes[text + 0x4d350] = lambda mm, a: mm.wr(
        mm.uc.reg_read(UC_ARM_REG_SP) + 16, 4, idx)
    c.ret = m.run('tm_add_acl_flow_rule', (c.flow, c.flow + 192, c.scratch), max_steps=3000000)
    print('== type %d idx %d ret %r' % (typ, idx, c.ret))
    for kind, slot, data in c.got:
        if kind == 'hash':
            print('   hash slot %-5d %s' % (slot, data.hex()))
        elif kind == 'xhash':
            print('   xhash args %s' % (slot,))
            print('         struct60 %s' % data.hex())
            print('         bytes16-23 %s' % data[16:24].hex())
        elif kind == 'index':
            print('   index[%s] %s' % (slot, data.hex()))
        else:
            print('   %s[%s] %s' % (kind, slot, data.hex()))

run(5, 10, tags=False)
run(5, 10, tags=True)
