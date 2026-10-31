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
 * P0 搭框架；P1 做 shadow 判定（只判不发，静态流表 + 本机地址集合 + 老化）；
 * P2 起 zx_ffe_rx() 真转发：判据链全部通过后改写 L2 头与 TTL，把**同一个
 * bp** 写进发送描述符交给硬件（zx_tx_submit_bp()，与 zx_eth_xmit() 共用），
 * 帧不再进 zc/skb/协议栈。P2 不做 NAT（带 NAT 语义的条目一律回栈）。
 *
 * P4 加 IPv6：判据链按 family 分派（zx_ffe_v4_check() / zx_ffe_v6_check()），
 * 提交段只有一份（zx_ffe_forward()），改写按族只动该动的字节——v6 只改
 * h_dest/h_source/hop_limit，**不碰任何校验和**（v6 固定头里没有校验和，
 * L4 伪首部也不含 hop_limit）。本机 IPv6 地址集合由 inet6addr 通知链维护，
 * 与 IPv4 那条并列。v6 没有 NAT66，带 NAT 语义的条目照样回栈。
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
#include <asm/checksum.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/net_namespace.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>
#include <linux/rhashtable.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/uaccess.h>
#include <net/addrconf.h>
#include <net/if_inet6.h>
#include <net/ip.h>

#include "zx279128s-eth.h"

/* 流表容量：PPE 硬件是 127 条，软件表给 1024 条（设计 §4.1） */
#define ZX_FFE_FLOWS_MAX	1024
#define ZX_FFE_NELEM_HINT	256

/* 老化/降级定时器的节拍，以及 S3 排空发送环的超时（设计 §1.4） */
#define ZX_FFE_TICK_MS		1000
#define ZX_FFE_DRAIN_MS		100

/*
 * 流表老化时间（秒）：老化 work 每 1 s 走一遍 flow_list，某个条目超过它没有
 * 再被快路命中的帧刷新就删除（设计 §4.1）。P1 的条目都是 ffe_add 下发
 * 的静态条目，照样老化，方便把"老化删掉了"和"判定错了"区分开。0 = 不老化。
 */
static unsigned int ffe_aging = 300;
module_param(ffe_aging, uint, 0644);
MODULE_PARM_DESC(ffe_aging, "Age out FFE flows after this many seconds (0 = never, default: 300)");

/*
 * 自动降级的阈值：drop > 0.1% 或 fallback > 50%，持续 10 s 就回 stack
 * （设计 §9-4）。drop 是 drop_noport + drop_local。
 */
#define ZX_FFE_FB_DROP_PERMILLE		1
#define ZX_FFE_FB_FALLBACK_PERMILLE	500
#define ZX_FFE_FB_WINDOW_MS		10000

/*
 * 快路能转发的最长帧：1514 = 14 B 以太头 + 1500 B MTU。1515..1536 只可能是
 * 带 VLAN 的帧，而 VLAN 帧在判据 #4（h_proto != 0x0800）就回栈了；这条上限
 * 是给"驱动没有 SG/TSO，zx_eth_xmit() 对 >1536 直接丢"兜底的第二道闸。
 */
#define ZX_FFE_MAX_FRAME		1514

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

/* P1 的条目全部来自 ffe_add（静态下发），老化照样生效（P1-TASK §3） */
#define ZX_FFE_FLOW_STATIC	BIT(0)
/*
 * 条目带 NAT 语义：P2 只做"纯路由"改写，源地址/端口一律不动（设计 §9-1
 * 决策 A）。P3 才会给条目写这个位；在那之前命中它的帧一律回栈——用没改写
 * 源地址的帧冒充 NAT 输出，比不转发更糟（对端收到一个源地址不属于链路的包）。
 */
#define ZX_FFE_FLOW_NAT		BIT(1)

struct zx_ffe_flow {
	struct rhash_head	node;
	struct list_head	list;		/* flow_list：老化与 ffe_flows 遍历 */
	struct rcu_head		rcu;
	struct zx_ffe_key	key;
	u8			dmac[ETH_ALEN];	/* 出口下一跳 MAC（P2 改写用） */
	u8			smac[ETH_ALEN];	/* 出口端口 MAC 快照（P2 改写用） */
	u8			eport;
	u8			flags;
	u16			mtu;		/* 出口 MTU 快照（add 时取），0 = 未知 */
	u64			hits;		/* 真正转发出去的帧数（P2 起 == fwd） */
	u64			bytes;
	unsigned long		lastuse;	/* 最近一次命中（转发或环满回栈） */
};

/*
 * 本机地址集合的条目。判据 #2 查 MAC、#13 查 IPv4、v6 链的 #10 查 IPv6；dev
 * 用来在地址变化 / 接口注销时把旧值删掉。netdev/inetaddr/inet6addr 三条通知
 * 链都是原子的，所以全部操作在 accel->local_lock 下，分配用 GFP_ATOMIC，删除
 * 用 kfree_rcu（数据面在 RCU 只读段里读）。
 */
struct zx_ffe_lmac {
	struct rhash_head	node;
	struct list_head	list;
	struct rcu_head		rcu;
	struct net_device	*dev;
	u8			mac[ETH_ALEN];
};

struct zx_ffe_lip {
	struct rhash_head	node;
	struct list_head	list;
	struct rcu_head		rcu;
	struct net_device	*dev;
	__be32			addr;
};

/* IPv6 版：键是 16 B 的 struct in6_addr，与 local_ips 完全并列的第二张表 */
struct zx_ffe_lip6 {
	struct rhash_head	node;
	struct list_head	list;
	struct rcu_head		rcu;
	struct net_device	*dev;
	struct in6_addr		addr;
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

static const struct rhashtable_params zx_ffe_lmac_params = {
	.head_offset		= offsetof(struct zx_ffe_lmac, node),
	.key_offset		= offsetof(struct zx_ffe_lmac, mac),
	.key_len		= ETH_ALEN,
	.nelem_hint		= 8,
	.max_size		= 64,
	.automatic_shrinking	= true,
};

static const struct rhashtable_params zx_ffe_lip_params = {
	.head_offset		= offsetof(struct zx_ffe_lip, node),
	.key_offset		= offsetof(struct zx_ffe_lip, addr),
	.key_len		= sizeof(__be32),
	.nelem_hint		= 8,
	.max_size		= 64,
	.automatic_shrinking	= true,
};

static const struct rhashtable_params zx_ffe_lip6_params = {
	.head_offset		= offsetof(struct zx_ffe_lip6, node),
	.key_offset		= offsetof(struct zx_ffe_lip6, addr),
	.key_len		= sizeof(struct in6_addr),
	.nelem_hint		= 16,
	.max_size		= 128,
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

/* ---- 静态流表（P1-TASK §2/§3）-------------------------------------- */

static void zx_ffe_free_flow(void *ptr, void *arg)
{
	(void)arg;
	kfree(ptr);
}

static void zx_ffe_free_lmac(void *ptr, void *arg)
{
	(void)arg;
	kfree(ptr);
}

static void zx_ffe_free_lip(void *ptr, void *arg)
{
	(void)arg;
	kfree(ptr);
}

static void zx_ffe_free_lip6(void *ptr, void *arg)
{
	(void)arg;
	kfree(ptr);
}

/*
 * 删除一个条目（调用者已持 accel->lock）。flow_list（老化/遍历）与 rhashtable
 * （数据面查表）必须同时改；kfree_rcu 让还在 RCU 只读段里的查表者拿到一个
 * 仍然有效的条目。
 */
static void __zx_ffe_flow_del(struct zx_eth_adapter *adapter,
			      struct zx_ffe_flow *flow)
{
	struct zx_accel *accel = &adapter->accel;

	list_del_rcu(&flow->list);
	rhashtable_remove_fast(&accel->flows, &flow->node, zx_ffe_ht_params);
	accel->n_flows--;
	kfree_rcu(flow, rcu);
}

/* family：0=IPv4，1=IPv6，-1=全部。调用者已持 accel->lock。 */
static u32 __zx_ffe_flush(struct zx_eth_adapter *adapter, int family)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *flow, *tmp;
	u8 key_family = family == 0 ? 4 : 6;
	u32 n = 0;

	list_for_each_entry_safe(flow, tmp, &accel->flow_list, list) {
		if (family >= 0 && flow->key.family != key_family)
			continue;
		__zx_ffe_flow_del(adapter, flow);
		n++;
	}
	return n;
}

/* ---- 本机地址集合（判据 #2 / #13 / v6 链 #10）---------------------- */

/*
 * 哪些接口的 dev_addr 要进"本机 MAC 集合"：
 *   - 本驱动的交换机口（lanX / wan）：地址来自 nvmem，探针里按 DT reg 建好；
 *   - 桥接口（br-lan）：桥下路由帧的 h_dest 就是它的地址，5 个口里查不到；
 *   - 桥成员口（例如 enslave 到 br-lan 的 wlan）：它自己的地址也会出现；
 *   - 无线口（phy0-ap0），哪怕还没上桥。
 * 全用通用接口属性判断，不按名字硬编码。
 */
static bool zx_ffe_should_track(const struct net_device *dev)
{
	if (dev->flags & IFF_LOOPBACK)
		return false;
	if (zx_eth_is_port(dev))
		return true;
	if (netif_is_bridge_master(dev))
		return true;
	if (netif_is_bridge_port(dev))
		return true;
	if (dev->ieee80211_ptr)
		return true;
	return false;
}

static bool zx_ffe_is_local_mac(struct zx_accel *accel, const u8 *mac)
{
	return rhashtable_lookup_fast(&accel->local_macs, mac,
				      zx_ffe_lmac_params) != NULL;
}

static bool zx_ffe_is_local_ip(struct zx_accel *accel, __be32 addr)
{
	return rhashtable_lookup_fast(&accel->local_ips, &addr,
				      zx_ffe_lip_params) != NULL;
}

static bool zx_ffe_is_local_ip6(struct zx_accel *accel,
				const struct in6_addr *addr)
{
	return rhashtable_lookup_fast(&accel->local_ip6s, addr,
				      zx_ffe_lip6_params) != NULL;
}

/*
 * add=true：用 dev 当前的 dev_addr 重建这个接口的条目；false：删掉它的条目。
 * netdev notifier 是原子通知链，所以这里 spin_lock + GFP_ATOMIC，删除用
 * kfree_rcu（数据面在 RCU 只读段里查表）。
 */
static void zx_ffe_local_mac_set(struct zx_eth_adapter *adapter,
				 struct net_device *dev, bool add)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_lmac *e, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&accel->local_lock, flags);
	list_for_each_entry_safe(e, tmp, &accel->local_mac_list, list) {
		if (e->dev != dev)
			continue;
		list_del_rcu(&e->list);
		rhashtable_remove_fast(&accel->local_macs, &e->node,
				       zx_ffe_lmac_params);
		kfree_rcu(e, rcu);
	}
	if (add) {
		e = kzalloc(sizeof(*e), GFP_ATOMIC);
		if (e) {
			e->dev = dev;
			ether_addr_copy(e->mac, dev->dev_addr);
			if (rhashtable_insert_fast(&accel->local_macs, &e->node,
						   zx_ffe_lmac_params)) {
				/* 同一个 MAC 已经有条目（重复地址）：留旧的就够 */
				kfree(e);
			} else {
				list_add_rcu(&e->list, &accel->local_mac_list);
			}
		}
	}
	spin_unlock_irqrestore(&accel->local_lock, flags);
}

