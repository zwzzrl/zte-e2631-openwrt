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
#include <linux/bitops.h>	/* DIAG: fls()/rol32() */
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
 * M2：ZX_PPPOE_ETH_P_SESSION/DISCOVERY、zx_pppoe_gate（门控读侧别名）与
 * zx_ffe_pppoe_sniff() 的声明。这个头文件在 __KERNEL__ 下只借
 * <linux/types.h>，不会把额外内核头带进来。
 */
#include "zx279128s-pppoe.h"

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
 * P10.5 A4：归还 bp 时毒化帧首 16 B 里的 8 B（dmac 6 B + 类型后 2 B），
 * RX 侧读到毒值就记 rx_poison 并回栈（不是静默丢）。默认开：它只影响
 * "已经被归还"的槽位，命中时也不丢帧，留着才能在上机时把"内容没被硬件
 * 重写"这件事变成可计数的证据。source-MAC 与以太类型故意不毒（见
 * zx_bp_poison_write()），所以 P10.4 的 alias 分子与分族一个字节都不变。
 * 关掉的口子留给性能 A/B。
 */
static bool ffe_poison = true;
module_param(ffe_poison, bool, 0644);
MODULE_PARM_DESC(ffe_poison, "Poison the first 16 B of a bp on return to the BMU and count poisoned RX frames (default: on)");

/*
 * P10.5 A3：RX 拿到仍在发送环里的 bp 时**拦投递**（丢帧、不归还）。C1 起默认开：
 * 它是"同一个 bp 被写进两个发送描述符"的第一道闸，误判窗口只有"硬件已完成、
 * reclaim 还没记到"那一段（soak 实测 8/6935 万帧）；写 0 可关掉做 A/B。
 */
static bool ffe_bp_drop = true;
module_param(ffe_bp_drop, bool, 0644);
MODULE_PARM_DESC(ffe_bp_drop, "Drop RX frames whose bp is still in the TX ring (A3, default: on)");

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

/*
 * P1（tx-lock-scope）：发送环深度限值（原 zx279128s-eth.h 的
 * 默认 1024 是**实测/经验取值 + 环长上限**，不是“对齐原厂”：原厂
 * txq_max=0x400 是 per-CPU 软件 backlog 的 b->qlen，不是 DMA 环占用
 * （PIPELINE.md Q3），量纲不同（它满了就丢帧；我方 1024 是整环无余量），
 * 所以只作参考、不作依据。本板实测 tx_pending 峰值 64（限 256 时），
 * 首次上机建议先 256 跑一轮、再切 1024 做 A/B。
 *
 * 为什么用 module_param_cb 而不是裸 module_param：环长只有 ZX_NUM_TX_DESC
 * (1024)，写进一个更大的值会让 tx_pending 越过环长、描述符在飞时被覆盖，
 * 所以写入口直接拒掉 [ZX_TX_QUEUE_MIN, ZX_NUM_TX_DESC] 以外的值（-EINVAL）。
 * sysfs 名字仍是 txq_limit：
 *   echo {64,128,256,512,1024} > /sys/module/zx279128s_eth/parameters/txq_limit
 * 变量名带 zx_ 前缀，免得给 vmlinux 添一个可能撞名的全局 txq_limit。
 * 模块是内置的，但 __param 段照样让 param_sysfs_builtin() 建出这个 sysfs 项。
 */
unsigned int zx_txq_limit = ZX_TX_QUEUE_DEFAULT;

static int zx_txq_limit_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v < ZX_TX_QUEUE_MIN || v > ZX_NUM_TX_DESC)
		return -EINVAL;
	WRITE_ONCE(zx_txq_limit, v);
	return 0;
}

static const struct kernel_param_ops zx_txq_limit_ops = {
	.set	= zx_txq_limit_set,
	.get	= param_get_uint,
};

module_param_cb(txq_limit, &zx_txq_limit_ops, &zx_txq_limit, 0644);
MODULE_PARM_DESC(txq_limit, "TX ring depth limit, 64..1024 (default: 1024, ring length; A/B knob)");

/* ---- 剂量-响应对照：纯延迟注入（delay-instr，2026-11-03） --------------
 *
 * 诊断镜像里唯一把单流从 ~740 抬到 ~863 Mbit/s 的开关是 diag_seq，但它同时
 * 带来两样东西：(A) 每帧固定的额外耗时（改变投递节奏）；(B) 读包头 + 写
 * 1024 槽哈希表带来的缓存/内存流量。这两个参数把 (A) 单独拿出来做成可调
 * 剂量：非 0 时在热路径上**只**延迟，不碰帧、不查表、不写任何诊断状态。
 *
 * 两个插入点**互相独立**（RX 与 TX 是两种机制：TX 那次在 tx_lock + RCU 临界
 * 区里，延长它会把下一帧的提交与 flow 记账一起推后），可以只开一侧。
 *
 * 单位纳秒（u32）。延迟用内核校准过的 udelay()/ndelay()，**不再每圈取时间**：
 *   - 1 <= ns < 1000：ndelay(ns)。本树（ARM）没有架构自己的 ndelay，
 *     linux/delay.h 的通用实现是 udelay(DIV_ROUND_UP(ns,1000))，
 *     所以实际最小非 0 延迟 = 1 us，请求值在 (0,1000] 都落到 1 us；
 *   - ns >= 1000：udelay(DIV_ROUND_UP(ns,1000))，粒度 1 us、向上取整。
 * 默认 0 = 只多一次 unlikely 分支（RX 每帧、TX 每次提交），行为不变。
 * 运行期可写，不需要重启：
 *   echo 2000 > /sys/module/zx279128s_eth/parameters/zxdly_rx_ns
 *   echo 2000 > /sys/module/zx279128s_eth/parameters/zxdly_tx_ns
 * 剂量按 us 排（1000/2000/4000/8000），**不要按 echo 的 ns 线性外推**：
 * 实际每帧延迟要用 softirq % 与 pps 反推（见 DESIGN.md §4）。TX 侧在
 * tx_lock 里延迟，单次别写 >100 us（会把提交侧整个卡住）。
 */
static unsigned int zxdly_rx_ns;
module_param(zxdly_rx_ns, uint, 0644);
MODULE_PARM_DESC(zxdly_rx_ns, "Fixed delay (ns, us granularity) in the RX hot path after the cache invalidate (default: 0)");

static unsigned int zxdly_tx_ns;
module_param(zxdly_tx_ns, uint, 0644);
MODULE_PARM_DESC(zxdly_tx_ns, "Fixed delay (ns, us granularity) after a successful TX submit, under tx_lock (default: 0)");

#define ZX_TS()	(unlikely(timing) ? ktime_get_ns() : 0)

/* ---- DIAG：诊断插桩（2026-11-03） ------------------------------------
 *
 * 任务：同拓扑、同 cubic、单流下原厂 921.8 vs 我们 740.3 Mbit/s，而我们在
 * 740 时每包只花 5.9 us（原厂 921.8 时 11.2 us）——算力不是瓶颈，所以差距
 * 只可能在"单流的包在盒内被怎么延迟、怎么乱序、怎么丢，以及为什么 -P16
 * 能补齐而单流不能"。本组插桩只回答这三个问题，**不改任何转发判据**。
 *
 * 开关（每项一个 module_param，默认 0）：
 *   diag_residence  1. 盒内停留：RX 快路入口 -> TX 提交成功，按出口口分桶
 *   diag_seq        2. TCP seq 反演：RX 入口与 TX 提交各一张 1024 槽表
 *   diag_poll       3. 每 NAPI poll 处理/转发帧数
 *   diag_txq        4. TX 环占用 + txq_limit 挡回
 *   diag_txlat      5. TX 完成延迟：writel(TX_START) -> reclaim 见到完成
 *   diag_drop       6. 丢点逐项计数（新增 rx_drop_nondev / rx_drop_down / skb_nomem）
 *   diag_burst      8. 每出口的突发段长与段间空档（diag_burst_us 定义"同一段"的阈值）
 *
 * 关掉时热路径只多一次 "ldr zx_diag_mask; tst; beq"——分支方向、顺序、结果
 * 都不变（任务书硬要求）。打开后按桶累加，桶宽是对数，单条插桩 10~30 条指令。
 */
unsigned int zx_diag_mask;	/* 位图，见 eth.h 的 ZX_DIAG_B_* */

/*
 * 一个开关 = 一个 bool 状态 + 它在 zx_diag_mask 里的那一位。写 sysfs
 * （/sys/module/zx279128s_eth/parameters/diag_*）立刻生效，不需要重启。
 */
struct zx_diag_sw {
	unsigned int	bit;
	bool		on;
};

static int zx_diag_sw_set(const char *val, const struct kernel_param *kp)
{
	struct zx_diag_sw *sw = (struct zx_diag_sw *)(uintptr_t)kp->arg;
	bool on;
	int ret;

	if (!sw)
		return -EINVAL;
	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	WRITE_ONCE(sw->on, on);
	if (on)
		WRITE_ONCE(zx_diag_mask, READ_ONCE(zx_diag_mask) | sw->bit);
	else
		WRITE_ONCE(zx_diag_mask, READ_ONCE(zx_diag_mask) & ~sw->bit);
	return 0;
}

static int zx_diag_sw_get(char *buf, const struct kernel_param *kp)
{
	const struct zx_diag_sw *sw = (const struct zx_diag_sw *)kp->arg;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 (sw && READ_ONCE(sw->on)) ? 1 : 0);
}

static const struct kernel_param_ops zx_diag_sw_ops = {
	.set	= zx_diag_sw_set,
	.get	= zx_diag_sw_get,
};

static struct zx_diag_sw zx_diag_sw_residence = { ZX_DIAG_B_RESIDENCE, false };
static struct zx_diag_sw zx_diag_sw_seq       = { ZX_DIAG_B_SEQ, false };
static struct zx_diag_sw zx_diag_sw_poll      = { ZX_DIAG_B_POLL, false };
static struct zx_diag_sw zx_diag_sw_txq       = { ZX_DIAG_B_TXQ, false };
static struct zx_diag_sw zx_diag_sw_txlat     = { ZX_DIAG_B_TXLAT, false };
static struct zx_diag_sw zx_diag_sw_drop      = { ZX_DIAG_B_DROP, false };
static struct zx_diag_sw zx_diag_sw_burst     = { ZX_DIAG_B_BURST, false };

module_param_cb(diag_residence, &zx_diag_sw_ops, &zx_diag_sw_residence, 0644);
MODULE_PARM_DESC(diag_residence, "DIAG 1: in-box residence histogram RX fastpath -> TX submit, per egress port (default: off)");
module_param_cb(diag_seq, &zx_diag_sw_ops, &zx_diag_sw_seq, 0644);
MODULE_PARM_DESC(diag_seq, "DIAG 2: TCP seq inversion counters at RX entry and before TX submit (default: off)");
module_param_cb(diag_poll, &zx_diag_sw_ops, &zx_diag_sw_poll, 0644);
MODULE_PARM_DESC(diag_poll, "DIAG 3: frames handled/forwarded per NAPI poll histogram (default: off)");
module_param_cb(diag_txq, &zx_diag_sw_ops, &zx_diag_sw_txq, 0644);
MODULE_PARM_DESC(diag_txq, "DIAG 4: TX ring occupancy histogram and txq_limit blocks (default: off)");
module_param_cb(diag_txlat, &zx_diag_sw_ops, &zx_diag_sw_txlat, 0644);
MODULE_PARM_DESC(diag_txlat, "DIAG 5: TX completion latency TX_START -> reclaim (default: off)");
module_param_cb(diag_drop, &zx_diag_sw_ops, &zx_diag_sw_drop, 0644);
MODULE_PARM_DESC(diag_drop, "DIAG 6: per-reason drop counters (default: off)");
module_param_cb(diag_burst, &zx_diag_sw_ops, &zx_diag_sw_burst, 0644);
MODULE_PARM_DESC(diag_burst, "DIAG 8: per-egress burst length and inter-burst gap histograms (default: off)");

/*
 * "同一段突发"的判定阈值：相邻两次提交到**同一出口**的间隔 <= 这个值就算
 * 还在同一段里。默认 2 us —— 1500 B 线速连续两个包是 12 us，所以 2 us 只
 * 会把"背靠背的一串"算成一段，不会把正常流量的间隔并进来。
 */
static unsigned int zx_diag_burst_us = 2;
module_param_named(diag_burst_us, zx_diag_burst_us, uint, 0644);
MODULE_PARM_DESC(diag_burst_us, "DIAG 8: consecutive submits to one egress closer than this are one burst (us, default: 2)");

/*
 * 时间直方图分桶：<1us，[1,2) [2,4) … [512,1024) us，[1024us,1ms)，>=1ms
 *
 * 入参故意是 **u32 纳秒**（差值本来就 < 4.29 s），除法因此是 32 位：
 * ARMv7 的 GCC 会把 u64/1000 编成 __aeabi_uldivmod，而 vmlinux 不导出
 * 那个符号 —— 第一版就是这么链失败的（build.log 里 2026-11-03 20:37 那轮
 * `undefined reference to __aeabi_uldivmod`）。32 位除法走 __aeabi_uidiv，
 * 内核自带。
 */
static inline unsigned int zx_diag_tbucket(u32 ns)
{
	u32 us;

	if (ns < 1000)
		return 0;
	us = ns / 1000;
	if (us < 1024)
		return (unsigned int)fls(us);	/* 1..10 */
	if (us < 1000000)
		return 11;
	return 12;
}

/* TX 环占用分桶：0-1、2-3、4-7 … 512+ */
static inline unsigned int zx_diag_qbucket(unsigned int pending)
{
	if (!pending)
		return 0;
	return min_t(unsigned int, (unsigned int)fls(pending) - 1,
		     ZX_DIAG_QBUCKETS - 1);
}

/* 突发段长分桶：0..15 逐值，16..31，32+ */
static inline unsigned int zx_diag_bbucket(u64 n)
{
	if (n < 16)
		return (unsigned int)n;
	if (n < 32)
		return 16;
	return 17;
}

/*
 * 从池槽位里的帧（以太头起，len 不含 FCS）抽 TCP 5 元组，压成 32 位 tag。
 * 只认裸 IPv4/IPv6 + 下一个头就是 TCP；VLAN/PPPoE/带扩展头的 v6/分片一律
 * 返回 false（不计数、不猜）——被测的那条单流是裸 v4/v6 TCP。
 * 全部用 memcpy 取值：ARMv7 上 14+20 的偏移是 2 字节对齐，直接解引用会告警。
 */
