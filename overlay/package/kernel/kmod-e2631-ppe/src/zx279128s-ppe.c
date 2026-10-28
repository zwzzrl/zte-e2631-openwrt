// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s Ethernet switch: hardware flow offload
 *
 * Copyright (C) 2026 Navid Ghahremani <ghahramani.navid@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/rhashtable.h>
#include <linux/slab.h>
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

static int zx_ppe_v6_entry = -1;
module_param_named(v6_entry, zx_ppe_v6_entry, int, 0644);
MODULE_PARM_DESC(v6_entry,
	"extract index byte inside the IPv6 hash entry (-1: same as v6_ext)");

static int zx_ppe_v6_rule_idx = ZX_PPE_V6_RULE;
module_param_named(v6_rule, zx_ppe_v6_rule_idx, int, 0644);
MODULE_PARM_DESC(v6_rule, "extra rule table entry the IPv6 windows are written to");

static int zx_ppe_v6_key = -1;
module_param_named(v6_key, zx_ppe_v6_key, int, 0644);
MODULE_PARM_DESC(v6_key, "id the IPv6 hash key carries (-1: same as v6_rule)");

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

	if (t->v6) {
		hw[0] = t->proto << 8;
		for (i = 0; i < 8; i++)
			hw[1 + i] = t->saddr6[2 * i] << 8 | t->saddr6[2 * i + 1];
		for (i = 0; i < 8; i++)
			hw[9 + i] = t->daddr6[2 * i] << 8 | t->daddr6[2 * i + 1];
		hw[17] = be16_to_cpu(t->sport);
		hw[18] = be16_to_cpu(t->dport);
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
	int i, n;

	n = zx_ppe_halfwords(t, hw);
	zx_ppe_put_bits(key, 23, 8, t->v6 ? zx_ppe_v6_key_byte() : ZX_PPE_V4_RULE);
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
	int i, n, words;

	n = zx_ppe_halfwords(t, hw);
	/* valid, extract index, upstream */
	zx_ppe_put_bits(key, 0, 8,
			0x40 | (t->v6 ? zx_ppe_v6_entry_idx() : ZX_PPE_V4_INDEX));
	for (i = 0; i < n; i++)
		zx_ppe_put_bits(key, 24 + 16 * i, 16, hw[i]);
	w[0] = FIELD_PREP(ZX_CLA_W0_FLOW, flow) | ZX_CLA_W0_FWD |
	       FIELD_PREP(ZX_CLA_W0_EGRESS, ZX_BRPORT(port)) | ZX_CLA_W0_BITS;
	w[1] = 0xfa11c000;
	w[2] = 0x00000608;
	w[3] = 0x80000000;
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
	zx_ppe_put_bits(b, 34, 1, 1);			/* decrement TTL/hop limit */
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
	ret = zx_pp_write(adapter, ZX_CLA_BASE,
			  slot[1] < ZX_PPE_HASH0_SIZE ? ZX_CLA_HASH0 : ZX_CLA_HASH1,
			  slot[1] % ZX_PPE_HASH0_SIZE, w, words);
	if (ret)
		goto err_nh;

	if (t.v6 && zx_ppe_v6_shotgun) {
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
 * The remaining difference is the key string itself: the vendor does not build
 * it from a homogeneous window list.  tm.ko's tm_acl_fast_add_v4v6() packs the
 * 45 bytes out of a hash rule computed in software by tm_acl_get_fastHashRule()
 * (60-byte struct, halfword windows at offsets 16..46), and switch.ko's
 * per-family descriptors differ between the families (npu_ipv4_l3tcpup_cap vs
 * npu_ipv6_l3tcpup_cap, same size, different field map).  Until that is
 * reproduced ipv6=1 means "correct forwarding, no fast path" - which is why it
 * is not the default.
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
	if (zx_ppe_v6_idxw4 >= 0)
		zx_ppe_v6_index[4] = zx_ppe_v6_idxw4;
}

/* Put the IPv6 extract rule and index into the hardware.  Split out of
 * zx_ppe_start() because the runtime ipv6 switch below has to do the same.
 */
static int zx_ppe_v6_tables(struct zx_eth_adapter *adapter)
{
	int ret;

	zx_ppe_v6_tune();
	ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE,
			  zx_ppe_v6_rule_idx, zx_ppe_v6_rule, 17);
	if (!ret)
		ret = zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX,
				  zx_ppe_v6_ext_idx, zx_ppe_v6_index, 5);
	return ret;
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
 * idx) << 6; if the hardware reads IPv6 entries from there rather than from the
 * two internal banks, that would explain a v6 lookup that is clearly active
 * (frames are dropped instead of being sent to the CPU) and never matches.
 * Asking for that memory directly may fault, ids are free to try.
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
		 "read the classifier back to dmesg: 1 = index/rule tables, 2 = hash banks + aging, 4 = CLA registers, 8 = table-id scan");

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
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, ZX_PPE_V4_INDEX, NULL, 0);
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE, ZX_PPE_V4_RULE, NULL, 0);
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_INDEX, zx_ppe_v6_ext_idx, NULL, 0);
	zx_pp_write(adapter, ZX_CLA_BASE, ZX_CLA_EXTRA_RULE, zx_ppe_v6_rule_idx, NULL, 0);
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
