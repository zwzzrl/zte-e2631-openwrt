#!/usr/bin/env python3
"""Which functions does a flow-add actually run, per rule type?

The emulator's fast-path script (t_key.py) pokes inside tm_acl_fast_add_v4v6 and
sees it run for every type, IPv6 included - which is why the "13-byte window for
IPv6 too" result needs a second look: if the shipped firmware routes IPv6 flows
through addAclRule()/aclBuildHashAndAclRule() instead, then the entry it wants
is built by other code and tm_acl_fast_add_v4v6 is simply not the v6 path.

    python3 t_v6path.py syms             # list the acl/fast symbols
    python3 t_v6path.py trace            # types 3/5/6 x fast flag 1/0
"""
import re
import sys

from t_poly import Q

WATCH = ('tm_acl_fast_add', 'tm_acl_fast_add_v4v6', 'addAclRule',
         'aclBuildHashAndAclRule', 'tm_acl_fast_init',
         'aclGetExtIdxRuleRamAddrByPort', 'cla_set_hash_table',
         'cla_set_external_hash_table', 'cla_set_extra_rule_table',
         'cla_set_extra_index_table', 'tm_acl_get_fastHashRule')


def trace(typ, fast, idx):
    p = Q(typ, fast=fast, idx=idx)
    order = []
    m = p.m
    for name in WATCH:
        try:
            a = p.sym(name)
        except Exception:
            continue
        m.pokes[a] = (lambda n: (lambda mm, addr: order.append(n)))(name)
    p._rerun()
    slot, entry = p.entry()
    kinds = [k for k, _i, _d in p.got]
    print('== type %d fast %d idx %d -> slot %r kinds %s'
          % (typ, fast, idx, slot, kinds or '-'))
    print('   order %s' % (' > '.join(order) if order else '-'))
    if entry:
        print('   entry %s' % entry.hex())
    for name, bank, key, buf in p.keys:
        print('   %-7s b%d key45 %s' % (name, bank, key.hex()))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'trace'
    if cmd == 'syms':
        p = Q(3)
        for s in p.m.symtab:
            n = s.name
            if re.search(r'acl|fast', n, re.I):
                print('%#x  %s' % (p.m.sec_base[s['st_shndx']] + s['st_value'], n))
    else:
        for t in (3, 5, 6):
            for fast in (1, 0):
                trace(t, fast, 10 if t in (5, 6) else 9)
