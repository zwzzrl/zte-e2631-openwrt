// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s Ethernet switch and DMA driver
 *
 * Copyright (C) 2026 Navid Ghahremani <ghahramani.navid@gmail.com>
 *
 * The zx279128s has a five-port gigabit switch with a traffic manager (TM),
 * a buffer management unit (BMU) and a DMA engine that moves frames between
 * the switch and the CPU. Every port is its own net_device (lan1-lan4 and wan
 * on the ZTE ZXHN H3600). Ports in the same Linux bridge switch known unicast
 * in hardware (zx279128s-switch.c), and established flows can be offloaded to
 * the packet processor (zx279128s-ppe.c).
 *
 * All frames pass through buffers of the BMU pool, which lives in a 16 MiB
 * DMA arena that is allocated once at probe and kept. Received frames are
 * copied out of their buffer into an skb, frames to send are copied into a
 * buffer. The ingress port of a received frame is in its descriptor, and a
 * frame to send goes to the TM queue of its port. The hardware tables are
 * programmed from scratch, with the values of the vendor firmware.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>

#include "zx279128s-eth.h"
#include "zx279128s-ppe.h"

/*
 * Map the 4096 x 2048 B DMA buffer pool cacheable (streaming DMA, so every
 * frame pays a cache invalidate on receive and a clean on transmit) instead of
 * uncached (the coherent arena, no maintenance at all).  Both costs are
 * measured per frame in docs/ffe-cacheable-pool-2026-10-30.md, and the zero-copy
 * path that skips the copy entirely in docs/ffe-zero-copy-2026-10-30.md.
 */
static bool pool_cacheable;
module_param(pool_cacheable, bool, 0644);
MODULE_PARM_DESC(pool_cacheable, "Map the packet pool cacheable (default: uncached)");

/*
 * 零拷贝：接收时把 BMU 池里的槽位直接当成 skb 的数据区交给协议栈，发送时把同一个
 * bp 直接交给硬件，省掉每包两次 payload 搬运。说明见 zx_zc_* 那一节。
 * 默认关（0）：这条路径的收益全在转发（UDP 900M 单流 612 → 741 Mbit/s、8 流
 * TCP 565 → 700 Mbit/s），代价是本机收包吞吐下降。公开版本把软件加速搁置，
 * 默认交给 PPE 硬件流表（accel_v4=hw），代码保留、参数保留。
 *
 * P0 之后 zc 归加速框架按族记账（accel_ipv4 / accel_ipv6 = zc，见
 * zx279128s-accel.c 与 accel->zc_mask），这个参数只剩兼容作用：zc_rx=1 等价于
 * 开机把两族都置成 zc（它从前的全局语义）。不要再拿它当第二套开关。
 */
static bool zc_rx;
module_param(zc_rx, bool, 0644);
MODULE_PARM_DESC(zc_rx, "Zero-copy RX/TX over the BMU packet pool: legacy alias for accel_v4=3 accel_v6=3 (default: off)");

/*
 * 加速路径的默认值，每条协议族各自独立（设计 §1.1）：
 *   0 stack、1 ffe、2 hw（仅 IPv4）、3 zc
 * 默认 v4=hw、**v6=ffe**（P10.2 F5 的交付形态）：
 *   - v4 继续放行 PPE 硬件流表（nft flowtable 在喂它）；ffe 是驱动内快路，
 *     v4 上它只是 hw 的替代品，交付不启用；
 *   - v6 没有 hw 路径，PPE 的 v6 分类表实测不成立，所以 v6 默认走驱动内
 *     快路。依据：多流 `-P 8` ffe 809–904 Mbit/s vs stack 378–390（2.14×），
 *     单流 BBR ffe 886 vs stack 497（1.78×）；单流 cubic 的落后（123 vs 179）
 *     是重排/伪重传的病理，不是丢包，记为交付风险但不阻断。
 * `accel_autofallback=1` 仍兜底：ffe 判据出问题时自动回 stack。
 * ffe 默认值不依赖 nft flowtable（那是 v4→hw 的要求，见 zx_accel_set()），
 * probe 阶段只是把 v6_mode 字段写成 ffe，不调用 zx_accel_set()，不会被拒。
 */
static int accel_v4 = ZX_ACC_HW;
module_param(accel_v4, int, 0644);
MODULE_PARM_DESC(accel_v4, "IPv4 acceleration path: 0 stack, 1 ffe, 2 hw, 3 zc (default: 2)");

static int accel_v6 = ZX_ACC_FFE;
module_param(accel_v6, int, 0644);
MODULE_PARM_DESC(accel_v6, "IPv6 acceleration path: 0 stack, 1 ffe, 3 zc (default: 1)");

/*
 * 自动降级：某一族 drop > 0.1% 或 fallback > 50% 持续 10 s 就回 stack
 * （设计 §9-4）。独立开关，开机默认开，运行态写 debugfs 的 accel_autofallback。
 */
static bool accel_autofallback = true;
module_param(accel_autofallback, bool, 0644);
MODULE_PARM_DESC(accel_autofallback, "Fall back to the stack when the fast path misbehaves (default: on)");

/* 调试用：给每个 zc 槽位在"交给协议栈"的那一刻存一份前 64 字节快照，等
 * zx_zc_reap() 归还 bp 时比对。改写了就说明槽位在我们手里被别的路径用了
 * （BMU 提前发出去、发送拷贝撞进来、……），这正是"数据静默损坏"的直接证据。
 * 运行态可关：echo 0 > /sys/module/zx279128s_eth/parameters/zc_canary
 */
static bool zc_canary = true;
module_param(zc_canary, bool, 0644);
MODULE_PARM_DESC(zc_canary, "Snapshot zc slots on delivery and verify them on reap");
#define ZX_ZC_CANARY	64

/*
 * 每包阶段计时（接收 5 段、发送 4 段），默认关。一次包要 6 个 ktime_get_ns()
 * 加 11 次累加，在 1500 B/包 的转发路径上是可测的额外成本（量级 100 ns/包，
 * 60 万包/秒时约 7 % 一个核），所以发布版本不跑它。做对比实验时打开：
 *   echo 1 > /sys/module/zx279128s_eth/parameters/timing
 * 打开后阶段数据出现在 /sys/kernel/debug/92000000.ethernet/dma_state 里。
 */
static bool timing;
module_param(timing, bool, 0644);
MODULE_PARM_DESC(timing, "Per-packet stage timing (default: off)");

#define ZX_TS()	(unlikely(timing) ? ktime_get_ns() : 0)

/* ---- Packet processor engine (package kmod-e2631-ppe) --------------- */

/*
 * Everything that programs the classifier, the packet modifier and the flow
 * tables lives in the kmod now (zx279128s-ppe.h explains why, and what the
 * reason for the hooks is).  What stays here is the state the engine has to be
 * handed and cannot own: the flow table, which is initialised at probe before
 * the module can be loaded, the adapter list it walks to find the adapters it
 * never got a probe callback for, and the flow_block callbacks the TC core
 * holds on to.
 */

/* Keyed by cookie.  The core driver initialises and destroys the table at
 * probe and teardown time, so the parameters cannot live in the module.
 */
const struct rhashtable_params zx_ppe_ht_params = {
	.head_offset		= offsetof(struct zx_ppe_entry, node),
	.key_offset		= offsetof(struct zx_ppe_entry, cookie),
	.key_len		= sizeof(unsigned long),
	.automatic_shrinking	= true,
};
EXPORT_SYMBOL_GPL(zx_ppe_ht_params);

/*
 * The registered engine, or all zeroes when no module is loaded.  The storage
 * is here and not in the module on purpose: a trampoline that reads it must
 * never touch memory the module owns, because the module may be unloading.
 */
static struct zx_ppe_hooks zx_ppe_engine;
static DEFINE_MUTEX(zx_ppe_engine_lock);

