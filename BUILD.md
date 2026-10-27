# BUILD - from a clone of this repository to a package that flashes

This is the whole supported path. README.md explains *why* the port is shaped like
this; this file is the procedure, and every step says what it does and what it
means when it fails.

Everything below runs on one Linux box (a VM, a container, WSL, or bare metal).
The build was developed in WSL2 Ubuntu on Windows, which is why `wsl/` is spelled
that way - those scripts are the same steps, plus the workarounds for one machine's
network. If you are on Linux, use `scripts/`.

```
base tree (pinned commit)  ->  apply.sh overlay  ->  make world  ->  collect
                           ->  mkfactory pack (header template ships in seed/)  ->  verify x2  ->  flash
```

## 1. What you need

| | |
|---|---|
| OS | any Linux with a working `make`/`gcc`; developed on Ubuntu 22.04 (WSL2) |
| Disk | **~12 GB** for the tree: the measured total here is 11 GB, of which `build_dir` is 8.4 GB, `dl` 573 MB, `feeds` 279 MB, `.git` 16 MB |
| RAM / cores | 4 GB and 2 cores will finish; measured here on 12 cores / 8 GB |
| Time | a from-scratch `make world` was about 40 minutes; with ccache warm and only the target rebuilt, 6 1/2 minutes (measured 2026-10-15 16:18:43 -> 16:25:12) |
| Packages | OpenWrt's own build-dependency set for Debian/Ubuntu (on 22.04: `build-essential clang flex bison g++ gawk gcc gettext libncurses-dev make python3 rsync swig unzip zlib1g-dev file wget`), plus `git`, `curl`, `busybox` for the ash gate, and `ccache` if you want fast rebuilds. `scripts/bootstrap.sh` checks `git make gcc g++ file` and stops with the package name if one is missing |
| Node | **20.15 or newer** (22 LTS recommended). The packer and the screens use `zlib.crc32`, which does not exist before that. `scripts/build.sh` checks it; by hand: `node -p "typeof require('node:zlib').crc32"` must print `function` |
| A serial console | 115200 8N1 on the board's UART. Not optional in practice: the failure modes on this SoC are silent, and `docs/known-issues.md` explains why no other channel survives them |

## 2. Two inputs this repository cannot give you

**The base tree.** This is an overlay, not a fork of OpenWrt. It is keyed to one
commit of our `sanechips` branch, because `apply.sh` edits the target's config
fragments and image Makefile by pattern:

    E2631_BASE_REPO   https://github.com/Zlion-Y/openwrt-sanechips-mirror
    E2631_BASE_BRANCH pr-zte-zxhn-h3600
    E2631_BASE_SHA    db8b8d4130457dbe46558a7cea872dd2dae539e6

`scripts/bootstrap.sh` clones and checks it out for you. Kernel 6.18.55 comes from
that tree; the `zte/*` patches in its `target/linux/sanechips/patches-6.18` are what
make the SoC exist at all. If you want to build against a different commit, apply the
overlay, then read `apply.sh`'s output: it says which commit it thinks it is editing.

**The version-header template.** `tools/mkfactory.mjs pack` copies a 512-byte version
header out of a template and rewrites only the geometry and CRC fields, so the template
has to supply everything the packer does not compute:

    +0x08/+0x0c  upgradeKey1/Key2      +0x6c  product string
    +0x4c..+0x60 slot A/B base + span  +0x8c/+0x98 bootloader inclusion + U-Boot CRC
    +0x110       hw version            +0x1e0..+0x1f8 slot table, serial, isNew

**That header now ships here: `seed/vendor-verhdr-8b8.bin`, 532 bytes, and it is the
default.** No `--template`, no vendor firmware on your disk, and packing still produces a
package the vendor flash page accepts. It is the outer header plus the version header cut
out of `V1.0.0.8B8.8000`; [seed/README.md](seed/README.md) records the source file, its
sha256, and the measurement that matters - packing a build directory twice, once with this
slice and once with the full 21 MB package, gives **the same bytes**
(`02b35106…`, which is also the published `v1.2`).

You still want a real official package in two cases:

* `mkfactory selftest` / `info` read the *payload* the header points at - they are the
  proof that the geometry model matches the vendor's, and they say so plainly when handed
  a header-only file.
* your hardware revision needs a different `upgradeKey1/Key2` pair. The keys are not
  stable across vendor versions: `6B5`/`7B5`/`8B8` all carry `0x1351/0x5211`, but the old
  `4B4` package carries `0x238/0x2e`. Re-cut the slice from a 6B5-or-newer package
  (`head -c 532 official.bin > my-template.bin`) and `host/verify_pkg.mjs` will check the
  packed header against it.

