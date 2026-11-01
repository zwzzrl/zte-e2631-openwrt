// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s Ethernet switch: hardware flow offload
 *
 * Copyright (C) 2026 Navid Ghahremani <ghahramani.navid@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/rhashtable.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <net/flow_offload.h>

#include "zx279128s-ppe.h"

/* ---- Hardware flow offload ------------------------------------------ */

/*
 * The packet processor forwards established IPv4 TCP/UDP flows without the
 * CPU: the classifier looks the 5-tuple up in two hash banks, the packet
 * modifier rewrites MACs, addresses, ports and TTL, and the entry sends
 * the frame straight to the egress port.  A miss goes to the CPU as usual.
 * Formats reverse engineered from the stock firmware's NPU tables.
 *
 * The WAN is a plain switch port here, so the hardware treats both
 * directions of a connection as upstream: both use the upstream key
 * header, and each direction has its own flow (rewrite) index.
 *
 * IPv6: the classifier has an IPv6 extract index (10, the only value that
 * works) and a rule, and the driver can program them (zx_ppe_ipv6, default
 * off).  With them in place IPv6 forwards exactly as well as with the trap on
 * - measured, not assumed - but the hash entry is never matched, so there is
 * no fast path yet.  The stock firmware's rule 2 re-encoded is not the whole
 * story: the vendor builds the key itself in software from per-family
 * descriptors.  Details in docs/known-issues.md (PPE-IPV6-TCP).
 *
 * This file is package kmod-e2631-ppe, not part of the kernel: the classifier,
 * the hash banks and the rewrite tables are exactly the part that keeps being
 * worked on, and a change here has to rebuild one .ko instead of the whole
 * kernel.  The switch driver (zx279128s-main.c) stays built in and reaches this
 * engine through the hooks table in zx279128s-ppe.h; what must not move across
 * the boundary is anything the kernel keeps a pointer to (the flow_block
 * callbacks), which is why ndo_setup_tc lives on that side.
 */

static bool zx_ppe_ipv6;
MODULE_PARM_DESC(ipv6,
	"program the IPv6 classifier tables and lift the IPv6 ethertype trap (writable at runtime)");

/*
 * Writable at runtime, and that is the point of it: the IPv6 ethertype trap is
 * what makes *every* IPv6 frame cross the CPU, including the ones the switch
 * would happily bridge (measured 2026-10-28: 2,000 LAN-to-LAN IPv6 UDP frames,
 * trap on = 2,018 packets through the CPU, trap off = 18).  Flipping this
 * switches between "IPv6 on the CPU but nothing can be missed" and "IPv6
 * switched in hardware, routed IPv6 through the classifier and then the CPU".
 * Both keep IPv6 working (see docs/known-issues.md, PPE-IPV6-TCP); the second
 * is faster for LAN traffic.  A box that wedges can be recovered with
 * "echo 0 > /sys/module/zx279128s_ppe/parameters/ipv6" or a power cycle.
 */
static int zx_ppe_ipv6_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops zx_ppe_ipv6_ops = {
	.set	= zx_ppe_ipv6_set,
	.get	= param_get_bool,
};
module_param_cb(ipv6, &zx_ppe_ipv6_ops, &zx_ppe_ipv6, 0644);

static bool zx_ppe_debug;
module_param_named(debug, zx_ppe_debug, bool, 0644);
MODULE_PARM_DESC(debug,
	"log every offloaded flow and the first hardware hit of each hash entry");

/*
 * Diagnostic only: the value the driver shall write to the ethertype trap
 * register, -1 to leave that to zx_update_traps().  A trapped ethertype may
 * take the frame away from the classifier, which would make the fast path
 * unobservable no matter how right its tables are -- the two experiments
 * that tell the two cases apart are "trap IPv4 too and see whether IPv4
 * offload stops" and "stop trapping IPv6 and see whether it starts".
 */
static int zx_ppe_trap_override = -1;
MODULE_PARM_DESC(trap_override,
	"32-bit value for the ethertype trap register (0x0800<<16 | 0x86dd masks), -1 = driver decides");

static void zx_ppe_apply_trap_override(struct zx_eth_adapter *adapter)
{
	if (!adapter->ppe_active || zx_ppe_trap_override < 0)
		return;
	writel(zx_ppe_trap_override, adapter->base + ZX_SPA_TRAP_ETH_TYPE);
}

static void zx_ppe_trap_reapply(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	zx_ppe_apply_trap_override(adapter);
	mutex_unlock(&adapter->ppe_lock);
}

/*
 * Writing this parameter takes effect immediately, which is what makes the
 * trap question answerable on a live box: trap the family, let the first
 * packets reach the CPU (and thereby create the flow), then stop trapping and
 * send the same five-tuple again.  If the classifier owns those frames the
 * hardware forwards them from its hash entry and the CPU transmit counter
 * stays flat; if the trap was the only reason anything worked, they vanish.
 */
static int zx_ppe_trap_override_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	zx_eth_for_each_adapter(zx_ppe_trap_reapply, NULL);
	return 0;
}

static const struct kernel_param_ops zx_ppe_trap_override_ops = {
	.set	= zx_ppe_trap_override_set,
	.get	= param_get_int,
};
module_param_cb(trap_override, &zx_ppe_trap_override_ops, &zx_ppe_trap_override, 0644);

/*
 * Diagnostic knobs for the IPv6 fast path, all of them "default = what the
 * driver ships".  The v4 tables are proven on hardware, so every IPv6 value
 * that differs from its v4 sibling is a guess; these let a guess be changed
 * on a loaded module instead of in a 45-minute kernel rebuild, and v6_ext,
 * v6_rule and v6_key exist because where the hardware looks up that rule -
 * and which id it puts into the hash key - is exactly what is unproven.
 */
/*
 * The index the stock firmware hangs its IPv6 extract on, and the only one
 * that means anything to the hardware: it is what tells the classifier that
 * an IPv6 packet is a packet it has to look at.  Reading the old sweep by
 * "did the CPU stop seeing the packets" got this exactly backwards -- the
 * index 10 run sent everything through the CPU *because* the extract worked
 * and the hash lookup missed, while 0/1/2/3/8/9/11..15 dropped the frames
 * without ever looking at them (2026-10-28, measured again with a far end
 * that counts what arrives: 2800 of 2800 packets arrive with index 10 and
 * none with index 11).  So 10 it is, and 11 must never be the default.
 */
static int zx_ppe_v6_ext_idx = 10;
module_param_named(v6_ext, zx_ppe_v6_ext_idx, int, 0644);
MODULE_PARM_DESC(v6_ext, "classifier extract index the IPv6 rule is hung off");

/* 已废弃，仅历史兼容 (deprecated, historical compatibility only): the entry
 * byte is derived from the extract index by v6_hwenc, and the old value it
 * used to produce - (v6_entry << 4) | 8 = 0xA8 - was measured not to match.
 * Kept so the older logs' repro commands still resolve.
 */
static int zx_ppe_v6_entry = -1;
module_param_named(v6_entry, zx_ppe_v6_entry, int, 0644);
MODULE_PARM_DESC(v6_entry,
	"extract index byte inside the IPv6 hash entry (-1: same as v6_ext; deprecated under v6_hwenc)");

static int zx_ppe_v6_rule_idx = ZX_PPE_V6_RULE;
module_param_named(v6_rule, zx_ppe_v6_rule_idx, int, 0644);
MODULE_PARM_DESC(v6_rule,
	"extra rule table entry the IPv6 windows are written to (deprecated under v6_hwenc: the vendor site is 2)");

static int zx_ppe_v6_key = -1;
module_param_named(v6_key, zx_ppe_v6_key, int, 0644);
MODULE_PARM_DESC(v6_key,
	"id the IPv6 hash key carries (-1: same as v6_rule; deprecated under v6_hwenc: the key carries the extr value)");

/*
 * C1, IPv6 self-consistency (default off).
 *
 * The vendor builds the 45-byte hash key and the 60-byte entry's extr_index
 * from one and the same value, in one instruction stream: tm_acl_fast_add_v4v6
 * 0x4d7f4..0x4d810 computes it, 0x4d61c..0x4d638 writes it into the key
 * (bit23..30), 0x4d71c..0x4d748 writes it into the entry (+15[7:4] and
 * +16[3:0]).  For IPv6 the value is (ext_idx == 10) ? 2 : ext_idx*16 + 2;
 * for IPv4 it is ext_idx*16.  (Re-checked against the vendor tm.ko with the
 * OpenWrt cross objdump on 2026-11-02, see the v6-selfconsist notes.)
 *
 * This driver has always had two independent knobs instead: v6_rule feeds the
 * key, v6_entry plus w[3]'s top nibble feed the entry.  Their defaults
 * disagree - key 0x99, entry 0xA8 - so the "same value in both places"
 * contract the vendor keeps is violated.  With this on, one function derives
 * both sides and the entry's two half-nibbles are patched from it.  Off by
 * default so the shipped behaviour is unchanged; it is an experiment.
 */
static bool zx_ppe_v6_selfconsist;
module_param_named(v6_selfconsist, zx_ppe_v6_selfconsist, bool, 0644);
MODULE_PARM_DESC(v6_selfconsist,
	"IPv6: derive key ex_rule_id and entry extr_index from one vendor-style value (deprecated: v6_hwenc is the delivered form)");

/*
 * The IPv6 encoding the hardware was measured to accept.  On 2026-11-02 a
 * sweep module (independent of this driver) spread one IPv6 entry over all
 * 520 hash slots and then watched the classifier's aging bits: with three
 * values that are all 2 - the extract-rule table entry the vendor's v6 rule
 * lives in, the 60 B entry's extr_index (+15[7:4] / +16[3:0]) and the 45 B
 * key's ex_rule_id (bit23..30) - and a 33-bit zero key header, the hardware
 * hit slot 193, which is exactly zx_ppe_crc(0x04c11db7, key, 45) & 0xff for
 * that flow.  Each of 0x99 / 0xA8 / 0xA2 / rule 153 / a non-zero header was
 * measured not to hit, and every hit variant immediately took the flow's
 * frames away from the CPU, so the entry really is used for forwarding.  See
 * .scratch/ffe-fastpath/cla-v6-v2/20261102-0510/RESULT.md.
 *
 * This parameter derives the three values from one number, the way the vendor
 * does in one instruction stream (tm_acl_fast_add_v4v6 0x4d7f4..0x4d810):
 * key rule byte == entry extr_index == the rule table id, which for the v6
 * extract index 10 is 2.  Off by default: it is the one switch that changes
 * what the shipped driver writes, and a wrong action word in w0 (the egress
 * port) black-holes every flow the entry catches, so arming it is a
 * deliberate act on a box whose serial console is reachable.
 */
static bool zx_ppe_v6_hwenc;
static int zx_ppe_v6_hwenc_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops zx_ppe_v6_hwenc_ops = {
	.set	= zx_ppe_v6_hwenc_set,
	.get	= param_get_bool,
};
module_param_cb(v6_hwenc, &zx_ppe_v6_hwenc_ops, &zx_ppe_v6_hwenc, 0644);
MODULE_PARM_DESC(v6_hwenc,
	"IPv6: write the hardware-verified encoding (rule 2 / extr 2 / key rule 2, zero header); 0 = the old 0x99/0xA8 shape");

/*
 * One-button undo, part 1: on stop (rmmod, or the unbind of the last
 * flowtable) zero all 520 hash slots and their aging words.  The classifier
 * keeps its entries in switch SRAM that survives a module reload, so a run
 * that spread one entry over many slots (v6_shotgun) would otherwise leave
 * entries behind that no bookkeeping knows about, and the next load would
 * find the hardware full while its own bitmap says empty.  Only active
 * together with v6_hwenc, because that is what turns such a run on.
 */
static bool zx_ppe_v6_wipe = true;
module_param_named(v6_wipe, zx_ppe_v6_wipe, bool, 0644);
MODULE_PARM_DESC(v6_wipe,
	"zero all 520 hash slots when the engine stops (only with v6_hwenc, default 1)");

/*
 * Post-hit IPv6 probes (default 0: the delivered behaviour, bit for bit).
 *
 * The hit is not in doubt any more: rule 2 / extr 2 / key rule byte 2 / zero
 * key header lands in the slot the software computes, and the aging bit goes
 * up under traffic.  What fails is everything *after* the hit - the frames are
 * taken away from the CPU and the far end never acknowledges a byte of them,
 * and the five action words that were A/B'd on the box all fail the same way.
 * The two knobs below are the two post-hit inputs a table entry can still
 * change; both are per-flow or table-only, neither needs a reboot to undo.
 *
 *   bit0  do not set the packet modifier's "decrement TTL / hop limit" bit
 *         (bit 34 of the 12-byte rewrite instruction) for an IPv6 flow.  The
 *         instruction has no "this header is IPv6" field - the only
 *         family-aware bits are the two checksum-update bits, which are
 *         already cleared for v6 - so if the modifier resolves bit 34 with the
 *         IPv4 offsets it decrements byte 8 of the IPv6 header, and byte 8 is
 *         source address byte 0.  The frame still leaves the box and the far
 *         end drops it silently (the transport checksum covers the source
 *         address and is not recomputed), which is exactly the observed shape.
 *         Applies to flows created after the write.
 *
 *   bit1  write extract index 10's word 4 with ext_mode = 2 (0x00190154
 *         instead of 0x00150154), the value the vendor's aclRamInit puts in
 *         its v6 groups 1..8 while groups 9/10 get 1.  This keeps index 10,
 *         the 16 field ids, the 45-byte key and therefore the slot unchanged
 *         (a hit that works today keeps hitting) and flips only the flag that
 *         may select the post-hit parse template.  It is a hybrid the vendor
 *         never writes - that is what makes it a one-bit experiment - and if
 *         ext_mode also drives extraction the hit dies, which the aging bit
 *         says within a second.  The entry is read back before the first write
 *         and put back word for word when the bit is cleared.
 */
#define ZX_PPE_POSTHIT_NOHOP	BIT(0)
#define ZX_PPE_POSTHIT_EXTMODE	BIT(1)
static unsigned int zx_ppe_posthit;
static int zx_ppe_posthit_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops zx_ppe_posthit_ops = {
	.set	= zx_ppe_posthit_set,
	.get	= param_get_uint,
};
module_param_cb(v6_posthit, &zx_ppe_posthit_ops, &zx_ppe_posthit, 0644);
MODULE_PARM_DESC(v6_posthit,
	"IPv6 post-hit probes (bitmask, default 0): 1 = do not decrement the hop limit, 2 = extract index 10 ext_mode 2");

/* The vendor's IPv6 extract-rule landing site.  Measured empty on this box: a
 * read of rule table entry 2 in the delivery state (2026-11-02, uptime 4825 s,
 * IPv6 classifier programmed) came back as 17 zero words, and this driver has
 * only ever written 0x99 (ZX_PPE_V6_RULE).  It is still read back and saved
 * before the first write and put back word for word afterwards: "reads as
 * zero" is a measurement, not a promise about the next boot.
 */
#define ZX_PPE_V6_HW_RULE	2

static unsigned int zx_ppe_v6_w1;
module_param_named(v6_w1, zx_ppe_v6_w1, uint, 0644);
MODULE_PARM_DESC(v6_w1, "word 1 of the IPv6 entry (0: 0xfa11c000)");

/*
 * w0's two variable fields as overrides, because which rewrite index and which
 * egress port a *hit* needs can only be settled by one boot testing several
 * candidates, and a rebuild per candidate costs a module load each time.
 *
 *	-1 / 0  derive: the rewrite index is the one this flow allocated and
 *	        programmed (zx_ppe_alloc(), 1..127, the v4-proven mechanism),
 *	        the egress is ZX_BRPORT() of the flow's redirect device.
 *	v6_flow >= 0  put that index into w0 instead.  0 is the vendor's golden
 *	        value and means "do not rewrite": no MAC swap, no hop-limit
 *	        decrement, i.e. the frame leaves as the host addressed it.
 *	v6_w0 != 0  use that word 0 verbatim and ignore both fields.
 *
 * A new value only affects flows added afterwards - the entry is assembled
 * once, at flow-add time - so an A/B needs a new flow (the runbook uses a new
 * --cport per candidate).
 */
