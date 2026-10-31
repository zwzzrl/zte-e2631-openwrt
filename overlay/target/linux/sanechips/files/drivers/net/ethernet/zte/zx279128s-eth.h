/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ZTE zx279128s Ethernet switch and DMA driver: registers and shared state
 *
 * Copyright (C) 2026 Navid Ghahremani <ghahramani.navid@gmail.com>
 */

#ifndef ZX279128S_ETH_H
#define ZX279128S_ETH_H

#include <linux/bitmap.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/if_ether.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/notifier.h>
#include <linux/phylink.h>
#include <linux/rhashtable.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define DRV_NAME		"zx279128s-eth"

/*
 * Register offsets from the start of the block (0x92000000). The names follow
 * the vendor modules where they are known. Registers that only appear in the
 * vendor's init sequences, with no known meaning, keep their offsets.
 */

/* Global control */
#define ZX_GLB_RESET		0x000008	/* write 0, then all ones */
#define ZX_GLB_CLK_CTRL		0x040018
#define  ZX_GLB_CLK_CTRL_BITS	GENMASK(1, 0)	/* cleared by the vendor code */
#define ZX_GLB_PORT_CLK_EN	0x04001c	/* bit n: clock of MAC port n */

/* Switch core */
#define ZX_SW_PORT_FILTER(p)	(0x1c0060 + 4 * (p))
#define  ZX_SW_PORT_FILTER_BITS	GENMASK(25, 23)	/* cleared for every port */
#define ZX_SW_FWD_CTRL		0x1cc000
#define  ZX_SW_FWD_CTRL_ON	17

/* Switch port attributes (stock tm.ko "spa") */
#define ZX_SPA_UP_REG_PKT_EN	0x1d4000
#define ZX_SPA_DN_REG_PKT_EN	0x1d4040
#define ZX_SPA_TRAP_DMAC	0x1d41a0	/* 8 bytes per entry */
#define  ZX_SPA_TRAP_DMAC_NUM	2		/* entries the driver uses */
#define ZX_SPA_TRAP_PROTO	0x1d41c0
#define ZX_SPA_TRAP_ETH_TYPE	0x1d41c4	/* 4 x 16 bits, big end first */
#define ZX_SPA_PORT_EN(i)	(0x1d428c + 4 * (i))
#define ZX_SPA_ONU_MAC		0x1d4120	/* 16 x 8 bytes: our MACs, routed */
#define  ZX_SPA_ONU_MAC_NUM	16
#define ZX_SPA_REG_PKT_TCP_ACK	BIT(4)		/* word 2: TCP without payload */

/* MAC ports 0-4 */
#define ZX_MAC_BASE		0x200000
#define ZX_PORT_STRIDE		0x040000
#define ZX_MAC_PORT(p)		(ZX_MAC_BASE + (p) * ZX_PORT_STRIDE)
#define ZX_MAC_CTRL		0x000
#define  ZX_MAC_CTRL_EN		GENMASK(1, 0)	/* receive and transmit */
#define  ZX_MAC_CTRL_FD		BIT(13)		/* full duplex */
#define  ZX_MAC_CTRL_100	BIT(14)		/* 100 Mbit/s, with _MII */
#define  ZX_MAC_CTRL_MII	BIT(15)		/* 10 or 100 Mbit/s */

/* MAC port counters: 32 bits, not cleared by reading */
#define ZX_MAC_TX_HIST(i)	(0x724 + 4 * (i))	/* frame size bins */
#define ZX_MAC_TX_MCAST		0x740
#define ZX_MAC_TX_BCAST		0x744
#define ZX_MAC_TX_OCTETS	0x764
#define ZX_MAC_TX_FRAMES	0x768
#define ZX_MAC_RX_OCTETS	0x784
#define ZX_MAC_RX_BCAST		0x78c
#define ZX_MAC_RX_MCAST		0x790
#define ZX_MAC_RX_HIST(i)	(0x7ac + 4 * (i))
#define ZX_MAC_RX_UCAST		0x7c4
#define ZX_MAC_HIST_BINS	6	/* 64, 65-127, ... 1024 bytes and up */

/* Traffic manager */
#define ZX_TM_BASE		0x340000
#define ZX_TM_CTRL		(ZX_TM_BASE + 0x00)
#define  ZX_TM_CTRL_BIT6	BIT(6)		/* set by the vendor code */
#define ZX_TM_CONFIG_START	(ZX_TM_BASE + 0x04)
#define ZX_TM_TAB0_ADDR		(ZX_TM_BASE + 0xe8)
#define ZX_TM_TAB1_ADDR		(ZX_TM_BASE + 0xec)
#define ZX_TM_BUF_F0		(ZX_TM_BASE + 0xf0)
#define ZX_TM_BUF_START		(ZX_TM_BASE + 0xf4)
#define ZX_TM_BUF_END		(ZX_TM_BASE + 0xf8)
#define ZX_TM_TAB_LIMIT		(ZX_TM_BASE + 0xfc)
#define ZX_TM_INT_STATUS	(ZX_TM_BASE + 0x100)
#define ZX_TM_INT_MASK		(ZX_TM_BASE + 0x104)	/* 1: masked */
#define  ZX_TM_INT_RX		GENMASK_U32(1, 0)		/* frames for the CPU */

/* Traffic manager tables, reached through an indirect access window */
#define ZX_TM_IND_CMD		(ZX_TM_BASE + 0x4014)
#define  ZX_TM_IND_INDEX	GENMASK(21, 0)
#define  ZX_TM_IND_TABLE	GENMASK(26, 22)
#define  ZX_TM_IND_READ		BIT(27)
#define ZX_TM_IND_STATUS	(ZX_TM_BASE + 0x4018)
#define  ZX_TM_IND_READY	BIT(0)
#define ZX_TM_IND_DATA(i)	(ZX_TM_BASE + 0x401c + 4 * (i))
#define ZX_TM_TABLE_QUEUE	0
#define ZX_TM_TABLE_QUEUE_USE	1
#define ZX_TM_TABLE_RED		2
#define ZX_TM_TABLE_SCHED	4

/* Release of consumed RX slots */
#define ZX_RX_REL_CTRL		(ZX_TM_BASE + 0x4064)
#define  ZX_RX_REL_BUSY		BIT(0)
#define ZX_RX_REL_DATA		(ZX_TM_BASE + 0x4068)
#define  ZX_RX_REL_QUEUE	GENMASK(2, 0)
#define  ZX_RX_REL_CLASS	BIT(3)
#define  ZX_RX_REL_COUNT	GENMASK(13, 4)

