# Known issues

The honest list, in two forms: a machine-readable block for tooling and grep, and one
short section per issue for the human who has to decide whether to care. "Verified" means
it was reproduced on this board with the evidence named; "unverified" means it is a
reasoned suspicion and must not be treated as a fact.

Schema: `id` is stable, `severity` is `blocker | major | minor`, `status` is
`open | mitigated | fixed | wontfix`, `surface` is what you are trying to do, `evidence`
points at something in the repository or at a measurement.

```yaml
- id: WIFI-ASSOC-HARD-HANG
  severity: major
  status: fixed
  surface: wi-fi
  symptom: a client associating stops the whole box - no console output, no ARP, no oops, no AER record
  verified: yes (docs/02 section P; board/ser-*.log)
  workaround: v1.3 auto-loads the radio after boot; if the box dies the pending marker retries the bring-up on the next boot (after 120 s, since 2026-10-25), so one power cycle recovers the router with Wi-Fi
  next: staged experiment plan in task list - one variable per flash cycle
  note: tigaotg (PR#1) verified dual-band APs, SSID/key change and reboot persistence on this hardware; repeated cold starts and association stress were NOT re-run. The PL310 io-read-lock flag is in the DTS since v1.3 (dmesg confirmed live on tigaotg's build)
- id: WIFI-COLD-BIND-HANG
  severity: major
  status: open
  surface: wi-fi
  symptom: the first firmware download after a power cycle succeeds on roughly one attempt in three
  verified: "yes (three cold boots on this exact image: hung, worked, hung)"
  workaround: since 2026-10-25 e2631-wifi-boot auto-heals - a boot that finds the pending marker clears it and retries the bring-up after 120 s instead of skipping the radio, and a bind that fails cleanly (box alive, radio never ready) gets the one documented rebind (e2631-wifi-on 0 again). A hang still costs one power cycle, but recovery no longer needs a manual reboot; deployed on the live unit as /etc/e2631-wifi-boot-hotfix until a flashed build ships the same logic in /sbin
  note: 2026-10-25 EP config-space comparison (live unit vs board/backup/vendor-ep0-config.txt) shows identical LnkCap/LnkCtl/LinkStatus - ASPM off, Gen1 x1, MPS128/MRRS512 on both - so the root cause is not EP configuration; it sits RC/PHY/driver-side, which means kernel work and a flash cycle. DevCtl differs only in the standard Linux error-reporting enable bits (ours 0x291f, vendor 0x2910)
- id: WIFI-OFF-BY-DEFAULT
  severity: minor
  status: fixed
  surface: wi-fi
  symptom: no radio until you run two commands
  verified: yes (v1.2); v1.3 replaces the two gates with START=95 deferred autoboot
  note: v1.3 default is AUTO-LOAD ON (tigaotg's verified sequencing); opt back out with /etc/e2631-wifi-noautoload. First boot provisions WPA2-PSK (key derived from the unit MAC, printed on the console) and sets the root password - never an open AP
- id: HB-CONSOLE-NOISE
  severity: minor
  status: fixed
  surface: console
  symptom: the liveness beacon printed a line to the serial console every 3 s, on every boot, forever
  verified: yes
  workaround: the diagnostic beacon is opt-in since v1.3 - "touch /etc/e2631-hb-enable" restores it
- id: FWSTATE-NEEDS-DEVMEM
  severity: minor
  status: open
  surface: debugging
  symptom: /sbin/e2631-fwstate cannot work in the shipped image ("no /dev/mem")
  verified: yes, on the box
  next: build/wsl/build-regprobe.sh produces the module that reads the same register
- id: DEVMEM-DROPPED
  severity: major
  status: open
  surface: kernel config
  symptom: CONFIG_DEVMEM=y is requested in both generic and the target fragment, and the built .config comes out "not set"
  verified: yes (out/gen1/config-6.18:2314 vs generic/config-6.18:1494 and sanechips/config-6.18:147)
  note: why the request is dropped is still unexplained - do not re-add a fragment edit on the assumption it will work
- id: WIFI-2G4-UNVERIFIED
  severity: minor
  status: fixed
  surface: wi-fi
  symptom: the 5G AP is confirmed from a phone; the 2.4G radio had never been verified on air
  verified: "yes, by tigaotg (PR#1 handoff): phy0-ap0 2.4 GHz ch1 and phy1-ap0 5 GHz ch36 both reached AP-ENABLED and broadcast without logging in"
- id: ETH-THROUGHPUT-CEILING
  severity: major
  status: fixed
  surface: ethernet
  symptom: link trains at 1 Gbps; v1.2 plateaued at ~305/190 Mbit/s with no offload at all
  verified: yes - v1.3 measured 395 Mbps SUSTAINED via OPPO-mall CDN (32 browser threads), CPU 7%, WAN software counter flat (PPE forwards without touching the CPU stack); the earlier "200M ceiling" across three CDNs and 48 connections was Tencent-CDN per-IP rate limiting, not the router
  cause: v1.2 shipped no flow offload (missing kmod-nft-offload + the firewall flag) although the gmac driver has had the full PPE hardware path (zx279128s-ppe.c, TC_SETUP_FT) all along; WiFi-originated flows are architecturally rejected by the PPE (ingress must be a switch port) and stay on the CPU software path (~200M) - the vendor reaches gigabit over WiFi only via its private IDM/NPP engines on the 4.1 kernel
  workaround: none needed for wired; do NOT use /proc/net/dev WAN counters to measure PPE traffic (hardware-forwarded packets are invisible there) - read ppe_flows in /sys/kernel/debug/92000000.ethernet/dma_state instead
  next: find an unthrottled server to probe the true PPE ceiling (395M was still climbing when stopped); PPPoE uplinks may not take the hardware path (sibling ZX279133 hit exactly that)
- id: PPE-IPV6-TCP
  severity: major
  status: mitigated
  surface: ethernet + ipv6
  symptom: LAN client -> router over IPv6 TCP connects in ~1 ms and then no payload ever arrives (0 bytes, times out); IPv4 over the same port and ICMPv6 are fine, so it reads as a random hang. Also stalls IPv6 TCP through the router to the internet.
  verified: yes - 2026-10-27 on a freshly flashed image: flow_offloading=1 + flow_offloading_hw=1 -> connect ok, 0 bytes in 6 s; hw=0 -> HTTP/1.0 200 OK in 1 ms; 0/0 -> same. Reproduced on ports 80/22/53 with both a ULA and a fe80 destination (fw4 stop/start flipped it every time)
  cause: |
    flow_offloading_hw=1 makes fw4 bind an nft flowtable, and zx_eth_setup_tc()/FLOW_BLOCK_BIND calls
    zx_ppe_start() unconditionally - that programs the classifier with the IPv4-ONLY index/rule tables
    (zx_ppe_v4_index/zx_ppe_v4_rule; zx_ppe_flow_add() rejects anything that is not
    FLOW_DISSECTOR_KEY_IPV4_ADDRS/ETH_P_IP), then sets ppe_active=1, which moves our MACs from the
    trap-DMAC table into the ONU "routed" table and clears the trap-DMAC entries. The IPv6 ethertype
    trap is already off whenever sw_members != 0 (three LAN ports in br-lan => always), so with
    ppe_active=1 an IPv6 frame addressed to the router has NO path left to the CPU: the SYN gets
    through by another trap, the payload and the replies do not. hw=0 restores the MACs to the
    trap-DMAC table and everything works, which is why the switch is what flips it
    (vendor comparison, 2026-10-27: the stock firmware has an IPv6-capable classifier - per-class NPU
    caps npu_ipv6_l3tcp{up,down}_cap, IPv6 SNAT/DNAT RAM, NPTv6, plus a local-IPv6-address register
    file at ONU reg 19 (0xe3028, 4x32 bit) and ONU S/D IPv6 at reg 23 (0xe303f, 8x32 bit) - and it
    never uses mainline nf_flow_table/TC_SETUP_FT at all: those strings appear 0 times in the stock
    vmlinux. The bug is ours, not inherited)
  workaround: |
    fixed in the driver on 2026-10-27 (option (a) below, then tested on hardware): zx_update_traps()
    now never lifts the IPv6 ethertype trap, so IPv6 always keeps the CPU path while IPv4 keeps the
    PPE. With that in place the image default went back to flow_offloading=1 + flow_offloading_hw=1
    (etc/uci-defaults/99_e2631_flowoffload) - wired IPv4 acceleration is on out of the box again and
    IPv6 no longer stalls. Verified on a freshly flashed slot 1 (serial 13, kernel 6.18.55): with
    1 / 1 the ULA/LL TCP reachability that used to hang now behaves, IPv4 SSH/ICMP unaffected, and
    ppe_active=1 with ppe_flows tracking the IPv4 flowtable. IPv6 is NOT hardware-accelerated: it is
    switched in hardware but its L3 forwarding stays on the CPU
  next: |
    the classifier's IPv6 half is written and in the tree but disabled (module parameter ipv6=1,
    off by default): IPv6 extract index 10, rule 0x99 (19 windows, hash_len 19), 15-word hash
    entries, and zx_ppe_flow_add() accepting FLOW_DISSECTOR_KEY_IPV6_ADDRS. Do not just flip it on:
    on 2026-10-27 a build that programmed those tables hung the box - SSH worked for ~30 s after
    fw4 bound the flowtable (ppe_flows=0, no IPv6 flow installed), then the first IPv6 frame from a
    LAN host wedged the switch: IPv4 and IPv6 both dead, ARP/NDP unanswered, link still up, no
    self-recovery in 8 minutes, fixed only by a power cycle (the second hang of the day; the config
    had persisted flow_offloading=1, so it also failed at boot until the VM's IPv6 traffic was
    stopped). What the vendor firmware does have, recovered 2026-10-27 by emulating tm.ko:
      * rule table: IPv4 fast-path templates at rule 144 (0x90) and 145 (0x91) - upstream/downstream
        differ only in word 14 bit 25 (0x00700000 vs 0x02700000); IPv6 templates at rule 2/3 with
        the same contract and hash_len 19 (0x01300000);
      * extract index table: aclRamInit fills index 1..8 with field ids 16..143, index 9 with
        144..159 and index 10 with 0..15 (byte 18 = 0x19 for 1..8, 0x15 for 9/10);
      * tm_acl_fast_init only *enables* them: index 9 |= 1 (v4 fast) and index 10 |= 4 (v6 fast).
        aclGetExtIdxRuleRamAddrByPort() returns 9 or 10 from two globals that
        tm_acl_fast_add_v4v6() sets from the session's L3 type;
      * tm_acl_fast_add_v4v6() builds the 45-byte hash key (__memzero 45) from the 60-byte entry
        that tm_acl_get_fastHashRule() fills, packing each masked window into the key as
        (hw & 0x7f) << 1 | (hw >> 15) - i.e. MSB-first within the window, which does NOT match what
        our driver does (LSB-first put_bits) although our IPv4 code is proven on hardware, so those
        templates are probably not the path the stock firmware runs in practice;
      * the vendor's own IPv6 template is inconsistent (window 5 reads offset 0, the last two
        windows both read offset 40, window 0 masks 0xaf00, window 11 has type 0), which is the
        other reason not to trust it.
    The driver's IPv6 constants were replaced on 2026-10-28 with the values above instead of the
    extrapolated ones: index 10 = extract ids 0..15, word 4 = 0x00150154 (the proven v4 entry
    0x00150151 with its own enable bit 0 swapped for the v6 bit 2, hash-bank bit 8 and byte 18 = 0x15
    kept), and the rule words taken verbatim from rule 2. Five words changed: 0xe1828406 ->
    0x01828406, 0x94264889 -> 0x14264889, 0xfff0000a -> 0xfaf0000a, 0xdb6db6db -> 0x1b6db6db, plus
    the whole index entry (0xa3a2a1a0.. with ids 160..175 -> 0x03020100.. with ids 0..15). What makes
    that extraction trustworthy despite the caveats above is the cross-check: the very same function
    programs rule 144/145 for IPv4 and those entries are byte-identical to zx_ppe_v4_rule, which runs
    on hardware. It is still unproven on hardware - ipv6=1 has never been booted with these values,
    and each wrong step costs a power cycle. The image that carries them was flashed to slot 1 on
    2026-10-28 03:16 through the vendor web upgrade page (real 8B8.000 header template, package
    sha256 c413e123...): both gates passed, the box came up in 21 s, the first 0xE60000 bytes of
    slot 1 match the package body per-MiB, and with ipv6=0 the defaults behave exactly as before
    (dmesg "hardware flow offload enabled", IPv6 ping6 and TCP 22/80 all fine). What remains
    unverified is only the ipv6=1 path itself.
    To finish this properly, either dump the vendor's live tables from a running stock firmware, or
    build the IPv6 key/window layout from scratch and bisect it one table at a time on hardware
    (program only the extract index first, then the rule, then a flow) - each wrong step costs a
    power cycle.
  docs: 01-文档/BUILD-NOTES.md 2026-10-27 (both entries); 06-分析与对比/原厂硬件加速驱动-IPv6-对比-2026-10-27.md
- id: USERDATA-EXTROOT
  severity: minor
  status: mitigated
  surface: storage
  symptom: the 47 MiB window behind both image slots (0x3f00000..0x6e00000) is unused while the internal jffs2 overlay holds only ~8 MiB
  verified: yes - the window is free in every vendor version checked (all four upgrade packages 4B4..8B8 declare identical slot tables ending at 0x3f00000; the stock kernel declares nothing beyond 0x2fc0000; the programmer dump shows 0x3cc0000..0x72c0000 erased); tigaotg ran a UBIFS extroot on it for weeks
  workaround: "v1.3 ships the partition plus a gated extroot: preinit attaches the UBI only, /etc/init.d/e2631-userdata enables the fstab mount only after reading a real UBIFS superblock (magic 0x06101831), and /sbin/e2631-userdata-init formats the volume only under an explicit E2631_CONFIRM"
  note: boot can never format the volume (a read-write mount of a blank volume would auto-format it; the gate keeps that out of the boot path). The JFFS2-to-UBIFS migration prototype from PR#1 is NOT shipped - whiteouts/xattrs/power-fail atomicity are unsolved there
- id: PACK-NEEDS-VENDOR-TEMPLATE
  severity: major
  status: fixed
  surface: packaging
  symptom: no flashable package without one official E2631 upgrade package as a version-header template
  verified: yes
  note: "fixed by commit 8dfcf4d (2026-10-15): seed/vendor-verhdr-8b8.bin (532 B) ships in-repo as the default; packing with it reproduced the published v1.2 byte for byte. A fully synthesized header remains untested against the vendor page - see the human-readable section below"
- id: ROOTFS-SLOT-ASSUMPTION
  severity: blocker
  status: mitigated
  surface: flashing
  symptom: "an image written to SLOT 0 panics at boot (\"VFS: Unable to mount root fs on /dev/mtdblock7\") and reboot-loops"
  verified: "yes (2026-10-24 serial log, v1.3 first flash: cspboot table showed our kernel@0x700000[0x3fffe0] fs@0xb00000 in slot 0 with serial 0x31 vs vendor@slot1 0x30; the kernel's rootfs partition was hard-wired to the slot-1 window 0x2700000, so mtd7 read encrypted vendor bytes from slot 1)"
  workaround: the zx279128s-slotfix early fixup (v1.3) reads cspboot's bootpara block at 0x40007000, validates its magic quadruple, scans for a (kernel,fs) u32 pair with fs-kernel == 0x400000 (a delta only our packer's 0x3fffe0 kernel padding produces; vendor images are 0x340000) and repoints the rootfs/slot1-header partitions at the slot that actually booted. Any miss keeps the built-in slot-1 layout - the old behavior, so no new failure mode. The scan logic passed 7/7 offline tests reproducing all three observed boot states; on-device confirmation pending the v1.3 flash
  note: the assumption was architectural since v1.0 (the DTS comment said "OpenWrt is installed into slot 1"); October's 16/16 flashes all wrote slot 1 because the vendor system was always the active one in slot 0 - the precondition broke for the first time on 2026-10-24 after serial churn. Our own writer was already guarded (e2631-fwslot flash refuses slot 0 without --allow-slot0). e2631-fwslot switch still assumes ours is in slot 1 - verify with "fwslot info" when the image lives in slot 0
- id: ZTE-MESH-CONTROLLER
  severity: minor
  status: open
  surface: mesh
  symptom: "the ZTE mesh controller (vendored cnjn/zte-mesh: the factory multiapd in a private IPC/network/mount namespace) has never run on THIS board"
  verified: no - upstream verified it end to end on the SR1010/ZX279133 with real ZTE child routers (Z506); on E2631 only the packaging is verified
  workaround: "the service ships DISABLED; enabling it pushes the configured Wi-Fi profile to every ZTE child router on br-lan (LuCI: Network -> ZTE Mesh)"
  note: "adaptations from upstream: @TARGET_sanechips instead of @TARGET_zte, PKGARCH dropped (the ARM32 vendor binary is native on this Cortex-A9; on the ZX279133 it runs under AArch64 compat), config default enabled 0 and SSID E2631 instead of ImmortalWrt. Kernel prereqs already on: CONFIG_SYSVIPC, CONFIG_IPC_NS, NET_SCHED; kmod-veth/kmod-sched-core/ip-full come in as deps. The runtime tarball carries proprietary ZTE binaries (marked nonshared), same redistribution posture as the vendor-materials release"
- id: FACTORY-SSID-FROM-TAG
  severity: minor
  status: fixed
  surface: wi-fi
  symptom: the first-boot SSID was a hard-coded "E2631" instead of this unit's own factory name
  verified: "the tag pipeline was validated against the real dump (build/board/backup/mtd2-tag.bin: pair \"ChinaNet-ygckk9\"/\"ChinaNet-ygckk9-5G\", plus what looks like the factory key pair \"dthe3524\" twice); on-device run pending"
  workaround: none needed - e2631-wifi-boot scans the tag's first page for a printable run S whose S+"-5G" sibling also exists (self-validating, no byte offsets pinned), falling back to "E2631"
  note: the tag also carries SN, carrier code (1630@CTCC on this unit) and model "ZXHN E2631"; the factory-key pair was deliberately NOT auto-used - confirm it against the label sticker first
- id: LED-DAEMON-NOOP
  severity: major
  status: fixed
  surface: leds
  symptom: the LED daemon had never driven a single LED since day one
  verified: yes - find_led matched "*:<colour>" but the LED names are "<colour>:<function>" (red:status, green:wan-online), so $red/$green/$blue were always empty
  workaround: fixed in v1.3 - names matched on either side, /etc/diag.sh overlay replaces the stock boot-blink (600 ms slow) and stops the done state lighting green unconditionally
  note: "v1.3 semantics (user-specified): boot <45s red slow blink -> online green steady / offline red steady -> red-to-green on connect, 2 s default-route poll"
- id: NETUNE-RESET-ON-RELOAD
  severity: minor
  status: fixed
  surface: network
  symptom: rps_cpus flips back to 1 (CPU0) around link events despite the boot-time netune
  verified: "yes - watched live through a wan bounce: ifup sets 2, fw4's procd-triggered reload resets it to 1 one to three seconds later"
  workaround: v1.3 ships /sbin/e2631-netune-apply called from the iface hotplug (fast path) and every minute from cron (backstop - last writer wins)
- id: JSMIN-CHANGES-HTDOCS-MD5
  severity: minor
  status: fixed
  surface: build
  symptom: "the md5 of a JS view in the image never matches the source copy, and grep for source-formatted strings finds nothing - looks exactly like a stale build cache"
  verified: "yes - chased as a cache bug for four hours (apk purge + staging purge + full target clean + three rebuilds); the md5 difference was CONFIG_LUCI_JSMIN=y all along (feeds/luci/luci.mk compresses every htdocs JS during package install)"
  workaround: "compare by minification-safe substrings (expect:{ not expect: {), never by md5 against the source; the fixes WERE in the image every time"
- id: SPI-NAND-PART-COVERAGE
  severity: major
  status: wontfix
  surface: flashing another board
  symptom: a unit whose flash is not in the upstream spi-nand table gets no rootfs, so it stays on the vendor slot (not bricked)
  verified: yes for the DS35Q1GA/GB 128/256 Mbit batch (docs/02 section S.8, S.9)
  note: adaptation was evaluated on 2026-10-15 and deliberately not done; the method to do it is documented
- id: APPLY-COPIES-NOT-SYNC
  severity: minor
  status: mitigated
  surface: build
  symptom: a patch deleted from overlay/ stays in the tree and keeps being applied
  verified: yes (two abandoned Dosilicon SPI-NAND patches survived their own deletion)
  workaround: apply.sh now lists tree patches that are in neither the base commit nor overlay/
- id: GENERIC-FRAGMENT-WINS
  severity: minor
  status: fixed
  surface: kernel config
  symptom: asking for a symbol in target/linux/sanechips/config-6.18 changes nothing when generic names it as "is not set"
  verified: yes (softlockup / hung-task detectors)
  workaround: flip it where it is decided; the witness is the built kernel's own .config, never a fragment
- id: WINDOWS-CHECKOUT-TRAPS
  severity: minor
  status: mitigated
  surface: build
  symptom: a CRLF working copy breaks kconfig ("unknown statement \"If\""); a case-insensitive filesystem collapses path pairs that differ only in case
  verified: yes (13 pairs were silently merged once)
  workaround: .gitattributes forces LF; build from `git archive HEAD | tar -x`; compare trees by blob hash, not by name
- id: SUPGRADE-NEEDS-A-BROWSER
  severity: minor
  status: wontfix
  surface: flashing
  symptom: the vendor page's login is RSA + sha256(plaintext + salt) with a CSRF pre-authorisation, so route A cannot be scripted here
  verified: yes (docs/02 section E)
  workaround: log in once in a browser and pick the file; or use host/stage.mjs over the vendor telnet service
- id: DELIVERY-SOURCE-MUST-MATCH-THE-NIC
  severity: minor
  status: mitigated
  surface: flashing
  symptom: host/stage.mjs serves the file from the PC address it is told, and the box can only reach the interface actually wired to it
  verified: yes (the PC's LAN address moved 192.168.5.5 -> 192.168.5.17 when the adapter changed)
  workaround: pass --pc with the address of the interface that has the router on it
- id: NO-CI
  severity: minor
  status: wontfix
  surface: contributing
  symptom: nothing runs on a push; the build happens on someone's machine
  note: deliberate (the owner publishes releases by hand). make check is the entry point that replaced the workflow's gates
```

