# Architecture - how a clone of this repo becomes a system that boots on the board

Read this before changing anything structural. BUILD.md is the procedure; README.md
is the status; `docs/01-...md` and `docs/02-...md` are the evidence. This file is the
map: which piece exists, what it is responsible for, and what breaks if you move it.

## The three repositories

| | |
|---|---|
| **this one** (`zte-e2631-openwrt`) | the whole port: overlay, board scripts, packer, verifiers, evidence documents. Active |
| `e2631-mainline-zx279128s` | the same SoC on **pristine** upstream Linux: upstream v6.18.38 plus 6 modified files and 4 new ones. Where the "this SoC really boots mainline" evidence comes from. No U-Boot/TFTP path any more - that is tagged `v0.1.1` |
| `openwrt-sanechips-mirror` | the base tree: OpenWrt with the `sanechips` target and the `zte/*` patch series. This kit is an overlay on one commit of it, not a fork |

## Repository map

    apply.sh          overlay -> base tree; idempotent; every edit is witnessed
    BUILD.md          the procedure, including the failure modes
    Makefile          check / bootstrap / build / package - wrappers over scripts/
    scripts/          env.sh (paths), bootstrap.sh, build.sh, check.sh
    overlay/          the port itself, in OpenWrt's own directory shape
      target/linux/sanechips/
        config-6.18           kernel symbols this board needs
        image/Makefile.e2631  the device definition, the slot kernel/rootfs recipe
        dts/zx279128s-zte-zxhn-e2631.dts   the board: PHYs, LEDs, partitions, PCIe
        patches-6.18/         029/030/031 - JFFS2 in-data markers, gated PCIe retrain
        base-files/           /sbin/e2631-*, etc/init.d/e2631-*, board.d/02_network
      package/firmware/e2631-fwslot/       the A/B slot tool (C, static, soft-float)
      package/kernel/mt76/                 the Wi-Fi patches, generated not hand-written
    wsl/              the build path, the patch generators, the witnesses, the probes
                      (grouped in wsl/INDEX.md; named for where they used to run)
    tools/            mkfactory.mjs (packer), make_fixture.mjs, verify_img.mjs,
                      dts_lint.mjs, PROBE.md (the unknowns, and which closed)
    host/             Windows-side tooling: release.mjs (pack+screens),
                      verify_pkg.mjs, publish.mjs, telnet/stage tooling
    board/            board-side scripts and the vendor evidence we do publish
                      (ser-*.log serial captures); dumps and harvests stay ignored
    seed/e2631.config the .config the build starts from
    docs/             01 facts, 02 the working log, refs-local/ the earlier notes

## From source to something that boots

    base tree @ pinned SHA
      -> apply.sh        overlay copy; image/Makefile hook; mt7915e AutoProbe removed;
                         VFP/NEON/THUMBEE off; CONFIG_CMDLINE* forced; lockup
                         detectors on (in generic too); root= counted from the DTS
      -> make world      OpenWrt builds kernel 6.18.55 + the target + LuCI
      -> collect.sh      kernel / root / e2631.dtb / config-6.18 / e2631-fwslot
      -> release.mjs     mkfactory pack -> verify_pkg (independent re-derivation)
                         -> verify_img (boot readiness) -> sha256
      -> supgrade.html or e2631-fwslot flash   writes the INACTIVE slot
      -> cspboot         picks the valid slot with the higher serial

The `.config` in an artifact is whichever phase wrote it last, so `release.mjs` also
reads the base tree's fragments as a second witness when the tree is reachable
(`E2631_TREE`). That second witness is not cosmetic: `INITRAMFS_SOURCE` in the collected
config belongs to the *rescue* kernel, and without the fragments the shipped kernel
would be judged by the wrong file.

## The flash geometry, which is the part you cannot guess

Two whole-system A/B slots in SPI-NAND: slot 0 at `0x700000`, slot 1 at `0x2300000`,
each with a span of `0x1c00000` (28 MiB). The vendor bootloader (cspboot) validates only
**CRC1** of the 512-byte version header, and CRC1 covers `[0x00:0xa4]` - which stops
before the serial field - so re-stamping a serial never invalidates a slot. That is what
makes `e2631-fwslot switch` (the way back to the vendor system) a 4-byte write.

A package is:

    [16 outer magic][4 hdr_ofs=0][512 verhdr][body ... ]
    body = [32B slot marker][kernel extent][0x00 pad to 128K]
           [rootfs extent][0xFF pad to 128K][version header block]

Two rules, both read off the serial console rather than inferred:

* cspboot **derives** the rootfs position as `0x14 + 0x200 + ceil(kSize / erasesize)`
  and ignores the header's own rootfs offset. Declaring a padded `kSize` is therefore the
  only way to pin the rootfs anywhere other than immediately after the kernel - which is
  what puts it at slot + `0x400000`, where the board DTS says it lives.
* `rSize` must stop at the end of the squashfs rounded to an erase block, because the
  kernel turns the *rest* of the rootfs partition into the writable overlay
  (`rootfs_data`). A checksum window over that tail means every config change breaks the
  boot.