/* Buffer management unit */
#define ZX_BMU_BASE		(ZX_TM_BASE + 0x8000)
#define ZX_BMU_CTRL		(ZX_BMU_BASE + 0x00)
#define  ZX_BMU_CTRL_EN		BIT(0)
#define ZX_BMU_CFG1		(ZX_BMU_BASE + 0x04)
#define ZX_BMU_CFG2		(ZX_BMU_BASE + 0x08)
#define ZX_BMU_BUF_ID		(ZX_BMU_BASE + 0x0c)
#define  ZX_BMU_BUF_ID_VALID	BIT(31)
#define  ZX_BMU_BUF_ID_BP	GENMASK(15, 0)
#define ZX_BMU_BUF_FREE		(ZX_BMU_BASE + 0x10)
#define ZX_BMU_BUF_ALLOC	(ZX_BMU_BASE + 0x14)
#define  ZX_BMU_BUF_ALLOC_REQ	BIT(0)
#define  ZX_BMU_BUF_ALLOC_BUSY	GENMASK(1, 0)
#define ZX_BMU_FREE_STATUS	(ZX_BMU_BASE + 0xdc)
#define  ZX_BMU_FREE_SLOTS	GENMASK(8, 3)
#define ZX_BMU_THRES1		(ZX_BMU_BASE + 0x48)
#define ZX_BMU_THRES2		(ZX_BMU_BASE + 0x4c)
#define ZX_BMU_LIMIT1		(ZX_BMU_BASE + 0x58)
#define ZX_BMU_LIMIT2		(ZX_BMU_BASE + 0x5c)

/* DMA engine between the switch and the CPU */
#define ZX_DMA_BASE		(ZX_TM_BASE + 0x10000)
#define ZX_DMA_CTRL		(ZX_DMA_BASE + 0x00)
#define  ZX_DMA_CTRL_EN		GENMASK(19, 16)
#define  ZX_DMA_CTRL_BIT21	BIT(21)		/* always set, as in the vendor code */
#define ZX_DMA_RX_IRQ_TIMER	(ZX_DMA_BASE + 0x30)	/* in 20 ns ticks */
#define ZX_DMA_RX_IRQ_FRAMES	(ZX_DMA_BASE + 0x34)
#define ZX_DMA_RING_CFG		(ZX_DMA_BASE + 0x3c)
#define ZX_DMA_AREA50		(ZX_DMA_BASE + 0x50)	/* 1 MiB area, use unknown */
#define ZX_DMA_TX_RING		(ZX_DMA_BASE + 0x60)	/* ring of frames to send */
#define ZX_DMA_TX_START		(ZX_DMA_BASE + 0x64)
#define ZX_DMA_RX_QUEUE_CNT(q)	(ZX_DMA_BASE + 0x100 + ((q) * 4))

/* Packet processor */
#define ZX_PP_RESET		0x380000

/* Bridge block (sbrg), ports numbered with ZX_BRPORT() */
#define ZX_SBRG_PORT_CTRL	0x388004
#define ZX_SBRG_FLUSH_PORTS	GENMASK(15, 8)	/* drop the entries of these */
#define ZX_SBRG_FLUSH		BIT(16)
#define ZX_SBRG_IND_CMD		0x388014	/* 27: read, 26-22: memory, 11-0: entry */
#define ZX_SBRG_IND_READ	BIT(27)
#define ZX_SBRG_IND_DONE	0x388018
#define ZX_SBRG_IND_DATA(i)	(0x38801c + 4 * (i))
#define ZX_SBRG_TABLE_SEL	0x388184	/* source table: 4 RAMs of 1024/256/512 */
#define ZX_SBRG_LEARN		0x3881c0	/* ports that learn source MACs */
#define ZX_SBRG_EGRESS(bp)	(0x3883c0 + 4 * (bp))	/* ports bp may send to */
#define ZX_SBRG_MEM_VLAN	4		/* 1 word per VID */
#define ZX_SBRG_VLAN_VALID	BIT(0)
#define ZX_SBRG_VLAN_UNTAG(bp)	(1 << (2 * (bp) + 1))

/* Packet processor tables, reached through an indirect access window:
 * command (index | table << 22 | read << 27), status bit 0 = idle, data.
 * The classifier (CLA) has 17 data words, the packet modifier (PM) 8.
 */
#define ZX_CLA_BASE		0x38c000
#define ZX_PM_BASE		0x39c000
#define ZX_PP_IND_CMD		0x14
#define ZX_PP_IND_STATUS	0x18
#define ZX_PP_IND_DATA		0x1c
#define ZX_PM_IND_DATA_HI	0x100	/* PM data words 4-7 */
#define ZX_PP_IND_READ		BIT(27)

#define ZX_CLA_EXTRA_INDEX	0	/* header fields each packet type extracts */
#define ZX_CLA_EXTRA_RULE	1
#define ZX_CLA_HASH0		2	/* 256 flow entries, CRC-32 slot */
#define ZX_CLA_HASH1		3	/* 128 flow entries, CRC-32C slot */
#define ZX_CLA_AGING		8	/* hit flags, bank 0 then bank 1 */
#define ZX_PM_FLOW		0	/* rewrite of a flow */
#define ZX_PM_NEXT_HOP		1	/* new destination IP and MAC */
#define ZX_PM_CMD		3
#define ZX_PM_SUB		6
#define ZX_PM_SRC_MAC		12	/* source MAC of each subnet */
#define ZX_PP_SNAT_IP(s)	(0x3a0400 + (s) * 4)	/* source IP of each subnet */

#define ZX_PPE_HASH0_SIZE	256
#define ZX_PPE_HASH_SIZE	(256 + 128)
/*
 * The rewrite index in a classifier entry has 7 bits (ZX_CLA_W0_FLOW), and
 * the stock firmware never used more than 125, so only 127 flows (1-127) can
 * be offloaded at a time. More would reuse the rewrite of another flow.
 */
#define ZX_PPE_FLOWS		128
#define ZX_PPE_NEXT_HOPS	512
#define ZX_PPE_SUBNETS		16
/* IPv4 TCP/UDP: extract index 9 and its rule; hashed with this rule id */
#define ZX_PPE_V4_INDEX		9
#define ZX_PPE_V4_RULE		0x98
/* IPv6 TCP/UDP: extract index 10 and its own rule, which extracts 19
 * windows instead of 7.
 */
#define ZX_PPE_V6_INDEX		10
#define ZX_PPE_V6_RULE		0x99
#define ZX_PPE_V4_WINDOWS	7
#define ZX_PPE_V6_WINDOWS	19

