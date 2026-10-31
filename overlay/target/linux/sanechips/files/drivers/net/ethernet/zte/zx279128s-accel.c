// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s Ethernet: the acceleration-path framework
 *
 * 每个协议族（IPv4 / IPv6）各自选一条加速路径：
 *
 *   stack  纯标准栈，驱动什么都不做
 *   ffe    驱动内零拷贝快路（docs/ffe-fastpath-design-2026-11-01.md）
 *   hw     PPE 硬件流表，只有 IPv4 有；驱动侧只表示"放行 PPE 接管"
 *   zc     既有零拷贝（zx279128s-main.c 的 zx_zc_* 一节）
 *
 * zc 的底层状态是全局的（一套池页、一个 adapter->zc_rx、一个 GRO 位），但它是
 * **按族生效**的：模式为 zc 的族在 accel->zc_mask 里占一位，只有这些族的帧才走
 * 零拷贝（zx279128s-main.c 的 zx_eth_rx()）。不变式：zc_rx 为真时 zc_mask 必然
 * 非零、且等于"两族里模式为 zc 的那些族的集合"——本文件是这两个字段唯一的
 * 作者，别的地方（开机脚本、debugfs 的 zc 兼容口、模块参数）都只能经
 * zx_accel_set() 或 probe 里的等价初始化来改。
 *
 * P0 阶段这里只搭框架：流表建起来但永远为空（没有插入路径），ffe 模式不产生
 * 任何转发——数据面只多一条"quiesce / 模式是不是 ffe"的判断并记一次 miss。
 * 切换协议 S0-S7、计数面、自动降级判据都按设计实现，P1 加条目与老化，P2 在
 * zx_ffe_rx() 里做真转发。
 *
 * 锁与顺序约定（很重要）：
 *   - accel->lock 串行化模式切换。持有它时**可以**调 zx_zc_set()；
 *     zx_zc_set() 自己会 rtnl_lock()，所以本文件在调它之前不得先持 rtnl。
 *   - ndo_open / ndo_stop 持着 rtnl 调 zx_accel_on_open()/on_close()：这两个
 *     钩子里不许再取 accel->lock，否则与上面"先 accel->lock 再 rtnl"构成 ABBA。
 */

/*
 * asm/byteorder.h 是本架构的端序实现头（ARM：linux/byteorder/little_endian.h
 * + byteorder/generic.h），htons() 和底层的 __cpu_to_be16() 都由它带进来；
 * 只包含 linux/byteorder/generic.h 会缺 __cpu_to_le16 那些底层宏。
 */
#include <linux/types.h>
#include <asm/byteorder.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/uaccess.h>

#include "zx279128s-eth.h"

/* 流表容量：PPE 硬件是 127 条，软件表给 1024 条（设计 §4.1） */
#define ZX_FFE_FLOWS_MAX	1024
#define ZX_FFE_NELEM_HINT	256

/* 老化/降级定时器的节拍，以及 S3 排空发送环的超时（设计 §1.4） */
#define ZX_FFE_TICK_MS		1000
#define ZX_FFE_DRAIN_MS		100

/*
 * 自动降级的阈值：drop > 0.1% 或 fallback > 50%，持续 10 s 就回 stack
 * （设计 §9-4）。drop 是 drop_noport + drop_local。
 */
#define ZX_FFE_FB_DROP_PERMILLE		1
#define ZX_FFE_FB_FALLBACK_PERMILLE	500
#define ZX_FFE_FB_WINDOW_MS		10000

/*
 * 流表键（设计 §4.1）：family 在键里，所以 v4/v6 共用一张表。P0 只把它当作
 * rhashtable 的键布局，没有任何插入；P1 的判据链用同一个结构查表。
 */
struct zx_ffe_key {
	union {
		__be32	v4[2];		/* saddr, daddr */
		u8	v6[32];
	};
	__be16	sport;
	__be16	dport;
	u8	proto;
	u8	ipport;
	u8	family;
	u8	pad;
	u16	vid;			/* 0xffff = 任意 */
};

/* P1 往这里加 dmac/mtu/eport/flags/hits/bytes/lastuse（设计 §4.1） */
struct zx_ffe_flow {
	struct rhash_head	node;
	struct zx_ffe_key	key;
};