static void zx_ffe_local_ip_add(struct zx_eth_adapter *adapter,
				struct net_device *dev, __be32 addr)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_lip *e;
	unsigned long flags;

	if (!addr)
		return;
	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return;
	e->dev = dev;
	e->addr = addr;

	spin_lock_irqsave(&accel->local_lock, flags);
	if (rhashtable_insert_fast(&accel->local_ips, &e->node,
				   zx_ffe_lip_params)) {
		kfree(e);	/* 已有同一个地址（重复事件） */
	} else {
		list_add_rcu(&e->list, &accel->local_ip_list);
	}
	spin_unlock_irqrestore(&accel->local_lock, flags);
}

/* addr == 0：删掉这个接口的全部地址（接口注销/重新扫描时用） */
static void zx_ffe_local_ip_del(struct zx_eth_adapter *adapter,
				struct net_device *dev, __be32 addr)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_lip *e, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&accel->local_lock, flags);
	list_for_each_entry_safe(e, tmp, &accel->local_ip_list, list) {
		if (e->dev != dev || (addr && e->addr != addr))
			continue;
		list_del_rcu(&e->list);
		rhashtable_remove_fast(&accel->local_ips, &e->node,
				       zx_ffe_lip_params);
		kfree_rcu(e, rcu);
	}
	spin_unlock_irqrestore(&accel->local_lock, flags);
}

/*
 * IPv6 版的本机地址集合（P4-TASK §0.4）。与 IPv4 那条完全并列：同一条
 * local_lock、同样的 GFP_ATOMIC + kfree_rcu，只是键换成 16 B 的 in6_addr。
 * 集合漏掉一个本机 v6 地址的后果是"本该终结的帧被快路转出去"（对端会收到
 * 一个 MAC 被改过、源地址却是我们的包），所以**除了 -EEXIST 这种真正的重复
 * 事件**（同一个地址挂在两个接口上，留旧的就行）之外的失败都要限速吼一声。
 * 原子通知链里不能等内存，也不能取 accel->lock。
 */
static void zx_ffe_local_ip6_add(struct zx_eth_adapter *adapter,
				 struct net_device *dev,
				 const struct in6_addr *addr)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_lip6 *e;
	unsigned long flags;
	int ret;

	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e) {
		dev_warn_ratelimited(adapter->dev,
			"ffe: out of memory recording a local IPv6 address; a frame to it may be forwarded instead of terminated\n");
		return;
	}
	e->dev = dev;
	e->addr = *addr;

	spin_lock_irqsave(&accel->local_lock, flags);
	ret = rhashtable_insert_fast(&accel->local_ip6s, &e->node,
				     zx_ffe_lip6_params);
	if (ret) {
		kfree(e);
		if (ret != -EEXIST)
			dev_warn_ratelimited(adapter->dev,
				"ffe: cannot record a local IPv6 address: %d (a frame to it may be forwarded instead of terminated)\n",
				ret);
	} else {
		list_add_rcu(&e->list, &accel->local_ip6_list);
	}
	spin_unlock_irqrestore(&accel->local_lock, flags);
}

/* addr == NULL：删掉这个接口的全部 IPv6 地址（接口注销时用） */
static void zx_ffe_local_ip6_del(struct zx_eth_adapter *adapter,
				 struct net_device *dev,
				 const struct in6_addr *addr)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_lip6 *e, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&accel->local_lock, flags);
	list_for_each_entry_safe(e, tmp, &accel->local_ip6_list, list) {
		/* 16 B 键的直接比较：这里不是热路径，不值得为它拉进 net/ipv6.h */
		if (e->dev != dev ||
		    (addr && memcmp(&e->addr, addr, sizeof(*addr))))
			continue;
		list_del_rcu(&e->list);
		rhashtable_remove_fast(&accel->local_ip6s, &e->node,
				       zx_ffe_lip6_params);
		kfree_rcu(e, rcu);
	}
	spin_unlock_irqrestore(&accel->local_lock, flags);
}