/* Word 0 of a classifier hash entry */
#define ZX_CLA_W0_FLOW		GENMASK(31, 25)	/* packet modifier rewrite index */
#define ZX_CLA_W0_FWD		BIT(24)		/* forward to the egress port */
#define ZX_CLA_W0_EGRESS	GENMASK(15, 12)	/* bridge port, ZX_BRPORT() */
#define ZX_CLA_W0_BITS		0x44		/* as in every stock entry */

/* Bridge ports: 0 is the PON side, 1-5 are the switch ports 0-4 */
#define ZX_BRPORT(p)		((p) + 1)

/* DMA buffer pool: 4096 buffers x 2048 bytes = 8 MiB */
#define ZX_NUM_BUFFERS		4096
#define ZX_BUF_SIZE		2048
#define ZX_BUF_POOL_SIZE	(ZX_NUM_BUFFERS * ZX_BUF_SIZE)

/* 零拷贝：隔离环长度、池页引用计数垫高值、未归还 bp 的告警门限 */
/*
 * 隔离环长度取自池子大小：一个 bp 在同一世代里最多出现在环里一项，池子 4096 个，
 * 所以 4096 项的环在数学上装不满，也就不需要“环满就强行归还最老一项”那种
 * 会提前还 bp 的兜底（环满只说明有人漏摘环项，那是 bug，不是常态）。
 */
#define ZX_ZC_Q_LEN		ZX_NUM_BUFFERS
#define ZX_ZC_PARK		(1u << 20)
/*
 * 留在 BMU 外面的 bp 上限。池子 4096 个，光硬件收发环就各占 1024，再算上 qdisc
 * 和协议栈手里的，只留一半给数据面才不会在拥塞时被自己掐住；本机收包被灌满时
 * 靠这道闸退回拷贝路径兜底。
 */
#define ZX_ZC_OUT_MAX		2048
#define ZX_ZC_MIN_LEN		128

/* Free-index tables (written big-endian u16 per U-Boot convention) */
#define ZX_TAB0_ENTRIES		ZX_NUM_BUFFERS
#define ZX_TAB1_ENTRIES		512

/* DMA descriptor rings */
#define ZX_NUM_TX_DESC		1024
#define ZX_TX_QUEUE_LIMIT	256
#define ZX_TX_POLL_US		250	/* see zx_tx_kick() */

/*
 * The RX interrupt comes after this many frames, or this long after the
 * first one.  Larger batches cost less per frame: 500 us receive faster
 * than polling every 1 ms did and still answer sooner.
 */
#define ZX_RX_IRQ_FRAMES	64
#define ZX_RX_IRQ_USECS		500
#define ZX_NUM_RX_DESC		1024
#define ZX_DESC_SIZE		16	/* bytes per descriptor */

/* Layout of the 16 MiB DMA arena */
#define ZX_DMA_ARENA_SIZE	0x1000000
#define ZX_TAB0_OFFSET		0x800000
#define ZX_TAB1_OFFSET		0x808000
#define ZX_RX_OFFSET		0xa00000
#define ZX_AREA50_OFFSET	0xb00000
#define ZX_TX_OFFSET		0xc00000
/* RX ring of queue q at ZX_RX_OFFSET + q * 16 KiB (1024 descriptors) */
#define ZX_RX_QUEUE_STRIDE	0x4000
#define ZX_RX_QUEUES		8
#define ZX_DMA_TX_DONE		(ZX_DMA_BASE + 0x68)

/* ---- 加速路径框架（stack / ffe / hw / zc）-------------------------- */

/*
 * 每个协议族各自选一条加速路径，两个模式字段互相独立：
 *   stack  纯标准栈（默认，驱动什么都不做）
 *   ffe    驱动内零拷贝快路（本设计；P0 只有骨架，不转发）
 *   hw     PPE 硬件流表，只有 IPv4 有；驱动侧只表示"放行 PPE 接管"
 *   zc     既有零拷贝直通：底层是全局机制（一套池页、一个 GRO 位、一个
 *          zx279128s-main.c 的 zc_rx 开关），但**按族生效**——模式为 zc 的
 *          族把自己那一位记进 zc_mask，数据面只把这些族的帧交给零拷贝
 *          路径（见 struct zx_accel.zc_mask 与 zx_eth_rx()）
 * 同一族内 ffe 与 hw 互斥（硬件命中后帧根本不上 CPU，两层改写叠加不可证明），
 * 同一族内 hw 与 zc 也互斥（PPE 与 zc 改写同一族），ffe 与 zc 也互斥（两者
 * 对同一个 bp 的所有权模型冲突），见设计 §5。
 */
enum zx_accel_mode {
	ZX_ACC_STACK = 0,
	ZX_ACC_FFE,
	ZX_ACC_HW,
	ZX_ACC_ZC,
};

/*
 * ffe_last：ffe_add/del/flush 的"最后一条操作结果"缓冲区。P4 起 v6 的两条
 * 地址最长各 39 个字符（%pI6c 的完整形式），一行要能装下两个地址加其余字段，
 * 所以从 160 提到 192。
 */
#define ZX_FFE_LAST_LEN		192

/*
 * 快路计数：cnt[0] 是 IPv4，cnt[1] 是 IPv6，口径见设计 §1.5 / P1-TASK §1。
 * 不变式 lookup == hit + miss：每一帧进判据链时 lookup 加一，真正被转发出去
 * 的才算 hit（所以 P2 起 hit == fwd），其余一律 miss；csum_err/ttl_err/
 * drop_local/drop_noport/fallback/fallback_qfull/rx_drop 都是 miss 的子集，
 * 用来区分是哪条判据把它挡回去的（rx_drop 是"改写后提交失败"的兜底，理论上
 * 不可达）。v6 没有头校验和，所以 cnt[1].csum_err 恒为 0；cnt[1].ttl_err 是
 * hop_limit 到期。
 */