static int zx_ppe_v6_flow = -1;
module_param_named(v6_flow, zx_ppe_v6_flow, int, 0644);
MODULE_PARM_DESC(v6_flow,
	"IPv6: rewrite index in w0 (-1: the per-flow one the driver programmed; 0 = the vendor's 'no rewrite')");

static unsigned int zx_ppe_v6_w0;
module_param_named(v6_w0, zx_ppe_v6_w0, uint, 0644);
MODULE_PARM_DESC(v6_w0,
	"IPv6: raw word 0 of the entry (0: flow | FWD | egress | 0x44 | BIT(3))");

static unsigned int zx_ppe_v6_w3;
module_param_named(v6_w3, zx_ppe_v6_w3, uint, 0644);
MODULE_PARM_DESC(v6_w3, "word 3 of the IPv6 entry (0: 0x80000000)");

static unsigned int zx_ppe_v6_tag_byte = 0x14;
module_param_named(v6_tag, zx_ppe_v6_tag_byte, uint, 0644);
MODULE_PARM_DESC(v6_tag,
	"byte 2 of the IPv6 entry key string (stock value 0x14; 0 = leave 0)");

static int zx_ppe_v6_idxw4 = -1;
module_param_named(v6_idxw4, zx_ppe_v6_idxw4, int, 0644);
MODULE_PARM_DESC(v6_idxw4, "word 4 of the IPv6 extract index entry (-1: shipped value)");

static int zx_ppe_v6_hashlen = -1;
module_param_named(v6_hashlen, zx_ppe_v6_hashlen, int, 0644);
MODULE_PARM_DESC(v6_hashlen, "hash length in the IPv6 rule (-1: shipped value)");

static int zx_ppe_v6_type = -1;
module_param_named(v6_type, zx_ppe_v6_type, int, 0644);
MODULE_PARM_DESC(v6_type, "offset type of every IPv6 window (-1: shipped value)");

/*
 * How many of the extract rule's windows the hardware feeds into the hash
 * key, and therefore how many halfwords the key holds.  For IPv4 that number
 * is 7 (protocol, four address halfwords, two ports) and it is encoded in the
 * rule's hash length field, so it doubles as the field's meaning: with all 19
 * IPv6 windows the field has to hold 19, and if the hardware caps the key at
 * some smaller size nothing ever matches.  Sweeping this is the cheapest way
 * to find the cap.
 */
static int zx_ppe_v6_hwn = -1;
module_param_named(v6_hwn, zx_ppe_v6_hwn, int, 0644);
MODULE_PARM_DESC(v6_hwn,
	"halfwords of the IPv6 key (windows hashed, -1: all 19)");

/*
 * Byte order inside one window.  The vendor's own IPv6 table entry is the
 * reference (tools/vendor-emu/t_v6win.py, and the 60-byte dump of a real run):
 * its first window holds the protocol in the *low* byte (e19 = 0x11, e20 = 0)
 * where this driver writes it into the high byte like the v4 mask suggests,
 * its port windows hold the two bytes in wire order, while its address
 * windows are the big-endian halfwords this driver already writes.  Either
 * the windows really are ordered differently for IPv6, or the vendor has a
 * different mask in the same rule id.  Rather than guess, each difference is
 * a bit that can be flipped on a loaded module:
 *
 *	bit 0: protocol in the low byte of window 0
 *	bit 1: swap the two port halfwords
 *	bit 2: swap the address halfwords
 *
 * With all bits clear the key is what earlier rounds wrote, so a hardware
 * test can walk from the old layout to the vendor's in three steps.
 */
static int zx_ppe_v6_fix;
module_param_named(v6_fix, zx_ppe_v6_fix, int, 0644);
MODULE_PARM_DESC(v6_fix,
	"IPv6 key byte order: bit 0 protocol low byte, 1 swapped ports, 2 swapped addresses");

/*
 * A window list of our own.  The extract rule is just a table, so instead of
 * reverse-engineering the layout the stock firmware's rule 2 was written with,
 * the driver can define the windows it wants: "offset[:mask]" per window,
 * offset in bytes from the start of the IPv6 header (the L4 ports live at 40
 * and 42 in that same buffer), mask in hex.  The rule table is patched to the
 * same list and the hash length set to the window count, so rule, key and
 * entry are always built from one description.
 *
 * This exists because a one-window rule ("6:ff00", the next-header byte) is
 * the cheapest possible question to the hardware: if even that never matches
 * an IPv6 flow, the classifier does not look up IPv6 hashes at all.  If it
 * does match, the list can be grown window by window until it stops.
 */
#define ZX_PPE_V6_WIN_MAX 19
static int zx_ppe_v6_win_ofs[ZX_PPE_V6_WIN_MAX];
static u16 zx_ppe_v6_win_mask[ZX_PPE_V6_WIN_MAX];
static int zx_ppe_v6_win_n;

static void zx_ppe_patch_bits(u8 *buf, int pos, int width, u32 val)
{
	int i;

	for (i = 0; i < width; i++, pos++) {
		if (val & BIT(i))
			buf[pos / 8] |= BIT(pos % 8);
		else
			buf[pos / 8] &= ~BIT(pos % 8);
	}
}

static void zx_ppe_v6_wins_apply(void);

static int zx_ppe_v6_wins_set(const char *val, const struct kernel_param *kp)
{
	char buf[128];
	const char *cur;
	size_t len;
	int n = 0;

	if (!val)
		return -EINVAL;
	/* sysfs hands the buffer over with its trailing newline */
	len = strscpy(buf, val, sizeof(buf));
	if (len < 0)
		return -EINVAL;
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
		       buf[len - 1] == ' '))
		buf[--len] = '\0';
	for (cur = buf; *cur && n < ZX_PPE_V6_WIN_MAX; cur++) {
		unsigned int ofs, mask = 0xffff;
		char *end;

		ofs = simple_strtoul(cur, &end, 0);
		if (end == cur || ofs > 46)
			return -EINVAL;
		cur = end;
		if (*cur == ':') {
			cur++;
			mask = simple_strtoul(cur, &end, 16);
			if (end == cur || mask > 0xffff)
				return -EINVAL;
			cur = end;
		}
		zx_ppe_v6_win_ofs[n] = ofs;
		zx_ppe_v6_win_mask[n] = mask;
		n++;
		if (*cur == ',')
			continue;
		if (*cur)
			return -EINVAL;
		break;
	}
	zx_ppe_v6_win_n = n;
	zx_ppe_v6_wins_apply();
	pr_info("zx279128s-ppe: v6_wins: %d window(s)\n", n);
	return 0;
}

static const struct kernel_param_ops zx_ppe_v6_wins_ops = {
	.set	= zx_ppe_v6_wins_set,
	.get	= param_get_int,
};
static int zx_ppe_v6_wins_storage;
module_param_cb(v6_wins, &zx_ppe_v6_wins_ops, &zx_ppe_v6_wins_storage, 0644);
MODULE_PARM_DESC(v6_wins,
	"custom IPv6 window list \"ofs[:mask],...\" (offset in bytes of the IPv6 header, mask hex)");

/*
 * Diagnostic: write the same IPv6 flow into a range of hash slots instead of
 * only the one zx_ppe_slots() computed.  That separates the two remaining
 * explanations for a flow the hardware never matches -- "the key string is
 * wrong" from "the key string is right and only the software's idea of the
 * slot is wrong" -- because if any slot hits, the string was fine.  Bisect
 * the range to find out which slot the hardware actually looks in.
 */
static bool zx_ppe_v6_shotgun;
static bool zx_ppe_v6_shot_ext;
module_param_named(v6_shot_ext, zx_ppe_v6_shot_ext, bool, 0644);
MODULE_PARM_DESC(v6_shot_ext,
		 "extend the IPv6 shotgun to the vendor's other three hash banks (slots 384..519, tables 4/5/6)");
module_param_named(v6_shotgun, zx_ppe_v6_shotgun, bool, 0644);
MODULE_PARM_DESC(v6_shotgun,
	"diagnostic: write an IPv6 flow into v6_shot_lo..v6_shot_hi instead of one slot");
static int zx_ppe_v6_shot_lo = -1;
module_param_named(v6_shot_lo, zx_ppe_v6_shot_lo, int, 0644);
MODULE_PARM_DESC(v6_shot_lo, "first slot of the shotgun range (-1: 0)");
static int zx_ppe_v6_shot_hi = -1;
module_param_named(v6_shot_hi, zx_ppe_v6_shot_hi, int, 0644);
MODULE_PARM_DESC(v6_shot_hi, "last slot of the shotgun range (-1: 383)");
/*
 * One conntrack entry offloads two flows, and the second add overwrites every
 * slot the first one wrote.  For a one-way IPv6 bench only the forward tuple
 * (the one that actually carries frames) is worth shotgun-ing, so this picks
 * it by source port: the reverse flow of "sport X -> dport Y" has sport Y.
 */
static int zx_ppe_v6_shot_sport = -1;
module_param_named(v6_shot_sport, zx_ppe_v6_shot_sport, int, 0644);
MODULE_PARM_DESC(v6_shot_sport,
	"only shotgun a flow whose source port is this value (-1: every flow)");

/*
 * The vendor's way of telling the classifier "this IPv6 address is one of
 * ours" without an ethertype trap (the stock firmware has no 0x86dd constant
 * anywhere - it classifies by address, not by ethertype).  Two register
 * files, both in the CLA window this driver already maps after the ONU
 * register numbers are turned into byte offsets (descriptor table at tm.ko
 * 0xac700: reg 19 = base 0x000e3028, 4 words, stride 1; reg 23 = base
 * 0x000e303f, 8 words, stride 1; byte offset = register * 4, which is the
 * same rule that puts the CLA command register at 0x38c014):
 *
 *   reg 19 -> 0x38c0a0 + 4*i, i = 0..3   one local IPv6 address (128 bit)
 *   reg 23 -> 0x38c0fc + 4*i, i = 0..7   ONU side SIP then DIP (two addresses)
 *
 * cla_set_local_ipv6_addr() in tm.ko writes exactly reg 19/idx 0..3 and
 * refuses idx > 3.  The import order (which 32-bit word is the high one) is
 * not documented by any code we have, so local6_rev flips it for the test.
 */
#define ZX_CLA_LOCAL_IPV6	0x38c0a0
#define ZX_CLA_ONU_IPV6		0x38c0fc

static unsigned int zx_ppe_local6[4];
static int zx_ppe_local6_n;
module_param_array_named(local6, zx_ppe_local6, uint, &zx_ppe_local6_n, 0644);
MODULE_PARM_DESC(local6,
	"the router's own IPv6 address as four 32-bit words (0 0 0 0 = leave alone)");

static unsigned int zx_ppe_onu6s[4];
static int zx_ppe_onu6s_n;
module_param_array_named(onu6s, zx_ppe_onu6s, uint, &zx_ppe_onu6s_n, 0644);
MODULE_PARM_DESC(onu6s, "ONU side source IPv6 address, four words");

static unsigned int zx_ppe_onu6d[4];
static int zx_ppe_onu6d_n;
module_param_array_named(onu6d, zx_ppe_onu6d, uint, &zx_ppe_onu6d_n, 0644);
MODULE_PARM_DESC(onu6d, "ONU side destination IPv6 address, four words");

static bool zx_ppe_local6_rev;
module_param_named(local6_rev, zx_ppe_local6_rev, bool, 0644);
MODULE_PARM_DESC(local6_rev, "store the four words in reverse order (word order test)");

static void zx_ppe_write_v6_addr(struct zx_eth_adapter *adapter, u32 reg,
				 const unsigned int *w, int n)
{
	int i;

	for (i = 0; i < n && i < 4; i++) {
		int k = zx_ppe_local6_rev ? 3 - i : i;

		writel(w[k], adapter->base + reg + 4 * i);
	}
}

static void zx_ppe_program_v6_addrs(struct zx_eth_adapter *adapter)
{
	u32 back[12];
	int i;

	if (zx_ppe_local6_n == 4)
		zx_ppe_write_v6_addr(adapter, ZX_CLA_LOCAL_IPV6, zx_ppe_local6, 4);
	if (zx_ppe_onu6s_n == 4)
		zx_ppe_write_v6_addr(adapter, ZX_CLA_ONU_IPV6, zx_ppe_onu6s, 4);
	if (zx_ppe_onu6d_n == 4)
		zx_ppe_write_v6_addr(adapter, ZX_CLA_ONU_IPV6 + 16, zx_ppe_onu6d, 4);
	if (!zx_ppe_local6_n && !zx_ppe_onu6s_n && !zx_ppe_onu6d_n)
		return;

	/* read back: a register that does not hold what was written is a
	 * wrong offset, which is cheaper to see here than in a packet test
	 */
	for (i = 0; i < 4; i++)
		back[i] = readl(adapter->base + ZX_CLA_LOCAL_IPV6 + 4 * i);
	for (i = 0; i < 8; i++)
		back[4 + i] = readl(adapter->base + ZX_CLA_ONU_IPV6 + 4 * i);
	dev_info(adapter->dev,
		 "IPv6 address files: local %08x %08x %08x %08x, onu %08x %08x %08x %08x / %08x %08x %08x %08x\n",
		 back[0], back[1], back[2], back[3], back[4], back[5], back[6],
		 back[7], back[8], back[9], back[10], back[11]);
}

static unsigned int zx_ppe_v6_entry_idx(void)
{
	return zx_ppe_v6_entry < 0 ? (unsigned int)zx_ppe_v6_ext_idx
				   : (unsigned int)zx_ppe_v6_entry;
}

static unsigned int zx_ppe_v6_key_byte(void)
{
	return zx_ppe_v6_key < 0 ? (unsigned int)zx_ppe_v6_rule_idx
				 : (unsigned int)zx_ppe_v6_key;
}

/* The one value the vendor puts in both places: the 45 B key's ex_rule_id
 * (bit23..30) and the 60 B entry's extr_index (+15[7:4]/+16[3:0]).  Formula
 * from tm_acl_fast_add_v4v6 0x4d7fc..0x4d808 (v6 path) and 0x4d614 (v4 path,
 * ext_idx*16).  Always even, which the vendor's key writer requires: it only
 * stores extr>>1 into key[3][6:0] and clears key[2][7].
 */
static unsigned int zx_ppe_v6_extr_index(void)
{
	unsigned int ext = zx_ppe_v6_entry_idx();

	return (ext == 10 ? 2 : ext * 16 + 2) & 0xff;
}

static unsigned int zx_ppe_v6_tag(void)
{
	return zx_ppe_v6_tag_byte & 0xff;
}

/* The rule table entry the v6 windows are written to: the vendor's landing
 * site 2 under v6_hwenc, the old 0x99 otherwise (see ZX_PPE_V6_HW_RULE and
 * zx_ppe_rule_remember, which saves whatever is there first).
 */
static unsigned int zx_ppe_v6_rule_id(void)
{
	return zx_ppe_v6_hwenc ? ZX_PPE_V6_HW_RULE
			       : (unsigned int)zx_ppe_v6_rule_idx;
}

/*
 * The classifier keeps no packet counters, only one hit bit per entry, so
 * "did the hardware really match this flow" is invisible from outside --
 * the one thing worth knowing while a key format is being worked out.  With
 * debug=1 zx_ppe_flow_stats() logs the first hit of every hash entry, which
 * separates "the key never matches" from "no traffic was generated".
 *
 * The family has to be remembered here rather than read off the entry: the
 * entry struct lives in the built-in header, and FLOW_CLS_STATS does not
 * carry a flow_rule (nf_flow_table_offload.c sets cls_flow.rule for
 * FLOW_CLS_REPLACE only).  A collision in this direct-mapped table costs a
 * log line and nothing else.
 */
#define ZX_PPE_COOKIE_MAP	256
static struct {
	unsigned long	cookie;
	bool		v6;
	bool		used;
} zx_ppe_cookie_map[ZX_PPE_COOKIE_MAP];
static DECLARE_BITMAP(zx_ppe_hit_logged, ZX_PPE_HASH_SIZE);

static unsigned int zx_ppe_cookie_slot(unsigned long cookie)
{
	return (cookie >> 4) & (ZX_PPE_COOKIE_MAP - 1);
}