We have not found a copy of an uploaded package left behind on the box - the vendor
flasher is copied into `/var` during an upgrade (`docs/02-...md` section F), the firmware
file itself was not located - and `e2631-fwslot info -j` prints a slot's header *fields*,
not a package you can reuse. The format is fully decoded in
`docs/01-情报汇总与事实核验-2026-10-13.md` section 2 - every field of four official
packages verified by recomputing CRC1 and CRC2 - and `tools/make_fixture.mjs` synthesizes
a self-consistent header from nothing so the slot tool is testable without any vendor
binary. **What has not been tested** is whether the vendor web page accepts a package
built from a *synthesized* header: shipped packages were built from a real one, and that
is the only combination proven to flash.

Override the template only if you need to:

    E2631_TEMPLATE=<path to the official .bin or your own slice>   # absolute, or relative to E2631_WORK

## 3. Configure the paths (optional)

Every machine-specific path has a default that works on a normal clone, and can be
overridden by an environment variable or by `.env` (untracked; copy `.env.example`).
`scripts/env.sh` is the single definition; `host/release.mjs` reads the same `.env`.

| variable | default | meaning |
|---|---|---|
| `E2631_KIT` | found from the script's own location | this repository |
| `E2631_WORK` | parent of the kit | where `out/` lives; relative values in `.env` resolve against it |
| `E2631_OUT` | `$E2631_WORK/out` | build directories, logs, packages |
| `E2631_TREE` | `$HOME/openwrt` | the base tree being built |
| `E2631_BASE_*` | the pinned repo/branch/SHA above | what bootstrap clones |
| `E2631_TEMPLATE` | `seed/vendor-verhdr-8b8.bin` | the 532-byte version-header template; see section 2 |
| `E2631_PROXY` | unset | http(s) proxy for the download phases |
| `E2631_GNU_MIRROR` | unset | move a fast GNU mirror to the head of `@GNU` |
| `E2631_SEGMENTED` | unset | fetch distfiles as parallel byte ranges (see section 7) |

## 4. Check the clone

    make check            # or: bash scripts/check.sh

Five gates, each reporting PASS / FAIL / SKIP with the reason:

| gate | what it proves |
|---|---|
| `bash -n` | every helper in the kit parses |
| `node --check` | every tool parses |
| busybox ash | the scripts that get put on the *device* parse under the device's shell, with CRLF stripped - a bashism or a `\r` here only shows up on the board |
| slot tool | `e2631-fwslot.c` compiles with `-Wall -Wextra -Werror`, then accepts a synthesized package and rejects the same package with one kernel byte changed. This is the parser that runs on the device before anything is written to flash |
| board dts | `#include`s and every `&label` in the board file resolve against the base tree's patch series (needs the tree, so it SKIPs before bootstrap) |
| `scripts/secret-scan.sh` | no credential shapes in any tracked file, and the evidence files that must stay ignored (the vendor harvest, the config export, the token helper) are not tracked. It exists because a scrub that searches for a value **literally** missed nine files that carry the tcl-escaped form of the same password |

The two gates that read a finished artifact - `host/verify_pkg.mjs` (re-derives the
geometry independently) and `tools/verify_img.mjs` (boot readiness) - run inside the
pack step, so they need a build first.

## 5. Bootstrap, then build

    make bootstrap        # clone + overlay + feeds + seed config, with witnesses
    make build            # make world, collect, pack, verify

`bootstrap` stops rather than guessing: it dies if the tree is not the base tree, if
the checkout is at another commit *and* has changes that might be yours, if a feed
index is missing, or if the seed's device selection does not survive `defconfig`.

`build` refuses to start a forty-minute job on inputs it can see are missing, so the
errors you get are the useful ones: no base tree, no `.config`, overlay not applied,
device not selected, no `node`, `node` too old for `zlib.crc32`, a template path that
does not exist (the default lives in `seed/`). It writes `$E2631_OUT/build-<timestamp>/world.log`, then collects into the
same directory (`kernel`, `root`, `e2631.dtb`, `config-6.18`, `e2631-fwslot`) and packs
`$E2631_OUT/e2631-openwrt-<timestamp>.bin`.

Run it twice in a row to see the loop you will actually live in: the second pass is
`apply.sh` + `make world` again, and `apply.sh` is idempotent by design.

## 6. Flash it

Two routes, both used on real hardware. The upgrade package is the same file for both.