static int zx_ffe_netdev_event(struct notifier_block *nb, unsigned long event,
			       void *ptr)
{
	struct zx_accel *accel = container_of(nb, struct zx_accel, netdev_nb);
	struct zx_eth_adapter *adapter =
		container_of(accel, struct zx_eth_adapter, accel);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (dev_net(dev) != &init_net)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_CHANGEADDR:
	case NETDEV_CHANGEUPPER:
		/* 地址变了/上桥下桥：先删这个接口的旧条目再按当前状态加回去 */
		zx_ffe_local_mac_set(adapter, dev, zx_ffe_should_track(dev));
		break;
	case NETDEV_UNREGISTER:
		zx_ffe_local_mac_set(adapter, dev, false);
		zx_ffe_local_ip_del(adapter, dev, 0);
		zx_ffe_local_ip6_del(adapter, dev, NULL);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static int zx_ffe_inetaddr_event(struct notifier_block *nb,
				 unsigned long event, void *ptr)
{
	struct zx_accel *accel = container_of(nb, struct zx_accel, inetaddr_nb);
	struct zx_eth_adapter *adapter =
		container_of(accel, struct zx_eth_adapter, accel);
	struct in_ifaddr *ifa = ptr;
	struct net_device *dev;

	if (!ifa || !ifa->ifa_dev)
		return NOTIFY_DONE;
	dev = ifa->ifa_dev->dev;
	if (!dev || dev_net(dev) != &init_net)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
		zx_ffe_local_ip_add(adapter, dev, ifa->ifa_local);
		break;
	case NETDEV_DOWN:
		zx_ffe_local_ip_del(adapter, dev, ifa->ifa_local);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

/*
 * inet6addr 通知链（P4-TASK §0.4）：事件只有 NETDEV_UP / NETDEV_DOWN，ptr 是
 * struct inet6_ifaddr，地址在 ifa->addr、设备经 ifa->idev->dev 拿到。
 *
 * 上下文：inet6addr_chain 是**原子**通知链（addrconf 在软中断/持锁上下文里
 * 调它），所以这里不能睡、不能取 accel->lock，只能走 local_lock + GFP_ATOMIC
 * + kfree_rcu——与 IPv4 那条 inetaddr 通知链完全同一套约束。
 *
 * br-lan 的 ULA 走的就是这条链：netifd 在 br-lan 起来之后 `ip -6 addr add`
 * 时才发 NETDEV_UP，探针时还看不到，所以必须靠通知链；zx_accel_on_open() 里
 * 那次全量扫描只是给"通知链注册之前就存在的地址"兜底。
 */
static int zx_ffe_inet6addr_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct zx_accel *accel = container_of(nb, struct zx_accel, inet6addr_nb);
	struct zx_eth_adapter *adapter =
		container_of(accel, struct zx_eth_adapter, accel);
	struct inet6_ifaddr *ifa = ptr;
	struct net_device *dev;

	if (!ifa || !ifa->idev)
		return NOTIFY_DONE;
	dev = ifa->idev->dev;
	if (!dev || dev_net(dev) != &init_net)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
		zx_ffe_local_ip6_add(adapter, dev, &ifa->addr);
		break;
	case NETDEV_DOWN:
		zx_ffe_local_ip6_del(adapter, dev, &ifa->addr);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

/* 端口全 down 后清表（在 work 里做，rtnl 上下文不能取 accel->lock） */
static void zx_ffe_port_flush_work(struct work_struct *work)
{
	struct zx_accel *accel =
		container_of(work, struct zx_accel, port_flush_work);
	struct zx_eth_adapter *adapter =
		container_of(accel, struct zx_eth_adapter, accel);

	mutex_lock(&accel->lock);
	__zx_ffe_flush(adapter, -1);
	mutex_unlock(&accel->lock);
}

/* ---- 判据链与真转发（P2 v4 / P4 v6）-------------------------------- */

/*
 * 出口端口 → netdev。ports[] 只在 probe 里写一次、没有 remove 路径，所以
 * READ_ONCE 足够；DTS 缺某个 reg 时数组有洞，返回 NULL 必须判。数据面不取
 * rtnl，netif_running/netif_carrier_ok 是良性竞态：判不准就回栈，不丢包。
 * 返回 NULL = "这个出口现在不能用"，调用方按 drop_noport 处理。
 */
static struct net_device *zx_ffe_eport_dev(struct zx_eth_adapter *adapter,
					   u32 eport)
{
	struct net_device *nd;

	if (eport >= ZX_NUM_PORTS)
		return NULL;
	nd = READ_ONCE(adapter->ports[eport]);
	/* IFF_UP 不等于能发：link down 时驱动会把 MAC 关掉（zx_mac_link_down） */
	if (!nd || !netif_running(nd) || !netif_carrier_ok(nd))
		return NULL;
	return nd;
}

/*
 * 老化（设计 §4.1）：每秒一拍，某个条目超过 ffe_aging 秒没有被"全部判据
 * 通过"的帧命中就删掉。P1 的条目都是静态条目，照样老化（P1-TASK §3）。
 */
static void zx_ffe_age(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *flow, *tmp;
	unsigned long now = jiffies;
	u64 max_idle_ms;

	if (!ffe_aging)
		return;
	max_idle_ms = (u64)ffe_aging * 1000;

	mutex_lock(&accel->lock);
	list_for_each_entry_safe(flow, tmp, &accel->flow_list, list) {
		if (jiffies_to_msecs(now - flow->lastuse) <= max_idle_ms)
			continue;
		__zx_ffe_flow_del(adapter, flow);
	}
	mutex_unlock(&accel->lock);
}

/*
 * 判据链的裁决。两条链（v4/v6）只报告"这一帧该被怎么处理"，所有计数集中在
 * zx_ffe_rx() 的出口做一次——两族的口径只有一份，不会出现"v6 的 miss 忘了
 * 加上某个子集"这种对不上的账。
 */
enum zx_ffe_verdict {
	ZX_FFE_V_OK = 0,	/* 判据链全部通过，可以查表/提交 */
	ZX_FFE_V_FWD,		/* 已改写并提交给发送硬件（只有 forward 返回它） */
	ZX_FFE_V_MISS,		/* 普通 miss：某条判据不符或查表未命中 */
	ZX_FFE_V_CSUM_ERR,	/* 只有 v4：IPv4 头校验和错 */
	ZX_FFE_V_TTL_ERR,	/* TTL/hop_limit <= 1：由栈回 ICMP(6) Time Exceeded */
	ZX_FFE_V_DROP_LOCAL,	/* 目的地址属本机：本机终结，回栈 */
	ZX_FFE_V_DROP_NOPORT,	/* 出口端口不存在 / !running / !carrier */
	ZX_FFE_V_FALLBACK,	/* 命中但按判据回栈（同口/NAT/帧长/出口 MTU） */
	ZX_FFE_V_QFULL,		/* 命中但发送环满：回栈让 qdisc 背压 */
	ZX_FFE_V_RX_DROP,	/* 已改写但提交失败，只能丢帧（不可达） */
};

/*
 * 两个前缀判据。直接判字节而不 include <net/ipv6.h>：判据在每包热路径上，
 * 两个 bit 测试就够，没必要为一个 ff00::/8 拉进整棵 IPv6 头文件树。
 */
static inline bool zx_ffe_v6_is_mcast(const struct in6_addr *addr)
{
	return addr->s6_addr[0] == 0xff;			/* ff00::/8 */
}

static inline bool zx_ffe_v6_is_linklocal(const struct in6_addr *addr)
{
	return addr->s6_addr[0] == 0xfe &&
	       (addr->s6_addr[1] & 0xc0) == 0x80;	/* fe80::/10 */
}

/*
 * IPv4 判据链（设计 §2.2 + 勘误 §1/§2）。通过时填好 key 与 *pkt_len（IPv4 的
 * tot_len，即 IP 层长度），返回 ZX_FFE_V_OK。入口口/MAC/runt 三条两族共用的
 * 判据在 zx_ffe_rx() 的前置段里做过，这里不再重复。
 *
 * 顺序按设计 §2.2，唯一的偏离是 #13（目的 IP 属本机）提到 #8（L4 协议）之前：
 * 否则 ping 本机地址（ICMP）会在协议判据里被算成普通 miss，drop_local 永远不
 * 涨，验收判据无法成立（P1-TASK §5.3 / design-errata §2）。
 */
static enum zx_ffe_verdict
zx_ffe_v4_check(struct zx_accel *accel, const u8 *frame, u16 len,
		__be16 h_proto, struct zx_ffe_key *key, u32 *pkt_len)
{
	struct iphdr ip;
	__be16 ports[2];

	/* #4 只处理裸 IPv4（ARP/PPPoE/VLAN 外层/IPv6 都不碰） */
	if (h_proto != htons(ETH_P_IP))
		return ZX_FFE_V_MISS;
	/* #5 版本/IHL/长度：带选项、被截断的都不碰 */
	memcpy(&ip, frame + ETH_HLEN, sizeof(ip));
	if (ip.version != 4 || ip.ihl != 5 ||
	    ntohs(ip.tot_len) > len - ETH_HLEN)
		return ZX_FFE_V_MISS;
	/* #6 IPv4 头校验和（错的帧栈也会丢，快路不背这个锅） */
	if (ip_fast_csum(&ip, ip.ihl))
		return ZX_FFE_V_CSUM_ERR;
	/* #7 分片：不重组、不猜偏移 */
	if (ip.frag_off & htons(IP_MF | IP_OFFSET))
		return ZX_FFE_V_MISS;
	/* #13（提前）：目的 IP 属本机 = 终结/端口映射，必须走栈 */
	if (zx_ffe_is_local_ip(accel, ip.daddr))
		return ZX_FFE_V_DROP_LOCAL;
	/* #8 只接 TCP/UDP：ICMP 要回错、IGMP 要上送，其余一律不碰 */
	if (ip.protocol != IPPROTO_TCP && ip.protocol != IPPROTO_UDP)
		return ZX_FFE_V_MISS;
	/* #9 TTL 归零必须由栈发 ICMP Time Exceeded */
	if (ip.ttl <= 1)
		return ZX_FFE_V_TTL_ERR;
	/* 取 L4 端口至少要 4 字节（TCP/UDP 头的前两个字段就是端口） */
	if (ntohs(ip.tot_len) < sizeof(ip) + 4)
		return ZX_FFE_V_MISS;
	memcpy(ports, frame + ETH_HLEN + sizeof(ip), sizeof(ports));

	memset(key, 0, sizeof(*key));
	key->v4[0] = ip.saddr;
	key->v4[1] = ip.daddr;
	key->sport = ports[0];
	key->dport = ports[1];
	key->proto = ip.protocol;
	key->family = 4;
	*pkt_len = ntohs(ip.tot_len);
	return ZX_FFE_V_OK;
}

/*
 * IPv6 判据链（设计 §6 + P4-TASK §0）。通过时填好 key（v6[32]）与 *pkt_len
 * （IPv6 的 payload_len + 40，即 IP 层长度），返回 ZX_FFE_V_OK。
 *
 * 顺序与理由：
 *   #4 ethertype 必须是 0x86dd（非 IP 帧走 nfam 分支进来时也在这里回栈）；
 *   #5 version == 6；
 *   #6 payload_len + 40 == 帧长 - 14：**严格相等**。payload_len 只算 40 B 固定
 *      头之后的部分（含扩展头），所以这条同时挡掉 jumbo payload（payload_len=0
 *      + 逐跳 jumbo 选项）、被截断的帧、被 GRO 聚合过的帧——快路不做任何拆分；
 *   #7 next_header 为 0/43/44/60（逐跳/路由/分片/目的选项）一律回栈：猜扩展头
 *      长度就是把扩展头当 L4 端口读，判错一次就是发错包；
 *   #8 hop_limit <= 1：必须由栈回 ICMPv6 Time Exceeded，计 ttl_err；
 *   #9 目的组播 ff00::/8（NS/NA/RS/RA/MLD 全在这里）回栈——组播没有"下一跳
 *      MAC + 出口"的确定映射，扩散面归栈与桥；
 *   #10 目的地址属本机 v6 地址集合 → drop_local（终结）。**必须排在协议判据
 *      之前**，否则 ping6 本机地址（ICMPv6）先被 #12 挡成普通 miss，
 *      drop_local 永远不涨（与 v4 提前 #13 同一条理由）；
 *   #11 源地址属 fe80::/10：链路本地不进快路，避免旁路 ND 状态机（同一
 *      链路两端的 fe80 流本来也该由栈按 scope id 处理）；
 *   #12 只有 TCP(6)/UDP(17)：58 ICMPv6 整类排除（NS/NA/RS/RA/MLD/echo 全在
 *      这条里），其余协议一律不碰。
 * 之后取 L4 端口进键；出口/NAT/帧长/MTU 四条判据两族共用（zx_ffe_forward）。
 */
static enum zx_ffe_verdict
zx_ffe_v6_check(struct zx_accel *accel, const u8 *frame, u16 len,
		__be16 h_proto, struct zx_ffe_key *key, u32 *pkt_len)
{
	struct ipv6hdr ip6;
	__be16 ports[2];
	u32 pkt;

	/* #4 */
	if (h_proto != htons(ETH_P_IPV6))
		return ZX_FFE_V_MISS;
	memcpy(&ip6, frame + ETH_HLEN, sizeof(ip6));
	/* #5 */
	if (ip6.version != 6)
		return ZX_FFE_V_MISS;
	/* #6 */
	pkt = (u32)ntohs(ip6.payload_len) + sizeof(ip6);
	if (pkt != (u32)len - ETH_HLEN)
		return ZX_FFE_V_MISS;
	/* #7 */
	switch (ip6.nexthdr) {
	case IPPROTO_HOPOPTS:	/* 0 */
	case IPPROTO_ROUTING:	/* 43 */
	case IPPROTO_FRAGMENT:	/* 44 */
	case IPPROTO_DSTOPTS:	/* 60 */
		return ZX_FFE_V_MISS;
	}
	/* #8 */
	if (ip6.hop_limit <= 1)
		return ZX_FFE_V_TTL_ERR;
	/* #9 */
	if (zx_ffe_v6_is_mcast(&ip6.daddr))
		return ZX_FFE_V_MISS;
	/* #10 */
	if (zx_ffe_is_local_ip6(accel, &ip6.daddr))
		return ZX_FFE_V_DROP_LOCAL;
	/* #11 */
	if (zx_ffe_v6_is_linklocal(&ip6.saddr))
		return ZX_FFE_V_MISS;
	/* #12 */
	if (ip6.nexthdr != IPPROTO_TCP && ip6.nexthdr != IPPROTO_UDP)
		return ZX_FFE_V_MISS;
	/* #6 已保证 payload_len >= 4（len >= 64），端口 4 字节在帧内 */
	memcpy(ports, frame + ETH_HLEN + sizeof(ip6), sizeof(ports));

	memset(key, 0, sizeof(*key));
	memcpy(key->v6, &ip6.saddr, sizeof(ip6.saddr));
	memcpy(key->v6 + sizeof(ip6.saddr), &ip6.daddr, sizeof(ip6.daddr));
	key->sport = ports[0];
	key->dport = ports[1];
	key->proto = ip6.nexthdr;
	key->family = 6;
	*pkt_len = pkt;
	return ZX_FFE_V_OK;
}

/*
 * 报文改写（设计 §3 + P4-TASK §0）。只碰 L2 头与 IP 层必然要变的那一个字节：
 *
 *   v4  h_dest ← 条目 dmac；h_source ← 出口端口 MAC 快照；ttl--；IPv4 头校验和
 *       整头重算（不用 RFC 1624 增量，见 P2）。
 *   v6  h_dest/h_source 同上；hop_limit--；**不碰任何校验和**——IPv6 固定头里
 *       没有校验和，L4 伪首部不含 hop_limit（RFC 8200 §8.1：hop limit 不在
 *       pseudo-header 里），所以改它对比上层 checksum 零影响。这里明确不写
 *       "重算/清零校验和"的代码：写了才是错的。
 *
 * **没有失败返回**：调用它的前提是"这一帧一定提交得出去"——容量检查在同一把
 * tx_lock 里、改写之前做完。改完以后帧头已经是出口形态，再交给协议栈就会发出
 * 一个 MAC 与 TTL 都被动过的坏包，所以调用点之后只剩"提交"和"丢弃"两条路。
 */
static void zx_ffe_rewrite(u8 *frame, const struct zx_ffe_flow *flow, int family)
{
	struct iphdr *ip = (struct iphdr *)(frame + ETH_HLEN);
	struct ipv6hdr *ip6 = (struct ipv6hdr *)(frame + ETH_HLEN);

	/* h_dest ← 出口下一跳 MAC；h_source ← 出口端口 MAC 快照（add 时取的） */
	ether_addr_copy(frame, flow->dmac);
	ether_addr_copy(frame + ETH_ALEN, flow->smac);

	if (family) {
		/* 判据 #8 已挡掉 hop_limit <= 1：这里必然 >= 2，减完 >= 1 */
		ip6->hop_limit--;
		return;
	}
	/* 判据 #9 已挡掉 ttl<=1：这里 ttl >= 2，减完必然 >= 1 */
	ip->ttl--;
	/*
	 * 整头重算（不用 RFC 1624 增量：省 30 cycle 不值一个系统级错）。判据 #5
	 * 已保证 ihl == 5，所以这里等价于 ip_send_check()，只是不跨文件调用。
	 */
	ip->check = 0;
	ip->check = ip_fast_csum((u8 *)ip, ip->ihl);
}

/*
 * 两族共用的提交段（P4 的硬要求：v4/v6 只走这一份，别复制两份）：
 *
 *   查表 → 出口现判 → 同口/NAT/帧长/出口 MTU → 取 tx_lock → 查容量 → 改写 →
 *   clean（池可缓存时）→ 提交 → 条目记账 → 解锁
 *
 * 顺序本身就是不变量（P2-TASK §1）：容量检查必须在改写之前（"回栈"只对没改过
 * 的帧成立），又必须在 tx_lock 里（查完到提交之间不能有别人把环填满：
 * tx_pending 只在 tx_lock 下增长）；改写也必须在锁里，否则与另一个 CPU 的
 * 提交/reclaim 交错时帧内容没有属主。
 *
 * pkt_len 是 IP 层长度（v4 的 tot_len / v6 的 payload_len+40），与出口
 * flow->mtu 同口径——不是帧长（帧长另有一条 len > 1514 的上限）。两族都传
 * IP 层长度，所以 MTU 判据也只有一份。
 *
 * 返回 ZX_FFE_V_FWD 表示已提交并记完账（/proc/net/dev 的 rx/tx 也在里面补），
 * 其余裁决交回 zx_ffe_rx() 统一计数。
 */
static enum zx_ffe_verdict
zx_ffe_forward(struct zx_eth_adapter *adapter, struct net_device *nd_in,
	       u8 *frame, u16 bp, u16 len, int rx_port, int family,
	       const struct zx_ffe_key *key, u32 pkt_len)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *flow;
	struct net_device *nd_out;
	unsigned int clean;
	u32 eport;

	/* #13 查表（自己持 RCU 只读段：条目由 kfree_rcu 释放） */
	rcu_read_lock();
	flow = rhashtable_lookup(&accel->flows, key, zx_ffe_ht_params);
	if (!flow) {
		rcu_read_unlock();
		return ZX_FFE_V_MISS;
	}
	/* #14 出口端口现查现判：不存在/down 都回栈 */
	eport = flow->eport;
	nd_out = zx_ffe_eport_dev(adapter, eport);
	if (!nd_out) {
		rcu_read_unlock();
		return ZX_FFE_V_DROP_NOPORT;
	}
	/* #15 同口进出：不做 hairpin */
	if ((int)eport == rx_port) {
		rcu_read_unlock();
		return ZX_FFE_V_FALLBACK;
	}
	/*
	 * #16 NAT 语义：P2 只做纯路由（设计 §9-1 决策 A）；v6 更是没有 NAT66
	 * （P4-TASK §0 "不做"），这条对两族都只是防御：带 NAT 位的条目回栈。
	 */
	if (flow->flags & ZX_FFE_FLOW_NAT) {
		rcu_read_unlock();
		return ZX_FFE_V_FALLBACK;
	}
	/*
	 * #17 帧长上限：这份驱动没有 SG/TSO，发送侧对 >1536 的帧直接丢（见
	 * zx_eth_xmit() 开头）；1515..1536 只可能是 VLAN 帧（判据 #4 已排除），
	 * 所以 >1514 一律回栈，别让对端收到一个驱动自己都发不出去的帧。
	 */
	if (len > ZX_FFE_MAX_FRAME) {
		rcu_read_unlock();
		return ZX_FFE_V_FALLBACK;
	}
	/*
	 * #18 出口 MTU：IP datagram 超过出口 MTU 时栈会分片（v4）或回 ICMPv6
	 * Packet Too Big（v6），快路两条都做不到，回栈。flow->mtu 是 ffe_add
	 * 时从出口 netdev 快照的 dev->mtu，语义是 **IP 层 MTU**：对 v6 它是
	 * "IPv6 包最大长度"（40 B 固定头已经算在里面，内核 ip6_mtu() 就是
	 * dev->mtu 减去 L2 开销后的这个值），所以两边都拿 IP 层长度跟它比，
	 * 不写成 payload_len > mtu。0 表示"不知道"——一律回栈，绝不猜。
	 * （邻居表里的 nd_mtu 是 RFC 8200 的 PMTU 缓存，数据面要查邻居才能
	 * 读到，且它只反映单条路径；这里用 dev->mtu 这个链路层上界，超了回栈
	 * 让栈去发 Packet Too Big，语义保守且不需要额外加锁。）
	 */
	if (!flow->mtu || pkt_len > flow->mtu) {
		rcu_read_unlock();
		return ZX_FFE_V_FALLBACK;
	}

	/* #19 环容量（设计 §2.2 #15）：满就回栈，让 qdisc 背压 */
	spin_lock_bh(&adapter->tx_lock);
	if (adapter->tx_pending >= ZX_TX_QUEUE_LIMIT) {
		/* 活跃流：不更新 lastuse，老化会把这条件正在用的条目删掉 */
		flow->lastuse = jiffies;
		spin_unlock_bh(&adapter->tx_lock);
		rcu_read_unlock();
		return ZX_FFE_V_QFULL;
	}

	/* 改写与提交都在 tx_lock 里，帧内容不会裸露在锁外（P2-TASK §1 顺序） */
	zx_ffe_rewrite(frame, flow, family);
	/*
	 * 池可缓存时，改写过的头必须在 BMU 读之前推到一致性点（设计 §8-1）：
	 * 脏的只有 16 B 池头 + L2 头 + 固定 IP 头——v4 是 34 B，v6 是 54 B
	 * （16 + ETH_HLEN + 40），payload 在收包侧 invalidate 过、这里只读不写。
	 * 默认映射是非缓存，平时不执行。
	 */
	clean = 16 + ETH_HLEN +
		(family ? sizeof(struct ipv6hdr) : sizeof(struct iphdr));
	if (adapter->pkt_pool_cacheable)
		dma_sync_single_for_device(adapter->dev,
					   adapter->pkt_pool_dma +
					   (u32)bp * ZX_BUF_SIZE,
					   clean, DMA_TO_DEVICE);
	/*
	 * 提交。tx_lock 在手里、容量刚查过，所以 zx_tx_submit_bp() 不可能返回
	 * false。真出现（代码/内存被改坏）就按"丢帧"处理：帧已经改过，绝不能
	 * 回栈；槽位也没交给硬件，所以 bp 由调用方还给 BMU（返回 DROPPED）。
	 */
	if (!zx_tx_submit_bp(adapter, eport, bp, len)) {
		spin_unlock_bh(&adapter->tx_lock);
		rcu_read_unlock();
		dev_warn_ratelimited(adapter->dev,
			"ffe: TX ring full right after the capacity check (pending=%u), dropping a rewritten frame for eport %u\n",
			adapter->tx_pending, eport);
		return ZX_FFE_V_RX_DROP;
	}

	/* 记账：条目 hits/bytes/lastuse 每次命中更新，否则老化会误删活跃流 */
	flow->hits++;
	flow->bytes += len;
	flow->lastuse = jiffies;
	spin_unlock_bh(&adapter->tx_lock);
	rcu_read_unlock();

	/*
	 * 统计对齐慢路径：慢路径在 zx_eth_rx() 里给入口 netdev 记 rx（这一帧进了
	 * 栈）、在 zx_eth_xmit() 里给出口 netdev 记 tx。快路两边都不经过，但
	 * /proc/net/dev 必须还是"从 lanX 进、从 lanY 出"（设计 §7 P2 的验收就是
	 * 看两个端口的计数照涨、而 IpOutForwDatagrams 不涨）。
	 */
	dev_dstats_rx_add(nd_in, len);
	dev_dstats_tx_add(nd_out, len);
	return ZX_FFE_V_FWD;
}

/*
 * 加速分发点的入口（zx_eth_rx() 调用）：判定 + 真转发。
 *
 * 判据链全部通过以后，把**同一个 bp** 写进发送描述符交给硬件，帧不再进
 * zc/skb/协议栈（P2-TASK §0）。返回值告诉调用方怎么收尾（enum
 * zx_ffe_rx_result）：STACK = 帧一个字节都没动、照原路径走；FORWARDED = 已交给
 * 发送硬件、只做 RX 收尾且**不许还 bp**；DROPPED = 快路里确定丢了、收尾时把 bp
 * 还回 BMU。
 *
 * 结构（P4 起按 family 分派，两族共用同一段提交逻辑）：
 *
 *   前置段（#0-#3，两族共用）：入口口已 probe → 目的 MAC 非组播/广播 →
 *     目的 MAC 在本机集合 → len >= 64
 *   判据段（按 family 二选一）：zx_ffe_v4_check() / zx_ffe_v6_check()
 *     ——各自的判据顺序详见这两个函数的头注释
 *   提交段（#13-#19 + 改写/提交/记账，两族共用）：zx_ffe_forward()
 *   计数段（只有这一份）：把裁决换算成 cnt[family] 的字段
 *
 * 计数口径（设计 §1.5 / P1-TASK §1）：lookup == hit + miss 恒成立；hit 只统计
 * 真正转发出去的帧（P2 起 hit == fwd）；csum_err/ttl_err/drop_local/
 * drop_noport/fallback/fallback_qfull/rx_drop 都是 miss 的子集，用来区分是哪
 * 条判据把它挡回去的。v6 没有头校验和，所以 csum_err 在 v6 上恒为 0；
 * ttl_err 对 v6 是 hop_limit 到期。
 */
enum zx_ffe_rx_result zx_ffe_rx(struct zx_eth_adapter *adapter, u16 bp, u16 len,
				int rx_port, int family, __be16 h_proto)
{
	struct zx_accel *accel = &adapter->accel;
	u8 *frame = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16;
	struct zx_ffe_key key;
	struct zx_ffe_cnt *c;
	struct net_device *nd_in;
	enum zx_ffe_verdict v;
	u32 pkt_len = 0;
	u8 dmac[ETH_ALEN];

	if (family < 0 || family > 1)
		return ZX_FFE_RX_STACK;
	c = &accel->cnt[family];
	c->lookup++;

	dma_rmb();
	memcpy(dmac, frame, ETH_ALEN);

	/*
	 * #0 入口口必须是已 probe 的交换机口：ports[] 的下标就是 RX 描述符里
	 * 的桥端口号 -1（port-map/NOTES.md §1/§3），DTS 少一个 reg 时数组有洞。
	 * 越界/洞口的帧慢路径本来就直接丢（dest_dev == NULL），快路也不许接
	 * （P1-TASK §1.1：PON 侧的帧不许进快路）。
	 */
	nd_in = rx_port >= 0 && rx_port < ZX_NUM_PORTS ?
		READ_ONCE(adapter->ports[rx_port]) : NULL;
	if (!nd_in)
		goto plain_miss;
	/* #1 组播/广播：扩散面由桥与协议栈决定，快路不参与 */
	if (is_multicast_ether_addr(dmac))
		goto plain_miss;
	/* #2 目的 MAC 不在本机集合：这帧不是给我们的，交给桥/栈扩散 */
	if (!zx_ffe_is_local_mac(accel, dmac))
		goto plain_miss;
	/* #3 runt（< 64 B） */
	if (len < 64)
		goto plain_miss;

	if (!family)
		v = zx_ffe_v4_check(accel, frame, len, h_proto, &key, &pkt_len);
	else
		v = zx_ffe_v6_check(accel, frame, len, h_proto, &key, &pkt_len);

	if (v == ZX_FFE_V_OK)
		v = zx_ffe_forward(adapter, nd_in, frame, bp, len, rx_port,
				   family, &key, pkt_len);

	/* 统一出口：裁决 → 计数 → 返回值（两族口径只有这一份） */
	switch (v) {
	case ZX_FFE_V_FWD:
		c->hit++;
		c->fwd++;
		c->bytes += len;
		return ZX_FFE_RX_FORWARDED;
	case ZX_FFE_V_RX_DROP:
		c->rx_drop++;
		break;
	case ZX_FFE_V_CSUM_ERR:
		c->csum_err++;
		break;
	case ZX_FFE_V_TTL_ERR:
		c->ttl_err++;
		break;
	case ZX_FFE_V_DROP_LOCAL:
		c->drop_local++;
		break;
	case ZX_FFE_V_DROP_NOPORT:
		c->drop_noport++;
		break;
	case ZX_FFE_V_QFULL:
		c->fallback_qfull++;
		c->fallback++;
		break;
	case ZX_FFE_V_FALLBACK:
		c->fallback++;
		break;
	case ZX_FFE_V_MISS:	/* 普通 miss，无子集计数 */
	case ZX_FFE_V_OK:	/* 不可能：OK 一定被 forward 消费掉了 */
	default:
		break;
	}
	c->miss++;
	return v == ZX_FFE_V_RX_DROP ? ZX_FFE_RX_DROPPED : ZX_FFE_RX_STACK;

plain_miss:
	c->miss++;
	return ZX_FFE_RX_STACK;
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
		/*
		 * ffe 与 hw 的互斥只在同一族内成立（设计 §5）：PPE 只接 IPv4
		 * （kmod-e2631-ppe 的 ipv6 参数保持 0，v6 没有硬件分类表），所以
		 * v4=hw 与 v6=ffe 允许并存——v6 帧照样上 CPU，正好由这条快路
		 * 接住。拿 bound 挡 v6 会让"默认 v4=hw + v6=ffe"永远切不过去
		 * （P4-TASK §2 判据 1：v6 切 ffe 不需要动 PPE）。
		 */
		if (bound && family == 0) {
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
	__zx_ffe_flush(adapter, family);
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
 *
 * P1（shadow）里这条判据是关掉的：那时 hit 的帧并没有真的转发，drop_local/
 * drop_noport 只是"如果走快路会被挡回去"的投影，拿影子计数做降级会把正常的
 * 本机终结流量误判成快路故障。P2 起 fwd 是真的提交给硬件了，drop/fallback
 * 都是真实结果，所以按设计的阈值与窗口直接启用。
 *
 * 注意 drop 里含 drop_local（目的 IP 属本机、回栈是**正确**行为），这是设计
 * §9-4 的原始口径：管理面 ping 多到 0.1% 以上时可能误降级，此时可以把
 * accel_autofallback 写成 0 关掉它。
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

/* 每秒一拍：流表老化 + 自动降级判据（设计 §4.1 / §9-4）。 */
static void zx_accel_age_work(struct work_struct *work)
{
	struct zx_eth_adapter *adapter =
		container_of(to_delayed_work(work), struct zx_eth_adapter,
			     accel.age);

	zx_ffe_age(adapter);
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
	accel->n_flows = 0;
	accel->tbl_full = 0;
	accel->ffe_last[0] = 0;
	INIT_LIST_HEAD(&accel->flow_list);
	INIT_LIST_HEAD(&accel->local_mac_list);
	INIT_LIST_HEAD(&accel->local_ip_list);
	INIT_LIST_HEAD(&accel->local_ip6_list);
	spin_lock_init(&accel->local_lock);
	INIT_WORK(&accel->port_flush_work, zx_ffe_port_flush_work);
	INIT_DELAYED_WORK(&accel->age, zx_accel_age_work);

	ret = rhashtable_init(&accel->flows, &zx_ffe_ht_params);
	if (ret) {
		dev_err(adapter->dev, "accel: rhashtable_init(flows) failed: %d\n",
			ret);
		return ret;
	}
	ret = rhashtable_init(&accel->local_macs, &zx_ffe_lmac_params);
	if (ret) {
		dev_err(adapter->dev,
			"accel: rhashtable_init(local_macs) failed: %d\n", ret);
		goto err_flows;
	}
	ret = rhashtable_init(&accel->local_ips, &zx_ffe_lip_params);
	if (ret) {
		dev_err(adapter->dev,
			"accel: rhashtable_init(local_ips) failed: %d\n", ret);
		goto err_macs;
	}
	ret = rhashtable_init(&accel->local_ip6s, &zx_ffe_lip6_params);
	if (ret) {
		dev_err(adapter->dev,
			"accel: rhashtable_init(local_ip6s) failed: %d\n", ret);
		goto err_ips;
	}

	/*
	 * 本机地址集合的三个来源（判据 #2 / #13 / v6 链 #10）：netdev 通知链管
	 * MAC（端口/桥/wlan 的 dev_addr，含地址变化与上桥下桥），inetaddr 通知
	 * 链管本机 IPv4 地址，inet6addr 通知链管本机 IPv6 地址（br-lan 的 ULA
	 * 就是从这里进来的）。三条都是原子通知链，处理函数里只做 spin_lock +
	 * GFP_ATOMIC，不取 accel->lock（那是可能睡的 mutex）。
	 * 注册时内核会把现存的接口依次回调一次，probe 之前就存在的 br-lan/
	 * wlan 也能被收进集合；地址是之后才配上的（netifd 的 `ip addr add`），
	 * 由通知链补。
	 */
	accel->netdev_nb.notifier_call = zx_ffe_netdev_event;
	accel->inetaddr_nb.notifier_call = zx_ffe_inetaddr_event;
	accel->inet6addr_nb.notifier_call = zx_ffe_inet6addr_event;
	ret = register_netdevice_notifier(&accel->netdev_nb);
	if (ret) {
		dev_err(adapter->dev,
			"accel: register_netdevice_notifier failed: %d\n", ret);
		goto err_ip6s;
	}
	ret = register_inetaddr_notifier(&accel->inetaddr_nb);
	if (ret) {
		dev_err(adapter->dev,
			"accel: register_inetaddr_notifier failed: %d\n", ret);
		goto err_netdev;
	}
	ret = register_inet6addr_notifier(&accel->inet6addr_nb);
	if (ret) {
		dev_err(adapter->dev,
			"accel: register_inet6addr_notifier failed: %d\n", ret);
		goto err_inetaddr;
	}

	schedule_delayed_work(&accel->age, msecs_to_jiffies(ZX_FFE_TICK_MS));
	return 0;

err_inetaddr:
	unregister_inetaddr_notifier(&accel->inetaddr_nb);
err_netdev:
	unregister_netdevice_notifier(&accel->netdev_nb);
err_ip6s:
	rhashtable_free_and_destroy(&accel->local_ip6s, zx_ffe_free_lip6, NULL);
err_ips:
	rhashtable_free_and_destroy(&accel->local_ips, zx_ffe_free_lip, NULL);
err_macs:
	rhashtable_free_and_destroy(&accel->local_macs, zx_ffe_free_lmac, NULL);
err_flows:
	rhashtable_free_and_destroy(&accel->flows, zx_ffe_free_flow, NULL);
	return ret;
}

/*
 * 收摊：先停两个 work（老化会动流表，端口清表 work 会取 accel->lock），再摘
 * 通知链，最后按条目释放三张表。本驱动是 builtin、正常没有 rmmod，但 probe
 * 失败与将来加 remove 都必须走这里，不能漏。
 */
void zx_accel_exit(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;

	cancel_delayed_work_sync(&accel->age);
	cancel_work_sync(&accel->port_flush_work);
	unregister_inet6addr_notifier(&accel->inet6addr_nb);
	unregister_inetaddr_notifier(&accel->inetaddr_nb);
	unregister_netdevice_notifier(&accel->netdev_nb);
	rhashtable_free_and_destroy(&accel->flows, zx_ffe_free_flow, NULL);
	rhashtable_free_and_destroy(&accel->local_macs, zx_ffe_free_lmac, NULL);
	rhashtable_free_and_destroy(&accel->local_ips, zx_ffe_free_lip, NULL);
	rhashtable_free_and_destroy(&accel->local_ip6s, zx_ffe_free_lip6, NULL);
	INIT_LIST_HEAD(&accel->flow_list);
	INIT_LIST_HEAD(&accel->local_mac_list);
	INIT_LIST_HEAD(&accel->local_ip_list);
	INIT_LIST_HEAD(&accel->local_ip6_list);
	accel->n_flows = 0;
}

/*
 * ndo_open / ndo_stop 的钩子（设计 §1.4 最后一段）。调用者持 rtnl，所以这里
 * 不许取 accel->lock（锁序见文件头：accel->lock → rtnl）。
 *
 * open：本机地址集合的兜底全量扫描（notifier 注册前就存在的接口）；
 * close：所有端口都 down 了，整张流表都不再可用，转 work 清表（设计 §4.2）。
 */
void zx_accel_on_open(struct zx_eth_adapter *adapter)
{
	struct net_device *dev;

	ASSERT_RTNL();

	for_each_netdev(&init_net, dev) {
		if (zx_ffe_should_track(dev))
			zx_ffe_local_mac_set(adapter, dev, true);
	}
	for_each_netdev(&init_net, dev) {
		struct in_device *in_dev = __in_dev_get_rtnl(dev);
		struct in_ifaddr *ifa;

		if (!in_dev)
			continue;
		for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next)
			zx_ffe_local_ip_add(adapter, dev, ifa->ifa_local);
	}
	/*
	 * IPv6 全量扫描：与上面两段同理，给"通知链注册之前就配好的地址"兜底。
	 * idev->addr_list 的锁是 idev->lock，这里靠 rtnl 互斥配置路径（与
	 * __in_dev_get_rtnl/ifa_list 同一套用法），地址列表在 rtnl 下是稳定的。
	 */
	for_each_netdev(&init_net, dev) {
		struct inet6_dev *idev = __in6_dev_get(dev);
		struct inet6_ifaddr *ifp;

		if (!idev)
			continue;
		list_for_each_entry(ifp, &idev->addr_list, if_list)
			zx_ffe_local_ip6_add(adapter, dev, &ifp->addr);
	}
}

void zx_accel_on_close(struct zx_eth_adapter *adapter)
{
	/* 持 rtnl 时不能取 accel->lock（会与 zx_accel_set() 的锁序相反） */
	schedule_work(&adapter->accel.port_flush_work);
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
	seq_printf(s, "lookup=%llu hit=%llu miss=%llu fwd=%llu fallback=%llu fallback_qfull=%llu drop_noport=%llu drop_local=%llu csum_err=%llu ttl_err=%llu bytes=%llu qlen_peak=%u rx_drop=%llu\n",
		   c->lookup, c->hit, c->miss, c->fwd, c->fallback,
		   c->fallback_qfull, c->drop_noport, c->drop_local,
		   c->csum_err, c->ttl_err, c->bytes, c->qlen_peak,
		   c->rx_drop);
	/*
	 * 第 3 行是给人和脚本看的状态行。hint 是"下一步该做什么"的机器可读
	 * 提示：PPE 绑着 flowtable 时切 ffe/zc 会被 S1 拒绝，用户态得先关掉
	 * 硬件 offload（uci firewall.@defaults[0].flow_offloading_hw=0 + reload），
	 * 所以给一个不含空格的单 token，不会被按空格切字段的读侧打散。
	 */
	seq_printf(s, "ppe_bound=%u sw_in=%llu sw_out=%llu drain_ms=%llu drain_frames=%llu autofallback=%u zc_mask=%u flows=%u tbl_full=%llu hint=%s\n",
		   bound ? 1u : 0u, accel->sw_in,
		   accel->sw_out, accel->drain_ms, accel->drain_frames,
		   READ_ONCE(accel->autofallback) ? 1u : 0u,
		   READ_ONCE(accel->zc_mask),
		   READ_ONCE(accel->n_flows), READ_ONCE(accel->tbl_full),
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

/* ---- ffe_add / ffe_del / ffe_flush（P1-TASK §2）--------------------- */

#define ZX_FFE_ARG_MAX		12

/* 按空白切成 token（就地写 0），返回 token 数 */
static int zx_ffe_split(char *buf, char **argv, int max)
{
	char *p = buf;
	int argc = 0;

	while (argc < max) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
			p++;
		if (*p)
			*p++ = 0;
	}
	return argc;
}

static int zx_ffe_bad_usage(struct zx_eth_adapter *adapter, const char *usage)
{
	struct zx_accel *accel = &adapter->accel;

	mutex_lock(&accel->lock);
	snprintf(accel->ffe_last, sizeof(accel->ffe_last), "usage: %s",
		 usage);
	mutex_unlock(&accel->lock);
	return -EINVAL;
}

/*
 * 解析流表键，共 8 个 token：
 *   <family> <saddr> <daddr> <sport> <dport> <proto> <ipport> <vid>
 * family 4：地址用 in4_pton（点分十进制）。
 * family 6：地址用 in6_pton，接受 `fd00::1` 与 `::ffff:1.2.3.4` 这类写法。
 *   注意 `::ffff:1.2.3.4` 落进 v6[32] 的是 16 B 的 v4-mapped 形式（与数据面
 *   从 IPv6 头里取到的 saddr/daddr 逐字节一致），**不做** v4-mapped 展开、
 *   也不把它当成 IPv4 键——一个键只属于一族。
 * 调用前 key 必须已清零（rhashtable 的键按字节比较，尾部填充也要一致）。
 */
static int zx_ffe_key_parse(char **argv, struct zx_ffe_key *key,
			    const char **err)
{
	unsigned int fam, sport, dport, proto, ipport, vid;

	if (kstrtouint(argv[0], 10, &fam) || (fam != 4 && fam != 6)) {
		*err = "family must be 4 or 6";
		return -EINVAL;
	}
	if (fam == 4) {
		if (!in4_pton(argv[1], -1, (u8 *)&key->v4[0], '\0', NULL) ||
		    !in4_pton(argv[2], -1, (u8 *)&key->v4[1], '\0', NULL)) {
			*err = "bad IPv4 address";
			return -EINVAL;
		}
	} else {
		if (!in6_pton(argv[1], -1, &key->v6[0], '\0', NULL) ||
		    !in6_pton(argv[2], -1, &key->v6[sizeof(struct in6_addr)],
			      '\0', NULL)) {
			*err = "bad IPv6 address";
			return -EINVAL;
		}
	}
	if (kstrtouint(argv[3], 10, &sport) || sport > 0xffff ||
	    kstrtouint(argv[4], 10, &dport) || dport > 0xffff ||
	    kstrtouint(argv[5], 10, &proto) || proto > 0xff ||
	    kstrtouint(argv[6], 10, &ipport) || ipport > 0xff ||
	    kstrtouint(argv[7], 10, &vid) || vid > 0xffff) {
		*err = "bad sport/dport/proto/ipport/vid range";
		return -EINVAL;
	}

	key->sport = htons(sport);
	key->dport = htons(dport);
	key->proto = proto;
	key->ipport = ipport;
	key->family = fam;
	key->vid = vid;
	return 0;
}

/* 把键里的地址按族打到 ffe_last：v4 用 %pI4，v6 用 %pI6c */
static int zx_ffe_key_print(char *buf, size_t len, const char *what,
			    const struct zx_ffe_key *key, unsigned int eport,
			    u32 n_flows)
{
	if (key->family == 6)
		return snprintf(buf, len,
				"%s 6 %pI6c %pI6c sport=%u dport=%u proto=%u ipport=%u vid=%u -> eport %u ok, flows=%u",
				what,
				(const struct in6_addr *)&key->v6[0],
				(const struct in6_addr *)&key->v6[sizeof(struct in6_addr)],
				ntohs(key->sport), ntohs(key->dport),
				key->proto, key->ipport, key->vid, eport,
				n_flows);
	return snprintf(buf, len,
			"%s 4 %pI4 %pI4 sport=%u dport=%u proto=%u ipport=%u vid=%u -> eport %u ok, flows=%u",
			what, &key->v4[0], &key->v4[1],
			ntohs(key->sport), ntohs(key->dport),
			key->proto, key->ipport, key->vid, eport, n_flows);
}

/* ffe_add <key 8 个 token> <eport> <dmac> */
static int zx_ffe_op_add(struct zx_eth_adapter *adapter, char **argv)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_key key;
	struct zx_ffe_flow *flow;
	struct net_device *nd;
	unsigned int eport;
	const char *err = NULL;
	u8 dmac[ETH_ALEN];
	int ret;

	memset(&key, 0, sizeof(key));
	ret = zx_ffe_key_parse(argv, &key, &err);
	if (ret)
		goto fail;
	if (kstrtouint(argv[8], 10, &eport) || eport >= ZX_NUM_PORTS) {
		err = "eport out of range";
		ret = -EINVAL;
		goto fail;
	}
	nd = READ_ONCE(adapter->ports[eport]);
	if (!nd) {
		err = "eport is not a probed switch port";
		ret = -EINVAL;
		goto fail;
	}
	if (!mac_pton(argv[9], dmac)) {
		err = "bad dmac (xx:xx:xx:xx:xx:xx)";
		ret = -EINVAL;
		goto fail;
	}

	mutex_lock(&accel->lock);
	/* 表满不驱逐（P1-TASK §3）：拒绝新条目，计数可解释 */
	if (accel->n_flows >= ZX_FFE_FLOWS_MAX) {
		accel->tbl_full++;
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "add: -ENOSPC (table full: %u entries)",
			 accel->n_flows);
		mutex_unlock(&accel->lock);
		return -ENOSPC;
	}
	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow) {
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "add: -ENOMEM");
		mutex_unlock(&accel->lock);
		return -ENOMEM;
	}
	flow->key = key;
	/* h_source 在 add 时快照，P2 改写不再每包读 dev_addr（6 字节会撕裂） */
	ether_addr_copy(flow->dmac, dmac);
	ether_addr_copy(flow->smac, nd->dev_addr);
	flow->eport = eport;
	/*
	 * 出口 MTU 也在 add 时快照：数据面判断据 #18（超 MTU 回栈让栈去分片/发
	 * ICMP(6)），每包读 nd->mtu 没有意义且要多一次跨 cache 访问。语义是
	 * **IP 层 MTU**：dev->mtu 本身就是"IPv4 tot_len / IPv6 payload_len+40
	 * 的上限"（v6 的 40 B 固定头算在里面），所以两族都拿 IP 层长度跟它比。
	 * MTU 后来被改大/改小不会刷新条目——这时宁可条目偏保守（回栈），不值得
	 * 为它加通知链。
	 */
	flow->mtu = min_t(u32, nd->mtu, 0xffff);
	flow->flags = ZX_FFE_FLOW_STATIC;
	flow->lastuse = jiffies;
	ret = rhashtable_insert_fast(&accel->flows, &flow->node,
				     zx_ffe_ht_params);
	if (ret) {
		kfree(flow);
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "add: %d (same key already present?)", ret);
		mutex_unlock(&accel->lock);
		return ret;
	}
	list_add_rcu(&flow->list, &accel->flow_list);
	accel->n_flows++;
	zx_ffe_key_print(accel->ffe_last, sizeof(accel->ffe_last), "add",
			 &key, eport, accel->n_flows);
	mutex_unlock(&accel->lock);
	return 0;