static bool zx_diag_tcp_tag(const u8 *frame, u16 len, u32 *tagp, u32 *seqp)
{
	struct ethhdr eh;
	__be16 proto;
	u32 h = 0, seq = 0;

	if (len < ETH_HLEN + 20)
		return false;
	memcpy(&eh, frame, ETH_HLEN);
	proto = eh.h_proto;

	if (proto == htons(ETH_P_IP)) {
		struct iphdr ip;
		struct tcphdr th;
		unsigned int ihl;

		memcpy(&ip, frame + ETH_HLEN, sizeof(ip));
		if (ip.protocol != IPPROTO_TCP)
			return false;
		ihl = (unsigned int)ip.ihl * 4;
		if (ihl < sizeof(ip) || len < ETH_HLEN + ihl + sizeof(th))
			return false;
		/* 0x1fff = 片偏移掩码（分片不看：只有首片带 TCP 头） */
		if (ip.frag_off & htons(0x1fffu))
			return false;
		memcpy(&th, frame + ETH_HLEN + ihl, sizeof(th));
		h = (u32)ntohl(ip.saddr) ^ 0x9e3779b9u;
		h ^= rol32((u32)ntohl(ip.daddr), 13);
		h ^= ((u32)ntohs(th.source) << 16) | (u32)ntohs(th.dest);
		h ^= 0x01000000u;		/* v4 标记 */
		seq = ntohl(th.seq);
	} else if (proto == htons(ETH_P_IPV6)) {
		struct ipv6hdr ip6;
		struct tcphdr th;
		u32 a[4];

		if (len < ETH_HLEN + sizeof(ip6) + sizeof(th))
			return false;
		memcpy(&ip6, frame + ETH_HLEN, sizeof(ip6));
		if (ip6.nexthdr != IPPROTO_TCP)
			return false;
		memcpy(&th, frame + ETH_HLEN + sizeof(ip6), sizeof(th));
		memcpy(a, ip6.saddr.s6_addr, sizeof(a));
		h = ntohl(a[0]) ^ rol32(ntohl(a[1]), 7) ^
		    rol32(ntohl(a[2]), 13) ^ rol32(ntohl(a[3]), 19);
		memcpy(a, ip6.daddr.s6_addr, sizeof(a));
		h ^= rol32(ntohl(a[0]), 3) ^ rol32(ntohl(a[1]), 11) ^
		     rol32(ntohl(a[2]), 17) ^ rol32(ntohl(a[3]), 23);
		h ^= ((u32)ntohs(th.source) << 16) | (u32)ntohs(th.dest);
		h ^= 0x02000000u;		/* v6 标记 */
		seq = ntohl(th.seq);
	} else {
		return false;
	}

	/* 32 位混合：乘法散列，无查表，够把一条流的 5 元组摊到 1024 槽 */
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	*tagp = h & ~3u;	/* 低 2 位留空：tag==0 专门表示"空槽" */
	*seqp = seq;
	return true;
}

/*
 * seq 反演：dir 0 = RX 入口（zx_eth_rx 解析完描述符之后），1 = TX 提交前
 * （zx_tx_submit_bp 成功之后）。两向**各一张 1024 槽表**：共用一张的话，
 * 同一帧先在 RX 写一次、再在 TX 写一次，两向互相覆盖，永远看不到"上次"。
 *
 * 判据只看 seq 是否回退（TCP 序列号按 32 位回绕比较）：回退一次说明这个
 * 5 元组在盒子内部被换了顺序。表满/冲突只计 seq_coll，不做任何纠正——
 * 冲突的结果是"当成新流"，只影响绝对数，不影响"有没有回退"的定性判断。
 */
static void zx_diag_seq_probe(struct zx_eth_adapter *adapter, const u8 *frame,
			      u16 len, unsigned int dir, int port)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);
	struct zx_diag_seq *e;
	u32 tag, seq, idx;

	if (!d || !d->seq)
		return;
	if (!zx_diag_tcp_tag(frame, len, &tag, &seq))
		return;
	d->seq_probe[dir]++;
	idx = tag & (ZX_DIAG_SEQ_SLOTS - 1);
	e = &d->seq[dir * ZX_DIAG_SEQ_SLOTS + idx];
	if (e->tag == tag) {
		bool roll = (s32)(seq - e->seq) < 0;

		d->seq_hit[dir]++;
		if (roll)
			d->seq_roll[dir]++;
		if (port >= 0 && port < ZX_NUM_PORTS) {
			if (dir) {
				d->seq_hit_out[port]++;
				if (roll)
					d->seq_roll_out[port]++;
			} else {
				d->seq_hit_in[port]++;
				if (roll)
					d->seq_roll_in[port]++;
			}
		}
	} else if (e->tag) {
		d->seq_coll++;
	}
	e->tag = tag;
	e->seq = seq;
}

/* RX 入口（每帧一次，只做 seq 反演；residence 的戳在快路入口打） */
static void zx_diag_rx_entry(struct zx_eth_adapter *adapter, u16 bp, u16 len,
			     int port)
{
	if (bp >= ZX_NUM_BUFFERS)
		return;
	if (zx_diag_on(ZX_DIAG_B_SEQ))
		zx_diag_seq_probe(adapter,
				  adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16,
				  len, 0, port);
}

/*
 * 1. 盒内停留的起点：快路入口（zx_ffe_rx()，accel.c）。只有真的进了快路的帧
 * 才有戳——栈路径的帧没有，所以它们不会把停留分布稀释成噪声。
 * 戳用 u32 低 32 位纳秒，|1 保证非 0（0 = 没有戳）。
 */
void zx_diag_res_stamp(struct zx_eth_adapter *adapter, u16 bp)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);

	if (!d || bp >= ZX_NUM_BUFFERS)
		return;
	d->rx_ts[bp] = (u32)ktime_get_ns() | 1u;
}

/* 快路没转发（回栈/丢弃）时撤销戳，免得被同一 bp 上后面的栈帧误结算 */
void zx_diag_res_clear(struct zx_eth_adapter *adapter, u16 bp)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);

	if (d && bp < ZX_NUM_BUFFERS)
		d->rx_ts[bp] = 0;
}

/* 6. 新增的丢点计数（既有计数仍在 adapter 上，见 diag 文件的第 6 段） */
void zx_diag_drop(struct zx_eth_adapter *adapter, unsigned int why)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);

	if (d && why < ZX_DIAG_DROP_N)
		d->drop[why]++;
}

/* 4/6：提交被 txq_limit 挡回（栈路径与快路共用这一个入口） */
static void zx_diag_txq_block(struct zx_eth_adapter *adapter, int eport)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);

	if (!d || eport < 0 || eport >= ZX_NUM_PORTS)
		return;
	d->txq_block[eport]++;
	d->drop[ZX_DIAG_DROP_TXQ_BLOCK]++;
}

/* 4：提交后把环打到上限 -> 停所有端口队列（这是"环满"的另一半证据） */
static void zx_diag_txq_trip(struct zx_eth_adapter *adapter, int eport)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);

	if (d && eport >= 0 && eport < ZX_NUM_PORTS)
		d->txq_trip[eport]++;
}

/*
 * TX 提交成功后一次结算（zx_tx_submit_bp 里，持 tx_lock）：
 *   1 停留（rx_ts[bp] -> now，按出口口分桶）
 *   4 环占用（提交前的 tx_pending）
 *   5 完成延迟起点（tx_ts[tx_idx] = now）
 *   2 TX 侧 seq 反演（读的是同一帧在池里的当前内容）
 *   8 突发（同一出口的段长/空档）
 * pending 传的是**自增之前**的 tx_pending，tx_idx 是本次描述符下标。
 */
static void zx_diag_tx_submit(struct zx_eth_adapter *adapter, int eport, u16 bp,
			      unsigned int tx_idx, unsigned int pending, u16 len)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);
	unsigned int mask = READ_ONCE(zx_diag_mask);
	u32 now;

	if (!d || bp >= ZX_NUM_BUFFERS || eport < 0 || eport >= ZX_NUM_PORTS)
		return;
	now = (u32)ktime_get_ns();

	if (mask & ZX_DIAG_B_RESIDENCE) {
		u32 ts = d->rx_ts[bp];

		d->rx_ts[bp] = 0;
		if (ts) {
			u32 dns = now - ts;
			unsigned int b = zx_diag_tbucket(dns);

			d->res[eport][b]++;
			d->res_n[eport]++;
			if (dns > d->res_max_ns[eport])
				d->res_max_ns[eport] = dns;
		} else {
			d->res_nostamp++;	/* 栈路径提交：本来就没有快路入口 */
		}
	}
	if (mask & ZX_DIAG_B_TXQ) {
		unsigned int b = zx_diag_qbucket(pending);

		d->txq[b]++;
		d->txq_port[eport][b]++;
	}
	if (mask & ZX_DIAG_B_TXLAT) {
		d->tx_ts[tx_idx] = now | 1u;
		d->tx_port[tx_idx] = (u8)eport;
	}
	if (mask & ZX_DIAG_B_SEQ)
		zx_diag_seq_probe(adapter,
				  adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16,
				  len, 1, eport);
	if (mask & ZX_DIAG_B_BURST) {
		u64 *cur = &d->burst_cur[eport];
		u32 last = READ_ONCE(d->burst_last_ns[eport]);

		d->burst_frames[eport]++;
		if (!last) {
			*cur = 1;
		} else {
			u32 gap = now - last;

			if (gap <= (u64)READ_ONCE(zx_diag_burst_us) * 1000ull) {
				(*cur)++;
			} else {
				/* 上一段到此为止：结算段长与空档 */
				d->burst_len[eport][zx_diag_bbucket(*cur)]++;
				d->burst_gap[eport][zx_diag_tbucket(gap)]++;
				d->burst_seg[eport]++;
				if (!d->burst_gap_min_ns[eport] ||
				    gap < d->burst_gap_min_ns[eport])
					d->burst_gap_min_ns[eport] = gap;
				if (gap > d->burst_gap_max_ns[eport])
					d->burst_gap_max_ns[eport] = gap;
				*cur = 1;
			}
		}
		d->burst_last_ns[eport] = now | 1u;
	}
}

/*
 * 5. TX 完成延迟的终点：zx_tx_reclaim() 结算 [tail, tail+n) 这批描述符。
 *
 * 口径注意：reclaim 只在"下一次提交 / NAPI poll / 250 us 定时器"里跑，所以
 * 这里量到的是**上界**（真实完成时刻在这之前）。要精确到硬件完成时刻需要
 * 每包中断，这颗 SoC 没有；作为"环里最长的一帧停了多久"的判据够用。
 */
static void zx_diag_tx_done(struct zx_eth_adapter *adapter, u32 tail, u32 n)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);
	u32 now;

	if (!d || !n)
		return;
	now = (u32)ktime_get_ns();
	for (u32 i = 0; i < n; i++) {
		u32 idx = (tail + i) % ZX_NUM_TX_DESC;
		u32 ts = d->tx_ts[idx];

		if (!ts)
			continue;
		{
			u32 dns = now - ts;
			unsigned int b = zx_diag_tbucket(dns);
			u8 p = d->tx_port[idx];

			d->tx_ts[idx] = 0;
			d->txlat[b]++;
			if (p < ZX_NUM_PORTS)
				d->txlat_port[p][b]++;
			d->txlat_n++;
			if (dns > d->txlat_max_ns)
				d->txlat_max_ns = dns;
		}
	}
}

/* 3. 每 poll 的帧数（work = 本轮处理总数，fwd = 其中快路转发数） */
static void zx_diag_poll(struct zx_eth_adapter *adapter, int work, int fwd)
{
	struct zx_diag *d = READ_ONCE(adapter->diag);
	unsigned int wb, fb;

	if (!d)
		return;
	wb = (unsigned int)clamp(work, 0, ZX_DIAG_PBUCKETS - 1);
	fb = (unsigned int)clamp(fwd, 0, ZX_DIAG_PBUCKETS - 1);
	d->poll_frames[wb]++;
	d->poll_fwd[fb]++;
	d->poll_n++;
	d->poll_work += (u64)max(work, 0);
	d->poll_ffeframes += (u64)max(fwd, 0);
}

/*
 * ABI 断言（任务书硬要求）：这些立即数是 ppe.ko 里写死的偏移，也是 fix2
 * 交付镜像的布局。**加字段只许追加在结构体末尾**，否则这里编译不过——
 * 这是"不改既有字段"的机器可验证形式，不靠人记。
 * 数值来源：.scratch/ffe-fastpath/fix2-abi/20261103-0200/offsetcheck/。
 */
static void zx_diag_abi_check(void)
{
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, ppe_flows) != 0x4eec);
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, ppe_lock) != 0x4ed4);
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, ppe_active) != 0x4ee8);
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, accel) != 0x50e0);
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, tx_dup_reject) != 0x5790);
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, accel_qfull_drop) != 0x5808);
	BUILD_BUG_ON(offsetof(struct zx_eth_adapter, diag) != 0x5818);
	BUILD_BUG_ON(sizeof(struct zx_eth_adapter) != 0x5820);
}

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

/* ---- coal-knob：硬件 RX 中断合并 + NAPI 权重（运行期可改，2026-11-03） ----
 *
 * 一、硬件合并器
 * DMA 块自带一对 RX 中断合并寄存器（zx279128s-eth.h:150-151）：
 *   ZX_DMA_RX_IRQ_TIMER  = DMA + 0x30（0x350030）  时间阈值
 *   ZX_DMA_RX_IRQ_FRAMES = DMA + 0x34（0x350034）  帧数阈值
 * 硬件语义（eth.h:295-303 原文）："The RX interrupt comes after this many
 * frames, or this long after the first one." —— 从第一帧起计时，时间或帧数
 * 任一先到就抬 RX 中断。基线把这一对钉死在 500 µs / 64 帧（main.c:1713-1714）。
 *
 * **编码：0x30 的单位是 20 ns，不是 µs**（eth.h:150 原文 "in 20 ns ticks"）。
 * 1 µs = 50 ticks；基线写的是 ZX_RX_IRQ_USECS * 50 = 500 * 50 = 25000 ticks，
 * 25000 × 20 ns = 500 µs，与上面注释自洽。纯线性、无移位/对数/档位编码，
 * 任何整数 µs 都能精确写入（×50 不截断）。0x34 是帧数原值，基线 = 64。
 *
 * 默认值 = 本板实测最优（eth.h:300-301 的常量，2026-11-04 起；基线 500/64）：
 *   zxcoal_us     = ZX_RX_IRQ_USECS  = 250 -> 写 12500（250 µs × 50 ticks）
 *   zxcoal_frames = ZX_RX_IRQ_FRAMES = 16  -> 写 16
 * 没人碰参数时 zx_hw_init()（只在 probe 调一次，main.c:5042）就写这两个值；
 * 开机后 /etc/init.d/zx_ffe_coalesce 按 /etc/config/zx_ffe 覆盖（同一个写入口）。
 *
 * 运行期是否生效：驱动里除 zx_hw_init() 外**没有任何路径**碰这两个寄存器
 * （ISR 只读 TM 中断状态/掩码，合并计数与重装全在 DMA 硬件里，没有软件
 * rearm），ndo_stop()/ndo_open() 也不重写它们（回到复位态只发生在
 * zx_hw_reset() 的 master reset，而它只在 probe 失败路径和 reboot 前调用）。
 * ⇒ 运行期直接写 + readl 读回自证；若某台片子只在 DMA 使能时锁存，回退办法
 *   是重载驱动（down/up 不够）。
 *
 * 二、NAPI 权重
 * 基线 probe 用 3 参 netif_napi_add（main.c:5159）⇒ 编译期 NAPI_POLL_WEIGHT
 * (64)。这里换成 4 参 netif_napi_add_weight(..., zxnapi_weight)，默认仍 64。
 * net/core/dev.c 的 napi_poll() 每次调用开头 `weight = n->weight;` 重读该字段
 * ⇒ 运行期改它在下一次 poll 生效。风险（照做、写清）：改的那一瞬间若正好有
 * 一轮 poll 在跑，那一轮的 budget 与 work<weight 判据可能来自不同次读取，
 * 最多多/少服务一帧；zx_eth_rx() 严格按 budget 截断、环长 1024 远大于允许的
 * weight，所以不影响正确性。weight>64 时 netif_napi_add_weight() 会
 * netdev_err_once 抱怨一次（本驱动的 poll 是 budget 截断的，功能上无害）。
 */