/* 与 zx_ppe_ht_params（zx279128s-main.c）同一套参数风格 */
static const struct rhashtable_params zx_ffe_ht_params = {
	.head_offset		= offsetof(struct zx_ffe_flow, node),
	.key_offset		= offsetof(struct zx_ffe_flow, key),
	.key_len		= sizeof(struct zx_ffe_key),
	.nelem_hint		= ZX_FFE_NELEM_HINT,
	.max_size		= ZX_FFE_FLOWS_MAX,
	.automatic_shrinking	= true,
};

/* 某个族模式字段的地址（切换时只写这一个字节） */
static u8 *zx_accel_mode_slot(struct zx_accel *accel, int family)
{
	return family ? &accel->v6_mode : &accel->v4_mode;
}

const char *zx_accel_mode_name(u8 mode)
{
	switch (mode) {
	case ZX_ACC_STACK:
		return "stack";
	case ZX_ACC_FFE:
		return "ffe";
	case ZX_ACC_HW:
		return "hw";
	case ZX_ACC_ZC:
		return "zc";
	}
	return "?";
}

/* 以太网类型 → 协议族：0=IPv4，1=IPv6，-1=快路不管（ARP/PPPoE/VLAN/…） */
int zx_eth_family(__be16 proto)
{
	if (proto == htons(ETH_P_IP))
		return 0;
	if (proto == htons(ETH_P_IPV6))
		return 1;
	return -1;
}

/*
 * P0 唯一的计数入口：ffe 模式下回栈的帧。查过表但没命中，所以 lookup 与
 * miss 各加一（不变式 lookup == hit + miss，hit == fwd + fallback + drop）。
 * 用普通 64 位累加：读侧最多看到一个撕裂值，P1 再换成 per-CPU/u64_stats。
 */
void zx_accel_count_miss(struct zx_eth_adapter *adapter, int family)
{
	if (family < 0 || family > 1)
		return;
	adapter->accel.cnt[family].lookup++;
	adapter->accel.cnt[family].miss++;
}

/*
 * 清掉某一族的流表项（S4，切表=全清）。P0 没有任何插入路径，表永远是空的，
 * 所以这里没有可删的东西；P1 加插入后在这里按 family 走
 * rhashtable_walk + rhashtable_remove_fast（调用者已持 accel->lock）。
 */
static void zx_ffe_flush(struct zx_eth_adapter *adapter, int family)
{
	(void)adapter;
	(void)family;
}

/*
 * S2：把数据面停到"一个包都不会被处理"。
 *
 * 先停所有端口的发送队列（netif_tx_disable() 会等正在跑的 ndo_start_xmit
 * 退出），再停 NAPI 并同步中断处理函数。
 *
 * napi_disable() 只能和 napi_enable() 配对：对已经停掉的 NAPI 再 disable 会
 * 一直等 SCHED 位，对还开着的 NAPI 再 enable 会撞 BUG_ON。所以这里用
 * open_count 判断 NAPI 处于什么状态——端口全关时 NAPI 是停的（zx_eth_open
 * 在 0→1 时 enable、zx_eth_stop 在 →0 时 disable）。
 *
 * P0 已知限制：模式切换与 ifup/ifdown 并发时这个判断会有窗口（调试口本来就
 * 是管理动作，正常不会同时做）；彻底解决要把 open/stop 与切换放进同一把锁，
 * 而切换路径不能先持 rtnl（zx_zc_set 内部要拿），留到 P1。
 */
static void zx_accel_quiesce_enter(struct zx_eth_adapter *adapter)
{
	int i;

	for (i = 0; i < ZX_NUM_PORTS; i++)
		if (adapter->ports[i])
			netif_tx_disable(adapter->ports[i]);

	if (READ_ONCE(adapter->open_count) > 0) {
		/* 切换期间不要让发送定时器把 NAPI 再排一次 */
		hrtimer_cancel(&adapter->tx_timer);
		napi_disable(&adapter->napi);
	}
	synchronize_irq(adapter->irq);
}

/* S6：恢复数据面。补一次 poll，免得切换期间到队的帧等不到中断。 */
static void zx_accel_quiesce_leave(struct zx_eth_adapter *adapter)
{
	int i;

	if (READ_ONCE(adapter->open_count) > 0) {
		napi_enable(&adapter->napi);
		/* 切换期间 ISR 可能把 RX 中断屏蔽掉了（见 zx_eth_isr），
		 * 这里必须放回来，否则收包再也不会被叫醒。
		 */
		writel(~ZX_TM_INT_RX, adapter->base + ZX_TM_INT_MASK);
		napi_schedule(&adapter->napi);
	}

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *nd = adapter->ports[i];

		if (nd && netif_running(nd))
			netif_tx_wake_all_queues(nd);
	}
}