int zx_ppe_hooks_register(const struct zx_ppe_hooks *hooks)
{
	if (!hooks || !hooks->owner || !hooks->flow_cb || !hooks->start ||
	    !hooks->stop)
		return -EINVAL;

	mutex_lock(&zx_ppe_engine_lock);
	if (zx_ppe_engine.owner) {
		mutex_unlock(&zx_ppe_engine_lock);
		return -EBUSY;
	}
	zx_ppe_engine = *hooks;
	mutex_unlock(&zx_ppe_engine_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(zx_ppe_hooks_register);

void zx_ppe_hooks_unregister(const struct zx_ppe_hooks *hooks)
{
	mutex_lock(&zx_ppe_engine_lock);
	if (hooks && zx_ppe_engine.owner == hooks->owner)
		memset(&zx_ppe_engine, 0, sizeof(zx_ppe_engine));
	mutex_unlock(&zx_ppe_engine_lock);
}
EXPORT_SYMBOL_GPL(zx_ppe_hooks_unregister);

/*
 * Snapshot the hooks and take a module reference for the duration of the call.
 * Both fail while the module is on its way out, and the caller then behaves as
 * if the flow could not be offloaded - which is what an absent engine means.
 */
static bool zx_ppe_engine_get(struct zx_ppe_hooks *hooks)
{
	bool ok = false;

	mutex_lock(&zx_ppe_engine_lock);
	if (zx_ppe_engine.owner && try_module_get(zx_ppe_engine.owner)) {
		*hooks = zx_ppe_engine;
		ok = true;
	}
	mutex_unlock(&zx_ppe_engine_lock);
	return ok;
}

static void zx_ppe_engine_put(const struct zx_ppe_hooks *hooks)
{
	module_put(hooks->owner);
}

/* The one flow-block callback of all ports, so each flow is offered once. */
static int zx_ppe_flow_trampoline(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	struct zx_eth_adapter *adapter = cb_priv;
	struct flow_cls_offload *f = type_data;
	struct zx_ppe_hooks hooks;
	int ret;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;
	if (!zx_ppe_engine_get(&hooks))
		return -EOPNOTSUPP;

	mutex_lock(&adapter->ppe_lock);
	ret = hooks.flow_cb(adapter, f);
	mutex_unlock(&adapter->ppe_lock);

	zx_ppe_engine_put(&hooks);
	return ret;
}

static LIST_HEAD(zx_ppe_block_cb_list);

/*
 * Whether any flowtable is bound to this adapter's ports.  The driver_list is
 * the only bookkeeping that cannot drift: the TC core adds and removes exactly
 * these entries, one per (flowtable, device), whereas a counter has to survive
 * a ruleset reload - which binds the new flowtable before unbinding the old one
 * - and an unload of the engine, which knows nothing about either.  Asking the
 * list is also what makes a reloaded module able to pick the bindings up again
 * (kmod-e2631-ppe calls this from its init, so it must be exported).
 */
bool zx_ppe_has_bound_blocks(struct zx_eth_adapter *adapter)
{
	struct flow_block_cb *cb;

	list_for_each_entry(cb, &zx_ppe_block_cb_list, driver_list) {
		if (flow_block_cb_priv(cb) == adapter)
			return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(zx_ppe_has_bound_blocks);

/*
 * ndo_setup_tc: the flowtable binding of firewall4.  This half cannot move
 * into the module either, because flow_block_cb_lookup()/flow_block_cb_add()
 * hand the TC core a function pointer (zx_ppe_flow_trampoline) that outlives
 * every unload.
 */
static int zx_eth_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data)
{
	struct zx_eth_priv *priv = netdev_priv(dev);
	struct zx_eth_adapter *adapter = priv->adapter;
	struct zx_ppe_hooks hooks;
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;
	bool have_engine = false;
	int ret = 0;

	if (type != TC_SETUP_FT)
		return -EOPNOTSUPP;
	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &zx_ppe_block_cb_list;
	block_cb = flow_block_cb_lookup(f->block, zx_ppe_flow_trampoline, adapter);

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(zx_ppe_flow_trampoline, adapter,
					       adapter, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		have_engine = zx_ppe_engine_get(&hooks);
		if (!have_engine) {
			flow_block_cb_free(block_cb);
			return -EOPNOTSUPP;
		}
		/* A ruleset reload binds the new flowtable before it unbinds the
		 * old one, and the engine only has to be told once: ppe_active is
		 * the hardware state, not a count of bindings.
		 */
		mutex_lock(&adapter->ppe_lock);
		if (!adapter->ppe_active)
			ret = hooks.start(adapter);
		mutex_unlock(&adapter->ppe_lock);
		zx_ppe_engine_put(&hooks);
		if (ret) {
			flow_block_cb_free(block_cb);
			return ret;
		}
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &zx_ppe_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
			/* Only when the last flowtable is gone: the unbind of the
			 * old one arrives after the new one was already bound.
			 */
			if (!zx_ppe_has_bound_blocks(adapter)) {
				have_engine = zx_ppe_engine_get(&hooks);
				if (have_engine) {
					mutex_lock(&adapter->ppe_lock);
					hooks.stop(adapter);
					mutex_unlock(&adapter->ppe_lock);
					zx_ppe_engine_put(&hooks);
				}
			}
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* ---- Probed adapters ------------------------------------------------ */

/*
 * The PPE module is loaded by kmodloader long after the platform devices have
 * probed, so it cannot rely on a probe callback to learn about an adapter; it
 * walks this list instead.  There is no remove path in this driver (the
 * platform driver and devm own the lifetime), so the list only ever grows, and
 * the lock exists to keep a probe that races module load from being walked
 * half-initialised.
 */
static LIST_HEAD(zx_adapter_list);
static DEFINE_MUTEX(zx_adapter_list_lock);

void zx_eth_for_each_adapter(void (*fn)(struct zx_eth_adapter *adapter,
				       void *data), void *data)
{
	struct zx_eth_adapter *adapter;

	mutex_lock(&zx_adapter_list_lock);
	list_for_each_entry(adapter, &zx_adapter_list, list)
		fn(adapter, data);
	mutex_unlock(&zx_adapter_list_lock);
}
EXPORT_SYMBOL_GPL(zx_eth_for_each_adapter);

/* zc 槽位状态（zx_bmu_free_bp 里就要用，所以定义放前面） */
#define ZX_ZC_BS_FREE	0	/* 在 BMU 空闲表里 */
#define ZX_ZC_BS_SKB	1	/* 在协议栈手里，还没人 orphan 过 */
#define ZX_ZC_BS_SHADOW	2	/* 我们在 dataref 上拿了一份，等它掉到 1 */
#define ZX_ZC_BS_TX	3	/* 已交给发送硬件，硬件发完自己归还 */
#define ZX_ZC_BS_LOST	4	/* 追不到（数据被 expand 搬走），永不归还 */

/* 追不到的槽位超过这个数就自动关 zc，免得池子被一滴滴吃空 */
#define ZX_ZC_LOST_MAX	64u

/* ---- BMU helpers --------------------------------------------------- */

/* 事件流水（定义在 zc 段），归还 bp 时也要记一笔 */
static void zx_zc_ev(struct zx_eth_adapter *adapter, u16 bp, u8 type, u8 aux);

/*
 * Allocate a buffer from the BMU (caller holds tx_lock).  A request that
 * does not complete in time stays with the BMU, which hands out its
 * buffer later: keep it pending and collect that buffer on the next call,
 * as the vendor driver does.  Starting a new request instead lost one
 * buffer for good with every timeout.
 */
static int zx_bmu_alloc_bp(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	u32 val;
	int ret;

	if (adapter->bmu_alloc_pending) {
		if (readl(base + ZX_BMU_BUF_ALLOC) & ZX_BMU_BUF_ALLOC_REQ) {
			adapter->bmu_alloc_fail++;
			return -EBUSY;
		}
		adapter->bmu_alloc_pending = false;
		val = readl(base + ZX_BMU_BUF_ID);
		if (val & ZX_BMU_BUF_ID_VALID) {
			adapter->bmu_alloc_late++;
			return FIELD_GET(ZX_BMU_BUF_ID_BP, val);
		}
	}

	val = readl(base + ZX_BMU_BUF_ALLOC);
	writel(val | ZX_BMU_BUF_ALLOC_REQ, base + ZX_BMU_BUF_ALLOC);
	ret = readl_poll_timeout_atomic(base + ZX_BMU_BUF_ALLOC, val,
					!(val & ZX_BMU_BUF_ALLOC_BUSY), 1, 1000);
	if (!ret) {
		val = readl(base + ZX_BMU_BUF_ID);
		if (val & ZX_BMU_BUF_ID_VALID)
			return FIELD_GET(ZX_BMU_BUF_ID_BP, val);
		ret = -ENOSPC;
	}

	adapter->bmu_alloc_pending = true;
	adapter->bmu_alloc_fail++;
	return ret;
}

static int zx_bmu_free_bp(struct zx_eth_adapter *adapter, u16 bp)
{
	u32 val;
	int ret = 0;

	/* 调试：记下这个 bp 最近两次是谁还的，归还一个还挂着影子的 bp 要计数 */
	if (unlikely(adapter->zc_free_at) && bp < ZX_NUM_BUFFERS) {
		adapter->zc_free_at[bp][adapter->zc_free_tog[bp] & 1] = (u32)_RET_IP_;
		adapter->zc_free_tog[bp]++;
		if (adapter->zc_state[bp] == ZX_ZC_BS_SHADOW ||
		    adapter->zc_state[bp] == ZX_ZC_BS_SKB)
			adapter->zc_badfree++;
		/* type 9 = 把 bp 还给 BMU；aux 里的 state 说明是谁还的 */
		zx_zc_ev(adapter, bp, 9,
			 (u8)(adapter->zc_state[bp] & 0xf) |
			 (adapter->zc_inring &&
			  test_bit(bp, adapter->zc_inring) ? 0x10 : 0));
	}

	spin_lock_bh(&adapter->bmu_free_lock);
	if (!adapter->bmu_free_slots) {
		ret = readl_poll_timeout_atomic(adapter->base + ZX_BMU_FREE_STATUS,
						val, val & ZX_BMU_FREE_SLOTS, 1, 5000);
		if (ret) {
			adapter->bmu_free_fail++;
			goto out;
		}
		adapter->bmu_free_slots = FIELD_GET(ZX_BMU_FREE_SLOTS, val);
	}

	adapter->bmu_free_slots--;
	writel(bp, adapter->base + ZX_BMU_BUF_FREE);
out:
	spin_unlock_bh(&adapter->bmu_free_lock);
	return ret;
}

/* ---- RX descriptor release ----------------------------------------- */

/*
 * Hand @n consumed slots of RX queue @q back to the hardware with one
 * command, as the vendor driver does (count in bits 13:4, ring B), instead
 * of two register polls and two writes per frame.
 *
 * The traffic manager counts each frame it hands to the CPU against the
 * class in descriptor byte 6 bit 0, and a release only returns slots of the
 * class it names.  Releasing a class-1 slot as class 0 leaves the frame
 * counted in the CPU queue for good: after 1024 frames the queue's reserved
 * space is used up.  So count the slots per class and release each class
 * separately, as the vendor driver does.
 */
static int zx_rx_release(struct zx_eth_adapter *adapter, int q, int class,
			 u32 n)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout_atomic(adapter->base + ZX_RX_REL_CTRL,
					val, !(val & ZX_RX_REL_BUSY), 1, 5000);
	if (ret)
		return ret;
	writel(FIELD_PREP(ZX_RX_REL_COUNT, n) | FIELD_PREP(ZX_RX_REL_CLASS, class) |
	       FIELD_PREP(ZX_RX_REL_QUEUE, q), adapter->base + ZX_RX_REL_DATA);
	writel(ZX_RX_REL_BUSY, adapter->base + ZX_RX_REL_CTRL);
	return 0;
}

/*
 * Clear a consumed descriptor.  All cleared descriptors must be visible
 * before their slots are released: with a full ring the hardware refills a
 * slot as soon as it is released, and clearing it afterwards would wipe
 * the new descriptor.
 */
static void zx_rx_clear_desc(__le32 *desc)
{
	desc[0] = 0; desc[1] = 0; desc[2] = 0; desc[3] = 0;
}

/* Count a consumed slot for its release class, then clear it. */
static void zx_rx_consume(__le32 *desc, u32 rel[2])
{
	rel[((u8 *)desc)[6] & 1]++;
	zx_rx_clear_desc(desc);
}

/* ---- MAC port helpers ---------------------------------------------- */

static void zx_port_set_speed(struct zx_eth_adapter *adapter, int port,
			      int speed, int duplex)
{
	void __iomem *reg = adapter->base + ZX_MAC_PORT(port) + ZX_MAC_CTRL;
	u32 val = readl(reg);

	if (speed == SPEED_1000) {
		val &= ~(ZX_MAC_CTRL_MII | ZX_MAC_CTRL_100);
		val |= ZX_MAC_CTRL_FD;
	} else {
		if (duplex == DUPLEX_FULL) {
			val |= ZX_MAC_CTRL_MII | ZX_MAC_CTRL_FD;
		} else {
			val &= ~ZX_MAC_CTRL_FD;
			val |= ZX_MAC_CTRL_MII;
		}
		if (speed == SPEED_100)
			val |= ZX_MAC_CTRL_100;
		else
			val &= ~ZX_MAC_CTRL_100;
	}
	writel(val, reg);
}

static void zx_port_enable(struct zx_eth_adapter *adapter, int port, bool enable)
{
	void __iomem *reg = adapter->base + ZX_MAC_PORT(port) + ZX_MAC_CTRL;
	u32 val = readl(reg);

	if (enable)
		val |= ZX_MAC_CTRL_EN;
	else
		val &= ~ZX_MAC_CTRL_EN;
	writel(val, reg);
}

/* ---- PHY link callbacks -------------------------------------------- */

static struct zx_eth_priv *zx_config_to_priv(struct phylink_config *config)
{
	return netdev_priv(to_net_dev(config->dev));
}

static void zx_mac_config(struct phylink_config *config, unsigned int mode,
			  const struct phylink_link_state *state)
{
	/* the MAC follows the link in zx_mac_link_up() only */
}

static void zx_mac_link_down(struct phylink_config *config, unsigned int mode,
			     phy_interface_t interface)
{
	struct zx_eth_priv *priv = zx_config_to_priv(config);

	zx_port_enable(priv->adapter, priv->port, false);
	zx_sw_port_down(priv->adapter, priv->port);
}

static void zx_mac_link_up(struct phylink_config *config,
			   struct phy_device *phy, unsigned int mode,
			   phy_interface_t interface, int speed, int duplex,
			   bool tx_pause, bool rx_pause)
{
	struct zx_eth_priv *priv = zx_config_to_priv(config);

	zx_port_set_speed(priv->adapter, priv->port, speed, duplex);
	zx_port_enable(priv->adapter, priv->port, true);
}

static const struct phylink_mac_ops zx_phylink_mac_ops = {
	.mac_config	= zx_mac_config,
	.mac_link_down	= zx_mac_link_down,
	.mac_link_up	= zx_mac_link_up,
};

/*
 * Frames addressed to one of our MAC addresses go to the CPU, whatever
 * their type (the ethertype traps below cover the common ones).
 */
void zx_set_trap_dmac(struct zx_eth_adapter *adapter, int idx, const u8 *mac)
{
	writel(mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5],
	       adapter->base + ZX_SPA_TRAP_DMAC + idx * 8);
	writel(mac[0] << 8 | mac[1], adapter->base + ZX_SPA_TRAP_DMAC + idx * 8 + 4);
}
EXPORT_SYMBOL_GPL(zx_set_trap_dmac);

/*
 * Traps that send frames to the CPU before the flow lookup: the IPv4
 * ethertype trap, and the registered packet class for TCP segments without
 * payload (trap reason 0x2c), which would send every pure ACK of an
 * offloaded connection through the software path.  Both are off while
 * offload is active.  SYN, FIN and RST still reach the CPU, so conntrack
 * sees connections open and close.  Frames for the router, broadcast,
 * multicast and unknown unicast still reach the CPU through the other traps.
 *
 * The IPv6 ethertype trap stays on unless the engine says its IPv6 tables are
 * in the hardware: with no IPv6 rule the classifier cannot classify an
 * IPv6 frame, and once offload is active the ports' MACs live in the ONU
 * "routed" table instead of the trap table, so an un-trapped IPv6 frame would
 * have no way back to the stack -- that is exactly how IPv6 TCP used to stall
 * once the flowtable was handed to the PPE.  With the tables programmed the
 * trap is lifted like the IPv4 one, because a trapped ethertype never reaches
 * the classifier at all; measured on hardware, IPv6 frames then still reach
 * the stack and IPv6 forwarding stays correct (2,800 of 2,800 packets per
 * burst at both settings).  What it does not do is make IPv6 faster: the
 * classifier does not match the IPv6 hash entries this driver writes yet, so
 * the datapath stays on the CPU either way -- see PPE-IPV6-TCP in
 * docs/known-issues.md.  Lifting the trap is still the prerequisite for a
 * fast path, which is why the switch is the engine's to flip.
 */
static bool zx_ppe_v6_offload;
static DEFINE_SPINLOCK(zx_ppe_v6_offload_lock);

void zx_ppe_set_v6_offload(bool on)
{
	spin_lock(&zx_ppe_v6_offload_lock);
	zx_ppe_v6_offload = on;
	spin_unlock(&zx_ppe_v6_offload_lock);
}
EXPORT_SYMBOL_GPL(zx_ppe_set_v6_offload);

void zx_update_traps(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	bool fwd, v6;
	u32 val;

	spin_lock(&zx_ppe_v6_offload_lock);
	v6 = zx_ppe_v6_offload;
	spin_unlock(&zx_ppe_v6_offload_lock);

	spin_lock(&adapter->trap_lock);
	fwd = READ_ONCE(adapter->ppe_active) || READ_ONCE(adapter->sw_members);
	writel((fwd ? 0 : ETH_P_IP << 16) | (v6 ? 0 : ETH_P_IPV6),
	       base + ZX_SPA_TRAP_ETH_TYPE);
	val = readl(base + ZX_SPA_UP_REG_PKT_EN + 8);
	if (fwd)
		val &= ~ZX_SPA_REG_PKT_TCP_ACK;
	else
		val |= ZX_SPA_REG_PKT_TCP_ACK;
	writel(val, base + ZX_SPA_UP_REG_PKT_EN + 8);
	spin_unlock(&adapter->trap_lock);
}
EXPORT_SYMBOL_GPL(zx_update_traps);

/*
 * Frames to the ports' own addresses reach the CPU through the trap table,
 * or with flow offload active, through the ONU MAC table, which marks them
 * as routed so that the classifier sees them first.  Addresses beyond the
 * trap table still reach the CPU as unknown unicast.  Called with ppe_lock
 * held, after an address or the offload state changed.
 */
void zx_update_mac_tables(struct zx_eth_adapter *adapter)
{
	static const u8 none[ETH_ALEN];
	void __iomem *base = adapter->base;
	const u8 *addrs[ZX_NUM_PORTS];
	int i, j, n = 0;
	u32 w[2];

	lockdep_assert_held(&adapter->ppe_lock);

	/* each distinct port address once */
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *dev = adapter->ports[i];

		if (!dev)
			continue;
		for (j = 0; j < n; j++)
			if (ether_addr_equal(addrs[j], dev->dev_addr))
				break;
		if (j == n)
			addrs[n++] = dev->dev_addr;
	}

	for (i = 0; i < ZX_SPA_ONU_MAC_NUM; i++) {
		zx_mac_words(adapter->ppe_active && i < n ? addrs[i] : none, w);
		writel(w[0], base + ZX_SPA_ONU_MAC + i * 8);
		writel(w[1], base + ZX_SPA_ONU_MAC + i * 8 + 4);
	}
	for (i = 0; i < ZX_SPA_TRAP_DMAC_NUM; i++)
		zx_set_trap_dmac(adapter, i,
				 !adapter->ppe_active && i < n ? addrs[i] : none);
}
EXPORT_SYMBOL_GPL(zx_update_mac_tables);

static void zx_dma_enable(struct zx_eth_adapter *adapter, bool enable)
{
	u32 val = readl(adapter->base + ZX_DMA_CTRL);

	val &= ~ZX_DMA_CTRL_EN;
	val |= ZX_DMA_CTRL_BIT21 | (enable ? ZX_DMA_CTRL_EN : 0);
	writel(val, adapter->base + ZX_DMA_CTRL);
	readl(adapter->base + ZX_DMA_CTRL);
}

static int zx_tm_write(struct zx_eth_adapter *adapter, u32 table, u32 index,
		       const u32 data[4])
{
	void __iomem *base = adapter->base;
	u32 val;
	int i, ret;

	ret = readl_poll_timeout(base + ZX_TM_IND_STATUS, val,
				 val & ZX_TM_IND_READY, 1, 1000);
	if (ret)
		return ret;
	writel(FIELD_PREP(ZX_TM_IND_INDEX, index) |
	       FIELD_PREP(ZX_TM_IND_TABLE, table), base + ZX_TM_IND_CMD);
	for (i = 3; i >= 0; i--)
		writel(data[i], base + ZX_TM_IND_DATA(i));
	return 0;
}

static int zx_tm_read0(struct zx_eth_adapter *adapter, u32 table, u32 index,
		       u32 *data)
{
	void __iomem *base = adapter->base;
	u32 val;
	int ret;

	ret = readl_poll_timeout(base + ZX_TM_IND_STATUS, val,
				 val & ZX_TM_IND_READY, 1, 1000);
	if (ret)
		return ret;
	writel(FIELD_PREP(ZX_TM_IND_INDEX, index) |
	       FIELD_PREP(ZX_TM_IND_TABLE, table) | ZX_TM_IND_READ,
	       base + ZX_TM_IND_CMD);
	ret = readl_poll_timeout(base + ZX_TM_IND_STATUS, val,
				 val & ZX_TM_IND_READY, 1, 1000);
	if (ret)
		return ret;
	*data = readl(base + ZX_TM_IND_DATA(0));
	return 0;
}

/* Queue, RED and scheduler tables.  The RED profiles use the vendor
 * pon_tm_red_init() value.
 *
 * Queue word 0 is the reserved space (bits 10:0) plus the shared space
 * (bits 25:11), in buffers.  Queues 0-15 feed the CPU.  They get 1023
 * reserved and no shared buffers, as in the stock firmware: a frame stays
 * counted in its queue until the driver releases its RX slot, so the
 * traffic manager never has more than 1023 frames in the 1024-slot RX
 * ring and drops the excess itself.  With more, a burst faster than the
 * driver drains the ring makes the DMA overwrite slots that were not read
 * yet, and those frames and their buffers are lost.
 *
 * The queue usage in table 1 survives the reset.  U-Boot receives a TFTP
 * download on CPU queue 0 and never returns the slots, so after a TFTP
 * boot the queue starts with up to 1024 frames counted that will never be
 * released, and a 1023 limit would drop every frame.  Raise the limit of
 * each CPU queue by what is left counted in it.
 */
static int zx_tm_init(struct zx_eth_adapter *adapter)
{
	static const u32 queue[4] = { 0x00800400, 0, 0, 0 };
	static const u32 red[4] = { 0x00200020, 0, 0, 0 };
	static const u32 scheduler[4] = {
		0xff803fff, 0x0100ff80, 0x00100200, 32
	};
	u32 cpu_queue[4] = { 0, 0, 0, 0 };
	u32 used, limit;
	int i, ret;

	for (i = 0; i < 16; i++) {
		ret = zx_tm_read0(adapter, ZX_TM_TABLE_QUEUE_USE, i, &used);
		if (ret)
			return ret;
		used = (used & 0x7ff) + ((used >> 11) & 0x7fff);
		if (used)
			dev_dbg(adapter->dev,
				"CPU queue %d: %u frames left counted by the boot loader\n",
				i, used);
		limit = 1023 + used;
		cpu_queue[0] = min(limit, 0x7ffU) | (limit - min(limit, 0x7ffU)) << 11;
		ret = zx_tm_write(adapter, ZX_TM_TABLE_QUEUE, i, cpu_queue);
		if (ret)
			return ret;
	}
	for (i = 16; i < 400; i++) {
		ret = zx_tm_write(adapter, ZX_TM_TABLE_QUEUE, i, queue);
		if (ret)
			return ret;
	}
	for (i = 0; i < 384; i++) {
		ret = zx_tm_write(adapter, ZX_TM_TABLE_RED, i, red);
		if (ret)
			return ret;
		ret = zx_tm_write(adapter, ZX_TM_TABLE_SCHED, i, scheduler);
		if (ret)
			return ret;
	}
	return 0;
}

/* ---- Hardware initialisation --------------------------------------- */

static void zx_pp_init(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;

	/* Mode-2 TX goes through the packet processor bridge.  The bootloader
	 * initializes it for TFTP, but not when booting directly from NAND.
	 * Initialize it ourselves, while DMA and the MACs are quiesced.  These
	 * are the H3600 bootloader's PP reset, bridge and classifier settings.
	 */
	writel(2, base + ZX_PP_RESET);
	usleep_range(1000, 2000);

	writel(0x000200ff, base + ZX_SBRG_PORT_CTRL);
	/* Stock firmware value.  With the reset value 0x00010000 the bridge
	 * steered IPv6 unicast arriving on WAN to a queue the driver never
	 * services, and each frame kept a BMU buffer: about one leaked
	 * buffer per background WAN frame on an IPv6 network.
	 */
	writel(0x0000dfdf, base + 0x388008);
	/* The bridge would forward a frame between ports only through VLAN
	 * table entries, which the driver does not program, so it drops what
	 * the traps leave.  Frames between two LAN ports must reach the Linux
	 * bridge instead.  Turn off source address learning, so that every
	 * unicast destination is unknown, and send unknown unicast arriving on
	 * a switch port (bridge ports 1-5) to the CPU.  Without this, lifting
	 * the IPv4 trap for flow offload loses IPv4 between two LAN hosts.
	 */
	writel(0x00000000, base + ZX_SBRG_LEARN);
	writel(0xff5ffdff, base + 0x388340);
	writel(0x0000003e, base + 0x388344);
	writel(0x0000003f, base + 0x388380);
	writel(0xaaaaaaaa, base + 0x38863c);
	writel(0x00005555, base + 0x3881c4);
	writel(0x000bf874, base + 0x388188);
	writel(0x000000ff, base + 0x3882c0);
	writel(0x0000ffff, base + 0x388300);
	writel(0x0000003e, base + 0x388304);

	writel(0x0000309a, base + 0x38c080);
	writel(0, base + 0x38c088);
	writel(1, base + 0x38c0cc);
	writel(0, base + 0x3a0010);
	writel(0, base + 0x3a0014);
}

/* Reset the switch and leave its ports and DMA stopped */
static void zx_hw_reset(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	int i;

	/* Master reset pulse */
	writel(0, base + ZX_GLB_RESET);
	fsleep(10);
	writel(0xffffffff, base + ZX_GLB_RESET);

	/* Quiesce all 5 ports */
	for (i = 0; i < ZX_NUM_PORTS; i++)
		zx_port_enable(adapter, i, false);
	zx_dma_enable(adapter, false);
	writel(0, base + ZX_BMU_CTRL);
}

static int zx_hw_init(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	static const int port_to_sw_idx[8] = { 0, 1, 2, 3, 4, -1, 5, 6 };
	__be16 *tab;
	u32 val;
	int i, ret;

	/* 0. Reset, interrupts off until a port is opened */
	zx_hw_reset(adapter);
	writel(~0, base + ZX_TM_INT_MASK);
	msleep(20);

	/* 1. Clock gating: enable the clocks of MAC ports 0-4 */
	writel(readl(base + ZX_GLB_CLK_CTRL) & ~ZX_GLB_CLK_CTRL_BITS,
	       base + ZX_GLB_CLK_CTRL);
	writel(GENMASK(ZX_NUM_PORTS - 1, 0), base + ZX_GLB_PORT_CLK_EN);
	zx_pp_init(adapter);

	/* 2. Free-buffer index tables */
	tab = adapter->tab0;
	for (i = 0; i < ZX_TAB0_ENTRIES; i++)
		tab[i] = cpu_to_be16(i);

	tab = adapter->tab1;
	for (i = 0; i < ZX_TAB1_ENTRIES; i++)
		tab[i] = cpu_to_be16(i);

	dma_wmb();
	ret = zx_tm_init(adapter);
	if (ret)
		return dev_err_probe(adapter->dev, ret, "TM table setup timed out\n");

	/* 3. Traffic Manager buffer pool */
	writel((u32)adapter->tab0_dma, base + ZX_TM_TAB0_ADDR);
	writel((u32)adapter->tab1_dma, base + ZX_TM_TAB1_ADDR);
	writel(0x08000800, base + ZX_TM_TAB_LIMIT);
	writel((u32)adapter->pkt_pool_dma, base + ZX_TM_BUF_START);
	writel((u32)adapter->pkt_pool_dma + ZX_BUF_POOL_SIZE, base + ZX_TM_BUF_END);
	writel((u32)adapter->rx_ring_dma, base + ZX_TM_BUF_F0);
	writel(16, base + ZX_TM_CONFIG_START);

	/* DMA and TM control values from the vendor pon_tm_dma_init() and
	 * the stock firmware, programmed before the BMU is configured.
	 */
	writel(1,      base + ZX_DMA_BASE + 0x28);
	writel(1,      base + ZX_DMA_BASE + 0x2c);
	writel(ZX_RX_IRQ_USECS * 50, base + ZX_DMA_RX_IRQ_TIMER);
	writel(ZX_RX_IRQ_FRAMES, base + ZX_DMA_RX_IRQ_FRAMES);
	writel(0x7f,   base + ZX_DMA_BASE + 0x04);
	writel(32,     base + ZX_DMA_BASE + 0x20);
	writel(32,     base + ZX_DMA_BASE + 0x24);
	/* The bootloader leaves 0x00131213 here, the stock firmware runs with
	 * 0x00131217.
	 */
	writel(0x00131217, base + ZX_DMA_BASE + 0x388);
	writel(readl(base + ZX_TM_CTRL) | ZX_TM_CTRL_BIT6, base + ZX_TM_CTRL);

	/* 4. Buffer Management Unit */
	writel(0,          base + ZX_BMU_CTRL);
	writel(0x0104c040, base + ZX_BMU_CFG1);
	writel(0x0104c040, base + ZX_BMU_CFG2);
	/* Pool size in units of 32 buffers.  With a 2048-buffer pool the BMU
	 * never fills its external free list (BPPE) after a cold NAND boot and
	 * runs on its ~64-entry internal cache only.  4096 works.
	 */
	writel(ZX_NUM_BUFFERS / 32 - 1, base + ZX_BMU_LIMIT1);
	writel(15,         base + ZX_BMU_LIMIT2);
	writel(ZX_NUM_BUFFERS << 16, base + ZX_BMU_THRES1);
	writel(0x2000000,  base + ZX_BMU_THRES2);
	writel(0,          base + ZX_BMU_BUF_ALLOC);
	writel(ZX_BMU_CTRL_EN, base + ZX_BMU_CTRL);

	writel(17, base + 0x3a00e0);

	/* 5. DMA descriptor rings */
	memset(adapter->tx_ring, 0, ZX_NUM_TX_DESC * ZX_DESC_SIZE);
	memset(adapter->rx_ring, 0, ZX_RX_QUEUES * ZX_RX_QUEUE_STRIDE);
	adapter->tx_cur_idx = 0;
	for (i = 0; i < ZX_RX_QUEUES; i++) {
		adapter->rx_cur_idx[i] = 0;
		adapter->rx_synced[i] = false;
	}
	adapter->tx_pending = 0;
	adapter->tx_complete_cnt = 0;
	adapter->tx_done_excess = 0;
	adapter->bmu_free_slots = 0;

	writel((u32)adapter->buf_pool_dma + ZX_AREA50_OFFSET, base + ZX_DMA_AREA50);
	writel((u32)adapter->tx_ring_dma, base + ZX_DMA_TX_RING);
	writel(0x00400040,             base + ZX_DMA_RING_CFG);
	readl(base + ZX_DMA_TX_DONE);

	dma_wmb();

	/* 6. Switch: enable forwarding */
	writel(ZX_SW_FWD_CTRL_ON, base + ZX_SW_FWD_CTRL);

	for (i = 0; i < 8; i++) {
		int idx = port_to_sw_idx[i];

		if (idx >= 0)
			writel(1, base + ZX_SPA_PORT_EN(idx));
	}

	/*
	 * Enable every class of packets the switch can hand to the CPU, up
	 * and down direction, as the stock firmware does.  With only a few
	 * classes enabled, IPv6 neighbour discovery never reached the CPU.
	 */
	writel(0xffffffff, base + ZX_SPA_UP_REG_PKT_EN);
	writel(0xffffffff, base + ZX_SPA_UP_REG_PKT_EN + 4);
	writel(0xffffffff, base + ZX_SPA_DN_REG_PKT_EN);
	writel(0xffffffff, base + ZX_SPA_DN_REG_PKT_EN + 4);

	/*
	 * The switch never forwards from port to port and hands the CPU only
	 * the frames a trap matches; the Linux bridge forwards between the
	 * port netdevs.  So trap the common ethertypes whatever their
	 * destination (IPv4, IPv6, ARP, VLAN tagged).  This also covers what
	 * the IPv4 protocol trap (TCP, UDP, ICMP) did, so clear that one.
	 */
	writel(ETH_P_IP << 16 | ETH_P_IPV6, base + ZX_SPA_TRAP_ETH_TYPE);
	writel(ETH_P_ARP << 16 | ETH_P_8021Q, base + ZX_SPA_TRAP_ETH_TYPE + 4);
	writel(0, base + ZX_SPA_TRAP_PROTO);

	/* Clear port filter bits for ports 0-4 */
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		val = readl(base + ZX_SW_PORT_FILTER(i));
		val &= ~ZX_SW_PORT_FILTER_BITS;
		writel(val, base + ZX_SW_PORT_FILTER(i));
	}

	/* 7. MAC ports 0-4 */
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		void __iomem *pb = base + ZX_MAC_PORT(i);

		writel(0x00bbe000, pb + ZX_MAC_CTRL);
		writel(0x80000001, pb + 0x08);
		writel(0x0000fffe, pb + 0x04);
		writel(0x00011200, pb + 0xe0);
		writel(50,         pb + 0xd00);
		writel(168,        pb + 0xd30);
		writel(0x00300002, pb + 0x70);
		writel(0x4000,     pb + 0xb4);
		writel(0x0010ff11, pb + 0xb00);

		zx_port_set_speed(adapter, i, SPEED_1000, DUPLEX_FULL);
		zx_port_enable(adapter, i, false);
	}

	return 0;
}