static unsigned int zxcoal_us = ZX_RX_IRQ_USECS;		/* µs */
static unsigned int zxcoal_frames = ZX_RX_IRQ_FRAMES;	/* 帧 */
static unsigned int zxnapi_weight = NAPI_POLL_WEIGHT;	/* 帧/轮 */

#define ZXCOAL_TICKS_PER_US	50U	/* 0x30 的 LSB = 20 ns ⇒ 1 µs = 50 ticks */
#define ZXCOAL_US_MAX		1000000U	/* 1 s = 50e6 ticks，u32 装得下 */
#define ZXCOAL_FRAMES_MAX	4096U
#define ZXNAPI_WEIGHT_MAX	1024U

/* 一次寄存器写：读旧 → 写新 → 读回自证；读回不符就把旧值写回去并报错。 */
struct zx_coal_apply {
	const char	*param;		/* "zxcoal_us" / "zxcoal_frames" */
	unsigned int	shown;		/* 打在日志里的参数值（µs 或帧） */
	u32		reg;		/* ZX_DMA_RX_IRQ_TIMER / _FRAMES */
	u32		raw;		/* 真正写进寄存器的原始值 */
	bool		quiet;		/* 值没变就不打印（probe 尾部同步用） */
	unsigned int	adapters;
	unsigned int	failed;
};

static void zx_coal_apply_one(struct zx_eth_adapter *adapter, void *data)
{
	struct zx_coal_apply *a = data;
	void __iomem *reg = adapter->base + a->reg;
	u32 old = readl(reg);
	u32 back;

	a->adapters++;
	writel(a->raw, reg);
	back = readl(reg);	/* 读回：这次读把 posted write 冲出去 */
	if (back == a->raw) {
		if (!a->quiet || old != a->raw)
			dev_info(adapter->dev,
				 "%s=%u: reg 0x%05x raw %u -> %u (readback %u)\n",
				 a->param, a->shown, a->reg, old, a->raw, back);
		return;
	}

	/* 值被硬件截断/忽略：写回旧值，别把寄存器留在半截状态 */
	a->failed++;
	writel(old, reg);
	dev_err(adapter->dev,
		"%s: reg 0x%05x write %u (param %u), read back %u — field too narrow? restored %u\n",
		a->param, a->reg, a->raw, a->shown, back, readl(reg));
}

static int zx_coal_write(const char *param, u32 reg, u32 raw,
			 unsigned int shown, bool quiet)
{
	struct zx_coal_apply a = {
		.param = param, .shown = shown, .reg = reg, .raw = raw,
		.quiet = quiet,
	};

	zx_eth_for_each_adapter(zx_coal_apply_one, &a);
	if (!a.adapters) {
		/* probe 之前（cmdline 或早到的 sysfs 写）：值先存着，probe 落一次 */
		pr_info("zx279128s: %s=%u staged (raw %u), no adapter yet\n",
			param, shown, raw);
		return 0;
	}
	return a.failed ? -EIO : 0;
}

static int zxcoal_us_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v > ZXCOAL_US_MAX)
		return -EINVAL;
	ret = zx_coal_write("zxcoal_us", ZX_DMA_RX_IRQ_TIMER,
			    v * ZXCOAL_TICKS_PER_US, v, false);
	if (ret)
		return ret;	/* 硬件没吃下：变量保持旧值，get 读到的=生效值 */
	WRITE_ONCE(zxcoal_us, v);
	return 0;
}

static const struct kernel_param_ops zxcoal_us_ops = {
	.set	= zxcoal_us_set,
	.get	= param_get_uint,
};

module_param_cb(zxcoal_us, &zxcoal_us_ops, &zxcoal_us, 0644);
MODULE_PARM_DESC(zxcoal_us, "RX IRQ coalescing timer in us (register counts 20 ns ticks, x50; 0..1000000, default: 250)");

static int zxcoal_frames_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v > ZXCOAL_FRAMES_MAX)
		return -EINVAL;
	ret = zx_coal_write("zxcoal_frames", ZX_DMA_RX_IRQ_FRAMES, v, v, false);
	if (ret)
		return ret;
	WRITE_ONCE(zxcoal_frames, v);
	return 0;
}

static const struct kernel_param_ops zxcoal_frames_ops = {
	.set	= zxcoal_frames_set,
	.get	= param_get_uint,
};

module_param_cb(zxcoal_frames, &zxcoal_frames_ops, &zxcoal_frames, 0644);
MODULE_PARM_DESC(zxcoal_frames, "RX IRQ coalescing frame count (0..4096, default: 16)");

/* NAPI 权重：一个 adapter 一个 napi，运行期直接改字段。 */
struct zx_napi_weight_apply {
	unsigned int	weight;
	bool		quiet;
	unsigned int	adapters;
};

static void zx_napi_weight_one(struct zx_eth_adapter *adapter, void *data)
{
	struct zx_napi_weight_apply *a = data;
	unsigned int old = READ_ONCE(adapter->napi.weight);

	a->adapters++;
	WRITE_ONCE(adapter->napi.weight, (int)a->weight);
	if (!a->quiet || old != a->weight)
		dev_info(adapter->dev,
			 "zxnapi_weight=%u: napi.weight %u -> %u (readback %d)\n",
			 a->weight, old, a->weight,
			 READ_ONCE(adapter->napi.weight));
}

static int zxnapi_weight_set(const char *val, const struct kernel_param *kp)
{
	struct zx_napi_weight_apply a = { .quiet = false };
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/* 0 会让 napi_poll() 的 work<weight 判据恒假（budget 0），直接拒掉 */
	if (v < 1 || v > ZXNAPI_WEIGHT_MAX)
		return -EINVAL;
	a.weight = v;
	zx_eth_for_each_adapter(zx_napi_weight_one, &a);
	if (!a.adapters)
		pr_info("zx279128s: zxnapi_weight=%u staged, no adapter yet\n", v);
	WRITE_ONCE(zxnapi_weight, v);
	return 0;
}

static const struct kernel_param_ops zxnapi_weight_ops = {
	.set	= zxnapi_weight_set,
	.get	= param_get_uint,
};

module_param_cb(zxnapi_weight, &zxnapi_weight_ops, &zxnapi_weight, 0644);
MODULE_PARM_DESC(zxnapi_weight, "NAPI poll weight, frames per poll (1..1024, default: 64 = NAPI_POLL_WEIGHT)");

/*
 * probe 尾部（adapter 已进链表）把三个参数值再落一次硬件：覆盖 "cmdline 设过"
 * 与 "sysfs 写在 list_add 之前" 两个窗口。默认值时三个都 silent（quiet）。
 */
static void zx_coal_sync_probe(void)
{
	struct zx_napi_weight_apply wa = {
		.weight = READ_ONCE(zxnapi_weight), .quiet = true,
	};

	zx_coal_write("zxcoal_us", ZX_DMA_RX_IRQ_TIMER,
		      READ_ONCE(zxcoal_us) * ZXCOAL_TICKS_PER_US,
		      READ_ONCE(zxcoal_us), true);
	zx_coal_write("zxcoal_frames", ZX_DMA_RX_IRQ_FRAMES,
		      READ_ONCE(zxcoal_frames), READ_ONCE(zxcoal_frames), true);
	zx_eth_for_each_adapter(zx_napi_weight_one, &wa);
}

/* zc 槽位状态（zx_bmu_free_bp 里就要用，所以定义放前面） */
#define ZX_ZC_BS_FREE	0	/* 在 BMU 空闲表里 */
#define ZX_ZC_BS_SKB	1	/* 在协议栈手里，还没人 orphan 过 */
#define ZX_ZC_BS_SHADOW	2	/* 我们在 dataref 上拿了一份，等它掉到 1 */
#define ZX_ZC_BS_TX	3	/* 已交给发送硬件，硬件发完自己归还 */
#define ZX_ZC_BS_LOST	4	/* 追不到（数据被 expand 搬走），永不归还 */

/*
 * P10.5：FFE 臂的 bp 状态（与上面的 zc 状态是两套账，别混）。
 *
 * 只有两格就够：FREE = 不认为它在发送环里（在 BMU 空闲表、在 RX 硬件手里、
 * 在驱动手里都算），TX = 已写进发送描述符、还没记到发送完成。
 *
 * **不能照抄 zc 的语义**：zc 的 TX 是终态（槽位交出去就不再要了，没有
 * TX→FREE 的边），而 FFE 的 bp 是要循环的 —— 硬件发完会把 bp 自己还回 BMU，
 * 下一个 RX 帧还会用它。若 TX 不清，快路上每个正常回收的 bp 都会被判成
 * "还在发送环里"，A2 会拒绝每一次归还、A3 会丢每一帧，快路当场停摆。
 * 所以 TX 的清除点是 zx_tx_reclaim()：按 ZX_DMA_TX_DONE 的完成数，把
 * [tx_cur_idx - tx_pending, +n) 这批描述符对应的 bp 清回 FREE。
 */
#define ZX_BP_FREE	0
#define ZX_BP_TX	1

/* 每个 bp 的 RX 投递计数（取证用，u16 饱和）；见 dma_state 的 bp_hot 行 */
#define ZX_BP_HITS_MAX	0xffffu

/* 追不到的槽位超过这个数就自动关 zc，免得池子被一滴滴吃空 */
#define ZX_ZC_LOST_MAX	64u

/* ---- BMU helpers --------------------------------------------------- */

/* 事件流水（定义在 zc 段），归还 bp 时也要记一笔 */
static void zx_zc_ev(struct zx_eth_adapter *adapter, u16 bp, u8 type, u8 aux);

/*
 * Allocate a buffer from the BMU.  A request that does not complete in time
 * stays with the BMU, which hands out its buffer later: keep it pending and
 * collect that buffer on the next call, as the vendor driver does.  Starting a
 * new request instead lost one buffer for good with every timeout.
 *
 * **P0（tx-lock-scope）：本函数不再"等"，也从不在 tx_lock 里轮询。**
 * 它只做一次有界探活——最多 ZX_BMU_ALLOC_PROBES 次 readl（≈ 几百 ns），
 * 没有 udelay、没有 readl_poll_timeout_atomic、不睡。原因是旧版在
 * readl_poll_timeout_atomic(1, 1000) 里可以持 tx_lock 整整 1 ms，而 tx_lock 是
 * 五个端口共享的发送环锁：那 1 ms 里快路（zx279128s-accel.c）也提交不进去。
 * 现在它自己的锁 bmu_alloc_lock 只包这几拍 MMIO，所以调用点可以在 tx_lock
 * 外（正常路径），也可以在 tx_lock 里（rare 的补申请）——后者持锁时长与一次
 * MMIO 读同量级，不是"长等"。
 *
 * 返回 <0 表示这次没拿到（-EBUSY：上一次请求还挂在 BMU 上；-EAGAIN：这次
 * 请求没在探测预算内完成；-ENOSPC：完成但缓冲号无效）。这时 bmu_alloc_pending
 * 已经置位，下一次调用会先把它收回来——调用方（zx_eth_xmit）据此把帧交回
 * qdisc 重排队，不丢帧。
 */
#define ZX_BMU_ALLOC_PROBES	8

static int zx_bmu_alloc_bp_nb(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	u32 val;
	int i;

	spin_lock_bh(&adapter->bmu_alloc_lock);
	if (adapter->bmu_alloc_pending) {
		if (readl(base + ZX_BMU_BUF_ALLOC) & ZX_BMU_BUF_ALLOC_REQ) {
			adapter->bmu_alloc_fail++;
			spin_unlock_bh(&adapter->bmu_alloc_lock);
			return -EBUSY;
		}
		adapter->bmu_alloc_pending = false;
		val = readl(base + ZX_BMU_BUF_ID);
		if (val & ZX_BMU_BUF_ID_VALID) {
			adapter->bmu_alloc_late++;
			val = FIELD_GET(ZX_BMU_BUF_ID_BP, val);
			spin_unlock_bh(&adapter->bmu_alloc_lock);
			return val;
		}
	}

	val = readl(base + ZX_BMU_BUF_ALLOC);
	writel(val | ZX_BMU_BUF_ALLOC_REQ, base + ZX_BMU_BUF_ALLOC);
	/*
	 * 有界探活：只读寄存器，不插延时。ZX_BMU_ALLOC_PROBES = 8 的依据是
	 * **预算**，不是"实测 8 拍够用"（复审 §3 指出上一版把推断写成了结论）：
	 *  - 旧代码 1 µs 一步 × 最多 1000 步；外场 450 万包上 bmu_alloc_fail /
	 *    bmu_alloc_late 全 0（.scratch/ffe-fastpath/tx-path-probe/
	 *    20261103-0030/RESULT.md §2.1）。这只证明"1 ms 内能完成"，**不能**
	 *    证明"8 拍内能完成"；
	 *  - 8 拍的推导：Cortex-A9 一次设备读按 30–100 ns 估（含 AXI 往返），
	 *    8 拍 ≈ 0.25–0.8 µs，比 1 Gbit/s、1500 B 帧的到达间隔（≈12 µs）
	 *    小一个数量级，所以"每帧最多花 0.8 µs"是固定可接受成本；
	 *  - 落在预算外时不再靠"重排队一次"收回：调用方 zx_tx_alloc_bp() 会按
	 *    环空位再探 1~2 次，仍失败就停口 + 丢帧（FIX1，见那里长注释）。
	 * 上机判据：dma_state 的 tx_bp_alloc_fail 随 pps 线性上涨 = 预算太短，
	 * 调大这个常量重编即可。
	 */
	for (i = 0; i < ZX_BMU_ALLOC_PROBES; i++) {
		val = readl(base + ZX_BMU_BUF_ALLOC);
		if (!(val & ZX_BMU_BUF_ALLOC_BUSY))
			break;
	}
	if (!(val & ZX_BMU_BUF_ALLOC_BUSY)) {
		val = readl(base + ZX_BMU_BUF_ID);
		if (val & ZX_BMU_BUF_ID_VALID) {
			val = FIELD_GET(ZX_BMU_BUF_ID_BP, val);
			spin_unlock_bh(&adapter->bmu_alloc_lock);
			return val;
		}
		adapter->bmu_alloc_pending = true;
		adapter->bmu_alloc_fail++;
		spin_unlock_bh(&adapter->bmu_alloc_lock);
		return -ENOSPC;
	}

	adapter->bmu_alloc_pending = true;
	adapter->bmu_alloc_fail++;
	spin_unlock_bh(&adapter->bmu_alloc_lock);
	return -EAGAIN;
}