/*
 * 切换协议 S0-S7（设计 §1.4）：要么整个切换完成，要么回滚到原模式，绝无
 * 半切换。family 0 = IPv4，1 = IPv6。返回 0 表示已是目标模式，-EINVAL 表示
 * 参数/该族不支持，-EBUSY 表示能力检查（ffe 与 PPE 冲突、池不可用）或排空
 * 失败（这两种情况模式未变）。
 */
int zx_accel_set(struct zx_eth_adapter *adapter, int family, u8 new_mode)
{
	struct zx_accel *accel = &adapter->accel;
	unsigned long deadline;
	u32 pending, drained;
	u64 t0;
	u8 *slot;
	u8 old_mode;
	u8 mask;
	bool bound;
	int ret = 0;

	if (family != 0 && family != 1)
		return -EINVAL;
	/* hw 只有 IPv4 有；其余超出枚举的值一律拒绝 */
	if (new_mode > ZX_ACC_ZC || (new_mode == ZX_ACC_HW && family != 0))
		return -EINVAL;

	/*
	 * 能力检查要读 TC 的 flow_block 列表，绑定时 TC 核持有 rtnl，所以这里
	 * 短占 rtnl 取一个快照；**不能**带着 rtnl 往下走，S4 的 zx_zc_set()
	 * 自己要拿 rtnl。
	 */
	rtnl_lock();
	bound = zx_ppe_has_bound_blocks(adapter);
	rtnl_unlock();

	slot = zx_accel_mode_slot(accel, family);

	mutex_lock(&accel->lock);				/* S0 */
	old_mode = READ_ONCE(*slot);
	if (old_mode == new_mode)
		goto unlock;

	/* S1 能力检查：目标状态必须真的能用 */
	switch (new_mode) {
	case ZX_ACC_HW:
		/*
		 * hw 只是"放行 PPE 接管"，不要求 flowtable 已经绑上：默认值
		 * accel_v4=hw 在开机时 nft flowtable 往往还没配置，这里拒绝会
		 * 让默认值写入失败。没绑定时照样允许，只提示一句；读侧靠
		 * ppe_bound=0 表示"现在没有东西在喂硬件"（设计 §1.1）。
		 */
		if (!bound)
			dev_info(adapter->dev,
				 "accel: IPv%d hw set, but no flowtable is bound to this adapter yet (ppe_bound=0)\n",
				 family ? 6 : 4);
		break;
	case ZX_ACC_FFE:
		if (bound) {
			/* 拒绝会随每次 echo 触发，限速：200 次实验不该冲掉 dmesg */
			dev_info_ratelimited(adapter->dev,
				 "accel: refusing IPv%d ffe: the PPE owns the flows (remove the flowtable or set flow_offloading_hw=0)\n",
				 family ? 6 : 4);
			ret = -EBUSY;
			goto unlock;
		}
		if (!adapter->pkt_pool) {
			dev_info_ratelimited(adapter->dev,
				 "accel: refusing IPv%d ffe: the packet pool is not available\n",
				 family ? 6 : 4);
			ret = -EBUSY;
			goto unlock;
		}
		break;
	case ZX_ACC_ZC:
		/*
		 * zc 与 hw 在同一族内互斥（设计 §5）：PPE 接管这一族时再把同一个族
		 * 的帧交给零拷贝，等于两条路径改写同一批 bp，所有权不可证明。v6 没有
		 * hw 路径，所以只检查 IPv4。
		 */
		if (bound && family == 0) {
			dev_info_ratelimited(adapter->dev,
				 "accel: refusing IPv%d zc: the PPE owns the flows (remove the flowtable or set flow_offloading_hw=0)\n",
				 family ? 6 : 4);
			ret = -EBUSY;
			goto unlock;
		}
		break;
	default:
		break;
	}

	accel->sw_in++;
	t0 = ktime_get_ns();

	/* S2 停数据面：从此没有新的 RX 处理，也没有新的发送提交 */
	WRITE_ONCE(accel->quiesce, true);
	zx_accel_quiesce_enter(adapter);

	/* S3 排空已提交的发送（zx_tx_reclaim 在 quiesce 期间不会唤醒队列） */
	pending = adapter->tx_pending;
	deadline = jiffies + msecs_to_jiffies(ZX_FFE_DRAIN_MS);
	while (adapter->tx_pending) {
		spin_lock_bh(&adapter->tx_lock);
		zx_tx_reclaim(adapter);
		spin_unlock_bh(&adapter->tx_lock);
		if (!adapter->tx_pending)
			break;
		if (time_after_eq(jiffies, deadline))
			break;
		usleep_range(50, 200);
	}
	drained = pending - adapter->tx_pending;
	accel->drain_frames = drained;
	accel->drain_ms = div_u64(ktime_get_ns() - t0, NSEC_PER_MSEC);

	if (adapter->tx_pending) {
		/* 排不空就回滚：数据面照旧，模式一个字节都不动 */
		dev_info(adapter->dev,
			 "accel: IPv%d %s->%s aborted: tx_pending=%u did not drain in %u ms\n",
			 family ? 6 : 4, zx_accel_mode_name(old_mode),
			 zx_accel_mode_name(new_mode), adapter->tx_pending,
			 ZX_FFE_DRAIN_MS);
		zx_accel_quiesce_leave(adapter);
		WRITE_ONCE(accel->quiesce, false);
		ret = -EBUSY;
		goto unlock;
	}

	/* S4 清表与隔离：切表=全清；zc 的 mask 位按目标模式维护。
	 *
	 * mask 与 zc_rx 的先后顺序是不变式要求的：先改本族那一位，位清零后如果
	 * 没有任何族再用 zc，才真正关掉全局开关（zc_rx 为真时 mask 必然非零）。
	 * 另一族还在 zc 里时不能关：那会把它的收包路径换掉，而它的模式仍写着 zc。
	 */
	zx_ffe_flush(adapter, family);
	mask = READ_ONCE(accel->zc_mask);
	if (new_mode == ZX_ACC_ZC)
		mask |= BIT(family);
	else
		mask &= ~BIT(family);
	WRITE_ONCE(accel->zc_mask, mask);
	if (!mask && READ_ONCE(adapter->zc_rx))
		zx_zc_set(adapter, false);

	/* S5 切点：单字节写，数据面只会看到旧模式或新模式 */
	WRITE_ONCE(*slot, new_mode);

	/* S6 目标 zc 打开零拷贝，然后恢复数据面。全局开关只在 0->非0 时开一次
	 * （mask 已置位，所以 zc_rx 打开时不变式立刻成立），另一族本来就在 zc
	 * 里时不用再调一遍 zx_zc_set()。
	 */
	if (mask && !READ_ONCE(adapter->zc_rx))
		zx_zc_set(adapter, true);
	zx_accel_quiesce_leave(adapter);

	/* S7 */
	WRITE_ONCE(accel->quiesce, false);
	accel->sw_out++;
	dev_info(adapter->dev,
		 "accel: IPv%d %s -> %s (%llu ms, %u frames drained)\n",
		 family ? 6 : 4, zx_accel_mode_name(old_mode),
		 zx_accel_mode_name(new_mode), accel->drain_ms, drained);

unlock:
	mutex_unlock(&accel->lock);				/* S7 */
	return ret;
}