**A. The vendor web page** (verified 2026-10-14, this is the acceptance route). Boot the
box into the vendor system, log into its admin UI (it lives on `192.168.5.3`), open the
hidden firmware-upgrade page - the writeups call it `supgrade.html`, and its form POSTs
the multipart field `VersionUpload` to `/?_type=hiddenData&_tag=firmware_upgrade` on
port 8085 - select the package, submit. It writes the **inactive** slot, raises the slot
header's serial itself (33 -> 34 was measured) and reboots into the new system. The
vendor slot stays byte-identical, which is what makes route B possible afterwards.

**B. From the vendor system with our own tool.** `node host/stage.mjs put` copies
`e2631-fwslot` and the package onto the box over its telnet service and runs `info` +
`check` - read-only. `node host/stage.mjs flash --yes` then writes the inactive slot.
This needs the vendor telnet root password in `ZTE_TELNET_PW`; it is not in this
repository and `host/stage.mjs` refuses to run without it.

**Getting back.** From OpenWrt: `e2631-fwslot switch` then `reboot` re-stamps the other
slot's header with a higher serial and nothing else - the vendor firmware body is never
touched. This also works from failsafe, because the tool only needs `/dev/mtd*` and the
slot partitions from the dtb. There is no `sysupgrade` path that overwrites the vendor
slot, by design.

**After first boot.** The image comes up with `br-lan` at `192.168.1.1/24` serving DHCP
(measured on the console: `lan1: Link is Up - 1Gbps/Full`, then `ssh root@192.168.1.1`,
LuCI on `http://192.168.1.1/`, Argon theme). The WAN port and the three LAN ports are
one WAN + `lan1 lan2 lan3`, all four on the SoC's internal PHYs.

## 7. When the build itself is the problem

The network is the constraint on most boxes, not the CPU. `wsl/build.sh` and README.md
document what was measured here: kernel.org 73 KB/s, `sources.cdn.openwrt.org`
25 KB/s, `sources.openwrt.org` 14 KB/s, `musl.libc.org` nothing at all inside a 12 s
probe - and the cap is per connection, not per host.

    E2631_SEGMENTED=1 make build     # fetch each distfile as 8-12 parallel byte ranges
    E2631_GNU_MIRROR=https://... make build
    E2631_PROXY=http://host:port make build

`wsl/segdl.sh` is the segmented fetcher, wired in through `DOWNLOAD_TOOL_CUSTOM` - the
only hook `scripts/download.pl` offers, because its `select_tool()` prefers curl and
never reaches aria2c. A 147 MiB kernel tarball came in at ~288 KB/s aggregate this way
and its sha256 matched the one in `download.pl`'s own command line, so the result is
byte-exact rather than merely fast. It falls back to a single connection for a server
that answers a range request with `200` and the whole file (`mirrors.huaweicloud.com`
does that), otherwise every worker would receive the same bytes.

`CURL_OPTIONS="--speed-limit 20000 --speed-time 20"` is set by default in
`scripts/build.sh`: OpenWrt gives curl a connect timeout but no transfer watchdog, so a
mirror that accepts the connection and sends nothing blocks forever instead of rotating.
A finished toolchain once sat five minutes on the 1 MiB musl tarball.

`make download` is deliberately not run up front: it walks every package in the tree,
selected or not, and was pulling `broadcom-wl` and `libwebsockets` while the packages
this image needs waited.

Windows side, if you build under WSL or edit from Windows: a **CRLF working copy breaks
kconfig** (`unknown statement "If"`), and a case-insensitive filesystem silently
collapses path pairs that differ only in case. Check files out with LF
(`.gitattributes` is set) and never compare a tree by directory names alone.

## 8. Making your own change

The overlay is the whole port: `overlay/target/linux/sanechips/` (config fragments,
`image/Makefile.e2631`, the board DTS, `base-files/`), `overlay/package/` (the slot
tool, the mt76 patch set), `seed/e2631.config`. Add your device or your patch there and
`make build` again - `apply.sh` copies `overlay/` into the tree, so nothing needs to be
edited in the tree by hand.

Four rules that exist because breaking them cost hours:

* **Never hand-write a patch.** Generate it against the source the build actually uses:
  `wsl/gen-retrain-patch.sh`, `wsl/gen-efuse-patch.sh`. A hand-written patch here failed
  with "malformed patch at line 37"; the generators prove `APPLIED == GENERATED`.