static void zx_ppe_cookie_set(unsigned long cookie, bool v6)
{
	unsigned int i = zx_ppe_cookie_slot(cookie);

	zx_ppe_cookie_map[i].cookie = cookie;
	zx_ppe_cookie_map[i].v6 = v6;
	zx_ppe_cookie_map[i].used = true;
}

static void zx_ppe_cookie_clear(unsigned long cookie)
{
	unsigned int i = zx_ppe_cookie_slot(cookie);

	if (zx_ppe_cookie_map[i].used && zx_ppe_cookie_map[i].cookie == cookie)
		zx_ppe_cookie_map[i].used = false;
}

static bool zx_ppe_cookie_get(unsigned long cookie, bool *v6)
{
	unsigned int i = zx_ppe_cookie_slot(cookie);

	if (!zx_ppe_cookie_map[i].used || zx_ppe_cookie_map[i].cookie != cookie)
		return false;
	*v6 = zx_ppe_cookie_map[i].v6;
	return true;
}

static u32 zx_pp_data_reg(u32 blk, int i)
{
	if (blk == ZX_PM_BASE && i >= 4)
		return ZX_PM_IND_DATA_HI + (i - 4) * 4;
	return ZX_PP_IND_DATA + i * 4;
}

static int zx_pp_wait(struct zx_eth_adapter *adapter, u32 blk)
{
	u32 val;

	return readl_poll_timeout(adapter->base + blk + ZX_PP_IND_STATUS, val,
				  val & 1, 0, 1000);
}

/* Write @n data words of an entry; the hardware takes them when word 0 is
 * written, so it goes last (as in the stock driver).
 */
static int zx_pp_write(struct zx_eth_adapter *adapter, u32 blk, u32 table,
		       u32 index, const u32 *data, int n)
{
	int words = blk == ZX_CLA_BASE ? 17 : 8;
	int i, ret;

	ret = zx_pp_wait(adapter, blk);
	if (ret)
		return ret;
	writel(index | table << 22, adapter->base + blk + ZX_PP_IND_CMD);
	for (i = words - 1; i >= 0; i--)
		writel(i < n ? data[i] : 0, adapter->base + blk + zx_pp_data_reg(blk, i));
	/* the status can still read idle right after the command */
	for (i = 0; i < 64 && (readl(adapter->base + blk + ZX_PP_IND_STATUS) & 1); i++)
		;
	return zx_pp_wait(adapter, blk);
}

static int zx_pp_read_word0(struct zx_eth_adapter *adapter, u32 blk, u32 table,
			    u32 index, u32 *val)
{
	int i, ret;

	ret = zx_pp_wait(adapter, blk);
	if (ret)
		return ret;
	writel(index | table << 22 | ZX_PP_IND_READ, adapter->base + blk + ZX_PP_IND_CMD);
	for (i = 0; i < 64 && (readl(adapter->base + blk + ZX_PP_IND_STATUS) & 1); i++)
		;
	ret = zx_pp_wait(adapter, blk);
	if (!ret)
		*val = readl(adapter->base + blk + ZX_PP_IND_DATA);
	return ret;
}

/* Read @n data words of an entry.  The command register is issued once and the
 * hardware then presents the whole entry in the per-word data registers, which
 * is how the stock driver dumps a rule (it reads them back to rebuild a hash
 * key), so no per-word command is needed.
 */
static int zx_pp_read_entry(struct zx_eth_adapter *adapter, u32 blk, u32 table,
			    u32 index, u32 *data, int n)
{
	int words = blk == ZX_CLA_BASE ? 17 : 8;
	int i, ret;

	ret = zx_pp_wait(adapter, blk);
	if (ret)
		return ret;
	writel(index | table << 22 | ZX_PP_IND_READ, adapter->base + blk + ZX_PP_IND_CMD);
	for (i = 0; i < 64 && (readl(adapter->base + blk + ZX_PP_IND_STATUS) & 1); i++)
		;
	ret = zx_pp_wait(adapter, blk);
	if (ret)
		return ret;
	for (i = 0; i < n && i < words; i++)
		data[i] = readl(adapter->base + blk + zx_pp_data_reg(blk, i));
	return 0;
}

/* Set @width bits at bit @pos of a little-endian bit string */
static void zx_ppe_put_bits(u8 *buf, int pos, int width, u32 val)
{
	int i;

	for (i = 0; i < width; i++, pos++)
		if (val & BIT(i))
			buf[pos / 8] |= BIT(pos % 8);
}

/* MSB-first CRC over the key, last byte first, no init or final xor */
static u32 zx_ppe_crc(u32 poly, const u8 *key, int len)
{
	u32 crc = 0;
	int i, b;

	for (i = len - 1; i >= 0; i--) {
		crc ^= key[i] << 24;
		for (b = 0; b < 8; b++)
			crc = crc & BIT(31) ? crc << 1 ^ poly : crc << 1;
	}
	return crc;
}

struct zx_ppe_tuple {
	bool	v6;
	u8	proto;
	__be32	saddr, daddr;
	u8	saddr6[16], daddr6[16];
	__be16	sport, dport;
};

/*
 * The windows the extract rule reads out of the L3 payload, in order: the
 * protocol byte and then the addresses as 16-bit pieces, then the two
 * ports.  IPv4 keeps the protocol in the low byte of the first window
 * (mask 0x00ff at offset 8), IPv6 in the high byte (mask 0xff00 at offset
 * 6), which is byte 6 of its header.
 */
static int zx_ppe_halfwords(const struct zx_ppe_tuple *t, u16 *hw)
{
	u32 s, d;
	int i, n;

	if (t->v6 && zx_ppe_v6_win_n) {
		u8 hdr[48] = {};
		u16 sp = be16_to_cpu(t->sport);
		u16 dp = be16_to_cpu(t->dport);

		hdr[6] = t->proto;
		memcpy(hdr + 8, t->saddr6, 16);
		memcpy(hdr + 24, t->daddr6, 16);
		hdr[40] = sp >> 8;
		hdr[41] = sp & 0xff;
		hdr[42] = dp >> 8;
		hdr[43] = dp & 0xff;
		for (i = 0; i < zx_ppe_v6_win_n; i++) {
			int o = zx_ppe_v6_win_ofs[i];

			hw[i] = ((u16)hdr[o] << 8 | hdr[o + 1]) &
				zx_ppe_v6_win_mask[i];
		}
		return zx_ppe_v6_win_n;
	}

	if (t->v6) {
		bool plow = zx_ppe_v6_fix & 1;
		bool pswap = zx_ppe_v6_fix & 2;
		bool aswap = zx_ppe_v6_fix & 4;

		hw[0] = plow ? t->proto : t->proto << 8;
		for (i = 0; i < 8; i++) {
			u16 v = t->saddr6[2 * i] << 8 | t->saddr6[2 * i + 1];

			hw[1 + i] = aswap ? swab16(v) : v;
		}
		for (i = 0; i < 8; i++) {
			u16 v = t->daddr6[2 * i] << 8 | t->daddr6[2 * i + 1];

			hw[9 + i] = aswap ? swab16(v) : v;
		}
		hw[17] = be16_to_cpu(t->sport);
		hw[18] = be16_to_cpu(t->dport);
		if (pswap) {
			hw[17] = swab16(hw[17]);
			hw[18] = swab16(hw[18]);
		}
		n = ZX_PPE_V6_WINDOWS;
		if (zx_ppe_v6_hwn >= 0 && zx_ppe_v6_hwn < n)
			n = zx_ppe_v6_hwn;
		return n;
	}

	s = be32_to_cpu(t->saddr);
	d = be32_to_cpu(t->daddr);
	hw[0] = t->proto;
	hw[1] = s >> 16;
	hw[2] = s & 0xffff;
	hw[3] = d >> 16;
	hw[4] = d & 0xffff;
	hw[5] = be16_to_cpu(t->sport);
	hw[6] = be16_to_cpu(t->dport);
	return ZX_PPE_V4_WINDOWS;
}

/* Hash slot of a tuple in bank 0 (0-255) and bank 1 (256-383).  The key is
 * 45 bytes: header (extract rule id at bit 23, direction bit 32 = 0) and
 * the extracted halfwords from bit 33.
 */
static void zx_ppe_slots(const struct zx_ppe_tuple *t, u16 slot[2])
{
	u8 key[45] = {};
	u16 hw[ZX_PPE_V6_WINDOWS];
	unsigned int rule;
	int i, n;

	n = zx_ppe_halfwords(t, hw);
	rule = t->v6 ? zx_ppe_v6_key_byte() : ZX_PPE_V4_RULE;
	/* The key's ex_rule_id and the entry's extr_index come from this same
	 * value, exactly like the vendor's single instruction stream: for the
	 * v6 extract index 10 it is 2 (v6_hwenc, the shape the hardware was
	 * measured to accept), for v4 it is ext_idx * 16 = 0x90, and the key
	 * byte 0x98 the v4 path writes is that value with bit 3 set.
	 */
	if (t->v6 && (zx_ppe_v6_hwenc || zx_ppe_v6_selfconsist))
		rule = zx_ppe_v6_extr_index();
	zx_ppe_put_bits(key, 23, 8, rule);
	for (i = 0; i < n; i++)
		zx_ppe_put_bits(key, 33 + 16 * i, 16, hw[i]);
	slot[0] = zx_ppe_crc(0x04c11db7, key, sizeof(key)) & 0xff;
	slot[1] = ZX_PPE_HASH0_SIZE + (zx_ppe_crc(0x1edc6f41, key, sizeof(key)) & 0x7f);
}

/*
 * The vendor's cla_set_hash_table(slot, ...) splits 520 slots over five table
 * ids, not two: 0..255 -> table 2, 256..383 -> table 3, 384..447 -> table 4,
 * 448..511 -> table 5, 512..519 -> table 6.  The driver only ever allocates
 * from the first two banks, but the lookup hardware is free to use the rest,
 * so the shotgun and the dump have to be able to reach them.
 */
static void zx_ppe_hash_loc(u32 slot, u32 *table, u32 *index)
{
	if (slot < ZX_PPE_HASH0_SIZE) {
		*table = ZX_CLA_HASH0;
		*index = slot;
	} else if (slot < 256 + 128) {
		*table = ZX_CLA_HASH1;
		*index = slot - 256;
	} else if (slot < 448) {
		*table = 4;
		*index = slot - 384;
	} else if (slot < 512) {
		*table = 5;
		*index = slot - 448;
	} else {
		*table = 6;
		*index = slot - 512;
	}
}

#define ZX_PPE_HASH_SLOTS_EXT 520

/*
 * Classifier entry: rewrite index, egress port and forward flag, then the
 * key. Words 1-3 are the same constants in every entry of the stock
 * firmware, and so is the 0x44 in word 0.  The key string starts at word 4
 * (bit 128 of the entry, where the stock firmware puts the extract index);
 * the hardware reads window n at bit 152 + 16n, which is bit 24 + 16n of
 * the string.  Returns the number of words to write.
 */
static int zx_ppe_hash_entry(const struct zx_ppe_tuple *t, u16 flow, int port,
			     u32 *w)
{
	u8 key[44] = {};
	u16 hw[ZX_PPE_V6_WINDOWS];
	unsigned int idx;
	int i, n, words;

	n = zx_ppe_halfwords(t, hw);
	idx = t->v6 ? zx_ppe_v6_entry_idx() : ZX_PPE_V4_INDEX;
	/* Entry +16[3:0] is extr_index[7:4]; valid_en (+16 bit6) stays 1.  The
	 * vendor patches only those four bits (cla_set_hash_table via
	 * tm_acl_fast_add_v4v6 0x4d71c..0x4d748), keeping +16[7:4].  For the v6
	 * extract index 10 extr_index is 2, so the byte becomes exactly 0x40.
	 */
	if (t->v6 && (zx_ppe_v6_hwenc || zx_ppe_v6_selfconsist))
		idx = zx_ppe_v6_extr_index() >> 4;
	/* valid, extract index, upstream */
	zx_ppe_put_bits(key, 0, 8, 0x40 | (idx & 0xff));
	/* The vendor's own flow-add (tm.ko run in tools/vendor-emu) puts 0x14
	 * in byte 2 of a v6 key string, where the v4 string has 0.  Whether the
	 * hardware compares that byte is unproven; this makes our entry
	 * byte-identical to theirs, which is what the parameter is for.
	 * Byte 1 stays 0.
	 */
	if (t->v6)
		zx_ppe_put_bits(key, 16, 8, zx_ppe_v6_tag());
	for (i = 0; i < n; i++)
		zx_ppe_put_bits(key, 24 + 16 * i, 16, hw[i]);
	/* word 0: rewrite index in 31:25, FWD in bit 24, egress in 15:12, the
	 * 0x44 every stock entry carries (bits 2 and 6) and bit 3 as the IPv6
	 * marker (every stock v6 entry has it, no v4 one does).
	 *
	 * The egress field holds a *bridge* port, not a switch port index: the
	 * receiver decodes the ingress port of a frame as (desc[6] >> 3) - 1 and
	 * ZX_BRPORT() adds that 1 back (zx279128s-eth.h:228 "0 is the PON side,
	 * 1-5 are the switch ports 0-4"), so port p and bridge port p + 1 name
	 * the same port and 0 means the PON.  Measured on this box in its
	 * delivery state (2026-11-02 02:1x, two IPv4 connections, four live
	 * entries): w0 = 03002044 / 07002044 / 05003044 / 09003044, i.e. flow 1
	 * and 3 egress 2, flow 2 and 4 egress 3 - lan2 is switch port 1 and lan1
	 * is switch port 2 on this board (docs/CONTRADICTIONS.md C9), so egress =
	 * port + 1 and the two directions of one connection differ by exactly
	 * that.  Those same entries forwarded at 942 Mbit/s.
	 *
	 * The rewrite index points at the packet-modifier flow entry this very
	 * flow programmed (zx_ppe_flow_entry() at ZX_PM_FLOW, index `flow`): it
	 * replaces both MACs and decrements the hop limit.  What IPv6 does *not*
	 * need - and does not get - are the NAT/port rewrites and the IPv4/IPv4
	 * checksum updates (the two !r->v6 bits), because there is no NAT66 and
	 * the transport checksum does not cover the hop limit.  So this is the
	 * v4-proven construction with the v4-only parts already absent, not a v4
	 * rewrite entry reused on a v6 frame.  The vendor's golden v6 entry has
	 * flow 0 there, i.e. "rewrite nothing": MACs and hop limit are left as
	 * the host sent them, which only suits a topology where the far side
	 * accepts the router's own MAC as destination (not a routed LAN->LAN
	 * hop).  v6_flow / v6_w0 exist so both readings can be A/B'd in one boot.
	 *
	 * The vendor's egress 0 is the PON side, which this board has no netdev
	 * for (docs/vendor-v6-entry-format-2026-10-31.md §5.4): a flow such an
	 * entry catches is steered at a port that does not exist, which is the
	 * "hit => iperf3 dies" signature every hit experiment showed
	 * (cla-v6-v2 RESULT.md §2).  Hence the egress comes from the flow's own
	 * redirect device - for host -> box -> server it is the server's port.
	 */
	if (t->v6 && zx_ppe_v6_flow >= 0)
		flow = (u16)zx_ppe_v6_flow;
	w[0] = FIELD_PREP(ZX_CLA_W0_FLOW, flow) | ZX_CLA_W0_FWD |
	       FIELD_PREP(ZX_CLA_W0_EGRESS, ZX_BRPORT(port)) | ZX_CLA_W0_BITS |
	       (t->v6 ? BIT(3) : 0);
	if (t->v6 && zx_ppe_v6_w0)
		w[0] = (u32)zx_ppe_v6_w0;
	/* Words 1-3 are the constants the IPv4 fast path has been forwarding
	 * with on this box (w1 0xfa11c000, w2 0x00000608, w3 0x80000000).  The
	 * vendor's v6 dump has w1 = 0xf513e100 and w3 = 0 instead, i.e. the
	 * shape that was never measured to forward; v6_w1 and v6_w3 let that be
	 * A/B'd on a loaded module rather than in a rebuild.  w2 is identical in
	 * both dumps.  With v6_hwenc, w3's top byte becomes 0x20 (extr_index 2
	 * in +15[7:4]), which is exactly the w3 = 0x20000000 of the entry that
	 * hit on hardware.
	 */
	w[1] = (t->v6 && zx_ppe_v6_w1) ? (u32)zx_ppe_v6_w1 : 0xfa11c000;
	w[2] = 0x00000608;
	w[3] = (t->v6 && zx_ppe_v6_w3) ? (u32)zx_ppe_v6_w3 : 0x80000000;
	/* Entry +15[7:4] is extr_index[3:0]; +15[3:0] (tag_level[2]/
	 * l2_type/pppoe_flag) is left alone, like the vendor's read-modify-write.
	 */
	if (t->v6 && (zx_ppe_v6_hwenc || zx_ppe_v6_selfconsist)) {
		u32 b = (w[3] >> 24) & 0x0f;

		b |= (zx_ppe_v6_extr_index() & 0x0f) << 4;
		w[3] = (w[3] & 0x00ffffff) | (b << 24);
	}
	/* 4 control words plus the string, rounded up to whole words */
	words = 4 + (24 + 16 * n + 31) / 32;
	for (i = 4; i < words; i++)
		w[i] = get_unaligned_le32(key + 4 * (i - 4));
	return words;
}