struct zx_ffe_cnt {
	u64	lookup;		/* 进判据链的帧 */
	u64	hit;		/* 通过全部判据并真正转发（P2 起 hit == fwd） */
	u64	miss;		/* 没通过：查表未命中或任一判据不符（含下面的子集） */
	u64	fwd;		/* P2：真正交给发送硬件 */
	u64	drop_noport;	/* miss 的子集：出口端口不存在/down */
	/*
	 * miss 的子集：目的地址属本机（终结）。P5 修订二起口径是**延迟判定**的
	 * 结果：v6 在判据 #10 一律算（无 NAT66，目的地址不会变）；v4 只在
	 *   - 命中条目但该条目没有 ZX_FFE_FLOW_NAT_D（这帧真是给本机终结的），或
	 *   - 非 TCP/UDP 的帧（ICMP 等，走不到查表）上算。
	 * 端口映射 / masquerade 回程的 pre-NAT 目的地址虽然是本机 WAN IP，但条目
	 * 带 DNAT，命中后会被放行并改写，不计 drop_local。详见
	 * zx279128s-accel.c 的 zx_ffe_forward() #13'。
	 */
	u64	drop_local;
	u64	fallback;	/* miss 的子集：查表命中但被同口等判据挡回栈 */
	u64	fallback_qfull;	/* P2：其中因为 TX 环满而回栈 */
	u64	rx_drop;	/* P2：已改写但提交失败（不可达），只能丢帧 */
	u64	csum_err;	/* miss 的子集：IPv4 头校验和错（v6 恒 0） */
	u64	ttl_err;	/* miss 的子集：TTL / hop_limit 到期 */
	u64	bytes;		/* fwd 的字节数 */
	/*
	 * 整环（v4 与 v6 共用）占用峰值：只由 zx279128s-main.c 的
	 * zx_tx_submit_bp() 写进 cnt[0]——那一层拿不到 family，而环本来就
	 * 是两族共享的，按族拆没有意义。所以 cnt[1].qlen_peak 恒 0 是设计
	 * 如此，不是漏写；debugfs 的 ffe_v6 一行也显示 cnt[0] 的值。
	 */
	u32	qlen_peak;
};

/*
 * 自动降级的窗口起点（P7 上机后改口径，design/extensibility.md §6.6；P8 C
 * 改成真 10 s 窗口）：三个字段都是 cnt[] 里对应计数的快照，由
 * zx_accel_fb_window_open() 在"开窗"那一刻写入（进入 ffe 的 S5.1，以及每
 * 次关窗后的下一拍），zx_accel_degraded() 只比"当前 - 窗口起点"。
 *
 * 只有 drop_noport 进快照：drop_local（目的地址属本机、帧正常回栈、一帧没
 * 丢）语义上不是失败，不能当降级依据；它照旧在 cnt[]/debugfs 里计数。
 *
 * 注意这里的"基线"是**窗口**起点，不是"进入 ffe 以来"的起点：窗口到点判过
 * 之后会被下一窗覆盖，历史上限是 ZX_FFE_FB_WINDOW_MS 而不是整段 ffe 时间。
 */
struct zx_ffe_fb_base {
	u64	lookup;
	u64	drop;		/* 只累计 cnt[].drop_noport */
	u64	fallback;
};

/*
 * 快路的包描述符（P6 分层，design/extensibility.md §1.1）：**入口无关**的最小
 * 集，由 ingress op 填、判据链只读，不认识"帧从哪来、到哪去"。
 *
 *   data/len  线性帧（含以太头），与 P2 起一样直接在池槽位里改，不做任何拷贝；
 *   ingress_id  ingress op 自用：switch-port 是 RX 描述符里的交换机端口号
 *             （ports[] 下标，即 DT reg），将来 wifi-rx 是接口索引；
 *   token     ingress op 自用：现在 = RX 的 bp（RX 环描述符持有它，发送由
 *             zx_tx_submit_bp() 接手，不引入第二条 bp 生命周期），将来
 *             wifi-rx 是 struct sk_buff *；
 *   prepared  判据已完成、尚未提交（"先判据、后批量提交"两段式预留，
 *             现在只置位不读，恒为 0 或 1）。
 */
struct zx_ffe_pkt {
	u8		*data;		/* 线性帧起点（以太头） */
	u16		len;
	u16		ingress_id;	/* ingress op 自用：交换机端口号（将来 wifi 接口索引） */
	u8		family;		/* 0=v4, 1=v6 */
	u8		prepared;	/* 判据已完成、尚未提交（两段式提交预留） */
	void		*token;		/* ingress op 自用：现在 = RX 的 bp */
};

/* 入口/出口 op（定义在文件末尾，要等 enum zx_ffe_rx_result 与 zx_ffe_pkt 就位） */
struct zx_ffe_ingress_ops;
struct zx_ffe_egress_ops;
/* 流表条目（只有 zx279128s-accel.c 碰它） */
struct zx_ffe_flow;

/*
 * 旁路学习的每 CPU 候选环（P5-TASK §1.4）：结构体定义在
 * zx279128s-accel.c（只有那个文件读写它），这里只需要一个 percpu 指针。
 */
struct zx_ffe_learn_ring;