/*
 * No interrupt reports sent frames.  They are reclaimed on every transmit
 * and in the NAPI poll; while a full ring keeps the queues stopped, a timer
 * runs the poll until they can be woken again.  Called with tx_lock held.
 */
static void zx_tx_kick(struct zx_eth_adapter *adapter)
{
	if (!hrtimer_active(&adapter->tx_timer))
		hrtimer_start(&adapter->tx_timer, us_to_ktime(ZX_TX_POLL_US),
			      HRTIMER_MODE_REL);
}

/* Called with tx_lock held; also from zx_accel_set() while it drains the ring. */
void zx_tx_reclaim(struct zx_eth_adapter *adapter)
{
	u32 done = readl(adapter->base + ZX_DMA_TX_DONE) & 0xffff;
	u32 n = min(done, adapter->tx_pending);

	if (unlikely(done > adapter->tx_pending))
		adapter->tx_done_excess += done - adapter->tx_pending;
	adapter->tx_complete_cnt += n;
	adapter->tx_pending -= n;
	/*
	 * 加速模式切换期间不动队列：切换自己会 netif_tx_disable() 所有端口，
	 * 这里再把队列叫醒就等于开着门切换（设计 §1.4 S2/S6）。
	 */
	if (adapter->tx_pending < ZX_TX_QUEUE_LIMIT &&
	    !READ_ONCE(adapter->accel.quiesce)) {
		int p;

		for (p = 0; p < ZX_NUM_PORTS; p++)
			if (adapter->ports[p] && netif_queue_stopped(adapter->ports[p]))
				netif_wake_queue(adapter->ports[p]);
	}
}

/* ---- 零拷贝收发（zc）----------------------------------------------- */

/*
 * BMU 发给 CPU 的帧只能落在池子里：bp 就是"池首 + bp * 2048"，收发两个方向
 * 都只认这个编号，发送描述符里根本没有物理地址字段。所以常规路径必须搬两次
 * payload：收包时从槽位搬进 skb，发包时再从 skb 搬进另一个槽位。这颗 SoC 上
 * 一次 1400 字节的搬运要 5~15 µs（非缓存池实测：收 14.4 µs、发 5.2 µs），两次
 * 就吃掉了转发包 CPU 时间（29.0 µs）里的 19.6 µs。
 *
 * 零拷贝把这条链路砍成 0 次搬运：
 *   收：build_skb() 把槽位本身当 skb 的数据区（shinfo 落在槽位尾部 1728..2048，
 *       帧最长 1536+16，不会打架），skb_reserve(16) 后 data 正好是帧首。
 *   发：发现 skb->head 就是池槽位、data 没动过，就把同一个 bp 写进描述符。
 *       硬件发完自己把 bp 还回 BMU（bmu_alloc_fail 在 180 万个发送包后仍是 0，
 *       说明发送缓冲确实由硬件回收），所以这条路上我们不需要管 bp。
 *
 * 三个必须处理的例外：
 *  1. 协议栈丢包（防火墙、无路由、桥过滤）：bp 只能靠 skb->destructor 找回来。
 *  2. skb_orphan() 会提前调用 destructor（br_netfilter 的 setup_pre_routing、
 *     nft flowtable 的 xfrm 分支都会），那时 skb 还活着、槽位不能还。更麻烦的是
 *     skb_set_owner_r() 把帧交给 socket 时也是先 skb_orphan()、再换成
 *     sock_rfree：那个 skb 之后躺在 socket 队列里几百毫秒，期间没有任何回调。
 *     所以 destructor 必须在**数据**上再拿一份引用，等协议栈真正放掉数据。
 *
 *     这份引用绝对不能用 skb_get()（skb->users）：__kfree_skb() 根本不过 users，
 *     它直接 skb_release_all()+kfree_skbmem()。socket 收包走的正是这条路——
 *     tcp_recvmsg() 消化完一个 skb 调 sk_eat_skb()，而 sk_eat_skb() 就是
 *     __kfree_skb()：skb 连同 sk_buff 结构体一起被释放，我们那份 users 引用被
 *     无声吃掉，隔离环里只剩一个悬空指针，之后再 kfree_skb() 它就成了二次释放
 *     （实测现象：109 s 时 kmem_cache_free(skbuff_cache) 报对象已在空闲链上，
 *     panic 落在 zx_eth_poll。推导见 docs/zc-rx-lifetime-2026-10-31.md）。
 *
 *     改用 skb_shared_info->dataref：内核自己维护的"数据引用数"，所有释放路径
 *     （kfree_skb()/__kfree_skb()/kfree_skb_partial()/pskb_expand_head()）都经
 *     skb_release_data()→skb_data_unref()，一分不多一分不少。destructor 里
 *     atomic_inc(dataref) + skb->cloned=1（等价于克隆一份数据引用，但不真的分配
 *     sk_buff），数据就一直活到我们放手；NAPI 里看 dataref 掉到 1（只剩我们这份）
 *     就说明数据没人读了，这时替 head 还掉页引用、再把 bp 还给 BMU。所以隔离环
 *     里只存 bp，不存 skb 指针。
 *  3. 释放 head_frag=1 的 skb 会走 page_frag_free()，它对槽位所在的页做
 *     put_page()。池子来自 dma_alloc_coherent()，是 2048 个独立 order-0 页，
 *     put_page() 会把其中一页还给 buddy、整块池子出现空洞。所以池页引用计数被
 *     垫高（ZX_ZC_PARK），每次释放只做一次无害的减一，NAPI 里定期补回；reap 里
 *     我们替 head 还的那次页引用同理。
 *
 * 另外 skb 可能被 pskb_expand_head() 换成新的 head（例如要更多 headroom，或者
 * GRO 合并）。expand 会把 shinfo 连同 destructor_arg 一起抄到新 head，tag 还在，
 * 但数据已经不在槽位里；这时候槽位还有没有别的 skb 在引用，从这个 skb 上看不出
 * 来（克隆过的 expand 会把旧 head 留给克隆体）。所以这类槽位**直接扣着不还**并
 * 记数（zc_lost），超过 ZX_ZC_LOST_MAX 自动关掉 zc：宁可少一个缓冲，也不还一个
 * 可能还在被读的缓冲。
 * 这也是开 zc 时必须关掉 GRO 的原因，但**不是**因为 bp 会被漏掉：实测开着 GRO
 * 时 out 一直平、frag_list 成员的析构会跑（docs/ffe-zero-copy-2026-10-30.md §6.6）。
 * 真正的原因在发送侧：合并出的聚合体要拆段，而这份驱动的发送描述符里一个 bp 只能
 * 放一整帧、没有 SG/TSO，拆段的第 2..N 段只能 memcpy 出新 skb，零拷贝的收益被
 * 抵消一半以上 —— 实测每包软中断 15.2 → 28.7 µs、UDP 900M 丢包 0.95 % → 22 %。
 */
#define ZX_ZC_TAG_VAL	0x5ac00000u
#define ZX_ZC_TAG_MASK	0xffff0000u


/* SoC 上只有一个以太网控制器；destructor 里没有别的办法拿到 adapter */
static struct zx_eth_adapter *zx_zc_adapter;

static inline void zx_zc_tag_set(struct sk_buff *skb, u16 bp)
{
	skb_shinfo(skb)->destructor_arg =
		(void *)(unsigned long)(ZX_ZC_TAG_VAL | (u32)bp);
}

static inline bool zx_zc_tag_get(const struct sk_buff *skb, u16 *bp)
{
	unsigned long v = (unsigned long)skb_shinfo(skb)->destructor_arg;

	if ((v & ZX_ZC_TAG_MASK) != ZX_ZC_TAG_VAL)
		return false;
	*bp = (u16)(v & 0xffffu);
	return true;
}

static inline bool zx_zc_in_pool(struct zx_eth_adapter *adapter,
				 const void *head, u16 *bp)
{
	unsigned long off;

	if (!adapter->pkt_pool || !head)
		return false;
	off = (unsigned long)head - (unsigned long)adapter->pkt_pool;
	if (off >= ZX_BUF_POOL_SIZE || (off & (ZX_BUF_SIZE - 1)))
		return false;
	*bp = (u16)(off / ZX_BUF_SIZE);
	return true;
}

/*
 * 有多少个 bp 现在不在 BMU 空闲表里（在协议栈手里、在发送硬件里、在隔离环里）。
 * 本机收包时 skb 会长期留在 socket 队列里，这个数会一直涨——实测 900 Mbit/s 灌
 * 本机自己的 UDP 端口时它涨到 3132，池子被吃空，RX 再也拿不到缓冲，盒子直接
 * 从网络消失。所以收包侧留了一道闸：超过 ZX_ZC_OUT_MAX 就退回拷贝路径，
 * 保证转发/发送始终有缓冲可用。
 */
static inline u64 zx_zc_out(struct zx_eth_adapter *adapter)
{
	return adapter->zc_rx_ok - adapter->zc_tx_ok - adapter->zc_free_ok;
}

/* 池页引用计数垫高：让 page_frag_free() 的 put_page() 只减一、不真的还页 */
static void zx_zc_park_pages(struct zx_eth_adapter *adapter)
{
	u32 i;

	if (!adapter->pkt_pool)
		return;
	for (i = 0; i < ZX_BUF_POOL_SIZE; i += PAGE_SIZE) {
		struct page *page = virt_to_page(adapter->pkt_pool + i);

		if (page_ref_count(page) < ZX_ZC_PARK / 2)
			page_ref_add(page, ZX_ZC_PARK);
	}
}

/*
 * 池子销毁时用（只在设备拆除时走；驱动本身不能被 unbind）。先关 zc、把状态表
 * 和隔离环清空，再把垫高的页引用抹平：不这样做，free_pages_exact() 会踩到还被
 * page_frag_free() 记着引用的页。
 */
static void zx_zc_unpark_pages(struct zx_eth_adapter *adapter)
{
	u32 i;

	if (!adapter->pkt_pool)
		return;
	WRITE_ONCE(adapter->zc_rx, false);
	for (i = 0; i < ZX_NUM_BUFFERS; i++) {
		adapter->zc_state[i] = ZX_ZC_BS_FREE;
		adapter->zc_skb[i] = NULL;
	}
	for (i = 0; i < ZX_ZC_Q_LEN; i++)
		adapter->zc_q_bp[i] = 0xffff;
	adapter->zc_q_head = 0;
	adapter->zc_q_tail = 0;
	adapter->zc_q_n = 0;
	for (i = 0; i < ZX_BUF_POOL_SIZE; i += PAGE_SIZE)
		set_page_count(virt_to_page(adapter->pkt_pool + i), 1);
}

/*
 * 调试：每个 bp 记 8 条事件流水（type:aux），配合 zc_canary 定位"槽位被提前
 * 归还"的完整时序。type: 1=RX交付(aux=len>>6) 2=影子入环 3=reap归还(aux=refs)
 * 4=发送申请(aux=当时state) 5=zc发送 6=陈旧tag 7=追不到 9=还bp给BMU
 * (aux=state|0x10 在环里)。同一个 bp 出现两次 9 就是双还的直接证据。
 */
static void zx_zc_ev(struct zx_eth_adapter *adapter, u16 bp, u8 type, u8 aux)
{
	u32 *slot;
	u8 n;

	if (unlikely(!adapter->zc_ev || bp >= ZX_NUM_BUFFERS))
		return;
	n = adapter->zc_ev_n[bp]++ & 7;
	slot = &adapter->zc_ev[bp][n];
	*slot = (u32)type << 24 | (u32)aux << 16 | (jiffies & 0xffff);
}

/*
 * 槽位尾部的 skb_shared_info。build_skb(data, 2048) 把 end 设成
 * SKB_WITH_OVERHEAD(2048)，shinfo 就落在槽位 + 1728，和 skb_shinfo(skb) 一致。
 */
static inline struct skb_shared_info *zx_zc_shinfo(struct zx_eth_adapter *adapter,
						   u16 bp)
{
	return (struct skb_shared_info *)(adapter->pkt_pool +
		(u32)bp * ZX_BUF_SIZE + SKB_WITH_OVERHEAD(ZX_BUF_SIZE));
}

/* 数据的引用数；等于 1 表示只剩我们这份影子引用 */
static inline u32 zx_zc_refs(struct skb_shared_info *sp)
{
	return atomic_read(&sp->dataref) & SKB_DATAREF_MASK;
}

/*
 * 出异常时把某个 bp 的完整状态 + 最近 8 条事件打进 dmesg。事件编码
 * type<<24 | aux<<16 | (jiffies & 0xffff)，type 见 zx_zc_ev()。
 */
static void zx_zc_ev_dump(struct zx_eth_adapter *adapter, u16 bp,
			  const char *what)
{
	struct skb_shared_info *sp;

	if (bp >= ZX_NUM_BUFFERS || !adapter->zc_ev)
		return;
	sp = zx_zc_shinfo(adapter, bp);
	dev_warn(adapter->dev,
		 "zc %s: bp=%u state=%u refs=%u tag=%08x skb=%p destructor=%pS q_n=%u inring=%d ev[%u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 what, bp, adapter->zc_state[bp], zx_zc_refs(sp),
		 (u32)(unsigned long)sp->destructor_arg,
		 adapter->zc_skb[bp],
		 adapter->zc_skb[bp] ? adapter->zc_skb[bp]->destructor : NULL,
		 adapter->zc_q_n,
		 adapter->zc_inring ? test_bit(bp, adapter->zc_inring) : -1,
		 adapter->zc_ev_n[bp],
		 adapter->zc_ev[bp][0], adapter->zc_ev[bp][1],
		 adapter->zc_ev[bp][2], adapter->zc_ev[bp][3],
		 adapter->zc_ev[bp][4], adapter->zc_ev[bp][5],
		 adapter->zc_ev[bp][6], adapter->zc_ev[bp][7]);
}

/*
 * 隔离环：存“数据还活着、但已经跟不到 skb”的槽位号。环长等于池子容量，
 * 一个 bp 在一个世代里最多进环一次，所以装不满。
 */
static bool zx_zc_q_push(struct zx_eth_adapter *adapter, u16 bp)
{
	u32 next = (adapter->zc_q_tail + 1) % ZX_ZC_Q_LEN;

	if (next == adapter->zc_q_head) {
		/* head 上的洞先剔掉再试一次（发送路径摘项会留下洞） */
		while (adapter->zc_q_head != adapter->zc_q_tail &&
		       adapter->zc_q_bp[adapter->zc_q_head] == 0xffff)
			adapter->zc_q_head = (adapter->zc_q_head + 1) %
					     ZX_ZC_Q_LEN;
		if (next == adapter->zc_q_head) {
			adapter->zc_q_ovf++;
			return false;
		}
	}
	adapter->zc_q_bp[adapter->zc_q_tail] = bp;
	adapter->zc_q_tail = next;
	adapter->zc_q_n++;
	if (adapter->zc_inring && test_and_set_bit(bp, adapter->zc_inring)) {
		adapter->zc_dup_push++;
		if (net_ratelimit())
			zx_zc_ev_dump(adapter, bp, "环里已有一份同一个 bp");
	}
	zx_zc_ev(adapter, bp, 2, 0);
	return true;
}

/* 摘掉某个 bp 的环项（发送路径接管槽位时用）。返回是否真的摘到 */
static bool zx_zc_q_remove(struct zx_eth_adapter *adapter, u16 bp)
{
	u32 i;

	for (i = adapter->zc_q_head; i != adapter->zc_q_tail;
	     i = (i + 1) % ZX_ZC_Q_LEN) {
		if (adapter->zc_q_bp[i] == bp) {
			adapter->zc_q_bp[i] = 0xffff;
			adapter->zc_q_n--;
			if (adapter->zc_inring)
				clear_bit(bp, adapter->zc_inring);
			return true;
		}
	}
	return false;
}

/*
 * 摘掉 head 上的环项（含发送路径留下的洞）。只有 reap 用，调用者持 zc_lock。
 */
static void zx_zc_q_drop_head(struct zx_eth_adapter *adapter, u32 idx)
{
	if (adapter->zc_q_bp[idx] != 0xffff)
		adapter->zc_q_n--;
	adapter->zc_q_bp[idx] = 0xffff;
	adapter->zc_q_head = (idx + 1) % ZX_ZC_Q_LEN;
}

/*
 * 有一个槽位再也追不回来了（扣住不还）。累计到 ZX_ZC_LOST_MAX 就自动关掉
 * zc：宁可退化到拷贝路径，也不让池子被一滴滴吃空。所有"只能扣着"的分支
 * 都走这里，保证 leak 只有一个计数器、一处自动关闭。
 */
static void zx_zc_leak(struct zx_eth_adapter *adapter)
{
	adapter->zc_lost++;
	if (adapter->zc_lost != ZX_ZC_LOST_MAX || !READ_ONCE(adapter->zc_rx))
		return;
	WRITE_ONCE(adapter->zc_rx, false);
	dev_warn(adapter->dev,
		 "zero-copy RX disabled: %llu buffers leaked\n",
		 adapter->zc_lost);
}