fail:
	mutex_lock(&accel->lock);
	snprintf(accel->ffe_last, sizeof(accel->ffe_last), "add: %d (%s)",
		 ret, err ? err : "invalid");
	mutex_unlock(&accel->lock);
	return ret;
}

/* ffe_del <key 8 个 token>：key 必须与 add 完全一致，不猜 */
static int zx_ffe_op_del(struct zx_eth_adapter *adapter, char **argv)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_key key;
	struct zx_ffe_flow *flow;
	const char *err = NULL;
	int ret;

	memset(&key, 0, sizeof(key));
	ret = zx_ffe_key_parse(argv, &key, &err);
	if (ret) {
		mutex_lock(&accel->lock);
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "del: %d (%s)", ret, err ? err : "invalid");
		mutex_unlock(&accel->lock);
		return ret;
	}

	mutex_lock(&accel->lock);
	rcu_read_lock();
	flow = rhashtable_lookup(&accel->flows, &key, zx_ffe_ht_params);
	rcu_read_unlock();
	if (!flow) {
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "del: -ENOENT (no such flow)");
		mutex_unlock(&accel->lock);
		return -ENOENT;
	}
	__zx_ffe_flow_del(adapter, flow);
	snprintf(accel->ffe_last, sizeof(accel->ffe_last),
		 "del: ok, flows=%u", accel->n_flows);
	mutex_unlock(&accel->lock);
	return 0;
}

