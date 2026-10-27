from flowadd import Case

def tag_case(typ, lo, hi):
    rs = {}
    for o in range(lo, hi):
        # every descriptor byte carries its own offset as a marker
        rs[o] = bytes([0x80 | (o & 0x3f), o & 0xff])[:1]
    return Case(typ=typ, rs={o: bytes([0x80 | (o & 0x3f)]) if o < 0xc0 else bytes([o & 0x3f]) for o in range(lo, hi)})

def probe(typ, lo, hi):
    c = Case(typ=typ, rs={o: bytes([o & 0x7f]) for o in range(lo, hi)})
    slot, data = c.hash_entry()
    print('== type %d, descriptor %#x..%#x tagged with its own offset, ret %r slot %r'
          % (typ, lo, hi, c.ret, slot))
    if data is None:
        print('   no write, log %s' % c.log[:4])
        return
    print('   entry %s' % data.hex())
    key = data[16:60]
    print('   key bytes (index, value -> source descriptor byte = value):')
    for i, b in enumerate(key):
        if b:
            print('     k%-2d = %#04x  -> desc[%d]' % (i, b, b))

probe(3, 0x60, 0x80)
probe(5, 0x60, 0x80)
probe(5, 0x80, 0xc0)