static void zx_zc_reap(struct zx_eth_adapter *adapter)
{
	u32 budget = 256;

	spin_lock_bh(&adapter->zc_lock);
	while (budget-- && adapter->zc_q_head != adapter->zc_q_tail) {
		u32 idx = adapter->zc_q_head;
		u16 bp = adapter->zc_q_bp[idx];
		struct skb_shared_info *sp;
		u8 *slot;
		int i;

		if (bp == 0xffff) {	/* 发送路径摘走留下的洞 */
			zx_zc_q_drop_head(adapter, idx);
			continue;
		}
		/*
		 * dataref 掉到 1 = 只剩我们这份影子引用，协议栈（包括
		 * __kfree_skb() 那条路）不会再来读这片数据了。环是先进先出的，
		 * 遇上还活着的就停下 —— 而且**原位不动**：不能在这里摘掉环项或者
		 * 清位图，否则下一次进来就会把自己这半截动作当成"重复环项"。
		 */
		sp = zx_zc_shinfo(adapter, bp);
		if (zx_zc_refs(sp) != 1)
			break;
		/*
		 * 两道闸，保证一个 bp 只被"还"一次：
		 *  - 同一个 bp 在环里出现第二份：谁先摘走谁负责归还，这里只丢项；
		 *  - 环项对应的 bp 状态已经不是 SHADOW：槽位已经被别人接管或归还，
		 *    这一项是陈旧的，碰它就会把别人正在读的槽位还给 BMU。
		 * 两种都只丢环项（槽位留在 BMU 外面 = 泄漏一个缓冲），宁漏不双还。
		 */
		if (adapter->zc_inring &&
		    test_and_clear_bit(bp, adapter->zc_inring) == 0) {
			adapter->zc_dup_reap++;
			zx_zc_leak(adapter);
			if (net_ratelimit())
				zx_zc_ev_dump(adapter, bp,
					      "环项重复，丢项不归还");
			zx_zc_q_drop_head(adapter, idx);
			continue;
		}
		if (unlikely(adapter->zc_state[bp] != ZX_ZC_BS_SHADOW)) {
			adapter->zc_badstate++;
			zx_zc_leak(adapter);
			if (net_ratelimit())
				zx_zc_ev_dump(adapter, bp,
					      "环项陈旧，丢项不归还");
			zx_zc_q_drop_head(adapter, idx);
			continue;
		}

		slot = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE;
		/* 数据里如果挂了 frag/frag_list，替协议栈放掉（正常不会出现） */
		if (unlikely(sp->nr_frags || sp->frag_list)) {
			for (i = 0; i < sp->nr_frags; i++)
				put_page(skb_frag_page(&sp->frags[i]));
			if (sp->frag_list)
				kfree_skb_list(sp->frag_list);
			sp->nr_frags = 0;
			sp->frag_list = NULL;
			adapter->zc_odd++;
		}
		/* 调试：槽位在我们手里这段时间有没有被写过 */
		if (unlikely(adapter->zc_snap && adapter->zc_snap_len[bp])) {
			u16 n = adapter->zc_snap_len[bp];

			adapter->zc_snap_len[bp] = 0;
			if (memcmp(adapter->zc_snap + (u32)bp * ZX_ZC_CANARY,
				   slot + 16, n)) {
				adapter->zc_corrupt++;
				if (net_ratelimit())
					zx_zc_ev_dump(adapter, bp,
						      "canary 被改写");
			}
		}
		zx_zc_ev(adapter, bp, 3, (u8)zx_zc_refs(sp));
		adapter->zc_state[bp] = ZX_ZC_BS_FREE;
		adapter->zc_skb[bp] = NULL;
		zx_zc_q_drop_head(adapter, idx);
		/* 替 head 还掉那次 page_frag_free()，再把 bp 还给 BMU */
		put_page(virt_to_head_page(slot));
		if (zx_bmu_free_bp(adapter, bp) == 0)
			adapter->zc_free_ok++;
	}
	spin_unlock_bh(&adapter->zc_lock);
}

/*
 * skb 走到生命尽头（或被人 orphan）时唯一的信号。做法见上面第 2 条例外：
 * 在 skb_shared_info->dataref 上拿一份影子引用，把槽位挂进隔离环，等 reap
 * 看到 dataref 掉到 1 再还 bp。这里不做任何分配，也不释放 skb。
 */
static void zx_zc_destructor(struct sk_buff *skb)
{
	struct zx_eth_adapter *adapter = READ_ONCE(zx_zc_adapter);
	struct skb_shared_info *sp;
	u16 bp, poolbp;

	if (!adapter)
		return;
	if (!zx_zc_tag_get(skb, &bp) || bp >= ZX_NUM_BUFFERS) {
		adapter->zc_notag++;	/* 调试：destructor 跑了但 tag 没了 */
		return;
	}

	if (unlikely(!zx_zc_in_pool(adapter, skb->head, &poolbp) ||
		     poolbp != bp)) {
		/*
		 * 数据被 pskb_expand_head() 搬出了槽位：槽位还在 BMU 外面，
		 * 但已经没有任何可靠的引用能告诉我们它什么时候没人读（克隆
		 * 过的 expand 把旧 head 留给了克隆体）。扣着不还，记数；多到
		 * 一定量就把 zc 关掉，让池子不再继续漏水。
		 */
		zx_zc_leak(adapter);
		return;
	}
	if (unlikely(skb->nohdr)) {
		/* nohdr 时 skb_data_unref() 的减数不是 1，影子引用会算错 */
		zx_zc_leak(adapter);
		return;
	}

	spin_lock_bh(&adapter->zc_lock);
	if (likely(adapter->zc_state[bp] == ZX_ZC_BS_SKB &&
		   adapter->zc_skb[bp] == skb)) {
		sp = zx_zc_shinfo(adapter, bp);
		atomic_inc(&sp->dataref);
		/*
		 * 等价于 skb_clone() 做的事（让 skb_data_unref() 认这份引用），
		 * 只是我们不真的分配 sk_buff：影子引用没有结构体，只由环和
		 * reap 记着。
		 */
		skb->cloned = 1;
		adapter->zc_state[bp] = ZX_ZC_BS_SHADOW;
		if (likely(zx_zc_q_push(adapter, bp))) {
			adapter->zc_shadow++;
			spin_unlock_bh(&adapter->zc_lock);
			return;
		}
		/* 环满：撤掉刚拿的引用，槽位永久扣住（正常到不了） */
		atomic_dec(&sp->dataref);
		adapter->zc_state[bp] = ZX_ZC_BS_LOST;
		zx_zc_leak(adapter);
	} else {
		/* 陈旧 tag：这个 bp 已经换了主人，什么都不做 */
		adapter->zc_stale++;
	}
	spin_unlock_bh(&adapter->zc_lock);
}

/*
 * 发送侧：能从槽位直接发就返回 true 并给出 bp。
 * 判断"数据只有这一个 skb 在用"靠 dataref：state==SKB 时只有 skb 自带的那份
 * （=1），state==SHADOW 时还有我们那份（=2）。多于这个数说明还有克隆体共享
 * 这份数据，不能把槽位交给硬件（硬件发完会把 bp 还给 BMU，克隆体还在读）。
 * 退出时不碰 bp：环里那份引用会照常把槽位收回来。
 */
static bool zx_zc_tx_take(struct zx_eth_adapter *adapter, struct sk_buff *skb,
			  u16 *bpp)
{
	struct skb_shared_info *sp;
	u16 bp;
	u8 *slot;
	u8 st;
	u32 mine;

	if (!adapter->zc_rx || !zx_zc_tag_get(skb, &bp) || bp >= ZX_NUM_BUFFERS)
		return false;
	slot = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE;
	if (!skb->head_frag || skb->head != slot || skb->data != slot + 16 ||
	    skb_headlen(skb) != skb->len || skb_shared(skb)) {
		adapter->zc_fallback++;
		return false;
	}

	spin_lock_bh(&adapter->zc_lock);
	st = adapter->zc_state[bp];
	if (adapter->zc_skb[bp] != skb ||
	    (st != ZX_ZC_BS_SKB && st != ZX_ZC_BS_SHADOW)) {
		adapter->zc_stale++;
		spin_unlock_bh(&adapter->zc_lock);
		return false;
	}
	mine = st == ZX_ZC_BS_SHADOW ? 1 : 0;
	sp = zx_zc_shinfo(adapter, bp);
	if (zx_zc_refs(sp) != 1 + mine) {
		adapter->zc_fallback++;
		spin_unlock_bh(&adapter->zc_lock);
		return false;
	}
	if (mine) {
		/* 槽位交给硬件以后，环里那份影子引用就没用了 */
		if (!zx_zc_q_remove(adapter, bp)) {
			/* 状态说 SHADOW 但环里没有：只能把槽位扣住 */
			adapter->zc_state[bp] = ZX_ZC_BS_LOST;
			zx_zc_leak(adapter);
			spin_unlock_bh(&adapter->zc_lock);
			return false;
		}
		atomic_dec(&sp->dataref);
	}
	adapter->zc_state[bp] = ZX_ZC_BS_TX;
	spin_unlock_bh(&adapter->zc_lock);

	/* 这个 skb 之后被释放时不许再碰槽位（bp 已经归硬件） */
	skb->destructor = NULL;
	zx_zc_tag_set(skb, 0xffff);
	adapter->zc_tx_ok++;
	*bpp = bp;
	return true;
}

/*
 * GRO 只能这样改。register_netdevice() 在 probe 之后才给每张网卡强加
 * NETIF_F_SOFT_FEATURES（GSO|GRO|GRO_FRAGLIST）并按它重算 wanted_features，
 * 所以 probe 里清位会被注册流程盖回去（实测开机后 features 仍是 on）。真正
 * 生效的时机是 ndo_open 和运行态开关，两处都写 features + wanted_features，
 * 这样之后任何 netdev_update_features() 都不会把位算回来。
 * 调用者必须持有 rtnl（ndo_open 和 zx_zc_set 都满足）。
 */
static void zx_gro_set(struct net_device *nd, bool on)
{
	if (on) {
		nd->features |= NETIF_F_GRO;
		nd->wanted_features |= NETIF_F_GRO;
	} else {
		nd->features &= ~NETIF_F_GRO;
		nd->wanted_features &= ~NETIF_F_GRO;
	}
}

/* 打开/关闭 zc，同时按需要开关 GRO（见上面第 3 条例外）
 *
 * GRO 位只由 zx_gro_set()（即 ndo_open 和这里）写，userspace 别用 ethtool
 * 去改：把 hw_features 里的位清掉之后，`ethtool -K gro off` 走的是
 * wanted_features 那条路，实测反而把 features 位留成 on（"on [fixed]"）。
 *
 * 非 static：加速模式切换（zx279128s-accel.c 的 S4/S6）也要用它进/出 zc。
 * 里面会 rtnl_lock()，所以调用它的路径**不得先持 rtnl**。
 */
void zx_zc_set(struct zx_eth_adapter *adapter, bool on)
{
	int i;

	rtnl_lock();
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *nd = adapter->ports[i];

		if (!nd)
			continue;
		zx_gro_set(nd, !on);
	}
	rtnl_unlock();
	WRITE_ONCE(adapter->zc_rx, on);
	if (on) {
		WRITE_ONCE(zx_zc_adapter, adapter);
		zx_zc_park_pages(adapter);
	} else {
		adapter->zc_off_cnt++;
	}
}

/* ---- TX path ------------------------------------------------------- */

/*
 * 把一个已经躺在池槽位 bp 里的帧提交给发送硬件：写描述符 → 每包一次
 * TX_START → 推进/占用发送环 → 到限流就停所有端口队列。
 *
 * 栈路径（zx_eth_xmit()）与快路（zx279128s-accel.c 的 zx_ffe_rx()）共用这
 * 一段，保证"协议栈发出去的帧"和"驱动内快路转发的帧"在硬件看来逐字节一样：
 * 同一套 desc[0..2] 常量、同一次 dma_wmb()、同一次 writel(ZX_DMA_TX_START)、
 * 同一套 tx_cur_idx/tx_pending 与限流动作。
 *
 * **调用方必须持 adapter->tx_lock**（本函数不加锁、也不检查持有者）：环索引、
 * tx_pending 与描述符的写入必须与 zx_tx_reclaim() 的唤醒、别的端口的提交互斥。
 * 调用方还必须拥有 bp：描述符一写，硬件就会去读这个槽位，之后再"还回去/改写
 * 内容"都是二次所有权。
 *
 * 返回 false 表示环满（tx_pending >= ZX_TX_QUEUE_LIMIT）：这时**一个字节都没
 * 写**，bp 与帧内容保持调用前的样子，调用方可以安全地把这一帧交给协议栈。
 * tx_lock 在手里时这个判断不会被别人改变——tx_pending 只在 tx_lock 下增长。
 */
bool zx_tx_submit_bp(struct zx_eth_adapter *adapter, int eport, u16 bp,
		     unsigned len)
{
	void __iomem *base = adapter->base;
	__le32 *desc;
	u32 tx_idx;

	if (unlikely(adapter->tx_pending >= ZX_TX_QUEUE_LIMIT))
		return false;

	tx_idx = adapter->tx_cur_idx % ZX_NUM_TX_DESC;
	desc = (__le32 *)adapter->tx_ring + tx_idx * (ZX_DESC_SIZE / sizeof(*desc));

	/*
	 * Directed egress to the port's TM queue.  The switch does not
	 * forward between ports: the Linux bridge does, one netdev per port.
	 */
	desc[0] = cpu_to_le32(0x80 | (ZX_PORT_TX_QUEUE(eport) << 20));
	desc[3] = cpu_to_le32(0x3 | ((len & 0x3fff) << 2));
	desc[1] = cpu_to_le32(0x10000);
	desc[2] = cpu_to_le32(0x21000000 | ((len & 0x3fff) << 9));

	((u8 *)desc)[7] = (u8)((bp & 0x7f) << 1);
	((u8 *)desc)[8] = (u8)(bp >> 7);

	dma_wmb();
	writel(1, base + ZX_DMA_TX_START);

	adapter->tx_cur_idx++;
	adapter->tx_pending++;
	/* 快路与栈共用一个环，dma_state 的 qlen_peak 记整环占用峰值 */
	if (adapter->tx_pending > READ_ONCE(adapter->accel.cnt[0].qlen_peak))
		WRITE_ONCE(adapter->accel.cnt[0].qlen_peak, adapter->tx_pending);
	if (adapter->tx_pending >= ZX_TX_QUEUE_LIMIT) {
		int p;

		for (p = 0; p < ZX_NUM_PORTS; p++)
			if (adapter->ports[p])
				netif_stop_queue(adapter->ports[p]);
		zx_tx_kick(adapter);
	}
	return true;
}

/*
 * 池子的映射属性（缓存 / 非缓存）由 pool_cacheable 决定，两条路径的实测代价见
 * docs/ffe-cacheable-pool-2026-10-30.md。预取（pld/pldw）在这颗 SoC 上是负收益：实测 1400 B
 * 帧的接收拷贝 6.67 -> 9.85 us、发送拷贝+同步 15.09 -> 18.57 us，已回退。
 */