Only the stock rootfs is AES-128-ECB at rest, and the decryption lives in the vendor
kernel's MTD read path - a mainline kernel reads plain bytes, so our squashfs needs no
crypto. A package carries no bootloader (`bootIncl == 0`): the boot chain is never
touched by an upgrade, which is also why a bad image can be walked away from.

## The image content model

The vendor U-Boot hands the kernel its own `bootargs` (`root=/dev/mtdblock8
rootfstype=jffs2 init=/bin/sh ...`), which name the *vendor* partition table and would
replace PID 1 with a shell. So the command line is compiled in with
`CONFIG_CMDLINE_FORCE=y`, and `apply.sh` **counts** the `rootfs` label's position in the
board DTS to derive `root=/dev/mtdblockN` - remembered numbers go stale when a partition
row moves. `CONFIG_MTD_PARTITIONED_MASTER` being off is what makes the numbering start
at mtd0 with the first child partition.

Three things are hard constraints from the silicon, not preferences: no VFP (so
soft-float userspace or every binary dies with SIGILL), JFFS2 clean markers in the data
area rather than OOB (on-die ECC regenerates page ECC on every program, so a page written
twice reads back uncorrectable - patches 029/030), and USB left disabled (probing an
unpopulated controller reboots this SoC).

## The Ethernet driver, and the one part of it that is a module

`zx279128s-main.c` (DMA engine, the five netdevices, phylink, the traps) and
`zx279128s-switch.c` (hardware forwarding between the ports of one Linux bridge) are
built into the kernel. The packet processor - the classifier index/rule tables, the two
classifier hash banks, the rewrite tables and the IPv6 tables - is package
`kmod-e2631-ppe`, because that is the part that keeps being worked on: linked into the
built-in driver, every edit under `target/linux/sanechips/files/...` invalidates the
kernel's prepared stamp and rebuilds ~1900 objects (45 minutes on this machine); as a
package it is one `.ko`.

What crosses the boundary, and what deliberately does not:

* The **hooks table** (`zx279128s-ppe.h`) is stored in the built-in driver, not in the
  module. Every call snapshots it and takes a module reference, so an unload cannot pull
  code out from under a call in progress; with no engine registered the call reports
  `-EOPNOTSUPP`, which is exactly "this flow stays on the software path".
* The **flow table** (`adapter->ppe_flows`) and its `rhashtable_params` belong to the
  core, because the table is initialised at probe - long before kmodloader runs.
* **`ndo_setup_tc` and the `flow_block` callbacks stay in the core.** They hand the TC
  core a function pointer that must outlive an unload: the callbacks are the trampolines,
  the classifier work behind them is the module's. An unload therefore leaves TC with
  valid callbacks that refuse, not with dangling ones.
* The module is **autoloaded from `/etc/init.d/boot`**, before `/etc/rc.d/S19firewall`
  binds the nft flowtable through `ndo_setup_tc`; a bind that arrives with no engine
  registered is refused and never retried, so the order matters.
* Reloading the module does **not** need a firewall restart: the TC blocks outlive the
  unload, and the module's init asks the core (`zx_ppe_has_bound_blocks()`) whether a
  flowtable is still bound and re-programs the hardware if so. What a reload does lose is
  the flow entries themselves - the hashtable is flushed on stop - so connections that
  were offloaded finish on the CPU path. There is no counter of bindings anywhere: the
  driver list the core fills is the truth, because a counter cannot survive a ruleset
  reload (bind new, then unbind old) or an unload (see PPE-BIND-ACCOUNTING).

## Traps, and the one thing that made IPv6 offload impossible

The switch sends a frame to the CPU when a trap matches it, and a trapped frame is
*diverted*: it never reaches the classifier, so it can never be offloaded. That is not
obvious from either side - the flowtable hands the driver a flow, the driver programs a
correct hash entry, and nothing ever hits it. Measured by trapping IPv4 the same way the
IPv6 trap used to be: 167,000 forwarded packets, every one of them through the CPU, with
the identical flow offloading (18 packets) as soon as the trap is lifted. `BUILD.md` §10
has the recipe.

So `zx_update_traps()` lifts the IPv6 ethertype trap only when the engine reports that its
IPv6 tables are in the hardware (`zx_ppe_set_v6_offload()`, called by the module after the
table writes returned 0). With no tables an un-trapped IPv6 frame would have nowhere to
go - our MACs live in the ONU "routed" table once offload is active, not in the trap
table - and that is exactly the stall of v1.4 (`docs/known-issues.md`, PPE-IPV6-TCP).

`PPE-IPV6-TCP` has a second trap for the reader, and the first version of this paragraph
got it backwards: extract index **10**, the one the stock firmware uses, is the only index
that makes the classifier look at an IPv6 frame at all. The old sweep - "did the CPU stop
seeing the packets" - read index 10 as dead because every packet went through the CPU
there, while the other indices looked good because the frames were being dropped
unnoticed. With a far end that counts what arrives, index 10 forwards 2,800 of 2,800
packets and index 11 forwards none. The driver uses 10.