* **`apply.sh` copies, it does not sync.** A patch you delete from `overlay/` stays in
  the tree and keeps being applied - that is how two abandoned SPI-NAND patches went on
  building into the kernel. `apply.sh` now lists those orphans (files in the tree's patch
  series that are in neither the base commit nor `overlay/`); remove them yourself, since
  they may be your work rather than stale. Deleted *base-files* are the same trap with a
  worse smell: the removed `e2631-wanport` init script, hotplug hook and udhcpc nop script
  survived a re-apply and stayed in the rootfs. Anything you delete from `overlay/` that
  may already sit in somebody's tree goes into `overlay-removed.txt`, which `apply.sh`
  deletes on every run (missing file = skipped).
* **Ask for a symbol where it is actually decided.** `target/linux/generic/config-6.18`
  wins over `target/linux/sanechips/config-6.18` for symbols generic names explicitly as
  "is not set" - that is why the lockup detectors are flipped in both files. The witness
  is the built kernel's own `.config` (`$E2631_OUT/build-*/config-6.18`), never a fragment.
* **A screen is not a boot.** Passing the bootloader's CRC gates only means the bytes are
  accepted. `tools/verify_img.mjs` checks the things that stop a boot instead: `root=`
  against the partition order the dtb really creates, flash/squashfs/decompressor built
  in, the forced command line beating the vendor bootargs, USB and watchdog staying
  disabled, soft-float, and the package list. Do not flash an artifact it refuses - a slot
  that hangs at startup does not fall back by itself.

`docs/architecture.md` maps the pieces and `docs/known-issues.md` is the machine-readable
honest list, including what is still broken on real hardware.

## 9. The fast loop, for the one part of the driver that is a module

Everything under `overlay/target/linux/sanechips/files/` is copied into the kernel tree
*before* the patches, so any edit there invalidates the kernel's prepared stamp and
rebuilds the whole kernel - ~1900 objects, 45 minutes on the machine this was written on.
That is the wrong loop for the packet processor (the classifier tables, the two hash
banks, the rewrite tables, the IPv6 tables), which is exactly the part that keeps being
worked on. It is therefore package `overlay/package/kernel/kmod-e2631-ppe` instead of the
third object of the built-in driver, and a change there is:

    make package/kernel/kmod-e2631-ppe/compile     # seconds, not minutes
    scp -O build_dir/*/linux-*/kmod-e2631-ppe/zx279128s-ppe.ko root@192.168.1.1:/tmp/
    ssh root@192.168.1.1 'rmmod zx279128s_ppe; insmod /tmp/zx279128s-ppe.ko'

(Use `rmmod`, not `modprobe -r`: busybox modprobe has no `-r`.  Use `scp -O`: this
dropbear has no sftp-server.)  No firewall restart is needed: module init attaches to the
flowtable that is already bound, so the tables come back and the first flow re-offloads
within a second.  `bash 04-构建环境与脚本/ppe-kmod-inner.sh` inside the builder container
does the compile half of this.

A bind that arrives while no engine is registered is still refused and never retried
(`ndo_setup_tc` runs at firewall start); that is why the module's init has to re-attach,
and why `docs/architecture.md` keeps the `flow_block` callbacks on the built-in side - TC
holds those pointers across an unload.

Two things that will otherwise cost you a wasted build:

* The shared headers (`zx279128s-eth.h`, `zx279128s-ppe.h`) still live in the kernel tree,
  because the built-in driver includes them; `src/Makefile` adds
  `-I$(srctree)/drivers/net/ethernet/zte` instead of keeping a second copy. Editing a
  *header* is still a full kernel rebuild - only `.c` files in the package are cheap.
* The link set is changed by `patches-6.18/033`, not by a `Makefile` in `files/`: a file
  that patch 021 creates as a new file cannot also be shipped by the overlay, since the
  overlay is copied first and 021 would then fail with "already exists".

**`DEVICE_PACKAGES` in `image/Makefile.e2631` does not reach `.config` on its own.** It is
expanded into `tmp/.targetinfo`, which is only regenerated when something named `Makefile`
or `*.mk` under `target/linux` changes (`include/scan.mk`, `SCAN_DEPS=... image/Makefile
image/*.mk`); `image/Makefile.e2631` is neither, so adding a package there leaves
`make defconfig` reporting success with the package still "not set". Select it in
`seed/e2631.config` as well - that is what the seed is for (the same trap is why
`kmod-mt7916-firmware` has an explicit line there). Confirm with `grep
CONFIG_PACKAGE_kmod-e2631-ppe .config`, never with the image Makefile.


## 10. Measuring the packet processor without a second host

Proving that a flow is forwarded by the switch and not by the CPU needs (a) a flow the
flowtable will offload and (b) a hardware-side signal. Both are cheaper than they look.