static netdev_tx_t zx_eth_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *adapter = priv->adapter;
	unsigned int len = skb->len;
	u8 *buf_ptr;
	int bp;
	u16 zcbp = 0;
	u64 t_entry, t_reclaim, t_alloc, t_copy, t_start;

	t_entry = ZX_TS();

	if (len > 1536) {
		dev_kfree_skb_any(skb);
		dev_dstats_tx_dropped(netdev);
		return NETDEV_TX_OK;
	}

	spin_lock_bh(&adapter->tx_lock);
	zx_tx_reclaim(adapter);
	t_reclaim = ZX_TS();
	/*
	 * The ports share the ring.  The queues are stopped as soon as it is
	 * full, but another port's transmit may already be waiting for the
	 * lock: give its frame back to the stack.
	 */
	if (adapter->tx_pending >= ZX_TX_QUEUE_LIMIT) {
		netif_stop_queue(netdev);
		zx_tx_kick(adapter);
		spin_unlock_bh(&adapter->tx_lock);
		return NETDEV_TX_BUSY;
	}
	if (skb_put_padto(skb, ETH_ZLEN)) {
		spin_unlock_bh(&adapter->tx_lock);
		dev_dstats_tx_dropped(netdev);
		return NETDEV_TX_OK;
	}
	len = skb->len;
	if (zx_zc_tx_take(adapter, skb, &zcbp)) {
		/*
		 * 零拷贝：帧本来就在这个槽位里，既不申请缓冲也不搬家，
		 * 硬件发完自己把 bp 还回 BMU。
		 */
		bp = zcbp;
		t_alloc = ZX_TS();
	} else {
		bp = zx_bmu_alloc_bp(adapter);
		if (bp < 0 || bp >= ZX_NUM_BUFFERS) {
			spin_unlock_bh(&adapter->tx_lock);
			dev_dstats_tx_dropped(netdev);
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}

		t_alloc = ZX_TS();
		if (unlikely(adapter->zc_state[bp] == ZX_ZC_BS_SHADOW))
			adapter->zc_tx_dup++;	/* 调试：BMU 把还挂着影子的 bp 给了发送 */
		zx_zc_ev(adapter, (u16)bp, 4, adapter->zc_state[bp]);
		buf_ptr = (u8 *)adapter->pkt_pool +
			  ((u32)bp * ZX_BUF_SIZE) + 16;
		if (skb_headlen(skb) >= len) {
			memcpy(buf_ptr, skb->data, len);
		} else if (skb_copy_bits(skb, 0, buf_ptr, len)) {
			zx_bmu_free_bp(adapter, bp);
			spin_unlock_bh(&adapter->tx_lock);
			dev_dstats_tx_dropped(netdev);
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
	}
	/*
	 * 池是可缓存映射时，帧必须在 BMU 读之前推到一致性点。零拷贝这一支同样
	 * 需要：帧是硬件 DMA 进来的，读之前失效过，但协议栈（转发时的 L2 重写、
	 * NAT66、flowtable 的改头）可能已经把它改脏在 cache 里，不 clean 就等于
	 * 把旧内容发出去。默认映射是非缓存，所以这段平时不执行。
	 */
	if (adapter->pkt_pool_cacheable)
		dma_sync_single_for_device(adapter->dev,
					   adapter->pkt_pool_dma +
					   (u32)bp * ZX_BUF_SIZE,
					   16 + len, DMA_TO_DEVICE);

	t_copy = ZX_TS();
	/*
	 * 描述符 + TX_START + 环推进 + 限流都在 zx_tx_submit_bp() 里，快路用的是
	 * 同一段代码。上面在同一次 tx_lock 会话里刚查过容量，tx_pending 在那之后
	 * 只可能被本函数增加，所以这里不可能返回 false；真出现就丢帧（bp 归还，
	 * zc 的那份 bp 由 skb 的 destructor 收回去），绝不发半成品。
	 */
	if (unlikely(!zx_tx_submit_bp(adapter, priv->port, (u16)bp, len))) {
		if (!zcbp)
			zx_bmu_free_bp(adapter, bp);
		spin_unlock_bh(&adapter->tx_lock);
		dev_dstats_tx_dropped(netdev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	t_start = ZX_TS();

	if (unlikely(timing)) {
		adapter->tx_stage_ns[0] += t_reclaim - t_entry;
		adapter->tx_stage_ns[1] += t_alloc - t_reclaim;
		adapter->tx_stage_ns[2] += t_copy - t_alloc;
		adapter->tx_stage_ns[3] += t_start - t_copy;
		adapter->tx_stage_n++;
	}

	spin_unlock_bh(&adapter->tx_lock);
	dev_dstats_tx_add(netdev, len);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- RX path ------------------------------------------------------- */

static bool zx_is_own_addr(struct zx_eth_adapter *adapter, const u8 *addr)
{
	int i;

	for (i = 0; i < ZX_NUM_PORTS; i++)
		if (adapter->ports[i] &&
		    ether_addr_equal(adapter->ports[i]->dev_addr, addr))
			return true;
	return false;
}

/*
 * P10.2 F6：alias 丢弃点的环形取证快照（见 struct zx_alias_snap 的注释）。
 *
 * 只记录、不改变 alias 判据的任何行为：调用点在 rx_alias_cnt++ 之后、
 * zx_rx_consume() 之前，帧还在池槽位里。调用前已经做过
 * dma_sync_single_for_cpu()，所以 head 直接 memcpy，**不要再 sync**。
 *
 * 写侧只有 RX NAPI 一处，读侧（debugfs alias_dump）不加锁：先填满槽位再
 * WRITE_ONCE 发布索引，读到半条也只是取证数据不完整，重读一次即可。
 */
static void zx_alias_snapshot(struct zx_eth_adapter *adapter,
			      const __le32 *desc, u16 len, int port,
			      const u8 *frame)
{
	u32 slot = adapter->alias_snap_idx & (ZX_ALIAS_SNAP_N - 1);
	struct zx_alias_snap *s = &adapter->alias_snap[slot];
	u32 head_len = min_t(u32, len, (u32)sizeof(s->head));
	int i;

	for (i = 0; i < 4; i++)
		s->desc[i] = le32_to_cpu(desc[i]);
	s->len = len;
	s->port = (port >= 0 && port <= 0xff) ? (u8)port : 0xff;
	s->rsvd = 0;
	/*
	 * 只拷描述符声明的长度：len 最小可以到 14（守卫只挡 <14），此时
	 * 拷贝 64 B 会把"本帧之后"的槽内残留（上一条帧的尾巴/未初始化垃圾）
	 * 打进 debugfs，取证会误导（不崩、也不越池）。余下的补 0，读侧能
	 * 一眼看出"这一帧只有这么长"。
	 */
	memcpy(s->head, frame, head_len);
	memset(s->head + head_len, 0, sizeof(s->head) - head_len);
	WRITE_ONCE(adapter->alias_snap_cnt, adapter->alias_snap_cnt + 1);
	/* 槽位填完才发布下一个索引（读侧 best-effort） */
	smp_wmb();
	WRITE_ONCE(adapter->alias_snap_idx,
		   (slot + 1) & (ZX_ALIAS_SNAP_N - 1));
}

static int zx_eth_rx(struct zx_eth_adapter *adapter, int budget)
{
	int work_done = 0, q;

	if (unlikely(adapter->zc_q_head != READ_ONCE(adapter->zc_q_tail)))
		zx_zc_reap(adapter);
	if (unlikely(adapter->zc_rx && time_after(jiffies, adapter->zc_park_at))) {
		adapter->zc_park_at = jiffies + HZ;
		zx_zc_park_pages(adapter);
	}

	for (q = ZX_RX_QUEUES - 1; q >= 0 && work_done < budget; q--) {
		u32 avail = readl(adapter->base + ZX_DMA_RX_QUEUE_CNT(q)) & 0xffff;
		u32 rel[2] = { 0, 0 };

		while (avail && work_done < budget) {
			u32 idx = adapter->rx_cur_idx[q] % ZX_NUM_RX_DESC;
			__le32 *desc = adapter->rx_ring + q * ZX_RX_QUEUE_STRIDE +
				       idx * ZX_DESC_SIZE;
			u16 bp, len;
			int port;
			bool zc;
			struct sk_buff *skb;
			struct net_device *dest_dev;
			u64 t0, t1, t2, t3, t4, t5;

			t0 = ZX_TS();
			dma_rmb();
			bp = (((u8 *)desc)[7] >> 1) | ((u16)((u8 *)desc)[8] << 7);
			len = (le32_to_cpu(desc[3]) >> 2) & 0x3fff;
			port = (((u8 *)desc)[6] >> 3) ? ((((u8 *)desc)[6] >> 3) - 1) : 0;
			t1 = ZX_TS();

			if (bp >= ZX_NUM_BUFFERS || len < ETH_HLEN || len > 1536) {
				u32 scan, idx2, found = 0;

				if (adapter->rx_synced[q]) {
					/*
					 * The hardware counted this slot but it
					 * holds no frame: it can leave a slot
					 * empty (e.g. when it had no buffer).
					 * Consume it as the vendor driver does.
					 * Waiting for it would leave our index
					 * behind the hardware's for good.
					 */
					adapter->rx_bad_cnt++;
					if (net_ratelimit())
						dev_warn(adapter->dev,
							 "rxq%d: skipping slot %u (%08x %08x %08x %08x), %u pending\n",
							 q, idx, le32_to_cpu(desc[0]),
							 le32_to_cpu(desc[1]),
							 le32_to_cpu(desc[2]),
							 le32_to_cpu(desc[3]), avail);
					zx_rx_consume(desc, rel);
					adapter->rx_cur_idx[q]++;
					work_done++;
					avail--;
					if (bp && bp < ZX_NUM_BUFFERS &&
					    zx_bmu_free_bp(adapter, bp))
						adapter->bmu_free_fail++;
					continue;
				}

				for (scan = 1; scan < ZX_NUM_RX_DESC; scan++) {
					__le32 *d2;
					u16 bp2, len2;

					idx2 = (idx + scan) % ZX_NUM_RX_DESC;
					d2 = adapter->rx_ring + q * ZX_RX_QUEUE_STRIDE +
					     idx2 * ZX_DESC_SIZE;
					dma_rmb();
					bp2 = (((u8 *)d2)[7] >> 1) |
					      ((u16)((u8 *)d2)[8] << 7);
					len2 = (le32_to_cpu(d2[3]) >> 2) & 0x3fff;
					if (bp2 < ZX_NUM_BUFFERS &&
					    len2 >= ETH_HLEN && len2 <= 1536) {
						found = 1;
						break;
					}
				}
				if (!found)
					break;
				adapter->rx_cur_idx[q] = idx2;
				adapter->rx_resync_cnt++;
				continue;
			}
			adapter->rx_synced[q] = true;

			if (bp == 0) {
				adapter->rx_nobuf_cnt++;
				zx_rx_consume(desc, rel);
				adapter->rx_cur_idx[q]++;
				work_done++;
				avail--;
				continue;
			}

			/*
			 * The frame sits in cacheable memory that the BMU wrote
			 * by DMA: drop the stale cache lines before the CPU
			 * touches it.  The 16 B header and the frame live in the
			 * same 32 B aligned slot, so one invalidate covers both
			 * (and never reaches into the next slot).
			 */
			if (adapter->pkt_pool_cacheable)
				dma_sync_single_for_cpu(adapter->dev,
							adapter->pkt_pool_dma +
							(u32)bp * ZX_BUF_SIZE,
							16 + len,
							DMA_FROM_DEVICE);
			t2 = ZX_TS();

			/* Aliasing detector: frames from one of our own addresses */
			if (len >= ETH_HLEN) {
				u8 *d = adapter->pkt_pool + bp * ZX_BUF_SIZE + 16;

				if (zx_is_own_addr(adapter, d + ETH_ALEN)) {
					adapter->rx_alias_cnt++;
					/*
					 * P10.2 F6：只留证，不改行为。
					 * 此时缓存已 invalidate、帧还在
					 * 池槽位里，head 直接从 d 拷。
					 */
					zx_alias_snapshot(adapter, desc, len,
							  port, d);
					zx_rx_consume(desc, rel);
					adapter->rx_cur_idx[q]++;
					work_done++;
					avail--;
					/*
					 * A frame of our own that the switch
					 * sent back still has an RX buffer of
					 * its own: return it, or every such
					 * frame leaks one BMU buffer.
					 */
					if (zx_bmu_free_bp(adapter, bp))
						adapter->bmu_free_fail++;
					continue;
				}
			}

			/* Deliver to the netdev of the ingress port */
			dest_dev = port < ZX_NUM_PORTS ? adapter->ports[port] : NULL;

			/*
			 * 加速分发点（唯一，设计 §1.2）：描述符已解析、池已
			 * invalidate、自有源 MAC 已排除，但还没有建任何 skb。
			 * P2 起 zx_ffe_rx() 会真转发：返回非 STACK 就说明这一帧
			 * 已经交给发送硬件（或已在快路里确定丢了），后面只做 RX
			 * 收尾，**不许**再建 skb、不许把 bp 还给 BMU。
			 *
			 * 协议族在 quiesce 判断之前算好：零拷贝那一段也要用它
			 * （zc 按族生效，见下）。两者都在 goto 之前初始化，所以
			 * 跳过去也拿得到值。
			 */
			__be16 h_proto = *(__be16 *)((u8 *)adapter->pkt_pool +
				(u32)bp * ZX_BUF_SIZE + 16 + 2 * ETH_ALEN);
			int fam = zx_eth_family(h_proto);
			int ffe = ZX_FFE_RX_STACK;

			if (unlikely(READ_ONCE(adapter->accel.quiesce)))
				goto accel_slow;
			/*
			 * 只在对应族是 ffe 时才进判据链（其它模式零开销）。
			 * 非 IPv4/IPv6 的帧（fam<0：ARP/PPPoE/VLAN 外层…）没有
			 * 协议族，只要有一族在 ffe 里也让它们过一遍判据链并记
			 * miss——"非 IP 一律不命中"这件事也要能被计数自证。
			 */
			if (fam >= 0) {
				if (zx_accel_cur_mode(&adapter->accel, fam) ==
				    ZX_ACC_FFE)
					ffe = zx_ffe_rx(adapter, bp, len,
							port, fam);
			} else {
				int nfam = zx_accel_cur_mode(&adapter->accel, 0) ==
					   ZX_ACC_FFE ? 0 : 1;

				if (zx_accel_cur_mode(&adapter->accel, nfam) ==
				    ZX_ACC_FFE)
					ffe = zx_ffe_rx(adapter, bp, len,
							port, nfam);
			}
			if (ffe != ZX_FFE_RX_STACK)
				goto ffe_done;
accel_slow:
			/*
			 * P10.2 F2：慢路径的"报头声明的长度 == 描述符报的字节数"
			 * 校验。硬件/交换机在我们已经把同一帧正常转发出去之后，
			 * 还会额外送一份**副本**给 CPU；副本的描述符 len 会错成
			 * 另一类帧的长度（REVIEW-V6-HDRONLYSKB §0.3）：
			 *   短副本 len=74/86（L3 60/72），头里 plen=1460；
			 *   长副本 len=1514（L3 1500），头里 plen=20/32。
			 * 今天前者由 ip6_rcv 判 truncated 丢掉（看着像丢包），
			 * 后者由 pskb_trim 裁掉尾巴后**再转发一次**（每秒 1-2 个
			 * 重复 ACK，且没有任何计数）。这里把判断提前到驱动：
			 *   claimed > avail              ⇒ 丢帧 + rx_len_short++
			 *                                  （语义与今天等价：栈也会
			 *                                  判 truncated 丢掉，只是
			 *                                  少一次无用的栈内处理）；
			 *   avail - claimed > MAX_PAD    ⇒ **不丢**，只
			 *                                  rx_len_long++ + 限速日志
			 *                                  （长副本今天无害，拿到
			 *                                  描述符 dump 之前不做
			 *                                  激进改动）。
			 *
			 * 口径变化（P10.2 复审补）：短副本现在在**驱动**就被摘
			 * 掉、不再进 ip_rcv/ip6_rcv，所以 IP 层的
			 * IpInTruncatedPkts / Ip6InTruncatedPkts 会**下降**
			 * （下降多少 ≈ rx_len_short 涨多少），不是"丢包变多"。
			 * 看丢包要么看 rx_len_short，要么看上层其它计数字段；
			 * 别把 IP 层 truncated 归零当成"栈不再截断"。
			 *
			 * 判据来源（已确认）：这里还没建 skb，所以用池里原始帧
			 * offset 12 的 h_proto，与 zx_eth_family()、快路
			 * zx_ffe_ingress_switch_run() 读的是同一个地址、同一个值。
			 * 建 skb 之后 eth_type_trans() 会把它填成 skb->protocol，
			 * 两者相等；用 h_proto 可以让"非 IP / VLAN 外层一律跳过"
			 * 这条在分配 skb 之前就成立（0x8100/0x88a8 都不是 IP）。
			 * 帧短到读不出完整报头的（< 14+20 / 14+40）不在这里判，
			 * 留给协议栈按它自己的规则处理。
			 */
			if (fam >= 0) {
				const u8 *l3 = (const u8 *)adapter->pkt_pool +
					       (u32)bp * ZX_BUF_SIZE + 16 +
					       ETH_HLEN;
				u32 l3_avail = (u32)len - ETH_HLEN;
				u32 claimed = 0;
				bool have_len = false;

				if (fam == 1 &&
				    len >= ETH_HLEN + sizeof(struct ipv6hdr)) {
					struct ipv6hdr ip6;

					memcpy(&ip6, l3, sizeof(ip6));
					claimed = sizeof(ip6) +
						  ntohs(ip6.payload_len);
					have_len = true;
				} else if (fam == 0 &&
					   len >= ETH_HLEN + sizeof(struct iphdr)) {
					struct iphdr ip;

					memcpy(&ip, l3, sizeof(ip));
					claimed = ntohs(ip.tot_len);
					have_len = true;
				}
				if (have_len && claimed > l3_avail) {
					adapter->rx_len_short_cnt++;
					if (adapter->rx_len_short_cnt <= 5)
						dev_warn_ratelimited(adapter->dev,
							"rxq%d: short frame? port=%d len=%u claimed=%u proto=0x%04x; dropping (the stack would call it truncated)\n",
							q, port, len, claimed,
							ntohs(h_proto));
					zx_rx_consume(desc, rel);
					adapter->rx_cur_idx[q]++;
					work_done++;
					avail--;
					if (zx_bmu_free_bp(adapter, bp))
						adapter->bmu_free_fail++;
					continue;
				}
				if (have_len &&
				    l3_avail - claimed > ZX_FFE_MAX_PAD) {
					adapter->rx_len_long_cnt++;
					if (adapter->rx_len_long_cnt <= 5)
						dev_warn_ratelimited(adapter->dev,
							"rxq%d: long frame? port=%d len=%u claimed=%u proto=0x%04x; not dropping (pskb_trim will cut it)\n",
							q, port, len, claimed,
							ntohs(h_proto));
				}
			}
			/*
			 * 零拷贝：把槽位本身做成 skb 交给协议栈，bp 先不还。
			 * 之后由发送路径交给硬件（硬件发完自己归还），或者由
			 * destructor 在协议栈丢包时放回隔离环。
			 *
			 * zc 的底层开关是全局的（adapter->zc_rx），但按族生效：
			 * 只有该族的 zc_mask 位开了（zx279128s-accel.c 在切进/切出
			 * zc 时维护）才把槽位交出去，否则这一族就算“模式=zc”也
			 * 只走拷贝路径。不是 IP 的帧（ARP/PPPoE/VLAN 外层）没有
			 * 协议族可言，只要还有任何一族在 zc 里就照旧交给 zc。
			 */
			zc = false;
			skb = NULL;
			if (adapter->zc_rx &&
			    (fam < 0 || (READ_ONCE(adapter->accel.zc_mask) & BIT(fam))) &&
			    dest_dev &&
			    (dest_dev->flags & IFF_UP) && len >= ZX_ZC_MIN_LEN) {
				if (zx_zc_out(adapter) >= ZX_ZC_OUT_MAX) {
					adapter->zc_capped++;
					if (net_ratelimit())
						dev_warn_ratelimited(adapter->dev,
						     "zero-copy paused: %llu buffers outside the BMU\n",
						     zx_zc_out(adapter));
					goto zc_skip;
				}
				/*
				 * 这个 bp 本该在 BMU 空闲表里（FREE），却还在协议栈、
				 * 发送硬件或隔离环手里：空闲表里出现了重复项。这一帧
				 * 不能交付——两个 skb 共用同一个槽位一定读到互相覆盖
				 * 的内容；也不能把 bp 还回去，那会把别人正在读的槽位
				 * 交给硬件。整帧丢掉，等真正的属主归还。
				 */
				if (unlikely(adapter->zc_state[bp] !=
					     ZX_ZC_BS_FREE)) {
					adapter->zc_rx_dup++;
					adapter->zc_rxdrop++;
					if (net_ratelimit())
						zx_zc_ev_dump(adapter, bp,
							"BMU 把还在用的 bp 又发了下来，丢帧");
					zx_rx_consume(desc, rel);
					adapter->rx_cur_idx[q]++;
					work_done++;
					avail--;
					continue;
				}
				skb = build_skb(adapter->pkt_pool +
						(u32)bp * ZX_BUF_SIZE,
						ZX_BUF_SIZE);
				if (skb) {
					zx_zc_tag_set(skb, bp);
					skb->destructor = zx_zc_destructor;
					zx_zc_ev(adapter, bp, 1, (u8)(len >> 6));
					adapter->zc_state[bp] = ZX_ZC_BS_SKB;
					adapter->zc_skb[bp] = skb;
					skb_reserve(skb, 16);
					skb_put(skb, len);
					if (unlikely(adapter->zc_snap)) {
						u16 n = min_t(u16, len,
							      ZX_ZC_CANARY);

						memcpy(adapter->zc_snap +
						       (u32)bp * ZX_ZC_CANARY,
						       adapter->pkt_pool +
						       (u32)bp * ZX_BUF_SIZE + 16,
						       n);
						adapter->zc_snap_len[bp] = n;
					}
					zc = true;
				}
			}
zc_skip:
			if (!zc)
				skb = napi_alloc_skb(&adapter->napi, len);
			if (skb && dest_dev && (dest_dev->flags & IFF_UP)) {
				if (!zc) {
					dma_rmb();
					memcpy(skb_put(skb, len),
					       adapter->pkt_pool +
					       bp * ZX_BUF_SIZE + 16, len);
				} else {
					adapter->zc_rx_ok++;
				}
				t3 = ZX_TS();
				skb->dev = dest_dev;
				skb->protocol = eth_type_trans(skb, dest_dev);
				napi_gro_receive(&adapter->napi, skb);
				t4 = ZX_TS();
				dev_dstats_rx_add(dest_dev, len);
			} else {
				if (skb)
					dev_kfree_skb_any(skb);
				if (dest_dev)
					dev_dstats_rx_dropped(dest_dev);
				/*
				 * zc 的 bp 已经由 destructor 放进隔离环；非 zc
				 * 的 bp 由下面 !zc 分支归还。
				 */
			}

			zx_rx_consume(desc, rel);
			adapter->rx_cur_idx[q]++;
			work_done++;
			avail--;
			if (!zc && zx_bmu_free_bp(adapter, bp))
				adapter->bmu_free_fail++;
			t5 = ZX_TS();
			if (unlikely(timing)) {
				adapter->rx_stage_ns[0] += t1 - t0;
				adapter->rx_stage_ns[1] += t2 - t1;
				adapter->rx_stage_ns[2] += t3 - t2;
				adapter->rx_stage_ns[3] += t4 - t3;
				adapter->rx_stage_ns[4] += t5 - t4;
				adapter->rx_stage_n++;
			}
			continue;

ffe_done:
			/*
			 * 快路收尾。与上面慢路径的收尾**逐行对照**（P2-TASK §1）：
			 *
			 *   描述符消费 zx_rx_consume(desc, rel) —— 同一句：按描述符
			 *     byte6 bit0 记 release class 并清零描述符。快路帧也必须
			 *     走这一句，否则 RX 环卡死、ring B 的保留槽位迟早用完。
			 *   rx_cur_idx[q]++ / work_done++ / avail-- —— 同慢路径，
			 *     一个字都不差（预算、索引推进、环内待处理计数）。
			 *   bp —— **唯一的不同**：慢路径在 !zc 时 zx_bmu_free_bp()
			 *     把槽位还给 BMU；快路里同一个 bp 已经写进发送描述符，
			 *     发完由 BMU 自己归还。这里再还一次就是"一个槽位两个
			 *     属主"：RX 环立刻重填同一个槽位，而发送硬件还在读它
			 *     当发送缓冲。所以 FORWARDED 一律不还。
			 *   rel[] 批量释放 —— 同慢路径：消费见上面一句，真正下发在
			 *     本圈循环外的 dma_wmb() + zx_rx_release()，快路帧与
			 *     慢路帧在同一批量里结账，释放时机不变。
			 *   timing 统计 —— 快路不加样本（它是另一条路径，加了会把
			 *     慢路的每段均值稀释成没有意义的数）。
			 *
			 * DROPPED 是"改写之后提交失败"的兜底（理论上不可达）：帧已
			 * 经改过，不能回栈，但槽位也没交给硬件，所以必须还 bp。
			 */
			zx_rx_consume(desc, rel);
			adapter->rx_cur_idx[q]++;
			work_done++;
			avail--;
			if (ffe == ZX_FFE_RX_DROPPED &&
			    zx_bmu_free_bp(adapter, bp))
				adapter->bmu_free_fail++;
			continue;
		}
		if (rel[0] || rel[1])
			dma_wmb();
		if (rel[0] && zx_rx_release(adapter, q, 0, rel[0]))
			adapter->rx_rel_fail++;
		if (rel[1] && zx_rx_release(adapter, q, 1, rel[1]))
			adapter->rx_rel_fail++;
	}
	return work_done;
}

/* ---- interrupt and NAPI poll -------------------------------------- */

static irqreturn_t zx_eth_isr(int irq, void *dev_id)
{
	struct zx_eth_adapter *adapter = dev_id;

	if (!(readl(adapter->base + ZX_TM_INT_STATUS) & ZX_TM_INT_RX))
		return IRQ_NONE;

	writel(~0, adapter->base + ZX_TM_INT_MASK);
	napi_schedule(&adapter->napi);
	return IRQ_HANDLED;
}

static int zx_eth_poll(struct napi_struct *napi, int budget)
{
	struct zx_eth_adapter *adapter =
		container_of(napi, struct zx_eth_adapter, napi);
	int work_done;

	spin_lock_bh(&adapter->tx_lock);
	zx_tx_reclaim(adapter);
	if (adapter->tx_pending >= ZX_TX_QUEUE_LIMIT)
		zx_tx_kick(adapter);
	spin_unlock_bh(&adapter->tx_lock);
	work_done = budget ? zx_eth_rx(adapter, budget) : 0;

	if (work_done < budget && napi_complete_done(napi, work_done))
		writel(~ZX_TM_INT_RX, adapter->base + ZX_TM_INT_MASK);

	return work_done;
}

static enum hrtimer_restart zx_eth_tx_timer(struct hrtimer *timer)
{
	struct zx_eth_adapter *adapter =
		container_of(timer, struct zx_eth_adapter, tx_timer);

	napi_schedule(&adapter->napi);
	return HRTIMER_NORESTART;
}

/* ---- ndo_open / ndo_stop ------------------------------------------- */

static int zx_eth_open(struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *adapter = priv->adapter;

	if (adapter->open_count == 0) {
		napi_enable(&adapter->napi);
		writel(~ZX_TM_INT_RX, adapter->base + ZX_TM_INT_MASK);
		/* 加速框架的 0→1 钩子（设计 §1.4 最后一段） */
		zx_accel_on_open(adapter);
	}
	adapter->open_count++;

	/* 每张网口 open 的时候按当前 zc 状态定 GRO（注册流程会强加 GRO，
	 * 所以关它的唯一可靠时机在这里，见 zx_gro_set）。 */
	zx_gro_set(netdev, !READ_ONCE(adapter->zc_rx));

	phylink_start(priv->phylink);
	netif_start_queue(netdev);
	return 0;
}

static int zx_eth_stop(struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *adapter = priv->adapter;

	netif_tx_disable(netdev);
	phylink_stop(priv->phylink);
	adapter->open_count--;

	if (adapter->open_count == 0) {
		writel(~0, adapter->base + ZX_TM_INT_MASK);
		hrtimer_cancel(&adapter->tx_timer);
		napi_disable(&adapter->napi);
		/* a last poll may have unmasked it again */
		writel(~0, adapter->base + ZX_TM_INT_MASK);
		synchronize_irq(adapter->irq);
		/* 加速框架的 →0 钩子：按端口 flush 流表（P0 是空实现） */
		zx_accel_on_close(adapter);
	}

	return 0;
}

/* ---- net_device_ops ------------------------------------------------ */

static int zx_eth_set_mac_address(struct net_device *netdev, void *p)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *adapter = priv->adapter;
	int ret;

	ret = eth_mac_addr(netdev, p);
	if (ret)
		return ret;

	mutex_lock(&adapter->ppe_lock);
	zx_update_mac_tables(adapter);
	mutex_unlock(&adapter->ppe_lock);
	return 0;
}

static int zx_eth_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);

	return phylink_mii_ioctl(priv->phylink, ifr, cmd);
}