struct zx_accel {
	u8			v4_mode;	/* enum zx_accel_mode */
	u8			v6_mode;	/* enum zx_accel_mode */
	/*
	 * 哪些族的帧可以走零拷贝：bit0=IPv4，bit1=IPv6。zc 的底层状态
	 * （adapter->zc_rx、池页、GRO 位）是全局的，但只有 mask 里置了位的族
	 * 才能用；不变式：zc_rx 为真时 zc_mask 必然非零（否则关了 zc 又留着
	 * 位，或开了 zc 却谁也不许用）。只由 zx_accel_set() 在 S4/S5 里改，
	 * 数据面用 READ_ONCE() 读。
	 */
	u8			zc_mask;
	bool			quiesce;	/* 切换中：数据面一律走栈 */
	struct mutex		lock;		/* 串行化模式切换（S0）与流表写操作 */
	struct rhashtable	flows;		/* 单表，family 在键里 */
	/*
	 * 静态流表的遍历/老化走这条 RCU 链表（rhashtable 只负责查表）：
	 * rhashtable 的 walk 不允许与删除并发，而老化、debugfs 的 ffe_flows
	 * 都要"边看边删"，所以所有条目同时挂在 flow_list 上，写侧持
	 * accel->lock，读侧 rcu_read_lock()，删除用 list_del_rcu + kfree_rcu。
	 */
	struct list_head	flow_list;
	u32			n_flows;	/* flow_list 的条目数（持 lock） */
	/*
	 * 数据面的 ingress 分派表（P6 分层，design/extensibility.md §1）：
	 * 现在只有 switch-port 一项（zx279128s-accel.c 的 zx_ffe_ingress_ops[]），
	 * probe 时 zx_accel_init() 指向它。WiFi 接入只需要往那张表里加一项，
	 * RX 入口（zx_ffe_rx）与判据链都不用改。
	 */
	const struct zx_ffe_ingress_ops *ingress;
	/*
	 * ffe_migrate 最近一次快照到的 PPE 绑定条数（P3 的 hw→ffe 迁移桥是
	 * 最小可用版：只统计与打印，不落表，见 zx279128s-accel.c 的
	 * zx_accel_migrate_store()）。v4 从 hw 切到 ffe 时拿它跟当前 FFE 条数
	 * 对照，提示"还有多少条要手工搬"。只由 ffe_migrate 写，READ_ONCE 读。
	 */
	u32			mig_ppe_flows;
	u64			tbl_full;	/* 表满（1024）拒绝的 add 次数 */
	/*
	 * 旁路学习（P5-TASK §1）：POST_ROUTING 钩子只看不抓帧，把"可学的流"
	 * 写进每 CPU 的无锁环（learn_rings），由 age work 每秒排空、查邻居、
	 * 落表。开关默认关（learn_on = 0）：不开时钩子第一行就返回，零副作用。
	 *
	 * learn_cand/add/dup/drop/noneigh/nobrport/nonport/dedup 八个计数全是
	 * "尽力而为"的裸 u64++：钩子在软中断里跑、可能并发，丢几个计数不影响
	 * 语义（与 cnt[] 同一口径）。
	 *
	 * 桥出口（P5 修订一）：WAN→LAN 的出口是 br-lan 而不是 lanX，钩子只看
	 * 到桥、拿不到交换机端口号，所以候选里带一个哨兵 + 桥的 ifindex，
	 * FDB 反查（要 rtnl）留到 age work 的 drain 里做；解析不出本驱动端口
	 * 就整条放弃，绝不猜（无线口也在"放弃"里，见 learn_nonport）。
	 *
	 * 两个方向都学（P5 修订二）：连接的 ORIGINAL 与 REPLY 两侧各生成一条
	 * 候选，键都是"该方向报文在链路上的 pre-NAT 形态"（ORIGINAL 取
	 * tuple[ORIGINAL]、REPLY 取 tuple[REPLY]），所以家庭流量的下载方向
	 * （REPLY，WAN→LAN 的 bulk）现在也能进表；依据见
	 * zx279128s-accel.c 的 zx_ffe_learn_hook() 头注释。
	 *
	 * 回程方向学不到的两个阻塞（P9）：桥端口进来的帧 skb_iif 已被改写成
	 * 桥的 ifindex，钩子的入口快路径对不上 ports[]；GRO 合并后的 bulk 回程
	 * 是 GSO skb，旧代码一律拒绝。现在入口改成"源 MAC + 桥 ifindex → drain
	 * 里 FDB 反查"，GSO 放开成"TCP/UDP + 无分片 + 头在线性区"。**没有新增
	 * 计数**：入口反查失败并入 learn_nonport（口径见下）。
	 */
	struct zx_ffe_learn_ring __percpu	*learn_rings;
	struct nf_hook_ops	learn_hook;	/* pf=IPv4, POST_ROUTING, NF_IP_PRI_LAST */
	u8			learn_on;	/* debugfs ffe_learn：0/1，默认 0 */
	u64			learn_cand;	/* 钩子看到的候选（含被丢的） */
	u64			learn_add;	/* 真正插进表的学习条目 */
	u64			learn_dup;	/* 同 key 已存在：原地更新 dmac/smac/MTU/NAT/lastuse */
	u64			learn_drop;	/* 环满丢掉的候选（绝不阻塞数据面） */
	u64			learn_noneigh;	/* 候选拿不到下一跳 MAC */
	u64			learn_nobrport;	/* 桥出口解析不出本驱动端口：FDB 未命中/要泛洪/桥已不在 */
	u64			learn_nonport;	/* 命中的不是本驱动交换机口：出口 FDB 命中无线口等（P5），
						 * 以及 P9 起**入口**桥的源 MAC 反查失败/命中无线口 */
	u64			learn_dedup;	/* 1 s 内同一五元组的重复候选，未再投递 */
	/*
	 * ffe_add 命中同 key 的次数（P5 修订二 §C 的幂等化）：这次 add 走的是
	 * "原地更新"而不是"新插入"，表里不会多出一行。与学习无关，放在这一组
	 * 里只是因为 ffe_learn 是唯一常开的计数行（见 zx_accel_learn_show()）。
	 */
	u64			add_replace;
	struct zx_ffe_cnt	cnt[2];
	/*
	 * 自动降级的窗口起点（P7 上机后改口径，design/extensibility.md §6.6；
	 * P8 C 改成真 10 s 窗口）：fb_base[] 是该窗口起点的 cnt[] 快照，
	 * fb_since[] 是窗口起点时刻，两者一起由 zx_accel_fb_window_open() 写
	 * （进入 ffe 的 S5.1 以及每次关窗后的下一拍）。zx_accel_degraded() 用
	 * "当前 - fb_base"判降级，所以判据只看最近一个 10 s 窗口，不背开机
	 * 以来、也不背进入 ffe 以来的历史计数。drop 只对 drop_noport 取快照：
	 * drop_local 是"目的地址属本机、帧正常回栈"，不是失败，不进分子。
	 */
	struct zx_ffe_fb_base	fb_base[2];
	/*
	 * 本机地址集合，只服务于判据 #2（目的 MAC）、#13（目的 IPv4）与 v6 判据
	 * 链的 #10（目的 IPv6）：local_macs 的键是 u8[ETH_ALEN]，local_ips 的键
	 * 是 __be32，local_ip6s 的键是 struct in6_addr（16 B）。三张表与三条
	 * 链表都由 local_lock 保护（netdev/inetaddr/inet6addr notifier 都是原子
	 * 通知链，不能在这里睡觉）；数据面查表只走 RCU。
	 */
	struct rhashtable	local_macs;
	struct rhashtable	local_ips;
	struct rhashtable	local_ip6s;
	spinlock_t		local_lock;
	struct list_head	local_mac_list;
	struct list_head	local_ip_list;
	struct list_head	local_ip6_list;
	struct notifier_block	netdev_nb;
	struct notifier_block	inetaddr_nb;
	struct notifier_block	inet6addr_nb;
	/* 端口全 down（open_count → 0）后清表；rtnl 里不能取 accel->lock，转 work */
	struct work_struct	port_flush_work;
	char			ffe_last[ZX_FFE_LAST_LEN];	/* 最后一条 add/del/flush 结果 */
	struct delayed_work	age;		/* 1 s：老化 + 自动降级判据 */
	u64			sw_in;		/* 进入切换的次数 */
	u64			sw_out;		/* 切换完成（或回滚）的次数 */
	u64			drain_ms;	/* 最近一次排空的耗时 */
	u64			drain_frames;	/* 最近一次排空回收的帧数 */
	bool			autofallback;	/* 允许自动降级回 stack */
	/*
	 * 每族当前降级窗口的起点（jiffies），0 = 现在没有在跑的窗口，下一次
	 * age tick 会开一个（P8 C）。只有 zx_accel_fb_window_open() 写、只有
	 * zx_accel_autofallback() 清零（切换离开 ffe / 窗口判完），都无锁。
	 */
	unsigned long		fb_since[2];
};

/* 某个族的当前模式。数据面每个包只读这一个字节：切换是单字节写，
 * 所以只会看到旧模式或新模式，没有半切换（设计 §1.4 S5）。
 */