**(a) One UDP packet already creates an offloadable flow.** `nft_flow_offload_eval()`
requires an established TCP connection (it looks for FIN/RST and calls
`nf_conntrack_tcp_established()`), but for UDP it only requires the conntrack entry to be
confirmed, so a single `sendto()` that the router forwards makes fw4 add the flow, and the
driver program a hash entry. No peer has to be listening, and no second host is needed
beyond the far end of the route.

**(b) A hardware-forwarded packet never touches the CPU DMA rings.** Read
`tx_submitted` from `/sys/kernel/debug/92000000.ethernet/dma_state` before and after a
burst: an offloaded flow leaves it flat (about 20 packets of setup), a software path adds
one per packet. That is a finer signal than throughput, and it also says *which* family
was offloaded, because the same file carries `ppe_flows` and the module can log per-entry
hits (`insmod ... debug=1` prints every offloaded flow and the first hardware hit of each
hash entry).

**(c) A flat CPU counter does not prove the hardware forwarded anything.** A dropped frame
also keeps the CPU out, and that ambiguity produced a wrong "IPv6 offload works" reading.
Make the far end something that counts: `ip -6 route add fd88::/64 via <sender> dev br-lan`
on the router, `ip -6 route replace fd88::/64 via <router> dev ens34` on the sender, and the
frames come back to the sender's own NIC, where a 30-line AF_PACKET script counts them
(.scratch/ppe-v6/cnt.py, non-zero = forwarded, 0 = dropped; the whole bench
harness that drove these runs is next to it). A negative result then
means something.

Three traps cost a day each before they were found, both worth remembering:

* **A trapped ethertype never reaches the classifier.** With the IPv4 trap set and
  offload otherwise unchanged, a forwarded IPv4 flow stops offloading entirely (167,000
  packets, all through the CPU); with it clear, the same flow offloads (18 packets). So a
  family whose trap is on can never be offloaded, no matter how right its tables are. The
  driver therefore lifts the IPv6 trap only when the engine has its IPv6 tables in the
  hardware (`zx_ppe_set_v6_offload()`). The trap register is also writable at runtime
  (`echo 0 > /sys/module/zx279128s_ppe/parameters/trap_override`), which is what lets a
  flow be created first and then tested with the trap out of the way.
* **Extract index 10 is the only one that works, and reading the sweep backwards hid it.**
  The sweep asked "did the CPU stop seeing the packets", so index 10 looked dead (every
  packet went through the CPU there) while 11 looked good (the packets were being dropped
  without ever being looked at). With a far end that counts (b/c above) the same test says
  the opposite: index 10 forwards 2,800 of 2,800, index 11 forwards 0 of 2,800 and shows
  20 packets of CPU. The driver now uses 10, the stock firmware's choice.
* **The IPv6 hash entry is never matched** - not even with the same entry written into all
  384 slots (`v6_shotgun=1`), with every window offset type 0..7, with key lengths 7..19
  halfwords, or with the index/rule byte swapped. The key layout the driver computes is
  simply not the hardware's; the vendor builds it in software (tm.ko
  `tm_acl_fast_add_v4v6()` + `tm_acl_get_fastHashRule()`) from per-family descriptors, so
  the fix is to reproduce that, see docs/known-issues.md PPE-IPV6-TCP.

**(d) LAN traffic and routed traffic are different questions.** A frame the switch can
bridge never needs the classifier, so an ethertype trap is what decides whether it crosses
the CPU: 2,000 IPv6 UDP frames between two LAN hosts cost 2,018 CPU packets with the IPv6
trap on and 18 with it off. `echo 1 > /sys/module/zx279128s_ppe/parameters/ipv6` flips
that live (tables + trap), `echo 0` puts the trap back.  Do not use ICMPv6 to judge a trap
experiment - it has its own trace/trap path and keeps working either way.

The knobs that made those experiments possible are module parameters, so a wrong guess
costs a reload rather than a kernel rebuild: `ipv6` (writable at runtime, see (d)),
`debug`, `trap_override` (raw value
for the ethertype trap register, `-1` = driver decides; writable while loaded),
`v6_ext`, `v6_entry`, `v6_rule`, `v6_key`, `v6_idxw4`, `v6_hashlen`, `v6_type`, `v6_hwn`,
`local6`/`onu6s`/`onu6d`/`local6_rev` (the vendor's IPv6 address register files)
(halfwords of the key, `-1` = all 19) and `v6_shotgun`/`v6_shot_lo`/`v6_shot_hi` (write an
IPv6 flow into a range of hash slots to separate "wrong key" from "wrong slot"). Everything
except `ipv6` and `debug` exists for diagnosis and can go once the IPv6 path has soaked.
