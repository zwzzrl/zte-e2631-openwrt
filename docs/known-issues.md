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
      What this changes: every CLA-hash sweep for v6 (v6_key, v6_hwn 7..19, v6_type 0..7,
      v6_idxw4, shotgun over all 384 slots) was aimed at an engine the vendor does not
      use for IPv6 forwarding - which is exactly why all of them measured 0 hits while
      the far end counted every forwarded frame. The next target is the 48-byte switch
      IPv6 table, and three things are still open before code: the table id / index
      coding in sbrg_set_indreg_cmd and sbrg_hash, the 48-byte entry layout, and the
      physical address behind ONU 0x000e2007 (the CLA window we do control is at
      0x9238c000, so the conversion rule is one register description away).
    * Once the key or the table is right: measure a *TCP* IPv6 flow end to end (the
      bench's only peer filters inbound IPv6, hence UDP above), then decide whether
      ipv6=1 becomes the default.
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
