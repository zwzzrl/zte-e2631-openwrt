# tm.ko / switch.ko under an ARM interpreter

Reading the vendor's classifier code is not enough to answer "what exactly does the
stock firmware hand the hardware"; these scripts run the code instead.  A 32-bit ARM
`.ko` is a relocatable object, so `km.py` lays its sections out at addresses of our
choosing, applies the relocations from its own `.rel.*` sections, and turns every
undefined (or deliberately intercepted) symbol into a stub whose first instruction is
hooked.  A handler does what the real function would do at that boundary - record a
table write, return a plausible hardware value, zero a buffer - and the emulation
continues.  `unicorn` executes everything else for real, which is why the loops and
multiplications in the vendor's bit packing come out right without being modelled.

    cd tools/vendor-emu
    docker build -t e2631-uni:py312 .
    docker run --rm \
      -v /path/to/vendor-rootfs/kmodule-unsquashfs/tm.ko:/ko/tm.ko:ro \
      -v "$PWD":/w -w /w --entrypoint python3 e2631-uni:py312 t_init.py tm_acl_fast_init

The image is `python:3.12-slim` plus `unicorn` and `pyelftools`.

## Scripts

| file | what it does |
| --- | --- |
| `km.py` | ELF layout, relocations, stubs, memory helpers, `run()` |
| `flowadd.py` | one run of the vendor's flow-add path with a synthetic flow descriptor; records every table setter |
| `t_init.py` | runs an initialiser (default `tm_acl_fast_init`) and prints the index/rule entries it writes |
| `t_tag.py` | tags each descriptor byte with its own offset, so the resulting hash key shows which descriptor byte landed where |
| `t_xh.py` | the same run with the extract index forced to 10 (IPv6), printing the hash entry, the external-hash call and the index re-patch |

`t_init.py` is the self-check: it must reproduce `tm_acl_fast_init`'s rule 144/145 and
rule 2/3 byte for byte, and rule 144 must stay identical to `zx_ppe_v4_rule` in the
driver, which is the pair that has been offloading IPv4 on hardware since v1.0.  If a
change to `km.py` breaks that agreement, the change is wrong, not the driver.

What the runs established is written up in `docs/known-issues.md` (PPE-IPV6-TCP,
finding 4, and the first item under `next`).
