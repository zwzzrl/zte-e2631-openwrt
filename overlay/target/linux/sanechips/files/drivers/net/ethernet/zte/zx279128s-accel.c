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
 * P0 搭框架；P1 做 shadow 判定：zx_ffe_rx() 只读帧头 + 计数，命中后帧照样走
 * 协议栈（不转发、不改写、不消费描述符），同时把静态流表（ffe_add/del/flush/
 * ffe_flows）、本机 MAC/IP 地址集合、老化 work 立起来。P2 才在 zx_ffe_rx() 里
 * 做真转发。
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
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
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
 * 再被"全部判据通过"的帧命中就删除（设计 §4.1）。P1 的条目都是 ffe_add 下发
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

struct zx_ffe_flow {
	struct rhash_head	node;
	struct list_head	list;		/* flow_list：老化与 ffe_flows 遍历 */
	struct rcu_head		rcu;
	struct zx_ffe_key	key;
	u8			dmac[ETH_ALEN];	/* 出口下一跳 MAC（P2 改写用） */
	u8			smac[ETH_ALEN];	/* 出口端口 MAC 快照（P2 改写用） */
	u8			eport;
	u8			flags;
	u16			mtu;
	u64			hits;		/* 通过全部判据（P2 会转发）的帧数 */
	u64			bytes;
	unsigned long		lastuse;
};

/*
 * 本机地址集合的条目。判据 #2 查 MAC、#13 查 IPv4；dev 用来在地址变化 /
 * 接口注销时把旧值删掉。netdev/inetaddr 通知链是原子的，所以全部操作在
 * accel->local_lock 下，分配用 GFP_ATOMIC，删除用 kfree_rcu（数据面在 RCU
 * 只读段里读）。
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

/* ---- 本机地址集合（判据 #2 / #13）---------------------------------- */

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

/* ---- 判据链（P1 shadow：只判不发）---------------------------------- */

/*
 * 出口端口 → netdev。ports[] 只在 probe 里写一次、没有 remove 路径，所以
 * READ_ONCE 足够；DTS 缺某个 reg 时数组有洞，返回 NULL 必须判。数据面不取
 * rtnl，netif_running/netif_carrier_ok 是良性竞态：判不准就回栈，不丢包。
 */