struct zx_ppe_rewrite {
	bool	v6;
	bool	sip, dip, sport, dport;
	__be16	new_sport, new_dport;
	u8	subnet;
	u16	nh;
};

/* Packet modifier flow entry (12 bytes, little-endian bit fields) */
static void zx_ppe_flow_entry(const struct zx_ppe_rewrite *r, u32 w[3])
{
	u8 b[12] = {};

	zx_ppe_put_bits(b, 0, 1, 1);			/* replace destination MAC */
	zx_ppe_put_bits(b, 1, 1, 1);			/* replace source MAC */
	if (r->dport)
		zx_ppe_put_bits(b, 2, 16, be16_to_cpu(r->new_dport));
	if (r->sport)
		zx_ppe_put_bits(b, 18, 16, be16_to_cpu(r->new_sport));
	/* bit 34: decrement TTL / hop limit.  Nothing in these 12 bytes tells
	 * the modifier that the L3 header is IPv6, so v6_posthit bit0 lets it
	 * stay clear on an IPv6 flow: if the modifier resolves the action with
	 * the IPv4 offsets it would decrement source address byte 0 instead of
	 * the hop limit (DESIGN-V6-POSTHIT.md §4).
	 */
	if (!(r->v6 && (zx_ppe_posthit & ZX_PPE_POSTHIT_NOHOP)))
		zx_ppe_put_bits(b, 34, 1, 1);		/* decrement TTL/hop limit */
	/* IPv6 has no header checksum and its transport pseudo header does not
	 * cover the hop limit, so nothing needs fixing up at all.
	 */
	zx_ppe_put_bits(b, 35, 1, !r->v6);		/* update L4 checksum */
	zx_ppe_put_bits(b, 36, 1, !r->v6);		/* update IP checksum */
	zx_ppe_put_bits(b, 37, 1, r->dport);
	zx_ppe_put_bits(b, 38, 1, r->sport);
	zx_ppe_put_bits(b, 39, 1, r->dip);
	zx_ppe_put_bits(b, 40, 1, r->sip);
	zx_ppe_put_bits(b, 41, 4, r->subnet);
	zx_ppe_put_bits(b, 50, 9, r->nh);
	w[0] = get_unaligned_le32(b);
	w[1] = get_unaligned_le32(b + 4);
	w[2] = get_unaligned_le32(b + 8);
}

/* Subnet: source MAC and (for SNAT) source IP of rewritten frames */
static int zx_ppe_subnet_get(struct zx_eth_adapter *adapter, const u8 *mac,
			     __be32 ip)
{
	int i, free = -1;
	u32 w[2];

	for (i = 0; i < ZX_PPE_SUBNETS; i++) {
		if (adapter->ppe_subnet[i].refs &&
		    ether_addr_equal(adapter->ppe_subnet[i].mac, mac) &&
		    adapter->ppe_subnet[i].ip == ip) {
			adapter->ppe_subnet[i].refs++;
			return i;
		}
		if (!adapter->ppe_subnet[i].refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	zx_mac_words(mac, w);
	if (zx_pp_write(adapter, ZX_PM_BASE, ZX_PM_SRC_MAC, free, w, 2))
		return -ETIMEDOUT;
	writel(be32_to_cpu(ip), adapter->base + ZX_PP_SNAT_IP(free));
	ether_addr_copy(adapter->ppe_subnet[free].mac, mac);
	adapter->ppe_subnet[free].ip = ip;
	adapter->ppe_subnet[free].refs = 1;
	return free;
}

static void zx_ppe_subnet_put(struct zx_eth_adapter *adapter, int i)
{
	adapter->ppe_subnet[i].refs--;
}

static int zx_ppe_alloc(unsigned long *map, int size, int first)
{
	int i = find_next_zero_bit(map, size, first);

	if (i >= size)
		return -ENOSPC;
	set_bit(i, map);
	return i;
}

/*
 * Apply one of the MAC rewrites that nf_flow_table_offload.c builds: the
 * destination MAC as 4 bytes at offset 0 and 2 bytes at offset 4 (mask
 * 0xffff0000), the source MAC as 2 bytes at offset 4 (mask 0x0000ffff, value
 * in the upper half) and 4 bytes at offset 8. The masks and values are in
 * CPU order, so the byte positions below are those of a little-endian CPU,
 * which the zx279128s is.
 */
static void zx_ppe_mangle_eth(const struct flow_action_entry *act, void *eth)
{
	void *dest = eth + act->mangle.offset;
	const void *src = &act->mangle.val;

	if (act->mangle.offset > 8)
		return;
	if (act->mangle.mask == 0xffff) {
		src += 2;
		dest += 2;
	}
	memcpy(dest, src, act->mangle.mask ? 2 : 4);
}

static int zx_ppe_flow_add(struct zx_eth_adapter *adapter,
			   struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_action_entry *act;
	struct zx_ppe_rewrite rw = {};
	struct zx_ppe_tuple t = {};
	struct net_device *odev = NULL;
	struct zx_ppe_entry *e;
	struct ethhdr eth = {};
	__be32 new_saddr, new_daddr;
	u16 slot[2];
	u32 w[17];
	int i, ret, port, subnet, flow, nh, words;

	if (rhashtable_lookup_fast(&adapter->ppe_flows, &f->cookie, zx_ppe_ht_params))
		return -EEXIST;

	/* the hardware only sees frames from our switch ports */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct flow_match_meta match;

		flow_rule_match_meta(rule, &match);
		for (i = 0; i < ZX_NUM_PORTS; i++)
			if (adapter->ports[i] &&
			    adapter->ports[i]->ifindex == match.key->ingress_ifindex)
				break;
		if (i == ZX_NUM_PORTS)
			return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		if (match.key->addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
			if (!zx_ppe_ipv6)
				return -EOPNOTSUPP;
			t.v6 = true;
		} else if (match.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
			return -EOPNOTSUPP;
		}
	} else {
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		if (match.key->n_proto != htons(t.v6 ? ETH_P_IPV6 : ETH_P_IP))
			return -EOPNOTSUPP;
		t.proto = match.key->ip_proto;
	}
	if (t.proto != IPPROTO_TCP && t.proto != IPPROTO_UDP)
		return -EOPNOTSUPP;
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
		return -EOPNOTSUPP;
	{
		struct flow_match_ports ports;

		flow_rule_match_ports(rule, &ports);
		t.sport = ports.key->src;
		t.dport = ports.key->dst;
	}
	if (t.v6) {
		struct flow_match_ipv6_addrs addrs;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv6_addrs(rule, &addrs);
		memcpy(t.saddr6, &addrs.key->src, sizeof(t.saddr6));
		memcpy(t.daddr6, &addrs.key->dst, sizeof(t.daddr6));
	} else {
		struct flow_match_ipv4_addrs addrs;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv4_addrs(rule, &addrs);
		t.saddr = addrs.key->src;
		t.daddr = addrs.key->dst;
	}
	new_saddr = t.saddr;
	new_daddr = t.daddr;
	rw.v6 = t.v6;
	rw.new_sport = t.sport;
	rw.new_dport = t.dport;

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				zx_ppe_mangle_eth(act, &eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				/* no NAT66, and no IPv4 mangle in an IPv6 flow */
				if (t.v6)
					return -EOPNOTSUPP;
				/* the value holds the address in network order */
				if (act->mangle.offset == offsetof(struct iphdr, saddr))
					new_saddr = (__force __be32)act->mangle.val;
				else if (act->mangle.offset == offsetof(struct iphdr, daddr))
					new_daddr = (__force __be32)act->mangle.val;
				else
					return -EOPNOTSUPP;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP: {
				u32 val = be32_to_cpu((__force __be32)act->mangle.val);

				if (t.v6)
					return -EOPNOTSUPP;
				if (act->mangle.offset == 0) {
					if (act->mangle.mask == ~(__force u32)cpu_to_be32(0xffff))
						rw.new_dport = cpu_to_be16(val);
					else
						rw.new_sport = cpu_to_be16(val >> 16);
				} else if (act->mangle.offset == 2) {
					rw.new_dport = cpu_to_be16(val);
				} else {
					return -EOPNOTSUPP;
				}
				break;
			}
			default:
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_REDIRECT:
			odev = act->dev;
			break;
		default:
			/* VLAN, PPPoE and tunnel encapsulation are not supported */
			return -EOPNOTSUPP;
		}
	}

	if (!odev || !zx_eth_is_port(odev))
		return -EOPNOTSUPP;
	port = ((struct zx_eth_priv *)netdev_priv(odev))->port;
	if (!is_valid_ether_addr(eth.h_source) || !is_valid_ether_addr(eth.h_dest))
		return -EOPNOTSUPP;

	rw.sip = new_saddr != t.saddr;
	rw.dip = new_daddr != t.daddr;
	rw.sport = rw.new_sport != t.sport;
	rw.dport = rw.new_dport != t.dport;

	zx_ppe_slots(&t, slot);
	if (!test_bit(slot[0], adapter->ppe_hash_used))
		slot[1] = slot[0];
	else if (test_bit(slot[1], adapter->ppe_hash_used))
		return -EBUSY;

	e = kzalloc_obj(*e);
	if (!e)
		return -ENOMEM;

	subnet = zx_ppe_subnet_get(adapter, eth.h_source, rw.sip ? new_saddr : 0);
	if (subnet < 0) {
		ret = subnet;
		goto err_free;
	}
	flow = zx_ppe_alloc(adapter->ppe_flow_used, ZX_PPE_FLOWS, 1);
	if (flow < 0) {
		ret = flow;
		goto err_subnet;
	}
	nh = zx_ppe_alloc(adapter->ppe_nh_used, ZX_PPE_NEXT_HOPS, 1);
	if (nh < 0) {
		ret = nh;
		goto err_flow;
	}
	rw.subnet = subnet;
	rw.nh = nh;

	/* next hop: new destination IP (used for DNAT) and MAC */
	w[0] = rw.dip ? be32_to_cpu(new_daddr) : 0;
	zx_mac_words(eth.h_dest, w + 1);
	ret = zx_pp_write(adapter, ZX_PM_BASE, ZX_PM_NEXT_HOP, nh, w, 3);
	if (ret)
		goto err_nh;
	zx_ppe_flow_entry(&rw, w);
	ret = zx_pp_write(adapter, ZX_PM_BASE, ZX_PM_FLOW, flow, w, 3);
	if (ret)
		goto err_nh;
	ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_AGING, slot[1], w, 0);
	if (ret)
		goto err_nh;
	words = zx_ppe_hash_entry(&t, flow, port, w);
	/* One line per IPv6 flow, and it is the line the runbook diffs against
	 * the offline slot calculator: slot is the bank-2/3 slot the entry went
	 * to, slot0 is what the software's own hash says the slot should be,
	 * flow/egress/w0 are the action word's three interesting fields (flow
	 * and egress are read back out of w0, so a v6_flow / v6_w0 override shows
	 * up here as what the hardware really got).
	 */
	if (t.v6 && (zx_ppe_v6_hwenc || zx_ppe_v6_selfconsist))
		dev_info(adapter->dev,
			 "v6 %s: rule=%u extr=%u key_rule=0x%02x slot=%u slot0=%u flow=%u egress=%u w0=%08x\n",
			 zx_ppe_v6_hwenc ? "hwenc" : "C1",
			 zx_ppe_v6_hwenc ? (unsigned int)zx_ppe_v6_rule_id()
					 : (unsigned int)zx_ppe_v6_rule_idx,
			 zx_ppe_v6_extr_index(), zx_ppe_v6_extr_index(), slot[1],
			 slot[0], (w[0] >> 25) & 0x7f, (w[0] >> 12) & 0xf, w[0]);
	ret = zx_pp_write(adapter, ZX_CLA_BASE,
			  slot[1] < ZX_PPE_HASH0_SIZE ? ZX_CLA_HASH0 : ZX_CLA_HASH1,
			  slot[1] % ZX_PPE_HASH0_SIZE, w, words);
	if (ret)
		goto err_nh;

	if (t.v6 && zx_ppe_v6_shotgun &&
	    (zx_ppe_v6_shot_sport < 0 ||
	     be16_to_cpu(t.sport) == zx_ppe_v6_shot_sport)) {
		int top = zx_ppe_v6_shot_ext ? ZX_PPE_HASH_SLOTS_EXT : ZX_PPE_HASH_SIZE;
		int lo = zx_ppe_v6_shot_lo < 0 ? 0 : zx_ppe_v6_shot_lo;
		int hi = zx_ppe_v6_shot_hi < 0 ? top - 1 : zx_ppe_v6_shot_hi;
		int s;

		for (s = lo; s <= hi && s < top; s++) {
			u32 table, index;

			if (s == slot[1])
				continue;
			zx_ppe_hash_loc(s, &table, &index);
			zx_pp_write(adapter, ZX_CLA_BASE, table, index, w, words);
		}
		dev_info(adapter->dev, "IPv6 shotgun: hash %d..%d hold the same entry\n",
			 lo, hi);
	}

	e->cookie = f->cookie;
	e->hash = slot[1];
	e->flow = flow;
	e->nh = nh;
	e->subnet = subnet;
	ret = rhashtable_insert_fast(&adapter->ppe_flows, &e->node, zx_ppe_ht_params);
	if (ret)
		goto err_entry;
	set_bit(e->hash, adapter->ppe_hash_used);
	adapter->ppe_count++;
	zx_ppe_cookie_set(f->cookie, t.v6);
	if (zx_ppe_debug)
		dev_info(adapter->dev, "offloaded %s flow into hash %u\n",
			 t.v6 ? "an IPv6" : "an IPv4", slot[1]);
	return 0;

err_entry:
	zx_pp_write(adapter, ZX_CLA_BASE,
		    slot[1] < ZX_PPE_HASH0_SIZE ? ZX_CLA_HASH0 : ZX_CLA_HASH1,
		    slot[1] % ZX_PPE_HASH0_SIZE, w, 0);
err_nh:
	clear_bit(nh, adapter->ppe_nh_used);
err_flow:
	clear_bit(flow, adapter->ppe_flow_used);
err_subnet:
	zx_ppe_subnet_put(adapter, subnet);
err_free:
	kfree(e);
	return ret;
}

static void zx_ppe_entry_remove(struct zx_eth_adapter *adapter,
				struct zx_ppe_entry *e)
{
	zx_pp_write(adapter, ZX_CLA_BASE,
		    e->hash < ZX_PPE_HASH0_SIZE ? ZX_CLA_HASH0 : ZX_CLA_HASH1,
		    e->hash % ZX_PPE_HASH0_SIZE, NULL, 0);
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_AGING, e->hash, NULL, 0);
	clear_bit(e->hash, adapter->ppe_hash_used);
	clear_bit(e->flow, adapter->ppe_flow_used);
	clear_bit(e->nh, adapter->ppe_nh_used);
	zx_ppe_subnet_put(adapter, e->subnet);
	adapter->ppe_count--;
}

static int zx_ppe_flow_del(struct zx_eth_adapter *adapter,
			   struct flow_cls_offload *f)
{
	struct zx_ppe_entry *e;