static const struct net_device_ops zx_netdev_ops = {
	.ndo_open		= zx_eth_open,
	.ndo_stop		= zx_eth_stop,
	.ndo_start_xmit		= zx_eth_xmit,
	.ndo_set_mac_address	= zx_eth_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= zx_eth_ioctl,
	.ndo_setup_tc		= zx_eth_setup_tc,
};

bool zx_eth_is_port(const struct net_device *dev)
{
	return dev->netdev_ops == &zx_netdev_ops;
}
EXPORT_SYMBOL_GPL(zx_eth_is_port);

/* ---- MAC counters -------------------------------------------------- */

/*
 * The offsets are from the vendor's statistics table, and the vendor driver
 * picks the same counters.  The MACs have more (errors, collisions, pause
 * frames), but these are the ones checked against known traffic.
 */
static const u16 zx_mib_regs[ZX_MIB_NUM] = {
	[ZX_MIB_TX_OCTETS]	= ZX_MAC_TX_OCTETS,
	[ZX_MIB_TX_FRAMES]	= ZX_MAC_TX_FRAMES,
	[ZX_MIB_TX_MCAST]	= ZX_MAC_TX_MCAST,
	[ZX_MIB_TX_BCAST]	= ZX_MAC_TX_BCAST,
	[ZX_MIB_TX_HIST + 0]	= ZX_MAC_TX_HIST(0),
	[ZX_MIB_TX_HIST + 1]	= ZX_MAC_TX_HIST(1),
	[ZX_MIB_TX_HIST + 2]	= ZX_MAC_TX_HIST(2),
	[ZX_MIB_TX_HIST + 3]	= ZX_MAC_TX_HIST(3),
	[ZX_MIB_TX_HIST + 4]	= ZX_MAC_TX_HIST(4),
	[ZX_MIB_TX_HIST + 5]	= ZX_MAC_TX_HIST(5),
	[ZX_MIB_RX_OCTETS]	= ZX_MAC_RX_OCTETS,
	[ZX_MIB_RX_UCAST]	= ZX_MAC_RX_UCAST,
	[ZX_MIB_RX_MCAST]	= ZX_MAC_RX_MCAST,
	[ZX_MIB_RX_BCAST]	= ZX_MAC_RX_BCAST,
	[ZX_MIB_RX_HIST + 0]	= ZX_MAC_RX_HIST(0),
	[ZX_MIB_RX_HIST + 1]	= ZX_MAC_RX_HIST(1),
	[ZX_MIB_RX_HIST + 2]	= ZX_MAC_RX_HIST(2),
	[ZX_MIB_RX_HIST + 3]	= ZX_MAC_RX_HIST(3),
	[ZX_MIB_RX_HIST + 4]	= ZX_MAC_RX_HIST(4),
	[ZX_MIB_RX_HIST + 5]	= ZX_MAC_RX_HIST(5),
};

/*
 * The counters are 32 bits wide and keep running.  At 1 Gbit/s the byte
 * counters wrap after 34 s, so they are read every few seconds, and what
 * they grew by since the last read is added to 64-bit sums.
 */
#define ZX_MIB_INTERVAL		(5 * HZ)

static void zx_mib_update(struct zx_eth_priv *priv)
{
	void __iomem *base = priv->adapter->base + ZX_MAC_PORT(priv->port);
	int i;

	lockdep_assert_held(&priv->adapter->mib_lock);

	for (i = 0; i < ZX_MIB_NUM; i++) {
		u32 val = readl(base + zx_mib_regs[i]);

		priv->mib[i] += val - priv->mib_last[i];	/* modulo 2^32 */
		priv->mib_last[i] = val;
	}
}

static void zx_mib_work(struct work_struct *work)
{
	struct zx_eth_adapter *adapter =
		container_of(to_delayed_work(work), struct zx_eth_adapter,
			     mib_work);
	int i;

	mutex_lock(&adapter->mib_lock);
	for (i = 0; i < ZX_NUM_PORTS; i++)
		if (adapter->ports[i])
			zx_mib_update(netdev_priv(adapter->ports[i]));
	mutex_unlock(&adapter->mib_lock);

	schedule_delayed_work(&adapter->mib_work, ZX_MIB_INTERVAL);
}

/* ---- ethtool_ops --------------------------------------------------- */

static int zx_eth_get_link_ksettings(struct net_device *netdev,
				     struct ethtool_link_ksettings *cmd)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);

	return phylink_ethtool_ksettings_get(priv->phylink, cmd);
}

static int zx_eth_set_link_ksettings(struct net_device *netdev,
				     const struct ethtool_link_ksettings *cmd)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);

	return phylink_ethtool_ksettings_set(priv->phylink, cmd);
}

static int zx_eth_nway_reset(struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);

	return phylink_ethtool_nway_reset(priv->phylink);
}

static void zx_eth_get_eth_mac_stats(struct net_device *netdev,
				     struct ethtool_eth_mac_stats *mac_stats)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	const u64 *mib = priv->mib;

	mutex_lock(&priv->adapter->mib_lock);
	zx_mib_update(priv);

	mac_stats->FramesTransmittedOK = mib[ZX_MIB_TX_FRAMES];
	mac_stats->OctetsTransmittedOK = mib[ZX_MIB_TX_OCTETS];
	mac_stats->MulticastFramesXmittedOK = mib[ZX_MIB_TX_MCAST];
	mac_stats->BroadcastFramesXmittedOK = mib[ZX_MIB_TX_BCAST];
	/* there is a total, but the vendor driver adds up these three */
	mac_stats->FramesReceivedOK = mib[ZX_MIB_RX_UCAST] +
				      mib[ZX_MIB_RX_MCAST] +
				      mib[ZX_MIB_RX_BCAST];
	mac_stats->OctetsReceivedOK = mib[ZX_MIB_RX_OCTETS];
	mac_stats->MulticastFramesReceivedOK = mib[ZX_MIB_RX_MCAST];
	mac_stats->BroadcastFramesReceivedOK = mib[ZX_MIB_RX_BCAST];

	mutex_unlock(&priv->adapter->mib_lock);
}

/* The last bin also counts 1522-byte (VLAN-tagged) frames */
static const struct ethtool_rmon_hist_range zx_rmon_ranges[] = {
	{   64,   64 },
	{   65,  127 },
	{  128,  255 },
	{  256,  511 },
	{  512, 1023 },
	{ 1024, 1522 },
	{}
};

static void zx_eth_get_rmon_stats(struct net_device *netdev,
				  struct ethtool_rmon_stats *rmon_stats,
				  const struct ethtool_rmon_hist_range **ranges)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(zx_rmon_ranges) != ZX_MAC_HIST_BINS + 1);

	mutex_lock(&priv->adapter->mib_lock);
	zx_mib_update(priv);
	for (i = 0; i < ZX_MAC_HIST_BINS; i++) {
		rmon_stats->hist[i] = priv->mib[ZX_MIB_RX_HIST + i];
		rmon_stats->hist_tx[i] = priv->mib[ZX_MIB_TX_HIST + i];
	}
	mutex_unlock(&priv->adapter->mib_lock);

	*ranges = zx_rmon_ranges;
}

static const struct ethtool_ops zx_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= zx_eth_get_link_ksettings,
	.set_link_ksettings	= zx_eth_set_link_ksettings,
	.nway_reset		= zx_eth_nway_reset,
	.get_eth_mac_stats	= zx_eth_get_eth_mac_stats,
	.get_rmon_stats		= zx_eth_get_rmon_stats,
};

/* ---- debugfs ------------------------------------------------------- */

/* Driver counters and the main DMA, BMU and switch registers */
static int zx_eth_dma_state_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = dev_get_drvdata(s->private);
	static const u32 regs[] = {
		ZX_TM_TAB0_ADDR, ZX_TM_TAB1_ADDR, ZX_TM_BUF_F0,
		ZX_TM_BUF_START, ZX_TM_BUF_END, ZX_BMU_CTRL,
		ZX_BMU_FREE_STATUS,
		ZX_DMA_CTRL, ZX_DMA_RING_CFG,
		ZX_DMA_AREA50, ZX_DMA_TX_RING,
		ZX_DMA_TX_START,
		ZX_RX_REL_CTRL, ZX_RX_REL_DATA,
		ZX_MAC_PORT(0), ZX_MAC_PORT(1), ZX_MAC_PORT(2),
		ZX_MAC_PORT(3), ZX_MAC_PORT(4),
		ZX_SW_FWD_CTRL,
		ZX_SPA_UP_REG_PKT_EN, ZX_SPA_UP_REG_PKT_EN + 4,
		ZX_SPA_DN_REG_PKT_EN, ZX_SPA_DN_REG_PKT_EN + 4,
		0x1d4080, 0x1d4084,
		0x1d40c0, 0x1d40c4,
		0x1d4100, 0x1d4104,
		ZX_SPA_TRAP_PROTO,
		ZX_SPA_PORT_EN(0), ZX_SPA_PORT_EN(1), ZX_SPA_PORT_EN(2),
		ZX_SPA_PORT_EN(3), ZX_SPA_PORT_EN(4), ZX_SPA_PORT_EN(6),
	};
	int i;

	rtnl_lock();
	seq_printf(s, "arena=%pad open_count=%d\n",
		   &adapter->buf_pool_dma, adapter->open_count);
	seq_printf(s, "tx_submitted=%u tx_completed=%u tx_pending=%u tx_done_excess=%u rx_resync=%u\n",
		   adapter->tx_cur_idx, adapter->tx_complete_cnt,
		   adapter->tx_pending, adapter->tx_done_excess,
		   adapter->rx_resync_cnt);
	seq_printf(s, "rx_alias=%u rx_nobuf=%u rx_rel_fail=%u rx_bad=%u rx_len_short=%llu rx_len_long=%llu\n",
		   adapter->rx_alias_cnt, adapter->rx_nobuf_cnt,
		   adapter->rx_rel_fail, adapter->rx_bad_cnt,
		   adapter->rx_len_short_cnt, adapter->rx_len_long_cnt);
	seq_printf(s, "ppe_active=%u ppe_flows=%u ppe_add_skip=%u ppe_add_fail=%u\n",
		   adapter->ppe_active, adapter->ppe_count,
		   adapter->ppe_add_skip, adapter->ppe_add_fail);
	if (adapter->rx_stage_n) {
		u32 n = adapter->rx_stage_n;
		int j;

		seq_printf(s, "rx阶段(ns/包) 描述符 %llu 无效化 %llu 分配+拷贝 %llu 上送 %llu 归还 %llu 合计 %llu n=%u\n",
			   div_u64(adapter->rx_stage_ns[0], n),
			   div_u64(adapter->rx_stage_ns[1], n),
			   div_u64(adapter->rx_stage_ns[2], n),
			   div_u64(adapter->rx_stage_ns[3], n),
			   div_u64(adapter->rx_stage_ns[4], n),
			   div_u64(adapter->rx_stage_ns[0] + adapter->rx_stage_ns[1] +
				   adapter->rx_stage_ns[2] + adapter->rx_stage_ns[3] +
				   adapter->rx_stage_ns[4], n), n);
		for (j = 0; j < 5; j++)
			adapter->rx_stage_ns[j] = 0;
		adapter->rx_stage_n = 0;
	}
	if (adapter->tx_stage_n) {
		u32 n = adapter->tx_stage_n;
		int j;

		seq_printf(s, "tx阶段(ns/包) 回收 %llu 申请BMU %llu 拷贝+同步 %llu 启动 %llu 合计 %llu n=%u\n",
			   div_u64(adapter->tx_stage_ns[0], n),
			   div_u64(adapter->tx_stage_ns[1], n),
			   div_u64(adapter->tx_stage_ns[2], n),
			   div_u64(adapter->tx_stage_ns[3], n),
			   div_u64(adapter->tx_stage_ns[0] + adapter->tx_stage_ns[1] +
				   adapter->tx_stage_ns[2] + adapter->tx_stage_ns[3], n), n);
		for (j = 0; j < 4; j++)
			adapter->tx_stage_ns[j] = 0;
		adapter->tx_stage_n = 0;
	}
	seq_printf(s, "sw_members=%#x sw_fdb_deleted=%u\n",
		   adapter->sw_members,
		   adapter->sw_fdb_deleted);
	seq_printf(s, "bmu_alloc_fail=%u bmu_alloc_late=%u bmu_free_fail=%u bmu_free_slots=%u\n",
		   adapter->bmu_alloc_fail, adapter->bmu_alloc_late,
		   adapter->bmu_free_fail, adapter->bmu_free_slots);
	seq_printf(s, "accel v4=%s v6=%s q=%u\n",
		   zx_accel_mode_name(READ_ONCE(adapter->accel.v4_mode)),
		   zx_accel_mode_name(READ_ONCE(adapter->accel.v6_mode)),
		   READ_ONCE(adapter->tx_pending));
	seq_printf(s, "zc_rx=%d rx_ok=%llu tx_ok=%llu free_ok=%llu out=%lld fallback=%llu q_ovf=%llu capped=%llu off=%llu\n",
		   READ_ONCE(adapter->zc_rx), adapter->zc_rx_ok,
		   adapter->zc_tx_ok, adapter->zc_free_ok,
		   (long long)adapter->zc_rx_ok -
			   (long long)adapter->zc_tx_ok -
			   (long long)adapter->zc_free_ok,
		   adapter->zc_fallback, adapter->zc_q_ovf,
		   adapter->zc_capped, adapter->zc_off_cnt);
	seq_printf(s, "zc_shadow=%llu q_n=%u lost=%llu stale=%llu odd=%llu corrupt=%llu tx_dup=%llu rx_dup=%llu notag=%llu badfree=%llu dup_push=%llu dup_reap=%llu badstate=%llu rxdrop=%llu\n",
		   adapter->zc_shadow, adapter->zc_q_n, adapter->zc_lost,
		   adapter->zc_stale, adapter->zc_odd, adapter->zc_corrupt,
		   adapter->zc_tx_dup, adapter->zc_rx_dup, adapter->zc_notag,
		   adapter->zc_badfree, adapter->zc_dup_push,
		   adapter->zc_dup_reap, adapter->zc_badstate,
		   adapter->zc_rxdrop);

	for (i = 0; i < 0x100; i += 4)
		seq_printf(s, "bmu+%03x=%08x\n", i,
			   readl(adapter->base + ZX_BMU_BASE + i));
	/* Reading 0x68 (TX done) would clear the count of sent frames */
	for (i = 0; i < 0x120; i += 4) {
		if (i == 0x68)
			continue;
		seq_printf(s, "dma+%03x=%08x\n", i,
			   readl(adapter->base + ZX_DMA_BASE + i));
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(s, "%06x=%08x\n", regs[i],
			   readl(adapter->base + regs[i]));

	for (i = 0; i < ZX_RX_QUEUES; i++)
		seq_printf(s, "rxq%d count=%u consumed=%u\n", i,
			   readl(adapter->base + ZX_DMA_RX_QUEUE_CNT(i)) & 0xffff,
			   READ_ONCE(adapter->rx_cur_idx[i]));
	rtnl_unlock();

	return 0;
}