/*
 * 阻塞版：**只给进程上下文**（debugfs 的 txtest）。内部是 nb + usleep_range，
 * 总预算与原 readl_poll_timeout_atomic(1, 1000) 相同（≈1 ms）。
 * **调用者不得持 tx_lock**：锁里等硬件正是 P0 要去掉的东西。
 */
static int zx_bmu_alloc_bp_wait(struct zx_eth_adapter *adapter)
{
	int i, bp = -EAGAIN;

	for (i = 0; i < 10; i++) {
		bp = zx_bmu_alloc_bp_nb(adapter);
		if (bp >= 0)
			break;
		usleep_range(50, 100);
	}
	return bp;
}

/*
 * P10.5 A4：归还 bp 时毒化帧首 16 B 里的 8 B，把"槽位回到 BMU 之后又被当成
 * 一帧送上来、但内容其实没被硬件重写"变成可计数事件（rx_poison）。
 *
 * 为什么毒**帧首**（槽位偏移 16..31）而不是槽位保留的 16 B 头（偏移 0..15）：
 *  - 帧首是硬件按描述符 len 必须写的区域：只要这次真是新帧，毒值一定被覆盖。
 *    保留头驱动从不读、也没有证据表明硬件每帧重写它；毒在那里会永久残留，
 *    把每个正常回收槽位的合法帧都判成毒帧 —— 不可接受的假阳性（会丢流量）。
 *
 * 为什么 16 B 里**只毒 8 B、故意放过 source-MAC（帧字节 6..11）与以太类型
 * （帧字节 12..13）**：这两段是 P10.4 判据的输入 —— alias 判据比的是
 * `d + ETH_ALEN`（smac），分子分族看的又是 `d + 12`（以太类型）。若把它们也
 * 毒掉，毒帧就既不再命中 alias、也分不出 v4/v6，`accel->alias_drop[]` 会被
 * 截断 ⇒ P10.4 的 autofallback 对慢态失明（复审 REVIEW-P10.4 §4 的 S8）。
 * 保留它们以后：原来的 alias 判据、分族计数、P10.4 判据**一个字节都不变**，
 * 毒帧只是多记一笔 rx_poison（"这一帧来自一个被归还过的槽位，且没被重写"）。
 *
 * 布局（帧字节，16 B 里占 8 B）：
 *   0..5    01 80 c2 00 00 0e  dmac = IEEE 保留的 link-local 组播：桥不转发，
 *                             万一毒帧真被回栈也不会泛洪到 LAN；
 *   6..11   （保留原样：smac，alias 判据的输入）
 *   12..13  （保留原样：以太类型，P10.4 分族的输入）
 *   14..15  5a a5             以太类型之后的第一处载荷字节（魔数尾）
 * 可辨识性：真实帧要同时命中这 8 B。帧字节 0..5 = 01:80:c2:00:00:0e 的真实帧
 * 只有 LLDP，而 LLDP 的字节 14..15 是首个 TLV 头（实测固定 02 07），不是
 * 5a a5；其它 link-local 协议（STP/LACP）用 LLC 不是以太类型。所以真实帧
 * 命中概率 ~2^-64 且具体为 0；即便误判一次，代价也只是这一帧回栈（不丢）。
 *  - 只在 bp_state[bp]==FREE 时写：TX 在途的槽位归发送硬件（见调用点）。
 */
#define ZX_BP_POISON_MAC_LO	0x00c28001u	/* 帧字节 0..3：01 80 c2 00 */
#define ZX_BP_POISON_MAC_HI	0x0e00u		/* 帧字节 4..5：00 0e */
#define ZX_BP_POISON_TAIL	0xa55au		/* 帧字节 14..15 */

static void zx_bp_poison_write(struct zx_eth_adapter *adapter, u16 bp)
{
	u8 *d = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16;
	u32 m0 = ZX_BP_POISON_MAC_LO;
	u16 m1 = ZX_BP_POISON_MAC_HI;
	u16 m2 = ZX_BP_POISON_TAIL;

	memcpy(d, &m0, sizeof(m0));	/* dmac 前 4 B */
	memcpy(d + 4, &m1, sizeof(m1));	/* dmac 后 2 B；d[6..11] 是 smac，不动 */
	/* d[12..13] 是以太类型，不动（P10.4 分族要读它） */
	memcpy(d + 14, &m2, sizeof(m2));	/* 类型后第一处载荷字节 */
}

/*
 * RX 侧：帧首还是不是我们的毒值（调用前必须已经 cache invalidate）。
 * 只比 8 B：dmac 6 B + 类型后 2 B；smac 与以太类型必须保持原样（见上）。
 */
static bool zx_bp_poisoned(struct zx_eth_adapter *adapter, u16 bp)
{
	const u8 *d = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16;
	u32 m0;
	u16 m1, m2;

	memcpy(&m0, d, sizeof(m0));
	memcpy(&m1, d + 4, sizeof(m1));
	if (m0 != ZX_BP_POISON_MAC_LO || m1 != ZX_BP_POISON_MAC_HI)
		return false;
	memcpy(&m2, d + 14, sizeof(m2));
	return m2 == ZX_BP_POISON_TAIL;
}

/*
 * FIX1（复审需修②）：空闲表一时回不来时**不再吞掉 bp**。
 *
 * may_wait = true  ：进程/非发送路径（RX 归还、txtest、accel_set 排空），保留
 *                   原来的 5 ms 忙等；
 * may_wait = false ：发送路径（ndo_start_xmit 栈内）用，空闲表满就立刻把 bp
 *                   放进待归还队列，由 NAPI poll 排水（zx_bmu_defer_drain），
 *                   不让一次 5 ms 忙等压在发送路径上（复审 §4 的相邻风险）。
 */
static int zx_bmu_free_bp_flags(struct zx_eth_adapter *adapter, u16 bp, bool may_wait)
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
		if (may_wait)
			ret = readl_poll_timeout_atomic(adapter->base + ZX_BMU_FREE_STATUS,
							val, val & ZX_BMU_FREE_SLOTS,
							1, 5000);
		else
			ret = -EAGAIN;		/* 发送路径：不忙等，转待归还队列 */
		if (ret) {
			adapter->bmu_free_fail++;
			/*
			 * 把 bp 放进待归还队列（不是丢掉）。队列满 32 说明 BMU
			 * 长时间不接空闲表：这时才真的放弃，并告警一次（每
			 * 32 个丢一个的静默比告警更坏）。
			 */
			if ((u16)(adapter->bmu_defer_head -
				  adapter->bmu_defer_tail) < ZX_BMU_DEFER_MAX) {
				adapter->bmu_defer[adapter->bmu_defer_head++ %
						   ZX_BMU_DEFER_MAX] = bp;
				adapter->bmu_free_defer++;
			} else {
				adapter->bmu_free_defer_drop++;
				WARN_ONCE(1,
					  "zx279128s: BMU free list stuck, bp %u abandoned (%u so far)\n",
					  bp, adapter->bmu_free_defer_drop);
			}
			ret = -EAGAIN;
			goto out;
		}
		adapter->bmu_free_slots = FIELD_GET(ZX_BMU_FREE_SLOTS, val);
	}

	adapter->bmu_free_slots--;
	/*
	 * P10.5 A4：把槽位交还 BMU 之前毒化帧首 16 B（语义与布局见
	 * zx_bp_poison_write()）。两个必要条件：
	 *  - bp_state[bp]==FREE：TX 在途的槽位归发送硬件，写进去就是改坏
	 *    正在发的帧（这是 A4 唯一的自伤面，所以它依赖 A0 的状态机）；
	 *  - 放在 writel() 之前：写完再交还，硬件不可能先把这个槽位发给 RX。
	 * 写 16 B 非缓存内存是纳秒量级，且只在"归还"这一侧，热路径上不占
	 * 预算；缓存池模式（只有 A/B 实验会开）要多一次 clean，见下。
	 */
	if (unlikely(READ_ONCE(ffe_poison)) && bp < ZX_NUM_BUFFERS &&
	    READ_ONCE(adapter->bp_state[bp]) == ZX_BP_FREE) {
		zx_bp_poison_write(adapter, bp);
		if (adapter->pkt_pool_cacheable)
			dma_sync_single_for_device(adapter->dev,
					adapter->pkt_pool_dma +
					(u32)bp * ZX_BUF_SIZE + 16,
					16, DMA_BIDIRECTIONAL);
	}
	writel(bp, adapter->base + ZX_BMU_BUF_FREE);
out:
	spin_unlock_bh(&adapter->bmu_free_lock);
	return ret;
}

static int zx_bmu_free_bp(struct zx_eth_adapter *adapter, u16 bp)
{
	return zx_bmu_free_bp_flags(adapter, bp, true);
}

/* FIX1：发送路径专用 —— 空闲表满时不忙等，直接进待归还队列。 */
static int zx_bmu_free_bp_nowait(struct zx_eth_adapter *adapter, u16 bp)
{
	return zx_bmu_free_bp_flags(adapter, bp, false);
}

/*
 * P10.5 A2：归还 bp 之前的属主检查 —— 同一个槽位只归还一次。
 *
 * 只改"还不还"，**不改"送不送"**：丢弃决定、描述符消费、索引推进一个字节
 * 都不动；被拒的只是"再还一次 BMU"这个动作。
 *
 * C1 补一条同源判据：bp_state[bp]==FREE 但槽位仍带毒，说明硬件已经把这个 bp
 * 还回 BMU（FREE 二义性），这一份描述符是重放 —— 再写一次空闲表就是第二份项。
 *
 * 为什么必须拒：bp_state[bp]==TX 说明这个槽位已经写进发送描述符、发送硬件
 * 还要读它。此时再 writel(bp, ZX_BMU_BUF_FREE)，BMU 空闲表就多出一份重复项，
 * 该 bp 从此被两个属主循环使用 —— 正是慢态"只有 1537/1540 两个 bp 在循环"
 * 的放大链（每命中一次多一份永久损坏，所以越跑越糟、不自愈）。
 *
 * zc 模式不介入：zc 有自己的 zc_state（而且它的 TX 是终态），两套账叠加只会
 * 互相误判；A 档先在 !zc_rx 上交付（TASK-P10.5-FIX §8 风险 7）。
 *
 * 返回 0 = 已归还 / 按 A2 有意跳过；非 0 = BMU 归还失败（调用方照旧记
 * bmu_free_fail，口径与改动前完全一样）。
 */
static int zx_bp_return(struct zx_eth_adapter *adapter, u16 bp)
{
	if (unlikely(READ_ONCE(adapter->zc_rx)))
		return zx_bmu_free_bp(adapter, bp);
	if (unlikely(bp < ZX_NUM_BUFFERS &&
		     (READ_ONCE(adapter->bp_state[bp]) == ZX_BP_TX ||
		      (READ_ONCE(ffe_poison) && zx_bp_poisoned(adapter, bp))))) {
		adapter->bp_double_free++;
		return 0;
	}
	return zx_bmu_free_bp(adapter, bp);
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
	/* coal-knob：默认 250 µs / 16 帧 ⇒ 12500 ticks / 16（本板实测最优；基线 500/64） */
	writel(READ_ONCE(zxcoal_us) * ZXCOAL_TICKS_PER_US,
	       base + ZX_DMA_RX_IRQ_TIMER);
	writel(READ_ONCE(zxcoal_frames), base + ZX_DMA_RX_IRQ_FRAMES);
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
	/* P10.5：环从头开始 ⇒ 账本也从头开始（probe 才走到这里） */
	if (adapter->bp_state)
		memset(adapter->bp_state, ZX_BP_FREE, ZX_NUM_BUFFERS);

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
	u32 tail = adapter->tx_cur_idx - adapter->tx_pending;

	if (unlikely(done > adapter->tx_pending))
		adapter->tx_done_excess += done - adapter->tx_pending;
	adapter->tx_complete_cnt += n;
	adapter->tx_pending -= n;
	/*
	 * P10.5 A0 的清除点：完成的是环上最老的 n 个描述符，即
	 * [tx_cur_idx - tx_pending, +n)。`tail` 必须在 `tx_pending -= n`
	 * **之前**算，否则回绕后 tail 偏移 n，会把还在飞的 bp 清成 FREE
	 * （那正是 A2 假阳性/二次归还的来源）。
	 *
	 * 用提交时记下的 cached 影子环 tx_bp[] 而不是反读描述符 byte7/8：
	 * 发送环在 dma_alloc_coherent 的非缓存事务区，反读每帧要多两次非缓存
	 * 事务（~40–60 ns/帧，65 kpps 上 0.3–0.4%），而影子环是 cached u16
	 * 读写（<2 ns）；描述符提交后归硬件所有，驱动也不必再从它反读自己的
	 * 字段。语义与任务书 §5 #3 完全一致（同一个 bp、同一次完成）。
	 */
	if (likely(adapter->bp_state)) {
		u32 i;

		if (unlikely(done > n)) {
			/*
			 * 完成数超过在飞数：记账已经漂了（实测全程 0）。逐个反推
			 * 不可信，宁可整表回 FREE（下一次提交/归还按"没有在飞"
			 * 处理），也不要把某个 bp 永久钉在 TX 上让 A2 拒还。
			 */
			adapter->bp_state_stale++;
			memset(adapter->bp_state, ZX_BP_FREE, ZX_NUM_BUFFERS);
		} else {
			for (i = 0; i < n; i++) {
				u16 bp = adapter->tx_bp[(tail + i) %
							ZX_NUM_TX_DESC];

				if (bp < ZX_NUM_BUFFERS)
					WRITE_ONCE(adapter->bp_state[bp],
						   ZX_BP_FREE);
			}
		}
	}
	/*
	 * DIAG 5：这批描述符从"TX_START 那一刻"到"现在被 reclaim 看到"的
	 * 时间分布。必须在 bp_state 的循环之后（那里会动 tail 附近的表项，
	 * 但 tx_ts[] 是独立数组，顺序无关），也不能提前 return：n == 0 时
	 * zx_diag_tx_done() 自己直接返回。
	 */
	if (unlikely(zx_diag_on(ZX_DIAG_B_TXLAT)))
		zx_diag_tx_done(adapter, tail, n);
	/*
	 * 加速模式切换期间不动队列：切换自己会 netif_tx_disable() 所有端口，
	 * 这里再把队列叫醒就等于开着门切换（设计 §1.4 S2/S6）。
	 */
	if (adapter->tx_pending < READ_ONCE(zx_txq_limit) &&
	    !READ_ONCE(adapter->accel.quiesce)) {
		int p;

		for (p = 0; p < ZX_NUM_PORTS; p++)
			if (adapter->ports[p] && netif_queue_stopped(adapter->ports[p]))
				netif_wake_queue(adapter->ports[p]);
	}
}

/* ---- FIX1：BMU 申请失败的处置与恢复（复审 BLOCKER） ----------------- */

/*
 * 重试次数与"环里还剩多少空位"挂钩：空位宽裕（≥ 环长一半）时多探一次，
 * 用一次 ~0.5 µs 的 MMIO 换掉一帧；空位紧张时立刻给背压，因为这时再探
 * 只会挤在别人的 reclaim 后面。上限 2，单帧 MMIO 预算保持 ≤ 2 × 8 拍。
 */