## WIFI-ASSOC-HARD-HANG

The one that decides everything else about Wi-Fi. A station completing association stops
the machine at a level that produces no report: the console goes silent mid-line, ARP
stops answering, and the only recovery is a power cycle. It is not a kernel panic, not an
oops, not an AER event, and it is not new - the three published packages are byte-identical
in the driver, the firmware blob and the bring-up script (`docs/02` section S.7), so no
release introduced it and no release can be blamed for it.

What has been ruled out with measurements: MPS/MRRS config-space differences from the
vendor system (identical), the proprietary-versus-mt76 driver question (the stock
`mt7916.ko` is MTK's own driver, not mt76, so a like-for-like comparison is not
available), and the firmware blob itself (the vendor's 2025-01-03 blob does not fix it).
What is still standing as a hypothesis: something about the first PCIe transaction the
chip issues on behalf of a newly associated station, on a bus where this SoC's L2 cache
has to be serialized for register reads. The staged plan for chasing it is in the task
list, and it is written to be one variable per flash cycle, because a flash cycle here
costs a power cycle if it goes wrong.

Practical consequence, and the reason the image behaves the way it does: the driver is not
loaded at boot. Bringing the radio up is two commands, run with the serial console
attached, and a failed experiment then costs a reboot rather than the box.

## DEVMEM-DROPPED and the instrumentation that depends on it

`CONFIG_DEVMEM` is asked for twice - in `target/linux/generic/config-6.18` and in the
target fragment - and the built config says it is not set. Nothing in the tree asks for
`n`, and `drivers/char/Kconfig` has it as a plain `bool` defaulting to on. The mechanism
that drops it is still unidentified, which is why this is listed as an open issue rather
than a solved one: the honest state is "we do not know, and a fragment edit written on the
assumption that it will work is a waste of a build".

The consequence is that `/sbin/e2631-fwstate`, which reads the MT7916's `MT_TOP_MISC`
register to answer "what is the firmware doing right now", cannot run in the shipped
image - it fails with `no /dev/mem`. `wsl/build-regprobe.sh` builds a module against
exactly the kernel this kit ships, and that module is the replacement: it reads the same
register from inside the kernel, where the address space is available.

## PACK-NEEDS-VENDOR-TEMPLATE

The packer copies the version header from a real package and rewrites the geometry and CRC
fields. That is enough to produce a file the vendor web page accepts, and it has been
accepted - but it means a stranger cannot pack without obtaining a vendor binary first,
which this repository will not provide or link.

Two things are worth knowing if that blocks you. The format is not secret: every field of
four official packages was decoded by recomputing CRC1 and CRC2 (`docs/01` section 2), and
`tools/make_fixture.mjs` already builds a self-consistent header from nothing, which is
what the slot tool's tests run against. So the synthetic route is plausible. What has not
been tested is whether the *page* accepts it: `upgradeKey1/2` (`0x1351/0x5211`) were the
same in all four packages, but the page also compares product and hardware strings, and we
have no data on what it refuses. If you try it, the failure is a rejected upload, not a
bricked board - and `tools/verify_pkg.mjs` will tell you what your header looks like before
you spend the attempt.

## SPI-NAND-PART-COVERAGE

Whether a given unit boots our image depends on the flash part, not on the board: the
kernel matches SPI-NAND on (manufacturer, device) ID, and an unknown ID means the MTD
device never appears, so there is no rootfs and the box falls back to the vendor slot.
For the DS35Q1GA / DS35Q1GB batch the comparison was done end to end (`docs/02` sections
S.8, S.9): the vendor's own table has parts ours lacks, and adapting them is possible but
each part needs its own OOB layout and ECC request - `ECCREQ(4,512)` with 64-byte OOB on
one, `ECCREQ(8,512)` with 128-byte OOB on the sibling, and getting that wrong is a
silently uncorrectable page. The decision on 2026-10-15 was to stop there. The method to
resume it - carve the vendor kernel's flash table out of the package and diff it against
upstream - is scripted in `wsl/vendor-flash-table.sh`.

## VENDOR-RUNTIME-TABLES - 判定：死路（2026-10-28）

目的：拿到原厂 PPE 分类器的运行时 index/rule/hash 表，用它替掉驱动里那份"外推版" v6 表。

手段：把改过的**原厂 rootfs** 写进原厂**非活动槽**，启动后让盒子自己把
`/proc/zte/verinfo/versionstates` 发回宿主机。一共刷了四轮（v1/v2/v3 是追加新版本节点；
v5 是**原地替换** `/etc/inittab` 那个 88 字节的不压缩节点：同 ino、同 version、同 offset、
同长度，只重算 data_crc/node_crc/hdr_crc），并做了一次真断电重启。

结果：**零回调** —— 监听日志里没有任何来自盒子的请求，23/2323 端口全关，抓包零出站；
UpTime 单调增长（没有重启循环），WebUI 响应正常（也没有 respawn 风暴）。

2026-10-28 用回到 slot1 的 OpenWrt 直接读 flash 取证：

| 读的是什么 | 结果 |
| --- | --- |
| 槽 A 版本头 `/dev/mtd5`+0x1440000 | `V1.0.0.8B8.9201`、`fs_crc=0x65c12a61`、`serial=19`、`slotStart=0x700000`、`isNew=1` |
| 槽 A rootfs 17 MiB 回读 | crc32 `0x65c12a61`，与我们的 `patched-v5.cipher.bin` **逐字节相同** |
| 槽 B 版本头 `/dev/mtd9`（OpenWrt 包） | `slotStart=0x2300000`、`serial=20`、`isNew=1` —— 升级器自己盖的章，完全正确 |

⇒ 那次断电重启启动的**就是我们的 rootfs**（serial 19 最大），我们的 `/etc/inittab` 就在里面却没执行。
于是"刷错槽/选错槽"这条解释被证伪，包结构、CRC1/CRC2、fs_crc、槽头盖章也全部排除。

剩下的候选只有两个，都需要**原厂 shell 或串口**才能证实，而这台机器没有串口访问路径：

1. 原厂内核的 `/` 来自**另一份明文副本**（升级器写了密文之后由别的东西解密到别处）；
2. 它的 JFFS2 读路径**忽略我们新写的节点** —— 最可疑的是 NAND cleanmarker/OOB
   （见 `tools/PROBE.md` 的 F1：cleanmarker 必须落在数据区，因为 on-die ECC 每次编程都重算页 ECC）。

**任何"故意放一个坏镜像再启动去看看它用不用"的判定实验都不能做**：原厂选中的槽起不来
不会自动换槽（`docs/01` §3），一次失手就是没有 shell、没有 WebUI、只能 TTL 救的砖。

## SLOT0-WAS-REWRITTEN - 已按备份恢复（2026-10-28）

原厂升级器写的是**非活动槽**，而活动槽随着每次新镜像赢得 serial 竞争在 A/B 之间轮换。
所以到 2026-10-28，槽 A 里躺的是我们改过的原厂镜像（v5），**不是出厂镜像**。
此前任何"slot0 原厂完好"的说法都是错的 —— 那是按"我们只让它写 slot1"的意图推的，不是读出来的。

已恢复：把 `工作文件/03-固件备份/router-flash/mtd5_slot0.bin`
（sha256 `4c7a83f6bcb48225440788a5aeaddba36ee135278163047311aa1ff8595fabcb`，
头 `V1.0.0.8B8.8000` / `serial=4` / `slotStart=0x700000`，CRC1/CRC2/fs_crc 三处自洽）
整段写回 slot0，回读整个 `0x1c00000` 分区，sha256 与备份一致。

教训：**别用"我打算写哪个槽"去推断"实际写了哪个槽"**，原厂这条路是按它自己的开机快照挑的。

## PACK-BUILD-FIELD-OVERRUN - 已修（2026-10-28）

打包脚本把 16 字节的 build 串按 **0x40 字节**写进 `+0xa8`，越界把
`+0xbc`/`+0xc0`（原厂 `0xffffffff`）、`+0xc4`（flash 容量 `0x08000000`）、
`+0xc8`（块对齐 `0x10000`）清成了 0 —— v1、v2、v3 三个包都带着这个坏头刷进了槽。

已修：build 只写 16 字节，并加"头部差异只允许出现在白名单字段"的硬断言
（版本串 `0x10..0x30`、`fs_crc 0x48`、build `0xa8..0xb8`、serial `0x1f4`、CRC1、CRC2）。

顺带记一笔：`host/verify_pkg.mjs` 只检查了 `+0xb8 integrity`，`+0xc4/+0xc8` 这两格
值得在那里也加一条断言。

## SOFTWAREVER-IS-A-FLASH-RECORD

原厂 WebUI 的 `SoftwareVer`/`VerDate` 在我们**没有重启**的情况下就变成了新包里的串
（v3 的 9100 → v5 的 9201 都是刷完、还没重启时就变了）。所以它反映的是"flash 上最新的镜像"，
**不是"这次启动的是哪个镜像"**。拿它当启动证据会直接把结论带偏。

## VENDOR-WEB-SHELL - 判定：死路（2026-10-28）

目的：在原厂 8B8 上拿一个 root shell，用它 dump PPE 运行时表、或改 `TelnetCfg`。
（`VENDOR-RUNTIME-TABLES` 之后换的路子：不再刷镜像，只打它自己的 WebUI。）

手段与结果都在 `192.168.5.1` 实测，本轮**零 flash 写入**，slot0 仍是原厂镜像：

| 入口 | 实测结果 |
| --- | --- |
| `ping_diag_data` 的 `Host` | 白名单：`;`、`|`、`$()`、反引号、`127.0.0.1`、`0.0.0.0` 全被 `IF_ERRORPARAM=Host`（`-257`）拒；`localhost`、`192.168.5.50` 通过。而且诊断**从不执行**：`DiagnosticsState` 恒为 `Requested`、`PingAck` 恒为 `NULL` —— 既没回显也没副作用 |
| `do_restore_usrcfg` 上传的文件名 | 14 种载荷（`;`、`$(curl …)`、`${IFS}`、反引号、换行、`|`、`&`、`../`、`%00`）全部 `IF_ERRORID=414`，宿主机 `:8000` 监听**零命中**。`414` 与文件名无关（纯 `cfg.bin` 同样 `414`），是内容校验 |
| `do_download_usercfg` 配置备份 | HTTP 200 + `application/octet-stream`，**0 字节** |
| 网络面 | 80/443/8085 开，23/22/2323 全关；131 个 thinklua tag 里没有命令/调试类 |

协议要点（后面还会用到）：令牌**轮换** —— 每个响应的 `x_xsrf_token` 头就是下一个请求要带的令牌；
登录前用 `login_token_json` 的 `_sessionToken`；登录 JSON 里的 `sess_token` 不是令牌，是前端加密 key。
在 URL 上追加 `&_=<ts>` 会 404。令牌错 → `<IF_ERRORSTR>页面过期，请刷新后重试</IF_ERRORSTR>`（`-1452`）。

## VENDOR-CFG-CRYPTO - 默认密钥已逆出，但那份 usercfg 不是它封的（2026-10-28）

已确证（`libhardcode.so` 反汇编 + `.rodata` 静态数据，`decry@0xf44`、`CspHardCodeDecryDefaultKey@0x130c`）：

- `_HCGetVerVal()`：`fopen("/etc/hardcode")` → 读 `0x40` 字节 → CR/LF 清 0 → 字符串。实测内容 `E2631_QD_MESH`（13 B）。
- 默认密钥 = `oss_snprintf_s(buf, 0x100, "%s%s%s", "0x1351", "0x5211", hardcode)` → 25 字符
  `0x13510x5211E2631_QD_MESH`（`%s%s%s`@`.rodata+0x17df`、`0x1351`@`+0x17e6`、`0x5211`@`+0x17ed`）。
- `decry(in, out, key)`：`memset(keybuf, 0, 0x21)` → `snprintf(keybuf, 0x21, "%s", key)` →
  按 `0x400` 块读、每 `0x10` 字节一次 `AES_set_decrypt_key(keybuf, 0x100, &ctx)` + `AES_decrypt(in, out, &ctx)`
  ⇒ **AES-256-ECB**，密钥 32 B = 25 字符 + 7 个 `0x00`（缓冲区先清零，`snprintf` 只写 26 B）。

但拿它解 `usercfg` 分区那份 25,704 B（头 `01020304 00000004` + BE 长度 `0x641c`/`0x6420`，
负载自 `0x48` 起、25632 B），在偏移 `0x44/0x48/0x4c/0x50` × AES-128/192/256 下**全是乱码**
（可打印率 `0.379`）⇒ 那份文件不是默认密钥封的。

剩下的线索：`CspHardCodeParamGet`（按参数名取 per-device 密钥）、`/etc/enhardcodefile`（9376 B）、
`/etc/enwebdhardcodefile`（1712 B）。配置还原这个入口要用起来，得先把 per-device 密钥破了；
在那之前它只是"能上传、会被内容校验拒"的空壳。