/*
 * The driver cannot be unbound (suppress_bind_attrs) and has no remove
 * function, so the directory stays until reboot.
 */
/* "zc": 读当前开关与未归还 bp 计数，写 0/1 切换（同一镜像里做 A/B） */
static int zx_eth_zc_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	long long out = (long long)adapter->zc_rx_ok -
			(long long)adapter->zc_tx_ok -
			(long long)adapter->zc_free_ok;

	seq_printf(s, "%d\n", READ_ONCE(adapter->zc_rx));
	seq_printf(s, "rx_ok=%llu tx_ok=%llu free_ok=%llu out=%lld fallback=%llu q_ovf=%llu capped=%llu off=%llu\n",
		   adapter->zc_rx_ok, adapter->zc_tx_ok, adapter->zc_free_ok,
		   out, adapter->zc_fallback, adapter->zc_q_ovf,
		   adapter->zc_capped, adapter->zc_off_cnt);
	seq_printf(s, "shadow=%llu q_n=%u lost=%llu stale=%llu odd=%llu corrupt=%llu tx_dup=%llu rx_dup=%llu notag=%llu badfree=%llu dup_push=%llu dup_reap=%llu badstate=%llu rxdrop=%llu\n",
		   adapter->zc_shadow, adapter->zc_q_n, adapter->zc_lost,
		   adapter->zc_stale, adapter->zc_odd, adapter->zc_corrupt,
		   adapter->zc_tx_dup, adapter->zc_rx_dup, adapter->zc_notag,
		   adapter->zc_badfree, adapter->zc_dup_push,
		   adapter->zc_dup_reap, adapter->zc_badstate,
		   adapter->zc_rxdrop);
	return 0;
}

static int zx_eth_zc_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_eth_zc_show, inode->i_private);
}

/*
 * "zc_ev": 把所有"不在 BMU 空闲表里"或者还挂在隔离环上的槽位连同最近 8 条
 * 事件流水打出来。定位泄漏/双还时不用重启，读一次就是一张快照。
 */
static int zx_eth_zc_ev_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	u32 bp;

	for (bp = 0; bp < ZX_NUM_BUFFERS; bp++) {
		struct skb_shared_info *sp;
		u8 st = adapter->zc_state[bp];
		int inring = adapter->zc_inring ?
			     test_bit(bp, adapter->zc_inring) : -1;

		if (st == ZX_ZC_BS_FREE && inring <= 0)
			continue;
		sp = zx_zc_shinfo(adapter, bp);
		seq_printf(s, "bp=%u state=%u refs=%u tag=%08x skb=%p inring=%d n=%u ev=%08x %08x %08x %08x %08x %08x %08x %08x\n",
			   bp, st, zx_zc_refs(sp),
			   (u32)(unsigned long)sp->destructor_arg,
			   adapter->zc_skb[bp], inring,
			   adapter->zc_ev_n[bp],
			   adapter->zc_ev[bp][0], adapter->zc_ev[bp][1],
			   adapter->zc_ev[bp][2], adapter->zc_ev[bp][3],
			   adapter->zc_ev[bp][4], adapter->zc_ev[bp][5],
			   adapter->zc_ev[bp][6], adapter->zc_ev[bp][7]);
	}
	return 0;
}

static int zx_eth_zc_ev_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_eth_zc_ev_show, inode->i_private);
}

static const struct file_operations zx_eth_zc_ev_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_eth_zc_ev_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * "zc" 的兼容写入口。它以前直接调 zx_zc_set()，那在 P0 之后是错的：zc 现在
 * 按协议族记账（accel->zc_mask），绕过 zx_accel_set() 会留下"zc_rx 为真、
 * mask 为 0"或"某族模式=hw 但 zc 开着"的半状态——正是 accel 框架要消灭的那种
 * 双主人记账。所以写 1 = 两族都切到 zc（依次过 S0-S7），写 0 = 把还在 zc 的族
 * 切回 stack；框架拒绝时（典型是 IPv4 的 PPE 还绑着 flowtable，-EBUSY）写失败
 * 并保持原状。读侧（zx_eth_zc_show）不变，还是给实验和巡检看的计数。
 */
static ssize_t zx_eth_zc_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct zx_eth_adapter *adapter = s->private;
	char buf[8];
	int family, ret = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;

	for (family = 0; family < 2; family++) {
		u8 want = buf[0] == '1' ? ZX_ACC_ZC : ZX_ACC_STACK;

		/* 关的时候只碰真的在 zc 里的族，别的模式（hw/ffe）不动 */
		if (want == ZX_ACC_STACK &&
		    zx_accel_cur_mode(&adapter->accel, family) != ZX_ACC_ZC)
			continue;
		ret = zx_accel_set(adapter, family, want);
		if (ret)
			break;
	}
	if (ret)
		return ret;

	dev_info(adapter->dev, "zero-copy %s (zc_mask=%u)\n",
		 READ_ONCE(adapter->zc_rx) ? "enabled" : "disabled",
		 READ_ONCE(adapter->accel.zc_mask));
	return count;
}

static const struct file_operations zx_eth_zc_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_eth_zc_open,
	.read		= seq_read,
	.write		= zx_eth_zc_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * 实验入口：TX 描述符只有 16 字节，帧的位置由 byte7/byte8 里的 bp 给出，长度字段
 * 14 位（最大 0x3fff）。发送路径因此一直假设"一个描述符 = 一个槽位里的整帧"，
 * 并且把超过 1536 字节的帧直接丢掉。这个假设没被验证过，而它决定了 GRO 合并出来
 * 的大帧（以及任何 SG/TSO 方案）能不能发出去。这里绕过协议栈，手工拼描述符和池里
 * 的帧内容，用宿主上的 tcpdump 判读。
 *
 * 用法：echo "<len> <which> <off2> <seglen>" > txtest
 *   len    整帧字节数（默认 1600）
 *   which  0 = 只用单个描述符；bit0 = 链式两描述符并置第一个的 byte7 bit0；
 *          bit1 = 第二个的 byte7 bit0 也置
 *   off2   链式时第二个描述符的数据从槽位内哪个偏移开始（默认 16，也可试 0）
 *   seglen 0 = 每个描述符写自己那段的长度；1 = 两个都写整帧总长
 *   dev    只在这个端口上发（如 lan2）；不给则所有有链路的端口都发一份
 *
 * 帧内容：广播目的 + 本口 MAC + ethertype 0x88b5，从帧偏移 14 起是小端 32 位序号
 * （0x5a5a0000 | k），十六进制就是 00 00 5a 5a, 01 00 5a 5a, …，能看出对端收到的
 * 字节连不连续、第二段从哪儿接上来。每个有链路的端口各发一份，省得猜宿主接在哪。
 */
static ssize_t zx_eth_txtest_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct zx_eth_adapter *adapter = file->private_data;
	unsigned int len = 1600, which = 0, off2 = 16, seglen = 0;
	char buf[48], dev[IFNAMSIZ] = "";
	u8 *frame;
	int i, p, sent = 0;
	u16 first_bp = 0, last_bp = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	if (sscanf(buf, "%u %u %u %u %15s", &len, &which, &off2, &seglen,
		   dev) < 1)
		return -EINVAL;
	if (len < ETH_ZLEN || len > 0x3fff)
		return -EINVAL;
	/* 第二段必须整段落在自己的槽位里，否则会写坏池里别的槽位 */
	if (off2 > ZX_BUF_SIZE || off2 + (len - len / 2) > ZX_BUF_SIZE)
		return -EINVAL;

	frame = kmalloc(len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	memset(frame, 0xff, 6);
	memset(frame + 6, 0, 6);
	frame[12] = 0x88;
	frame[13] = 0xb5;
	for (i = 14; i + 4 <= (int)len; i += 4) {
		u32 k = (i - 14) >> 2;

		frame[i] = k & 0xff;
		frame[i + 1] = (k >> 8) & 0xff;
		frame[i + 2] = 0x5a;
		frame[i + 3] = 0x5a;
	}
	while (i < (int)len)
		frame[i++] = 0;

	/*
	 * 打印发送前的 MAC 计数器：硬件到底把这个帧算没算进 tx_octets，
	 * 是"MAC 拒发"和"宿主没收到"之间的分水岭。计数器是硬件绝对值，
	 * 下一次调用打印的就是这一轮传完后的值。
	 */
	for (p = 0; p < ZX_NUM_PORTS; p++) {
		struct net_device *nd = adapter->ports[p];
		struct zx_eth_priv *pv;
		void __iomem *mb;

		if (!nd || !netif_running(nd))
			continue;
		if (dev[0] && strcmp(nd->name, dev))
			continue;
		pv = netdev_priv(nd);
		mb = adapter->base + ZX_MAC_PORT(pv->port);
		dev_info(adapter->dev,
			 "txtest: %s 发送前 MIB tx_octets=%u tx_frames=%u\n",
			 nd->name, readl(mb + ZX_MAC_TX_OCTETS),
			 readl(mb + ZX_MAC_TX_FRAMES));
	}

	spin_lock_bh(&adapter->tx_lock);
	zx_tx_reclaim(adapter);
	for (p = 0; p < ZX_NUM_PORTS; p++) {
		struct net_device *nd = adapter->ports[p];
		struct zx_eth_priv *priv;
		unsigned int ndesc, nslots, seg[2], cut;
		u16 bp[9] = { 0 };
		int k, ok = 1;

		if (!nd || !netif_running(nd))
			continue;
		if (dev[0] && strcmp(nd->name, dev))
			continue;
		priv = netdev_priv(nd);
		ndesc = (which & 0x3) ? 2 : 1;
		/* 一个描述符要读出槽位时，后面的槽位得是相邻的才有意义 */
		nslots = (len + 16 + ZX_BUF_SIZE - 1) / ZX_BUF_SIZE;
		if (ndesc == 2)
			nslots = 2;
		if (adapter->tx_pending + ndesc > ZX_TX_QUEUE_LIMIT)
			break;

		for (k = 0; k < (int)nslots; k++) {
			bp[k] = zx_bmu_alloc_bp(adapter);
			if (bp[k] < 0 || bp[k] >= ZX_NUM_BUFFERS) {
				ok = 0;
				break;
			}
		}
		if (!ok) {
			while (k-- > 0)
				zx_bmu_free_bp(adapter, bp[k]);
			break;
		}
		if (ndesc == 1) {
			for (k = 1; k < (int)nslots; k++)
				if (bp[k] != bp[0] + k) {
					dev_warn(adapter->dev,
						 "txtest: 槽位不相邻 bp=%u %u\n",
						 bp[0], bp[k]);
					ok = 0;
				}
			if (!ok) {
				for (k = 0; k < (int)nslots; k++)
					zx_bmu_free_bp(adapter, bp[k]);
				break;
			}
		}

		cut = len / 2;
		if (ndesc == 1) {
			memcpy((u8 *)adapter->pkt_pool +
			       (u32)bp[0] * ZX_BUF_SIZE + 16, frame, len);
			seg[0] = len;
		} else {
			/* 第二段放 +off2，之后靠 tcpdump 看硬件从哪里开始读 */
			memcpy((u8 *)adapter->pkt_pool +
			       (u32)bp[0] * ZX_BUF_SIZE + 16, frame, cut);
			memcpy((u8 *)adapter->pkt_pool +
			       (u32)bp[1] * ZX_BUF_SIZE + off2, frame + cut,
			       len - cut);
			seg[0] = seglen ? len : cut;
			seg[1] = seglen ? len : len - cut;
		}
		if (adapter->pkt_pool_cacheable)
			dma_sync_single_for_device(adapter->dev,
						   adapter->pkt_pool_dma +
						   (u32)bp[0] * ZX_BUF_SIZE,
						   16 + len, DMA_TO_DEVICE);

		for (k = 0; k < (int)ndesc; k++) {
			u32 idx = (adapter->tx_cur_idx + k) % ZX_NUM_TX_DESC;
			__le32 *d = (__le32 *)adapter->tx_ring +
				    idx * (ZX_DESC_SIZE / sizeof(*d));

			d[0] = cpu_to_le32(0x80 |
					   (ZX_PORT_TX_QUEUE(priv->port) << 20));
			d[3] = cpu_to_le32(0x3 | ((seg[k] & 0x3fff) << 2));
			d[1] = cpu_to_le32(0x10000);
			d[2] = cpu_to_le32(0x21000000 |
					   ((seg[k] & 0x3fff) << 9));
			((u8 *)d)[7] = (u8)((bp[k] & 0x7f) << 1) |
				       ((which & (1u << k)) ? 1 : 0);
			((u8 *)d)[8] = (u8)(bp[k] >> 7);
			/*
			 * 一次门铃只启动一个描述符：正常发送也是这么写的。
			 * 上一轮把 4 个描述符攒起来只敲一次门铃，硬件只吃掉一个，
			 * 其余卡在 ring 里，整条发送路径随后慢到 3 秒一个包
			 * （tx_pending 留了 3，重启才清）。
			 */
			dma_wmb();
			writel(1, adapter->base + ZX_DMA_TX_START);
		}
		adapter->tx_cur_idx += ndesc;
		adapter->tx_pending += ndesc;
		if (!sent)
			first_bp = bp[0];
		last_bp = bp[ndesc - 1];
		sent++;
	}
	spin_unlock_bh(&adapter->tx_lock);
	kfree(frame);
	if (!sent)
		return -EAGAIN;
	dev_info(adapter->dev,
		 "txtest: len=%u which=%u off2=%u seglen=%u dev=%s 端口 %d 个，bp=%u..%u，tx_pending=%u\n",
		 len, which, off2, seglen, dev[0] ? dev : "(全部)", sent,
		 first_bp, last_bp, adapter->tx_pending);
	return count;
}

static const struct file_operations zx_eth_txtest_fops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.write		= zx_eth_txtest_write,
};

/*
 * P10.2 F6：alias_dump —— alias 丢弃点的取证回读（只读 0444）。
 *
 * 这是**区分「已转发帧的副本」与「本该转发的原帧」的工具**，不参与任何
 * 数据面判据：
 *   - 每条 `#N` 是第 N 个被 alias 判据丢掉的帧（N 从 0 开始，最多 32 条，
 *     之后覆盖最老的）。`desc0..3` 是 RX 描述符原值（`le32_to_cpu` 之后），
 *     高位的 reason/copy/trap 类字段驱动平时完全没读；`len/port` 是描述符
 *     解析结果；`head` 是池里帧的前 64 B 十六进制。
 *   - `head` 前 6 字节是目的 MAC、6..11 是源 MAC。**若源 MAC 等于
 *     br-lan/本口 MAC**，说明 RX 缓冲已被 FFE 的 zx_ffe_rewrite() 原地改写过
 *     （h_source ← 出口端口 MAC）；若它是外部主机的 MAC，说明这帧进缓冲时
 *     就是那样，不是 FFE 改的。
 *   - 再结合同一时刻 lan2.tx 是否已发出同 seq 的帧（宿主机抓包），就能定性
 *     丢掉的是副本还是原帧。
 * 读侧不加锁：写侧只有 RX NAPI 一处，先填槽位再发布 alias_snap_idx；极端
 * 并发下可能读到正在覆盖的半条，重读一次即可。
 */
static int zx_eth_alias_dump_show(struct seq_file *s, void *data)
{
	/*
	 * s->private 就是 debugfs_create_file(..., adapter, ...) 传进来的
	 * `struct zx_eth_adapter *`（见 zx_eth_debugfs_init()），**不是**
	 * struct device *：这里绝不能调 dev_get_drvdata()（那会把 adapter
	 * 结构体里 driver_data 偏移处的字节当指针解引用，`cat alias_dump`
	 * 读一次就 oops）。与本文件其它 single_open 的 show（zc/zc_ev）同一
	 * 写法；只有走 debugfs_create_devm_seqfile() 的 dma_state 才是
	 * struct device *、才该用 dev_get_drvdata()。
	 */
	struct zx_eth_adapter *adapter = s->private;
	u32 cnt = READ_ONCE(adapter->alias_snap_cnt);
	u32 idx = smp_load_acquire(&adapter->alias_snap_idx);
	u32 n = min_t(u32, cnt, ZX_ALIAS_SNAP_N);
	u32 i, j;

	seq_printf(s, "alias_dump: cnt=%u idx=%u\n", cnt, idx);
	for (i = 0; i < n; i++) {
		/*
		 * cnt < N：槽位从 0 顺序写；cnt >= N：idx 是最老的槽位，
		 * 按时间顺序从它开始绕一圈。
		 */
		u32 slot = cnt >= ZX_ALIAS_SNAP_N ?
			   ((idx + i) & (ZX_ALIAS_SNAP_N - 1)) : i;
		const struct zx_alias_snap *a = &adapter->alias_snap[slot];

		seq_printf(s, "#%u slot=%u desc0=%08x desc1=%08x desc2=%08x desc3=%08x len=%u port=%u\n",
			   i, slot, a->desc[0], a->desc[1], a->desc[2],
			   a->desc[3], a->len, a->port);
		seq_puts(s, "  head=");
		for (j = 0; j < sizeof(a->head); j++)
			seq_printf(s, "%02x", a->head[j]);
		seq_putc(s, '\n');
	}
	/*
	 * 同一行给出对照读数：rx_alias 是 alias 判据累计丢掉的帧数；
	 * rx_len_short/long 是慢路径长度校验的两个计数；ffe_fwd 是两族快路
	 * 转发总数（cnt[0]+cnt[1]）。一次读全能对上时间口径。
	 */
	seq_printf(s, "rx_alias=%u rx_len_short=%llu rx_len_long=%llu ffe_fwd=%llu\n",
		   adapter->rx_alias_cnt,
		   READ_ONCE(adapter->rx_len_short_cnt),
		   READ_ONCE(adapter->rx_len_long_cnt),
		   READ_ONCE(adapter->accel.cnt[0].fwd) +
		   READ_ONCE(adapter->accel.cnt[1].fwd));
	return 0;
}

static int zx_eth_alias_dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_eth_alias_dump_show, inode->i_private);
}