/* ffe_flush <4|6|all> */
static int zx_ffe_op_flush(struct zx_eth_adapter *adapter, const char *arg)
{
	struct zx_accel *accel = &adapter->accel;
	u32 n;
	int family;

	if (!strcmp(arg, "all")) {
		family = -1;
	} else if (!strcmp(arg, "4")) {
		family = 0;
	} else if (!strcmp(arg, "6")) {
		/* P4 起 v6 条目真的会被命中，flush 6 必须和 flush 4 一样能用 */
		family = 1;
	} else {
		return zx_ffe_bad_usage(adapter, "ffe_flush <4|6|all>");
	}

	mutex_lock(&accel->lock);
	n = __zx_ffe_flush(adapter, family);
	snprintf(accel->ffe_last, sizeof(accel->ffe_last),
		 "flush %s: %u flows removed, flows=%u", arg, n,
		 accel->n_flows);
	mutex_unlock(&accel->lock);
	return 0;
}

enum zx_ffe_op {
	ZX_FFE_OP_ADD,
	ZX_FFE_OP_DEL,
	ZX_FFE_OP_FLUSH,
};

static ssize_t zx_ffe_store(struct file *file, const char __user *ubuf,
			    size_t count, int op)
{
	struct seq_file *s = file->private_data;
	struct zx_eth_adapter *adapter = s->private;
	char buf[256];
	char *argv[ZX_FFE_ARG_MAX];
	int argc, ret = -EINVAL;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	argc = zx_ffe_split(buf, argv, ARRAY_SIZE(argv));

	switch (op) {
	case ZX_FFE_OP_ADD:
		if (argc != 10)
			return zx_ffe_bad_usage(adapter,
				"ffe_add <family> <saddr> <daddr> <sport> <dport> <proto> <ipport> <vid> <eport> <dmac>");
		ret = zx_ffe_op_add(adapter, argv);
		break;
	case ZX_FFE_OP_DEL:
		if (argc != 8)
			return zx_ffe_bad_usage(adapter,
				"ffe_del <family> <saddr> <daddr> <sport> <dport> <proto> <ipport> <vid>");
		ret = zx_ffe_op_del(adapter, argv);
		break;
	default:
		if (argc != 1)
			return zx_ffe_bad_usage(adapter,
				"ffe_flush <4|6|all>");
		ret = zx_ffe_op_flush(adapter, argv[0]);
		break;
	}
	return ret ? ret : (ssize_t)count;
}