/*
 * 自动降级的判据：某一族在 ffe 模式下 drop 超过 0.1% 或者 fallback 超过
 * 50%，并且持续 10 s，就把它切回 stack 并返回 true（设计 §9-4）。
 * 由 zx_accel_age_work() 每秒调一次；P0 没有转发，这些计数不会增长，所以
 * 它总是返回 false——接口先立起来，P2 真转发落地后自然生效。
 */
static bool zx_accel_degraded(const struct zx_accel *accel, int family)
{
	const struct zx_ffe_cnt *c = &accel->cnt[family];
	u64 lookup = READ_ONCE(c->lookup);
	u64 drop;

	if (!lookup)
		return false;
	drop = READ_ONCE(c->drop_noport) + READ_ONCE(c->drop_local);
	if (drop * 1000 > lookup * ZX_FFE_FB_DROP_PERMILLE)
		return true;
	if (READ_ONCE(c->fallback) * 1000 >
	    lookup * ZX_FFE_FB_FALLBACK_PERMILLE)
		return true;
	return false;
}

bool zx_accel_autofallback(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;
	bool degraded = false;
	int family;

	if (!READ_ONCE(accel->autofallback))
		return false;

	for (family = 0; family < 2; family++) {
		if (zx_accel_cur_mode(accel, family) != ZX_ACC_FFE ||
		    !zx_accel_degraded(accel, family)) {
			accel->fb_since[family] = 0;
			continue;
		}
		if (!accel->fb_since[family]) {
			accel->fb_since[family] = jiffies;
			continue;
		}
		if (time_before(jiffies, accel->fb_since[family] +
				msecs_to_jiffies(ZX_FFE_FB_WINDOW_MS)))
			continue;

		dev_info(adapter->dev,
			 "accel: IPv%d fell back to stack: %llu drop, %llu fallback of %llu lookups\n",
			 family ? 6 : 4,
			 READ_ONCE(accel->cnt[family].drop_noport) +
				 READ_ONCE(accel->cnt[family].drop_local),
			 READ_ONCE(accel->cnt[family].fallback),
			 READ_ONCE(accel->cnt[family].lookup));
		accel->fb_since[family] = 0;
		if (!zx_accel_set(adapter, family, ZX_ACC_STACK))
			degraded = true;
	}
	return degraded;
}