static inline u8 zx_accel_cur_mode(const struct zx_accel *accel, int family)
{
	/* READ_ONCE() 要取地址，只能作用在左值上，不能包整个条件表达式 */
	return family ? READ_ONCE(accel->v6_mode) : READ_ONCE(accel->v4_mode);
}

struct zx_eth_adapter;

/* One net_device per switch port: ports 0-3 LAN, port 4 WAN */
#define ZX_NUM_PORTS		5
/* TM egress queue of switch port p (as in the vendor driver) */
#define ZX_PORT_TX_QUEUE(p)	(40 + (p))
/* MACs the Linux bridge moved away from a switch port, queued for removal */
#define ZX_SW_FDB_QUEUE		16

/* The MAC counters the driver keeps, see zx_mib_regs[] */
enum zx_mib {
	ZX_MIB_TX_OCTETS,
	ZX_MIB_TX_FRAMES,
	ZX_MIB_TX_MCAST,
	ZX_MIB_TX_BCAST,
	ZX_MIB_TX_HIST,
	ZX_MIB_RX_OCTETS = ZX_MIB_TX_HIST + ZX_MAC_HIST_BINS,
	ZX_MIB_RX_UCAST,
	ZX_MIB_RX_MCAST,
	ZX_MIB_RX_BCAST,
	ZX_MIB_RX_HIST,
	ZX_MIB_NUM = ZX_MIB_RX_HIST + ZX_MAC_HIST_BINS,
};

struct zx_eth_priv {
	struct net_device	*netdev;
	struct zx_eth_adapter	*adapter;
	int			port;
	struct phylink		*phylink;
	struct phylink_config	phylink_config;

	/* MAC counters, summed up into 64 bits (under adapter->mib_lock) */
	u64			mib[ZX_MIB_NUM];
	u32			mib_last[ZX_MIB_NUM];	/* last register values */
};

struct zx_eth_adapter {
	struct device		*dev;
	void __iomem		*base;
	struct napi_struct	napi;
	struct hrtimer		tx_timer;

	struct net_device	*ports[ZX_NUM_PORTS];
	int			open_count;
	int			irq;

	/* DMA arena remains allocated across down/up */
	void			*buffer_pool;
	dma_addr_t		buf_pool_dma;

	/*
	 * Packet buffers live in their own cacheable allocation, mapped with
	 * the streaming API - not inside the coherent arena above.  The BMU
	 * writes frames straight into this pool; reading them out of uncached
	 * memory cost 8.2 us per 1500 B frame (measured with /dev/mem:
	 * 184 MB/s, against 0.48 us for the same copy in cached memory), which
	 * was the largest single per-packet cost on the software forwarding
	 * path.  Every access is bracketed by
	 * dma_sync_single_for_{cpu,device}() instead.
	 */
	void			*pkt_pool;
	dma_addr_t		pkt_pool_dma;
	bool			pkt_pool_cacheable;
	void			*tx_ring;
	dma_addr_t		tx_ring_dma;
	void			*rx_ring;
	dma_addr_t		rx_ring_dma;
	void			*tab0;
	dma_addr_t		tab0_dma;
	void			*tab1;
	dma_addr_t		tab1_dma;

	u32			tx_cur_idx;
	u32			tx_pending;
	u32			tx_complete_cnt;
	u32			tx_done_excess;
	u32			bmu_free_slots;
	u32			bmu_alloc_fail;
	u32			bmu_alloc_late;		/* buffers of timed-out requests */
	bool			bmu_alloc_pending;
	u32			bmu_free_fail;
	u32			rx_cur_idx[ZX_RX_QUEUES];
	bool			rx_synced[ZX_RX_QUEUES];
	u32			rx_resync_cnt;
	u32			rx_alias_cnt;
	u32			rx_nobuf_cnt;
	u32			rx_rel_fail;
	u32			rx_bad_cnt;

	/*
	 * 每包耗时分解（临时调试用）。收包五段：解析描述符、缓存无效化、
	 * memcpy、上送协议栈（napi_gro_receive）、归还 BMU 缓冲；发包四段：
	 * 回收完成、申请 BMU 缓冲、拷贝+同步、写 TX_START。dma_state 打印
	 * 平均值，用来定位转发那 20 µs 到底花在哪一步。
	 */
	u64			rx_stage_ns[5];
	u32			rx_stage_n;
	u64			tx_stage_ns[4];
	u32			tx_stage_n;

	/*
	 * 零拷贝。zc_state[bp] 记录一个池缓冲归谁：FREE（在 BMU 空闲表里）、
	 * SKB（在协议栈手里，还没人 orphan）、SHADOW（我们在 dataref 上拿了一份
	 * 影子引用，等它掉到 1）、TX（已交给发送硬件，硬件发完自己归还）、
	 * LOST（数据被 expand 搬走，追不到，永不归还）。zc_skb[bp] 是当前主人，
	 * 用来识别陈旧 tag。详见 zx279128s-main.c 里 zx_zc_* 的说明。
	 */
	bool			zc_rx;
	u8			*zc_state;
	struct sk_buff		**zc_skb;
	spinlock_t		zc_lock;
	/* 隔离环只存槽位号：影子引用挂在 skb_shared_info->dataref 上，不要 skb */
	u32			zc_q_bp[ZX_ZC_Q_LEN];
	u32			zc_q_head;
	u32			zc_q_tail;
	u32			zc_q_n;
	u64			zc_rx_ok;
	u64			zc_tx_ok;
	u64			zc_free_ok;
	u64			zc_fallback;
	u64			zc_q_ovf;
	u64			zc_capped;
	u64			zc_off_cnt;
	u64			zc_shadow;
	u64			zc_lost;
	u64			zc_stale;
	u64			zc_odd;
	/* 调试：交付快照与"槽位被改写"/"发送拿到挂着影子的 bp"计数 */
	u8			*zc_snap;
	u16			*zc_snap_len;
	u64			zc_corrupt;
	u64			zc_tx_dup;
	u64			zc_notag;
	u64			zc_rx_dup;	/* 硬件给的 bp 还挂着影子/在协议栈手里 */
	u64			zc_badfree;	/* 调试：归还了 state 还是 SHADOW/SKB 的 bp */
	u32			(*zc_free_at)[2];	/* 每个 bp 最近两次归还的调用点 */
	u8			*zc_free_tog;
	/* 每个 bp 的事件流水：8 项 × (type, aux, tick) */
	u32			(*zc_ev)[8];
	u8			*zc_ev_n;
	unsigned long		*zc_inring;	/* 位图：这个 bp 现在在环里 */
	u64			zc_dup_push;
	u64			zc_dup_reap;
	u64			zc_badstate;	/* 环项对应的 bp 状态已经不是 SHADOW */
	u64			zc_rxdrop;	/* 硬件给的 bp 还有人用，整帧丢弃 */
	unsigned long		zc_park_at;