That leaves the third finding, which is why this section is not called "the fix": with the
trap lifted and the extract in place, the classifier hands IPv6 to the CPU, the driver
writes the hash entry, and the hardware still never matches it - not with the entry in all
384 slots at once, not with any window offset type, not with a key of 7 to 19 halfwords.
The 45-byte IPv6 key this driver builds is not the hardware's key; the vendor computes it
in software from per-family descriptors (`tm_acl_fast_add_v4v6()` in tm.ko), and until
that is reproduced IPv6 stays on the CPU path. It works there - that is the shipped
configuration (`ipv6=0`, trap on) and the measured one (`ipv6=1` forwards just as well,
only without a fast path).

## Wi-Fi, and why it is off by default

The MT7916 is a PCIe card at `0000:01:00.0` (`14c3:7906`) whose second function
(`0001:01:00.0`, `14c3:790a`) is the ext-HIF. **One PCI function registers two phys** -
phy0 is Band 1 (2.4G), phy1 is Band 2 (5G) - and netifd distinguishes them with a `+N`
suffix on the device path, the same shape `/usr/share/hostap/wifi-detect.uc writes. Give
both radios the same path and they fight over one phy; give the second one the wrong
suffix and the 2.4G radio is handed the 5G device.

The radio stays silent for two independent reasons: `kmod-mt76`'s `AutoProbe` entry was
removed (probing both functions in parallel during boot has the main function read the
other's BAR, which has been seen to wedge this SoC), and a generated wifi-iface comes up
`disabled` unless `board.json` carries a `wlan.defaults` section. Bringing it up is two
commands on purpose - `/sbin/e2631-wifi-on 0` then `/sbin/e2631-wifi-ap` - so the driver
load stays a deliberate act, with the console attached. `/sbin/e2631-wifi-ap` is the
part that was hard: it parses `uci show wireless` **by section name** (the generator
writes `radioN`/`default_radioN`, not `@wifi-iface[n]`), counts phys to build the `+N`
path, and does a `wifi down; sleep 1; wifi up` cycle because a bare `wifi reload` leaves
the interface in a state the driver does not act on.

## Recovery, which is the reason the design looks over-cautious

A hang on this board is silent: no console output, no ARP, no oops, no AER record, and
only a power cycle returns it. So every path is built to keep an way in:

* the vendor slot is never written, and `e2631-fwslot switch` + `reboot` walks back to it
  - also reachable from failsafe, since the tool needs only `/dev/mtd*` and the dtb's slot
  partitions;
* the 600 s first-boot trial revert is **opt-in** (`touch /etc/e2631-trial-arm`), because
  auto-reverting once threw away a boot that was worth studying;
* softlockup + hung-task detectors and serial SysRq are compiled in (`apply.sh`), so the
  kernel can speak for itself next time;
* `/dev/mem` is unreachable in the shipped config (the request is dropped somewhere that
  is still unexplained - `docs/02-...md` section P), so register reads that would need it
  go through the `regprobe.ko` module instead (`wsl/build-regprobe.sh`).

## The verification stack

| screen | question it answers | when |
|---|---|---|
| `wsl/lint-kit.sh` / `wsl/sh-lint.sh` | does every script that goes on the device parse under busybox ash, with CRLF stripped | `make check` |
| `wsl/check-kit.sh` | does the slot tool compile with `-Werror`, accept a good package, reject one altered byte | `make check` |
| `tools/dts_lint.mjs` | do the board file's includes and `&labels` resolve (dtc otherwise costs a whole build to say no) | `make check` |
| `host/verify_pkg.mjs` | is the packed file internally consistent: geometry, CRC1/CRC2, uImage, the rootfs/overlay split, keys and product against the template | inside pack |
| `tools/verify_img.mjs` | will it boot: `root=` vs the dtb's partition order, drivers built in, forced cmdline, USB/watchdog off, soft-float, package list | inside pack |
| `host/publish.mjs` | refuses to publish a file whose first 16 bytes are not the vendor magic, and compares GitHub's asset digest with the local sha256 | by hand |

Two independent screens exist because "the bootloader accepts it" and "the system runs"
are different claims, and the second one cannot be tested without spending a flash cycle.

## Where the evidence lives

`docs/01-...md` is the fact table (A-level = recomputed from bytes, B-level = read once,
C-level = inferred). `docs/02-...md` is the working log, section by section, including the
sections that record a falsified assumption - those are the ones worth reading before
re-trying something. `docs/known-issues.md` is the machine-readable honest list.
`board/ser-*.log` are serial captures, committed on purpose: they are the primary witness
for every "it died at this second" conclusion. `out/` is not in git - it is 1.1 GB of
build directories, packed images, logs and one-off experiments, all either regenerable or
named inline in `docs/`. A path written as `out/xxx.log` means the workspace, not this
repository.