/* 每秒一拍：P0 只有自动降级判据；P1 在这里加流表老化（设计 §4.1）。 */
static void zx_accel_age_work(struct work_struct *work)
{
	struct zx_eth_adapter *adapter =
		container_of(to_delayed_work(work), struct zx_eth_adapter,
			     accel.age);

	zx_accel_autofallback(adapter);
	schedule_delayed_work(&adapter->accel.age,
			      msecs_to_jiffies(ZX_FFE_TICK_MS));
}

int zx_accel_init(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;
	int ret;

	mutex_init(&accel->lock);
	accel->v4_mode = ZX_ACC_STACK;
	accel->v6_mode = ZX_ACC_STACK;
	accel->zc_mask = 0;
	WRITE_ONCE(accel->quiesce, false);
	memset(accel->cnt, 0, sizeof(accel->cnt));
	accel->sw_in = 0;
	accel->sw_out = 0;
	accel->drain_ms = 0;
	accel->drain_frames = 0;
	accel->autofallback = true;
	memset(accel->fb_since, 0, sizeof(accel->fb_since));
	INIT_DELAYED_WORK(&accel->age, zx_accel_age_work);

	ret = rhashtable_init(&accel->flows, &zx_ffe_ht_params);
	if (ret) {
		dev_err(adapter->dev, "accel: rhashtable_init failed: %d\n",
			ret);
		return ret;
	}

	schedule_delayed_work(&accel->age, msecs_to_jiffies(ZX_FFE_TICK_MS));
	return 0;
}

void zx_accel_exit(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;

	cancel_delayed_work_sync(&accel->age);
	/* P0 的表是空的；P1 有插入后换成按条目释放的回调 */
	rhashtable_free_and_destroy(&accel->flows, NULL, NULL);
}

/*
 * ndo_open / ndo_stop 的钩子（设计 §1.4 最后一段）。调用者持 rtnl，所以这里
 * 不许取 accel->lock（锁序见文件头）。
 *
 * P0：流表是空的。P1 在 close 里按端口 flush（设计 §4.2），在 open 里重建
 * 本机地址集合。
 */
void zx_accel_on_open(struct zx_eth_adapter *adapter)
{
	(void)adapter;
}

void zx_accel_on_close(struct zx_eth_adapter *adapter)
{
	(void)adapter;
}

/* ---- debugfs ------------------------------------------------------- */

static void zx_accel_show(struct seq_file *s, int family)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;
	const struct zx_ffe_cnt *c = &accel->cnt[family];
	bool bound;

	/* 读 flow_block 列表要持 rtnl（TC 绑定/解绑时持着它） */
	rtnl_lock();
	bound = zx_ppe_has_bound_blocks(adapter);
	rtnl_unlock();

	seq_printf(s, "%s\n",
		   zx_accel_mode_name(zx_accel_cur_mode(accel, family)));
	seq_printf(s, "lookup=%llu hit=%llu miss=%llu fwd=%llu fallback=%llu fallback_qfull=%llu drop_noport=%llu drop_local=%llu csum_err=%llu ttl_err=%llu bytes=%llu qlen_peak=%u\n",
		   c->lookup, c->hit, c->miss, c->fwd, c->fallback,
		   c->fallback_qfull, c->drop_noport, c->drop_local,
		   c->csum_err, c->ttl_err, c->bytes, c->qlen_peak);
	/*
	 * 第 3 行是给人和脚本看的状态行。hint 是"下一步该做什么"的机器可读
	 * 提示：PPE 绑着 flowtable 时切 ffe/zc 会被 S1 拒绝，用户态得先关掉
	 * 硬件 offload（uci firewall.@defaults[0].flow_offloading_hw=0 + reload），
	 * 所以给一个不含空格的单 token，不会被按空格切字段的读侧打散。
	 */
	seq_printf(s, "ppe_bound=%u sw_in=%llu sw_out=%llu drain_ms=%llu drain_frames=%llu autofallback=%u zc_mask=%u hint=%s\n",
		   bound ? 1u : 0u, accel->sw_in,
		   accel->sw_out, accel->drain_ms, accel->drain_frames,
		   READ_ONCE(accel->autofallback) ? 1u : 0u,
		   READ_ONCE(accel->zc_mask),
		   bound ? "disable_flow_offloading_hw_to_use_ffe" : "none");
}