static unsigned int zx_tx_alloc_retry_cnt(struct zx_eth_adapter *adapter)
{
	u32 limit = READ_ONCE(zx_txq_limit);
	u32 pending = READ_ONCE(adapter->tx_pending);
	u32 free_slots = limit > pending ? limit - pending : 0;

	return free_slots >= limit / 2 ? ZX_TX_BP_RETRY_MAX : ZX_TX_BP_RETRY_MIN;
}

/* 有界重试版申请：最多 retry_cnt 次 zx_bmu_alloc_bp_nb()，中间不插延时。 */
static int zx_tx_alloc_bp(struct zx_eth_adapter *adapter)
{
	unsigned int tries = zx_tx_alloc_retry_cnt(adapter);
	unsigned int i;
	int bp = -EAGAIN;

	for (i = 0; i < tries; i++) {
		bp = zx_bmu_alloc_bp_nb(adapter);
		if (bp >= 0 && bp < ZX_NUM_BUFFERS)
			return bp;
		if (i + 1 < tries)
			adapter->tx_bp_alloc_retry++;
	}
	return bp;
}

/*
 * 申请失败的统一处置（FIX1）：**不返回 NETDEV_TX_BUSY**。
 *
 *  1. 只停**入口口**（netdev 就是那个口），不碰其余四个；
 *  2. tx_lock 下挂 kick：250 µs 后必定来一次 NAPI poll
 *     （zx_eth_poll → zx_tx_starve_recover），队列不会"停着没人管"；
 *  3. 丢这一帧并计数：dev_dstats_tx_dropped() 让 /proc/net/dev 也看得见。
 *
 * 为什么不用"停口 + NETDEV_TX_BUSY 重排队"：帧确实不丢，但一旦恢复探测
 * 失灵，这个口就永久卡在 head-of-line；丢帧版本即使恢复机制全挂，也只是
 * 持续丢帧（每帧 ≤ 2 × 8 拍 MMIO），不会静默变砖。
 */
static void zx_tx_alloc_fail_drop(struct zx_eth_adapter *adapter,
				  struct net_device *netdev, struct sk_buff *skb)
{
	adapter->tx_bp_alloc_drop++;
	if (!netif_queue_stopped(netdev)) {
		adapter->tx_starve_stop++;
		adapter->tx_starve_polls = 0;
		WRITE_ONCE(adapter->tx_bmu_starved, true);
		netif_stop_queue(netdev);
	}
	spin_lock_bh(&adapter->tx_lock);
	zx_tx_kick(adapter);
	spin_unlock_bh(&adapter->tx_lock);
	dev_dstats_tx_dropped(netdev);
	dev_kfree_skb_any(skb);
}

/*
 * 停口后的恢复（在 zx_eth_poll 里、**不持 tx_lock** 调用）：
 *
 *  A. 锁外探一次 zx_bmu_alloc_bp_nb()（8 拍 MMIO，无延时）。成功说明 BMU
 *     又有缓冲：把探针拿到的 bp 立刻还回去，不占池子（还失败也有待归还
 *     队列兜着）。这一句就是"有配对唤醒"的探针来源。
 *  B. 锁内只做"清标志 + 唤醒"（与停口侧对称，都在 tx_lock 里）。
 *  C. 兜底：探测满 ZX_TX_STARVE_MAX_POLLS 轮（64 × 250 µs ≈ 16 ms）仍没好，
 *     也把队列放开 —— 之后每帧走"申请失败 → 丢帧"，口绝不会永久停住。
 */
static void zx_tx_starve_recover(struct zx_eth_adapter *adapter)
{
	int probe;
	bool ok;

	if (!READ_ONCE(adapter->tx_bmu_starved))
		return;

	probe = zx_bmu_alloc_bp_nb(adapter);
	ok = probe >= 0 && probe < ZX_NUM_BUFFERS;
	if (ok) {
		zx_bmu_free_bp(adapter, (u16)probe);
		adapter->tx_starve_polls = 0;
	}

	spin_lock_bh(&adapter->tx_lock);
	if (ok || ++adapter->tx_starve_polls >= ZX_TX_STARVE_MAX_POLLS) {
		int p;

		WRITE_ONCE(adapter->tx_bmu_starved, false);
		adapter->tx_starve_polls = 0;
		if (!READ_ONCE(adapter->accel.quiesce))
			for (p = 0; p < ZX_NUM_PORTS; p++)
				if (adapter->ports[p] &&
				    netif_queue_stopped(adapter->ports[p]))
					netif_wake_queue(adapter->ports[p]);
	}
	spin_unlock_bh(&adapter->tx_lock);
}

/*
 * FIX1：待归还队列的排水。每轮 NAPI 最多还 4 个，避免"归还失败→再排队"
 * 自我放大；空闲表还是满就立刻停手（不在 NAPI 里做 5 ms 忙等）。
 */