	e = rhashtable_lookup_fast(&adapter->ppe_flows, &f->cookie, zx_ppe_ht_params);
	if (!e)
		return -ENOENT;
	rhashtable_remove_fast(&adapter->ppe_flows, &e->node, zx_ppe_ht_params);
	zx_ppe_cookie_clear(f->cookie);
	zx_ppe_entry_remove(adapter, e);
	kfree(e);
	return 0;
}

/* No packet counters: report use from the entry's hit flag */
static int zx_ppe_flow_stats(struct zx_eth_adapter *adapter,
			     struct flow_cls_offload *f)
{
	struct zx_ppe_entry *e;
	bool v6 = false;
	u32 hit;

	e = rhashtable_lookup_fast(&adapter->ppe_flows, &f->cookie, zx_ppe_ht_params);
	if (!e)
		return -ENOENT;
	if (zx_pp_read_word0(adapter, ZX_CLA_BASE, ZX_CLA_AGING, e->hash, &hit))
		return -ETIMEDOUT;
	if (hit & 1) {
		f->stats.lastused = jiffies;
		zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_AGING, e->hash, NULL, 0);
		if (zx_ppe_debug && !test_and_set_bit(e->hash, zx_ppe_hit_logged)) {
			zx_ppe_cookie_get(f->cookie, &v6);
			dev_info(adapter->dev,
				 "hash %u matched by the hardware (%s flow)\n",
				 e->hash, v6 ? "IPv6" : "IPv4");
		}
	}
	return 0;
}

/* Values from the stock firmware */
static const u32 zx_ppe_v4_index[5] = {
	0x93929190, 0x97969594, 0x9b9a9998, 0x9f9e9d9c,
	0x00150151,	/* bit 8: look up the hash banks */
};

static const u32 zx_ppe_v4_rule[16] = {
	0x22038608, 0x000058a1, 0, 0, 0xf00ff000, 0xffffffff, 0xffffffff,
	0x0fffffff, 0, 0, 0, 0, 0, 0, 0x00700000, 0x00092492,
};

/*
 * IPv6 versions.  The rule is not what the stock firmware's rule 2 reads
 * literally: it is what rule 2 *means*, re-encoded from the layout the v4
 * entry proves (window offset: 7 bits at bit 7n, window mask: 16 bits at
 * bit 140+16n, window offset type: 3 bits at bit 480+3n, hash length: bit
 * 468), because encoding the v4 tuple with that layout reproduces the whole
 * zx_ppe_v4_rule above byte for byte.
 *
 * Encoded that way, the stock rule 2 (tm.ko, tm_acl_fast_init ->
 * cla_set_extra_rule_table(2, ...), emulated 2026-10-27) is a clean
 * 19-window entry: next header at offset 6 with mask 0xff00, the eight
 * source and eight destination halfwords at 8..38, the two ports at 40 and
 * 42, window offset type 3 (IPv6) everywhere, hash length 19.  One caveat:
 * that emulation captured 16 of the entry's 17 words, so the dump is one
 * 0xffffffff mask word short and its tail decodes to nonsense.  Earlier
 * revisions of this driver took the dump literally and "tidied" the tail by
 * hand, clearing 7 bits that are not spare at all: w0 bits 29-31 are window
 * 4's offset, w3 bit 31 is the last window's offset (40 -> 42), w4 bits 24
 * and 26 are two bits of window 0's mask (0xff00 -> 0xaf00), w15 bits 30-31
 * are window 10's offset type (3 -> 0).  The words below are rule 2 with its
 * dropped mask word put back.
 *
 * What these clean tables do, measured on hardware 2026-10-28: the classifier
 * recognises the frame as IPv6 and hands it to the CPU when the hash lookup
 * misses, the flow is created, the entry is written - and the hardware never
 * matches it.  Every other candidate was excluded first, on the same forwarded
 * flow: the entry in the slot the software computes, then in all 384 slots at
 * once (v6_shotgun), every window offset type 0..7 (v6_type), key lengths 7 to
 * 19 halfwords (v6_hwn), and the index/rule byte inside the key (v6_key) as
 * 0x99, 10, 11 and 0x98.  No combination hit, while the far end counted every
 * one of the 2,800 packets, so the frames were being forwarded and not dropped.
 * Ninth round (2026-10-29) closed that question: a rule with a *single*
 * window (the next-header byte, mask 0xff00, hash length 1) written by this
 * driver, with the matching entry written into all 520 slots, still sends
 * every packet through the CPU - while an IPv4 flow in the same topology
 * costs 98 of 600 packets.  The vendor's own IPv6 add does not even write a
 * rule (tools/vendor-emu/t_v6rule.py), and its 13-byte entry contradicts the
 * shipped rule's 0xff00 mask.  So the classifier does not look up an IPv6
 * hash on this part and no key layout can change that; the vendor's IPv6
 * acceleration is their NPU.  ipv6=1 therefore stays "correct forwarding, no
 * fast path", which is why it is not the default.  Details in
 * docs/known-issues.md, PPE-IPV6-TCP.
 *
 * 2026-10-31: the reason is now structural rather than a guess.  Re-running the
 * vendor's own flow-add with a *real* AF_INET6 session (tools/vendor-emu/
 * t_v6ops.py) shows it never touches the internal banks for IPv6: its only
 * write is cla_set_external_hash_table(), which lands at the vendor VA
 * 0xF1020000 + 64*idx - physical 0x20000 + 64*idx through their iotable.  That
 * window is the boot ROM alias on this board (writes raise an imprecise
 * external abort), and the indirect command register has no "external space"
 * selector (its first argument is the read/write direction).  So a v6 unicast
 * entry has no table to live in here; see docs/ppe-ipv6-external-hash-2026-10-31.md.
 *
 * The index entry is aclRamInit's: extract field ids 0..15, word 4 the proven
 * v4 entry (bit 0 = v4 fast enable) carrying the v6 enable bit 2 instead, byte
 * 17 = 0x01 "look up the hash banks" and byte 18 = 0x15 as in the v4 entry.
 *
 * The rule goes to 0x99 rather than the stock 2 because 0x98/0x99 is the id the
 * hardware puts into the hash key of a v4/v6 fast-path frame - the v4 pair at
 * 0x98 has been offloading traffic on hardware since v1.0 - and the extract
 * index is 10, the only value that works (see zx_ppe_v6_ext_idx).
 */
static u32 zx_ppe_v6_index[5] = {
	0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
	0x00150154,	/* bit 2: v6 fast enable, bit 8: hash banks */
};

static u32 zx_ppe_v6_rule[17] = {
	0xe1828406, 0x16284880, 0x01e3868c, 0x94264889, 0xfff0000a,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0x0fffffff, 0x01300000,
	0xdb6db6db, 0x00db6db6,
};

static const u32 zx_ppe_sub[2] = { 0xfc000000, 0x00001fff };

/*
 * The whole extract-index entry, as five words.  The tenth round found that
 * the vendor's own tm_acl_fast_init() does not leave index 10 the way the
 * stock tables have it: it writes { 0, 1, 0, 0, 5 } - only two field ids and
 * word 4 = 5 - and the same shape shows up for the L2 fast index (index 1 =
 * { 0, 1, 0, 0, 5 }).  Our driver inherited the stock firmware's entry
 * (field ids 0..15, word 4 = 0x00150154), so this parameter exists to write
 * the vendor's version instead and let the hardware say which one it wants.
 */
static int zx_ppe_v6_idx_set(const char *val, const struct kernel_param *kp)
{
	char buf[128];
	const char *cur;
	size_t len;
	int n = 0;

	if (!val)
		return -EINVAL;
	len = strscpy(buf, val, sizeof(buf));
	if (len < 0)
		return -EINVAL;
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
		       buf[len - 1] == ' '))
		buf[--len] = '\0';
	for (cur = buf; *cur && n < 5; cur++) {
		char *end;

		zx_ppe_v6_index[n] = simple_strtoul(cur, &end, 0);
		if (end == cur)
			return -EINVAL;
		n++;
		cur = end;
		if (*cur == ',')
			continue;
		if (*cur)
			return -EINVAL;
		break;
	}
	if (n != 5)
		return -EINVAL;
	zx_ppe_v6_wins_apply();
	pr_info("zx279128s-ppe: v6_idx: %08x %08x %08x %08x %08x\n",
		zx_ppe_v6_index[0], zx_ppe_v6_index[1], zx_ppe_v6_index[2],
		zx_ppe_v6_index[3], zx_ppe_v6_index[4]);
	return 0;
}

static const struct kernel_param_ops zx_ppe_v6_idx_ops = {
	.set	= zx_ppe_v6_idx_set,
};

static int zx_ppe_v6_idx_storage;
module_param_cb(v6_idx, &zx_ppe_v6_idx_ops, &zx_ppe_v6_idx_storage, 0644);
MODULE_PARM_DESC(v6_idx,
		 "IPv6 extract index entry as five words, e.g. \"0,1,0,0,5\"");

/*
 * Apply the v6_* overrides to the two tables above.  Bits are patched with
 * the same little-endian bit numbering the tables were encoded with, so a
 * patched table is what the encoder would have produced for that value.
 */
static void zx_ppe_v6_tune(void)
{
	u8 *rule = (u8 *)zx_ppe_v6_rule;
	int hashlen = zx_ppe_v6_hashlen;
	int i;

	if (hashlen < 0 && zx_ppe_v6_hwn >= 0)
		hashlen = zx_ppe_v6_hwn;
	if (hashlen >= 0) {
		zx_ppe_put_bits(rule, 468, 4, hashlen & 0xf);
		zx_ppe_put_bits(rule, 472, 1, (hashlen >> 4) & 1);
	}
	if (zx_ppe_v6_type >= 0)
		for (i = 0; i < ZX_PPE_V6_WINDOWS; i++)
			zx_ppe_put_bits(rule, 480 + 3 * i, 3, zx_ppe_v6_type);
	/*
	 * A window list of our own overrides offsets, masks and hash length:
	 * the rule is then exactly the description the key is built from.
	 */
	if (zx_ppe_v6_win_n) {
		for (i = 0; i < zx_ppe_v6_win_n; i++) {
			zx_ppe_patch_bits(rule, 7 * i, 7, zx_ppe_v6_win_ofs[i]);
			zx_ppe_patch_bits(rule, 140 + 16 * i, 16,
					  zx_ppe_v6_win_mask[i]);
			zx_ppe_patch_bits(rule, 480 + 3 * i, 3,
					  zx_ppe_v6_type >= 0 ? zx_ppe_v6_type : 3);
		}
		zx_ppe_patch_bits(rule, 468, 4, zx_ppe_v6_win_n & 0xf);
		zx_ppe_patch_bits(rule, 472, 1, (zx_ppe_v6_win_n >> 4) & 1);
	}
	if (zx_ppe_v6_idxw4 >= 0)
		zx_ppe_v6_index[4] = zx_ppe_v6_idxw4;
}

/* ---- One-button undo, part 2: the extract-rule table ------------------ */

/*
 * The classifier tables are 17-word entries in switch SRAM that survives a
 * module reload, so anything this engine writes into a rule it did not own
 * has to be read back first and put back afterwards.  One snapshot per rule
 * id, not one snapshot total: a run may touch two ids, and saving only the
 * first would write that one back over the second (the sweep module learned
 * this the hard way - its H1/H2 checks compare each id separately).
 */
#define ZX_PPE_SAVED_RULES	4

static struct {
	unsigned int	id;
	bool		valid;
	u32		w[17];
} zx_ppe_rule_save[ZX_PPE_SAVED_RULES];

/* Read rule table entry @id back and remember it, once per id. */
static int zx_ppe_rule_remember(struct zx_eth_adapter *adapter, unsigned int id)
{
	int i, ret;

	for (i = 0; i < ZX_PPE_SAVED_RULES; i++)
		if (zx_ppe_rule_save[i].valid && zx_ppe_rule_save[i].id == id)
			return 0;
	for (i = 0; i < ZX_PPE_SAVED_RULES; i++)
		if (!zx_ppe_rule_save[i].valid)
			break;
	if (i == ZX_PPE_SAVED_RULES)
		return -ENOSPC;

	ret = zx_pp_read_entry(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE, id,
			       zx_ppe_rule_save[i].w, 17);
	if (ret)
		return ret;
	zx_ppe_rule_save[i].id = id;
	zx_ppe_rule_save[i].valid = true;
	dev_info(adapter->dev,
		 "cla rule %u saved before the first write: %08x %08x %08x ... %08x\n",
		 id, zx_ppe_rule_save[i].w[0], zx_ppe_rule_save[i].w[1],
		 zx_ppe_rule_save[i].w[2], zx_ppe_rule_save[i].w[16]);
	return 0;
}

/* Put every entry we overwrote back, word for word, and forget it. */
static void zx_ppe_rule_restore(struct zx_eth_adapter *adapter)
{
	int i;

	for (i = 0; i < ZX_PPE_SAVED_RULES; i++) {
		if (!zx_ppe_rule_save[i].valid)
			continue;

		zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
			    zx_ppe_rule_save[i].id, zx_ppe_rule_save[i].w, 17);
		dev_info(adapter->dev,
			 "cla rule %u restored: %08x %08x %08x ... %08x\n",
			 zx_ppe_rule_save[i].id, zx_ppe_rule_save[i].w[0],
			 zx_ppe_rule_save[i].w[1], zx_ppe_rule_save[i].w[2],
			 zx_ppe_rule_save[i].w[16]);
		zx_ppe_rule_save[i].valid = false;
	}
}

/* ---- One-button undo, part 3: the extract-index table -----------------
 *
 * Same shape as the rule table just above, one table lower: the index entry is
 * 5 words in switch SRAM and survives a module reload, so the entry that
 * v6_posthit's ext_mode bit patches is read back before the first write and
 * put back word for word afterwards.  A read that fails means "we do not know
 * what is in there", and writing over an unknown entry would leave something
 * behind that no snapshot can restore - so the write is refused and the caller
 * logs it.  Only index 10 (zx_ppe_v6_ext_idx) is ever touched, so one snapshot
 * keyed by id is enough; a second id would mean two experiments at once.
 */
static struct {
	unsigned int	id;
	bool		valid;
	u32		w[5];
} zx_ppe_idx_save;

static int zx_ppe_index_remember(struct zx_eth_adapter *adapter, unsigned int id)
{
	int ret;

	if (zx_ppe_idx_save.valid)
		return zx_ppe_idx_save.id == id ? 0 : -ENOSPC;
	ret = zx_pp_read_entry(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, id,
			       zx_ppe_idx_save.w, 5);
	if (ret)
		return ret;
	zx_ppe_idx_save.id = id;
	zx_ppe_idx_save.valid = true;
	dev_info(adapter->dev,
		 "cla index %u saved before the first write: %08x %08x %08x %08x %08x\n",
		 id, zx_ppe_idx_save.w[0], zx_ppe_idx_save.w[1],
		 zx_ppe_idx_save.w[2], zx_ppe_idx_save.w[3],
		 zx_ppe_idx_save.w[4]);
	return 0;
}

static int zx_ppe_index_restore(struct zx_eth_adapter *adapter)
{
	int ret;

	if (!zx_ppe_idx_save.valid)
		return 0;
	ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX,
			  zx_ppe_idx_save.id, zx_ppe_idx_save.w, 5);
	if (ret)
		return ret;
	dev_info(adapter->dev,
		 "cla index %u restored: %08x %08x %08x %08x %08x\n",
		 zx_ppe_idx_save.id, zx_ppe_idx_save.w[0], zx_ppe_idx_save.w[1],
		 zx_ppe_idx_save.w[2], zx_ppe_idx_save.w[3],
		 zx_ppe_idx_save.w[4]);
	zx_ppe_idx_save.valid = false;
	return 0;
}

/* Apply or undo the ext_mode half of v6_posthit.  Word 4 of the index entry is
 * 0x00150154 in the delivered tables: byte 16..17 is index_valid, byte 18 bit0
 * is valid_en, bits 2..3 are ext_mode (= 1), bit4 is nomatch_cfg, byte 19 is 0.
 * ext_mode = 2 therefore means setting bit 19 of the word (0x00080000) and
 * clearing bit 18 (0x00040000) - nothing else changes, so the 16 field ids,
 * index_valid and valid_en stay exactly as they were.
 */