static int zx_accel_v4_show(struct seq_file *s, void *data)
{
	zx_accel_show(s, 0);
	return 0;
}

static int zx_accel_v6_show(struct seq_file *s, void *data)
{
	zx_accel_show(s, 1);
	return 0;
}

static int zx_accel_v4_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_v4_show, inode->i_private);
}

static int zx_accel_v6_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_v6_show, inode->i_private);
}

/*
 * 写 stack|ffe|hw|zc（v6 没有 hw）。切换是同步的：返回时要么已经是新模式，
 * 要么什么都没变（-EINVAL 参数不支持，-EBUSY 能力检查或排空失败）。
 */
static ssize_t zx_accel_write(struct file *file, int family,
			      const char __user *ubuf, size_t count,
			      loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct zx_eth_adapter *adapter = s->private;
	char buf[16];
	char *cmd;
	u8 mode;
	int ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	cmd = strim(buf);

	if (!strcmp(cmd, "stack"))
		mode = ZX_ACC_STACK;
	else if (!strcmp(cmd, "ffe"))
		mode = ZX_ACC_FFE;
	else if (!strcmp(cmd, "hw"))
		mode = ZX_ACC_HW;
	else if (!strcmp(cmd, "zc"))
		mode = ZX_ACC_ZC;
	else
		return -EINVAL;

	ret = zx_accel_set(adapter, family, mode);
	if (ret)
		return ret;
	return count;
}

static ssize_t zx_accel_v4_store(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	return zx_accel_write(file, 0, ubuf, count, ppos);
}

static ssize_t zx_accel_v6_store(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	return zx_accel_write(file, 1, ubuf, count, ppos);
}

const struct file_operations zx_accel_v4_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_v4_open,
	.read		= seq_read,
	.write		= zx_accel_v4_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

const struct file_operations zx_accel_v6_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_v6_open,
	.read		= seq_read,
	.write		= zx_accel_v6_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* P0：没有插入路径，表永远为空；P1 在这里按 family 遍历打印条目（设计 §1.3） */
static int zx_accel_flows_show(struct seq_file *s, void *data)
{
	(void)data;
	seq_puts(s, "# ffe flows: none (P0 has no insert path)\n");
	return 0;
}

static int zx_accel_flows_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_flows_show, inode->i_private);
}

const struct file_operations zx_accel_flows_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_flows_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int zx_accel_autofallback_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;

	seq_printf(s, "%u\n",
		   READ_ONCE(adapter->accel.autofallback) ? 1u : 0u);
	return 0;
}

static int zx_accel_autofallback_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_autofallback_show, inode->i_private);
}

static ssize_t zx_accel_autofallback_store(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct zx_eth_adapter *adapter = s->private;
	char buf[8];
	char *cmd;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	cmd = strim(buf);

	if (!strcmp(cmd, "1") || !strcmp(cmd, "on"))
		WRITE_ONCE(adapter->accel.autofallback, true);
	else if (!strcmp(cmd, "0") || !strcmp(cmd, "off"))
		WRITE_ONCE(adapter->accel.autofallback, false);
	else
		return -EINVAL;

	dev_info(adapter->dev, "accel: autofallback %s\n",
		 READ_ONCE(adapter->accel.autofallback) ? "enabled" : "disabled");
	return count;
}

const struct file_operations zx_accel_autofallback_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_autofallback_open,
	.read		= seq_read,
	.write		= zx_accel_autofallback_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};