	/* TX ring, BMU buffer allocation and the TX counters */
	spinlock_t		tx_lock;
	/* BMU free register and bmu_free_slots */
	spinlock_t		bmu_free_lock;

	/* MAC counters of all ports, read before their 32 bits wrap */
	struct mutex		mib_lock;
	struct delayed_work	mib_work;

	/* Where zx_eth_for_each_adapter() finds this adapter, for the packet
	 * processor module: it is loaded long after probe, and has to program
	 * the tables of adapters it did not get a probe callback for.
	 */
	struct list_head	list;

	/* Hardware flow offload (packet processor NAT) */
	struct mutex		ppe_lock;
	bool			ppe_active;
	struct rhashtable	ppe_flows;
	DECLARE_BITMAP(ppe_hash_used, ZX_PPE_HASH_SIZE);
	DECLARE_BITMAP(ppe_flow_used, ZX_PPE_FLOWS);
	DECLARE_BITMAP(ppe_nh_used, ZX_PPE_NEXT_HOPS);
	struct {
		u8	mac[ETH_ALEN];
		__be32	ip;
		int	refs;
	} ppe_subnet[ZX_PPE_SUBNETS];
	u32			ppe_saved[4];
	u32			ppe_count;
	u32			ppe_add_skip;	/* flows the hardware cannot take */
	u32			ppe_add_fail;	/* no free entry, access timeout */

	/* 加速路径框架（见文件上方 enum zx_accel_mode / struct zx_accel） */
	struct zx_accel		accel;

	/* Hardware LAN switching between the ports of one Linux bridge */
	spinlock_t		trap_lock;	/* zx_update_traps() */
	struct mutex		sw_lock;
	bool			sw_ready;	/* bridge notifications registered */
	u8			sw_members;	/* bridge ports that switch */
	struct work_struct	sw_work;
	spinlock_t		sw_fdb_lock;	/* the fields below */
	struct net_device	*sw_bridge[ZX_NUM_PORTS];
	bool			sw_recompute;
	u8			sw_flush;	/* bridge ports to flush */
	bool			sw_fdb_overflow;
	unsigned int		sw_fdb_count;
	u8			sw_fdb_mac[ZX_SW_FDB_QUEUE][ETH_ALEN];
	u32			sw_fdb_deleted;
	struct notifier_block	sw_netdev_nb;
	struct notifier_block	sw_switchdev_nb;
	struct notifier_block	sw_switchdev_blocking_nb;

	/*
	 * SoC 温度传感器（debugfs soc_temp，见 zx279128s-accel.c 那一节）。
	 * 懒映射：**probe 不碰传感器**，第一次读 soc_temp 时才 devm_ioremap
	 * 0x94100000 一页并做一次性初始化，所以没人读过时这两个字段一直是空的。
	 */
	void __iomem		*soc_temp_base;	/* 已挪到 0x94100034 */
	bool			soc_temp_inited;/* 一次性使能/detector 配置已做过 */
};

/* MAC in the layout of the switch's MAC registers: bytes 2-5, then 0-1 */
static inline void zx_mac_words(const u8 *mac, u32 *w)
{
	w[0] = mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5];
	w[1] = mac[0] << 8 | mac[1];
}

/* zx279128s-main.c */
bool zx_eth_is_port(const struct net_device *dev);
void zx_set_trap_dmac(struct zx_eth_adapter *adapter, int idx, const u8 *mac);
void zx_update_traps(struct zx_eth_adapter *adapter);
/* Whether the engine has the IPv6 classifier tables in the hardware.  Only
 * then may the IPv6 ethertype trap be lifted: without tables, an IPv6 frame
 * that the classifier does not know has no path to the CPU left.  Set by the
 * engine (the module) around zx_update_traps().
 */
void zx_ppe_set_v6_offload(bool on);
void zx_update_mac_tables(struct zx_eth_adapter *adapter);
bool zx_ppe_has_bound_blocks(struct zx_eth_adapter *adapter);
/* Visit every probed adapter.  For the packet processor module, which is
 * loaded after the platform devices have probed.  The callback runs without
 * the adapter list lock and may sleep; it must not probe or remove a device.
 */
void zx_eth_for_each_adapter(void (*fn)(struct zx_eth_adapter *adapter,
				       void *data), void *data);
/* Reclaim the sent frames the DMA reported; the caller holds tx_lock. */
void zx_tx_reclaim(struct zx_eth_adapter *adapter);
/*
 * Hand the frame in pool buffer bp to the egress TM queue of eport: descriptor,
 * one TX_START, the ring accounting and the queue-stop throttle.  The caller
 * holds tx_lock and owns bp.  Returns false - nothing written at all - when the
 * ring is full, so the caller may still hand the untouched frame to the stack.
 */
bool zx_tx_submit_bp(struct zx_eth_adapter *adapter, int eport, u16 bp,
		     unsigned len);
/* Turn the zero-copy path on/off.  It takes rtnl_lock() itself, so the caller
 * must not hold rtnl (zx_accel_set() relies on that).
 */
void zx_zc_set(struct zx_eth_adapter *adapter, bool on);

/* The packet processor engine (zx279128s-ppe.h): the switch driver is built
 * in, the engine is the kmod.  Everything that crosses that boundary - the
 * hooks table, the flow table parameters and the entry type - is declared
 * there.
 */

/* zx279128s-accel.c: one acceleration path per protocol family.  The design
 * and the phase plan are in docs/ffe-fastpath-design-2026-11-01.md.
 */
int zx_accel_init(struct zx_eth_adapter *adapter);
void zx_accel_exit(struct zx_eth_adapter *adapter);
const char *zx_accel_mode_name(u8 mode);
int zx_accel_set(struct zx_eth_adapter *adapter, int family, u8 new_mode);
bool zx_accel_autofallback(struct zx_eth_adapter *adapter);
/* 0→1 / →0 open_count hooks; the caller holds rtnl. */
void zx_accel_on_open(struct zx_eth_adapter *adapter);
void zx_accel_on_close(struct zx_eth_adapter *adapter);
/* Ethernet type to family: 0 = IPv4, 1 = IPv6, -1 = the fast path passes. */
int zx_eth_family(__be16 proto);
/*
 * zx_ffe_rx() 的结果（P2 起真转发）。调用方 zx_eth_rx() 据此决定收尾：
 *   STACK     没转发，帧一个字节都没动：照原路径走 zc/skb/GRO
 *   FORWARDED 已写进发送描述符：只做 RX 收尾，**不许**还 bp（硬件发完自己还）
 *   DROPPED   在快路里确定丢了：做 RX 收尾并把 bp 还给 BMU
 */