static ssize_t zx_accel_add_store(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	return zx_ffe_store(file, ubuf, count, ZX_FFE_OP_ADD);
}

static ssize_t zx_accel_del_store(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	return zx_ffe_store(file, ubuf, count, ZX_FFE_OP_DEL);
}

static ssize_t zx_accel_flush_store(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	return zx_ffe_store(file, ubuf, count, ZX_FFE_OP_FLUSH);
}

/* ffe_add/del/flush 的读侧：最后一条操作结果（同一份缓冲区） */
static int zx_accel_last_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;

	(void)data;
	mutex_lock(&accel->lock);
	if (accel->ffe_last[0])
		seq_printf(s, "%s\n", accel->ffe_last);
	else
		seq_puts(s, "(no ffe_add/del/flush yet)\n");
	mutex_unlock(&accel->lock);
	return 0;
}

static int zx_accel_last_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_last_show, inode->i_private);
}

const struct file_operations zx_accel_add_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_last_open,
	.read		= seq_read,
	.write		= zx_accel_add_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

const struct file_operations zx_accel_del_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_last_open,
	.read		= seq_read,
	.write		= zx_accel_del_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

const struct file_operations zx_accel_flush_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_last_open,
	.read		= seq_read,
	.write		= zx_accel_flush_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * ffe_flows：每行
 *   family saddr daddr sport dport proto ipport vid -> eport dmac hits bytes lastuse_ms [S]
 * （P1-TASK §2 的读格式；末尾的 S 表示静态条目，P1-TASK §3。）
 * family 4 的地址用 %pI4，family 6 用 %pI6c（P4-TASK §0.3），token 位置与含义
 * 两族一致。hits/bytes 只统计真正转发出去的帧（P2 起 == fwd）；lastuse 在
 * "转发"与"命中但环满回栈"两种情况下都刷新。读侧持
 * RCU 只读段，条目删除用 list_del_rcu + kfree_rcu。
 */