static int zx_ppe_index_posthit(struct zx_eth_adapter *adapter)
{
	u32 w[5];
	int ret;

	if (!(zx_ppe_posthit & ZX_PPE_POSTHIT_EXTMODE))
		return zx_ppe_index_restore(adapter);

	ret = zx_ppe_index_remember(adapter, zx_ppe_v6_ext_idx);
	if (ret) {
		dev_warn(adapter->dev,
			 "v6_posthit: index %d could not be read back, not writing it\n",
			 zx_ppe_v6_ext_idx);
		return ret;
	}
	memcpy(w, zx_ppe_v6_index, sizeof(w));
	w[4] = (w[4] & ~(u32)0x000c0000) | (u32)0x00080000;
	ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX,
			  zx_ppe_v6_ext_idx, w, 5);
	if (!ret)
		dev_info(adapter->dev,
			 "v6_posthit: index %d word4 %08x -> %08x (ext_mode 2)\n",
			 zx_ppe_v6_ext_idx, zx_ppe_v6_index[4], w[4]);
	return ret;
}

/* Zero all 520 hash slots and their aging words.  Caller holds ppe_lock; the
 * hash tables and the aging words are separate address spaces (the vendor's
 * cla_set_hash_table never clears aging), so both are written.
 */
static void zx_ppe_hash_zero(struct zx_eth_adapter *adapter)
{
	int s;

	for (s = 0; s < ZX_PPE_HASH_SLOTS_EXT; s++) {
		u32 table, index;

		zx_ppe_hash_loc(s, &table, &index);
		zx_pp_write(adapter, ZX_CLA_BASE, table, index, NULL, 0);
		zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_AGING, index, NULL, 0);
	}
	bitmap_zero(adapter->ppe_hash_used, ZX_PPE_HASH_SIZE);
}

/* Put the IPv6 extract rule and index into the hardware.  Split out of
 * zx_ppe_start() because the runtime ipv6 switch below has to do the same.
 *
 * The rule goes to the entry the key's ex_rule_id names, which under v6_hwenc
 * is the vendor's 2 - read back and saved first, and restored by
 * zx_ppe_rule_restore() on stop or on leaving v6_hwenc.  Disarming restores
 * immediately rather than only at stop, so "write 0 into v6_hwenc" is already
 * a complete undo of the rule table half.
 */
static int zx_ppe_v6_tables(struct zx_eth_adapter *adapter)
{
	int ret;

	zx_ppe_v6_tune();
	/* Put back an index entry the post-hit knob patched last time before the
	 * normal write below: the write is the authority, the snapshot is only
	 * there for the case where the knob is turned off later.
	 */
	if (!(zx_ppe_posthit & ZX_PPE_POSTHIT_EXTMODE))
		zx_ppe_index_restore(adapter);
	if (zx_ppe_v6_hwenc) {
		ret = zx_ppe_rule_remember(adapter, ZX_PPE_V6_HW_RULE);
		if (ret) {
			dev_warn(adapter->dev,
				 "IPv6: rule %u could not be read back, not writing it\n",
				 ZX_PPE_V6_HW_RULE);
			return ret;
		}
	} else {
		zx_ppe_rule_restore(adapter);
	}
	ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
			  zx_ppe_v6_rule_id(), zx_ppe_v6_rule, 17);
	if (!ret)
		ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX,
				  zx_ppe_v6_ext_idx, zx_ppe_v6_index, 5);
	if (!ret && (zx_ppe_posthit & ZX_PPE_POSTHIT_EXTMODE))
		ret = zx_ppe_index_posthit(adapter);
	if (!ret && zx_ppe_v6_hwenc)
		dev_info(adapter->dev,
			 "IPv6 hw encoding armed: rule %u + index %d, key rule byte 0x%02x, extr %u\n",
			 zx_ppe_v6_rule_id(), zx_ppe_v6_ext_idx,
			 zx_ppe_v6_extr_index(), zx_ppe_v6_extr_index());
	return ret;
}

/* v6_posthit is a table entry, so it takes effect immediately (unlike its bit0,
 * which only shapes flows created from now on).  The knob is only meaningful
 * with the IPv6 tables programmed, so an adapter that never ran
 * zx_ppe_v6_tables() is left alone.
 */
static void zx_ppe_posthit_adapter(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	if (adapter->ppe_active && zx_ppe_ipv6) {
		if (zx_ppe_index_posthit(adapter))
			dev_warn(adapter->dev,
				 "v6_posthit: the classifier index entry was not updated\n");
	}
	mutex_unlock(&adapter->ppe_lock);
}

static int zx_ppe_posthit_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (ret)
		return ret;
	if (zx_ppe_posthit & ~(u32)(ZX_PPE_POSTHIT_NOHOP | ZX_PPE_POSTHIT_EXTMODE))
		pr_warn("zx279128s-ppe: v6_posthit: unknown bits set, only bit0/bit1 exist\n");
	zx_eth_for_each_adapter(zx_ppe_posthit_adapter, NULL);
	return 0;
}

static int zx_ppe_v6_hwenc_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;
	if (zx_ppe_v6_hwenc &&
	    (zx_ppe_v6_rule_idx != ZX_PPE_V6_RULE || zx_ppe_v6_key >= 0 ||
	     zx_ppe_v6_entry >= 0 || zx_ppe_v6_selfconsist))
		pr_warn("zx279128s-ppe: v6_hwenc=1: v6_rule / v6_key / v6_entry / v6_selfconsist are ignored (deprecated)\n");
	zx_ppe_v6_wins_apply();
	return 0;
}

static void zx_ppe_v6_toggle_adapter(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	if (adapter->ppe_active) {
		if (zx_ppe_ipv6 && zx_ppe_v6_tables(adapter))
			dev_warn(adapter->dev,
				 "IPv6 classifier tables could not be written, keeping the trap\n");
		zx_ppe_set_v6_offload(zx_ppe_ipv6);
		zx_update_traps(adapter);
	}
	mutex_unlock(&adapter->ppe_lock);
}

static void zx_ppe_v6_wins_apply(void)
{
	zx_eth_for_each_adapter(zx_ppe_v6_toggle_adapter, NULL);
}

/*
 * Read-back dump of the classifier ("echo <mask> > .../parameters/dump").
 *
 * Writes only ever proved that a value *left* the CPU; this reads the tables
 * back through the same indirect interface, so the IPv6 work can be checked
 * against what the hardware really holds instead of against what was sent.
 * Mask bits: 1 = extract index + rule tables, 2 = both hash banks (plus the
 * aging word of every occupied slot), 4 = the CLA register window.
 */
static int zx_ppe_dump;

static int zx_ppe_fmt_words(char *buf, size_t size, const u32 *w, int n)
{
	int i, len = 0;

	for (i = 0; i < n; i++)
		len += scnprintf(buf + len, size - len, " %08x", w[i]);
	return len;
}

static void zx_ppe_dump_tables(struct zx_eth_adapter *adapter)
{
	static const int rules[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 17, 18, 19,
		144, 145, 152, 153, 160, 161,
	};
	char buf[192];
	u32 w[17];
	int i;

	for (i = 0; i < 16; i++) {
		if (zx_pp_read_entry(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, i, w, 5)) {
			dev_warn(adapter->dev, "dump: index read failed at %d\n", i);
			return;
		}
		zx_ppe_fmt_words(buf, sizeof(buf), w, 5);
		dev_info(adapter->dev, "cla-index[%2d]%s\n", i, buf);
	}

	for (i = 0; i < ARRAY_SIZE(rules); i++) {
		if (zx_pp_read_entry(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
				     rules[i], w, 17)) {
			dev_warn(adapter->dev, "dump: rule read failed at %d\n", rules[i]);
			return;
		}
		zx_ppe_fmt_words(buf, sizeof(buf), w, 17);
		dev_info(adapter->dev, "cla-rule[%3d]%s\n", rules[i], buf);
	}
}

static void zx_ppe_dump_hash(struct zx_eth_adapter *adapter)
{
	char buf[192];
	u32 w[17], ag;
	int s, shown = 0, empty = 0;

	for (s = 0; s < ZX_PPE_HASH_SLOTS_EXT; s++) {
		u32 table, index;

		zx_ppe_hash_loc(s, &table, &index);

		if (zx_pp_read_entry(adapter, ZX_CLA_BASE, table, index, w, 15)) {
			dev_warn(adapter->dev, "dump: hash read failed at slot %d\n", s);
			return;
		}
		if (!w[0] && !w[1] && !w[2] && !w[3] && !w[4] && !w[5]) {
			empty++;
			continue;
		}
		if (zx_pp_read_word0(adapter, ZX_CLA_BASE, ZX_CLA_AGING, index, &ag))
			ag = 0;
		zx_ppe_fmt_words(buf, sizeof(buf), w, 15);
		dev_info(adapter->dev, "cla-hash[%3d] (tbl %u idx %3u)%s ag %08x\n",
			 s, table, index, buf, ag);
		shown++;
	}
	dev_info(adapter->dev, "cla-hash: %d of %d slots occupied, %d empty\n",
		 shown, ZX_PPE_HASH_SLOTS_EXT, empty);
}

static void zx_ppe_dump_regs(struct zx_eth_adapter *adapter)
{
	int off, i;

	for (off = 0; off <= 0x11c; off += 16) {
		char buf[64];
		u32 v[4];

		for (i = 0; i < 4; i++)
			v[i] = readl(adapter->base + ZX_CLA_BASE + off + 4 * i);
		zx_ppe_fmt_words(buf, sizeof(buf), v, 4);
		dev_info(adapter->dev, "cla-reg %03x:%s%s\n", off, buf,
			 off == 0x90 ? "  <- hash poly (onu reg 12)" : "");
	}
}

/* Which table ids the indirect command register actually answers.  The vendor's
 * IPv6 add path also writes an "external" hash table at (0x3c40000 + 0x800 +
 * idx) << 6, i.e. vendor VA 0xF1020000 + 64*idx = physical 0x20000 + 64*idx
 * through their iotable; the hardware could have read v6 entries from there
 * instead of from the internal banks.  Measured 2026-10-31: this scan answers
 * for table ids 2..6 only (the five internal hash banks), and the vendor's
 * indirect command word has no "external space" bit (its first argument is the
 * read/write direction), so there is no fifth table to reach this way either.
 * The external window itself is the boot ROM alias on this board.
 */
static void zx_ppe_dump_tablescan(struct zx_eth_adapter *adapter)
{
	char buf[192];
	u32 w[8];
	int t, i;

	for (t = 0; t < 16; t++) {
		u32 seen = 0;

		for (i = 0; i < 8; i++) {
			if (zx_pp_read_entry(adapter, ZX_CLA_BASE, t, i, w, 5))
				break;
			if (!w[0] && !w[1] && !w[2] && !w[3] && !w[4])
				continue;
			zx_ppe_fmt_words(buf, sizeof(buf), w, 5);
			dev_info(adapter->dev, "cla-tbl%02d[%2d]%s\n", t, i, buf);
			seen++;
		}
		if (!seen)
			dev_info(adapter->dev, "cla-tbl%02d: index 0..7 empty or unreadable\n", t);
	}
}

/*
 * "echo 80=00000e00,94=8 > .../parameters/cla_write" writes the CLA register
 * window directly ("offset in hex = value in hex", comma separated), then reads
 * every word back.  This exists because the emulated vendor CLA init
 * (tm_pon_pp_cla_initial) programmes registers that this driver never touches:
 * ONU register 3 (CLA+0x80, the config word, 0x00000e00 in the vendor image
 * against 0x0000301a from our bootloader), register 6/14/16 (CLA+0x88/0x98,
 * 0x3fff and 0x3fff3fff), register 13 (CLA+0x94, the outer hash space: the
 * vendor asks for outer_hash_num=2, the driver writes 1) and register 12
 * (CLA+0x90, the hash polynomial config, already 0x00e400e4 at boot).  Which of
 * those the IPv6 lookup actually needs is a question for the hardware, not for
 * a disassembly, and a reboot restores the bootloader values.
 */
#define ZX_PPE_CLA_WR_MAX 8

struct zx_ppe_cla_wr {
	u32 off[ZX_PPE_CLA_WR_MAX];
	u32 val[ZX_PPE_CLA_WR_MAX];
	int n;
};

static void zx_ppe_cla_write_adapter(struct zx_eth_adapter *adapter, void *data)
{
	struct zx_ppe_cla_wr *wr = data;
	int i;

	mutex_lock(&adapter->ppe_lock);
	for (i = 0; i < wr->n; i++) {
		writel(wr->val[i], adapter->base + ZX_CLA_BASE + wr->off[i]);
		dev_info(adapter->dev, "cla_write %03x <- %08x (reads back %08x)\n",
			 wr->off[i], wr->val[i],
			 readl(adapter->base + ZX_CLA_BASE + wr->off[i]));
	}
	mutex_unlock(&adapter->ppe_lock);
}

static int zx_ppe_cla_write_storage;

static int zx_ppe_cla_write_set(const char *val, const struct kernel_param *kp)
{
	struct zx_ppe_cla_wr wr = {};
	char buf[192], *cur, *sep = NULL;
	int ret;

	if (!val)
		return -EINVAL;
	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -EINVAL;
	for (cur = buf; cur && *cur && wr.n < ZX_PPE_CLA_WR_MAX; cur = sep) {
		unsigned int off, v;

		sep = strchr(cur, ',');
		if (sep)
			*sep++ = '\0';
		ret = sscanf(cur, "%x=%x", &off, &v);
		if (ret != 2 || off > 0x1ff)
			return -EINVAL;
		wr.off[wr.n] = off;
		wr.val[wr.n] = v;
		wr.n++;
	}
	if (!wr.n)
		return -EINVAL;
	zx_eth_for_each_adapter(zx_ppe_cla_write_adapter, &wr);
	return 0;
}

static const struct kernel_param_ops zx_ppe_cla_write_ops = {
	.set = zx_ppe_cla_write_set,
	.get = param_get_int,
};
module_param_cb(cla_write, &zx_ppe_cla_write_ops, &zx_ppe_cla_write_storage, 0644);
MODULE_PARM_DESC(cla_write,
		 "write CLA registers: \"offset=value,offset=value\" (hex), reads each back");

/*
 * "echo 0x20000 > .../parameters/xprobe" reads a physical window that the
 * vendor's cla_set_external_hash_table() writes its hash entries to.
 *
 * That function forms the address arithmetically out of thin air:
 *
 *	addr = (0x3c40800 + idx) << 6 = 0xF1020000 + 64 * idx
 *
 * 0xF1020000 is a *virtual* address, not a physical one, and the vendor
 * kernel's private map_desc table says exactly what it is.  The table sits at
 * ~0xc060e350 in vendor-vmlinux.bin as 16 {virtual, pfn, length, type}
 * entries, and its last entry is
 *
 *	[15] virt 0xF1000000  phys 0x00000000  len 0x00520000  type 0 (MT_DEVICE)
 *
 * so the window is 0xF1000000 + 0x20000, i.e. physical 0x00020000, one entry
 * per 64 bytes - a 5 MiB device region at the bottom of the physical map
 * (the other entries in that table map the switch at 0xF4000000 <- 0x92000000
 * and 0xF0E00000 <- 0x92000000, plus the register blocks at 0x94/0x93/0x91...).
 * Nothing in this kernel claims or maps physical 0x20000, hence the probe: a
 * read of an unbacked address raises an external abort, so set
 * /proc/sys/kernel/panic_on_oops=0 first - then the oops kills the faulting
 * task and "Unhandled fault: external abort" lands in dmesg instead of a
 * reboot.  A bus hang (no abort, no data) is the only unrecoverable outcome.
 *
 * syntax:	"0x20000"  - read 16 words at that physical address
 *		"w0x20000" - then write four words and read them back
 * Every run reads the CLA window at 0x9238c000 first as a control, so a
 * failure cannot be blamed on a broken ioremap path.
 */
#define ZX_PPE_XPROBE_STRIDE	64
#define ZX_PPE_XPROBE_WORDS	16

static int zx_ppe_xprobe_storage;