enum zx_ffe_rx_result {
	ZX_FFE_RX_STACK = 0,
	ZX_FFE_RX_FORWARDED,
	ZX_FFE_RX_DROPPED,
};
/*
 * P2 真转发：按设计 §2.2 的判据链判断，全部通过就把**同一个 bp** 写进发送
 * 描述符（zx_tx_submit_bp()）交给硬件，帧不再进 zc/skb/协议栈。判据不符一律
 * 返回 STACK 且帧完好无损；数据面不消费描述符、不还 bp，收尾由调用方按返回
 * 值做（收尾对账见 zx279128s-main.c 的 ffe_done 标签）。
 *
 * P4 起按 family 分派判据链（v4/v6 各一条），提交段只有一份。
 * P6 起这一层只剩"建包描述符 + 调 ingress op"（design/extensibility.md §1）：
 * 判据链、改写、提交在 ingress->run()，裁决→计数/RX 结果在 ingress->complete()。
 * 帧的以太类型不再由这里转交（ingress op 自己从 data + 2*ETH_ALEN 读，同一个值）。
 */
enum zx_ffe_rx_result zx_ffe_rx(struct zx_eth_adapter *adapter, u16 bp, u16 len,
				int rx_port, int family);

/*
 * 判据链的裁决（P6 起由 ingress op 的 run() 返回）。两条链（v4/v6）只报告
 * "这一帧该被怎么处理"，所有计数集中在 ingress op 的 complete() 出口做一次
 * ——两族的口径只有一份，不会出现"v6 的 miss 忘了加上某个子集"这种对不上的账。
 *
 * 比 enum zx_ffe_rx_result 细一档是必要的：RX 结果只有三态，而计数要区分是
 * 哪条判据把人挡回去的（csum_err / ttl_err / drop_local / drop_noport /
 * fallback / qfull / rx_drop 都是 miss 的子集）。
 */
enum zx_ffe_verdict {
	ZX_FFE_V_OK = 0,	/* 判据链全部通过，可以查表/提交 */
	ZX_FFE_V_FWD,		/* 已改写并提交给发送硬件（只有 forward 返回它） */
	ZX_FFE_V_MISS,		/* 普通 miss：某条判据不符或查表未命中 */
	ZX_FFE_V_CSUM_ERR,	/* 只有 v4：IPv4 头校验和错 */
	ZX_FFE_V_TTL_ERR,	/* TTL/hop_limit <= 1：由栈回 ICMP(6) Time Exceeded */
	ZX_FFE_V_DROP_LOCAL,	/* 目的地址属本机：v6 #10 一律终结；v4 只在"命中条目
				 * 但该条目没有 DNAT 改写"或非 TCP/UDP 的 ICMP 上
				 * 报（P5 修订二 §B 的延迟判定）
				 */
	ZX_FFE_V_DROP_NOPORT,	/* 出口端口不存在 / !running / !carrier */
	ZX_FFE_V_FALLBACK,	/* 命中但按判据回栈（同口/NAT 缺字段/帧长/出口 MTU） */
	ZX_FFE_V_QFULL,		/* 命中但发送环满：回栈让 qdisc 背压 */
	ZX_FFE_V_RX_DROP,	/* 已改写但提交失败，只能丢帧（不可达） */
};

/*
 * 入口（ingress）op（design/extensibility.md §1.1）：**谁把帧交进来**。
 *   run()      入参适配 + 判据链 + 改写 + 提交，返回裁决；
 *   complete() 收尾：裁决 → cnt[family] 的字段 → 调用方要的 RX 结果。switch-port
 *              的 bp/描述符仍由调用方（zx_eth_rx）持有，complete() 不碰它；
 *              将来 wifi-rx 的"消费描述符 / 释放 skb"就在这一层做。
 *
 * 现在只注册 switch-port 一项（zx279128s-accel.c 的 zx_ffe_ingress_ops[]），
 * accel->ingress 指向它。WiFi 接入时加一项即可：判据链、流表、老化、学习全部复用。
 */
struct zx_ffe_ingress_ops {
	const char *name;
	enum zx_ffe_verdict (*run)(struct zx_eth_adapter *adapter,
				   struct zx_ffe_pkt *pkt);
	enum zx_ffe_rx_result (*complete)(struct zx_eth_adapter *adapter,
					  struct zx_ffe_pkt *pkt,
					  enum zx_ffe_verdict verdict);
};

/*
 * 出口（egress）op（design/extensibility.md §1.1/§5 第 1 条）：**谁把帧发出去**。
 * 流表条目只记 type + id，提交按 type 分派；未注册的类型一律回栈（不猜）。
 *   usable()  这个出口现在能不能发（交换机口 = 存在 + running + carrier）；
 *   submit()  按批提交（现在恒 n == 1，见 §5 第 1 条）。调用者已经持 tx_lock、把帧
 *             改写完，并做过容量检查；返回非 0 = 这一帧已经改过、但提交不
 *             出去，只能丢（调用方返回 DROPPED，把 bp 还给 BMU）。
 */
struct zx_ffe_egress_ops {
	const char *name;
	u8	type;
	bool	(*usable)(struct zx_eth_adapter *adapter, u16 id);
	int	(*submit)(struct zx_eth_adapter *adapter,
			  struct zx_ffe_pkt **pkts, int n,
			  struct zx_ffe_flow *flow);
};
/* debugfs: accel_ipv4, accel_ipv6, ffe_flows, ffe_add/del/flush, ffe_aging,
 * ffe_migrate, ffe_learn, accel_autofallback, soc_temp
 */
extern const struct file_operations zx_accel_v4_fops;
extern const struct file_operations zx_accel_v6_fops;
extern const struct file_operations zx_accel_flows_fops;
extern const struct file_operations zx_accel_add_fops;
extern const struct file_operations zx_accel_del_fops;
extern const struct file_operations zx_accel_flush_fops;
extern const struct file_operations zx_accel_aging_fops;
extern const struct file_operations zx_accel_migrate_fops;
extern const struct file_operations zx_accel_learn_fops;
extern const struct file_operations zx_accel_autofallback_fops;
extern const struct file_operations zx_accel_soc_temp_fops;

/* zx279128s-switch.c */
void zx_sw_init(struct zx_eth_adapter *adapter);
void zx_sw_work(struct work_struct *work);
void zx_sw_port_down(struct zx_eth_adapter *adapter, int port);

#endif