static void zx_bmu_defer_drain(struct zx_eth_adapter *adapter, unsigned int max)
{
	unsigned int i;

	spin_lock_bh(&adapter->bmu_free_lock);
	for (i = 0; i < max && adapter->bmu_defer_tail != adapter->bmu_defer_head; i++) {
		u16 bp;

		if (!adapter->bmu_free_slots)
			break;
		bp = adapter->bmu_defer[adapter->bmu_defer_tail % ZX_BMU_DEFER_MAX];
		adapter->bmu_free_slots--;
		writel(bp, adapter->base + ZX_BMU_BUF_FREE);
		adapter->bmu_defer_tail++;
		adapter->bmu_free_defer_ok++;
	}
	spin_unlock_bh(&adapter->bmu_free_lock);
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
 * 锁外预判（P0）：这一帧**可能**走零拷贝发送吗？只读 skb 与池指针，不取锁、
 * 不改任何状态，可以在 tx_lock 之外随便调。
 *
 * 它必须与 zx_zc_tx_take() 的前段判据**同一份**（take 现在直接调它）：返回
 * false 时 take 一定不会成功，所以 zx_eth_xmit() 可以据此决定"要不要在锁外
 * 先申请一个 BMU 缓冲"。计数口径也没变：head/data/len/shared 不符时照旧记
 * zc_fallback，每帧仍然只记一次。
 */
static bool zx_zc_tx_candidate(struct zx_eth_adapter *adapter,
			       struct sk_buff *skb)
{
	const u8 *slot;
	u16 bp;

	if (!adapter->zc_rx || !zx_zc_tag_get(skb, &bp) || bp >= ZX_NUM_BUFFERS)
		return false;
	slot = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE;
	if (!skb->head_frag || skb->head != slot || skb->data != slot + 16 ||
	    skb_headlen(skb) != skb->len || skb_shared(skb)) {
		adapter->zc_fallback++;
		return false;
	}
	return true;
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
	u8 st;
	u32 mine;

	if (!zx_zc_tx_candidate(adapter, skb) || !zx_zc_tag_get(skb, &bp))
		return false;

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
 * 返回 false 表示环满（tx_pending >= txq_limit，见 zx_txq_limit）：这时**一个字节都没
 * 写**，bp 与帧内容保持调用前的样子，调用方可以安全地把这一帧交给协议栈。
 * tx_lock 在手里时这个判断不会被别人改变——tx_pending 只在 tx_lock 下增长。
 */
bool zx_tx_submit_bp(struct zx_eth_adapter *adapter, int eport, u16 bp,
		     unsigned len)
{
	void __iomem *base = adapter->base;
	__le32 *desc;
	u32 tx_idx;

	if (unlikely(adapter->tx_pending >= READ_ONCE(zx_txq_limit))) {
		/* DIAG 4/6：这是"提交被 txq_limit 挡回"的唯一入口（栈与快路共用） */
		if (unlikely(zx_diag_on(ZX_DIAG_B_TXQ | ZX_DIAG_B_DROP)))
			zx_diag_txq_block(adapter, eport);
		return false;
	}
	/* C1：bp 已在发送环就拒（调用方持 tx_lock）：否则 tx_bp[] 两格同 bp、reclaim 两次 FREE */
	if (unlikely(adapter->bp_state && bp < ZX_NUM_BUFFERS &&
		     READ_ONCE(adapter->bp_state[bp]) == ZX_BP_TX)) {
		adapter->tx_dup_reject++;
		return false;
	}

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
	/*
	 * P10.5：从这一行起 bp 归发送硬件（直到 zx_tx_reclaim() 记到这次完成）。
	 * 影子环 tx_bp[] 与 bp_state[] 一起写：前者给 reclaim 反查是哪一个 bp，
	 * 后者给 RX/归还侧判断"槽位是否还在发送环里"。三处写侧
	 * （这里、reclaim、hw_init）都在 tx_lock 下或 probe 期，不需要新锁。
	 */
	if (likely(adapter->bp_state && bp < ZX_NUM_BUFFERS)) {
		adapter->tx_bp[tx_idx] = bp;
		WRITE_ONCE(adapter->bp_state[bp], ZX_BP_TX);
	}
	/* 快路与栈共用一个环，dma_state 的 qlen_peak 记整环占用峰值 */
	if (adapter->tx_pending > READ_ONCE(adapter->accel.cnt[0].qlen_peak))
		WRITE_ONCE(adapter->accel.cnt[0].qlen_peak, adapter->tx_pending);
	/*
	 * DIAG：一次结算 1/4/5/2/8 五项（内部按 zx_diag_mask 分派，
	 * 关掉时外层这一个位测试就是全部代价）。pending 传**自增之前**的值，
	 * 所以是 tx_pending - 1。
	 */
	if (unlikely(READ_ONCE(zx_diag_mask)))
		zx_diag_tx_submit(adapter, eport, bp, tx_idx,
				  adapter->tx_pending - 1, (u16)len);
	/*
	 * zxdly_tx_ns：纯延迟，与 diag_seq 的 TX 侧同处（提交成功、tx_lock +
	 * RCU 临界区里）。它只消耗时间：不读帧、不查表、不写共享状态。
	 * 这是一条**独立**的剂量臂（TX-only），不要和 RX 侧合并看。
	 * 默认 0 ⇒ 只多这一次 unlikely 分支，行为不变。
	 */
	if (unlikely(READ_ONCE(zxdly_tx_ns))) {
		unsigned int zxdly_ns = READ_ONCE(zxdly_tx_ns);

		if (zxdly_ns < 1000)
			ndelay(zxdly_ns);
		else
			udelay(DIV_ROUND_UP(zxdly_ns, 1000));
	}
	if (adapter->tx_pending >= READ_ONCE(zx_txq_limit)) {
		int p;

		for (p = 0; p < ZX_NUM_PORTS; p++)
			if (adapter->ports[p])
				netif_stop_queue(adapter->ports[p]);
		zx_tx_kick(adapter);
		if (unlikely(zx_diag_on(ZX_DIAG_B_TXQ)))
			zx_diag_txq_trip(adapter, eport);
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
	int bp = -1;
	u16 zcbp = 0;
	bool pre_bp = false;
	u64 t_entry, t_reclaim, t_alloc, t_copy, t_start;

	t_entry = ZX_TS();

	if (len > 1536) {
		dev_kfree_skb_any(skb);
		dev_dstats_tx_dropped(netdev);
		return NETDEV_TX_OK;
	}

	/*
	 * P0（tx-lock-scope）：环**可能**满时先不申请 BMU 缓冲。旧代码在锁里
	 * 先判容量再申请，"环满"这条路上一次申请都不做；申请挪到锁外以后若
	 * 仍然无条件申请，环满时每帧都要多做一次申请+归还（归还侧空闲表空时
	 * zx_bmu_free_bp 自己还要等硬件，预算是 5 ms）。这里是**无锁提示**：
	 * tx_pending 只在 tx_lock 下变，读到的可能是旧值，所以锁里还会再判
	 * 一次；提示为假时（绝大多数）直接走下面的正常路径。
	 */
	if (unlikely(READ_ONCE(adapter->tx_pending) >= READ_ONCE(zx_txq_limit))) {
		spin_lock_bh(&adapter->tx_lock);
		zx_tx_reclaim(adapter);
		if (adapter->tx_pending >= READ_ONCE(zx_txq_limit)) {
			netif_stop_queue(netdev);
			zx_tx_kick(adapter);
			spin_unlock_bh(&adapter->tx_lock);
			/* DIAG 6：栈路径环满 -> 交回 qdisc 重排（这是"回栈"的一半） */
			if (unlikely(zx_diag_on(ZX_DIAG_B_DROP)))
				zx_diag_drop(adapter, ZX_DIAG_DROP_TXQ_BUSY);
			return NETDEV_TX_BUSY;
		}
		spin_unlock_bh(&adapter->tx_lock);
		/* 提示是旧的（别的端口刚 reclaim 过）：按正常路径重来一次 */
	}

	/*
	 * 零拷贝候选为真时不预申请（take 成功就一个缓冲都不用）；其余情况
	 * 在**锁外**把缓冲拿到手：BMU 可能要等，而 tx_lock 里绝不许等
	 * （旧版 readl_poll_timeout_atomic(1, 1000) 可以在这里持锁 1 ms）。
	 *
	 * FIX1（复审 BLOCKER）：拿不到**不能**返回 NETDEV_TX_BUSY。6.18 的
	 * dev_requeue_skb() → __netif_schedule() → qdisc_run() 会立刻重排队
	 * （net/sched/sch_generic.c:144-176、:357-370；net/core/dev.c:5778），
	 * 只要队列开着就是**无界重试**；BMU 空闲表干涸时（REQ 位不撤，
	 * nb 永远 -EBUSY）结果就是"数据面 0 包 + 一个 CPU 的 sirq 顶满"。
	 * 处置改为：有界重试（次数按环空位裁）→ 只停**入口口** → 丢这一帧
	 * 并计数 → 返回 OK。唤醒在 zx_eth_poll() 的 zx_tx_starve_recover()，
	 * 里面有 16 ms 无条件放开的兜底。
	 */
	if (!zx_zc_tx_candidate(adapter, skb)) {
		bp = zx_tx_alloc_bp(adapter);
		if (unlikely(bp < 0 || bp >= ZX_NUM_BUFFERS)) {
			adapter->tx_bp_alloc_fail++;
			zx_tx_alloc_fail_drop(adapter, netdev, skb);
			return NETDEV_TX_OK;
		}
		pre_bp = true;
	}
	t_alloc = ZX_TS();

	spin_lock_bh(&adapter->tx_lock);
	zx_tx_reclaim(adapter);
	t_reclaim = ZX_TS();
	/*
	 * The ports share the ring.  The queues are stopped as soon as it is
	 * full, but another port's transmit may already be waiting for the
	 * lock: give its frame back to the stack.
	 */
	if (unlikely(adapter->tx_pending >= READ_ONCE(zx_txq_limit))) {
		netif_stop_queue(netdev);
		zx_tx_kick(adapter);
		spin_unlock_bh(&adapter->tx_lock);
		if (pre_bp)
			zx_bmu_free_bp_nowait(adapter, (u16)bp);
		return NETDEV_TX_BUSY;
	}
	/*
	 * skb_put_padto() 失败时**已经释放了 skb**（__skb_put_padto 的
	 * free_on_error=true，include/linux/skbuff.h:3949），这里只差把预申请的
	 * bp 还回去，绝不能再 kfree 一次。
	 */
	if (skb_put_padto(skb, ETH_ZLEN)) {
		spin_unlock_bh(&adapter->tx_lock);
		if (pre_bp)
			zx_bmu_free_bp_nowait(adapter, (u16)bp);
		dev_dstats_tx_dropped(netdev);
		return NETDEV_TX_OK;
	}
	len = skb->len;
	if (!pre_bp) {
		if (zx_zc_tx_take(adapter, skb, &zcbp)) {
			/*
			 * 零拷贝：帧本来就在这个槽位里，既不申请缓冲也不搬家，
			 * 硬件发完自己把 bp 还回 BMU。
			 */
			bp = zcbp;
		} else {
			/*
			 * 罕见：候选为真但 take 拒了（引用计数 > 1、陈旧 tag、
			 * 环项缺失）。锁内补一次有界探活（8 拍 MMIO，不等）；
			 * FIX1：拿不到也走同一条处置（停口 + 丢帧 + OK），
			 * 不再返回 NETDEV_TX_BUSY。
			 */
			bp = zx_bmu_alloc_bp_nb(adapter);
			if (unlikely(bp < 0 || bp >= ZX_NUM_BUFFERS)) {
				spin_unlock_bh(&adapter->tx_lock);
				adapter->tx_bp_alloc_fail++;
				zx_tx_alloc_fail_drop(adapter, netdev, skb);
				return NETDEV_TX_OK;
			}
		}
	}
	if (!zcbp) {
		if (unlikely(adapter->zc_state[bp] == ZX_ZC_BS_SHADOW))
			adapter->zc_tx_dup++;	/* 调试：BMU 把还挂着影子的 bp 给了发送 */
		zx_zc_ev(adapter, (u16)bp, 4, adapter->zc_state[bp]);
		buf_ptr = (u8 *)adapter->pkt_pool +
			  ((u32)bp * ZX_BUF_SIZE) + 16;
		if (skb_headlen(skb) >= len) {
			memcpy(buf_ptr, skb->data, len);
		} else if (skb_copy_bits(skb, 0, buf_ptr, len)) {
			spin_unlock_bh(&adapter->tx_lock);
			zx_bmu_free_bp_nowait(adapter, (u16)bp);
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
		spin_unlock_bh(&adapter->tx_lock);
		if (!zcbp)
			zx_bmu_free_bp_nowait(adapter, (u16)bp);
		dev_dstats_tx_dropped(netdev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	t_start = ZX_TS();

	/*
	 * P0 起四段的含义（申请已经不在锁里）：[0] 锁外申请/零拷贝预判，
	 * [1] 取锁 + 回收，[2] 拷贝 + 同步（含锁内那次罕见的补申请），
	 * [3] 提交。dma_state 的标签同步改过。
	 */
	if (unlikely(timing)) {
		adapter->tx_stage_ns[0] += t_alloc - t_entry;
		adapter->tx_stage_ns[1] += t_reclaim - t_alloc;
		adapter->tx_stage_ns[2] += t_copy - t_reclaim;
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
 * P10.4：alias 丢弃点按以太类型分族计数（只计数，不改行为），给 accel 的
 * 自动降级判据用。慢态下这批帧是"进了 FFE 又被静默丢"的证词：它们发生在
 * 判据链之前，所以既不进 cnt[].lookup、也不进 /proc/net/dev，唯一的痕迹
 * 就是这里（以及 accel->alias_drop[]）。
 *
 * `d` 指向**以太头起点**，不需要再加偏移：调用点就是
 * `d = pkt_pool + bp * ZX_BUF_SIZE + 16`，与 zx_alias_snapshot() 的 frame、
 * 以及快路判据链里的 `data + 2 * ETH_ALEN` 是同一个指针（快照里
 * head[0..5] = dmac、head[6..11] = smac 就是按这个口径取的），所以以太类型
 * 在 **d + 2 * ETH_ALEN = d + 12**。调用点外面已有 `len >= ETH_HLEN` 守卫，
 * d[12..13] 一定在帧内；帧是 DMA 写进池槽位的，读之前已经
 * dma_sync_single_for_cpu()。
 *
 * 族判定直接复用 zx_eth_family()（与快路判据链、慢路径同一个口径）：
 * 0x0800 ⇒ 0（v4）、0x86dd ⇒ 1（v6）、VLAN（0x8100/0x88a8）与其它 ⇒ -1，
 * 记 ZX_ALIAS_FAM_UNK，不参与判据（判据分母是本族 lookup，VLAN 内层族与它
 * 不对应）。
 *
 * 写侧只有 RX NAPI，裸 u64++，与 cnt[] 同一无锁口径（丢几个计数不影响判据
 * 结论的方向：少计只会晚降级一个窗，不会误降级）。
 */
static void zx_alias_count(struct zx_eth_adapter *adapter, const u8 *d)
{
	int fam = zx_eth_family(*(const __be16 *)(d + 2 * ETH_ALEN));

	if (fam < 0)
		fam = ZX_ALIAS_FAM_UNK;
	adapter->accel.alias_drop[fam]++;
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
			      const __le32 *desc, u16 len, int port, u16 bp,
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
	/*
	 * P10.5：记录"alias 判据命中那一刻，这个 bp 在不在发送环里"。
	 * 这是切开 H1/H2（bp 在途被二次投递）与 H3/永久陈旧（bp 已是 FREE）
	 * 的那一格，A0 的必采项（TASK-P10.5-FIX §7 第 3 条）。
	 */
	s->rsvd = bp < ZX_NUM_BUFFERS ?
		  (u8)READ_ONCE(adapter->bp_state[bp]) : 0xff;
	/* P10.5：快照时刻（jiffies），把 32 条定到时间轴上（§9 第 5 条） */
	s->stamp = (u32)jiffies;
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
	/* DIAG 3：本轮快路转发帧数（只在 diag_poll 打开时才累加） */
	int diag_fwd = 0;

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
			bool poisoned;
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
					    zx_bp_return(adapter, bp))
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
			 * P10.5 A0：每个 bp 的 RX 投递计数（取证，见 dma_state 的
			 * bp_hot 行与 alias_dump 每条表项的 hits=）。一个 bp 的
			 * 投递速率是"该槽位 100% 失败（内容永久陈旧）"与
			 * "0.5% 失败（偶发在途重叠）"唯一的分辨器。
			 */
			if (likely(adapter->bp_rx_hits)) {
				u16 h = adapter->bp_rx_hits[bp];

				if (h != ZX_BP_HITS_MAX)
					WRITE_ONCE(adapter->bp_rx_hits[bp], h + 1);
			}

			/*
			 * P10.5 A0：这个 bp 还在发送环里 —— 同一个槽位同时被 RX 与
			 * TX 拥有，正是慢态里"少数 bp 反复错配"的签名。
			 *   A0（默认）：只计数 ffe_dup，帧照旧走下面的判据链。
			 *   A3（ffe_bp_drop=1，默认关，会丢帧）：消费描述符、丢帧、
			 *   **不归还** bp（它属于发送环，再还一次就是二次归还）。
			 */
			if (unlikely(READ_ONCE(adapter->bp_state[bp]) ==
				     ZX_BP_TX)) {
				adapter->ffe_dup++;
				if (READ_ONCE(ffe_bp_drop)) {
					zx_rx_consume(desc, rel);
					adapter->rx_cur_idx[q]++;
					work_done++;
					avail--;
					continue;
				}
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
			/*
			 * zxdly_rx_ns：纯延迟，位置在这里 —— 紧接着
			 * dma_sync_single_for_cpu()（上面那次 invalidate）、在快路
			 * 判据之前，与 diag_seq 的 RX 入口同一个槽位。它只消耗
			 * 时间：不读帧、不查表、不写任何共享状态。
			 * 这是一条**独立**的剂量臂（RX-only），不要和 TX 侧合并看。
			 * 默认 0 ⇒ 只多这一次 unlikely 分支，行为不变。
			 * 注意：开它时不要再开 timing=1 —— 这段会算进 rx_stage_ns[2]。
			 */
			if (unlikely(READ_ONCE(zxdly_rx_ns))) {
				unsigned int zxdly_ns = READ_ONCE(zxdly_rx_ns);

				if (zxdly_ns < 1000)
					ndelay(zxdly_ns);
				else
					udelay(DIV_ROUND_UP(zxdly_ns, 1000));
			}

			/*
			 * DIAG 2（RX 侧）：**必须放在上面那次 invalidate 之后**
			 * ——池默认是可缓存的，先读会拿到上一个帧在这个槽位
			 * 留下的缓存行，seq 就成了假的（会凭空多出"回退"）。
			 * 这里 bp/len 已校验、缓存已同步，是"RX 入口"最准的一点：
			 * 它记的回退只可能是交换机/DMA 侧的乱序，与 TX 侧的
			 * 回退（zx_tx_submit_bp 里记的那一格）能分开归属。
			 */
			if (unlikely(zx_diag_on(ZX_DIAG_B_SEQ)))
				zx_diag_rx_entry(adapter, bp, len, port);

			/*
			 * P10.5 A4：帧首还是我们归还时写的毒值 ⇒ 这个槽位回到
			 * BMU 之后又被当成"有一帧"送了上来，但内容没有被硬件
			 * 重写（否则毒值一定被覆盖）。记 rx_poison。
			 *
			 * 这里**只计数、不改变后面的判据**：毒值只落在 dmac 与
			 * 类型后的 2 B 上，source-MAC 与以太类型保持原样，所以
			 * 下面的 alias 判据、P10.4 的分族计数照旧命中 —— 这是
			 * 复审要求的口径（毒帧既算 alias 分子又单独计数，二者
			 * 不重复扣）。真正改变行为的地方只有一个：毒帧**不进
			 * 快路**（见下面 fam/h_proto 之后的 goto），因为它的
			 * dmac 已经是保留组播，被 FFE 改写后发上线就是脏帧。
			 */
			poisoned = READ_ONCE(ffe_poison) &&
				   zx_bp_poisoned(adapter, bp);
			if (unlikely(poisoned))
				adapter->rx_poison++;

			/* Aliasing detector: frames from one of our own addresses */
			if (len >= ETH_HLEN) {
				u8 *d = adapter->pkt_pool + bp * ZX_BUF_SIZE + 16;

				if (zx_is_own_addr(adapter, d + ETH_ALEN)) {
					adapter->rx_alias_cnt++;
					/*
					 * P10.4：同一帧再按以太类型记到
					 * accel->alias_drop[]（判据链看不见
					 * 这批帧，见 zx_alias_count()）。
					 */
					zx_alias_count(adapter, d);
					/*
					 * P10.2 F6：只留证，不改行为。
					 * 此时缓存已 invalidate、帧还在
					 * 池槽位里，head 直接从 d 拷。
					 */
					zx_alias_snapshot(adapter, desc, len,
							  port, bp, d);
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
					if (zx_bp_return(adapter, bp))
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

			/*
			 * P10.5 A4 的后半句：毒帧不进快路。走到这里的是"投毒过、
			 * 但 source-MAC 不是本机"的毒帧（alias 的那一类已经在上面
			 * 按**原有规则**丢弃并计数了，行为没变）。这些帧的 dmac 已经
			 * 是保留组播，内容也不可信，绝不能进 FFE 被改写后发上线；
			 * 回栈交给协议栈按它自己的规则处理（下面照常消费描述符、
			 * 按规则把 bp 还给 BMU，A2 仍会拦"还在发送环里"的那一种）。
			 * 放在 fam/h_proto 初始化之后，慢路径拿到的口径与普通回栈一致。
			 */
			if (unlikely(poisoned))
				goto accel_slow;
			if (unlikely(READ_ONCE(adapter->accel.quiesce)))
				goto accel_slow;
			/*
			 * M2：PPPoE 窥探（P1-9 的位置约束）。
			 *
			 *   1. 必须在 poisoned 判据之后：毒帧的 dmac 已被改成保留
			 *      组播、内容不可信，拿它更新会话就是让任意伪造帧改
			 *      写 sid / 触发 flush；
			 *   2. 必须在 quiesce 之后：切换中一律回栈，不进快路也
			 *      不学会话；
			 *   3. 必须在 fam 算好之后、按族分派之前：探针成功后把
			 *      fam 换成**内层**协议族（0/1），后面才走"这一族是
			 *      ffe 才进判据链"的分支；否则 PPPoE 帧会以 fam=-1
			 *      落进 nfam 兜底分支，被当成"另一族"再送一遍判据链。
			 *
			 * pppoe=0 时这里只多一次 "读门控 + cmp + 前向跳过"：
			 * zx_pppoe_gate 与 M1 的 module_param(pppoe) 是同一字节
			 * （accel.c 里的同地址 alias），所以运行期写 sysfs 立刻
			 * 生效，不需要重启、也不需要再挂一个 notifier。
			 *
			 * 只看以太类型是 0x8864/0x8863 的帧才进函数：普通
			 * IPv4/IPv6 流量在门控打**开**时也不多一次调用。
			 */
			if (zx_pppoe_gate &&
			    (h_proto == htons(ZX_PPPOE_ETH_P_SESSION) ||
			     h_proto == htons(ZX_PPPOE_ETH_P_DISCOVERY))) {
				int pfam = zx_ffe_pppoe_sniff(adapter,
						adapter->pkt_pool +
						(u32)bp * ZX_BUF_SIZE + 16,
						len, port);

				if (pfam >= 0)
					fam = pfam;
			}
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
			/*
			 * DIAG 1：快路入口打的戳只属于"真的被快路转发"的帧。
			 * 回栈/丢弃的帧把戳撤掉，否则同一个 bp 之后被栈路径
			 * 拿去发送时会被误结算成一次超长停留。
			 * FORWARDED 的戳已经在 zx_tx_submit_bp() 里结算掉了。
			 */
			if (unlikely(zx_diag_on(ZX_DIAG_B_RESIDENCE)) &&
			    ffe != ZX_FFE_RX_FORWARDED)
				zx_diag_res_clear(adapter, bp);
			if (ffe == ZX_FFE_RX_FORWARDED &&
			    unlikely(zx_diag_on(ZX_DIAG_B_POLL)))
				diag_fwd++;
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
				u32 poff = 0;
				u32 claimed = 0;
				bool have_len = false;

				/*
				 * M2：PPPoE 会话帧的内层 L3 在 14 B 以太头 +
				 * (6 B PPPoE 头 + 1/2 B PPP 协议) 之后。l3 与
				 * l3_avail **必须一起**减这个偏移：
				 *   claimed 是 IP 层长度（v4 tot_len / v6
				 *   40+payload_len），它相对 l3 计量；
				 *   只改 l3 不改 l3_avail 会让可用长度多算
				 *   7/8 B（短帧漏判）；只改 l3_avail 不改 l3
				 *   会让 claimed 比真正的小 7/8 B（长帧误判）。
				 * 只认 h_proto == 0x8864：探针判过 SESS 的帧
				 * 才会把 fam 换成内层族，才会走到这里；PADT
				 * （fam 仍 < 0）与普通 IP 帧 poff 恒 0。
				 */
				if (h_proto == htons(ZX_PPPOE_ETH_P_SESSION)) {
					poff = zx_pppoe_l3_off(l3 -
						ETH_HLEN);
					l3 += poff;
					l3_avail -= poff;
				}

				/*
				 * 两个族的门槛都用 l3_avail 而不是 len：
				 *   l3_avail = len - ETH_HLEN - poff，
				 * 所以 "len >= ETH_HLEN+poff+sizeof(hdr)"
				 * 与 "l3_avail >= sizeof(hdr)" 是同一件事，
				 * 而后者在 poff=0 时与原式逐字等价。
				 */
				if (fam == 1 &&
				    l3_avail >= sizeof(struct ipv6hdr)) {
					struct ipv6hdr ip6;

					memcpy(&ip6, l3, sizeof(ip6));
					claimed = sizeof(ip6) +
						  ntohs(ip6.payload_len);
					have_len = true;
				} else if (fam == 0 &&
					   l3_avail >= sizeof(struct iphdr)) {
					struct iphdr ip;

					memcpy(&ip, l3, sizeof(ip));
					claimed = ntohs(ip.tot_len);
					have_len = true;
				}
				if (have_len && claimed > l3_avail) {
					adapter->rx_len_short_cnt++;
					if (adapter->rx_len_short_cnt <= 5)
						dev_warn_ratelimited(adapter->dev,
							"rxq%d: short frame? port=%d len=%u claimed=%u proto=0x%04x bp=%u; dropping (the stack would call it truncated)\n",
							q, port, len, claimed,
							ntohs(h_proto), bp);
					zx_rx_consume(desc, rel);
					adapter->rx_cur_idx[q]++;
					work_done++;
					avail--;
					if (zx_bp_return(adapter, bp))
						adapter->bmu_free_fail++;
					continue;
				}
				if (have_len &&
				    l3_avail - claimed > ZX_FFE_MAX_PAD) {
					adapter->rx_len_long_cnt++;
					if (adapter->rx_len_long_cnt <= 5)
						dev_warn_ratelimited(adapter->dev,
							"rxq%d: long frame? port=%d len=%u claimed=%u proto=0x%04x bp=%u; not dropping (pskb_trim will cut it)\n",
							q, port, len, claimed,
							ntohs(h_proto), bp);
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
				/*
				 * DIAG 6：投递失败的三条**互斥**原因。既有代码
				 * 只对 dest_dev 非空的那一种记 dev_dstats，这里把
				 * "dest_dev == NULL"（端口越界/未 probe）单独
				 * 计成 rx_drop_nondev —— 这是任务书点名新加的。
				 */
				if (unlikely(zx_diag_on(ZX_DIAG_B_DROP)))
					zx_diag_drop(adapter, !dest_dev ?
						ZX_DIAG_DROP_RX_NONDEV :
						(!(dest_dev->flags & IFF_UP) ?
						 ZX_DIAG_DROP_RX_DOWN :
						 ZX_DIAG_DROP_SKB_NOMEM));
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
			if (!zc && zx_bp_return(adapter, bp))
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
			 * DROPPED 是"不能回栈、槽位也没交给硬件"的那一类：默认是
			 * "改写之后提交失败"的兜底（理论上不可达）；P2 起多一个来源
			 * —— ffe_qfull_drop=1 时环满直接丢（帧还没改写，bp 同样没
			 * 交给硬件）。两种都必须还一次 bp。
			 */
			zx_rx_consume(desc, rel);
			adapter->rx_cur_idx[q]++;
			work_done++;
			avail--;
			if (ffe == ZX_FFE_RX_DROPPED &&
			    zx_bp_return(adapter, bp))
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
	/* DIAG 3：本轮的"处理帧数 / 其中快路转发数"各进一个桶（0..64） */
	if (unlikely(zx_diag_on(ZX_DIAG_B_POLL)))
		zx_diag_poll(adapter, work_done, diag_fwd);
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
	/*
	 * FIX1：环满要 kick（等 reclaim 把 tx_pending 降下来）；BMU 申请失败
	 * 停过口也一样要 kick —— 否则“队列停着 → 没人申请 → 永远拿不到缓冲”
	 * 就成死结（复审 §1.1 点名的变砖路径）。
	 */
	if (adapter->tx_pending >= READ_ONCE(zx_txq_limit) ||
	    READ_ONCE(adapter->tx_bmu_starved))
		zx_tx_kick(adapter);
	spin_unlock_bh(&adapter->tx_lock);

	/* FIX1：先排待归还队列，再探“停口是否该唤醒” */
	zx_bmu_defer_drain(adapter, 4);
	zx_tx_starve_recover(adapter);
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
	/*
	 * P10.5：bp 生命周期取证（§6 的判读表按这几格读，TASK-P10.5-FIX §7）。
	 *   ffe_dup        RX 拿到的 bp 仍在发送环里（A0 只计数；A3 才拦）
	 *   bp_double_free A2/C1 拒掉的二次归还（bp 仍是 TX，或槽位仍带毒 = 重放）
	 *   tx_dup_reject C1 拒掉的重复提交（提交时 bp 已经在发送环里）
	 *   rx_poison      RX 帧首仍是我们归还时写的毒值（A4）
	 *   bp_state_stale 完成记账漂移、状态表整体重扫的次数（应恒 0）
	 *   bp_tx_now      账上仍在发送环里的 bp 个数（应 ≤ tx_pending）
	 *   bp_poison/bp_drop 两个模块参数的现态（A4 默认 1、A3 自 C1 起默认 1）
	 *   bp_hot         投递次数最多的 8 个 bp —— "同一 bp 的投递速率"
	 */
	{
		u32 tx_now = 0;
		u16 hot_bp[8] = { 0 }, hot_n[8] = { 0 };
		int k, r;

		if (adapter->bp_state)
			for (i = 0; i < ZX_NUM_BUFFERS; i++)
				if (READ_ONCE(adapter->bp_state[i]) == ZX_BP_TX)
					tx_now++;
		for (i = 0; i < ZX_NUM_BUFFERS && adapter->bp_rx_hits; i++) {
			u16 n = READ_ONCE(adapter->bp_rx_hits[i]);

			if (!n)
				continue;
			for (k = 0; k < 8; k++)
				if (n > hot_n[k])
					break;
			if (k == 8)
				continue;
			for (r = 7; r > k; r--) {
				hot_n[r] = hot_n[r - 1];
				hot_bp[r] = hot_bp[r - 1];
			}
			hot_n[k] = n;
			hot_bp[k] = (u16)i;
		}
		/* C1：bp_double_free 非 0 只表示"想再写一份空闲表项但被挡住"，不是"已损坏" */
		seq_printf(s, "ffe_dup=%llu bp_double_free=%llu tx_dup_reject=%llu rx_poison=%llu bp_state_stale=%u bp_tx_now=%u bp_poison=%d bp_drop=%d\n",
			   adapter->ffe_dup, adapter->bp_double_free,
			   adapter->tx_dup_reject,
			   adapter->rx_poison, adapter->bp_state_stale, tx_now,
			   READ_ONCE(ffe_poison), READ_ONCE(ffe_bp_drop));
		seq_puts(s, "bp_hot:");
		for (k = 0; k < 8 && hot_n[k]; k++)
			seq_printf(s, " %u=%u", hot_bp[k], hot_n[k]);
		seq_putc(s, '\n');
	}
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

		seq_printf(s, "tx阶段(ns/包) 申请BMU(锁外) %llu 取锁+回收 %llu 拷贝+同步 %llu 启动 %llu 合计 %llu n=%u\n",
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
	seq_printf(s, "bmu_alloc_fail=%u bmu_alloc_late=%u bmu_free_fail=%u bmu_free_slots=%u tx_bp_alloc_fail=%u\n",
		   adapter->bmu_alloc_fail, adapter->bmu_alloc_late,
		   adapter->bmu_free_fail, adapter->bmu_free_slots,
		   READ_ONCE(adapter->tx_bp_alloc_fail));
	seq_printf(s, "tx_bp_alloc_drop=%u tx_bp_alloc_retry=%u tx_starve_stop=%u starved=%u starve_polls=%u\n",
		   READ_ONCE(adapter->tx_bp_alloc_drop),
		   READ_ONCE(adapter->tx_bp_alloc_retry),
		   adapter->tx_starve_stop,
		   READ_ONCE(adapter->tx_bmu_starved) ? 1 : 0,
		   READ_ONCE(adapter->tx_starve_polls));
	seq_printf(s, "bmu_free_defer=%u bmu_free_defer_ok=%u bmu_free_defer_drop=%u\n",
		   adapter->bmu_free_defer, adapter->bmu_free_defer_ok,
		   adapter->bmu_free_defer_drop);
	/*
	 * P2：qfull_drop 是"环满时按 ffe_qfull_drop=1 直接丢掉"的帧数（v4+v6
	 * 两族之和）。默认 0 时它恒 0、fallback_qfull 才是证据；切到 1 以后
	 * 这两个数一起看就能分清"回栈背压"还是"真丢"。
	 */
	seq_printf(s, "accel v4=%s v6=%s q=%u qfull_drop=%llu\n",
		   zx_accel_mode_name(READ_ONCE(adapter->accel.v4_mode)),
		   zx_accel_mode_name(READ_ONCE(adapter->accel.v6_mode)),
		   READ_ONCE(adapter->tx_pending),
		   READ_ONCE(adapter->accel_qfull_drop[0]) +
		   READ_ONCE(adapter->accel_qfull_drop[1]));
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

	/*
	 * P0（tx-lock-scope）：BMU 申请必须在 tx_lock 之外。这里一次要为每个
	 * 端口拿 1~9 个缓冲，旧版在锁里调 zx_bmu_alloc_bp()（当时是 1 ms 轮询/次）：
	 * 5 个端口 × 每口最多 9 个 bp × 1 ms = 最坏约 45 ms 持锁（复审 §10.2 指出
	 * 之前写“9 ms”只算了单口）。debugfs 写是进程上下文，用会睡的
	 * zx_bmu_alloc_bp_wait()；锁里只剩容量检查、拷贝、写描述符。
	 * 锁里发现容量不够或槽位不相邻时，把还没交出去的 bp 记进 tofree[]，
	 * 解锁后统一归还（zx_bmu_free_bp 自己也要等硬件，不能进锁）。
	 */
	{
		struct zx_eth_priv *apriv[ZX_NUM_PORTS];
		unsigned int andesc[ZX_NUM_PORTS], anslots[ZX_NUM_PORTS];
		int abp[ZX_NUM_PORTS][9];
		u16 tofree[ZX_NUM_PORTS * 9];
		int nfree = 0, f, k2;

		memset(apriv, 0, sizeof(apriv));
		memset(andesc, 0, sizeof(andesc));
		memset(anslots, 0, sizeof(anslots));

		/* 锁外：算每个端口的 ndesc/nslots 并申请槽位 */
		for (p = 0; p < ZX_NUM_PORTS; p++) {
			struct net_device *nd = adapter->ports[p];

			if (!nd || !netif_running(nd))
				continue;
			if (dev[0] && strcmp(nd->name, dev))
				continue;
			apriv[p] = netdev_priv(nd);
			andesc[p] = (which & 0x3) ? 2 : 1;
			anslots[p] = (len + 16 + ZX_BUF_SIZE - 1) / ZX_BUF_SIZE;
			if (andesc[p] == 2)
				anslots[p] = 2;
			for (k2 = 0; k2 < (int)anslots[p]; k2++) {
				abp[p][k2] = zx_bmu_alloc_bp_wait(adapter);
				if (abp[p][k2] < 0 || abp[p][k2] >= ZX_NUM_BUFFERS)
					break;
			}
			if (k2 < (int)anslots[p]) {
				while (k2-- > 0)
					zx_bmu_free_bp(adapter, (u16)abp[p][k2]);
				apriv[p] = NULL;
			}
		}

		spin_lock_bh(&adapter->tx_lock);
		zx_tx_reclaim(adapter);
		for (p = 0; p < ZX_NUM_PORTS; p++) {
			struct zx_eth_priv *pv = apriv[p];
			unsigned int ndesc, nslots, seg[2], cut;
			u16 bp[9];
			int k, ok = 1;

			if (!pv)
				continue;
			ndesc = andesc[p];
			nslots = anslots[p];
			for (k = 0; k < (int)nslots; k++)
				bp[k] = (u16)abp[p][k];
			if (adapter->tx_pending + ndesc > READ_ONCE(zx_txq_limit)) {
				for (k = 0; k < (int)nslots; k++)
					tofree[nfree++] = bp[k];
				continue;
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
						tofree[nfree++] = bp[k];
					continue;
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
						   (ZX_PORT_TX_QUEUE(pv->port) << 20));
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
		for (f = 0; f < nfree; f++)
			zx_bmu_free_bp(adapter, tofree[f]);
	}
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

	/*
	 * P10.4：头行在 cnt=/idx= 之后**追加**分族的 alias 丢弃数（判据用的
	 * 就是这三格）。已有字段与顺序不动——上机的解析脚本按 cnt= 取数。
	 */
	seq_printf(s, "alias_dump: cnt=%u idx=%u alias_v4=%llu alias_v6=%llu alias_unk=%llu\n",
		   cnt, idx,
		   READ_ONCE(adapter->accel.alias_drop[ZX_ALIAS_FAM_V4]),
		   READ_ONCE(adapter->accel.alias_drop[ZX_ALIAS_FAM_V6]),
		   READ_ONCE(adapter->accel.alias_drop[ZX_ALIAS_FAM_UNK]));
	for (i = 0; i < n; i++) {
		/*
		 * cnt < N：槽位从 0 顺序写；cnt >= N：idx 是最老的槽位，
		 * 按时间顺序从它开始绕一圈。
		 */
		u32 slot = cnt >= ZX_ALIAS_SNAP_N ?
			   ((idx + i) & (ZX_ALIAS_SNAP_N - 1)) : i;
		const struct zx_alias_snap *a = &adapter->alias_snap[slot];

		/*
		 * P10.5：行尾追加 bp= / st= / hits= / stamp=（既有字段与顺序
		 * 一字不动，按 len=/port= 或行首 #N 解析的脚本不受影响；
		 * 仍然保持"一条两行：#N 行 + head= 行"的既有形状）。
		 *   bp   = 描述符字节 7/8 还原的槽位号（与 RX 解码同一口径）
		 *   st   = 快照那一刻的 bp_state（0 FREE / 1 TX / 0xff 无效）
		 *   hits = 该 bp 的 RX 投递计数（bp_rx_hits）
		 *   stamp= 快照时刻的 jiffies
		 */
		{
			u16 bpn = (u16)((((a->desc[1] >> 24) & 0xff) >> 1) |
					((a->desc[2] & 0xff) << 7));
			u16 hits = (adapter->bp_rx_hits && bpn < ZX_NUM_BUFFERS) ?
				   READ_ONCE(adapter->bp_rx_hits[bpn]) : 0;

			seq_printf(s, "#%u slot=%u desc0=%08x desc1=%08x desc2=%08x desc3=%08x len=%u port=%u bp=%u st=%u hits=%u stamp=%u\n",
				   i, slot, a->desc[0], a->desc[1], a->desc[2],
				   a->desc[3], a->len, a->port, bpn, a->rsvd,
				   hits, a->stamp);
		}
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

/*
 * DIAG（2026-11-03）：诊断插桩的回读口，**只读 0444**，不写任何寄存器、
 * 不改任何行为。每一项对应一个 module_param（默认全关），关着的那一段
 * 打印出来恒为 0 —— 读的人拿"开关状态那一行 diag: mask=" 对照即可。
 *
 * s->private 就是 debugfs_create_file(..., adapter, ...) 传进来的
 * `struct zx_eth_adapter *`（与 alias_dump/zc/zc_ev 同一写法），**不是**
 * struct device *，绝不能调 dev_get_drvdata()。
 */
static const char * const zx_diag_tlabel[ZX_DIAG_TBUCKETS] = {
	"<1us", "<2us", "<4us", "<8us", "<16us", "<32us", "<64us",
	"<128us", "<256us", "<512us", "<1024us", "<1ms", ">=1ms",
};

static const char * const zx_diag_qlabel[ZX_DIAG_QBUCKETS] = {
	"0-1", "2-3", "4-7", "8-15", "16-31", "32-63", "64-127",
	"128-255", "256-511", "512+",
};

/* 任务书第 6 项要求的"一项一格"，顺序与枚举一致 */
static const char * const zx_diag_dlabel[ZX_DIAG_DROP_N] = {
	"rx_drop_nondev", "rx_drop_down", "skb_nomem", "ffe_rx_drop",
	"ffe_qfull", "ffe_qfull_drop", "txq_block", "txq_xmit_busy",
};

static void zx_diag_show_hist(struct seq_file *s, const u64 *h, int n,
			      const char * const *label)
{
	int i;

	for (i = 0; i < n; i++)
		seq_printf(s, " %s=%llu", label[i], h[i]);
	seq_putc(s, '\n');
}

static int zx_eth_diag_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_diag *d = READ_ONCE(adapter->diag);
	int p, f;

	seq_printf(s, "diag: mask=0x%x burst_gap_us=%u (1=residence 2=seq 4=poll 8=txq 16=txlat 32=drop 64=burst)\n",
		   READ_ONCE(zx_diag_mask), READ_ONCE(zx_diag_burst_us));
	seq_printf(s, "abi: sizeof_adapter=%zu ppe_flows=%zu accel=%zu diag=%zu (ppe.ko 必须与这些偏移同源)\n",
		   sizeof(struct zx_eth_adapter),
		   offsetof(struct zx_eth_adapter, ppe_flows),
		   offsetof(struct zx_eth_adapter, accel),
		   offsetof(struct zx_eth_adapter, diag));
	if (!d) {
		seq_puts(s, "diag: state=NOT-ALLOCATED (probe 的 devm_kzalloc 失败，插桩全部静默)\n");
		return 0;
	}

	/* ---- 1. 盒内停留 ---- */
	seq_puts(s, "-- 1. in-box residence: RX fastpath entry -> TX submit ok (per egress) --\n");
	for (p = 0; p < ZX_NUM_PORTS; p++) {
		seq_printf(s, "egress port%d(%s): n=%llu max=%lluns nostamp_all=%llu",
			   p, adapter->ports[p] ? adapter->ports[p]->name : "-",
			   d->res_n[p], d->res_max_ns[p], d->res_nostamp);
		seq_putc(s, '\n');
		seq_puts(s, "  ");
		zx_diag_show_hist(s, d->res[p], ZX_DIAG_TBUCKETS, zx_diag_tlabel);
	}

	/* ---- 5. TX 完成延迟 ---- */
	seq_puts(s, "-- 5. TX completion latency: writel(TX_START) -> zx_tx_reclaim sees done --\n");
	seq_printf(s, "txlat: n=%llu max=%lluns older-than-reclaim(upper bound)\n  ",
		   d->txlat_n, d->txlat_max_ns);
	zx_diag_show_hist(s, d->txlat, ZX_DIAG_TBUCKETS, zx_diag_tlabel);
	/* 按出口分开：单流"盒子->宿主"看 egress=lan1 这一行 */
	for (p = 0; p < ZX_NUM_PORTS; p++) {
		seq_printf(s, "txlat egress port%d(%s):\n  ", p,
			   adapter->ports[p] ? adapter->ports[p]->name : "-");
		zx_diag_show_hist(s, d->txlat_port[p], ZX_DIAG_TBUCKETS,
				  zx_diag_tlabel);
	}

	/* ---- 2. seq 反演 ---- */
	seq_puts(s, "-- 2. TCP seq inversion (5-tuple hash, 1024 slots per direction) --\n");
	for (f = 0; f < 2; f++)
		seq_printf(s, "%s: probe=%llu hit=%llu rollback=%llu\n",
			   f ? "tx-submit" : "rx-entry  ",
			   d->seq_probe[f], d->seq_hit[f], d->seq_roll[f]);
	seq_printf(s, "collisions=%llu (槽位被别的 5 元组占用，只计数不纠正)\n",
		   d->seq_coll);
	/* 按端口：in = RX 入口的入端口，out = TX 提交的出口端口 */
	for (p = 0; p < ZX_NUM_PORTS; p++)
		seq_printf(s, "port%d(%s): rx_entry hit=%llu rollback=%llu | tx_submit hit=%llu rollback=%llu\n",
			   p, adapter->ports[p] ? adapter->ports[p]->name : "-",
			   d->seq_hit_in[p], d->seq_roll_in[p],
			   d->seq_hit_out[p], d->seq_roll_out[p]);

	/* ---- 3. 每 poll ---- */
	seq_puts(s, "-- 3. frames per NAPI poll --\n");
	seq_printf(s, "polls=%llu sum_handled=%llu sum_fwd=%llu\n",
		   d->poll_n, d->poll_work, d->poll_ffeframes);
	seq_puts(s, "handled:");
	for (f = 0; f < ZX_DIAG_PBUCKETS; f++)
		if (d->poll_frames[f] || f == ZX_DIAG_PBUCKETS - 1)
			seq_printf(s, " %s%d=%llu", f == ZX_DIAG_PBUCKETS - 1 ? "" : "",
				   f, d->poll_frames[f]);
	seq_putc(s, '\n');
	seq_puts(s, "fwd    :");
	for (f = 0; f < ZX_DIAG_PBUCKETS; f++)
		if (d->poll_fwd[f] || f == ZX_DIAG_PBUCKETS - 1)
			seq_printf(s, " %d=%llu", f, d->poll_fwd[f]);
	seq_putc(s, '\n');

	/* ---- 4. TX 环占用 ---- */
	seq_puts(s, "-- 4. TX ring occupancy at submit (tx_pending before ++) --\n");
	seq_puts(s, "all: ");
	zx_diag_show_hist(s, d->txq, ZX_DIAG_QBUCKETS, zx_diag_qlabel);
	for (p = 0; p < ZX_NUM_PORTS; p++) {
		seq_printf(s, "egress port%d(%s): txq_block=%llu ring_full_stop=%llu\n",
			   p, adapter->ports[p] ? adapter->ports[p]->name : "-",
			   d->txq_block[p], d->txq_trip[p]);
		seq_puts(s, "  ");
		zx_diag_show_hist(s, d->txq_port[p], ZX_DIAG_QBUCKETS,
				  zx_diag_qlabel);
	}

	/* ---- 8. 突发 ---- */
	seq_puts(s, "-- 8. bursts per egress (segment length / gap between segments) --\n");
	for (p = 0; p < ZX_NUM_PORTS; p++) {
		int i;

		seq_printf(s, "egress port%d(%s): frames=%llu segments=%llu cur=%llu gap_min=%lluns gap_max=%lluns\n",
			   p, adapter->ports[p] ? adapter->ports[p]->name : "-",
			   d->burst_frames[p], d->burst_seg[p], d->burst_cur[p],
			   d->burst_gap_min_ns[p], d->burst_gap_max_ns[p]);
		seq_puts(s, "  len:");
		for (i = 0; i < ZX_DIAG_BBUCKETS; i++) {
			if (!d->burst_len[p][i])
				continue;
			if (i < 16)
				seq_printf(s, " %d=%llu", i, d->burst_len[p][i]);
			else if (i == 16)
				seq_printf(s, " 16-31=%llu", d->burst_len[p][i]);
			else
				seq_printf(s, " 32+=%llu", d->burst_len[p][i]);
		}
		seq_putc(s, '\n');
		seq_puts(s, "  gap:");
		zx_diag_show_hist(s, d->burst_gap[p], ZX_DIAG_TBUCKETS,
				  zx_diag_tlabel);
	}

	/*
	 * ---- 6. 丢点 ----
	 * 既有计数（adapter 上）**始终有效**，与 diag_drop 开关无关；新增的三格
	 * 只在 diag_drop=1 时累加。一项一格，不合并。
	 */
	seq_puts(s, "-- 6. drop reasons, one per line (source in brackets) --\n");
	seq_printf(s, "rx_bad=%u [main.c desc bp/len invalid]\n", adapter->rx_bad_cnt);
	seq_printf(s, "rx_resync=%u [main.c index resync]\n", adapter->rx_resync_cnt);
	seq_printf(s, "rx_nobuf=%u [main.c bp==0]\n", adapter->rx_nobuf_cnt);
	seq_printf(s, "ffe_dup=%llu [main.c bp still in TX ring]\n", adapter->ffe_dup);
	seq_printf(s, "rx_alias=%u [main.c src MAC is ours]\n", adapter->rx_alias_cnt);
	seq_printf(s, "rx_len_short=%llu [main.c claimed>avail]\n",
		   READ_ONCE(adapter->rx_len_short_cnt));
	seq_printf(s, "rx_len_long=%llu [main.c padding>MAX_PAD]\n",
		   READ_ONCE(adapter->rx_len_long_cnt));
	seq_printf(s, "rx_drop_nondev=%llu [main.c dest_dev==NULL]\n",
		   d->drop[ZX_DIAG_DROP_RX_NONDEV]);
	seq_printf(s, "rx_drop_down=%llu [main.c dest_dev !IFF_UP]\n",
		   d->drop[ZX_DIAG_DROP_RX_DOWN]);
	seq_printf(s, "skb_nomem=%llu [main.c napi_alloc_skb failed]\n",
		   d->drop[ZX_DIAG_DROP_SKB_NOMEM]);
	seq_printf(s, "ffe_rx_drop=%llu [accel.c #19 submit failed after rewrite, v4=%llu v6=%llu]\n",
		   d->drop[ZX_DIAG_DROP_FFE_RX_DROP],
		   READ_ONCE(adapter->accel.cnt[0].rx_drop),
		   READ_ONCE(adapter->accel.cnt[1].rx_drop));
	seq_printf(s, "ffe_qfull=%llu [accel.c #19 ring full, back to stack]\n",
		   d->drop[ZX_DIAG_DROP_QFULL]);
	seq_printf(s, "ffe_qfull_drop=%llu [accel.c #19 ring full, dropped, v4=%llu v6=%llu]\n",
		   d->drop[ZX_DIAG_DROP_QFULL_DROP],
		   READ_ONCE(adapter->accel_qfull_drop[0]),
		   READ_ONCE(adapter->accel_qfull_drop[1]));
	seq_printf(s, "fallback_qfull_total=%llu(v4)/%llu(v6) [accel.c cnt[].fallback_qfull = qfull + qfull_drop]\n",
		   READ_ONCE(adapter->accel.cnt[0].fallback_qfull),
		   READ_ONCE(adapter->accel.cnt[1].fallback_qfull));
	seq_printf(s, "txq_block=%llu [main.c zx_tx_submit_bp hit txq_limit]\n",
		   d->drop[ZX_DIAG_DROP_TXQ_BLOCK]);
	seq_printf(s, "txq_xmit_busy=%llu [main.c xmit -> NETDEV_TX_BUSY, back to qdisc]\n",
		   d->drop[ZX_DIAG_DROP_TXQ_BUSY]);
	return 0;
}

static int zx_eth_diag_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_eth_diag_show, inode->i_private);
}

static const struct file_operations zx_eth_diag_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_eth_diag_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void zx_eth_debugfs_init(struct zx_eth_adapter *adapter)
{
	struct dentry *dir = debugfs_create_dir(dev_name(adapter->dev), NULL);

	debugfs_create_devm_seqfile(adapter->dev, "dma_state", dir,
				    zx_eth_dma_state_show);
	/* DIAG（2026-11-03）：只读诊断口，见 zx_eth_diag_show() 的分段注释 */
	debugfs_create_file("diag", 0444, dir, adapter, &zx_eth_diag_fops);
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

	/*
	 * DIAG 状态：**独立分配**，不进 adapter，所以既有字段偏移不受影响
	 * （见 eth.h 的注释与下面的 BUILD_BUG_ON）。默认全关时它只是一块
	 * 零初始化的内存，任何插桩点都不会碰它。
	 *
	 * 分配失败**不让 probe 失败**：诊断是可选件，拿不到就让 diag 保持
	 * NULL，所有插桩点静默跳过（数据面照常）。seq 表单独再分一次，避免
	 * 一个 ~40 KB 的大块分配受碎片影响。
	 */
	zx_diag_abi_check();
	adapter->diag = devm_kzalloc(dev, sizeof(*adapter->diag), GFP_KERNEL);
	if (adapter->diag) {
		adapter->diag->seq = devm_kzalloc(dev,
			sizeof(*adapter->diag->seq) * 2 * ZX_DIAG_SEQ_SLOTS,
			GFP_KERNEL);
		if (!adapter->diag->seq)
			dev_warn(dev, "diag: seq table allocation failed, diag_seq disabled\n");
	} else {
		dev_warn(dev, "diag: state allocation failed, all diag_* disabled\n");
	}

	adapter->irq = platform_get_irq(pdev, 0);
	if (adapter->irq < 0)
		return adapter->irq;

	adapter->dev = dev;
	spin_lock_init(&adapter->tx_lock);
	spin_lock_init(&adapter->bmu_free_lock);
	spin_lock_init(&adapter->bmu_alloc_lock);
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
	/*
	 * P10.5：bp 生命周期账本（A0）。bp_state 4096 B（两态，u8），
	 * tx_bp 1024×2 B（发送环影子），bp_rx_hits 4096×2 B（投递计数）。
	 * 都是常驻小数组，devm 管生命周期；全零 = 全部 FREE。
	 */
	adapter->bp_state = devm_kzalloc(dev, ZX_NUM_BUFFERS, GFP_KERNEL);
	adapter->tx_bp = devm_kzalloc(dev, ZX_NUM_TX_DESC *
				      sizeof(*adapter->tx_bp), GFP_KERNEL);
	adapter->bp_rx_hits = devm_kzalloc(dev, ZX_NUM_BUFFERS *
					   sizeof(*adapter->bp_rx_hits),
					   GFP_KERNEL);
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
	    !adapter->zc_inring || !adapter->bp_state || !adapter->tx_bp ||
	    !adapter->bp_rx_hits) {
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
	netif_napi_add_weight(adapter->ports[i], &adapter->napi, zx_eth_poll,
			      (int)READ_ONCE(zxnapi_weight));
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

	/* coal-knob：把三个参数值落一次（默认值时无日志、行为与基线相同） */
	zx_coal_sync_probe();
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