static void zx_ppe_xprobe_one(unsigned long addr, bool do_write)
{
	void __iomem *p;
	u8 __iomem *q;
	u32 w[ZX_PPE_XPROBE_WORDS];
	int i;

	pr_info("zx279128s-ppe: xprobe: mapping phys 0x%08lx, reading next\n",
		addr);
	p = ioremap(addr, ZX_PPE_XPROBE_STRIDE);
	if (!p) {
		pr_err("zx279128s-ppe: xprobe: ioremap 0x%08lx failed\n", addr);
		return;
	}
	q = p;
	pr_info("zx279128s-ppe: xprobe: mapped, reading %d words\n",
		ZX_PPE_XPROBE_WORDS);
	for (i = 0; i < ZX_PPE_XPROBE_WORDS; i++)
		w[i] = readl(q + i * 4);
	pr_info("zx279128s-ppe: xprobe: %08x %08x %08x %08x | %08x %08x %08x %08x\n",
		w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	pr_info("zx279128s-ppe: xprobe: %08x %08x %08x %08x | %08x %08x %08x %08x\n",
		w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);

	if (do_write) {
		for (i = 0; i < 4; i++)
			writel(0xe2631000 | i, q + i * 4);
		for (i = 0; i < 4; i++)
			w[i] = readl(q + i * 4);
		pr_info("zx279128s-ppe: xprobe: wrote e263100[0-3], reads back %08x %08x %08x %08x\n",
			w[0], w[1], w[2], w[3]);
	}
	iounmap(p);
}

static int zx_ppe_xprobe_set(const char *val, const struct kernel_param *kp)
{
	void __iomem *ctl;
	unsigned long addr;
	bool do_write = false;
	int ret;

	if (!val)
		return -EINVAL;
	if (*val == 'w' || *val == 'W') {
		do_write = true;
		val++;
	}
	ret = kstrtoul(val, 0, &addr);
	if (ret || addr < 0x1000 || addr > 0xffffffffUL - ZX_PPE_XPROBE_STRIDE)
		return -EINVAL;

	/* control: the classifier window, certainly backed */
	ctl = ioremap(0x9238c000, 4);
	if (ctl) {
		pr_info("zx279128s-ppe: xprobe: control 0x9238c000 reads %08x\n",
			readl(ctl));
		iounmap(ctl);
	} else {
		pr_warn("zx279128s-ppe: xprobe: control ioremap failed\n");
	}

	zx_ppe_xprobe_one(addr, do_write);
	return 0;
}

static const struct kernel_param_ops zx_ppe_xprobe_ops = {
	.set = zx_ppe_xprobe_set,
	.get = param_get_int,
};
module_param_cb(xprobe, &zx_ppe_xprobe_ops, &zx_ppe_xprobe_storage, 0644);
MODULE_PARM_DESC(xprobe,
		 "probe a physical window: \"0x20000\" reads 16 words, \"w0x20000\" also writes and reads back");

/*
 * The vendor's IPv6 *multicast* table (MLD snooping / IPTV) - the one tm.ko
 * reaches through tm_ipv6table_add_set() -> sbrg_add_ipv6table() - lives in the
 * bank at 0x388000 and uses the same indirect interface as the classifier:
 * command at +0x14, status bit 0 at +0x18, and THREE data words at
 * +0x1c/+0x20/+0x24 (the vendor's ONU registers 76/77/78).  A 48-byte entry is
 * four consecutive indices, three words each.
 *
 * It is *not* the unicast forwarding table: switch.ko is the only caller of
 * tm_ipv6table_add_set(), from sw_add_mld_addr_e8v4() / sw_del_mld_addr_e8v4()
 * and drv_del_mld_addr_gpon_sfu(), and that function fills the 52-byte record
 * as valid=1, age=0, mode=1, vlan, dip = the multicast group, pt_mask = 1 <<
 * switch port (the egress port bitmask) and sip = 0 unless the group is
 * source-specific.  So the key is (sip, dip, vlan) = an MLDv2 (S,G,V) entry and
 * "pt_mask" is a port mask.  Unicast IPv6 acceleration is a different engine
 * (switch.ko's NPU/hardfast path); this table cannot do it.  See
 * tools/vendor-emu README "第八轮" for the evidence.
 *
 * Everything about the access path is measured, not assumed: the register
 * numbers come from tm.ko's sbragRegTable (tools/vendor-emu t_v6tbl.py anchors),
 * the access sequence from running the vendor code (t_v6seq.py), and the reset
 * values from the board itself (xprobe: reg 20 bit0 = 1 = access granted,
 * reg 25 = 0 = idle, reg 29 mask 3 = 1 = the table selector).  One add is: read
 * the four tables at the entry's four blocks, then write table 0 at the same
 * blocks.
 *
 * This parameter is the primitive for the hardware test of that protocol, and
 * carries the whole add path the vendor module uses (tools/vendor-emu README,
 * "第八轮"):
 *
 *	echo "r <table> <index>" > .../parameters/sbrg          # start-bit read
 *	echo "w <table> <index> <w0> <w1> <w2>" > .../parameters/sbrg
 *	echo "h <sip32hex> <dip32hex> <vlan> [pt]" > .../parameters/sbrg
 *	echo "a <sip32hex> <dip32hex> <vlan> [pt]" > .../parameters/sbrg
 *	echo "d <sip32hex> <dip32hex> <vlan>" > .../parameters/sbrg
 *
 * "h" only prints the key hash, the selected table slot and the 48-byte entry
 * it would write; "a"/"d" program/zero that entry.  The key is sip, dip, vlan;
 * tm.ko hashes it backwards with CRC-16/CCITT (init 0) and keeps ten bits,
 * then picks the block from vendor reg 29.
 *
 * The write order is the vendor's: word 0 last, because that is the word the
 * hardware latches the entry on.  Only three data registers are touched - the
 * PP/CLA helper writes eight or seventeen - so a bad guess cannot scribble over
 * the five registers behind them.
 */
#define ZX_SBRG_BASE	0x388000
#define ZX_SBRG_WORDS	3
#define ZX_SBRG_START	BIT(27)		/* start_stop: 1 = read, 0 = write */
#define ZX_SBRG_SEL	0x184		/* vendor reg 29, mask 3 (in the bank) */
#define ZX_SBRG_KEY_LEN	34		/* sip[16] + dip[16] + vlan[2] */
#define ZX_SBRG_ENTRY_WORDS 12		/* 48 bytes = four blocks of three */
#define ZX_SBRG_HASH_MASK 0x3ff

enum zx_ppe_sbrg_mode {
	ZX_SBRG_MODE_READ,	/* one block, through the start bit */
	ZX_SBRG_MODE_WRITE,	/* one block, verbatim */
	ZX_SBRG_MODE_ADD,	/* key -> hash -> slot -> 48-byte entry */
	ZX_SBRG_MODE_DEL,	/* same slot, entry zeroed */
	ZX_SBRG_MODE_PLAN,	/* ADD without touching the table */
};

struct zx_ppe_sbrg_op {
	enum zx_ppe_sbrg_mode mode;
	u32 table;
	u32 index;
	u32 w[ZX_SBRG_WORDS];
	u8 sip[16];
	u8 dip[16];
	u32 vlan;
	u32 pt_mask;
};

/* One indirect access.  A read is "start_stop = 1, rw = 0" and returns the
 * three data words; a write is "start_stop = 0", word 0 last.  The vendor
 * polls reg 20 before the data words and for completion afterwards, which is
 * what zx_pp_wait() and the status poll below do.
 */
static int zx_ppe_sbrg_xfer(struct zx_eth_adapter *adapter, u32 table, u32 index,
			    bool read, u32 *w)
{
	void __iomem *blk = adapter->base + ZX_SBRG_BASE;
	int i, ret;

	ret = zx_pp_wait(adapter, ZX_SBRG_BASE);
	if (ret)
		return ret;
	writel(index | table << 22 | (read ? ZX_SBRG_START : 0),
	       blk + ZX_PP_IND_CMD);
	if (!read) {
		for (i = ZX_SBRG_WORDS - 1; i >= 0; i--)
			writel(w[i], blk + ZX_PP_IND_DATA + 4 * i);
	}
	for (i = 0; i < 64 && (readl(blk + ZX_PP_IND_STATUS) & 1); i++)
		;
	ret = zx_pp_wait(adapter, ZX_SBRG_BASE);
	if (ret)
		return ret;
	if (read) {
		for (i = 0; i < ZX_SBRG_WORDS; i++)
			w[i] = readl(blk + ZX_PP_IND_DATA + 4 * i);
	}
	return 0;
}

/* tm.ko's sbrg_hash(): copy the key backwards, CRC-16/CCITT with init 0, keep
 * ten bits.  The key is sip[16] + dip[16] + vlan as u16 little-endian.
 */
static u16 zx_ppe_sbrg_hash(const u8 *key, int len)
{
	u16 crc = 0;
	int i;

	while (len--) {
		crc ^= (u16)key[len] << 8;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc & ZX_SBRG_HASH_MASK;
}

/* reg 29 (table_sel) -> slot base, transcribed instruction for instruction
 * from sbrg_add_ipv6table+0x184 (tm.ko 0xf184..0xf1a0):
 *
 *     andeq r2, r0, #127     ; sel == 1: hash & 0x7f
 *     lsleq r2, r2, #2       ;            << 2     (128 buckets x 4 blocks)
 *     orreq r2, r2, #512     ;            | 0x200
 *     lslne r2, r0, #2       ; otherwise: hash << 2
 *     uxtbne r2, r2          ;            keep 8 bits
 *     orrne r2, r2, #768     ;            | 0x300
 *
 * Both branches stay inside 0x200..0x3ff, i.e. the last 512 of the table's
 * 0x1000 entries, which is also why sel must be 1 or 2 (see the guard in
 * zx_ppe_sbrg_adapter).  docs/vendor-cla-hash-recovered-2026-10-30.md §5.
 */
static u32 zx_ppe_sbrg_slot(u16 hash, u32 sel)
{
	if (sel == 1)
		return ((hash & 0x7f) << 2) | 0x200;
	return ((hash << 2) & 0xff) | 0x300;
}

static u64 zx_ppe_sbrg_le64(const u8 *p)
{
	u64 v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/* The entry is a 384-bit little-endian bit stream (tools/vendor-emu
 * t_v6entry.py build(), checked against the vendor code on random records):
 * bits 0..2 valid/age/mode, 3..14 vlan, 15..78 dip bytes 8..15, 96..103
 * pt_mask, 104..167 dip bytes 0..7, 192..255 sip bytes 8..15, 288..351 sip
 * bytes 0..7; words 8 and 11 are always zero.
 */
static void zx_ppe_sbrg_entry(const struct zx_ppe_sbrg_op *op,
			      u32 w[ZX_SBRG_ENTRY_WORDS])
{
	u64 dip_hi = zx_ppe_sbrg_le64(op->dip + 8);
	u64 dip_lo = zx_ppe_sbrg_le64(op->dip);
	u64 sip_lo = zx_ppe_sbrg_le64(op->sip);
	u64 sip_hi = zx_ppe_sbrg_le64(op->sip + 8);

	memset(w, 0, ZX_SBRG_ENTRY_WORDS * sizeof(u32));
	/* valid = 1, age = 0, mode = 1: what the vendor's MLD add fills in.
	 * op->pt_mask is the egress port bitmask (1 << port).
	 */
	w[0] = 1 | (1 << 2) | ((op->vlan & 0xfff) << 3) |
	       ((dip_hi & 0x1ffff) << 15);
	w[1] = (dip_hi >> 17) & 0xffffffff;
	w[2] = (dip_hi >> 49) & 0x7fff;
	w[3] = (op->pt_mask & 0xff) | ((dip_lo & 0xffffff) << 8);
	w[4] = (dip_lo >> 24) & 0xffffffff;
	w[5] = (dip_lo >> 56) & 0xff;
	w[6] = sip_hi & 0xffffffff;
	w[7] = (sip_hi >> 32) & 0xffffffff;
	w[9] = sip_lo & 0xffffffff;
	w[10] = (sip_lo >> 32) & 0xffffffff;
}

static void zx_ppe_sbrg_adapter(struct zx_eth_adapter *adapter, void *data)
{
	struct zx_ppe_sbrg_op *op = data;
	u32 words[ZX_SBRG_ENTRY_WORDS], v[ZX_SBRG_WORDS] = {};
	u32 hash = 0, sel = 0, index = op->index;
	int i, b, t, ret;

	mutex_lock(&adapter->ppe_lock);
	if (op->mode == ZX_SBRG_MODE_ADD || op->mode == ZX_SBRG_MODE_DEL ||
	    op->mode == ZX_SBRG_MODE_PLAN) {
		u8 key[ZX_SBRG_KEY_LEN];

		memcpy(key, op->sip, 16);
		memcpy(key + 16, op->dip, 16);
		key[32] = op->vlan & 0xff;
		key[33] = (op->vlan >> 8) & 0xff;
		hash = zx_ppe_sbrg_hash(key, sizeof(key));
		sel = readl(adapter->base + ZX_SBRG_BASE + ZX_SBRG_SEL) & 3;
		index = zx_ppe_sbrg_slot(hash, sel);
		zx_ppe_sbrg_entry(op, words);
		if (op->mode == ZX_SBRG_MODE_DEL)
			memset(words, 0, sizeof(words));
		dev_info(adapter->dev, "sbrg: vlan %u hash %#x sel %u index %#x%s\n",
			 op->vlan, hash, sel, index,
			 op->mode == ZX_SBRG_MODE_DEL ? " (delete)" :
			 op->mode == ZX_SBRG_MODE_PLAN ? " (plan)" : "");
		for (i = 0; i < ZX_SBRG_ENTRY_WORDS; i += 3)
			dev_info(adapter->dev, "sbrg:   w%02d %08x %08x %08x\n",
				 i, words[i], words[i + 1], words[i + 2]);
	}

	switch (op->mode) {
	case ZX_SBRG_MODE_PLAN:
		break;
	case ZX_SBRG_MODE_READ:
		ret = zx_ppe_sbrg_xfer(adapter, op->table, op->index, true, v);
		if (ret)
			dev_info(adapter->dev, "sbrg: read table %u index %#x: no completion (%d)\n",
				 op->table, op->index, ret);
		else
			dev_info(adapter->dev, "sbrg: table %u index %#x = %08x %08x %08x\n",
				 op->table, op->index, v[0], v[1], v[2]);
		break;
	case ZX_SBRG_MODE_WRITE:
		ret = zx_ppe_sbrg_xfer(adapter, op->table, op->index, false, op->w);
		if (ret)
			dev_info(adapter->dev, "sbrg: write table %u index %#x: no completion (%d)\n",
				 op->table, op->index, ret);
		else
			dev_info(adapter->dev, "sbrg: wrote table %u index %#x <- %08x %08x %08x\n",
				 op->table, op->index, op->w[0], op->w[1], op->w[2]);
		break;
	default:
		/* The vendor refuses the whole operation unless table_sel reads 1
		 * or 2: sbrg_add_ipv6table+0x178 does `cmp #0 / cmpne #3` and
		 * takes the error path for both, and sbrg_lookup_ipv6table shares
		 * the check.  Only those two values give a slot base that keeps
		 * the four blocks inside the table (0x200..0x3ff), so writing
		 * under any other value would land in a region the hardware never
		 * looks at -- and would silently look like "acceleration did not
		 * take".
		 */
		if (sel != 1 && sel != 2) {
			dev_err(adapter->dev,
				"sbrg: table_sel reads %u (vendor accepts only 1 or 2), refusing to write\n",
				sel);
			break;
		}
		/* The vendor reads all four tables at the entry's four blocks
		 * before writing; the values are unused, but the hardware then
		 * sees the same sequence.  The entry itself goes to table 0.
		 */
		if (index + 3 >= 0x1000) {
			dev_err(adapter->dev,
				"sbrg: slot %#x + 3 overflows the table, refusing to write\n",
				index);
			break;
		}
		for (t = 0; t < 4; t++)
			for (b = 0; b < 4; b++)
				zx_ppe_sbrg_xfer(adapter, t, index + b, true, v);
		for (b = 0; b < 4; b++) {
			ret = zx_ppe_sbrg_xfer(adapter, 0, index + b, false,
					       &words[3 * b]);
			if (ret) {
				dev_info(adapter->dev, "sbrg: write index %#x: no completion (%d)\n",
					 index + b, ret);
				break;
			}
		}
		dev_info(adapter->dev, "sbrg: %s slots %#x..%#x done\n",
			 op->mode == ZX_SBRG_MODE_DEL ? "delete" : "add",
			 index, index + 3);
		break;
	}
	mutex_unlock(&adapter->ppe_lock);
}

static int zx_ppe_sbrg_storage;

static int zx_ppe_sbrg_set(const char *val, const struct kernel_param *kp)
{
	struct zx_ppe_sbrg_op op = {};
	char how[8] = {}, s_sip[33] = {}, s_dip[33] = {};
	unsigned int table = 0, index = 0, w0 = 0, w1 = 0, w2 = 0;
	unsigned int vlan = 0, pt = 0xff;
	int n;

	if (!val)
		return -EINVAL;
	n = sscanf(val, "%7s %i %i %x %x %x", how, &table, &index, &w0, &w1, &w2);

	/* a/d/h take the two addresses as 32 hex digits plus the VLAN. */
	if (!strcmp(how, "a") || !strcmp(how, "add") ||
	    !strcmp(how, "d") || !strcmp(how, "del") ||
	    !strcmp(how, "h") || !strcmp(how, "hash")) {
		n = sscanf(val, "%7s %32s %32s %i %x", how, s_sip, s_dip,
			   &vlan, &pt);
		if (n < 4)
			return -EINVAL;
		if (hex2bin(op.sip, s_sip, 16) || hex2bin(op.dip, s_dip, 16))
			return -EINVAL;
		if (vlan >= 0x1000 || pt > 0xff)
			return -EINVAL;
		op.vlan = vlan;
		op.pt_mask = pt;
		if (!strcmp(how, "a") || !strcmp(how, "add"))
			op.mode = ZX_SBRG_MODE_ADD;
		else if (!strcmp(how, "d") || !strcmp(how, "del"))
			op.mode = ZX_SBRG_MODE_DEL;
		else
			op.mode = ZX_SBRG_MODE_PLAN;
		zx_eth_for_each_adapter(zx_ppe_sbrg_adapter, &op);
		return 0;
	}

	if (n < 3)
		return -EINVAL;
	if (table > 4 || index >= 0x1000)
		return -EINVAL;
	if (!strcmp(how, "w") || !strcmp(how, "write")) {
		if (n != 6)
			return -EINVAL;
		op.mode = ZX_SBRG_MODE_WRITE;
	} else if (!strcmp(how, "r") || !strcmp(how, "read")) {
		op.mode = ZX_SBRG_MODE_READ;
	} else {
		return -EINVAL;
	}
	op.table = table;
	op.index = index;
	op.w[0] = w0;
	op.w[1] = w1;
	op.w[2] = w2;
	zx_eth_for_each_adapter(zx_ppe_sbrg_adapter, &op);
	return 0;
}

static const struct kernel_param_ops zx_ppe_sbrg_ops = {
	.set = zx_ppe_sbrg_set,
	.get = param_get_int,
};
module_param_cb(sbrg, &zx_ppe_sbrg_ops, &zx_ppe_sbrg_storage, 0644);
MODULE_PARM_DESC(sbrg,
		 "vendor IPv6 multicast table at 0x388000: \"r <table> <index>\", \"w <table> <index> <w0> <w1> <w2>\", or \"h|a|d <sip32hex> <dip32hex> <vlan> [pt_mask]\" (pt_mask = egress port bits)");

static void zx_ppe_dump_agewords(struct zx_eth_adapter *adapter);

static void zx_ppe_dump_adapter(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	if (!adapter->ppe_active)
		dev_info(adapter->dev, "dump: ppe inactive, reading the tables anyway\n");
	if (zx_ppe_dump & 1)
		zx_ppe_dump_tables(adapter);
	if (zx_ppe_dump & 2)
		zx_ppe_dump_hash(adapter);
	if (zx_ppe_dump & 4)
		zx_ppe_dump_regs(adapter);
	if (zx_ppe_dump & 8)
		zx_ppe_dump_tablescan(adapter);
	if (zx_ppe_dump & 32)
		zx_ppe_dump_agewords(adapter);
	mutex_unlock(&adapter->ppe_lock);
}

static int zx_ppe_dump_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	if (zx_ppe_dump)
		zx_eth_for_each_adapter(zx_ppe_dump_adapter, NULL);
	return 0;
}

static const struct kernel_param_ops zx_ppe_dump_ops = {
	.set = zx_ppe_dump_set,
	.get = param_get_int,
};
module_param_cb(dump, &zx_ppe_dump_ops, &zx_ppe_dump, 0644);
MODULE_PARM_DESC(dump,
		 "read the classifier back to dmesg: 1 = index/rule tables, 2 = hash banks + aging, 4 = CLA registers, 8 = table-id scan, 32 = aging words only");

/*
 * The aging word of every slot, eight to a line: the oracle for "did the
 * hardware look up this key, and in which slot".  Diffing two of these is far
 * cheaper than dumping 520 entries, and it fits in dmesg.
 */
static void zx_ppe_dump_agewords(struct zx_eth_adapter *adapter)
{
	char buf[8 * 9 + 24];
	int s, i, len;

	for (s = 0; s < ZX_PPE_HASH_SLOTS_EXT; s += 8) {
		len = scnprintf(buf, sizeof(buf), "cla-age[%3d]", s);
		for (i = 0; i < 8 && s + i < ZX_PPE_HASH_SLOTS_EXT; i++) {
			u32 ag = 0;

			if (zx_pp_read_word0(adapter, ZX_CLA_BASE, ZX_CLA_AGING,
					     s + i, &ag))
				ag = 0xffffffff;
			len += scnprintf(buf + len, sizeof(buf) - len, " %08x", ag);
		}
		dev_info(adapter->dev, "%s\n", buf);
	}
}

/*
 * Zero every hash slot.  The classifier keeps its entries in switch SRAM that
 * survives a module reload, so a diagnostic run that wrote all 520 slots (the
 * shotgun) leaves them behind: the driver's bookkeeping comes back fresh while
 * the hardware is full, and every later add lands in a slot the hardware never
 * looks in.  This clears that state without a power cycle.  The work itself is
 * zx_ppe_hash_zero(), which zx_ppe_stop() also uses as the undo of a v6_hwenc
 * run; v4 flows lose their hardware entries here and are re-added by the next
 * flowtable bind (firewall reload), which is the documented cost.
 */
static void zx_ppe_cla_clear_adapter(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	zx_ppe_hash_zero(adapter);
	mutex_unlock(&adapter->ppe_lock);
	dev_info(adapter->dev, "cla_clear: %d hash slots and their aging words zeroed\n",
		 ZX_PPE_HASH_SLOTS_EXT);
}

static int zx_ppe_cla_clear_storage;

static int zx_ppe_cla_clear_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	if (zx_ppe_cla_clear_storage)
		zx_eth_for_each_adapter(zx_ppe_cla_clear_adapter, NULL);
	return 0;
}

static const struct kernel_param_ops zx_ppe_cla_clear_ops = {
	.set	= zx_ppe_cla_clear_set,
	.get	= param_get_int,
};
module_param_cb(cla_clear, &zx_ppe_cla_clear_ops, &zx_ppe_cla_clear_storage, 0644);
MODULE_PARM_DESC(cla_clear, "write 1 to zero all 520 IPv6/IPv4 hash slots and their aging words");

/*
 * "echo 1 > .../parameters/ipv6" programmes the v6 tables and lifts the v6
 * ethertype trap; "echo 0" writes the tables' enable off and traps IPv6 again.
 * The trap half is the load-bearing part: with it on, no IPv6 frame can be
 * missed (all of them cross the CPU); with it off, LAN-to-LAN IPv6 is switched
 * in hardware and routed IPv6 goes through the classifier before the CPU.
 */
static int zx_ppe_ipv6_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;
	zx_eth_for_each_adapter(zx_ppe_v6_toggle_adapter, NULL);
	return 0;
}