static int zx_accel_flows_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *flow;

	(void)data;
	rcu_read_lock();
	list_for_each_entry_rcu(flow, &accel->flow_list, list) {
		if (flow->key.family == 6)
			seq_printf(s, "6 %pI6c %pI6c %u %u %u %u %u -> %u %pM %llu %llu %u%s\n",
				   (const struct in6_addr *)&flow->key.v6[0],
				   (const struct in6_addr *)
					&flow->key.v6[sizeof(struct in6_addr)],
				   ntohs(flow->key.sport), ntohs(flow->key.dport),
				   flow->key.proto, flow->key.ipport,
				   flow->key.vid, flow->eport, flow->dmac,
				   flow->hits, flow->bytes,
				   jiffies_to_msecs(jiffies - flow->lastuse),
				   (flow->flags & ZX_FFE_FLOW_STATIC) ? " S" : "");
		else
			seq_printf(s, "4 %pI4 %pI4 %u %u %u %u %u -> %u %pM %llu %llu %u%s\n",
				   &flow->key.v4[0], &flow->key.v4[1],
				   ntohs(flow->key.sport), ntohs(flow->key.dport),
				   flow->key.proto, flow->key.ipport,
				   flow->key.vid, flow->eport, flow->dmac,
				   flow->hits, flow->bytes,
				   jiffies_to_msecs(jiffies - flow->lastuse),
				   (flow->flags & ZX_FFE_FLOW_STATIC) ? " S" : "");
	}
	rcu_read_unlock();
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

/*
 * ffe_aging 的 debugfs 读写（P4 补的测试便利）。上机要动态改老化时间，
 * module_param 只能经 /sys/module/<模块名>/parameters/ 改、名字还依赖模块名；
 * 这里与 module_param 是**同一个变量**（谁写都生效，module_param 保持不动），
 * 读回当前值，写 0 = 不老化。
 */
static int zx_accel_aging_show(struct seq_file *s, void *data)
{
	(void)data;
	seq_printf(s, "%u\n", READ_ONCE(ffe_aging));
	return 0;
}

static int zx_accel_aging_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_aging_show, inode->i_private);
}

static ssize_t zx_accel_aging_store(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct zx_eth_adapter *adapter = s->private;
	unsigned int val;
	int ret;

	ret = kstrtouint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;
	WRITE_ONCE(ffe_aging, val);
	dev_info(adapter->dev, "accel: ffe_aging = %u s (0 = never age out)\n",
		 val);
	return count;
}

const struct file_operations zx_accel_aging_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_aging_open,
	.read		= seq_read,
	.write		= zx_accel_aging_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};