static const struct file_operations zx_eth_alias_dump_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_eth_alias_dump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void zx_eth_debugfs_init(struct zx_eth_adapter *adapter)
{
	struct dentry *dir = debugfs_create_dir(dev_name(adapter->dev), NULL);

	debugfs_create_devm_seqfile(adapter->dev, "dma_state", dir,
				    zx_eth_dma_state_show);
	/* P10.2 F6：alias 丢弃点的环形取证快照（只读） */
	debugfs_create_file("alias_dump", 0444, dir, adapter,
			    &zx_eth_alias_dump_fops);
	debugfs_create_file("zc", 0644, dir, adapter, &zx_eth_zc_fops);
	debugfs_create_file("zc_ev", 0444, dir, adapter, &zx_eth_zc_ev_fops);
	debugfs_create_file("txtest", 0200, dir, adapter, &zx_eth_txtest_fops);
	/* 加速路径（设计 §1.3）：两个族的模式与计数、流表读写、降级开关 */
	debugfs_create_file("accel_ipv4", 0644, dir, adapter,
			    &zx_accel_v4_fops);
	debugfs_create_file("accel_ipv6", 0644, dir, adapter,
			    &zx_accel_v6_fops);
	debugfs_create_file("ffe_flows", 0644, dir, adapter,
			    &zx_accel_flows_fops);
	/* P1 的静态流表入口（P1-TASK §2）：add/del/flush 写，读回最后一条结果 */
	debugfs_create_file("ffe_add", 0644, dir, adapter, &zx_accel_add_fops);
	debugfs_create_file("ffe_del", 0644, dir, adapter, &zx_accel_del_fops);
	debugfs_create_file("ffe_flush", 0644, dir, adapter,
			    &zx_accel_flush_fops);
	/* 老化时间的调试口（P4）：与模块参数 ffe_aging 同一个变量，读回当前值 */
	debugfs_create_file("ffe_aging", 0644, dir, adapter,
			    &zx_accel_aging_fops);
	/* hw→ffe 迁移桥（P3）：写 1 取 PPE 快照，读回统计与样例 cookie */
	debugfs_create_file("ffe_migrate", 0644, dir, adapter,
			    &zx_accel_migrate_fops);
	/* 旁路学习（P5）：读回开关与计数，写 0/1 开关（默认 1，P10.2 F5） */
	debugfs_create_file("ffe_learn", 0644, dir, adapter,
			    &zx_accel_learn_fops);
	debugfs_create_file("accel_autofallback", 0644, dir, adapter,
			    &zx_accel_autofallback_fops);
	/*
	 * SoC 温度（只读，0444）：读一次就现场触发一次转换并返回摄氏度。
	 * 传感器是懒映射的，probe 不碰它；完整寄存器语义见 zx279128s-accel.c
	 * 的 "SoC 温度" 一节（由原厂 vmlinux 反汇编得出）。
	 */
	debugfs_create_file("soc_temp", 0444, dir, adapter,
			    &zx_accel_soc_temp_fops);
}

static void zx_pkt_pool_free(struct device *dev,
			     struct zx_eth_adapter *adapter)
{
	zx_zc_unpark_pages(adapter);
	if (!adapter->pkt_pool_cacheable || !adapter->pkt_pool)
		return;
	dma_unmap_single(dev, adapter->pkt_pool_dma, ZX_BUF_POOL_SIZE,
			 DMA_BIDIRECTIONAL);
	free_pages_exact(adapter->pkt_pool, ZX_BUF_POOL_SIZE);
	adapter->pkt_pool = NULL;
	adapter->pkt_pool_cacheable = false;
}

static int zx_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zx_eth_adapter *adapter;
	struct device_node *np;
	u8 addrs[ZX_NUM_PORTS][ETH_ALEN];
	u8 zc_mask;
	unsigned long have_addr = 0;
	int i, ret;

	struct device_node *ports_np __free(device_node) =
		of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports_np)
		return dev_err_probe(dev, -ENODEV, "no ethernet-ports node\n");

	/*
	 * The MAC addresses can come from nvmem cells on a flash partition
	 * that is not registered yet.  Get them before the hardware is set
	 * up, so that deferring the probe leaves no DMA running.
	 */
	for_each_available_child_of_node_scoped(ports_np, port_np) {
		u32 port;

		if (of_property_read_u32(port_np, "reg", &port) ||
		    port >= ZX_NUM_PORTS)
			continue;
		ret = of_get_mac_address(port_np, addrs[port]);
		if (ret == -EPROBE_DEFER)
			return ret;
		if (!ret)
			__set_bit(port, &have_addr);
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set the DMA mask\n");

	adapter = devm_kzalloc(dev, sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return -ENOMEM;

	adapter->irq = platform_get_irq(pdev, 0);
	if (adapter->irq < 0)
		return adapter->irq;

	adapter->dev = dev;
	spin_lock_init(&adapter->tx_lock);
	spin_lock_init(&adapter->bmu_free_lock);
	mutex_init(&adapter->ppe_lock);
	spin_lock_init(&adapter->trap_lock);
	mutex_init(&adapter->sw_lock);
	spin_lock_init(&adapter->sw_fdb_lock);
	INIT_WORK(&adapter->sw_work, zx_sw_work);
	mutex_init(&adapter->mib_lock);
	INIT_DELAYED_WORK(&adapter->mib_work, zx_mib_work);
	adapter->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adapter->base))
		return PTR_ERR(adapter->base);

	ret = rhashtable_init(&adapter->ppe_flows, &zx_ppe_ht_params);
	if (ret)
		return ret;

	/*
	 * 加速框架骨架：锁、空流表、计数面、老化/自动降级定时器。它必须在
	 * 数据面（DMA、中断、网口注册）之前立起来，之后任何一步失败都走
	 * err_accel 收摊。
	 */
	ret = zx_accel_init(adapter);
	if (ret)
		goto err_hashtable;	/* accel 没建起来，只销毁 PPE 表 */

	/*
	 * 初始模式按 module_param 直接写字段，不走 zx_accel_set()：probe 期
	 * NAPI 还没 enable、端口还没注册，做 quiesce/napi_disable 没有意义，
	 * 也没有数据面在跑。
	 */
	adapter->accel.v4_mode = (unsigned int)accel_v4 <= ZX_ACC_ZC ?
				 (u8)accel_v4 : (u8)ZX_ACC_STACK;
	adapter->accel.v6_mode = (unsigned int)accel_v6 <= ZX_ACC_ZC ?
				 (u8)accel_v6 : (u8)ZX_ACC_STACK;
	if (adapter->accel.v6_mode == ZX_ACC_HW) {
		dev_warn(dev, "accel_v6=%d: IPv6 has no hardware path, using stack\n",
			 accel_v6);
		adapter->accel.v6_mode = ZX_ACC_STACK;
	}
	adapter->accel.autofallback = accel_autofallback;
	/*
	 * zc 现在按协议族记账（accel->zc_mask，见 zx279128s-eth.h）。开机参数
	 * 直接写 mode 字段、不走 zx_accel_set()，所以这里必须把 mask 与 zc_rx
	 * 一起补齐，否则 accel_v4=3 会得到"模式写着 zc、数据面却没有一个族允许
	 * 走 zc"的半状态。老的 zc_rx=1 按它从前的全局语义处理：两族都算 zc。
	 */
	if (zc_rx) {
		adapter->accel.v4_mode = ZX_ACC_ZC;
		adapter->accel.v6_mode = ZX_ACC_ZC;
	}
	zc_mask = 0;
	if (adapter->accel.v4_mode == ZX_ACC_ZC)
		zc_mask |= BIT(0);
	if (adapter->accel.v6_mode == ZX_ACC_ZC)
		zc_mask |= BIT(1);
	WRITE_ONCE(adapter->accel.zc_mask, zc_mask);
	dev_info(dev, "accel: v4=%s v6=%s autofallback=%u\n",
		 zx_accel_mode_name(adapter->accel.v4_mode),
		 zx_accel_mode_name(adapter->accel.v6_mode),
		 adapter->accel.autofallback ? 1u : 0u);

	adapter->buffer_pool = dma_alloc_coherent(dev, ZX_DMA_ARENA_SIZE,
						  &adapter->buf_pool_dma, GFP_KERNEL);
	if (!adapter->buffer_pool) {
		ret = -ENOMEM;
		goto err_accel;
	}
	/*
	 * Packet buffers: 8 MiB, physically contiguous.  Two mappings are
	 * possible, and which one is cheaper depends on how expensive this
	 * SoC's cache maintenance is, measured per frame:
	 *
	 *   cacheable (streaming mapping):  1 invalidate per RX frame
	 *                                   + 1 clean per TX frame
	 *   uncached  (coherent arena):     no maintenance, but every payload
	 *                                   byte is read/written uncached
	 *
	 * On the ZX279128S one cache-line maintenance op costs about as much
	 * as a 32 B uncached read (~160 ns), so a 1400 B frame pays 8.4 us of
	 * invalidate + clean on top of the copies, while the uncached path
	 * reads 1400 B in 7.4 us and *writes* it in 0.75 us (writes coalesce).
	 * The uncached mapping therefore wins on transmit by a wide margin;
	 * pool_cacheable=1 selects the cacheable one for A/B tests.
	 */
	if (pool_cacheable) {
		adapter->pkt_pool = alloc_pages_exact(ZX_BUF_POOL_SIZE,
						      GFP_KERNEL | __GFP_ZERO);
		if (adapter->pkt_pool) {
			adapter->pkt_pool_dma =
				dma_map_single(dev, adapter->pkt_pool,
					       ZX_BUF_POOL_SIZE,
					       DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, adapter->pkt_pool_dma)) {
				free_pages_exact(adapter->pkt_pool,
						 ZX_BUF_POOL_SIZE);
				adapter->pkt_pool = NULL;
			} else {
				adapter->pkt_pool_cacheable = true;
			}
		}
	}
	if (!adapter->pkt_pool) {
		adapter->pkt_pool = adapter->buffer_pool;
		adapter->pkt_pool_dma = adapter->buf_pool_dma;
	}

	/*
	 * 零拷贝的状态表。zc_rx 打开时把池页引用计数垫高，并把 adapter
	 * 登记给 skb destructor（SoC 上只有一个以太网控制器）。
	 */
	adapter->zc_state = devm_kzalloc(dev, ZX_NUM_BUFFERS, GFP_KERNEL);
	adapter->zc_skb = devm_kzalloc(dev, ZX_NUM_BUFFERS *
				       sizeof(*adapter->zc_skb), GFP_KERNEL);
	adapter->zc_snap = devm_kzalloc(dev, (size_t)ZX_NUM_BUFFERS * ZX_ZC_CANARY,
					GFP_KERNEL);
	adapter->zc_snap_len = devm_kzalloc(dev, ZX_NUM_BUFFERS *
					    sizeof(*adapter->zc_snap_len),
					    GFP_KERNEL);
	adapter->zc_free_at = devm_kzalloc(dev, ZX_NUM_BUFFERS *
					   sizeof(*adapter->zc_free_at), GFP_KERNEL);
	adapter->zc_free_tog = devm_kzalloc(dev, ZX_NUM_BUFFERS, GFP_KERNEL);
	adapter->zc_ev = devm_kzalloc(dev, ZX_NUM_BUFFERS *
				      sizeof(*adapter->zc_ev), GFP_KERNEL);
	adapter->zc_ev_n = devm_kzalloc(dev, ZX_NUM_BUFFERS, GFP_KERNEL);
	adapter->zc_inring = devm_kzalloc(dev, BITS_TO_LONGS(ZX_NUM_BUFFERS) *
					  sizeof(unsigned long), GFP_KERNEL);
	if (!adapter->zc_state || !adapter->zc_skb || !adapter->zc_snap ||
	    !adapter->zc_snap_len || !adapter->zc_free_at ||
	    !adapter->zc_free_tog || !adapter->zc_ev || !adapter->zc_ev_n ||
	    !adapter->zc_inring) {
		ret = -ENOMEM;
		goto err_dma;
	}
	for (i = 0; i < ZX_ZC_Q_LEN; i++)
		adapter->zc_q_bp[i] = 0xffff;
	spin_lock_init(&adapter->zc_lock);
	/* 不变式：zc_rx 为真 <=> 至少一族在 zc 模式（accel->zc_mask 非零） */
	WRITE_ONCE(adapter->zc_rx, zc_mask != 0);
	if (zc_mask) {
		zx_zc_park_pages(adapter);
		WRITE_ONCE(zx_zc_adapter, adapter);
		dev_info(dev, "zero-copy RX/TX on the packet pool enabled (zc_mask=%u)\n",
			 zc_mask);
	}

	adapter->tab0 = adapter->buffer_pool + ZX_TAB0_OFFSET;
	adapter->tab0_dma = adapter->buf_pool_dma + ZX_TAB0_OFFSET;
	adapter->tab1 = adapter->buffer_pool + ZX_TAB1_OFFSET;
	adapter->tab1_dma = adapter->buf_pool_dma + ZX_TAB1_OFFSET;
	adapter->rx_ring = adapter->buffer_pool + ZX_RX_OFFSET;
	adapter->rx_ring_dma = adapter->buf_pool_dma + ZX_RX_OFFSET;
	adapter->tx_ring = adapter->buffer_pool + ZX_TX_OFFSET;
	adapter->tx_ring_dma = adapter->buf_pool_dma + ZX_TX_OFFSET;

	ret = zx_hw_init(adapter);
	if (ret)
		goto err_dma;

	/* One netdev per port, named by the port's label */
	for_each_available_child_of_node(ports_np, np) {
		struct net_device *netdev;
		struct zx_eth_priv *priv;
		phy_interface_t interface;
		const char *label;
		u32 port;

		if (of_property_read_u32(np, "reg", &port) || port >= ZX_NUM_PORTS ||
		    adapter->ports[port]) {
			dev_warn(dev, "%pOF: bad or duplicate reg\n", np);
			continue;
		}

		netdev = devm_alloc_etherdev(dev, sizeof(struct zx_eth_priv));
		if (!netdev) {
			ret = -ENOMEM;
			of_node_put(np);
			goto err_disconnect;
		}
		SET_NETDEV_DEV(netdev, dev);
		if (!of_property_read_string(np, "label", &label)) {
			strscpy(netdev->name, label, IFNAMSIZ);
			netdev->name_assign_type = NET_NAME_PREDICTABLE;
		}
		if (test_bit(port, &have_addr))
			eth_hw_addr_set(netdev, addrs[port]);
		else
			eth_hw_addr_random(netdev);
		netdev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
		netdev->netdev_ops = &zx_netdev_ops;
		netdev->ethtool_ops = &zx_ethtool_ops;
		netdev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
		netdev->hw_features |= NETIF_F_HW_TC;
		netdev->features |= NETIF_F_HW_TC;
		/*
		 * 零拷贝的 skb 就是这个槽位本身，GRO 一旦合并，frags 会指向
		 * 别的槽位，那条 bp 就再也没人回收了（见 zx_zc_* 一节）。
		 * 但这里不能关：注册流程在这之后才强加 SOFT_FEATURES，
		 * 关它的是 ndo_open / zx_zc_set 里的 zx_gro_set()。
		 */
		netdev->min_mtu    = 68;
		netdev->max_mtu    = 1500;
		/*
		 * 默认 fq_codel 会排 1000 帧，转发器排这么深只会把延迟堆起来；
		 * 零拷贝时这些排队的 skb 还各占一个池缓冲。64 帧够吸收突发，
		 * 又能让 bp 尽快回到 BMU。
		 */
		netdev->tx_queue_len = 64;
		priv = netdev_priv(netdev);
		priv->netdev  = netdev;
		priv->adapter = adapter;
		priv->port    = port;
		adapter->ports[port] = netdev;

		ret = of_get_phy_mode(np, &interface);
		if (ret) {
			dev_err_probe(dev, ret, "%pOF: no valid phy-mode\n", np);
			of_node_put(np);
			goto err_disconnect;
		}
		priv->phylink_config.dev = &netdev->dev;
		priv->phylink_config.type = PHYLINK_NETDEV;
		priv->phylink_config.mac_capabilities = MAC_10 | MAC_100 |
							MAC_1000FD;
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  priv->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_GMII,
			  priv->phylink_config.supported_interfaces);
		priv->phylink = phylink_create(&priv->phylink_config,
					       of_fwnode_handle(np), interface,
					       &zx_phylink_mac_ops);
		if (IS_ERR(priv->phylink)) {
			ret = dev_err_probe(dev, PTR_ERR(priv->phylink),
					    "port %u: phylink\n", port);
			priv->phylink = NULL;
			of_node_put(np);
			goto err_disconnect;
		}
		ret = phylink_of_phy_connect(priv->phylink, np, 0);
		if (ret) {
			dev_err_probe(dev, ret, "port %u: PHY connect failed\n", port);
			of_node_put(np);
			goto err_disconnect;
		}
	}

	/* The MAC counters start from zero, whatever the registers hold */
	mutex_lock(&adapter->mib_lock);
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct zx_eth_priv *priv;

		if (!adapter->ports[i])
			continue;
		priv = netdev_priv(adapter->ports[i]);
		zx_mib_update(priv);
		memset(priv->mib, 0, sizeof(priv->mib));
	}
	mutex_unlock(&adapter->mib_lock);

	for (i = 0; i < ZX_NUM_PORTS; i++)
		if (adapter->ports[i])
			break;
	if (i == ZX_NUM_PORTS) {
		ret = dev_err_probe(dev, -ENODEV, "no usable port\n");
		goto err_disconnect;
	}

	mutex_lock(&adapter->ppe_lock);
	zx_update_mac_tables(adapter);
	mutex_unlock(&adapter->ppe_lock);

	/* One NAPI context serves all ports */
	netif_napi_add(adapter->ports[i], &adapter->napi, zx_eth_poll);
	hrtimer_setup(&adapter->tx_timer, zx_eth_tx_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	/* zx_hw_init() masked the interrupt, a port's open enables it */
	ret = devm_request_irq(dev, adapter->irq, zx_eth_isr, 0, dev_name(dev),
			       adapter);
	if (ret)
		goto err_napi;

	/* Start DMA only after every port, NAPI and the interrupt are ready. */
	zx_dma_enable(adapter, true);

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		if (!adapter->ports[i])
			continue;
		ret = register_netdev(adapter->ports[i]);
		if (ret) {
			dev_err(dev, "port %d: register_netdev failed: %d\n", i, ret);
			while (--i >= 0)
				if (adapter->ports[i])
					unregister_netdev(adapter->ports[i]);
			goto err_irq;
		}
	}

	platform_set_drvdata(pdev, adapter);

	zx_sw_init(adapter);
	schedule_delayed_work(&adapter->mib_work, ZX_MIB_INTERVAL);

	zx_eth_debugfs_init(adapter);

	mutex_lock(&zx_adapter_list_lock);
	list_add_tail(&adapter->list, &zx_adapter_list);
	mutex_unlock(&zx_adapter_list_lock);
	return 0;

err_irq:
	writel(~0, adapter->base + ZX_TM_INT_MASK);
	devm_free_irq(dev, adapter->irq, adapter);
err_napi:
	netif_napi_del(&adapter->napi);
err_disconnect:
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct zx_eth_priv *priv;

		if (!adapter->ports[i])
			continue;
		priv = netdev_priv(adapter->ports[i]);
		if (priv->phylink) {
			phylink_disconnect_phy(priv->phylink);
			phylink_destroy(priv->phylink);
		}
	}
err_dma:
	/* A failed or deferred probe must leave no DMA targeting this arena. */
	zx_pkt_pool_free(dev, adapter);
	zx_hw_reset(adapter);
	dma_free_coherent(dev, ZX_DMA_ARENA_SIZE, adapter->buffer_pool,
			  adapter->buf_pool_dma);
err_accel:
	zx_accel_exit(adapter);
err_hashtable:
	rhashtable_destroy(&adapter->ppe_flows);
	return ret;
}

/* ---- module glue --------------------------------------------------- */

/*
 * The switch writes received frames, and frames of offloaded flows, into
 * the buffer pool without the CPU.  Stop it before a reboot or kexec: after
 * kexec, that memory belongs to the next kernel.
 */
static void zx_eth_shutdown(struct platform_device *pdev)
{
	struct zx_eth_adapter *adapter = platform_get_drvdata(pdev);
	int i;

	if (!adapter)
		return;

	rtnl_lock();
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		if (!adapter->ports[i])
			continue;
		netif_device_detach(adapter->ports[i]);
		dev_close(adapter->ports[i]);
	}
	rtnl_unlock();
	cancel_work_sync(&adapter->sw_work);
	cancel_delayed_work_sync(&adapter->mib_work);

	zx_hw_reset(adapter);
}

static const struct of_device_id zx_eth_dt_ids[] = {
	{ .compatible = "zte,zx279128s-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx_eth_dt_ids);

static struct platform_driver zx_eth_driver = {
	.probe  = zx_eth_probe,
	.shutdown = zx_eth_shutdown,
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = zx_eth_dt_ids,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(zx_eth_driver);

MODULE_AUTHOR("Navid Ghahremani <ghahramani.navid@gmail.com>");
MODULE_DESCRIPTION("ZTE zx279128s Ethernet switch and DMA driver");
MODULE_LICENSE("GPL");