/* classifier L3 MTU and actions, packet modifier padding of short frames.
 * The "other L3 packet" action (0x38c0cc) keeps the value 1 of the base
 * driver: with the stock value 0, IPv6 addressed to our MACs was dropped
 * once its trap was lifted (hardware LAN switching).
 */
static const u32 zx_ppe_regs[4] = { 0x38c088, 0x38c094, 0x38c098, 0x39c034 };
static const u32 zx_ppe_reg_vals[4] = { 0x7fff, 4, 0x7fff7fff, 0x3d };



/*
 * Routed frames must reach the classifier: mark our MACs as routed (ONU
 * MAC table) and stop trapping IPv4 and frames to our MACs straight to the
 * CPU.  IPv6, ARP and VLAN tagged frames stay trapped.  Frames the
 * classifier does not know still go to the CPU.
 */
static int zx_ppe_start(struct zx_eth_adapter *adapter)
{
	static const u32 cmd = 0x00010000;
	void __iomem *base = adapter->base;
	int i, ret;

	zx_ppe_v6_tune();
	for (i = 0; i < ZX_PPE_SUBNETS; i++)
		adapter->ppe_subnet[i].refs = 0;
	bitmap_zero(adapter->ppe_hash_used, ZX_PPE_HASH_SIZE);
	bitmap_zero(adapter->ppe_flow_used, ZX_PPE_FLOWS);
	bitmap_zero(adapter->ppe_nh_used, ZX_PPE_NEXT_HOPS);
	bitmap_zero(zx_ppe_hit_logged, ZX_PPE_HASH_SIZE);
	memset(zx_ppe_cookie_map, 0, sizeof(zx_ppe_cookie_map));

	for (i = 0; i < ARRAY_SIZE(zx_ppe_regs); i++) {
		adapter->ppe_saved[i] = readl(base + zx_ppe_regs[i]);
		writel(zx_ppe_reg_vals[i], base + zx_ppe_regs[i]);
	}

	ret = zx_pp_write(adapter, ZX_PM_BASE, ZX_PM_CMD, 0, &cmd, 1);
	if (!ret)
		ret = zx_pp_write(adapter, ZX_PM_BASE, ZX_PM_SUB, 0, zx_ppe_sub, 2);
	if (!ret)
		ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
				  ZX_PPE_V4_RULE, zx_ppe_v4_rule, 16);
	if (!ret)
		ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX,
				  ZX_PPE_V4_INDEX, zx_ppe_v4_index, 5);
	if (!ret && zx_ppe_ipv6)
		ret = zx_ppe_v6_tables(adapter);
	if (ret) {
		for (i = 0; i < ARRAY_SIZE(zx_ppe_regs); i++)
			writel(adapter->ppe_saved[i], base + zx_ppe_regs[i]);
		return ret;
	}

	adapter->ppe_active = true;
	/* the IPv6 trap may only be lifted once its tables are in the hardware */
	zx_ppe_set_v6_offload(zx_ppe_ipv6);
	zx_ppe_program_v6_addrs(adapter);
	zx_update_mac_tables(adapter);
	zx_update_traps(adapter);
	zx_ppe_apply_trap_override(adapter);
	dev_info(adapter->dev, "hardware flow offload enabled\n");
	return 0;
}

static void zx_ppe_flush(void *ptr, void *arg)
{
	struct zx_ppe_entry *e = ptr;

	zx_ppe_entry_remove(arg, e);
	kfree(e);
}

static void zx_ppe_stop(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	int i;

	/* back to trapping everything, then drop the tables */
	adapter->ppe_active = false;
	zx_ppe_set_v6_offload(false);
	zx_update_traps(adapter);
	zx_update_mac_tables(adapter);

	rhashtable_free_and_destroy(&adapter->ppe_flows, zx_ppe_flush, adapter);
	rhashtable_init(&adapter->ppe_flows, &zx_ppe_ht_params);
	/* One-button undo, in the order the runbook checks it: zero the hash
	 * slots first (the entries are useless without the tables), then put
	 * every extract-rule entry we overwrote back word for word.  The 520
	 * slots are only wiped when the hardware-verified v6 encoding was on -
	 * that is the mode that can leave entries behind (v6_shotgun), and a
	 * wipe costs a few hundred indirect writes, so the plain v4 unload keeps
	 * its old, instant path.
	 */
	if (zx_ppe_v6_hwenc && zx_ppe_v6_wipe) {
		zx_ppe_hash_zero(adapter);
		dev_info(adapter->dev, "cla_clear: %d hash slots and their aging words zeroed\n",
			 ZX_PPE_HASH_SLOTS_EXT);
	}
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, ZX_PPE_V4_INDEX, NULL, 0);
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE, ZX_PPE_V4_RULE, NULL, 0);
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, zx_ppe_v6_ext_idx, NULL, 0);
	if (!zx_ppe_v6_hwenc)
		zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
			    zx_ppe_v6_rule_idx, NULL, 0);
	zx_ppe_rule_restore(adapter);
	for (i = 0; i < ARRAY_SIZE(zx_ppe_regs); i++)
		writel(adapter->ppe_saved[i], base + zx_ppe_regs[i]);
	dev_info(adapter->dev, "hardware flow offload disabled\n");
}

/* FLOW_CLS_REPLACE / DESTROY / STATS for one flow.  The caller is the
 * flow-block trampoline in the built-in driver, which holds adapter->ppe_lock
 * and a reference on this module for the duration of the call.
 */
static int zx_ppe_flow_cb(struct zx_eth_adapter *adapter,
			  struct flow_cls_offload *f)
{
	int ret;

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		ret = zx_ppe_flow_add(adapter, f);
		if (ret == -EOPNOTSUPP)
			adapter->ppe_add_skip++;
		else if (ret && ret != -EEXIST)
			adapter->ppe_add_fail++;
		break;
	case FLOW_CLS_DESTROY:
		ret = zx_ppe_flow_del(adapter, f);
		break;
	case FLOW_CLS_STATS:
		ret = zx_ppe_flow_stats(adapter, f);
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	return ret;
}

/* ---- module glue --------------------------------------------------- */

static const struct zx_ppe_hooks zx_ppe_kmod_hooks = {
	.owner			= THIS_MODULE,
	.flow_cb		= zx_ppe_flow_cb,
	.start			= zx_ppe_start,
	.stop			= zx_ppe_stop,
};

/*
 * A load has to pick up the flowtable bindings that were made without it.  TC
 * keeps its blocks across an unload (their callbacks live in the built-in
 * driver), so a module that comes back finds a bound flowtable and torn-down
 * tables; the core answers whether anything is still bound.
 */
static void zx_ppe_attach(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	if (!adapter->ppe_active && zx_ppe_has_bound_blocks(adapter)) {
		if (zx_ppe_start(adapter))
			dev_warn(adapter->dev, "hardware flow offload could not be re-enabled\n");
	}
	mutex_unlock(&adapter->ppe_lock);
}

/*
 * An unload deliberately leaves the flowtable bindings alone - their callbacks
 * live in the built-in driver, so they outlive this module.  Only the hardware
 * state goes away with the code; the next load picks the bindings back up
 * (zx_ppe_attach), and so does a later bind after a ruleset reload.
 */
static void zx_ppe_detach(struct zx_eth_adapter *adapter, void *data)
{
	mutex_lock(&adapter->ppe_lock);
	if (adapter->ppe_active)
		zx_ppe_stop(adapter);
	mutex_unlock(&adapter->ppe_lock);
}

static int __init zx_ppe_init(void)
{
	int ret;

	ret = zx_ppe_hooks_register(&zx_ppe_kmod_hooks);
	if (ret) {
		pr_err("zx279128s-ppe: the switch driver refused the hooks (%d); another engine is registered\n",
		       ret);
		return ret;
	}

	zx_eth_for_each_adapter(zx_ppe_attach, NULL);
	pr_info("zx279128s-ppe: engine loaded, IPv6 classifier %s\n",
		zx_ppe_ipv6 ? "on (experimental)" : "off");
	return 0;
}

static void __exit zx_ppe_exit(void)
{
	zx_ppe_hooks_unregister(&zx_ppe_kmod_hooks);
	zx_eth_for_each_adapter(zx_ppe_detach, NULL);
}

module_init(zx_ppe_init);
module_exit(zx_ppe_exit);

MODULE_AUTHOR("Navid Ghahremani <ghahramani.navid@gmail.com>");
MODULE_DESCRIPTION("ZTE zx279128s Ethernet switch: hardware flow offload engine");
MODULE_LICENSE("GPL");