static bool zx_ffe_eport_usable(struct zx_eth_adapter *adapter, u32 eport)
{
	struct net_device *nd;

	if (eport >= ZX_NUM_PORTS)
		return false;
	nd = READ_ONCE(adapter->ports[eport]);
	/* IFF_UP 不等于能发：link down 时驱动会把 MAC 关掉（zx_mac_link_down） */
	return nd && netif_running(nd) && netif_carrier_ok(nd);
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
 * P1 的判定函数：判"这一帧如果走快路会不会成功"，结果只进计数与条目的
 * hits/bytes。**无论结果如何，调用方都让帧继续走原栈路径**（P1 唯一允许的
 * 数据面副作用是"读 + 计数"）。
 *
 * 判据顺序按设计 §2.2，廉价在前；唯一的偏离是 #13（目的 IP 属本机）提前到
 * #8（L4 协议）之前：这样 ping 本机地址（ICMP）也会记进 drop_local，而不是
 * 在协议判据里被算成普通 miss（P1-TASK §5.3 的验收要求）。
 *
 * 计数口径（P1-TASK §1）：lookup == hit + miss 恒成立；hit 只统计"全部判据
 * 通过"；csum_err/ttl_err/drop_local/drop_noport/fallback 是 miss 的子集，
 * 各自单独计，便于判断是哪条判据把它挡回去的。
 */
bool zx_ffe_rx(struct zx_eth_adapter *adapter, u16 bp, u16 len, int rx_port,
	       int family, __be16 h_proto)
{
	struct zx_accel *accel = &adapter->accel;
	const u8 *frame = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16;
	struct zx_ffe_key key;
	struct zx_ffe_flow *flow;
	struct zx_ffe_cnt *c;
	struct iphdr ip;
	__be16 ports[2];
	u8 dmac[ETH_ALEN];
	u32 eport;

	if (family < 0 || family > 1)
		return false;
	c = &accel->cnt[family];

	if (family != 0) {
		/* IPv6 判定在 P1 是桩：只记一次 miss，恒回栈（设计 §6 留 P4） */
		c->lookup++;
		c->miss++;
		return false;
	}

	c->lookup++;

	dma_rmb();
	memcpy(dmac, frame, ETH_ALEN);

	/* #1 组播/广播：扩散面由桥与协议栈决定，快路不参与 */
	if (is_multicast_ether_addr(dmac))
		goto miss;
	/* #2 目的 MAC 不在本机集合：这帧不是给我们的，交给桥/栈扩散 */
	if (!zx_ffe_is_local_mac(accel, dmac))
		goto miss;
	/* #3 runt（< 64 B） */
	if (len < 64)
		goto miss;
	/* #4 只处理裸 IPv4（ARP/PPPoE/VLAN 外层/IPv6 都不碰） */
	if (h_proto != htons(ETH_P_IP))
		goto miss;
	/* #5 版本/IHL/长度：带选项、被截断的都不碰 */
	memcpy(&ip, frame + ETH_HLEN, sizeof(ip));
	if (ip.version != 4 || ip.ihl != 5 ||
	    ntohs(ip.tot_len) > len - ETH_HLEN)
		goto miss;
	/* #6 IPv4 头校验和（错的帧栈也会丢，快路不背这个锅） */
	if (ip_fast_csum(&ip, ip.ihl))
		goto csum_err;
	/* #7 分片：不重组、不猜偏移 */
	if (ip.frag_off & htons(IP_MF | IP_OFFSET))
		goto miss;
	/* #13（提前）：目的 IP 属本机 = 终结/端口映射，必须走栈 */
	if (zx_ffe_is_local_ip(accel, ip.daddr))
		goto drop_local;
	/* #8 只接 TCP/UDP：ICMP 要回错、IGMP 要上送，其余一律不碰 */
	if (ip.protocol != IPPROTO_TCP && ip.protocol != IPPROTO_UDP)
		goto miss;
	/* #9 TTL 归零必须由栈发 ICMP Time Exceeded */
	if (ip.ttl <= 1)
		goto ttl_err;
	/* 取 L4 端口至少要 4 字节（TCP/UDP 头的前两个字段就是端口） */
	if (ntohs(ip.tot_len) < sizeof(ip) + 4)
		goto miss;
	memcpy(ports, frame + ETH_HLEN + sizeof(ip), sizeof(ports));

	/* #10 查表（自己持 RCU 只读段：条目由 kfree_rcu 释放） */
	memset(&key, 0, sizeof(key));
	key.v4[0] = ip.saddr;
	key.v4[1] = ip.daddr;
	key.sport = ports[0];
	key.dport = ports[1];
	key.proto = ip.protocol;
	key.family = 4;

	rcu_read_lock();
	flow = rhashtable_lookup(&accel->flows, &key, zx_ffe_ht_params);
	if (!flow) {
		rcu_read_unlock();
		goto miss;
	}
	/* #11 出口端口现查现判：不存在/down 都回栈（P1 不转发，回栈不会丢） */
	eport = flow->eport;
	if (!zx_ffe_eport_usable(adapter, eport)) {
		rcu_read_unlock();
		goto drop_noport;
	}
	/* #12 同口进出：不做 hairpin */
	if ((int)eport == rx_port) {
		rcu_read_unlock();
		goto fallback;
	}
	/* 全部判据通过：P1 只记账，帧照样走栈 */
	flow->hits++;
	flow->bytes += len;
	flow->lastuse = jiffies;
	rcu_read_unlock();
	c->hit++;
	return false;

csum_err:
	c->csum_err++;
	goto miss;
ttl_err:
	c->ttl_err++;
	goto miss;
drop_local:
	c->drop_local++;
	goto miss;
drop_noport:
	c->drop_noport++;
	goto miss;
fallback:
	c->fallback++;
miss:
	c->miss++;
	return false;
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
 * P1 是 shadow（只判不发）：hit 的帧并没有真的转发，drop_local/drop_noport
 * 也只是"如果走快路会被挡回去"的投影，不是真实丢包。拿这些影子计数做自动
 * 降级，会把正常的本地终结流量（例如 ping 本机地址）误判成快路故障，所以 P1
 * 里这条判据不启用；P2 接上真转发后把它改成 1 即可复用同一套阈值。
 */
#define ZX_FFE_FORWARD_ENABLED	0

static bool zx_accel_degraded(const struct zx_accel *accel, int family)
{
	const struct zx_ffe_cnt *c = &accel->cnt[family];
	u64 lookup = READ_ONCE(c->lookup);
	u64 drop;

	if (!ZX_FFE_FORWARD_ENABLED)
		return false;
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

	/*
	 * 本机地址集合的两个来源（判据 #2/#13）：netdev 通知链管 MAC（端口/
	 * 桥/wlan 的 dev_addr，含地址变化与上桥下桥），inetaddr 通知链管本机
	 * IPv4 地址。两者都是原子通知链，处理函数里只做 spin_lock + GFP_ATOMIC。
	 * 注册时内核会把现存的接口依次回调一次，probe 之前就存在的 br-lan/
	 * wlan 也能被收进集合。
	 */
	accel->netdev_nb.notifier_call = zx_ffe_netdev_event;
	accel->inetaddr_nb.notifier_call = zx_ffe_inetaddr_event;
	ret = register_netdevice_notifier(&accel->netdev_nb);
	if (ret) {
		dev_err(adapter->dev,
			"accel: register_netdevice_notifier failed: %d\n", ret);
		goto err_ips;
	}
	ret = register_inetaddr_notifier(&accel->inetaddr_nb);
	if (ret) {
		dev_err(adapter->dev,
			"accel: register_inetaddr_notifier failed: %d\n", ret);
		goto err_netdev;
	}

	schedule_delayed_work(&accel->age, msecs_to_jiffies(ZX_FFE_TICK_MS));
	return 0;

err_netdev:
	unregister_netdevice_notifier(&accel->netdev_nb);
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
	unregister_inetaddr_notifier(&accel->inetaddr_nb);
	unregister_netdevice_notifier(&accel->netdev_nb);
	rhashtable_free_and_destroy(&accel->flows, zx_ffe_free_flow, NULL);
	rhashtable_free_and_destroy(&accel->local_macs, zx_ffe_free_lmac, NULL);
	rhashtable_free_and_destroy(&accel->local_ips, zx_ffe_free_lip, NULL);
	INIT_LIST_HEAD(&accel->flow_list);
	INIT_LIST_HEAD(&accel->local_mac_list);
	INIT_LIST_HEAD(&accel->local_ip_list);
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
 * family 只接受 4；6 返回 -EOPNOTSUPP（P1 没有 v6 判定，见 P1-TASK §0）。
 * 调用前 key 必须已清零（rhashtable 的键按字节比较，尾部填充也要一致）。
 */
static int zx_ffe_key_parse(struct zx_eth_adapter *adapter, char **argv,
			    struct zx_ffe_key *key, const char **err)
{
	unsigned int fam, sport, dport, proto, ipport, vid;

	if (kstrtouint(argv[0], 10, &fam) || (fam != 4 && fam != 6)) {
		*err = "family must be 4 or 6";
		return -EINVAL;
	}
	if (fam == 6) {
		dev_info(adapter->dev,
			 "ffe: IPv6 flow keys are not supported yet (P1)\n");
		*err = "IPv6 keys are -EOPNOTSUPP in P1";
		return -EOPNOTSUPP;
	}
	if (!in4_pton(argv[1], -1, (u8 *)&key->v4[0], '\0', NULL) ||
	    !in4_pton(argv[2], -1, (u8 *)&key->v4[1], '\0', NULL)) {
		*err = "bad IPv4 address";
		return -EINVAL;
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
	key->family = 4;
	key->vid = vid;
	return 0;
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
	ret = zx_ffe_key_parse(adapter, argv, &key, &err);
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
	snprintf(accel->ffe_last, sizeof(accel->ffe_last),
		 "add %u %pI4 %pI4 sport=%u dport=%u proto=%u ipport=%u vid=%u -> eport %u ok, flows=%u",
		 key.family, &key.v4[0], &key.v4[1],
		 ntohs(key.sport), ntohs(key.dport), key.proto, key.ipport,
		 key.vid, eport, accel->n_flows);
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
	ret = zx_ffe_key_parse(adapter, argv, &key, &err);
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
		dev_info(adapter->dev,
			 "ffe: IPv6 flush is not supported yet (P1)\n");
		mutex_lock(&accel->lock);
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "flush 6: -EOPNOTSUPP");
		mutex_unlock(&accel->lock);
		return -EOPNOTSUPP;
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
 * hits/bytes 只统计"全部判据通过"的帧——就是 P2 会真正转发的那部分。读侧持
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
		seq_printf(s, "%u %pI4 %pI4 %u %u %u %u %u -> %u %pM %llu %llu %u%s\n",
			   flow->key.family,
			   &flow->key.v4[0], &flow->key.v4[1],
			   ntohs(flow->key.sport), ntohs(flow->key.dport),
			   flow->key.proto, flow->key.ipport, flow->key.vid,
			   flow->eport, flow->dmac,
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
