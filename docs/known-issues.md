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
  status: fixed
  surface: debugging
  symptom: /sbin/e2631-fwstate cannot work in the shipped image ("no /dev/mem")
  verified: yes, on the box
  cause: the same dropped symbol as DEVMEM-DROPPED below - the tool is correct, the kernel had no /dev/mem
  fix: seed/e2631.config carries CONFIG_KERNEL_DEVMEM=y; the build now does produce CONFIG_DEVMEM=y (2026-10-29, checked in the built kernel .config, not assumed). /sbin/e2631-fwstate still needs one on-box run before the issue is closed for real
  next: build/wsl/build-regprobe.sh produces the module that reads the same register
- id: DEVMEM-DROPPED
  severity: major
  status: fixed
  surface: kernel config
  symptom: CONFIG_DEVMEM=y is requested in both generic and the target fragment, and the built .config comes out "not set"
  verified: yes (out/gen1/config-6.18:2314 vs generic/config-6.18:1494 and sanechips/config-6.18:147); mechanism confirmed upstream and the fix verified in a local build on 2026-10-29
  cause: |
    include/kernel-defaults.mk appends every CONFIG_KERNEL_* line of the top-level
    .config to the merged kernel config with the CONFIG_KERNEL_ prefix stripped
    (the package -> kernel passthrough), and the kconfig.pl merge is last-wins.
    config/Config-kernel.in declares KERNEL_DEVMEM with default n, so the top-level
    .config carried "# CONFIG_KERNEL_DEVMEM is not set" and the passthrough appended
    "# CONFIG_DEVMEM is not set" after the fragments' =y. No fragment or patch ever
    asked for n; the package-level option simply won.
  fix: CONFIG_KERNEL_DEVMEM=y in seed/e2631.config - the upstream-sanctioned mechanism, explicitly not another fragment edit (that was the wrong layer all along). Found upstream (d68e3b7) after this file spent a week saying "mechanism unexplained".
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
  verified: yes - 2026-10-27 (the stall, then the IPv4-only workaround), 2026-10-28 (trap cause, extract index, and the hash-entry experiments below, all on this board), 2026-10-29 (the vendor's external hash window decoded and probed: it is boot code, finding 5)
  cause: |
    Three findings, in the order they were proven, and the third one says the entry
    cannot be closed as "fixed":

    1. A trapped ethertype never reaches the classifier.  zx_update_traps() kept the
       IPv6 ethertype trap on in every state (that is the v1.4 fix for the stall
       above), and a trapped frame goes to the CPU instead of being classified first.
       Measured 2026-10-28 by trapping IPv4 the same way: the same forwarded IPv4
       flow that involves the CPU in 18 packets with its trap off sent all 167,000
       packets of the test through the CPU with the trap on.
    2. The IPv6 extract index is the stock firmware's 10, and it is the only one that
       works.  The earlier sweep was read backwards: it asked "did the CPU stop seeing
       the packets", so index 10 looked dead because *every* packet went through the
       CPU there, while 0/1/2/3/8/9/11..15 "passed" because the frames were being
       dropped without ever being looked at.  With a far end that counts what arrives
       (see the note at the end) the picture is unambiguous: index 10 forwards 2,800
       of 2,800 packets of a forwarded IPv6 UDP burst, index 11 forwards 0 of 2,800
       and the CPU sees 20 packets.  So the driver writes 10 now, and a v6_ext other
       than 10 makes IPv6 traffic to our MACs disappear.
    3. The IPv6 hash entry is never matched by the hardware.  With the trap lifted and
       index 10 in place, the classifier recognises IPv6 and hands every packet to the
       CPU, the flowtable creates the flow, the driver writes the hash entry ("offloaded
       an IPv6 flow into hash N" with debug=1) -- and the hit bit never sets, so no
       packet ever takes the fast path.  Six kinds of experiment, all on the same
       forwarded flow, all negative:
         * the computed slot, then the entry written into all 384 slots at once
           (module param v6_shotgun, added for this) -- if the key string were right
           and only the software's slot formula wrong, one of them would hit;
         * the extract index byte inside the entry: rule id 0x99, 10, 11, and 0x98;
         * the window offset type, 0..7 (2 = IPv4, 3 = IPv6 per the v4 entry);
         * the key length, 7 to 19 halfwords (param v6_hwn), same rule hash length;
         * the trap itself: trap on and the same five-tuple re-sent after writing
           trap_override=0, i.e. the flow exists before the frames stop being trapped.
       Conclusion: the hardware's IPv6 key is not the 19-homogeneous-window string the
       driver builds.  Where the real one comes from is now known, see "next".
    4. The vendor's own flow-add path, executed on 2026-10-29 in a Unicorn ARM harness
       (tm.ko loaded with its relocations, hardware access stubbed, every table setter
       recorded), narrows this down a lot: the *table entries we build are not the
       problem*, the missing piece is a write we never make.
         * tm_acl_fast_init() re-emulated from scratch reproduces rule 144/145 (IPv4)
           and rule 2/3 (IPv6) byte for byte, and rules 2/3 are exactly the
           zx_ppe_v6_rule this driver already writes, so the IPv6 rule content is
           no longer a guess.
         * tm_acl_fast_add_v4v6() builds the hash entry as: control words, then the key
           string from byte 16, windows from bit 24, byte 16 = 0x40 | extract index
           (0x49 for IPv4 index 9) - the layout zx_ppe_hash_entry() uses.
         * For an IPv6 flow (extract index 10) the same run also calls
           cla_set_external_hash_table() - a table the driver never writes - and
           re-patches the extract index through EN_V6FAST_EXTIDXTBL(), which is a
           read-modify-write of bits 2..3 in the low half of the index entry's word 4
           (4 = on, 12 = fast).  For an IPv4 flow neither call happens.
         * The extract index itself comes from aclGetExtIdxRuleRamAddrByPort(): 9 for
           one direction and 10 for the other, or else a 24-entry port->index table,
           so "index 10 = the IPv6 extract" is consistent with finding 2.
       What this rules out for IPv6: wrong rule content, wrong window placement in the
       entry, and (now also measured on the board) the rule-table index.  Rule 2, rule
       3, rule 160 (= 10 << 4, what the vendor's software uses as a rule base) and rule
       153, each with extract-index word 4 both 0x00150004 and 0x00150154 and with the
       entry written into all 384 slots, still sent every one of ~28,000 packets of a
       forwarded burst through the CPU (far end counted them all, so nothing was
       dropped).
    5. The external table of finding 4 is *not* table memory on this board, measured
       on 2026-10-29.  cla_set_external_hash_table() builds its destination by
       arithmetic on a constant - "(0x3c40800 + idx) << 6", i.e. 0xF1020000 + 64*idx,
       one entry per 64 bytes - and 0xF1020000 is a *virtual* address.  The vendor
       kernel says what is behind it: vendor-vmlinux.bin carries the board's private
       iotable_init map_desc array (16 {virtual, pfn, length, type} entries at file
       offset 0x606350, ~0xc060e350 once loaded at 0xc0008000), and its entries
       decode to virt 0xF4000000 <- phys 0x92000000 (the switch, 4 MiB), virt
       0xF0E00000 <- phys 0x92000000, the register blocks at phys 0x910/0x930/0x940/
       0x94400000 - and, as the last one,
         [15] virt 0xF1000000  phys 0x00000000  len 0x00520000  type 0 (MT_DEVICE)
       so the window is physical 0x00020000 + 64*idx, a 5 MiB device mapping at the
       bottom of the physical map.  Reading it (module param xprobe, added for this:
       "echo 0x20000 > .../parameters/xprobe" does ioremap+readl on any physical
       address, after first reading the CLA window at 0x9238c000 as a control) gives
       boot code, not a hash table:
         * phys 0x001000, 0x008000, 0x020000, 0x028000, 0x040000 and 0x100000 all
           return the same 16 words - e59ff120 eafffffe x7 | e10f0000 e38000c0
           e121f000 ee100f10 - an ARM exception vector table (ldr pc,[pc,#0x120],
           every unused vector a "b .") followed by a startup stub (mrs/msr cpsr,
           mrc p15 reading the control register).  Six addresses 64 KiB apart
           returning word-for-word the same data means the low window is aliased
           ROM/SRAM, not addressed storage;
         * phys 0x400000 reads 16 zero words;
         * phys 0x51ff00 raises an imprecise external abort ("Unhandled fault:
           imprecise external abort (0x406)"), and 0x20840 did the same to the
           writing shell.  Both survived only because /proc/sys/kernel/panic_on_oops
           was set to 0 for the probe; with the shipped setting that is a reboot.
       Conclusion: on this board the vendor's external-table window is the boot code
       area, and writing 24-byte hash entries at 0x20000+ would overwrite the vector
       table, so the v6 "missing write" of finding 4 is not a write into physical
       0x20000.  What cla_set_external_hash_table() does for a v6 flow in the
       emulator is therefore either dead code carried over from another SoC or a
       fallback that this board never reaches - but it is no longer a candidate for
       why the v6 entry never matches.  What that leaves is the key string itself
       (finding 3) and the EN_V6FAST_EXTIDXTBL read-modify-write of the index
       entry's word 4, which is a write to the *internal* index table we do control.
  workaround: |
    IPv6 *works* in the configuration that ships: ipv6=0 keeps the IPv6 trap on, so
    IPv6 takes the CPU software path exactly as it did before any of this, and that
    path is proven end to end (2,800 of 2,800 packets of a forwarded IPv6 UDP burst
    arrive at the far end; IPv6 traffic to the router itself reaches the stack -
    ICMPv6 4/4, TCP 22/80/53 connect, HTTP 200).
    ipv6=1 is now also safe but still does not make anything faster: it programs the
    v6 tables, lifts the trap and offloads v6 flows into the hash, and the datapath
    stays on the CPU because of finding 3.  Both settings forward 1,800 of 1,800
    packets (2026-10-28, measured after the index went back to 10), so ipv6=1 is a
    "correct but no faster" switch, not a trap.
    zx_update_traps() only lifts the IPv6 trap when the module reports its v6 tables
    are in the hardware (zx_ppe_set_v6_offload()), so a module that fails to program
    them leaves the box on the old, safe path.
  next: |
    * The v6 lookup does engage - and still matches nothing (2026-10-28, second round).
      Feeding the emulator the values the hardware really holds changed what its stubs
      return, and that in turn produced the first hardware observation that moves:
        - The polynomial config is not a raw register copy.  cla_get_hash_poly_config
          splits ONU register 12 into eight 2-bit fields (register bits 0/2/4/6 into
          out[0..3], 16/18/22 into out[4..7]); the real register is 0x00e400e4, so the
          fields are 0,1,0,3,0,1,0,3.  Feeding it the raw value makes the vendor code
          reject its own arguments ("[cla_acl_hash_addr_gen]input invalid parameter"),
          which is what "porting the vendor path" looks like when a stub lies.
        - With real values, the vendor's CLA init is: tm_acl_fast_init writes rule 144
          and 145 (IPv4 up/down) and rule 2 and 3 (IPv6 up/down, byte-identical to
          zx_ppe_v6_rule) plus index[9] word4 = 1 and index[10] word4 = 4; and
          tm_pon_pp_cla_initial writes ONU register 3 (CLA+0x80) = 0x00000e00, register
          13 (CLA+0x94, outer hash space) = 8 i.e. outer_hash_num=2, registers 6/14/16
          (CLA+0x88/0x98) = 0x3fff and 0x3fff3fff, register 21 (CLA+0xcc) = 0.  The
          bootloader leaves 0x80 = 0x0000301a, and this driver writes 0x94 = 4 itself.
        - cla_set_hash_table() splits 520 slots over FIVE table ids, not two: 0..255 ->
          table 2, 256..383 -> table 3, 384..447 -> table 4, 448..511 -> table 5,
          512..519 -> table 6.  The driver only ever allocated from the first two
          banks.  The v6 shotgun and the dump now reach all 520 (v6_shot_ext=1), and
          filling all 520 with the same entry still yields no hit.
        - v6_idxw4 = 0x00000004 - exactly what the vendor's init writes into index[10] -
          changes the hardware's behaviour for the first time: with the trap off, 56,800
          IPv6 frames cost the CPU 18 tx_submitted and 0 arrive at the far end, i.e. the
          v6 fast lookup is now taken and the miss is a drop.  With the driver's
          0x00150154 the same frames all cross the CPU and are forwarded by software.
          So the v6 engine is switched on by that word and by nothing else checked so
          far, and the reason nothing matches is downstream of "is it on".
        - Excluded by measurement on the same bench: all 520 hash slots holding the
          entry (shotgun, extended banks), the vendor's rule index 2/3 instead of 153,
          the vendor's config words 0x80=0x00000e00 / 0x94=8 / 0x88=0x3fff /
          0x98=0x3fff3fff, and the ONU-side IPv6 prefix files (onu6s/onu6d/local6) being
          programmed with the real /64s - every combination still drops the frames.
        - A real v6 entry read back from slot 144 decodes to exactly the intended key:
          window 0 = 0x1100 (UDP<<8), source fd99::2, destination fd88::2, ports
          0xbc53/0x270f, the same encoding the working IPv4 entry at slot 29 uses.
          So the entry content is not where the mismatch lives either.
    * What is now measured instead of assumed (2026-10-28, with the `dump` module
      parameter, which reads the classifier back through the same indirect command
      register the writes use - the first two-way link to these tables on this board):
        - Write path verified.  Read-back of the index table gives 93929190 97969594
          9b9a9998 9f9e9d9c 00150151 for entry 9 and 03020100 07060504 0b0a0908
          0f0e0d0c 00150154 for entry 10, of the rule table gives rule 152 and rule 153
          word for word as zx_ppe_v4_rule / zx_ppe_v6_rule, and of the hash banks gives
          15 words whose word 4 starts with 0x49 (IPv4) or 0x4a (IPv6).  Every table
          the driver writes is therefore really in the hardware, all 17 words of it.
        - The classifier matched an IPv4 entry and has never matched an IPv6 one.  The
          aging word - which zx_ppe_flow_stats already used for IPv4 hits - is the
          oracle: install the flow, send one 1.2 s UDP burst, read the banks back.
            IPv4 192.168.1.250:41234 -> 10.88.0.1:9999: flows at slots 29/38, slot 29
            aging bit 0 = 1 immediately after the burst, and a second 67,800-packet
            burst costs the CPU 18 tx_submitted (hardware forwarding).
            IPv6 fd99::2:48211 -> fd88::2:9999: flows at 144/180, aging bit 0 = 0
            after both bursts, and the second 64,000-packet burst costs the CPU 63,991
            tx_submitted (every frame still to the CPU, i.e. a classifier miss).
          Same bench, same probe, same second burst: the difference is the protocol
          family, not the measurement.
        - Re-swept with that oracle, all misses: v6_rule=2 and v6_rule=152 (the
          emulated vendor call is cla_set_extra_rule_table(2, ...), so the hardware may
          want the extract rule somewhere other than 153), v6_ext=2/9/11 (these do not
          even reach a flow add - ppe_flows stays 0), and v6_idxw4 =
          0x00150153/55/57/5c/5d plus 0x00150054.  The earlier 16-value
          0x00150050..15f sweep is thus confirmed with a sharper probe: the "IPv6
          lookup on" bit is not in that word.
        - Two registers the emulator needed are now known values instead of stubs:
          CLA+0x90 (ONU register 12, mask 0x00ff00ff - the polynomial config that
          cla_get_hash_poly_config reads) reads 0x00e400e4, i.e. 0xe4 in both halves;
          CLA+0xa0 (the local IPv6 file) still holds the ULA prefix from an earlier
          local6= run, and the ONU S/D IPv6 file at 0xfc..0x113 is all zero.  The whole
          CLA window 0x00..0x7c is zero apart from the indirect registers themselves,
          so there is no obvious unwritten enable bit in the low window.
      What this settles: the tables are not the problem (they are provably in the
      hardware) and neither is forwarding, next hop or egress - a v6 frame never
      produces a key that matches the entry, so the gap is on the extract side.
    * Write the table the IPv6 path needs and the driver does not touch.  The emulated
      vendor code (see finding 4) makes cla_set_external_hash_table() part of the IPv6
      flow add and never part of the IPv4 one; its entries are 40 bytes taken from the
      same 60-byte hash rule, at (0x3c40000 + 0x800 + idx) << 6 with the low bit 6 of
      byte 16 forced on, and the caller hands it a slot that aclGetAvailableHashAddr()
      returned.  That address arithmetic is the same unknown as the ONU register path
      below: whether 0xF0200000+64*idx is a directly addressable window on this part or
      a bit address that has to go through the indirect command register is exactly
      what to measure next - an ioremap plus one read tells the difference, and a bad
      guess costs a reboot on a box that boots from an untouched slot 0.
      Reproducing a *complete* IPv6 flow add (19-window key, external entry, index
      re-patch) needs the flow descriptor layout, which the harness has only partly
      mapped so far: descriptor +98 is the protocol, +100..103 and +108..111 the two
      IPv4 addresses, +116..119 the ports, +12 the rule type (3/4 IPv4, 5/6 IPv6,
      7/8 L2), +8..11 the source port id (a bad one prints "invalid sorce port"), and
      the IPv6 addresses are in a part of the descriptor not yet found.
      Two things checked and *excluded*, so nobody repeats them: (a) the "is the IPv6
      hash lookup even switched on" bit is not in the low half of the extract-index
      entry's word 4 - sweeping all sixteen values 0x00150050..15f with the entry
      written into all 384 slots forwarded every packet through the CPU and hit nothing;
      (b) the slot maths, because of that same all-slots run.
    * 2026-10-29 third round (tools/vendor-emu `t_v6path.py`, `t_v6win.py`,
      `t_v6tbl.py`; README "第三轮"): the 13-vs-19 window question is answered, and
      neither of the two earlier candidates was right.
      - `t_v6path.py` breaks on every acl/fast function entry: types 3/5/6 all run
        tm_acl_fast_add -> tm_acl_fast_add_v4v6 -> aclGetExtIdxRuleRamAddrByPort ->
        tm_acl_get_fastHashRule; addAclRule and aclBuildHashAndAclRule never run, and
        the fast flag does not change the path.
      - `t_v6win.py` block-marks the descriptor and reports every output buffer: type 3
        and type 5 put the *same* bytes in the same places - entry e19 from desc +0x62,
        e21..e24 from +0x64..+0x67, e25..e28 from +0x6c..+0x6f, e29..e32 from +0x74..
        +0x77, and nothing else in desc +0x00..+0x124 reaches the entry, the 45-byte key
        or the 60-byte hash rule. So that builder physically cannot produce a 19-halfword
        IPv6 key: its v6 branch only changes the extract index byte and the direction bit.
      - The vendor's IPv6 forwarding entry is a different table in the same module:
        tm_ipv6table_add_set -> sbrg_add_ipv6table (tm.ko 0xf004), whose entry is
        **48 bytes** read-modify-written as 4 rounds x 3 words through ONU registers
        78/77/76 (module descriptor bases 0x000e2007/0x000e2008/0x000e2009, mask
        0xffffffff) after sbrg_*_indreg_wr_cfg(i), with a status bit at register 25
        (base 0x000e2060, mask 1, shift 2). It is exposed to userland as dev_attr_v6Table
        and carries its own globals (g_ipv6FastEn, g_fast_entry_v6, g_v6_total_entry,
        g_IPv6_Snat_Ram_Length, g_IPv6_Dnat_Ram_Length, g_nptv6_sipv6/dipv6).
      - The ONU-address to physical-address rule is now derived, not guessed:
        fpga_read_reg/fpga_write_reg (zx_ponreg.ko 0x0/0x14) are readl/writel on
        [g + 4*off] and fpga_module_init stores 0xF4000000 there (0x198: mov lr,
        #-201326592), which the vendor iotable maps to phys 0x92000000 (the 4 MiB
        switch window). tmOnuRegWrite computes off = descriptor base + stride * idx
        (the 3rd argument, not the register number), so
        **phys = 0x92000000 + 4 * (base + stride * idx)**. Two anchors our driver has
        already verified on hardware confirm it: claRegTable reg 12 (base 0xe3024,
        mask 0x00ff00ff) -> 0x9238c090, the polynomial config we read back as
        0x00e400e4; and reg 19 (base 0xe3028, stride 1, idx 0..3) -> 0x9238c0a0+4*idx,
        the local6 file we wrote and read back.
      - Applied to the sbrag descriptor table, the vendor's IPv6 table port is inside
        the block our driver already maps (adapter->base = 0x92000000, 4 MiB), so no
        new ioremap would be needed: indirect command at 0x92388014 (mask 0x8fc00fff),
        data words 0..2 at 0x9238801c / 0x92388020 / 0x92388024 (regs 22/76/77/78),
        status bits 0..5 at 0x92388180 (regs 23..28), two more fields at 0x92388184 and
        0x92388188, and the access config fields (regs 1..21) at 0x92388004..0x92388018.
        The command word is built by sbrg_set_indreg_cmd (tm.ko 0xc694) as
        (rw << 31) | (start_stop << 27) | (table << 22) | (index & 0xfff) with table <=
        4 and index < 0x1000 enforced - the same encoding our driver already uses for
        its CLA indirect accesses (index | table << 22 at 0x38c014), just on the
        0x388000 bank instead. Whether this board decodes those offsets is UNMEASURED
        (the box is waiting for its serial cable), so this is "reachable by the vendor's
        own code, plausibility checked", not a hardware fact yet.
      What this changes: every CLA-hash sweep for v6 (v6_key, v6_hwn 7..19, v6_type 0..7,
      v6_idxw4, shotgun over all 384 slots) was aimed at an engine the vendor does not
      use for IPv6 forwarding - which is exactly why all of them measured 0 hits while
      the far end counted every forwarded frame. The next target is the 48-byte switch
      IPv6 table; what is still open is the entry layout (needs sbrg_get_table_sel /
      sbrg_hash stubbed with their real semantics so sbrg_add_ipv6table can produce one)
      and the SNAT/DNAT partition bounds (g_IPv6_Snat_Ram_Length / g_IPv6_Dnat_Ram_Length
      / g_v6_total_entry).
    * 2026-10-29 第四轮（纯离线，`tools/vendor-emu/t_v6entry.py`，映射表在那边的 README
      「第四轮」）：48 字节表项的字段布局解出来了，且用随机记录复验过。输入记录 52 字节 =
      valid/age/mode 三个 u32（各 ≤1）、vlan_id（<0x1000）、dip[16]、pt_mask（u32 ≤0xff）、
      sip[16]；字段名不是猜的，是打开 `g_tm_debug_level=7` 之后厂商自己的 printk 打出来的
      （`input:vlan_id / dip / sip / valid / mode / age / pt_mask`），偏移又和函数开头的
      参数校验逐条对上。12 个输出字是一条 384 位小端位流：bit0..2 三个标志、bit3..14 vlan、
      bit15..78 dip 的后 8 字节、bit96..103 pt_mask、bit104..167 dip 的前 8 字节、
      bit192..255 sip 的后 8 字节、bit288..351 sip 的前 8 字节，**w8/w11 硬写 0**（反汇编
      0xf20c 的 `mov r2,#0` + 两次 `str` 证实，且 readmap 实验里读回值对写出的 12 个字
      毫无影响）。按这张表写的 20 行组装函数在 60 条随机记录上与仿真器逐字一致。
      访问协议也定下来了：`sbrg_access_timeout` 轮询 ONU reg 20（`0x92388018`）**读到
      非 0** 才放行（10 次都 0 就返回 -1），`sbrg_set_indreg_cmd` 把
      `index|table<<22|start_stop<<27|rw<<31` 写进 reg 19（`0x92388014`），数据字写
      78/77/76（`0x9238801c/20/24`），读阶段是 4 张 table × 4 个块（index 0x248..0x24b）
      × 3 字、写阶段只写 table 0。仍未定：槽号从哪来、4 张 table 各管什么 —— 两者都卡在
      reg 20 的真实语义上，只能上板读一次复位值再接下去。
    * 2026-10-29 fifth round (real hardware, read-only; `tools/vendor-emu` README
      "第五轮"): the reset values the fourth round said were the only remaining gate.
      Dumping 0x92388000..0x92388200 through the driver's own `xprobe` parameter
      (ioremap+readl), with the register numbers taken from tm.ko's `sbragRegTable`
      (`t_v6tbl.py anchors`), gives: cmd word 0x92388014 = 0x01000001 (table=4,
      index=1, rw=0, start=0 - a leftover from a boot-time write), access gate
      reg 20 bit0 at 0x92388018 = **1** (sbrg_access_timeout wants non-zero, so the
      interface is granted on this board), the three data words at 0x1c/0x20/0x24 =
      0xa1/0/0 (table still empty), all six status bits at 0x92388180 = 0 (idle).
      No read raised an external abort and nothing past 0x92388200 decodes, so this
      strip is the interface itself, not an alias. What is left of the fourth round's
      list is unchanged and now the *whole* list: where the entry slot number comes
      from (sbrg_hash / sbrg_get_table_sel) and what the four tables are for.
    * 2026-10-29 sixth round (offline, `tools/vendor-emu/t_v6seq.py`, README
      "第六轮"): the slot question is closed.  Instead of stubbing the helpers, the
      script lets the vendor code run and only intercepts the ONU accessors, feeding
      the three register values measured in the fifth round (reg 20 bit0 = 1, reg 25
      = 0, reg 29 = 1).  One add is then: read the four tables at the entry's four
      blocks (start=1), then write table 0 only (start=0), three words per block via
      regs 78/77/76.  The index comes from `sbrg_hash()` through the arithmetic at
      0x1000f184: table_sel 1 -> ((hash & 0x7f) << 2) | 0x200, table_sel 2 ->
      ((hash << 2) & 0xff) | 0x300, and the 48-byte entry occupies index..index+3.
      Verified on three records (0x3fd -> 0x3f4, 0x45 -> 0x314, 0x26a -> 0x3a8), and
      the twelve words the vendor code assembles are byte-identical to
      t_v6entry.py's build() from the fourth round.  Still open: what tables 1..3
      hold, sbrg_hash's algorithm (needed to compute a slot in the driver), and what
      this address/prefix table actually does for forwarding - it carries no ports
      and no five-tuple, which is the final explanation for why v6 never matched the
      CLA hash.
    * 2026-10-29 seventh round (on the board, tools/vendor-emu README "第七轮"): the
      table is writable from this kernel.  A new module parameter `sbrg` (three data
      words at 0x38801c/20/24, command 0x388014, status 0x388018, word 0 written last)
      put the twelve words of a valid record (vlan 1, dip fd88::2, sip fd99::2) into
      table 0 slots 0x3f4..0x3f7 and read them back word for word; a re-read later
      still matched, tables 1..3 at the same index stayed zero, and the box kept
      pinging its LAN peer.  So the access protocol, the slot addressing and the
      48-byte layout are hardware facts, not inference.  What is left is no longer a
      hardware probe: port sbrg_hash (328 bytes) into the driver so the kernel can
      compute the slot itself, and then answer the behavioural question - what a
      table of only vlan + two addresses + pt_mask does to a forwarded v6 flow.
    * 2026-10-29 eighth round (offline port + board run, tools/vendor-emu README
      "第八轮"): sbrg_hash is ported and the whole add path runs in the driver.
      Hooking sbrg_hash's entry in the emulator showed what it hashes - not the
      52-byte record but a 34-byte key of sip[16] + dip[16] + vlan as u16le,
      fed backwards through CRC-16/CCITT (init 0, the 256-word table at
      .rodata:0x108) and masked to 10 bits.  With that, the port matches the
      vendor function 200/200 on random keys (t_hash run) and t_v6slot.py matches
      the slot the vendor's own sbrg_add_ipv6table writes 8/8 on random records.
      The `sbrg` parameter grew `h|a|d <sip32hex> <dip32hex> <vlan> [pt_mask]`
      (plan / add / delete: vendor's four-table read, then four blocks of table 0)
      and `r` now uses the vendor's read encoding (start=1, rw=0) instead of the
      CLA read bit 31.  On the board: plan output == t_v6plan.py word for word
      (hash 0x3fd, sel 1, slot 0x3f4, twelve words), add+read-back matched for
      two records (vlan 1 at 0x3f4, vlan 7 at 0x238), delete read back all zero,
      and the LAN kept forwarding.  The read encoding is now proven too, not just
      the write side.  One self-inflicted lesson: the first build stored the
      selector as 0x388184 and added ZX_SBRG_BASE on top, so it read
      0x92000000+0x710184 (outside the 4 MiB window) and oopsed; panic_on_oops=0
      spared the reboot, but the oopsing thread died holding ppe_lock, so recovery
      was module swap + reboot.  Offsets in that bank are relative to
      ZX_SBRG_BASE only.  Still open: tables 1..3, the SNAT/DNAT bounds, and the
      behavioural question below.
    * 2026-10-29 correction to the third round (found while finishing the eighth;
      tools/vendor-emu README "第八轮" section 6): that 48-byte table is the
      IPv6 **multicast (MLD) table**, not the unicast forwarding table, so the
      third round's "the vendor's IPv6 forwarding entry is a different table"
      conclusion is wrong on what the table is for.
        - `tm_ipv6table_add_set` / `_delete_set` / `_lookup_get` have no caller
          inside tm.ko at all (the only relocations into them are from their own
          bodies); they are exported for switch.ko.
        - switch.ko calls add_set only from `sw_add_mld_addr_e8v4`,
          `sw_del_mld_addr_e8v4` and `drv_del_mld_addr_gpon_sfu`, i.e. MLD
          address add/remove.
        - `sw_add_mld_addr_e8v4` (switch.ko 0x11a14) builds the 52-byte record as
          valid=1, age=0, mode=1, vlan, dip = the multicast group it was handed,
          `pt_mask = 1 << port` and sip = 0 - so the key (sip, dip, vlan) is an
          MLDv2 (S,G,V), (*,G) when sip is 0, and `pt_mask` is the egress port
          bitmask ("pt" = port), not a protocol mask.
      So this table cannot accelerate unicast IPv6.  The driver's `sbrg`
      add/delete/read path stays (it is a faithful MLD-entry programmer and the
      only two-way link to that bank we have), but the unicast goal moves to
      switch.ko's NPU/hardfast path: `npu_ipv6_l3{tcp,udp}{up,down}_cap`,
      `npu_drv_create_flow`, and `sw_acl_l3_hardfast_session_add` ->
      `zte_api_fast_l3_session_add`.  Not started.
    * 2026-10-29 ninth round (board, `tools/vendor-emu/t_v6rule.py` + a far-end
      bench): the CLA IPv6 hash path is excluded, not just unmatched.
      First, two measurement corrections.  The aging word is *not* a usable oracle:
      an IPv4 flow that provably goes through the hardware (tx_submitted flat) does
      not change any aging word either, so "aging bit stayed 0" never meant "no
      match".  tx_submitted in dma_state is the oracle that works, and it needs a
      control in the same topology: the same hairpin flow (LAN host -> router ->
      same LAN host) over IPv4 costs 98 of 600 packets of CPU, so the topology is
      not what stops IPv6.
      - `t_v6rule.py` runs the vendor flow-add again and records rule/index writes:
        a type-5 (IPv6) add writes **no rule and no index table at all**, only the
        60-byte hash entry, so the entry has to match whatever rule the vendor
        provisioned at boot.  That entry is 13 bytes (protocol in the *low* byte,
        four bytes of each address, both ports in wire order) - which contradicts
        the shipped rule, whose window 0 mask is 0xff00 and therefore puts the
        protocol in the *high* byte.  The vendor's own ACL IPv6 path cannot match
        either; their IPv6 fast path is the NPU (previous bullet).
      - The driver grew the diagnostics to test that directly instead of guessing:
        `v6_wins "ofs[:mask],..."` defines an arbitrary window list, patches the
        extract rule to exactly that list (offset at bit 7n, mask at 140+16n, type
        at 480+3n, hash length at 468/472) and builds rule, hash key and entry from
        one description; `v6_fix` flips protocol/port/address byte order; `cla_clear`
        zeroes all 520 slots (switch SRAM survives a module reload, and a previous
        shotgun run left every slot occupied, which is what made one IPv4 test look
        broken); `dump 32` prints all 520 aging words compactly.
      - Decisive run: `v6_wins "6:ff00"`, i.e. a **one-window** key that only holds
        the next-header byte (mask 0xff00, hash length 1), with the entry written
        into **all 520 slots** so the slot model cannot matter.  Rule read back from
        the hardware: offset 6, mask 0xff00, hashlen 1; entry read back: window value
        0x1100.  Result: 617 of 600 packets through the CPU, no match.  Repeated with
        the entry index byte 0x6a (the value the vendor writes), with the rule at
        index 2 (where the vendor's `cla_set_extra_rule_table` call goes), and with
        the vendor's CLA config registers written in (`+0x80=0x00000e00`,
        `+0x88=0x3fff`, `+0x94=2`, `+0x98=0x3fff3fff`; ours read 0x301a/0x7fff/4/
        0x7fff7fff): all miss.  An IPv4 flow in the same topology hits with the same
        driver, same slot model, same measurement.
      Conclusion: this classifier does not look up an IPv6 hash on this silicon (or
      needs a knob outside the CLA window that no vendor code we can emulate
      touches).  Reproducing the vendor's IPv6 acceleration in mainline is therefore
      not a matter of the key layout; it needs the vendor NPU, whose driver cores
      live in their 4.1 vmlinux and talk to `CSPKernel_skb_SelectQueue`.  IPv6 stays
      on the CPU path (`ipv6=1` = correct forwarding, no fast path, which is why it
      is not the default), IPv4 stays accelerated.
    * Bench that made the above cheap, for the next round: DESKTOP-A5A2TK2 runs an
      iperf3 server on 5201 and its firewall accepts 192.168.0.0/16 plus IPv6
      link-local only.  So a *routed* IPv4 flow is made by using a second subnet
      inside 192.168/16 (`iperf3 -c 192.168.1.243 -B 192.168.5.50`, host route
      `192.168.1.243/32 via 192.168.1.1`, router route `192.168.5.0/24 via
      192.168.1.250`), and routed IPv6 to that peer is blocked by policy, which
      leaves the one-way hairpin bench (`fd99::2:48211 -> fd88::2:9999`) as the IPv6
      traffic source.  The iperf3 server hung twice mid-session and had to be left
      alone; the hairpin bench needs no far end and is the reliable one.
    * The local IPv6 address register file (ONU reg 19) and the ONU S/D IPv6 file (reg
      23) are now programmable - module parameters local6/onu6s/onu6d take the address
      as four 32-bit words, and the offsets were derived from the vendor's register
      descriptors (base 0x000e3028/0x000e303f, stride 1, count 4/8; byte offset =
      register number * 4, the same rule that puts the CLA command register at
      0x38c014), so they land at 0x38c0a0 and 0x38c0fc.  Read-back confirms the writes
      stick word for word.  What they *do* is still unproven: writing the router's own
      ULA there changed nothing observable, because a frame for our own address is
      already handled by the classifier's "other L3 -> CPU" default action.  Their
      real job is probably on the vendor's ACL/DPI path, not on ours.
    * 2026-10-29 晚补的三个仿真器探针（`tools/vendor-emu` 的 `t_gfhr.py` /
      `t_key.py` / `t_poly.py`，细节和映射表在那个目录的 README「第二轮」小节）：
        - 表项里的 extract index 字节我们是对的：原厂 IPv6 就是 `0x40|10 = 0x4a`，
          和 `zx_ppe_hash_entry()` 写的一致。
        - 45 字节 key 里的规则号不一样：原厂 IPv6 上行写 `0xa0 = 10<<4`，我们写
          `0x99 = v6_rule`。同一条差异在 v4 上也存在（原厂 `0x90`、我们 `0x98`）却
          没有挡住 v4 命中，所以它只是候选。真机上把 `v6_key=0xa0` 一设、看同一股
          v6 流的 aging bit 0 就知道了。
        - 还有一个**没解的矛盾**，别再按旧假设推：喂了真实多项式配置之后，原厂
          type-5 的 hash 表项仍然只写 v4 那 13 字节窗口（`e19..e32`，来自描述符
          `+0x62/+0x64..+0x67/+0x6c..+0x6f/+0x74..+0x77`），描述符 `+0x00..+0x124`
          里没有任何 IPv6 地址被读进窗口；而驱动写的是 19 窗口 38 字节。要么原厂
          v6 快表项只用前缀+端口做 key、地址靠规则掩码在硬件侧比，要么
          `tm_acl_fast_add_v4v6()` 不是原厂 IPv6 快路径走的那条。
    Measurement note, because it cost a wrong conclusion once: tx_submitted in
    /sys/kernel/debug/92000000.ethernet/dma_state proves that the CPU did *not* touch
    a packet, which is not the same as proving the hardware forwarded it - a dropped
    frame also keeps the CPU out.  The far end here is fd88::2, an address the router
    routes back out the same LAN port, so the sending host counts the frames arriving
    on its own NIC (non-zero means forwarded, 0 means dropped).  Both tricks are in
    BUILD.md "Measuring the packet processor".
  docs: 01-文档/BUILD-NOTES.md 2026-10-27; 06-分析与对比/原厂硬件加速驱动-IPv6-对比-2026-10-27.md; 06-分析与对比/拿原厂运行时表内容-方案-2026-10-27.md
- id: PPE-IPV6-TRAP-CPU
  severity: minor
  status: mitigated
  surface: ethernet + ipv6
  symptom: every IPv6 frame on the LAN crosses the CPU, including the ones the switch would bridge on its own, so IPv6 traffic between two LAN hosts is capped by the CPU and shows up in softirq while the same traffic over IPv4 is hardware switched.
  verified: yes - 2026-10-28 on this board, 2,000 LAN-to-LAN IPv6 UDP frames per run, four switches of the runtime knob, numbers repeat exactly
  cause: |
    The IPv6 ethertype trap.  It is set whenever the engine has no IPv6 classifier
    tables (that is the v1.4 fix for the PPE-IPV6-TCP stall), and a trapped frame is
    diverted to the CPU *before* the classifier, so nothing the switch could have
    bridged ever gets bridged in hardware.  The IPv4 trap has been lifted whenever
    offload is active since v1.0, which is why only IPv6 had this cost.
    Measured with the far-end-free method: 2,000 UDP frames from one LAN host to
    another, CPU tx_submitted delta 2,018 with the trap on and 18 with it off (the 18
    is the ARP/ND/setup floor seen in every run).  The same knob was toggled 0 -> 1 ->
    0 -> 1 with the matching numbers each time.
  workaround: |
    The module parameter `ipv6` is now writable at runtime and does exactly this
    switch - it programmes the v6 extract tables and lifts the trap, or traps IPv6
    again:

      echo 1 > /sys/module/zx279128s_ppe/parameters/ipv6      # hardware switching
      echo 0 > /sys/module/zx279128s_ppe/parameters/ipv6      # trap back on

    No reload, no firewall restart, no reboot, and `echo 0` is the recovery path if a
    box ever behaves badly with the trap lifted.
    With it on (ipv6=1) all three IPv6 paths were re-measured on the same box:
      * LAN host -> LAN host            : 2,000 UDP frames, CPU +18   (was +2,018)
      * LAN host -> routed through it   : 1,800 UDP frames, 1,800 arrive at the far
        end, CPU +1,819 (classifier -> CPU, no fast path; see PPE-IPV6-TCP)
      * LAN host -> the router itself   : TCP/80 3/3, ICMPv6 3/3
    ICMPv6 is not a usable probe for trap experiments - it has its own trace/trap path
    and keeps working even when the ethertype trap diverts everything (that is why the
    ping numbers above prove nothing on their own; the TCP and UDP numbers do).
  next: |
    * Make ipv6=1 the shipped default, but from the boot side rather than from the
      compiled-in default: a UCI option that the init script applies after the module
      loads, so a bad default is fixable from failsafe instead of needing a power
      cycle.  Until then the image stays on ipv6=0 and the switch is a one-liner.
    * Untested with the trap lifted: MLD/multicast IPv6 (the switch has its own MLD
      tables in the vendor design - `drv_add_mld_addr_gpon_sfu` - and our port does not
      program them), and IPv6 over the (unused) WAN.
  docs: BUILD.md "Measuring the packet processor"; 06-分析与对比/原厂硬件加速驱动-IPv6-对比-2026-10-27.md
- id: USERDATA-EXTROOT
  severity: minor
  status: mitigated
  surface: storage
  symptom: the 47 MiB window behind both image slots (0x3f00000..0x6e00000) is unused while the internal jffs2 overlay holds only ~8 MiB
  verified: yes - the window is free in every vendor version checked (all four upgrade packages 4B4..8B8 declare identical slot tables ending at 0x3f00000; the stock kernel declares nothing beyond 0x2fc0000; the programmer dump shows 0x3cc0000..0x72c0000 erased); tigaotg ran a UBIFS extroot on it for weeks
  workaround: "v1.3 ships the partition plus a gated extroot: preinit attaches the UBI only, /etc/init.d/e2631-userdata enables the fstab mount only after reading a real UBIFS superblock (magic 0x06101831), and /sbin/e2631-userdata-init formats the volume only under an explicit E2631_CONFIRM"
  note: boot can never format the volume (a read-write mount of a blank volume would auto-format it; the gate keeps that out of the boot path). The JFFS2-to-UBIFS migration prototype from PR#1 is NOT shipped - whiteouts/xattrs/power-fail atomicity are unsolved there
  blockers: |
    Three defects kept this feature from ever working, all found by upstream actually
    formatting the partition (42daef9) and two of them present in our tree until
    2026-10-29 - they are recorded here because "the mechanism exists" was true for a
    week while "the mechanism ran" was not:
      * the window guard used 66384896 (0x3f4f400) where the boundary is 66060288
        (0x3f00000). Both the preinit attach (lib/preinit/79_e2631_userdata) and the
        formatter (/sbin/e2631-userdata-init) therefore refused on every unit, while the
        DTS declares userdata at exactly 0x3f00000 - an offline arithmetic proof, no
        hardware needed.
      * USE_PROCD=0 in etc/init.d/e2631-userdata is not "off": rc.common tests
        [ -n "$USE_PROCD" ], so the string 0 selected the procd branch. The block at
        package/base-files/files/etc/rc.common:121 runs after `. "$initscript"` and
        redefines start() as rc_procd start_service - both scripts define start() and
        neither defines start_service, so the script's own start() was never called and
        the gate became a no-op. Same line removed from etc/init.d/e2631-netune, which
        means that boot-time tuning (RPS/XPS, IRQ affinity, PAUSE off, GRO) had been
        doing nothing at all. The correct form is to leave the variable unset.
      * the UBIFS magic was checked as md5 of the big-endian display order (06 10 18 31)
        while the volume stores the little-endian u32 (31 18 10 06): 2c877f78... never
        matched, 570471ba... is the real constant. An initialised volume would never
        have been activated.
    Fixed offline on 2026-10-29; the on-box acceptance (format -> 376 blocks -> mount ->
    extroot on the next boot) still needs the serial console.
- id: WPAD-VARIANT-AND-REGDB
  severity: major
  surface: wi-fi
  status: fixed
  symptom: no 802.11s mesh, no DPP/DPP2, no EAP-PWD and no Suite-B 192 whatever the configuration; and 5 GHz channel/power/DFS followed the driver's built-in regdomain instead of the CN view
  verified: offline - the built manifest carries wpad-mbedtls + wireless-regdb and no wpad-basic, and the installed binary is the full one (1,696,459 B vs the basic variant's 979,231 B; `dpp` 21 hits vs 1, `mesh_fwding` 2 vs 0, `EAP-pwd` 3 vs 0, `suiteb` 2 vs 0 in the two binaries, 2026-10-29). On-air behaviour unverified: the box is awaiting its serial cable
  cause: |
    Nothing in this tree ever selected a wpad variant, so the target's DEFAULT picked
    wpad-basic-mbedtls; CONFIG_PACKAGE_wireless-regdb=y sat commented out in the seed.
    Upstream (f41432e) describes the basic variant as having no 802.11r/k/v, no mesh and
    no WPA3-SAE - that is NOT what this tree's hostapd Makefile says, and it was worth
    checking instead of copying: every mbedtls variant gets CONFIG_SAE=y and CONFIG_OWE=y
    (basic included), and the basic description itself advertises 802.11r + 802.11w. What
    basic genuinely lacks is the `full` variant's extra makeopts - CONFIG_MESH, CONFIG_DPP,
    CONFIG_DPP2, CONFIG_EAP_PWD, CONFIG_SUITEB192, CONFIG_WPS_NFC - and that is exactly
    the difference measured in the two binaries above.
  fix: seed/e2631.config now selects CONFIG_PACKAGE_wpad-mbedtls=y and CONFIG_PACKAGE_wireless-regdb=y
  next: |
    * On the box: `iw reg get` should show the regdb CN view, and 802.11s/DPP should now be
      offered where they were absent before (e.g. `iw list` mesh point interface modes,
      LuCI's mesh page). 802.11r/11w were already there in basic; 11k/11v was never
      verified either way on this board.
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
- id: PPE-BIND-ACCOUNTING
  severity: major
  status: fixed
  surface: ethernet
  symptom: the flowtable is bound and nft accepts flows, but the PPE is off - ppe_active=0, every packet takes the CPU path, no warning anywhere
  verified: yes - 2026-10-28 on slot 1 serial 24 (kmod-e2631-ppe build): after rmmod/insmod plus /etc/init.d/firewall restart, /sys/kernel/debug/92000000.ethernet/dma_state read ppe_active=0 while `nft list flowtables` still listed the flowtable with all four devices, and the dmesg pairs ended "disabled"
  cause: |
    adapter->ppe_users was a counter, incremented on FLOW_BLOCK_BIND and decremented on FLOW_BLOCK_UNBIND.
    nft replaces a ruleset by binding the NEW flowtable before unbinding the OLD one, so the decrement of
    the old block took the counter to 0 while the new block was already bound and the driver called
    zx_ppe_stop(). Nothing ever brought it back up: the next bind saw ppe_users != 0 and skipped
    zx_ppe_start(). A module reload made it worse - the counter cannot know how many flowtables are
    actually bound, so it drifted negative and the "if (!ppe_users)" guard could never fire again.
  workaround: |
    fixed in the driver on 2026-10-28: the counter is gone, and the answer to "is any flowtable bound"
    comes from zx_ppe_block_cb_list (zx_ppe_has_bound_blocks(), exported), which the TC core itself
    populates and drains. Bind starts the hardware only when ppe_active is 0, unbind stops it only when
    the list is empty, and the module's init re-attaches to whatever it finds. Verified on slot 1
    serial 26: ppe_active=1 at boot, fw4 restart leaves it at 1 (the log is now enabled, disabled,
    enabled - the last transition is a bind), rmmod/insmod alone brings it back to 1 without touching
    the firewall, and a 12 s IPv4 TCP stream through the router (VM 192.168.10.2 -> 192.168.1.243,
    hairpin, so a genuinely forwarded flow) ran at 934 Mbit/s with ppe_flows=4, ppe_add_skip=0,
    ppe_add_fail=0
  next: none - but note that this counter existed in the built-in driver too, so this was not introduced by the module split, only exposed by it (the reload path was never reachable before)
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
that drops it is now known, and the lever was never a fragment: `include/kernel-defaults.mk`
appends every `CONFIG_KERNEL_*` line of the top-level `.config` to the merged kernel config
with the prefix stripped, and the `kconfig.pl` merge is last-wins; `config/Config-kernel.in`
declares `KERNEL_DEVMEM` with default `n`, so the passthrough carried
`# CONFIG_DEVMEM is not set` *after* the fragments' `=y` and won. Fixed on 2026-10-29 with
`CONFIG_KERNEL_DEVMEM=y` in `seed/e2631.config` - one line at the layer that decides it -
after upstream published the same root cause (`d68e3b7`). This file said "mechanism
unexplained, do not re-add a fragment edit" for a week, and that warning was correct.

The consequence *was* that `/sbin/e2631-fwstate`, which reads the MT7916's `MT_TOP_MISC`
register to answer "what is the firmware doing right now", could not run in the shipped
image - it failed with `no /dev/mem`. With the symbol in place that tool has its device;
`wsl/build-regprobe.sh` (a module that reads the same register from inside the kernel)
stays as the fallback for a kernel built without `/dev/mem`.

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

## PPE 引擎拆成模块（v1.5，2026-10-28）

目的只有一个：让"改一行分类器代码"不再等于"等一次 40~45 分钟的内核重编"。PPE
（分类器 index/rule 表、两个哈希 bank、重写表、IPv6 表）现在是包
`overlay/package/kernel/kmod-e2631-ppe`，内建驱动通过 `zx279128s-ppe.h` 里的钩子表
调它。边界为什么这么划、什么不能跨过去，写在 `docs/architecture.md` 和
`BUILD.md` 第 9 节，这里只记实测。

- 快路：`make package/kernel/kmod-e2631-ppe/compile`，连 overlay 同步一共
  **11.2 秒**（旧路：改 `files/` 下一次全量 45 分钟）。
- 真机闭环（slot 1 serial 26）：改完 `.ko` → `scp` 到 `/tmp/new-ppe.ko` →
  `rmmod zx279128s_ppe; insmod /tmp/new-ppe.ko` → 不用碰防火墙，模块 init 自己
  把已经绑着的 flowtable 接管回来（`ppe_active=1`）→ 再打同一批 IPv4 流，930 Mbit/s、
  `ppe_flows=4`。
- 开机路径没变：模块由 `/etc/init.d/boot` 里的 `/sbin/kmodloader` 从
  `/etc/modules.d/31-e2631-ppe` 加载，早于 `/etc/rc.d/S19firewall` 的 flowtable 绑定；
  没有引擎时 `ndo_setup_tc` 的 BIND 返回 `-EOPNOTSUPP`，也就是"这条流不加速、走软件"，
  不会崩也不会挂。
- 新增的 sysfs 路径：`/sys/module/zx279128s_ppe/parameters/ipv6`（旧的
  `zx279128s_eth` 那个随内建驱动一起没了）。

## PPE-MODULE-NEEDS-ETH-HOOKS - 模块能不能加载取决于镜像里的以太网驱动（2026-10-29）

`zx279128s-ppe.ko` 不是独立驱动：它靠内建以太网驱动导出的九个符号跟引擎对接
（`zx_ppe_hooks_register` / `zx_ppe_hooks_unregister` / `zx_ppe_has_bound_blocks` /
`zx_ppe_set_v6_offload` / `zx_ppe_ht_params` / `zx_eth_for_each_adapter` /
`zx_eth_is_port` / `zx_update_traps` / `zx_update_mac_tables`）。这些导出来自
`overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c`，
是**模块化改造（v1.5，2026-10-28）之后**才加进内核源文件的。

实测（2026-10-29，slot 1 serial 41 跑 `e2631-openwrt-factory-v8.bin`，构建时间 10-28 00:00）：

```
insmod /tmp/t7.ko dump=1
zx279128s_ppe: Unknown symbol zx_update_traps (err -2)
zx279128s_ppe: Unknown symbol zx_eth_for_each_adapter (err -2)
... 九个全部 Unknown symbol，insmod rc=255
```

也就是说 **v8 这个镜像比 hook 早，装在里面的以太网驱动没有那批导出**，换多少次
`.ko` 都白费：`insmod` 在符号解析阶段就失败，`dmesg` 里只有 `Unknown symbol`，
不会有任何 CLA 访问。判断方法：`ls /proc/kallsyms | grep zx_ppe_hooks` 为空就是
这个原因，别去怀疑模块本身或参数。

结论/前置条件：**做硬件侧 PPE 实验，镜像必须是 hook 落地之后全量构建的**
（`04-构建环境与脚本/ppe-build-inner.sh`，因为以太网驱动是内建的，改它的源文件
必然要重编内核，不是 11 秒快路能解决的）。快路 `ppe-kmod-inner.sh` 只重新编模块，
对"镜像里有没有 hook"没有任何影响。

## RECOVER-VENDOR-TO-OPENWRT - 从原厂刷回的实际步骤与两个坑（2026-10-29）

这轮完整跑了一遍"原厂固件 → OpenWrt"，记录能省下下一次两小时的细节。

1. **包头要自己改**。cspboot 只认 serial 高的槽，而出厂包 `H_SERIAL` 是 1；原厂
   httpd 到底是"当前槽 +1"还是"照着包里的值写"没有实测过，而一旦照写就等于白刷
   还先毁掉唯一的 OpenWrt 槽。所以上传前把包改成 `H_SLOT_START=0x2300000`、
   `H_SERIAL=两槽最大值+1`、`H_NEW=1`，并把版本串（`+0x10`）抬高以免原厂 UI 判降级，
   然后重算 `+0xa4`（覆盖 `[0:0xa4]`）与 `+0x1fc`（覆盖 `[0:0x1fc]`）——**标准
   zlib.crc32，不是 JFFS2 那套取反约定**。实测：`serial 41` / `V1.0.0.8B8.9300` 的包
   上传后盒子重启进 OpenWrt，`e2631-fwslot info` 读回 `serial=41 [running]`。
2. **上传本身很朴素**：只发 `VersionUpload` 一个文件字段，21 MB，3.7 秒回
   `{"IF_ERRORID":0}`，随后 `upgrade_firmware_query` 依次给 `needRefresh=true`、
   `upgrDesc=处理中，请稍候……`、`willReboot=true`；再约 27 秒 192.168.1.1 的 80/22
   就都通了。
3. **回灌 overlay 不能用 `scp -r`**：`upper/etc/rc.d` 里有指向 overlay 之外目标的
   相对符号链接（如 `S50cron -> ../init.d/cron` 在本地副本里不存在），`scp -r` 报
   `No such file or directory` 并**中途退出**，只灌进去一部分。用 tar 管道：
   `tar -cf - etc lib usr | ssh root@<ip> 'tar -C / -xf -'`。`lib` 必须带上——
   无线 caldata `lib/firmware/mediatek/mt7916_eeprom.bin` 在里面。
4. **回灌之后 dropbear 会楔死**：22 端口 TCP 能连、服务端 banner 之前就 RST
   （客户端看到 `kex_exchange_identification: Connection reset by peer`），系统日志是
   `dropbear[..]: Early exit: String too long`，连刚重启过的机器也一样，一直持续到
   把 dropbear 重启一次。救法（uhttpd 一直活着，不需要物理接触）：

   ```
   ubus call session login {"username":"root","password":<ap.sh 里那个口令>}
   ubus call rc init {"name":"dropbear","action":"restart"}     # 实测重启后 SSH 立即恢复
   ```

   这两条经 HTTP 的 `/ubus` JSON-RPC 就能发，封装好的客户端是
   `工作文件/05-路由器操作脚本/ubus.py`（另有 `ubus.py sh '<命令>'` 与
   `ubus.py cat <路径>`，权限受 luci-base ACL 限制——`file exec` 对该镜像放行，
   `/sbin/reboot`、`/sbin/wifi`、`/bin/kill` 等都在允许列表里）。
5. **无线要等约 3 分钟**：`e2631-wifi-load`（START=95，`DEFER=25`）+ 开机 pending
   标记的重试，dmesg 打印 `e2631-wifi-boot: waiting 25s` 之后就安静了，成功标志是
   `wifi-on: done: 7906=mt7915e 790a=` 和 `phy1-sta0: associated`；急着用可以手工
   `/sbin/e2631-wifi-on` 立刻拉起（实测有效，且不会重复绑定 790a）。

## WANPORT-ANY-PORT - 手动切口的真机验收（2026-10-29）

`e2631-wanport apply <port>` 的语义是"WAN netifd 接口绑到哪个 netdev、哪个 netdev
留在 br-lan 里"，四步动作全部实测生效（slot 1 serial 41，V1.0.0.8B8.9300）：

```
e2631-wanport status                    # 切换前：wan device=wan, br-lan ports=lan1 lan2 lan3
DRY_RUN=1 e2631-wanport apply lan3      # 预演：打印要跑的 uci 命令，不落地
e2631-wanport apply lan3                # 实测输出：e2631-wanport: WAN 口 -> lan3（原来 wan）
```

切换后读回的现场（`e2631-wanport status` + `uci` + `ubus`）：

| 观测点 | 切到 lan3 后 |
|---|---|
| `wan device` / `manual_port` | `lan3` / `lan3` |
| `br-lan ports` | `lan1 lan2 wan`（lan3 被摘掉，原 `wan` 口进 br-lan） |
| `ubus call network.interface.wan status` | `"device": "lan3"`, `"proto": "dhcp"` |
| `uci get network.wan.device` / `network.wan6.device` | 都是 `lan3`（落盘，重启保持） |
| 端口 MAC | lan3 拿到 WAN 的固化 MAC `e4:60:4d:00:00:01`，原 `wan` 口拿回 br-lan 的 MAC |
| 管理面 | 管理侧所在 lan2 一直在 br-lan 里，SSH 全程没断 |

再 `e2631-wanport apply wan` 回退，上面每一项都回到切换前的值。

**没验的部分**（不要当成已验）：真正从"被移动的那个口"跑一条 ISP 上行。
这需要一根独立的上行线 + 一台对端（DHCP/PPPoE），现场只有一个管理侧网线口，
把管理侧所在口切走会把自己关在门外（`docs/02` 7.6 那次就是这样丢的访问）。
所以这一轮只证明了动作链与配置持久化，业务面要到有独立上行线时再验。

## PPE-AUTOLOAD-BOOT-LOCKOUT - 一次真实的自锁，以及为什么引擎不再随开机加载（2026-10-29）

> **2026-10-29 晚更正：下面这条的归类和根因都是错的。**当天接上串口之后，日志显示
> 盒子在 `pdt_getreal_kernelfs:539 verify_kernel … at ram:0x42020020` 之后执行
> `bootm` 时就被拒绝了：
>
> ```
> ## Booting kernel from Legacy Image at 42020020 ...
> Wrong Image Format for bootm command
> pdt_cspboot_run:1091 Start kernel failed
> ```
>
> 也就是说**内核从来没有被加载**，卡死点不在"内核跑起来之后的模块加载"。真正的原因是
> 那次刷进 slot1 的镜像（v9，10-28 13:57 全量构建的产物；v10 是同一缺陷）内核区里只有
> 裸 zImage——没有 uImage 头、也没有 DTB，而包头 CRC 是对这份内容重算的，所以引导器的
> 校验照样通过。逐字节证据、机制和已加的三道打包闸见 `KERNEL-EXTENT-NOT-A-UIMAGE`。
>
> 因此下面这段里"链路在、零 ARP ⇒ 内核跑起来之后挂死"的**判据不成立**：这两种故障在
> 网络侧完全不可区分，必须上串口看 `bootm` 的输出。`e2631-ppe` 的 pending 守卫仍然保留
> （作为对"开机路径上的模块加载"的防御，思路本身没错），但它不是这次自锁的原因，也没有
> 被这次事故验证过。

这是这台机器第一次把自己锁在门外，值得逐条记下来。

**做了什么**：为了让 `zx279128s-ppe.ko` 能加载，镜像必须由"以太网驱动导出 hook
符号"之后的内核构建（见 PPE-MODULE-NEEDS-ETH-HOOKS）。v8 镜像（10-28 00:00）比 hook
早，所以重新全量构建，并把这个新镜像用 `e2631-fwslot flash --slot 1` 写进了**正在
运行的那个槽**（slot1），重启。

**结果**：盒子再也回不来。症状很具体：ens34 的链路一直 1000M/Full（PHY/交换侧活着），
但 12 个候选 LAN 地址（192.168.1.1 / 0.1 / 2.1 / 4.1 / 8.1 / 10.1 / 31.1 / 43.1 /
100.1 / 5.1 / 10.0.0.1 / 172.16.0.1）裸 ARP 全部无应答，8 秒抓包 ens34 上连一帧都没有。
断电重启无效。

**归类**：属于"内核跑起来之后挂死"，不是"CRC 层起不来"。前者 cspboot **没有任何自动
补救**——`pdt_getimgindex` 只比 serial（槽1 serial 42 > 槽0 的 40），没有失败计数，
反复上电只会一次次回到同一个挂死点（docs/02 §"起不来的处置阶梯"第 2 条早就写明，
2026-10-29 用整台机器又验证了一遍）。唯一的路是串口：上电后 `*** Press 1 means
entering boot mode***` 那一秒按 `1`、口令 `5cE080@fyBD` 进 U-Boot，然后
`tftp 0x43000000 <initramfs.uImage>` + `bootm 0x43000000`。

**根因（推断，未在串口上确认）**：模块是 `AUTOLOAD:=$(call AutoLoad,31,zx279128s-ppe)`
装进 `/etc/modules.d/` 的，`kmodloader` 在 `/etc/init.d/boot` 里就 insmod——**早于网络**。
所以卡死点落在网络起来之前，这正好解释"链路在、ARP 全无"。具体是 insmod 里哪一步卡住
（CLA 寄存器访问？hook 注册时的 traps/mac 表下发？）还没定位，要等串口日志。

**现在的做法**（本轮已改，随镜像一起发）：
- `kmod` 包去掉 `AUTOLOAD`，引擎不再由 kmodloader 加载；
- 新增 `base-files/etc/init.d/e2631-ppe`（`START=99`，procd）与
  `base-files/sbin/e2631-ppe-boot`：加载前先写 `/etc/e2631-ppe-pending`（在 overlay 里，
  重启还在），加载后再开一个 60 秒确认窗口，**只有这台机器确实活着**才撤掉标记；
  下次开机会看到标记还在，于是**跳过引擎**继续正常启动，并留一份
  `/etc/e2631-ppe-failed` 说明原因。要再试就删掉这两个文件。
  这和 Wi-Fi 那次（`e2631-wifi-boot` 的 pending 标记）是同一套思路。
  （2026-10-29 晚补：标记现在带 boot_id，且 init 脚本去掉了 `procd_set_param respawn 0`，
  否则守卫会把**自己**判成上次卡死——见 `PPE-GUARD-RESPAWN-LOOP`。）
- 附带效果：引擎出问题最多让这一台少一次硬件加速，而不是少一台路由器。

**教训（写给下一次的自己）**：
1. **永远不要把"改了启动路径的东西"直接写进唯一的可启动槽**。要么写另一槽并先用串口
   确认，要么让改动默认不生效（这次的 `AUTOLOAD` 就是典型：先在开机路径上动手，等于
   把唯一的退路也押上了）。
2. 这台机器没有串口就等于没有退路；接 TTL 之前，任何"新内核 + 新开机服务"的组合都
   属于高风险动作。Wi-Fi 驱动当年也是这么把 console 拖死的，解法和今天一样：把加载
   推到 START=95/99 并加 pending 守卫。
3. `e2631-fwslot flash --slot 1` 会连带擦掉 mtd7/mtd8（rootfs 与 overlay 都在槽内），
   所以刷 slot1 不只是换系统，还会清掉全部运行时配置——刷之前先假设配置会丢。

## PPE-GUARD-RESPAWN-LOOP - 守卫把自己判成"上次卡死"，引擎加载成功却被永久拉黑（2026-10-29 实测）

v9b（slot1，`serial=42`，`V1.0.0.8B8.9700`）是第一个真正带引擎跑起来的镜像：
内核 hook（PPE-MODULE-NEEDS-ETH-HOOKS）、`zx279128s-ppe.ko`、`e2631-ppe` 加载器都在里面。
真机上**引擎确实加载了**——`lsmod` 给出 `zx279128s_ppe 24576 0`，dmesg 给出
`zx279128s-ppe: engine loaded, IPv6 classifier off`，SSH 与 WAN/LAN 全正常。也就是说
v9 要验的那件事（引擎能不能在真机上以模块方式活下来）**答案是能**。

但同一次开机里，守卫把自己判成了"上一次启动内核卡死"，因此这一条必须连同证据一起记下来，
否则下一次谁看到 `/etc/e2631-ppe-failed` 都会以为是引擎的锅。

**症状（同一台机器、同一次开机，日志原文按时间顺序）**

```
01:15:22 e2631-ppe-boot[3057]: 引擎已加载；60 秒后…撤掉 pending 标记
01:15:27 e2631-ppe-boot[...]: 上次加载没有确认（内核可能卡死过），本次跳过引擎
01:15:32 … 01:16:18  同上，每 5 秒一条，3 分钟里 36 条
/etc/e2631-ppe-pending  0 字节      ← 第一个实例刚写的
/etc/e2631-ppe-failed   93 字节     ← 每 5 秒被重写一次
```

第一次实例在 20 秒延迟后写下 pending 并 `modprobe` 成功，然后它 fork 出 60 秒确认子进程、
自己 `exit 0`。**procd 在 5 秒后又把它拉起来**，新实例一进门就看到 pending（上一个实例刚写的），
按设计判成"上一次启动没确认"→ 跳过引擎、写 failed。此后每 5 秒一次，直到盒子重启；
每次都在 overlay 上重写一个 93 字节文件（overlay 在 NAND 上，这是实打实的无谓擦写）。

**两个各自独立、叠在一起才致命的缺陷**

1. `etc/init.d/e2631-ppe` 里的 `procd_set_param respawn 0` 不是"不要 respawn"。
   真机上 `ubus call service list '{"name":"e2631-ppe"}'` 直接给出
   `"respawn": {"threshold": 0, "timeout": 5, "retry": 5}`——**threshold 0 是"永远重启"**，
   timeout 5 就是那 5 秒间隔。对照隔壁 `e2631-wifi-load`：它**没有**任何 `respawn` 行，
   同一份输出里就没有 `respawn` 键——procd 的默认是"不管"，一次性脚本要的正是这个。
2. pending 标记只表示"有文件在"，没有**启动身份**。任何第二个调用者都会把
   "本次启动正在加载"误读成"上次启动没确认"。文件存在性不是启动身份。

**修法（两个都改了，缺一不可）**

- 去掉了 `respawn 0` 那一行（一次性脚本不该被监管），并在脚本注释里写明为什么"删一行"是修复。
- `sbin/e2631-ppe-boot` 的 pending 首行改写成 `/proc/sys/kernel/random/boot_id`：
  - 首行与本次 boot_id **相同** → 本次启动已经认领过，重复调用直接退出，什么都不写；
  - **不同** → 真的是上一次启动没确认，跳过引擎并写 `failed`；
  - 认领用 `set -C`（noclobber）原子创建，抢输的实例安静退出，不会误写 failed；
  - 60 秒确认子进程只撤**属于自己这次 boot 的**标记，避免跨启动误清。

**验证**：`05-路由器操作脚本/ppe-guard-test.sh` 在真机上 10 项全过（pending 首行是本次
boot_id / 正常路径不写 failed / 同启动重复调用被识别且不写 failed / 确认窗口到点撤标记 /
日志有确认记录 / 无 failed 残留 / 旧 boot_id 触发跳过 / failed 写明原因 / 跳过时保留 pending），
随后冷重启复验：一次开机只有一条"引擎已加载"，60 秒后 pending 自动消失，`failed` 不出现。

修好后的产物是 v9c：`02-固件产物/e2631-openwrt-v9c-ppe-20261029-slot1.bin`
（slot1、`serial=43`、`V1.0.0.8B8.9800`、sha256 `bc75048d…6fd7`），
经 `sysupgrade -n` 刷进 slot1 后冷启动复验通过，机器现在跑的就是它。

**教训**

1. "文件在 = 上次失败" 这种守卫必须带**启动身份**（boot_id / 开机计数），否则它把
   自己的正常执行当成故障证据。
2. procd 的 `respawn` 语义反直觉：**不写这行 = 不重启**，写 `respawn 0` = 永远重启。
   一次性脚本（加载器、探针、迁移脚本）要么不写，要么别用 procd。
3. 守卫的失败路径也会写 flash。一个"永远不会自己好"的失败标记如果落在被反复重试的
   路径上，等于拿 NAND 擦写寿命换日志。

## WANPORT-RELOAD-POLICY - 切 WAN 口默认不动运行态（2026-10-29）

`e2631-wanport apply <port>` 现在有两道保护，都是吸收上游 v1.8.0 的做法：

1. **目标口有链路就拒绝**（`--force` 绕过）。`port != 当前 WAN 口 && carrier == 1`
   时直接报错退出，因为真机上最可能的情况就是这根网线正是你此刻的管理连接
   （2026-10-27 那次就是把自己切到门外，WAN 区 input=REJECT，只剩 ping）。
   LuCI 页面走的是 `apply <port> --force`：页面本身就是显式选择，而"把 ISP 网线
   插到 lan3 再选 lan3"这个正常流程里目标口**必然**有链路，不加 --force 就永远
   切不过去。
2. **默认不 `network reload`**。配置写进 uci、端口 MAC 立刻落地，但运行态留给
   reboot；要立刻生效才加 `--reload`。

第 2 条的取舍来自两边互相矛盾的真机证据，如实记在这里：

- 上游 v1.8.0 记录：这颗 SoC 的 gmac/bridge 扛不住 `network reload/restart`，
  会半挂死（ICMP 部分应答、TCP 全死）。所以他们的 worker 不 reload。
- 我们在 2026-10-29 自己的机上（slot1 serial 41）连续 `apply lan3` → `apply wan`
  两次，每次都有 reload，SSH 与 ping 全程正常，`ubus call network.interface.wan
  status` 立刻给出 `"device": "lan3"`。

两条都真，说明风险与具体固件/流量状态有关。默认取保守（不 reload），把"立刻生效"
做成显式开关，而不是删掉这个能力。

## KERNEL-EXTENT-NOT-A-UIMAGE - v9/v10 为什么起不来（CRC 一致不等于能启动）（2026-10-29）

这是这台机器第二次"看起来死机"的真正原因，也是 2026-10-29 那次自锁的结论性更正
（见下面的 PPE-AUTOLOAD-BOOT-LOCKOUT 条目）。**内核从头到尾没有被加载过**，不是加载
之后挂死。

**证据（v8 与 v9 的包逐字节比较，两个包都在 `02-固件产物/`）**

| 观测点 | v8（能启动） | v9（起不来） |
|---|---|---|
| 槽 +0x20 处（`kFileOff` = `0x234`） | `27 05 19 56`（legacy uImage） | `00 00 a0 e1`（裸 zImage 的 ARM 跳转指令） |
| uImage 头 | 有，`ARM OpenWrt Linux-6.18.55`，load = entry = `0x40008000` | 没有 |
| 追加 DTB | 有，uImage 载荷末尾 10,823 B（`d00dfeed` @ 载荷 +0x3501b0） | 没有（`0x944` 那个 magic 是压缩数据误命中，全镜像是零个真 DTB） |
| 内核载荷长度 | 3,484,663 B | 3,470,747 B（去尾部零填充） |
| 32 字节槽标记 | `33333333cccccccc…`，在包内 `0x214` | 一模一样，也在 |
| `kSize` / `kFileOff` / `rFileOff` | `0x3fffe0` / `0x234` / `0x400214` | 完全相同 |
| `kCrc` | `0x00be98da` | `0x7d04b973`（对错内容重算，仍然自洽） |
| rootfs | rCrc `0x81b0df59`（rSize `0xa60000`） | rCrc `0x6996884b`（rSize `0xa60000`，另一份构建） |

**为什么 v9 会变成裸 zImage**：`tools/mkfactory.mjs` 的 `assemble()` 原本把"输入内核
前 32 字节是不是槽标记"当作可选——是就剥掉再补，不是就直接补一个标记。所以 `--kernel`
喂成 `build_dir/.../arch/arm/boot/zImage`（还没走过镜像配方的 `append-dtb | uImage none`
两段）时，打出来就是 `[标记][裸 zImage]`：标记还在，uImage 头和 DTB 一起丢了。

**引导器这边的因果链**（串口实拍，`10-私密记录/box-boot-2026-10-29.log` 第 161/201 行）：

```
pdt_getreal_kernelfs:539 verify_kernel readflash @0x2300000 size:0x400000 at ram:0x42020020
csp_crc:103 ... crc=0x82fb468c, 0x7d04b973      ← 与头里 kCrc 一致，"校验通过"
zteboot_verify_kernel:196 verify kernel success!!
## Booting kernel from Legacy Image at 42020020 ...
Wrong Image Format for bootm command
pdt_cspboot_run:1091 Start kernel failed
pdt_cspboot_run:1094 No image found!
```

`kCrc` 只能证明"存的字节就是头里描述的那些字节"，证明不了"这些字节是内核"。于是
两道引导校验全过、`bootm` 一步拒绝，内核一个字节都没执行。盒子表现为网口链路
1000M/Full（那是 U-Boot 把交换侧拉起来的状态）、零 ARP、抓包一帧都没有——**和"内核
跑起来之后挂死"在外部完全不可区分**，所以只能靠串口判。

**为什么当时没人拦住**：`host/verify_pkg.mjs` 本来就会拒（`release.mjs` 打包后必跑它，
失败即 `do not flash it`）。拿它跑 v9，四条直接红：

```
FAIL  kernel extent begins with a legacy uImage magic
FAIL  uImage os=Linux arch=arm type=kernel comp=none   os=0 arch=0 type=160 comp=225
FAIL  load == entry == 0x40008000                       0xa0e1 / 0xa0e1
FAIL  uImage data CRC (ih_dcrc) self-checks             0xa0e1 vs 0x51feb6a
```

v9/v10 是绕过 `release.mjs`、直接调 `mkfactory.mjs pack` 出的包，所以这四条从产物
落地那一刻就成立，只是没人跑那一步。

**现在 `mkfactory.mjs` 里的三道闸**（都在 `assemble()` 里，对 `pack` 强制，`selftest`
回放原厂包时用 `allowNoDtb: true` 放行）：

1. 剥完标记后 `+0x00` 必须是 `27051956`，否则 die——裸 zImage 不能再靠"补个标记"混过去；
2. `ih_size + 64 <= 长度`，且 `crc32(uImage 载荷) == ih_dcrc`，否则 die（`bootm` 会以
   `Bad Data CRC` 拒绝）；
3. uImage 载荷必须**以一棵完整的 DTB 收尾**（`d00dfeed` 的 `total_size` 正好补齐到
   载荷末尾），否则 die。这一条单独立出来是因为 v11 的教训：v11 把 uImage 头补回去
   了、`ih_dcrc` 也对，但载荷是没有 DTB 的裸 zImage，ARM32 拿不到 device tree 会在
   解压后**静默挂死**——一样不可启动，只是死法不同。

自测（本机跑过）：v8 的内核打包通过并 `post-write verify` 全 ok；v9、v11 的内核都被
拒且给出具体理由；`mkfactory.mjs selftest --template 06-分析与对比/原厂包分析/fwinspect/vendor.bin`
仍然 `byte-exact: YES`。

**教训**：打包器的输入必须是"能启动的内核镜像"，而不是"某个叫 zImage 的文件"；
校验链要一路检到"这东西能不能 boot"，而不是停在"CRC 对得上"。

## VM-FIREWALLD-BLOCKS-TFTP - U-Boot 的 TFTP 被防火墙以 admin-prohibited 拒掉（2026-10-29）

现象：U-Boot 里 `ping 192.168.1.250` 是 `alive`，但
`tftp 0x43000000 init.uImage` 只有一行 `Loading: *T T T T …`，五轮 `Retry count
exceeded` 之后回到 `=>`。TFTP 服务端日志里一个请求都没有。

定位方式（`AF_PACKET` 抓 ens34，不用装 tcpdump）：RRQ 确实到了网卡上——

```
UDP  192.168.1.10>192.168.1.250 3507>69 ulen=49 uck=0x0000
ICMP 192.168.1.250>192.168.1.10 type=3 code=13 len=97     ← 紧跟其后 0 ms
```

`code 13` = administratively prohibited，即**本地策略拒绝**，不是"端口没人听"（那是
`code 3`）。查下来 `ens34` 没有绑定区域，落在默认的 `public` 区，而 `firewalld` 的
`public` 是 `target: default`：入站外来连接一律 reject，只放行了 `3080/3099/3100/tcp`
——ICMP echo 默认放行，所以 ping 通、UDP 69 全灭。

修法（运行时生效，重启失效；要长期就加 `--permanent` 再 `--reload`）：

```sh
firewall-cmd --zone=public --add-port=69/udp
firewall-cmd --zone=public --list-ports      # 确认 69/udp 在列表里
```

放行后同一台机器、同一条线：`Bytes transferred = 13371783`，`886.7 KiB/s`。

判据速记：U-Boot 的 `Loading: *` 那个 `*` 是"收到了 ICMP/非 TFTP 包"，随后连续 `T`
是超时；**`ping` 通而 `tftp` 全 `T`，先查主机防火墙**，不要去怀疑串口、MAC 或 U-Boot
网口驱动。

## OVERLAY-BACKUP-TTYD-CORRUPT - 回灌用的 ttyd 配置是二进制垃圾（2026-10-29）

`03-固件备份/openwrt-overlay-from-backup/upper/etc/config/ttyd` 只有 59 字节，内容是
`\0\5\032\234\0?I\306Yk9.\274\347 -…` 这样的二进制，不是配置文本：抓取 overlay 那一步
就把它写坏了。镜像里的正常内容是：

```
config ttyd
	option interface '@lan'
	option command '/bin/login'
```

后果（回灌后逐项核对时发现）：`uci commit ttyd` 报
`uci: Parse error (invalid command) at line 0, byte 0`，`uci show ttyd` 全空，ttyd
服务起不来（不影响 SSH/网络，所以很容易漏掉）。

已处理：把镜像里的这份配置写回盒子并 `uci commit ttyd`；仓库里的备份文件也换成同样
内容，避免下一次回灌再传染。

顺手记一个核对手法：回灌后逐个跑

```sh
for p in $(ls /etc/config); do msg=$(uci commit "$p" 2>&1); [ -n "$msg" ] && echo "$p: $msg"; done
```

`*.bak-ula` / `*.bak-guard` 这类备份文件报错是正常的（它们本来不是配置），其余任何一条
报错都说明回灌带进去了坏文件。

## VENDOR-ROOTSH-NO-DEVICE-NODES - 改版原厂系统起不来的根因：341 个 /dev 节点被写成了普通文件（2026-10-30 复盘）

### 现象

把"原厂系统 + root shell"的包刷进 slot1 后：cspboot 两道 CRC 都过
（`verify kernel success!!`、`verify fs success!!`），内核与原厂逐字节相同
（`k_crc 0x2edf94cd`，与 slot0 一致），但 `Starting kernel ...` 之后约 10 秒整机复位，
串口一个字节的内核日志都没有，然后无限循环。slot0 未被动过。

### 先纠正上一轮的两个错误判断

这两个判断当时都被当成结论写进了记录，事后都被真机推翻了：

1. **"bootargs 里的 `console=$(console)` 没被展开，所以内核没有控制台"——错的。**
   U-Boot 的命令行处理器会展开 `$(...)`。在 U-Boot 里实测：

   ```
   => echo [$(console)]
   [ttyAMA0,115200n8]
   ```

   cspboot 打印的 `cmd=setenv bootargs console=$(console) ...` 是**展开前的命令原文**，
   不是最终的 bootargs。真正传给内核的是 `console=ttyAMA0,115200n8 root=/dev/mtdblock8
   ro rootfstype=jffs2 mem=256M`，是对的。

2. **"`Starting kernel ...` 之后静默 = 内核崩了"——错的。**
   用同样的方式让 cspboot 选 slot0 启动**未改动过的原厂固件**：串口同样一片空白，
   但系统正常起来——`192.168.5.1` 的 80/443 可达、原厂登录握手成功
   （`SoftwareVer=V1.0.0.8B8.9201`）、ping 0.7 ms。原厂 4.1.25 内核根本不往 ttyAMA0
   打内核日志，所以**"静默"什么也证明不了**，能作为"死没死"依据的只有两件事：
   有没有复位（掉电重启循环）、网络有没有起来。

### 根因

离线把两边镜像的 JFFS2 节点逐个数出来对比：

| inode 类型 | 原厂 slot0 | 旧产物（刷进去的那份） |
|---|---|---|
| 目录 | 47 | 47 |
| 符号链接 | 23 | 23 |
| **字符设备** | **173** | **0** |
| **块设备** | **168** | **0** |
| 普通文件 | 107 | 449 |

原厂 rootfs 的 `/dev` 里**预置了 341 个设备节点**（`console`、`null`、`zero`、
`mtd0-14`、`mtdblock0-14`、`ttyAMA0`、`FeedDog` 看门狗……），因为 vendor 内核
**没有 devtmpfs**，整个 `/dev` 就靠这份静态节点。旧产物的打包链路是：

1. `jffs2_tree.py` 解包时把设备节点写成了 `<名字>.chr` / `<名字>.blk` 的**空普通文件**；
2. 重建目录树时没把它们还原成真节点，`mkfs.jffs2` 就照普通文件写回镜像。

于是启动时：内核挂上 JFFS2、执行 `/sbin/init`，init 去开 `/dev/console` —— 那是一个
只读文件系统上的空普通文件，打不开 → init 死 → panic（然后被看门狗复位）。这一路
全在 userspace 早期，正好符合"内核跳进去约 10 秒整机复位、串口无输出"的表现。

设备号在 JFFS2 inode 里存成 **2 字节小端 `(major<<8)|minor`**，已核对：
`/dev/console`=5:1、`/dev/ttyAMA0`=204:64、`/dev/mtdblock8`=31:8、`/dev/mtd8`=90:16。

顺带说明另外 341 个多余 inode 的来历：`.chr`/`.blk` 占位文件还留在树里，
所以旧镜像比原厂多出 341 个 inode + 341 个 dirent（约 44 KB）。

### 次要问题：压缩器选错

旧打包命令用了 `-m size`（按压缩率挑压缩器），结果 272 个数据节点用了 `rtime`
（compr=2，集中在 `etc/wireless/mt7916/*` 的 WiFi 校准/固件文件上）。原厂镜像只有
`none(0)` 和 `zlib(6)`。vendor 内核里能看到 rtime 压缩器的名字，所以它八成能解，
但这属于无必要的偏离，改回原厂默认的 `-m priority` 并禁掉 lzma（vendor 内核里
连 `lzma` 字符串都没有）。

### 修法与验证

`09-工具脚本/build-vendor-rootsh.py`：从原厂镜像读回每个设备节点的
(类型, mode, uid, gid, major, minor)，用 `os.mknod` 建真节点；删掉 `.chr`/`.blk`
占位；按 `-m priority -x lzma` 出镜像；版本头整份沿用原厂 slot0 的头，只改
body 长度 / fs_crc / 槽起点 / serial / H_NEW / 两个 CRC。

产物 `02-固件产物/vendor-root-shell/e2631-vendor-rootsh-slot1-fixed.pkg`
（21,234,196 B）自检全过：

- inode 类型 `{目录 47, 链接 23, 字符 173, 块 168, 普通 108}`（普通 =107+1，多的是 `/sbin/shd`）
- 压缩分布 `{none 3930, zlib 1969}`（原厂 `{3930, 1964}`，多的 5 个是 shd）
- 镜像长度正好 `0x1100000`
- 解包回来与原厂树只差四处：`etc/init.norm`、`etc/init.debug`、`etc/shadow`、`+sbin/shd`
- 盒子上 `e2631-fwslot check` → `ok: valid E2631 package for ZXHN E2631 V1.0.0`

### 一条已排除的差异（留档，别再追）

原厂 slot0 的头块（`0x1440000` 那 128 KiB）在 512 字节版本头之后还有约 **127 KiB 高熵
数据**，用 rootfs 的 AES key 解不开；但原厂 slot1 的头块（`mtd9_slot1-header.bin` 备份）
只有 512 字节头、没有这块。也就是说原厂自己的升级流程也不写它，判定与启动无关。

### 下一步验收

把 fixed 包刷进 slot1（`e2631-fwslot flash <pkg> --slot 1`），预期：

- 串口**仍然静默**（原厂内核不打日志），所以别拿串口判断死活；
- 判据换成网络：起来后 LAN 是 `192.168.5.1`，WebUI 80/443 通；
- 然后 `telnet <IP> 2323` 拿 root shell（`/sbin/shd`，口令见 `shd.c`）；
- 若依然 10 秒复位，则剩下的唯一变量就是"内容"本身（3 个改动 + shd），按
  `/etc/init.norm` → `/etc/shadow` → `shd` 的顺序二分。
