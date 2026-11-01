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
 * P3 起 v4 条目带 NAT 改写：`flags` 的 ZX_FFE_FLOW_NAT_S/_D 两位分别表示
 * "命中后把 saddr/sport 换成 saddr_out/sport_out"与"把 daddr/dport 换成
 * daddr_out/dport_out"，两位可同时置位。改写顺序是：先按旧值算 L4 增量
 * 校验和（RFC 1624，csum_replace4/csum_replace2），再写新地址/端口，最后
 * 整头重算 IPv4 头校验和。UDP 校验和为 0 表示"发送方没算"（RFC 768），
 * 这时地址照改、校验和一个比特都不动。v6 仍然没有 NAT66。
 *
 * P3 还加了 debugfs `ffe_migrate`：hw→ffe 的迁移桥目前是**最小可用版**，
 * 只统计 PPE 已绑定条数并打印样例，不落表（原因见 zx_accel_migrate_store()
 * 的头注释）。
 *
 * P5 加**旁路学习**：在 NF_INET_POST_ROUTING（NF_IP_PRI_LAST，NAT 之后）挂一个
 * NFPROTO_IPV4 钩子，把"本机转发出去、目的不是本机、TCP/UDP"的流从 conntrack
 * 的 ORIGINAL 元组（pre-NAT，就是快路键）与当前报头（post-NAT，就是要写回的
 * 地址/端口）拼成一条候选，写进**每 CPU 的无锁环**；age work 每秒排空一次，
 * 查邻居 MAC、按 ffe_add 的等价路径落表。钩子恒返回 NF_ACCEPT、只做 memcpy +
 * 环写，任何分配都留在 work 里——学习失败不影响转发。开关是 debugfs
 * `ffe_learn`（**默认 0**：probe 期不学，开机窗口过去后由 1 s 节拍自动开
 * 一次，P10.3 门控见 learn_delay_ms 的注释），计数也在那里。
 *
 * P5 修订一（桥出口）：WAN→LAN 的出口是 br-lan 而不是 lanX，钩子原来那条
 * "出口必须是本驱动端口"把整个方向挡住了。现在钩子允许桥出口、只记桥的
 * ifindex，FDB 反查（br_fdb_find_port()，要求持 rtnl）留到 drain 里做；查到
 * 的端口再用 zx_eth_is_port() 复核，无线口/查不到一律放弃，绝不猜。
 *
 * P5 修订二（P5b/P6，三处；都只在本文件与 -eth.h 里）：
 *   1. **两个方向都学**。P5 只学 ctinfo 的 ORIGINAL 侧，于是家庭流量里真正
 *      的 bulk（下载，WAN→LAN，属于连接的 REPLY 侧）完全不加速。现在按
 *      ctinfo 的方向位选元组：ORIGINAL 取 tuple[ORIGINAL]、REPLY 取
 *      tuple[REPLY]，两侧存的都是"该方向报文在链路上的 pre-NAT 形态"，所以
 *      "键 = 元组 / 输出 = 当前报头(post-NAT)"这条规则对两方向同构。依据见
 *      zx_ffe_learn_hook() 的 #11 注释（6.18.55 源码逐行核对过）。
 *   2. **"目的地址属本机"改成延迟判定**（v4 判据 #13 → #13'）。端口映射的
 *      pre-NAT daddr 就是盒子 WAN IP（本机），P5 在判据链里直接 drop_local
 *      回栈，于是命中带 DNAT 的学习条目也永远进不了快路。现在判据链只算
 *      daddr_local，查表之后才裁决：无条目 → miss；有条目且带 NAT_D → 放行
 *      （改写把 daddr 换成 daddr_out）；有条目不带 NAT_D → drop_local。
 *      drop_local 仍是 miss 的子集，但口径从"未查表的本机目的地"变成
 *      "命中条目且该条目没有 DNAT 改写"（见 zx_ffe_forward() 的 #13'）。
 *   3. **ffe_add 幂等**：同 key 已存在时用新节点整体替换（dmac/smac/mtu/NAT
 *      输出/flags 合并/hits/bytes 继承/lastuse 刷新）并返回成功，不再 -EEXIST、
 *      不再产生重复行；学习路径的"已存在就刷新"与它共用 __zx_ffe_store()。
 *      P10.2 F4 起不再原地改活条目，见 __zx_ffe_flow_replace()。
 *
 * P9（回程方向学习修复，**数据面零改动**）：P7 上机确认"回程方向永远学不到"
 * 是学习这一侧的两处阻塞，不是数据面问题（手工 ffe_add 一条回程条目后
 * hits 立刻涨到 119359/168 MB，转发完全正常）。三处改动：
 *   1. **入口端口反查**：回程帧从桥端口（lan2/lan3）进来，桥做本地投递，
 *      br_pass_frame_up() 把 skb->dev 换成 brdev，__netif_receive_skb_core()
 *      再把 skb_iif 改写成 brdev->ifindex，于是钩子里"skb_iif 对得上 ports[]"
 *      永远不成立、候选被**静默丢弃**（连计数都不涨）。现在钩子不命中时再判
 *      一次"这一帧是不是从我们的桥端口进来的"（state->in / skb_iif 对上
 *      某个端口的上层桥），是就记下**源 MAC + 桥 ifindex**；drain 在 rtnl 下用
 *      br_fdb_find_port() 反查源 MAC 得到物理端口（P5.1 出口反查的同一套，
 *      只是方向相反）。反查不到/命中无线口 → learn_nonport++、丢弃。
 *   2. **GSO 放开**：GRO 合并后的 bulk 回程是 GSO skb（实测 7.6–18.5 KB），
 *      旧代码在 #3 一律 return。键只取 IP 头与 L4 头，GRO 合并 skb 的这两层
 *      头逐字段合法；现在的允许条件就是 #5/#6/#9（无分片、TCP/UDP、头在线性
 *      区），见钩子 #6' 的注释。数据面 #17 仍然只吃 <=1514，GSO 大包永远走栈。
 *   3. **出口兜底**：出口先看 state->out（交换机口/桥），都不是时退回
 *      skb_dst(skb)->dev（POST_ROUTING 时路由已定）；仍然解析不出就不落表。
 * 同口保护（数据面 #15）改用**解析后的**入口端口判：回程向入口 lan2、出口
 * lan1，不会被误判成"进=出"。计数与 ffe_learn 的行格式一个字段都没动
 * （learn_nonport 的口径扩到"入口解析不出交换机端口"，语义见 -eth.h）。
 *
 * P10（v6 旁路学习，**数据面零改动**）：把上面整套学习路径扩到 IPv6。
 *   1. **钩子覆盖两个族**：同一个回调 zx_ffe_learn_hook() 注册两次
 *      （learn_hook = NFPROTO_IPV4、learn_hook6 = NFPROTO_IPV6，都是
 *      POST_ROUTING + NF_IP_PRI_LAST），按 state->pf 选报头解析器
 *      （zx_ffe_learn_parse4()/parse6()）。之后的出口解析（P9 的 state->out
 *      + skb_dst 兜底）、入口解析（skb_iif 快路径 + 桥 FDB 反查）、同口保护、
 *      ctinfo 方向、候选组装、每 CPU 环投递**只有一份**，不会两族漂移。
 *   2. **地址按族取宽**：候选里的地址是 union nf_inet_addr；v4 只拷 4 B、v6
 *      拷 16 B，且候选先 memset（去重是整结构 memcmp），所以 v4 的
 *      "余下 12 B"恒为 0，v4 去重语义不变。键仍填进同一个 struct zx_ffe_key
 *      （v4[2] 或 v6[32]，同一片 union）。
 *   3. **v6 报头解析**：从 ipv6_hdr(skb) 取 saddr/daddr；扩展头（逐跳/路由/
 *      分片/目的选项）一律放弃候选、ICMPv6(58) 与 fe80::/10 源排除——全部
 *      与数据面 zx_ffe_v6_check() 的 #7/#12/#11 同一条线，避免学出永远命中
 *      不了的条目。GSO 与 v4 同一口径（不额外拒绝，边界在解析器里）。
 *   4. **下一跳 MAC 按族**：v4 用 arp_tbl + ip_route_output_key()，v6 用
 *      nd_tbl + ip6_route_output()/rt6_nexthop()；"直连优先、否则查路由取
 *      网关"的语义相同。
 * 计数与 ffe_learn 的行格式**一个字段都没动**：cand/add/dup/... 是两族合计，
 * 要分辨族就看 ffe_flows 里条目的地址形态。
 *
 * P10.1（v6 旁路学习加固，**数据面零改动**；全部来自 P10 复审的非阻塞项）：
 *   1. **v6 + NAT 位直接丢弃**（复审 B1）：v6 没有 NAT66，带 NAT 位的 v6 条目
 *      在数据面 #16 必回栈（flow->*_out 恒 0），学进表只是白占一个表项到老化。
 *      真出现 NAT 位只可能是内核挂了 v6 NAT（nat66/masq6）；候选在进环之前就
 *      丢掉并计 nat6，等数据面补上 v6 NAT 改写再放开。
 *   2. **parse6 的线性区基准改成 skb_network_header**（复审 B 第 1 条）：
 *      skb_headlen() 的起点是 skb->data，钩子里二者目前相等，但"从 data 起
 *      有 44 B"证明不了"从 ipv6_hdr 起有 44 B"。按 skb_network_offset() 折算
 *      （int 先转 unsigned 再比，负 offset 直接拒）。
 *   3. **补两条拒绝**（P10.1；与数据面**不是"同口径"，而是严一档**）：
 *      v6 的 payload_len < 4（端口都不全，学出来的键永不命中）与
 *      hop_limit <= 1；parse4 也补 ttl <= 1。钩子在 POST_ROUTING，转发帧的
 *      TTL/hop_limit 已经由 ip_forward()/ip6_forward() 递减过，所以这里看到
 *      的 `<= 1` 对应数据面（递减前判）的 `<= 2`：学习侧多拒一档。方向是
 *      安全的——只会少学一条短 TTL 流（traceroute 那类），不会学出数据面
 *      必拒的条目。
 *   4. **入口/出口解析不出时不再静默丢**（复审 B 第 6 条）：learn_one 里
 *      "出口端口指针为空/没起来"的两处 return 并入 learn_nonport。
 *   5. **v6 钩子优先级改用 NF_IP6_PRI_LAST**（复审 A 第 4 条）：数值与
 *      NF_IP_PRI_LAST 同为 INT_MAX，但 v6 conntrack confirm 用的就是它，两族
 *      各用各的常量，读者不会误以为定义不同。不合并成 NFPROTO_INET。
 *   6. **学习计数按族分开**：learn_cnt[2] 在每处 ++ 时同步 +1，ffe_learn
 *      末尾追加 v4/v6 两行；现有两行（开关 + 两族合计）的字段名与顺序一个
 *      字都没动，debugfs 文件数不变。
 *   7. **P9.1 老账**：brport 加 br_vlan_enabled() 护栏（开了 VLAN filtering 的
 *      桥不拿 vid=0 的 FDB 猜出口）、命中的端口再确认上层桥是候选里那个
 *      （防 ifindex 被复用）。P10.2 更正去重槽尺寸的口径：旧注释按 64 位
 *      主机 ABI 记 104 B，本板 ARM32 实测 dedup=100 B / cand=96 B /
 *      learn_cnt=72 B（见 ZX_FFE_DEDUP_SLOTS 处注释）。
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
#include <asm/barrier.h>
#include <asm/byteorder.h>
#include <asm/checksum.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <linux/if_packet.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kconfig.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/net_namespace.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>	/* NF_IP6_PRI_LAST：v6 钩子优先级 */
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/rhashtable.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <linux/time64.h>
#include <linux/udp.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <net/addrconf.h>
#include <net/arp.h>
#include <net/checksum.h>
#include <net/if_inet6.h>
#include <net/ip.h>
#include <net/ip6_route.h>	/* ip6_route_output()/rt6_nexthop()：v6 下一跳 */
#include <net/ndisc.h>		/* nd_tbl：v6 邻居表 */
#include <net/neighbour.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/route.h>

#include "zx279128s-eth.h"
/* ffe_migrate 要读 adapter->ppe_flows 的条目（struct zx_ppe_entry/cookie） */
#include "zx279128s-ppe.h"

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
 * P10.3 学习门控延时（毫秒）。p10.2 把 learn_on 默认写成 1，两次开机都在
 * ~44 s 硬死机（关中断自旋、engine loaded/flow offload/wifi-on 三行全缺，
 * 见 .scratch/ffe-fastpath/DIAG-P10.2-HANG.md）；而 learn_on=0 开机、运行期
 * 再打开一切正常（p10.3a 阶梯，verify-p10/PHASE-G.md）。
 *
 * 但学习是 v6 加速的必要条件：同一个 p10.3a 镜像，学习关 = 300.8 Mbit/s
 * （ffe_flows 0 行、全 miss 回栈），运行期打开 = 775.1 Mbit/s。
 * 口径限定（REVIEW-P10.3A.md，应 1659bc2f 证据审计）：这两个数是同一镜像上
 * 的两次**单次**测量（相隔数分钟，max cwnd 197k vs 481k、mean RTT 3.7 vs
 * 2.83 ms，互不可比），只能当方向性证据：表空 ⇒ 无命中 ⇒ 没有快路转发；
 * 正式交付验收要补**同轮交替** stack/ffe 各 3 次 ×15 s。
 * 所以交付默认是"probe 期不学，probe 后延时这么久由 1 s 节拍自动开一次"，
 * 既拿到加速，又不让学习在开机窗口里活着。0 = 立刻开（等价于 p10.2 的旧
 * 行为，只在复现死机时用；它是开机参数，不是运行期开关）。
 */
static unsigned int learn_delay_ms = 120000;
module_param(learn_delay_ms, uint, 0444);
MODULE_PARM_DESC(learn_delay_ms, "Delay before ffe_learn auto-enables after probe, in ms (default: 120000, 0 = at probe)");

/*
 * 自动降级的阈值：drop > 0.1% 或 fallback > 50%，持续 10 s 就回 stack
 * （设计 §9-4）。
 *
 * P7 上机后改口径（design/extensibility.md §6.6）：drop 只算 drop_noport，
 * 因为 drop_local 是"目的地址属本机、帧正常回栈、一帧没丢"，语义上不是失败；
 * 而且分子分母都用增量（当前 cnt[] 减去窗口起点的快照 fb_base[]），不再拿
 * 开机累计值判——累计口径下一次 ping -f 就能永久污染判据。窗口小于
 * ZX_FFE_FB_MIN_LOOKUP 帧时不判定（见 zx_accel_degraded()）。
 *
 * P8 C（复审 B/C/D）：上面这个"增量"原先是从**进入 ffe** 那一刻算起的累计
 * （fb_since 只在谓词为假时清零），于是"1 秒抖动 + 之后 10 秒都正常"照样
 * 会在第 10 秒触发降级，和设计 §9-4 的"持续 10 s"不是一回事。现在改成真
 * 10 s 固定窗口：窗口起点的计数快照放在 fb_base[]，起点时刻放在 fb_since[]，
 * 只有窗口到点才判一次，未达阈值就关窗、下一拍从新起点重开。见
 * zx_accel_fb_window_open() 与 zx_accel_autofallback()。
 */
#define ZX_FFE_FB_DROP_PERMILLE		1
#define ZX_FFE_FB_FALLBACK_PERMILLE	500
#define ZX_FFE_FB_WINDOW_MS		10000
/* 窗口内的最小帧数：几帧抖动不足以判降级，也保证判据里没有除零 */
#define ZX_FFE_FB_MIN_LOOKUP		1000

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
 * 条目带 SNAT/DNAT 语义（P3-TASK §0 A）。两位可以同时置位（双向都改），
 * 命中时改写见 zx_ffe_v4_nat_rewrite()：
 *
 *   NAT_S  saddr ← saddr_out、sport ← sport_out
 *   NAT_D  daddr ← daddr_out、dport ← dport_out
 *
 * 只有 v4 条目可能有这两位（v6 没有 NAT66，ffe_add 直接拒）。BIT(1) 在 P2
 * 是"带 NAT 一律回栈"的占位位，P3 把它给 SNAT 用——条目本来就不能跨版本
 * 持久化（只在 debugfs 里现加现用），不需要兼容旧镜像里的内存布局。
 */
#define ZX_FFE_FLOW_NAT_S	BIT(1)
#define ZX_FFE_FLOW_NAT_D	BIT(2)
#define ZX_FFE_FLOW_NAT_MASK	(ZX_FFE_FLOW_NAT_S | ZX_FFE_FLOW_NAT_D)
/*
 * 条目是旁路学习（P5）插进来的，不是 ffe_add 下发的。两位互斥地描述来源：
 * 静态条目老化、命中、改写的规则与学习条目完全一样（P5-TASK §1.5），
 * 区别只在 ffe_flows 行尾打 `L` 而不是 `S`，方便上机时一眼看出这条是谁加的。
 */
#define ZX_FFE_FLOW_LEARNED	BIT(3)
/*
 * 条目的**出口**（P6 分层，design/extensibility.md §1 硬规则 2）：只记"类型 +
 * 标识"，不假设标识是交换机端口号。0 = switch-port，此时 egress_id 就是
 * ports[] 的下标（DT reg，与 ffe_add 的 eport 同一套编号）；1 预留给 wifi-tx
 * （egress_id 是队列/接口号）。判据链与改写只看这两个字段，不认识"交换机端口"。
 * debugfs 的 add/flows 语法与格式不变：只给端口号的调用就是 type 0。
 */
#define ZX_FFE_EGRESS_SWITCH	0

struct zx_ffe_flow {
	struct rhash_head	node;
	struct list_head	list;		/* flow_list：老化与 ffe_flows 遍历 */
	struct rcu_head		rcu;
	struct zx_ffe_key	key;
	u8			dmac[ETH_ALEN];	/* 出口下一跳 MAC（P2 改写用） */
	u8			smac[ETH_ALEN];	/* 出口端口 MAC 快照（P2 改写用） */
	/*
	 * NAT 后的地址/端口（P3）。只有 flags 里对应位有效；没置位时这些字段
	 * 不参与改写（纯路由条目保持 P2 的行为）。判据 #16 要求"置了位就必须
	 * 齐全"：地址或端口为 0 视为未设，命中后回栈。
	 */
	__be32			saddr_out;
	__be32			daddr_out;
	__be16			sport_out;
	__be16			dport_out;
	u8			egress_type;	/* 0 = switch-port，1 = wifi-tx 预留 */
	u16			egress_id;	/* type 0 时 = 交换机端口号（现为 0..4） */
	u8			flags;
	u16			mtu;		/* 出口 MTU 快照（add 时取），0 = 未知 */
	u64			hits;		/* 真正转发出去的帧数（P2 起 == fwd） */
	u64			bytes;
	unsigned long		lastuse;	/* 最近一次命中（转发或环满回栈） */
};

/*
 * 数据面的一次性条目快照（P10.2 F4）。
 *
 * 背景：控制面过去在**活条目**上原地改 flags/dmac/smac/daddr_out 等字段，而
 * 数据面在 RCU 只读段里反复重读它们；最坏结果是"按无 NAT 校验通过、按有 NAT
 * 改写"，发出一个改坏的帧。现在控制面改成"新节点 + rhashtable_replace_fast()
 * + list_replace_rcu()"整体换位，数据面则在查到条目后**只读一次**，把后面
 * 判据/改写/提交要用的字段全部拷进这个结构，之后不再解引用 flow。
 *
 * 只放"会被控制面更新"的字段；hits/bytes/lastuse 是数据面自己写的运行态，
 * 不在快照里（见 zx_ffe_forward() 末尾的记账段）。
 */
struct zx_ffe_flow_rt {
	u8		flags;
	u8		dmac[ETH_ALEN];
	u8		smac[ETH_ALEN];
	u8		egress_type;
	u16		egress_id;
	u16		mtu;
	__be32		saddr_out;
	__be32		daddr_out;
	__be16		sport_out;
	__be16		dport_out;
	u8		proto;
};

/*
 * ffe_add 可选 NAT 尾段的解析结果（P3-TASK §0 A.2）。先解析到这个临时结构，
 * 等 key/eport/dmac 全部通过、容量检查也过了，再一次性写进条目——失败路径
 * 不需要回滚半填的 flow。
 */
struct zx_ffe_nat {
	u8			flags;		/* ZX_FFE_FLOW_NAT_S / _D */
	__be32			saddr_out;
	__be32			daddr_out;
	__be16			sport_out;
	__be16			dport_out;
};

/* ---- 旁路学习（P5-TASK §1）：每 CPU 候选环 -------------------------- */

/*
 * 一条候选就是"钩子看到的一条可学的流"，字段已经拆成 pre-NAT（键）与
 * post-NAT（改写目标）两套，work 里只剩"解析入口/出口 + 查邻居 + 落表"。
 * 钩子里不许分配：这个结构必须是 POD，只 memcpy（整结构先 memset 再赋值，
 * 见 zx_ffe_learn_hook() 的组装段——去重是整结构 memcmp）。
 *
 * 两个方向共用这个结构（P5 修订二 §A）：**键永远是"这一方向的报文在链路上
 * 的 pre-NAT 形态"**（也就是数据面从帧头拼出来的那个键），输出永远是"同一
 * 报文 NAT 之后的形态"（也就是要写到对端的形态）。钩子按 ctinfo 的方向位挑
 * 元组，其它字段的来源不变，所以 drain 与判据链对方向一无所知。
 *
 * 两个协议族也共用它（P10）：地址字段是 union nf_inet_addr（16 B），v4 只用
 * 前 4 B、v6 用满 16 B，**按 family 决定拷几字节**（拷 16 B 会把 v4 的垃圾
 * 字节带进来，而本结构的去重是整结构 memcmp）。family 是必填字段：键按它决定
 * 填 key.v4[] 还是 key.v6[]，下一跳 MAC 按它决定查 arp_tbl 还是 nd_tbl。
 *
 * 入口（P9）：iiport 是"这一帧从哪个交换机端口进来"，只用来做同口保护
 * （数据面 #15 的预判），不进键。快路径由钩子用 skb_iif 直接对出来；桥端口
 * 进来的帧只有源 MAC 与桥的 ifindex，留给 drain 反查 FDB。
 */
struct zx_ffe_learn_cand {
	union nf_inet_addr saddr;	/* pre-NAT：conntrack 该方向的元组（按族取 4/16 B） */
	union nf_inet_addr daddr;
	union nf_inet_addr saddr_out;	/* post-NAT：ip_hdr/ipv6_hdr(skb)（按族取 4/16 B） */
	union nf_inet_addr daddr_out;
	__be16	sport;
	__be16	dport;
	__be16	sport_out;
	__be16	dport_out;
	u8	proto;
	u8	family;		/* 4 / 6：键的族、下一跳用哪张邻居表 */
	u8	eport;		/* 出口端口（DT reg 编号，与 ffe_add 的 eport 同一套） */
	u8	nat;		/* ZX_FFE_FLOW_NAT_S / _D（键与输出不一致的那几维） */
	u8	iiport;		/* 入口端口 0..4；ZX_FFE_IPORT_BRIDGE = 桥、待反查 */
	u32	br_ifindex;	/* 出口桥 ifindex：eport == ZX_FFE_EPORT_BRIDGE 时有效 */
	u32	in_br_ifindex;	/* 入口桥 ifindex：iiport == ZX_FFE_IPORT_BRIDGE 时有效 */
	u8	src_mac[ETH_ALEN]; /* 入口 FDB 反查键：iiport == ZX_FFE_IPORT_BRIDGE 时有效 */
};

/*
 * 钩子从**本族报头**解析出来的"这一帧在链路上的形态"（post-NAT）。两族只看
 * 地址宽度不同，其余字段共用；解析结果先落到这里，再由共用段组装候选。
 */
struct zx_ffe_learn_pkt {
	union nf_inet_addr saddr;
	union nf_inet_addr daddr;
	__be16	sport;
	__be16	dport;
	u8	proto;
	u8	family;
};

/* 按族拷 4/16 字节：v4 的 union 余下 12 B 必须保持 memset 后的 0。 */
static inline void zx_ffe_learn_addr_cpy(union nf_inet_addr *dst,
					 const union nf_inet_addr *src,
					 u8 family)
{
	memcpy(dst, src, family == 6 ? sizeof(struct in6_addr) :
				       sizeof(__be32));
}

/* 按族比较两个地址（NAT 位判定用）；两边的非本族字节都是 0。 */
static inline bool zx_ffe_learn_addr_eq(const union nf_inet_addr *a,
					const union nf_inet_addr *b,
					u8 family)
{
	return !memcmp(a, b, family == 6 ? sizeof(struct in6_addr) :
					  sizeof(__be32));
}

/*
 * cand->eport 的哨兵值（ZX_NUM_PORTS 是 5，0xff 不会与真端口号冲突）。
 * 只表示"出口是桥、eport 待定"，**不**表示任何具体端口。
 */
#define ZX_FFE_EPORT_BRIDGE	0xff

/*
 * cand->iiport 的哨兵值：入口是桥端口、物理端口号待 drain 里的 FDB 反查
 * （P9）。与 ZX_FFE_EPORT_BRIDGE 数值相同但语义相反（那是出口、这是入口），
 * 同一条候选里两位各管一头，互不干扰。
 */
#define ZX_FFE_IPORT_BRIDGE	0xff

/*
 * 每 CPU 一个环：head 只有本 CPU 的钩子写（smp_store_release 发布，槽位填完
 * 才可见），tail 只有 age work 的 drain 写，所以数据面**无锁、无原子指令**。
 * 环满只丢候选（learn_drop++），绝不阻塞转发——学习失败不影响数据面。
 */
#define ZX_FFE_LEARN_RING	256	/* 必须是 2 的幂（下标用 & 取模） */

/*
 * 投递去重：同一五元组 1 s 内只投一次（P5 修订一的可选优化）。
 *
 * 一条大流每秒会投上千条一模一样的候选，把每 CPU 的 256 槽环挤满
 * （learn_drop 涨），drain 里还要做上千次重复的邻居查询/落表。数据面命中时
 * 自己会刷新 lastuse，所以 dmac/MTU 每 1 s 刷新一次完全够用。
 *
 * 16 槽 × 100 B 的线性比（memcmp 整键 96 B，P10 把地址加宽到 16 B 之后）+
 * 一个 jiffies，只在**已经确认环有空位**之后才做；不分配、不取锁，表满就
 * 覆盖最老的一格（dedup_idx 轮转）。
 *
 * 尺寸口径（P10.2 更正）：旧注释写"104 B"是**按 64 位主机 ABI** 算的
 * （unsigned long 8 B + cand 96 B）。本板是 ARM32：unsigned long 4 B，
 * 所以实测 dedup=100 B、整键 cand=96 B、learn_cnt=72 B（9×u64）。
 * 这些数只影响每 CPU 环占多少内存，不参与任何判据。
 * 同一 CPU 的软中断理论上可能嵌套，最坏结果是偶尔多投/漏投一条候选；计数
 * 本来就是"尽力而为"的口径，接受。
 */
#define ZX_FFE_DEDUP_SLOTS	16	/* 必须是 2 的幂（下标用 & 取模） */
#define ZX_FFE_DEDUP_MS		1000

struct zx_ffe_learn_dedup {
	unsigned long		ts;	/* 上次投递的 jiffies */
	struct zx_ffe_learn_cand cand;	/* 整键：同一条流的字段完全一致 */
};

struct zx_ffe_learn_ring {
	u32			head;
	u32			tail;
	u32			dedup_idx;	/* 只有本 CPU 的钩子写 */
	struct zx_ffe_learn_dedup dedup[ZX_FFE_DEDUP_SLOTS];
	struct zx_ffe_learn_cand cand[ZX_FFE_LEARN_RING];
};

/* 1 s 内同一条流已经投过 → true（调用方计 learn_dedup 并直接返回）。 */
static bool zx_ffe_learn_dup(struct zx_ffe_learn_ring *ring,
			     const struct zx_ffe_learn_cand *cand)
{
	unsigned long now = jiffies;
	unsigned long ttl = msecs_to_jiffies(ZX_FFE_DEDUP_MS);
	int i;

	for (i = 0; i < ZX_FFE_DEDUP_SLOTS; i++) {
		struct zx_ffe_learn_dedup *d = &ring->dedup[i];

		/* time_before() 自带 jiffies 回绕处理 */
		if (time_before(now, d->ts + ttl) &&
		    !memcmp(&d->cand, cand, sizeof(*cand)))
			return true;
	}
	i = ring->dedup_idx & (ZX_FFE_DEDUP_SLOTS - 1);
	ring->dedup_idx++;
	ring->dedup[i].ts = now;
	memcpy(&ring->dedup[i].cand, cand, sizeof(*cand));
	return false;
}

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

/*
 * 把一个已经填好字段的条目挂进表（rhashtable + flow_list 同时加）。
 * 调用者持 accel->lock，并且已经做过容量检查；失败时条目没进任何结构，
 * 由调用者 kfree。
 *
 * P5 把这段从 ffe_add 里抽出来给旁路学习复用（P5-TASK §2 的接口要求）：
 * 静态条目与学习条目在表里的存在形式必须一模一样，差别只在 flags。
 */
static int __zx_ffe_insert(struct zx_eth_adapter *adapter,
			   struct zx_ffe_flow *flow)
{
	struct zx_accel *accel = &adapter->accel;
	int ret;

	ret = rhashtable_insert_fast(&accel->flows, &flow->node,
				     zx_ffe_ht_params);
	if (ret)
		return ret;
	list_add_rcu(&flow->list, &accel->flow_list);
	accel->n_flows++;
	return 0;
}

/*
 * 同 key 条目的合并语义（P10.2 F4）：ffe_add 与旁路学习对"这条流已经在表
 * 里"的处理必须是同一段代码，否则同一条流会被写进两行（P4 验收实测出现过），
 * 或者学习条目与静态条目在 flags 上打架。
 *
 * 谁赢：**后写者赢**。同一个 key 就是同一条流，两边的字段本该一致；用户用
 * ffe_add 钉住的 dmac/eport/MTU 也以用户给的为准（学习条目被钉住后就在
 * ffe_flows 里变成 `S`，见下）。
 *
 * flags 合并规则（改之前先想清楚 ffe_flows 的行尾标记与数据面判据）：
 *   - NAT 位只增不减：同一 key 的 NAT 语义不会"一会儿 NAT 一会儿不 NAT"
 *     （NAT 在 conntrack 里是先确定的、对整条连接一致的），后来者没给 nat/dnat
 *     段时**不许**把旧位擦掉，否则数据面会把带 NAT 语义的帧当纯路由发出去；
 *   - NAT 输出字段按新给者的维度覆盖：带 NAT_S 就更新 saddr_out/sport_out，
 *     带 NAT_D 就更新 daddr_out/dport_out，另一维保持旧值；
 *   - STATIC 只增不减，且一旦是静态条目就清掉 LEARNED：用户钉过的条目在
 *     ffe_flows 里只打 `S`，不会出现 `S L` 两个标记；
 *   - 学习条目不能把静态条目降级成学习条目。
 * hits/bytes 不清零：还是同一条流，计数连续比"看起来是新建的"有用。
 * **与旧 __zx_ffe_flow_update() 语义一致：更新时刷新 `lastuse`**
 * （任务书 §F4 的原意是"别让撕裂读"，不是"别刷新时间戳"；零回归优先，
 * 主 agent 13:5x 拍板保持刷新）。旧实现结尾就是
 * `flow->lastuse = jiffies`，即控制面更新被视为"这条流还活跃"。若改成继承
 * 旧值，只靠 ffe_add/learn_dup 续命、数据面从不命中的条目会比以前更早老化，
 * 出现"反复更新却被老化删掉"的抖动（P10.2 复审整改 2）。
 *
 * P10.2 F4：这段现在只操作**还没进表的新节点**（new 已由调用者按"后写者赢"
 * 填好 dmac/smac/egress/mtu/NAT 输出），不再触碰活条目。旧条目的字段只被
 * 读、不被写；运行态（hits/bytes）在这里继承到新节点，lastuse 刷新。
 */
static void __zx_ffe_flow_merge(struct zx_ffe_flow *new,
				const struct zx_ffe_flow *old)
{
	/* NAT 位只增不减；新给者没给的维度继承旧值 */
	new->flags |= old->flags & ZX_FFE_FLOW_NAT_MASK;
	if (old->flags & ZX_FFE_FLOW_NAT_S &&
	    !(new->flags & ZX_FFE_FLOW_NAT_S)) {
		new->saddr_out = READ_ONCE(old->saddr_out);
		new->sport_out = READ_ONCE(old->sport_out);
	}
	if (old->flags & ZX_FFE_FLOW_NAT_D &&
	    !(new->flags & ZX_FFE_FLOW_NAT_D)) {
		new->daddr_out = READ_ONCE(old->daddr_out);
		new->dport_out = READ_ONCE(old->dport_out);
	}
	if ((old->flags | new->flags) & ZX_FFE_FLOW_STATIC) {
		new->flags |= ZX_FFE_FLOW_STATIC;
		new->flags &= ~ZX_FFE_FLOW_LEARNED;
	} else {
		new->flags |= ZX_FFE_FLOW_LEARNED;
	}
	/*
	 * 运行态：hits/bytes 继承（统计连续），lastuse 刷新（与旧原地更新
	 * 语义一致：控制面更新 = 这条流还活跃，避免"反复更新却被老化删掉"）。
	 */
	new->hits = READ_ONCE(old->hits);
	new->bytes = READ_ONCE(old->bytes);
	new->lastuse = jiffies;
}

/*
 * 用新节点整体替换同 key 的旧节点（P10.2 F4）。
 *
 * 为什么必须换节点而不是原地改：数据面在 RCU 只读段里读 flags/dmac/smac/
 * daddr_out 等字段，控制面原地写会让它读到"一半旧一半新"的组合（最坏是
 * "按无 NAT 校验通过、按有 NAT 改写"）。换节点后数据面要么拿旧、要么拿新，
 * 两边都是自洽的一份快照。
 *
 * 两条归属必须同时换，且都要在 accel->lock 里：
 *   - rhashtable：rhashtable_replace_fast() 在同一个桶里把 obj_old 换
 *     obj_new（内部就是 rcu_assign_pointer，见 6.18.55
 *     include/linux/rhashtable.h:1147-1195）。数据面查表只会看到旧或新，
 *     **不会短暂 miss**；表元素数不变，所以 n_flows 不用动。
 *   - flow_list（老化/ffe_flows 遍历）：list_replace_rcu() 把 prev->next 和
 *     next->prev 原子指向新节点（include/linux/rculist.h:244-262），前向遍历
 *     的读者要么看到旧、要么看到新；老化 work 与本函数持同一把 accel->lock，
 *     不会看到"哈希换了、链表还是旧的"的半状态。
 *
 * 旧节点 kfree_rcu()：还在 RCU 只读段里、拿着旧指针的数据面可以继续用到
 * 宽限期结束。它们对 hits/bytes/lastuse 的写入会落到旧节点上（替换瞬间之后
 * 的在飞帧，计数丢几个），不影响转发内容——新节点在替换时已经继承了旧节点的
 * hits/bytes，并把 lastuse 刷新为替换时刻。
 *
 * 失败（只可能是 -ENOENT/-EINVAL，正常不可达）时两条结构都原封不动，新节点
 * 由调用者释放；**不做**原地更新兜底，否则又把撕裂读放回来了。
 */
static int __zx_ffe_flow_replace(struct zx_eth_adapter *adapter,
				 struct zx_ffe_flow *old,
				 struct zx_ffe_flow *new)
{
	struct zx_accel *accel = &adapter->accel;
	int ret;

	__zx_ffe_flow_merge(new, old);

	ret = rhashtable_replace_fast(&accel->flows, &old->node, &new->node,
				      zx_ffe_ht_params);
	if (ret)
		return ret;

	list_replace_rcu(&old->list, &new->list);
	kfree_rcu(old, rcu);
	return 0;
}

/*
 * 落表（ffe_add 与旁路学习共用的唯一入口）。调用者持 accel->lock。
 *   返回 1  = 同 key 已存在：新节点已整体换上（旧节点 kfree_rcu），
 *             **flow 已经属于表**，调用者不要再释放它；
 *   返回 0  = 新键，已插入（flow 属于表）；
 *   返回 <0 = 失败（-ENOSPC = 表满；-ENOMEM/-EEXIST 来自 rhashtable；
 *             replace 失败是 -EINVAL/-ENOENT）。失败时 flow **没有进表**，
 *             调用者自己释放。
 *
 * 容量检查放在**查表之后**：同 key 的更新不该因为表满被拒（ffe_add 幂等化的
 * 要点之一——表里已满 1024 条时，对一条已存在的键重新 add 必须成功）。表满
 * 只拒绝**新键**，照旧计 tbl_full、不驱逐，等老化腾位置。
 */
static int __zx_ffe_store(struct zx_eth_adapter *adapter,
			  struct zx_ffe_flow *flow)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *old;
	int ret;

	rcu_read_lock();
	old = rhashtable_lookup(&accel->flows, &flow->key, zx_ffe_ht_params);
	rcu_read_unlock();
	if (old) {
		ret = __zx_ffe_flow_replace(adapter, old, flow);
		return ret ? ret : 1;
	}
	if (accel->n_flows >= ZX_FFE_FLOWS_MAX) {
		accel->tbl_full++;
		return -ENOSPC;
	}
	return __zx_ffe_insert(adapter, flow);
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
	if (ret)
		kfree(e);
	else
		list_add_rcu(&e->list, &accel->local_ip6_list);
	spin_unlock_irqrestore(&accel->local_lock, flags);
	/*
	 * P10.3：告警搬到锁外。关中断区间里 printk 会经 console 锁写串口，
	 * console 被别的 CPU 攥住时 console_trylock_spinning() 会**关中断自旋**
	 * 等——这正是 p10.2 死机签名（关中断、tick/NMI 全停）的一种可能形态；
	 * 见 .scratch/ffe-fastpath/DIAG-BOOTLEARN-RACE.md §Q4③。
	 * 语义不变：-EEXIST 静默、其他失败限速吼一声、失败不挂链表。
	 */
	if (ret && ret != -EEXIST)
		dev_warn_ratelimited(adapter->dev,
			"ffe: cannot record a local IPv6 address: %d (a frame to it may be forwarded instead of terminated)\n",
			ret);
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
 * 出口"现在能不能发"的唯一判据（P10.2 F3）：判据段（usable）与提交段
 * （submit）必须用**同一套**，否则两处一漂移就会出现"判据说能发、提交段
 * 按另一套判据说不能"或反过来。判据就是 zx_ffe_eport_dev() 的三条：
 * 端口存在 + netif_running + netif_carrier_ok。
 *
 * 数据面不取 rtnl（会与 ndo 路径构成 ABBA），所以这两次判定都是良性竞态：
 * 判不准的后果只有两种——判据段回栈（不丢包），或提交段丢帧（rx_drop++，
 * bp 由 main.c 的 DROPPED 收尾还给 BMU）。绝不允许把帧发给一个已经
 * carrier off 的端口却还记 fwd++。
 */
static inline bool zx_ffe_out_ok(struct zx_eth_adapter *adapter, u16 id)
{
	return zx_ffe_eport_dev(adapter, id) != NULL;
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

/* ---- 旁路学习：POST_ROUTING 钩子 → 每 CPU 环 → age work 落表 -------- */

/*
 * 下一跳 MAC（P10 起按族分成 v4/v6 两份实现，语义逐条对应）。先在出口 dev
 * 上按**目的地址**查邻居表（v4 = arp_tbl、v6 = nd_tbl，ha/nud_state/ha_lock
 * 的读法完全一样）；查不到说明这个目的不直连，要用出口 dev 做一次路由查询、
 * 拿网关地址再查一次。只在 work 里调（进程上下文，可以睡眠），钩子里一步都
 * 不能碰。
 *
 * neigh_lookup() 不创建邻居（那是 __neigh_lookup/neigh_create 的事）：邻居表
 * 是转发路径自己填的，我们只是抄。拿不到 = 不学，绝不为了学习去触发 ARP/ND。
 * n->ha 由 ha_lock（seqlock）保护，读侧用 read_seqbegin/retry 复制，
 * nud_state 必须落在 NUD_VALID 里，否则 ha 还是全 0（INCOMPLETE）。
 */
static bool zx_ffe_learn_neigh4(struct net_device *out, __be32 daddr, u8 *dmac)
{
	struct neighbour *n;
	unsigned int seq;
	bool ok = false;

	n = neigh_lookup(&arp_tbl, &daddr, out);
	if (!n)
		return false;
	if (READ_ONCE(n->nud_state) & NUD_VALID) {
		do {
			seq = read_seqbegin(&n->ha_lock);
			memcpy(dmac, n->ha, ETH_ALEN);
		} while (read_seqretry(&n->ha_lock, seq));
		ok = !is_zero_ether_addr(dmac);
	}
	neigh_release(n);
	return ok;
}

/*
 * 一条候选的下一跳：直连目的先查，查不到再按出口 dev 查路由取网关。
 * flowi4_oif 钉住出口，保证拿到的是"这一帧真会走的那条路由"的网关。
 */
static bool zx_ffe_learn_mac4(struct net_device *out, __be32 daddr, u8 *dmac)
{
	struct flowi4 fl4;
	struct rtable *rt;
	__be32 nh;

	if (zx_ffe_learn_neigh4(out, daddr, dmac))
		return true;

	memset(&fl4, 0, sizeof(fl4));
	fl4.daddr = daddr;
	fl4.flowi4_oif = out->ifindex;
	rt = ip_route_output_key(dev_net(out), &fl4);
	if (IS_ERR(rt))
		return false;
	nh = rt->rt_gw4;
	ip_rt_put(rt);
	if (!nh || nh == daddr)
		return false;
	return zx_ffe_learn_neigh4(out, nh, dmac);
}

/*
 * IPv6 版下一跳 MAC（P10）：邻居表是 nd_tbl；路由查询用 ip6_route_output()
 * + flowi6_oif 钉住出口（与 v4 版 ip_route_output_key() 一一对应），网关由
 * rt6_nexthop() 选（RTF_GATEWAY 时是 rt6i_gateway，否则是目的地址本身）。
 * 两族的"直连优先、否则查路由取网关"语义相同，只是 API 与表不同。
 */
static bool zx_ffe_learn_neigh6(struct net_device *out,
				const struct in6_addr *daddr, u8 *dmac)
{
	struct neighbour *n;
	unsigned int seq;
	bool ok = false;

	n = neigh_lookup(&nd_tbl, daddr, out);
	if (!n)
		return false;
	if (READ_ONCE(n->nud_state) & NUD_VALID) {
		do {
			seq = read_seqbegin(&n->ha_lock);
			memcpy(dmac, n->ha, ETH_ALEN);
		} while (read_seqretry(&n->ha_lock, seq));
		ok = !is_zero_ether_addr(dmac);
	}
	neigh_release(n);
	return ok;
}

static bool zx_ffe_learn_mac6(struct net_device *out,
			      const struct in6_addr *daddr, u8 *dmac)
{
	struct flowi6 fl6;
	struct dst_entry *dst;
	const struct in6_addr *nh;
	bool ok;

	if (zx_ffe_learn_neigh6(out, daddr, dmac))
		return true;

	memset(&fl6, 0, sizeof(fl6));
	fl6.daddr = *daddr;
	fl6.flowi6_oif = out->ifindex;
	dst = ip6_route_output(dev_net(out), NULL, &fl6);
	if (IS_ERR_OR_NULL(dst))
		return false;
	nh = rt6_nexthop((const struct rt6_info *)dst, daddr);
	ok = !ipv6_addr_equal(nh, daddr) && zx_ffe_learn_neigh6(out, nh, dmac);
	dst_release(dst);
	return ok;
}

/*
 * 按族分派（P10）：候选里存的是 union nf_inet_addr，family 已经定了宽度；
 * 这里只把"查哪张邻居表"的选择收在一处，learn_one 的两个出口共用。
 */
static bool zx_ffe_learn_mac_family(struct net_device *out, u8 family,
				    const union nf_inet_addr *nh, u8 *dmac)
{
	if (family == 6)
		return zx_ffe_learn_mac6(out, &nh->in6, dmac);
	return zx_ffe_learn_mac4(out, nh->ip, dmac);
}

/*
 * 桥 + 一个 MAC → 真正的交换机端口号（FDB 反查）。只在 drain（进程上下文）
 * 里调。**两个方向共用**：出口查**目的** MAC（P5.1：WAN→LAN 的目的主机在桥
 * 的 FDB 里），入口查**源** MAC（P9：桥端口进来的回程帧，skb_iif 已经被改写
 * 成桥的 ifindex，只能靠源 MAC 反查它是从哪个物理口进的）。方向由调用者
 * 决定，函数只做"MAC → 端口"。
 *
 * 钩子在软中断里，**不能**自己查 FDB：linux-6.18.55 的
 *   struct net_device *br_fdb_find_port(const struct net_device *br_dev,
 *                                       const unsigned char *addr, __u16 vid)
 * （include/linux/if_bridge.h:177）开头就是 ASSERT_RTNL()，之后才进
 * rcu_read_lock() 查 fdb 哈希（net/bridge/br_fdb.c:235）。也就是说它虽然只
 * 做 RCU 读，但当前实现要求调用者持 rtnl——软中断里拿 rtnl 等于睡眠，不
 * 行。所以钩子只把桥的 ifindex（+ 需要时的 MAC）记进候选，这里补查，且
 * rtnl 只在这几行里。
 *
 * vid 用 0：OpenWrt 默认 br-lan 的 vlan_filtering=0，FDB 条目都是 vid 0。
 * P10.1 起对"开了 VLAN filtering 的桥"显式拒绝（br_vlan_enabled()）：
 * 这种桥的 FDB 条目带 vid，用 vid=0 查到的不是这一帧的出口（或干脆查不到），
 * 而快路的键里没有 vid（数据面 #4 只吃无 tag 帧），学出来也是错的——不拿 FDB
 * 当"无 VLAN"用。不猜的理由：要查对 vid 得先知道帧所属的 VLAN，而钩子拿不到
 * （出口方向的 tag 由路由/桥决定，入口方向的 vid 在 br_allowed_ingress() 里
 * 算完就丢了）。
 *
 * 返回 0..ZX_NUM_PORTS-1 是真端口；ZX_FFE_BRPORT_NOFDB = FDB 没命中（还没
 * 学到 / 这一帧本来要被泛洪 / 目标是桥自己的地址 / 桥开了 VLAN filtering）；
 * ZX_FFE_BRPORT_NONPORT = 命中的不是本驱动交换机口（无线 phy0-ap0、macvlan
 * 等），或者这个端口已经不在 br 上（ifindex 被复用，P10.1 的二次确认）。
 *
 * 无线口必须挡住：br_fdb_find_port() 对 phy0-ap0 照样返回非空 netdev，但那
 * 个 netdev 的 netdev_priv 不是本驱动私有结构，不存在 0..4 的 TX 队列语义；
 * 把无线 MAC 写进描述符 = 发错地方，而且绕过了 mac80211 的队列/聚合/功率
 * 控制。zx_eth_is_port() 只比 netdev_ops 指针，对任意 netdev 都安全。
 *
 * CONFIG_BRIDGE=n：没有桥设备，静态退化成 NOFDB（调用方计数放弃），
 * #else 分支不引用 br_fdb_find_port。
 */
#define ZX_FFE_BRPORT_NOFDB	(-1)
#define ZX_FFE_BRPORT_NONPORT	(-2)

static int zx_ffe_learn_brport(struct zx_eth_adapter *adapter,
			       struct net_device *br, const u8 *dmac)
{
#if IS_ENABLED(CONFIG_BRIDGE)
	struct net_device *port;
	int eport = ZX_FFE_BRPORT_NOFDB;
	int i;

	/*
	 * P10.1（P9.1 老账）：开了 VLAN filtering 的桥不做 FDB 反查。FDB 条目
	 * 带 vid，vid=0 查到的可能根本不是这条流的出口，而快路键里没有 vid。
	 * 宁可这一拍不学（NOFDB），也不把别的 VLAN 的端口当成出口写进描述符。
	 * br_vlan_enabled() 只读 bridge 的 opt 位，不要求 rtnl（调用点都在
	 * rtnl 之外/之内均可）。
	 */
	if (br_vlan_enabled(br))
		return ZX_FFE_BRPORT_NOFDB;

	rtnl_lock();
	port = br_fdb_find_port(br, dmac, 0);
	if (port) {
		/*
		 * 先复核"是不是本驱动端口"，再确认它确实还挂在 br 上，最后做
		 * ports[] 反查。上层桥的确认是 P10.1 加的：候选里只存了桥的
		 * ifindex，drain 晚于钩子最多 1 s，这个号可能已经被别的桥复用，
		 * FDB 也可能返回一个不在 br 上的端口；rtnl 下比指针是安全的，
		 * 代价一次比较，错判的代价是把帧从错的端口发出去。
		 */
		eport = ZX_FFE_BRPORT_NONPORT;
		if (zx_eth_is_port(port) &&
		    netdev_master_upper_dev_get(port) == br) {
			for (i = 0; i < ZX_NUM_PORTS; i++)
				if (READ_ONCE(adapter->ports[i]) == port)
					eport = i;
		}
	}
	rtnl_unlock();
	return eport;
#else
	(void)adapter;
	(void)br;
	(void)dmac;
	return ZX_FFE_BRPORT_NOFDB;	/* 没编桥：桥出口不可能存在 */
#endif
}

/*
 * 一条候选落表（P5-TASK §1.4）：与 ffe_add 走同一个 __zx_ffe_store，区别只在
 * 字段来源——键来自 pre-NAT 元组、出口/NAT 来自 post-NAT 报头、dmac 来自上面
 * 的邻居查询。**方向无关**：两个方向的候选在钩子里就已经归一成"键=线上 pre-NAT
 * 形态、输出=NAT 后形态"，这里不需要知道它来自连接的哪一侧。
 * **协议族也只影响三处**（P10）：下一跳查哪张邻居表、键填 key.v4[] 还是
 * key.v6[]、NAT 输出字段（v6 没有 NAT66，保持 0）。入口/出口解析、同口保护、
 * 落表路径两族完全共用。
 *
 * 入口（P9）：桥端口进来的候选只带"源 MAC + 入口桥 ifindex"，这里在 rtnl 下
 * 用 FDB 反查物理端口；反查不出/命中无线口 → learn_nonport++、丢弃。入口端口
 * 不进帧键，只用于同口保护（数据面 #15 的预判）：同口进出的流数据面会回栈，
 * 学了也用不上。
 *
 * 出口分两种（P5 修订一）：
 *   - 钩子直接拿到交换机口：out = ports[cand->eport]，邻居按这个口查；
 *   - 钩子拿到桥（WAN→LAN 的常态）：先用**桥**查下一跳 MAC（LAN 主机的 ARP/
 *     ND 邻居挂在 br-lan 上，不在 lanX 上），再拿这个 MAC 查桥 FDB 得到真正
 *     的出口端口；smac 必须是**桥**的 dev_addr（对端 ARP/ND 表里网关就是桥
 *     的 MAC），MTU 取出口端口的。
 *   两条路的出口都必须解析成 0..4 的真端口才落表（解析失败不猜、不落表）。
 *
 * 已存在（同 key）：交给 __zx_ffe_flow_replace() 用新节点整体替换，计
 * learn_dup。字段与 flags 的合并规则写在 __zx_ffe_flow_merge() 头上（NAT 位
 * 只增不减、STATIC 只增不减）；替换经 rhashtable_replace_fast() +
 * list_replace_rcu()，数据面拿到的永远是自洽的一份（旧或新），不再有"读到
 * 撕裂的 6 字节 MAC"那个窗口（P10.2 F4 之前是原地写、没有加锁）。
 */
static void zx_ffe_learn_one(struct zx_eth_adapter *adapter,
			     const struct zx_ffe_learn_cand *cand)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *flow;
	struct net_device *out;		/* 真正的交换机出口口：决定 eport 与 MTU */
	struct net_device *br = NULL;	/* 桥出口：决定 smac，也是 FDB 反查的入口 */
	struct zx_ffe_key key;
	union nf_inet_addr nh;		/* 下一跳（按族取 4/16 B，非本族字节为 0） */
	u8 smac[ETH_ALEN];
	u8 dmac[ETH_ALEN];
	u8 family = cand->family;
	int eport = cand->eport;
	int iiport = cand->iiport;
	int ret;

	/* DNAT 之后的目的地址才是链路上的目标；SNAT 不动目的地址（v6 无 NAT） */
	nh = (cand->nat & ZX_FFE_FLOW_NAT_D) ? cand->daddr_out : cand->daddr;

	/*
	 * 入口（P9）：快路径的 iiport 是钩子用 skb_iif 直接对出来的；桥端口
	 * 进来的帧只有"源 MAC + 桥 ifindex"，这里在 rtnl 下反查（与出口反查
	 * 同一个 zx_ffe_learn_brport()，只是这次查的是**源** MAC）。
	 *
	 * 反查不到 / 命中的不是本驱动交换机口（无线口等）→ 整条放弃并计
	 * learn_nonport（P9 起这个计数的口径含"入口解析不出交换机端口"）：
	 * 数据面只从交换机端口的 RX 环进来，入口不在交换机端口上的流学了也
	 * 永远命中不了；猜一个端口只会把同口保护判错，所以宁可放弃。
	 *
	 * 桥被删/ifindex 被复用同样计 learn_nonport：这条候选已经没有可用的
	 * 入口坐标，下一拍同一条流会重新生成候选。
	 */
	if (iiport == ZX_FFE_IPORT_BRIDGE) {
		struct net_device *inbr;
		int p;

		inbr = dev_get_by_index(&init_net, cand->in_br_ifindex);
		if (!inbr || !netif_is_bridge_master(inbr)) {
			accel->learn_nonport++;
			accel->learn_cnt[family == 6].nonport++;
			if (inbr)
				dev_put(inbr);
			return;
		}
		p = zx_ffe_learn_brport(adapter, inbr, cand->src_mac);
		dev_put(inbr);
		if (p < 0) {
			accel->learn_nonport++;
			accel->learn_cnt[family == 6].nonport++;
			return;
		}
		iiport = p;
	}
	if (iiport < 0 || iiport >= ZX_NUM_PORTS) {
		/* 钩子只会填 0..4 或 ZX_FFE_IPORT_BRIDGE；越界说明候选坏了 */
		accel->learn_nonport++;
		accel->learn_cnt[family == 6].nonport++;
		return;
	}

	if (eport == ZX_FFE_EPORT_BRIDGE) {
		br = dev_get_by_index(&init_net, cand->br_ifindex);
		if (!br || !netif_is_bridge_master(br)) {
			/* 桥被删了/ifindex 被复用：本拍不学，下一拍重新判断 */
			accel->learn_nobrport++;
			accel->learn_cnt[family == 6].nobrport++;
			if (br)
				dev_put(br);
			return;
		}
		if (!zx_ffe_learn_mac_family(br, family, &nh, dmac)) {
			accel->learn_noneigh++;
			accel->learn_cnt[family == 6].noneigh++;
			dev_put(br);
			return;
		}
		eport = zx_ffe_learn_brport(adapter, br, dmac);
		if (eport == ZX_FFE_BRPORT_NOFDB) {
			/* FDB 没命中：还没学到或这一帧要被泛洪 → 不学，别猜 */
			accel->learn_nobrport++;
			accel->learn_cnt[family == 6].nobrport++;
			dev_put(br);
			return;
		}
		if (eport < 0) {
			/* 命中的是无线口等非本驱动端口：桥→无线的帧必须回栈 */
			accel->learn_nonport++;
			accel->learn_cnt[family == 6].nonport++;
			dev_put(br);
			return;
		}
		out = READ_ONCE(adapter->ports[eport]);
		/*
		 * P10.1（复审 B 第 6 条）：出口端口指针为空/没起来时原来静默
		 * return，上机时会被误判成"根本没学到"。并入 learn_nonport
		 * （口径是"这条候选的出口坐标已经不可用"），不新增字段。
		 */
		if (!out || !netif_running(out)) {
			accel->learn_nonport++;
			accel->learn_cnt[family == 6].nonport++;
			dev_put(br);
			return;
		}
		ether_addr_copy(smac, br->dev_addr);
		dev_put(br);
	} else {
		if (eport < 0 || eport >= ZX_NUM_PORTS)
			return;
		out = READ_ONCE(adapter->ports[eport]);
		/* 同上：直连出口端口不可用也要计数（P10.1） */
		if (!out || !netif_running(out)) {
			accel->learn_nonport++;
			accel->learn_cnt[family == 6].nonport++;
			return;
		}
		if (!zx_ffe_learn_mac_family(out, family, &nh, dmac)) {
			accel->learn_noneigh++;
			accel->learn_cnt[family == 6].noneigh++;
			return;
		}
		ether_addr_copy(smac, out->dev_addr);
	}

	/*
	 * #15 同口进出（P9）：用**解析后的**入口端口判，而不是钩子里的
	 * skb_iif。回程向的入口是 lan2、出口是 lan1，这里不会误判；反过来，
	 * 从同一个交换机口进出（L3 hairpin）的帧数据面 #15 也会回栈，学了也
	 * 用不上，直接放弃（与钩子快路径一样不计数）。
	 */
	if (eport == iiport)
		return;

	memset(&key, 0, sizeof(key));
	/*
	 * 键按族填（P10）：v4 填 key.v4[2]（union 的前 8 B），v6 填 key.v6[32]；
	 * 数据面 zx_ffe_v4_check()/zx_ffe_v6_check() 建键时填的是同一片字节
	 * （同为 memset 后赋值），键按字节比较，所以必须逐字段一致。
	 */
	if (family == 6) {
		memcpy(key.v6, &cand->saddr, sizeof(struct in6_addr));
		memcpy(key.v6 + sizeof(struct in6_addr), &cand->daddr,
		       sizeof(struct in6_addr));
	} else {
		key.v4[0] = cand->saddr.ip;
		key.v4[1] = cand->daddr.ip;
	}
	key.sport = cand->sport;
	key.dport = cand->dport;
	key.proto = cand->proto;
	key.family = family;
	/*
	 * ipport/vid 保持 0：数据面两族的判据链建键时都只填地址/sport/dport/
	 * proto/family（memset 之后），键按字节比较，填别的值这条永远不命中。
	 */

	mutex_lock(&accel->lock);
	/*
	 * 模板条目：先填好，再交给 __zx_ffe_store()。它要么把 flow 挂进表（新键），
	 * 要么把字段并进同 key 的旧条目并返回 1——两条路都只有一行，绝不会出现
	 * 同键两条。
	 */
	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow) {
		mutex_unlock(&accel->lock);
		return;
	}
	flow->key = key;
	ether_addr_copy(flow->dmac, dmac);
	/* smac 是 L2 出口的地址：直接口时是那个口，桥出口时是桥的 dev_addr */
	ether_addr_copy(flow->smac, smac);
	/* 条目里只存交换机端口号（0..4），桥/无线出口在上面已经被挡掉 */
	flow->egress_type = ZX_FFE_EGRESS_SWITCH;
	flow->egress_id = eport;
	flow->mtu = min_t(u32, out->mtu, 0xffff);
	flow->flags = ZX_FFE_FLOW_LEARNED |
		      (cand->nat & ZX_FFE_FLOW_NAT_MASK);
	/*
	 * NAT 输出字段是 __be32，只有 v4 有对应语义（v6 没有 NAT66，flags 里的
	 * NAT 位恒 0）；v6 条目保持 kzalloc 的 0，避免把 v6 地址的前 4 B 写进
	 * 一个只对 v4 有意义的字段。
	 */
	if (family == 4) {
		flow->saddr_out = cand->saddr_out.ip;
		flow->daddr_out = cand->daddr_out.ip;
		flow->sport_out = cand->sport_out;
		flow->dport_out = cand->dport_out;
	}
	flow->lastuse = jiffies;
	ret = __zx_ffe_store(adapter, flow);
	if (ret == 1) {
		/*
		 * 同 key 已存在：P10.2 F4 起是"新节点整体换上、旧节点
		 * kfree_rcu"，flow 已经属于表，**不要 free**。
		 */
		accel->learn_dup++;
		accel->learn_cnt[family == 6].dup++;
	} else if (ret == 0) {
		accel->learn_add++;
		accel->learn_cnt[family == 6].add++;
	} else {
		/* 表满（__zx_ffe_store 已计 tbl_full）或 rhashtable 失败：本拍不学 */
		kfree(flow);
	}
	mutex_unlock(&accel->lock);
}

/*
 * 出口设备 → 候选的出口坐标（P9 抽出来给"state->out + skb_dst(skb)->dev"
 * 两次判共用）。返回：
 *   0..ZX_NUM_PORTS-1   = 本驱动交换机口，返回值就是 eport；
 *   ZX_FFE_EPORT_BRIDGE = 桥出口，*br_ifindex 填桥的 ifindex；
 *   -1                  = 不是我们要的出口（这条不学）。
 *
 * 只做指针比较与读 ifindex：调用者若在 RCU 只读段里（dst 兜底那一路），
 * 出段之后只保留这个整数结果，不把 net_device 指针带出去。
 */
static int zx_ffe_learn_outport(struct zx_eth_adapter *adapter,
				struct net_device *out, u32 *br_ifindex)
{
	int i;

	if (!out)
		return -1;
	if (zx_eth_is_port(out)) {
		for (i = 0; i < ZX_NUM_PORTS; i++)
			if (READ_ONCE(adapter->ports[i]) == out)
				return i;
		return -1;
	}
	if (netif_is_bridge_master(out)) {
		/*
		 * 桥出口：钩子只记 ifindex、置哨兵，真正的端口号由 drain 里的
		 * FDB 反查得出（br_fdb_find_port() 要 rtnl，软中断里不能查）。
		 * CONFIG_BRIDGE=n 时这里不可能命中，属于死代码。
		 */
		*br_ifindex = out->ifindex;
		return ZX_FFE_EPORT_BRIDGE;
	}
	return -1;
}

/*
 * 这一帧是不是从"我们的桥端口"进来的？是就返回入口桥的 ifindex，否则 0。
 * 只在 skb_iif 快路径对不上 ports[] 时调（软中断；可能已经在 RCU 只读段里，
 * 这里再套一层 rcu_read_lock() 是允许的）。
 *
 * 为什么需要它：回程帧从 br-lan 的 lan2/lan3 进来，桥做本地投递，
 * br_pass_frame_up() 把 skb->dev 换成 brdev，__netif_receive_skb_core() 再把
 * skb_iif 改写成 brdev->ifindex；物理端口号在钩子里已经不可得了。
 *
 * 判据（两个来源任一命中即可，并且都要求该桥是 adapter->ports[] 里某个端口
 * 的 master——不是我们的桥就不学）：
 *   - in：POST_ROUTING 的入口设备。ip_output() 先 `indev = skb->dev`（此时
 *     是 brdev）再 `skb->dev = dst_dev`，然后把 indev 作为 NF_HOOK 的 in
 *     传进来（net/ipv4/ip_output.c:430-439），所以 state->in 才是入口设备；
 *   - skb_iif：brdev->ifindex（__netif_receive_skb_core 在 skb->dev 换成
 *     brdev 之后无条件写入）。即使某些路径没把入口设备通过 in 传进来，
 *     这个值也对得上桥。
 *
 * RCU：netdev_master_upper_dev_get_rcu() 要求 RCU 只读段，返回的 master 只在
 * 段内有效；这里只取 ifindex，不把指针带出去。
 */
static u32 zx_ffe_learn_inbridge(struct zx_eth_adapter *adapter,
				 struct sk_buff *skb, struct net_device *in)
{
	u32 br_ifindex = 0;
	int i;

	rcu_read_lock();
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *nd = READ_ONCE(adapter->ports[i]);
		struct net_device *master;

		if (!nd)
			continue;
		master = netdev_master_upper_dev_get_rcu(nd);
		if (!master)
			continue;
		if (master == in || master->ifindex == READ_ONCE(skb->skb_iif)) {
			br_ifindex = master->ifindex;
			break;
		}
	}
	rcu_read_unlock();
	return br_ifindex;
}

/*
 * 两个前缀判据（v6 学习解析器与 v6 判据链共用）。直接判字节而不 include
 * <net/ipv6.h>：判据在每包热路径上，两个 bit 测试就够，没必要为一个
 * ff00::/8 拉进整棵 IPv6 头文件树。
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
 * 钩子的 IPv4 报头解析（P5/P9 的全部过滤条件，逐条原样搬进来）：只做"这一帧
 * 能不能学 + 线上 saddr/daddr/sport/dport/proto 是什么"，不碰 skb、不分配。
 * 判据：
 *   #4 裸 IPv4、ihl == 5（带选项的帧数据面也不收）；
 *   #5 无分片（MF/OFFSET）；
 *   #6 只学 TCP/UDP；
 *   #9 IP 头与 L4 端口都在 skb_headlen() 里（钩子不做 pskb_may_pull）；
 *   #9' TTL <= 1 放弃（P10.1）。**比数据面严一档**：本钩子在 POST_ROUTING，
 *      转发帧的 TTL 已被 ip_forward() 递减，所以这里 `<= 1` 等价于数据面
 *      （递减前）的 `<= 2`；数据面 ttl=2 的帧会转发，学习侧却不学。只会
 *      少学（traceroute 那类短 TTL 流），不会学出数据面必拒的条目。
 * P9 的 GSO 放开不需要额外代码：这里没有任何 skb_is_gso() 判据，GSO skb 的
 * IP 头与 L4 头同样合法（同一五元组才会被 GRO 合并）。
 */
static bool zx_ffe_learn_parse4(struct sk_buff *skb,
				struct zx_ffe_learn_pkt *pkt)
{
	const struct iphdr *iph;
	__be16 ports[2];

	if (skb->protocol != htons(ETH_P_IP))
		return false;
	if (skb_headlen(skb) < sizeof(*iph))
		return false;
	iph = ip_hdr(skb);
	if (iph->version != 4 || iph->ihl != 5)
		return false;
	if (iph->frag_off & htons(IP_MF | IP_OFFSET))
		return false;
	if (iph->protocol != IPPROTO_TCP && iph->protocol != IPPROTO_UDP)
		return false;
	/*
	 * #9' 与数据面 #9 **不是同口径，是严一档**（P10.1，见函数头）：
	 * POST_ROUTING 看到的是已递减的 TTL，数据面 #9 判的是递减前的值。
	 */
	if (iph->ttl <= 1)
		return false;
	if (skb_headlen(skb) < iph->ihl * 4 + (unsigned int)sizeof(ports))
		return false;
	memcpy(ports, (const u8 *)iph + iph->ihl * 4, sizeof(ports));

	memset(pkt, 0, sizeof(*pkt));
	pkt->saddr.ip = iph->saddr;
	pkt->daddr.ip = iph->daddr;
	pkt->sport = ports[0];
	pkt->dport = ports[1];
	pkt->proto = iph->protocol;
	pkt->family = 4;
	return true;
}

/*
 * 钩子的 IPv6 报头解析（P10；P10.1 加固）。取法与 v4 版同构，但 v6 有这些
 * 特有的边界，全部与数据面 zx_ffe_v6_check() 对齐——**学出来的条目必须是数据面
 * 真会命中的键**，否则只是占一行、等老化：
 *
 *   1. **线性区从 skb_network_header 起算**（P10.1）：skb_headlen() 的起点是
 *      skb->data，本钩子里二者目前相等（POST_ROUTING），但"data 起有 44 B"并
 *      不能证明"ipv6_hdr 起有 44 B"。offset 是 int（data 已 pull 过 IPv6 头时
 *      为负），先转 unsigned 再比：offset > headlen（含负数）一律拒。
 *   2. **payload_len < 4 放弃**（P10.1）：端口都不全，学出来的键永不命中
 *      （数据面 #6 的严格长度等式本来也会把这些帧全挡掉）。
 *   3. **扩展头一律放弃**。逐跳(0)/路由(43)/分片(44)/目的选项(60) 与数据面
 *      判据 #7 同一个黑名单（AH/ESP 等其余 next header 在下面按"非 TCP/UDP"
 *      拒掉，与数据面 #12 一致）。不猜扩展头长度，就不用把扩展头当 L4 端口
 *      读；这正是"v6 快路只吃纯 TCP/UDP 帧"的落地方式。
 *   4. **hop_limit <= 1 放弃**（P10.1）：**比数据面 #8 严一档**——钩子在
 *      POST_ROUTING，转发帧的 hop_limit 已由 ip6_forward() 递减，这里
 *      `<= 1` 对应数据面（递减前）的 `<= 2`。数据面 hlim=2 的帧会转发，
 *      学习侧不学；只会少学，不会学出数据面必拒的条目。
 *   5. **ICMPv6(58) 排除**：NS/NA/RS/RA/MLD/echo 全在"非 TCP/UDP"这条里，
 *      与数据面 #12 同一刀。
 *   6. **fe80::/10 源排除**：数据面 #11 不收链路本地源（避免旁路 ND 状态机），
 *      学了也永远命中不了。目的组播 ff00::/8（数据面 #9）同样排除。
 *
 * 不查 "payload_len + 40 == 帧长 - 14" 的严格等式：数据面在**每一帧**上都会
 * 查（它决定这一帧能不能走快路），而学习只需要键；GSO 超级包的 payload_len
 * 覆盖的是聚合长度，等式本来就不该套在"学不学"上（P9 已为 v4 放开 GSO）。
 * payload_len >= 4 是"端口在不在报头里"的必要条件，对 GSO 也成立。
 */
static bool zx_ffe_learn_parse6(struct sk_buff *skb,
				struct zx_ffe_learn_pkt *pkt)
{
	const struct ipv6hdr *ip6;
	__be16 ports[2];
	unsigned int hlen;

	if (skb->protocol != htons(ETH_P_IPV6))
		return false;
	/*
	 * 1. 线性区以 skb_network_header 为界（P10.1）：skb_network_offset() 是
	 * int，data 已经 pull 过 IPv6 头时为负、报头越出线性区时为正；先转
	 * unsigned，两种异常（负数变成巨大值、正数大于 headlen）都在比较里被拒。
	 */
	if (skb_headlen(skb) < (unsigned int)skb_network_offset(skb))
		return false;
	hlen = skb_headlen(skb) - (unsigned int)skb_network_offset(skb);
	if (hlen < sizeof(*ip6))
		return false;
	ip6 = ipv6_hdr(skb);
	if (ip6->version != 6)
		return false;
	/* 2. 端口 4 B 必须在 payload 里（P10.1，见函数头） */
	if (ntohs(ip6->payload_len) < 4)
		return false;
	/* 3. 扩展头：与数据面 #7 同一个黑名单，遇到就放弃 */
	switch (ip6->nexthdr) {
	case IPPROTO_HOPOPTS:
	case IPPROTO_ROUTING:
	case IPPROTO_FRAGMENT:
	case IPPROTO_DSTOPTS:
		return false;
	}
	/* 4. hop_limit<=1：比数据面 #8 严一档（POST_ROUTING 已递减，见函数头） */
	if (ip6->hop_limit <= 1)
		return false;
	/* 5. 只学 TCP/UDP（ICMPv6 = 58 在这里被排除） */
	if (ip6->nexthdr != IPPROTO_TCP && ip6->nexthdr != IPPROTO_UDP)
		return false;
	if (hlen < sizeof(*ip6) + (unsigned int)sizeof(ports))
		return false;
	/* 6. 目的组播 / 源链路本地：与数据面 #9/#11 同一条线 */
	if (zx_ffe_v6_is_mcast(&ip6->daddr) ||
	    zx_ffe_v6_is_linklocal(&ip6->saddr))
		return false;
	memcpy(ports, (const u8 *)ip6 + sizeof(*ip6), sizeof(ports));

	memset(pkt, 0, sizeof(*pkt));
	memcpy(&pkt->saddr.in6, &ip6->saddr, sizeof(ip6->saddr));
	memcpy(&pkt->daddr.in6, &ip6->daddr, sizeof(ip6->daddr));
	pkt->sport = ports[0];
	pkt->dport = ports[1];
	pkt->proto = ip6->nexthdr;
	pkt->family = 6;
	return true;
}

/*
 * 学习钩子（P5-TASK §1.1/§1.2；P10 起**两个协议族共用**）：NFPROTO_IPV4 与
 * NFPROTO_IPV6 各注册一次，hooknum = POST_ROUTING；优先级 v4 用
 * NF_IP_PRI_LAST、v6 用 NF_IP6_PRI_LAST（P10.1 起：数值同为 INT_MAX，正好
 * 各自是本族 conntrack confirm 用的常量），
 * 也就是 NAT（nf_nat 的 POST_ROUTING 在 NF_IP_PRI_NAT_SRC=100）之后，所以
 * `ip_hdr()/ipv6_hdr()` 里已经是 NAT 后的地址；pre-NAT 的元组只能从 conntrack
 * 取。两个 pf 走**同一个回调**，按 state->pf 选报头解析器（parse4/parse6），
 * 之后的入口/出口解析、同口保护、元组选择、候选组装、投递只有一份——两族
 * 判据不会漂移，这也是"优先共用"的落法。
 *
 * 语义：**只旁听，不做任何决定**，恒返回 NF_ACCEPT。过滤不通过就直接返回，
 * 投递只做 memcpy + 环写，所有分配/邻居查询都在 work 里。
 *
 * ctinfo 方向（P5 修订二 §A 改成两侧都学）：conntrack 给两个方向的 tuple，与
 * 包的方向无关，方向只看 ctinfo 落在枚举的哪一侧：CTINFO2DIR(ctinfo) 判的是
 * `ctinfo >= IP_CT_IS_REPLY`（定义见
 * include/uapi/linux/netfilter/nf_conntrack_tuple_common.h:44；**不是位与**，
 * 理由见本函数 #11 的注释）：
 *
 *   ORIGINAL 侧：键 = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple（pre-NAT），
 *               输出 = ip_hdr(skb) + L4 端口（post-NAT）。这就是 P5 的行为。
 *   REPLY 侧：   键 = ct->tuplehash[IP_CT_DIR_REPLY].tuple，
 *               输出 = ip_hdr(skb) + L4 端口。
 *
 * **两侧存的都是"该方向报文在链路上的 pre-NAT 形态"**，不是"一侧 pre、一侧
 * post"，所以上面这条规则对两个方向同构。依据（linux-6.18.55 源码，P5 修订二
 * 逐行核对过）：
 *   - 建连：net/netfilter/nf_conntrack_core.c:1735 init_conntrack() 里
 *     nf_ct_invert_tuple(&repl_tuple, tuple)，invert 只交换源/目的与端口（同一
 *     文件 :432 nf_ct_invert_tuple()），于是 tuple[REPLY] 一开始就是"回包到达
 *     时的样子"；
 *   - 原始方向的 tuple 全内核只在 nf_conntrack_core.c:1651
 *     __nf_conntrack_alloc() 里写过一次（取首包的 pre-NAT 元组），NAT 之后再没人
 *     碰它 → tuple[ORIGINAL] 永远是 pre-NAT；
 *   - NAT：net/netfilter/nf_nat_core.c:771 nf_nat_setup_info() 先
 *     :793 curr_tuple = invert(tuple[REPLY]) —— 注意这恰好回到"原始方向 pre-NAT"
 *     的坐标系 —— 由 get_unique_tuple() 挑新值，再 :802 reply = invert(new_tuple)
 *     交给 :803 nf_conntrack_alter_reply() 写回 tuple[REPLY]
 *     （include/net/netfilter/nf_conntrack.h:284 里就是
 *     `tuplehash[IP_CT_DIR_REPLY].tuple = *newreply`）。invert(原始方向 NAT 之后
 *     的值) 正是"回程包在链路上的 pre-NAT 形态"：例如 masquerade 的下载方向
 *     tuple[REPLY] = (服务器:53 → WAN_IP:随机端口)，与 WAN 口收到的帧头逐字段
 *     相同；NAT 之后才变成 (服务器:53 → 客户端:1234)，也就是 ip_hdr(skb)。
 *   - 反证：net/netfilter/nf_nat_proto.c:376/383 nf_nat_manip_pkt() 用
 *     invert(tuple[!dir]) 作**改写目标**（"We are aiming to look like inverse of
 *     other direction"），说明报文自身在改写前持有的就是 pre-NAT 值——与本钩子
 *     在 NAT 之后读到的 post-NAT 正好互补。
 *
 * 因此两个方向都能拼出 (键, 输出) 一对；NAT 位照旧按"键 vs 输出"的差异置位，
 * 不需要任何方向特判。下载方向（WAN→LAN 的 bulk）就是 REPLY 侧，P5 只学
 * ORIGINAL 等于只加速了上行小包，这是本次修订要修的第一件事。
 *
 * 为什么在钩子里能看到 confirmed：本驱动是 builtin（probe 早于 conntrack 模块
 * 加载），而 nf_hook_entries_grow() 对同优先级是"后注册的排前面"，ipv4_confirm
 * 也是 INT_MAX 且注册更晚，所以它先跑、这里的 IPS_CONFIRMED 已经置位。
 *
 * P9 入口反查（回程方向学不到的第一个阻塞）：回程帧（WAN→LAN 的 bulk、LAN
 * 主机的回包）从 br-lan 的 lan2/lan3 进来，桥做本地投递，br_pass_frame_up()
 * 把 skb->dev 换成 brdev，__netif_receive_skb_core() 再把 skb_iif 改写成
 * brdev->ifindex，于是 "skb_iif 对上 ports[]" 永远不成立、候选被静默丢弃
 * （连计数都不涨，P7 上机看到的就是 learn_cand 只 +连接数）。现在不命中时再判
 * 一次"是不是从我们的桥端口进来的"（zx_ffe_learn_inbridge()），是就记**源
 * MAC + 桥 ifindex**，物理端口留给 drain 里的 br_fdb_find_port() 反查。
 *
 * 注意：POST_ROUTING 的入口设备只能从 state->in 拿。ip_output()
 * （net/ipv4/ip_output.c:428-444）先把 `skb->dev` 改成**出口**设备、再把原来的
 * skb->dev 作为 NF_HOOK 的 in 传进来，所以在这个钩子里 skb->dev 是 lan1，不是
 * br-lan（别按直觉写 skb->dev）。
 *
 * P9 GSO（第二个阻塞）：GRO 合并后的 bulk 回程是 GSO skb（实测 7.6–18.5 KB），
 * 旧代码在 #3 一律 return。键只取 IP 头与 L4 头，GRO 合并 skb 的这两层头逐字段
 * 合法；现在的允许条件就是 #5/#6/#9（无分片、TCP/UDP、头在线性区），见 #6'。
 *
 * 同口保护（数据面 #15）改用**解析后的**入口端口判：钩子快路径能当场判
 * （skb_iif 对得上 ports[]），桥入口那一路只能等 drain 反查完再判——回程向
 * 入口 lan2、出口 lan1，不会误判成"进=出"。
 *
 * 覆盖范围（WiFi 预留，别踩）：
 *   - 本设计只加速"**从交换机端口进、从交换机端口出**"的路由帧。
 *   - **WiFi 客户端的上行流量入口是 mac80211，不经过本驱动的 RX 环**，所以
 *     本路径天然覆盖不到；要加速得另做"发送侧快路"（skb → 池 → 描述符的
 *     直提交通道），未来的新加速模式（例如 ZX_ACC_FFE_TX）应该挂在那里。
 *   - 桥 → 无线口（phy0-ap0 / phy1-ap0 等）的帧必须回栈：mac80211 的队列/
 *     聚合/功率控制不能绕过。所以 FDB 命中的端口不是本驱动交换机口时，候选
 *     直接丢弃（learn_nonport++），绝不把无线 MAC 当交换机端口写描述符。
 */
static unsigned int zx_ffe_learn_hook(void *priv, struct sk_buff *skb,
				      const struct nf_hook_state *state)
{
	struct zx_eth_adapter *adapter = priv;
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_learn_pkt pkt;
	struct zx_ffe_learn_ring *ring;
	struct zx_ffe_learn_cand cand;
	const struct nf_conntrack_tuple *ot;
	enum ip_conntrack_dir dir;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;
	u32 br_ifindex = 0;
	u32 in_br_ifindex = 0;
	u32 head;
	int eport = -1;
	int iiport = -1;
	int i;
	u8 src_mac[ETH_ALEN];

	/* 开关关掉时第一行就返回：零副作用（P5-TASK §1.8） */
	if (!READ_ONCE(accel->learn_on))
		return NF_ACCEPT;
	/*
	 * P10.2 复审整改 1：**学习只为喂 FFE**，该族不在 ffe 模式就直接返回。
	 *
	 * 交付形态是 `accel_v4=hw` + `accel_v6=ffe` + `ffe_learn=1`。若这里只按
	 * learn_on 放行，IPv4 的 POST_ROUTING 钩子也会学，把 v4 条目写进与 v6
	 * **共用**的 1024 条流表；v4 学满后 v6 学习会 tbl_full，而 v6 才是交付
	 * 卖点。所以按族判"该族当前模式 == ffe"：不是就直接 return，
	 * **不计数、不入环**（learn_cand 的口径仍是"钩子看到并成功投递"，
	 * B2 的 `v6=ffe + learn=1 ⇒ Δcand = 2×连接数` 不变）。
	 *
	 * 用 zx_accel_cur_mode()（内部 READ_ONCE）而不是裸读字段：debugfs 切
	 * 模式与钩子并发，读到旧值最多多学/少学一拍，下一拍自愈。
	 */
	if (state->pf == NFPROTO_IPV4 &&
	    zx_accel_cur_mode(accel, 0) != ZX_ACC_FFE)
		return NF_ACCEPT;
	if (state->pf == NFPROTO_IPV6 &&
	    zx_accel_cur_mode(accel, 1) != ZX_ACC_FFE)
		return NF_ACCEPT;
	/*
	 * #1 按注册的协议族选报头解析器（P10）。钩子注册了两次（IPv4/IPv6），
	 * state->pf 就是注册时的 pf；两族只在这里分叉，之后的 #7 出口、#8
	 * 入口、#10 本机目的、#11 conntrack、组装、投递**共用同一份代码**。
	 */
	if (state->pf == NFPROTO_IPV4) {
		if (!zx_ffe_learn_parse4(skb, &pkt))
			return NF_ACCEPT;
	} else if (state->pf == NFPROTO_IPV6) {
		if (!zx_ffe_learn_parse6(skb, &pkt))
			return NF_ACCEPT;
	} else {
		return NF_ACCEPT;
	}
	/* #2 转发帧的目的 MAC 是本机，pkt_type 必须是 PACKET_HOST */
	if (skb->pkt_type != PACKET_HOST)
		return NF_ACCEPT;
	/*
	 * #3 GSO（P9 放开，P10 起 v4/v6 同一口径）：旧代码在这里就
	 * `if (skb_is_gso(skb)) return`，把 GRO 合并后的 bulk（v4 实测
	 * 7.6–18.5 KB）整片挡住，于是"只有握手小包那些流"能学、"整条流的 bulk"
	 * 永远学不到。键只取 IP 头与 L4 头，GRO 合并 skb 的这两层头是合法的
	 * （同一 5 元组才会被合并），不该一刀切。真正的边界收在 parse4() /
	 * parse6() 里：TCP/UDP、无分片（v6 还要求无扩展头）、头在线性区。
	 *
	 * 数据面完全不受影响：它只吃 <=1514 的帧（#17），GSO 大包永远走栈；
	 * 学到的条目是给这条流**后续的单帧**用的键——bulk 在驱动 RX 侧是一个个
	 * 单帧到达的，那时 FFE 已经能命中。
	 */

	/*
	 * #7 出口。先看 state->out：
	 *   本驱动交换机口           → 直接得到 eport；
	 *   桥（br-lan，WAN→LAN 常态）→ 只记桥 ifindex + 哨兵，FDB 反查留到
	 *                               drain 里做（br_fdb_find_port() 要 rtnl，
	 *                               软中断里拿不到）；
	 *   其它（虚拟口/空）         → P9 兜底：POST_ROUTING 时路由已定，
	 *                               skb_dst(skb)->dev 才是真正要发出去的设备，
	 *                               再判一次（是交换机口/桥才要）。
	 * 出口解析不出来就整条不学——绝不猜。
	 *
	 * ports[] 只在 probe 里写一次，READ_ONCE 足够。
	 */
	eport = zx_ffe_learn_outport(adapter, state->out, &br_ifindex);
	if (eport < 0) {
		struct net_device *dst_dev = NULL;

		/*
		 * skb_dst() 可能带 SKB_DST_NOREF（转发帧的 dst 是
		 * skb_dst_set_noref() 挂上的），读它要求 RCU；这里在锁内只做
		 * 指针比较/取 ifindex，出锁后不再碰 dst_dev（设备随时可能被
		 * 注销，指针不能带出 RCU 只读段）。
		 */
		rcu_read_lock();
		if (skb_dst(skb))
			dst_dev = skb_dst(skb)->dev;
		eport = zx_ffe_learn_outport(adapter, dst_dev, &br_ifindex);
		rcu_read_unlock();
		if (eport < 0)
			return NF_ACCEPT;
	}
	/*
	 * #8 入口。快路径：skb->skb_iif 是收包口的 ifindex，对得上 ports[] 就
	 * 直接得到 iiport（WAN 口 lan1 不在桥里，进来的帧走这条）。
	 *
	 * 慢路径（P9）：回程帧从桥端口进来时 skb_iif 已经被改写成桥的 ifindex
	 * （见函数头），这里再判"是不是从我们的桥端口进来的"。是就记源 MAC +
	 * 桥 ifindex，真正的端口号交给 drain 的 FDB 反查（桥把源 MAC 写进 FDB
	 * 是在 br_pass_frame_up() **之前** 做的，见 br_handle_frame_finish()，
	 * 所以钩子看到这一帧时 FDB 里已经有它了）。不是我们的桥端口就不学。
	 *
	 * #8' 同口进出不学（数据面判据 #15 会回栈）：快路径能当场判；慢路径的
	 * 入口端口在 drain 里才知道，那条判定搬到了 zx_ffe_learn_one()。
	 */
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *nd = READ_ONCE(adapter->ports[i]);

		if (nd && nd->ifindex == skb->skb_iif)
			iiport = i;
	}
	eth_zero_addr(src_mac);
	if (iiport < 0) {
		in_br_ifindex = zx_ffe_learn_inbridge(adapter, skb, state->in);
		/*
		 * skb_mac_header_was_set() 只是防御：这条路走到的帧都是从交换机
		 * 端口收上来的，eth_type_trans/br_handle_frame 已经在同一个 skb
		 * 上读过 eth_hdr(skb)->h_source。
		 */
		if (!in_br_ifindex || !skb_mac_header_was_set(skb))
			return NF_ACCEPT;
		ether_addr_copy(src_mac, eth_hdr(skb)->h_source);
	} else if (eport == iiport) {
		return NF_ACCEPT;
	}
	/*
	 * #10 目的地址属本机（看的是 **post-NAT** 的 daddr，即这一帧真正要去的
	 * 地址）：本机终结的流学了也没用。注意端口映射 / NAT 回程的帧，pre-NAT
	 * 的 daddr 才是本机地址（WAN IP），post-NAT 的 daddr 是内网主机——所以
	 * 这条判据不会把该加速的 NAT 流挡掉；数据面那边由判据 #13' 的延迟判定
	 * 放行（见 zx_ffe_forward()）。
	 *
	 * v6（P10）与 v4 逐条对应：集合换成本机 IPv6 地址表（数据面 v6 判据
	 * #10 用的同一个 local_ip6s），v6 没有 NAT66，所以这里就是"终点是本机"。
	 */
	if (pkt.family == 6) {
		if (zx_ffe_is_local_ip6(accel, &pkt.daddr.in6))
			return NF_ACCEPT;
	} else if (zx_ffe_is_local_ip(accel, pkt.daddr.ip)) {
		return NF_ACCEPT;
	}

	/*
	 * #11 conntrack：**两个方向都学**（P5 修订二 §A，依据见函数头注释）。
	 * 方向只看 ctinfo 的方向位，元组按方向挑：ORIGINAL 侧取 tuple[ORIGINAL]、
	 * REPLY 侧取 tuple[REPLY]，两侧存着的都是"该方向报文在链路上的 pre-NAT
	 * 形态"，正是数据面从帧头拼出来的键。v4/v6 的元组结构同构（union
	 * nf_inet_addr + 同一偏移的 L4 端口），所以只有 l3num 与地址宽度按族取。
	 *
	 * 未跟踪/没跟踪的包在 6.18 里是 nf_ct_set(skb, NULL, IP_CT_UNTRACKED)
	 * （net/netfilter/nf_conntrack_core.c:1981 就是这么判的），所以 ct 为
	 * NULL；下面那条 ctinfo 判据纯属防御（枚举本身是给用户态兼容留的）。
	 * 只学已 confirm 的连接：首包在 confirm 之前，本来也不该学。
	 *
	 * 方向判据必须用内核的 CTINFO2DIR()（定义见
	 * include/uapi/linux/netfilter/nf_conntrack_tuple_common.h:44，经
	 * include/net/netfilter/nf_conntrack_tuple.h 引入）——它比的是 **>=**，
	 * 不是位与：IP_CT_ESTABLISHED=0 / RELATED=1 / NEW=2 / IS_REPLY=3，且
	 * *_REPLY = 基值 + 3。P7 上机前这里写成 `ctinfo & IP_CT_IS_REPLY`，于是
	 * NEW(2)、RELATED(1) 被当成回程方向、RELATED_REPLY(4) 被当成原始方向：
	 * 新 UDP 流的首包（NEW）学出来的是"键 = tuple[REPLY]、输出 = 原始方向
	 * 的报头"，回程帧命中这条错条目后被 #15 同口保护打回栈，回程条目 hits
	 * 恒 0（-R 反向只有 58.9 Mbit/s）。
	 */
	ct = nf_ct_get(skb, &ctinfo);
	if (!ct || ctinfo == IP_CT_UNTRACKED)
		return NF_ACCEPT;
	if (!nf_ct_is_confirmed(ct) || nf_ct_is_dying(ct))
		return NF_ACCEPT;
	dir = CTINFO2DIR(ctinfo);
	ot = &ct->tuplehash[dir].tuple;
	if (ot->src.l3num != (pkt.family == 6 ? AF_INET6 : AF_INET) ||
	    ot->dst.protonum != pkt.proto)
		return NF_ACCEPT;

	/*
	 * 组装候选。先写本地变量、再整体 memcpy 进环：去重要拿整条候选做键，
	 * 而且"槽位发布"必须排在所有字段写完之后（smp_store_release），本地
	 * 副本让这两件事都简单。
	 *
	 * 整结构先 memset 再逐字段赋值：P9 加了 iiport/src_mac/in_br_ifindex、
	 * P10 把地址加宽到 16 B 之后，结构里有隐式填充与"本族用不到的字节"，
	 * 而去重是**整结构 memcmp**，读未初始化字节会让同一条流的两次候选比较
	 * 结果不确定。结构是 POD：memset + 赋值，不分配。
	 *
	 * 地址按族只拷 4/16 字节（zx_ffe_learn_addr_cpy）：union nf_inet_addr 是
	 * 16 B，v4 只用前 4 B；拷满 16 B 会把 v4 元组/报头 union 里的垃圾字节
	 * 带进候选，去重就失效了。
	 *
	 * 键（cand.saddr/daddr/sport/dport）来自上面按方向挑好的元组，输出
	 * （*_out）永远是当前报头：两个方向、两个族用的是同一条规则，没有方向
	 * 特判。入口坐标（iiport + 桥 ifindex + 源 MAC，P9）只用于同口保护，
	 * 不进键。
	 */
	memset(&cand, 0, sizeof(cand));
	zx_ffe_learn_addr_cpy(&cand.saddr, &ot->src.u3, pkt.family);
	zx_ffe_learn_addr_cpy(&cand.daddr, &ot->dst.u3, pkt.family);
	/* TCP/UDP 时 union 的第一个成员 all 就是端口（两族同一偏移） */
	cand.sport = ot->src.u.all;
	cand.dport = ot->dst.u.all;
	zx_ffe_learn_addr_cpy(&cand.saddr_out, &pkt.saddr, pkt.family);
	zx_ffe_learn_addr_cpy(&cand.daddr_out, &pkt.daddr, pkt.family);
	cand.sport_out = pkt.sport;
	cand.dport_out = pkt.dport;
	cand.proto = pkt.proto;
	cand.family = pkt.family;
	cand.eport = eport;
	cand.br_ifindex = br_ifindex;
	cand.iiport = (iiport < 0) ? ZX_FFE_IPORT_BRIDGE : (u8)iiport;
	cand.in_br_ifindex = in_br_ifindex;
	ether_addr_copy(cand.src_mac, src_mac);
	cand.nat = 0;
	/*
	 * NAT 位：键与输出不一致 = 这一维被 NAT 改写过（纯路由流两位都不置）。
	 * 注意对 REPLY 侧来说，"目的地址变了"就是 masquerade 的反向改写
	 * （键里的 daddr 是 WAN IP、输出是内网主机），数据面要靠 NAT_D 这位
	 * 才能放行，见 zx_ffe_forward() 的 #13'。
	 *
	 * v6（P10）没有 NAT66：tuple 与报头逐字段相同，这两位自然为 0；真出现
	 * 不一致（只可能是内核加了 v6 NAT），条目会带 NAT 位而 flow->*_out 保持
	 * 0，数据面判据 #16 会把它回栈——不会改写错包。
	 */
	if (!zx_ffe_learn_addr_eq(&cand.saddr, &cand.saddr_out, pkt.family) ||
	    cand.sport != cand.sport_out)
		cand.nat |= ZX_FFE_FLOW_NAT_S;
	if (!zx_ffe_learn_addr_eq(&cand.daddr, &cand.daddr_out, pkt.family) ||
	    cand.dport != cand.dport_out)
		cand.nat |= ZX_FFE_FLOW_NAT_D;

	/*
	 * v6 + NAT 位直接丢弃（P10.1，复审 B1）：v6 没有 NAT66，数据面 #16 对
	 * "family=6 且带 NAT 位"的条目一律回栈（flow->*_out 在落表侧保持 0），
	 * 学进去只是白占一个表项到老化。真出现 NAT 位只可能是环境挂了 v6 NAT
	 * （nat66/masq6）；宁可这一拍不学，也不去猜 v6 的 NAT 改写语义。
	 * 与环满/去重一样是"主动放弃"，还没进环，所以不算进 cand。
	 */
	if (pkt.family == 6 && cand.nat) {
		accel->learn_cnt[1].nat6++;
		return NF_ACCEPT;
	}

	/*
	 * 投递：本 CPU 的环。get_cpu_ptr() 关抢占，保证整个"填槽位 + 发布 head"
	 * 不会被迁移打断；环满（head - tail == 容量）就丢候选，绝不等待。
	 */
	ring = get_cpu_ptr(accel->learn_rings);
	head = READ_ONCE(ring->head);
	if (head - READ_ONCE(ring->tail) >= ZX_FFE_LEARN_RING) {
		accel->learn_drop++;
		accel->learn_cnt[pkt.family == 6].drop++;
		put_cpu_ptr(accel->learn_rings);
		return NF_ACCEPT;
	}
	/*
	 * 去重：同一五元组 1 s 内只投一次。放在容量检查之后——去重位只记
	 * "已经真的进了环"的候选，环满丢掉的那条不会被记住，环一有空位就能
	 * 再投。
	 */
	if (zx_ffe_learn_dup(ring, &cand)) {
		accel->learn_dedup++;
		accel->learn_cnt[pkt.family == 6].dedup++;
		put_cpu_ptr(accel->learn_rings);
		return NF_ACCEPT;
	}
	memcpy(&ring->cand[head & (ZX_FFE_LEARN_RING - 1)], &cand, sizeof(cand));
	smp_store_release(&ring->head, head + 1);
	/* 裸 ++：并发下可能丢几个计数，不影响语义（同 cnt[] 的口径） */
	accel->learn_cand++;
	accel->learn_cnt[pkt.family == 6].cand++;
	put_cpu_ptr(accel->learn_rings);
	return NF_ACCEPT;
}

/*
 * 排空每 CPU 的环（age work 的 1 s 节拍里调，进程上下文）。
 * 一次最多处理一个环的容量：环是"尽力而为"的，剩下的下一拍继续，绝不让
 * 单个 work 占着 CPU 太久。
 *
 * 开关关掉时只把环清空、不落表：否则 `echo 0 > ffe_learn` 之后环里剩的
 * 候选还会在下一拍进表（P5-TASK §1.8 "新流不再进表"）。
 */
static void zx_ffe_learn_drain(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_learn_ring *ring;
	int cpu;

	for_each_possible_cpu(cpu) {
		u32 head, tail, n = 0;

		ring = per_cpu_ptr(accel->learn_rings, cpu);
		head = smp_load_acquire(&ring->head);
		tail = READ_ONCE(ring->tail);
		if (!READ_ONCE(accel->learn_on)) {
			WRITE_ONCE(ring->tail, head);
			continue;
		}
		while (tail != head && n++ < ZX_FFE_LEARN_RING) {
			struct zx_ffe_learn_cand cand;

			/*
			 * 先复制再推进 tail：处理期间生产侧仍在写同一个环，
			 * 槽位随时可能被覆盖；候选允许丢，但不许读半个。
			 */
			memcpy(&cand, &ring->cand[tail & (ZX_FFE_LEARN_RING - 1)],
			       sizeof(cand));
			WRITE_ONCE(ring->tail, ++tail);
			zx_ffe_learn_one(adapter, &cand);
		}
	}
}

/*
 * 判据链的裁决（enum zx_ffe_verdict）从 P6 起在 zx279128s-eth.h 里：它是
 * ingress op 的返回类型，WiFi 接入时也要看得见。计数仍然集中在
 * zx_ffe_ingress_switch_complete() 的出口做一次——两族的口径只有一份。
 */

/*
 * 长度判据（P10.2 F1，v4/v6 共用同一份，别复制两份）：
 *   avail   = 线缆上的 L3 字节数（含 MAC padding）= len - ETH_HLEN；
 *   claimed = 报头声明的 IP 层长度（v4 = tot_len，v6 = 40 + payload_len）。
 *
 * 允许 padding、拒绝真截断与槽位残留：
 *   claimed > avail              → 真截断，回栈（计 len_short）；
 *   avail - claimed > MAX_PAD    → 尾部不是本帧的 padding，回栈（计 len_long）；
 *   avail >= claimed 且差 <= 6   → 只可能是 MAC 补的最小帧 padding，放行。
 * 容差**只允许 padding 方向**，所以"声称比实到还长"这件事永远不会被放过。
 *
 * 前置不变式（P10.2 复审整改 6）：调用者只会在 runt 判据之后调用
 * （zx_ffe_ingress_switch_run() 的 `if (len < (family ? 64 : 60))`，当前在
 * accel.c:3052），所以 `avail = len - ETH_HLEN >= 46`，是无符号安全值；
 * 本函数里先判 `claimed > avail` 再减，`avail - claimed` 也不会下溢。
 * **若将来给 zx_ffe_v4_check()/v6_check() 加别的调用点**（例如不经 runt 的
 * 直调），调用者必须自己保证 `len >= ETH_HLEN`，否则 avail 会回绕成
 * 巨大值、这条判据就失效。
 *
 * 为什么 6 B 的差不可能把 VLAN 外层误吞成 padding（已逐处确认）：
 *   1. 进 parse 的帧，offset 12 就已经是 0x0800/0x86dd：
 *      zx_ffe_ingress_switch_run() 在 frame+12 读 h_proto（A:2776），
 *      zx_ffe_v4_check() 第一句核对 ETH_P_IP（A:2186）、v6 核对
 *      ETH_P_IPV6（A:2265）。VLAN 帧的 h_proto 是 0x8100/0x88a8，在
 *      这两句就 MISS，根本走不到长度判据。
 *   2. RX 路径**没有**"剥了 VLAN 再交上来"的软件路径：zx_ffe_rx() 拿到的
 *      是 BMU 池里的原始帧，main.c 的 RX 段没有任何 vlan_untag/skb_vlan
 *      处理；交换机驱动的 VLAN 配置只服务 br 交换与 egress untag
 *      （zx279128s-switch.c:131 ZX_SBRG_VLAN_UNTAG），不碰送 CPU 的帧。
 *   3. 反过来说，如果哪天硬件在送 CPU 前剥 tag 而 len 仍含 4 B tag，
 *      `len - ETH_HLEN` 的口径就错了——那时这条判据要跟着重核，不能
 *      靠把 6 改大来圆。今天的代码与抓包都没有这种路径的证据。
 *
 * 注：v4 UDP 的最小 IP 包是 28 B，MAC padding 可到 18 B；本判据对它是
 * 保守的（miss 回栈，不误发）。这条快路的目标帧（v6/TCP）6 B 是精确上界。
 */
static bool zx_ffe_len_ok(struct zx_accel *accel, int family, u32 avail,
			  u32 claimed)
{
	struct zx_ffe_cnt *c = &accel->cnt[family];

	if (claimed > avail) {
		c->len_short++;
		return false;
	}
	if (avail - claimed > ZX_FFE_MAX_PAD) {
		c->len_long++;
		return false;
	}
	return true;
}

/*
 * IPv4 判据链（设计 §2.2 + 勘误 §1/§2）。通过时填好 key 与 *pkt_len（IPv4 的
 * tot_len，即 IP 层长度），返回 ZX_FFE_V_OK。入口口/MAC/runt 三条两族共用的
 * 判据在 zx_ffe_rx() 的前置段里做过，这里不再重复。
 *
 * #13（目的 IP 属本机）从 P5 修订二起**只算不判**（P5 修订二 §B）：端口映射与
 * NAT 回程的 pre-NAT daddr 就是盒子自己的 WAN IP，在这一步直接 drop_local 会把
 * 所有"命中带 DNAT 的学习条目"挡在快路外面。所以这里只把布尔值交给调用方
 * （*daddr_local），真正的裁决在查表之后（zx_ffe_forward() 的 #13'）：
 *   查表未命中          → miss（本来也要回栈）
 *   命中且带 NAT_D      → 放行（改写把 daddr 换成 daddr_out）
 *   命中但不带 NAT_D    → drop_local（这帧确实是给本机终结的）
 *
 * 唯一的例外是**非 TCP/UDP** 的帧：它们在 #8 就返回了，根本走不到查表，所以
 * #8 里就地按 daddr_local 报 DROP_LOCAL——否则 ping 本机地址（ICMP）会被算成
 * 普通 miss，P1-TASK §5.3 的验收判据（drop_local 上涨）不成立
 * （design-errata §2 记的就是这条）。
 */
static enum zx_ffe_verdict
zx_ffe_v4_check(struct zx_accel *accel, const u8 *frame, u16 len,
		__be16 h_proto, struct zx_ffe_key *key, u32 *pkt_len,
		bool *daddr_local)
{
	struct iphdr ip;
	__be16 ports[2];

	/* #4 只处理裸 IPv4（ARP/PPPoE/VLAN 外层/IPv6 都不碰） */
	if (h_proto != htons(ETH_P_IP))
		return ZX_FFE_V_MISS;
	/* #5 版本/IHL/长度：带选项的、真截断的、尾部是槽位残留的都不碰。
	 *
	 * P10.2 F1 之前这里是 `tot_len > len - ETH_HLEN → miss`，只挡"声明
	 * 比实到长"，不挡"实到比声明多一大截"——后者正是硬件长副本的形态
	 * （len=1514、tot_len=40）。现在改走 zx_ffe_len_ok()，两个方向都挡，
	 * 同时把 <= 6 B 的 MAC padding（60 B 裸 ACK）放进来。 */
	memcpy(&ip, frame + ETH_HLEN, sizeof(ip));
	if (ip.version != 4 || ip.ihl != 5)
		return ZX_FFE_V_MISS;
	if (!zx_ffe_len_ok(accel, 0, (u32)len - ETH_HLEN,
			   (u32)ntohs(ip.tot_len)))
		return ZX_FFE_V_MISS;
	/* #6 IPv4 头校验和（错的帧栈也会丢，快路不背这个锅） */
	if (ip_fast_csum(&ip, ip.ihl))
		return ZX_FFE_V_CSUM_ERR;
	/* #7 分片：不重组、不猜偏移 */
	if (ip.frag_off & htons(IP_MF | IP_OFFSET))
		return ZX_FFE_V_MISS;
	/*
	 * #13（算出来但延后判）：一次 local_ips 查表，与判据链里其它步骤同价，
	 * 而且 pre-NAT 的 daddr 在一条流里不变。裁决见函数头注释。
	 */
	*daddr_local = zx_ffe_is_local_ip(accel, ip.daddr);
	/*
	 * #8 只接 TCP/UDP：ICMP 要回错、IGMP 要上送，其余一律不碰。
	 * 非 TCP/UDP 走不到查表，所以本机目的地址在这里就要报 drop_local（见函数
	 * 头注释最后一段）。
	 */
	if (ip.protocol != IPPROTO_TCP && ip.protocol != IPPROTO_UDP)
		return *daddr_local ? ZX_FFE_V_DROP_LOCAL : ZX_FFE_V_MISS;
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
 *   #6 长度：payload_len + 40 与"帧长 - 14"比较，**允许 <= 6 B 的 MAC
 *      padding，不允许报头声明比实到长**（P10.2 F1）。payload_len 只算
 *      40 B 固定头之后的部分（含扩展头），所以这条同时挡掉 jumbo payload
 *      （payload_len=0 + 逐跳 jumbo 选项）、被截断的帧、被 GRO 聚合过的
 *      帧，以及硬件长副本（len=1514、payload_len=20，尾部是槽位残留）；
 *      快路不做任何拆分。padding 只可能是"最小帧 60 B"补出来的，v6 的
 *      最小 L3（TCP/UDP）是 44 B ⇒ 最多 6 B，与 ZX_FFE_MAX_PAD 一致。
 *   #7 next_header 为 0/43/44/60（逐跳/路由/分片/目的选项）一律回栈：猜扩展头
 *      长度就是把扩展头当 L4 端口读，判错一次就是发错包；
 *   #8 hop_limit <= 1：必须由栈回 ICMPv6 Time Exceeded，计 ttl_err；
 *   #9 目的组播 ff00::/8（NS/NA/RS/RA/MLD 全在这里）回栈——组播没有"下一跳
 *      MAC + 出口"的确定映射，扩散面归栈与桥；
 *   #10 目的地址属本机 v6 地址集合 → drop_local（终结）。**必须排在协议判据
 *      之前**，否则 ping6 本机地址（ICMPv6）先被 #12 挡成普通 miss，
 *      drop_local 永远不涨（与 v4 提前 #13 同一条理由）。v6 保持"立刻回栈"
 *      而不是 v4 的延迟判定：没有 NAT66 就没有"目的地址被改写"的情况，本机
 *      v6 地址就是这条流的终点（P4-TASK §0 "不做 NAT66"）；
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
	/* #6 长度（P10.2 F1）：claimed 是 IP 层长度，不是 payload_len */
	pkt = (u32)ntohs(ip6.payload_len) + sizeof(ip6);
	if (!zx_ffe_len_ok(accel, 1, (u32)len - ETH_HLEN, pkt))
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
	/*
	 * #6 + #3 保证 payload_len >= 4：len >= 64 ⇒ avail >= 50，而
	 * claimed >= avail - 6 >= 44，claimed = 40 + plen ⇒ plen >= 4。
	 * 端口 4 字节必在帧内。
	 */
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
 * IPv4 条目的 NAT 改写（P3-TASK §0 A.3）：先按**旧**值把 L4 校验和的增量算
 * 掉，再写新地址/新端口，最后调用方整头重算 IPv4 头校验和。
 *
 * 为什么 L4 校验和必须增量、而且地址和端口都要算进去：
 *   TCP/UDP 的校验和覆盖"伪首部 + L4 头 + 数据"，伪首部里就有源/目的地址。
 *   只改端口不减地址的增量，校验和立刻错（对端静默丢包，比崩溃难查）。所以
 *   每改一个 4 字节地址就 csum_replace4() 一次，每改一个 2 字节端口就
 *   csum_replace2() 一次——两者都是 RFC 1624 的公式，内核实现在
 *   include/net/checksum.h，不需要 skb。
 *
 * 两个特例（都会让"看起来对"的实现产生静默丢包）：
 *   - UDP 校验和 0 = 发送方没算校验和（RFC 768）。这时**不许**给它编一个：
 *     对端见到非 0 会真的去校验，而我们没有覆盖全部数据的和。整段跳过。
 *   - 增量更新恰好算出 0 的 UDP 校验和必须按全 1 发（0 有"没有校验和"的
 *     特殊含义），原值 0 的情况上面已经跳过。
 *
 * 对齐与字节序（这里错过一次，别想当然）：
 *   - 帧基址是 4 字节对齐（池页 + 16 B 头），L4 起点 14 + ihl*4 = 34，端口在
 *     +0/+2、校验和在 +6（UDP）/+16（TCP），都是 2 字节对齐；访问走
 *     get_unaligned/put_unaligned，避免以后放宽 ihl 时踩到奇地址。
 *   - csum_replace2/csum_replace4 的 **三个操作数必须是同一套字节序约定**：
 *     `*check` 是内存里的 u16（网络序字节被按主机序读出来的那个值），
 *     `flow->sport_out` 是 htons() 过的（同一个约定），所以旧端口也必须用
 *     get_unaligned(*(__be16 *)l4) 取内存值——用 get_unaligned_be16() 会拿到
 *     主机序的数值（字节交换过的），三个操作数就不是同一套约定，算出来的
 *     校验和是错的（用户态对拍：mix 约定给 0xb68e，正确答案 0x1233）。
 *     地址那两处没这个问题：`ip->saddr` 是解引用 __be32，也是内存约定。
 */
static void zx_ffe_v4_nat_rewrite(u8 *frame, struct iphdr *ip,
				  const struct zx_ffe_flow_rt *rt)
{
	u8 *l4 = frame + ETH_HLEN + ip->ihl * 4;
	bool udp = ip->protocol == IPPROTO_UDP;
	__sum16 *check = (__sum16 *)(l4 + (udp ? offsetof(struct udphdr, check) :
					       offsetof(struct tcphdr, check)));
	/*
	 * TCP/UDP 头的前两个字段都是端口：sport、dport。取**内存值**（不是
	 * get_unaligned_be16 的字节交换值），与 *check / *_out 保持同一约定。
	 */
	__be16 sport = get_unaligned((__be16 *)l4);
	__be16 dport = get_unaligned((__be16 *)(l4 + 2));

	if (!(udp && !*check)) {
		if (rt->flags & ZX_FFE_FLOW_NAT_S) {
			csum_replace4(check, ip->saddr, rt->saddr_out);
			csum_replace2(check, sport, rt->sport_out);
		}
		if (rt->flags & ZX_FFE_FLOW_NAT_D) {
			csum_replace4(check, ip->daddr, rt->daddr_out);
			csum_replace2(check, dport, rt->dport_out);
		}
		if (udp && !*check)
			*check = CSUM_MANGLED_0;
	}
	/* 校验和算完才能动地址/端口：上面的 from 值必须是改写前的 */
	if (rt->flags & ZX_FFE_FLOW_NAT_S) {
		ip->saddr = rt->saddr_out;
		put_unaligned(rt->sport_out, (__be16 *)l4);
	}
	if (rt->flags & ZX_FFE_FLOW_NAT_D) {
		ip->daddr = rt->daddr_out;
		put_unaligned(rt->dport_out, (__be16 *)(l4 + 2));
	}
}

/*
 * 报文改写（设计 §3 + P4-TASK §0 + P3-TASK §0 A）。只碰必然要变的字节：
 *
 *   v4  h_dest ← 条目 dmac；h_source ← 出口端口 MAC 快照；带 NAT 位时按
 *       zx_ffe_v4_nat_rewrite() 改地址/端口并增量更新 TCP/UDP 校验和；ttl--；
 *       IPv4 头校验和整头重算（头里改了两处地址加 TTL，重算比三次增量更直白，
 *       而且这是每包一次、20 字节的活）。
 *   v6  h_dest/h_source 同上；hop_limit--；**不碰任何校验和**——IPv6 固定头里
 *       没有校验和，L4 伪首部不含 hop_limit（RFC 8200 §8.1：hop limit 不在
 *       pseudo-header 里），所以改它对比上层 checksum 零影响。这里明确不写
 *       "重算/清零校验和"的代码：写了才是错的。v6 没有 NAT66，带 NAT 位的
 *       v6 条目在判据 #16 就回栈了，不会走到这里。
 *
 * **没有失败返回**：调用它的前提是"这一帧一定提交得出去"——容量检查在同一把
 * tx_lock 里、改写之前做完。改完以后帧头已经是出口形态，再交给协议栈就会发出
 * 一个 MAC 与 TTL 都被动过的坏包，所以调用点之后只剩"提交"和"丢弃"两条路。
 */
/*
 * 数据面条目快照：进 RCU 只读段、查到 flow 之后**只读这一次**，之后判据/
 * 改写/提交只用 rt，不再解引用 flow（P10.2 F4）。
 *
 * 控制面现在用 __zx_ffe_flow_replace() 整体换节点，理论上一次 memcpy 也能
 * 拿到自洽的一份；但"只拷要用到的字段"让数据面与结构布局解耦，也避免把
 * rhash_head/list/rcu 这些会被控制面改的字段读进来。全部用 READ_ONCE：
 * 换节点之前的旧代码路径（以及以后可能的原地写）不会读到撕裂值。
 */
static void zx_ffe_flow_snapshot(struct zx_ffe_flow_rt *rt,
				 const struct zx_ffe_flow *flow)
{
	rt->flags = READ_ONCE(flow->flags);
	ether_addr_copy(rt->dmac, flow->dmac);
	ether_addr_copy(rt->smac, flow->smac);
	rt->egress_type = READ_ONCE(flow->egress_type);
	rt->egress_id = READ_ONCE(flow->egress_id);
	rt->mtu = READ_ONCE(flow->mtu);
	rt->saddr_out = READ_ONCE(flow->saddr_out);
	rt->daddr_out = READ_ONCE(flow->daddr_out);
	rt->sport_out = READ_ONCE(flow->sport_out);
	rt->dport_out = READ_ONCE(flow->dport_out);
	rt->proto = READ_ONCE(flow->key.proto);
}

static void zx_ffe_rewrite(u8 *frame, const struct zx_ffe_flow_rt *rt, int family)
{
	struct iphdr *ip = (struct iphdr *)(frame + ETH_HLEN);
	struct ipv6hdr *ip6 = (struct ipv6hdr *)(frame + ETH_HLEN);

	/* h_dest ← 出口下一跳 MAC；h_source ← 出口端口 MAC 快照（add 时取的） */
	ether_addr_copy(frame, rt->dmac);
	ether_addr_copy(frame + ETH_ALEN, rt->smac);

	if (family) {
		/* 判据 #8 已挡掉 hop_limit <= 1：这里必然 >= 2，减完 >= 1 */
		ip6->hop_limit--;
		return;
	}
	if (rt->flags & ZX_FFE_FLOW_NAT_MASK)
		zx_ffe_v4_nat_rewrite(frame, ip, rt);
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
 * 查表薄封装（design/extensibility.md §5 第 2 条）：判据链里只调这一个入口，现在内部
 * 就是 rhashtable_lookup；将来换成"per-CPU 直接映射缓存 + 回退 rhashtable"，
 * 调用方零改动。调用者持 rcu_read_lock()（条目由 kfree_rcu 释放）。
 */
static struct zx_ffe_flow *zx_ffe_lookup(struct zx_accel *accel,
					 const struct zx_ffe_key *key)
{
	return rhashtable_lookup(&accel->flows, key, zx_ffe_ht_params);
}

/* ---- 出口（egress）op：现在只有 switch-port ------------------------- */

/*
 * 交换机出口的 usable()：本驱动端口 + running + carrier。判据与 P2 的 #14
 * 完全一样，P10.2 F3 起与提交段共用 zx_ffe_out_ok()：数据面不取 rtnl，判不准
 * 就回栈，不丢包。
 */
static bool zx_ffe_egress_switch_usable(struct zx_eth_adapter *adapter, u16 id)
{
	return zx_ffe_out_ok(adapter, id);
}

/*
 * 交换机出口的提交：写发送描述符 + TX_START + 出口侧记账。
 *
 * 调用者（zx_ffe_forward()）已经持 adapter->tx_lock、刚查过环容量、并且已经把
 * 帧改写成出口形态，所以这里正常不会失败；真失败（出口在判据段之后掉链、
 * 代码/内存被改坏）就按"丢帧"处理：帧已经改过，绝不能回栈，槽位也没交给
 * 硬件，由调用方把 bp 还给 BMU（返回 DROPPED，complete() 计 rx_drop）。
 *
 * P10.2 F3（usable() → submit() 的 TOCTOU）：判过出口可用之后、写描述符之前，
 * 出口可能掉链（carrier off）或 admin down。旧代码照样写描述符并记 fwd++，
 * 这一帧没有任何 FFE 计数能看见它没出去。现在在真正提交前**紧邻处**用同一套
 * zx_ffe_out_ok() 再判一次，不满足就返回失败（调用方返 DROPPED：bp 还给
 * BMU、rx_drop++、dev_warn_ratelimited），fwd++ 只在提交成功后由 complete()
 * 加。数据面不取 rtnl，所以这是"宁可丢一帧也不发错"的良性竞态。
 *
 * 现在恒 n == 1（design §5 第 1 条的批量预留）：一批一帧，与 P2 的提交段**逐字节
 * 等价**——同一个描述符内容、同一次 TX_START、同样的失败返回。将来的批量提交
 * （一次取锁/一次记账）只改这个函数内部，分层与调用方不变。
 *
 * P10.2 F4 起不再收 flow 指针：egress_id 由调用方从 RCU 段的条目快照传入，
 * 控制面换节点不会让提交段落到另一条条目的出口上。
 */
static int zx_ffe_egress_switch_submit(struct zx_eth_adapter *adapter,
				       struct zx_ffe_pkt **pkts, int n,
				       u16 eid)
{
	struct zx_ffe_pkt *pkt = pkts[0];
	struct net_device *nd;
	u16 bp = (u16)(uintptr_t)pkt->token;

	/* 现在恒 n == 1；批量提交见 design/extensibility.md §5 第 1 条 */
	(void)n;

	if (eid >= ZX_NUM_PORTS)	/* 与 #14 同源守卫；今天不可达 */
		return -EINVAL;

	/* F3：判据段到提交段之间出口可能已经不可用，紧邻提交再判一次 */
	if (!zx_ffe_out_ok(adapter, eid)) {
		dev_warn_ratelimited(adapter->dev,
			"ffe: egress port %u went down between the check and the submit, dropping a rewritten frame\n",
			eid);
		return -ENETDOWN;
	}

	if (!zx_tx_submit_bp(adapter, eid, bp, pkt->len)) {
		dev_warn_ratelimited(adapter->dev,
			"ffe: TX ring full right after the capacity check (pending=%u), dropping a rewritten frame for eport %u\n",
			adapter->tx_pending, eid);
		return -ENOSPC;
	}
	/*
	 * 出口侧 /proc/net/dev 记账（慢路径在 zx_eth_xmit() 里记，快路在这里记）。
	 * 端口号是入口快照（不再重读条目）；ports[] 只在 probe 里写一次、没有
	 * remove 路径，所以直接取同一个 netdev，不再走一次 running/carrier
	 * 判定——帧已经发出去了，统计必须落在拍板时那个出口上。
	 */
	nd = READ_ONCE(adapter->ports[eid]);
	dev_dstats_tx_add(nd, pkt->len);
	return 0;
}

static const struct zx_ffe_egress_ops zx_ffe_egress_ops[] = {
	{
		.name		= "switch-port",
		.type		= ZX_FFE_EGRESS_SWITCH,
		.usable		= zx_ffe_egress_switch_usable,
		.submit		= zx_ffe_egress_switch_submit,
	},
};

/*
 * 出口类型 → egress op。未注册的类型返回 NULL：调用方一律回栈（不猜，
 * design §1 硬规则 3）。
 */
static const struct zx_ffe_egress_ops *zx_ffe_egress_lookup(u8 type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(zx_ffe_egress_ops); i++)
		if (zx_ffe_egress_ops[i].type == type)
			return &zx_ffe_egress_ops[i];
	return NULL;
}

/*
 * 两族共用的提交段（P4 的硬要求：v4/v6 只走这一份，别复制两份）：
 *
 *   查表 → #13' 本机目的地的延迟判定 → 出口现判（egress op）→ 同口/NAT 字段/
 *   帧长/出口 MTU → 取 tx_lock → 查容量 → 改写 → clean（池可缓存时）→
 *   提交（egress op）→ 条目记账 → 解锁
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
 * daddr_local 是判据链算出来的"这一帧 pre-NAT 的 daddr 属本机"（只有 v4 可能
 * 为真，v6 在 #10 就回栈了）。它不再是提前回栈的条件，而是**查表之后**才用：
 * 端口映射/NAT 回程的 pre-NAT daddr 就是盒子自己的 WAN IP，只有"该条目带
 * DNAT 改写"时这帧才该被转发出去。
 *
 * 出口只通过 flow->egress_type + flow->egress_id 决定（P6）：这里不出现"交换机
 * 端口"的概念，能不能发问 egress op 的 usable()，发出去靠 submit()。
 *
 * 返回 ZX_FFE_V_FWD 表示已提交并记完账（/proc/net/dev 的 rx/tx 也在里面补，
 * tx 那一半在 egress op 的 submit() 里），其余裁决交回 complete() 统一计数。
 */
static enum zx_ffe_verdict
zx_ffe_forward(struct zx_eth_adapter *adapter, struct net_device *nd_in,
	       struct zx_ffe_pkt *pkt, int rx_port,
	       const struct zx_ffe_key *key, u32 pkt_len, bool daddr_local)
{
	struct zx_accel *accel = &adapter->accel;
	const struct zx_ffe_egress_ops *eops;
	struct zx_ffe_flow *flow;
	struct zx_ffe_flow_rt rt;
	struct zx_ffe_pkt *pkts[1];
	unsigned int clean;
	u8 *frame = pkt->data;
	u16 len = pkt->len;
	u16 bp = (u16)(uintptr_t)pkt->token;	/* switch-port 的 token 就是 RX 的 bp */
	int family = pkt->family;

	/* #13 查表（自己持 RCU 只读段：条目由 kfree_rcu 释放）。
	 * 没命中就是普通 miss（P5 修订二起本机目的地在未命中时也算 miss，不再记
	 * drop_local——这时候它和别的 miss 没有区别，且回栈的决定与 daddr 无关）。
	 */
	rcu_read_lock();
	flow = zx_ffe_lookup(accel, key);
	if (!flow) {
		rcu_read_unlock();
		return ZX_FFE_V_MISS;
	}
	/*
	 * P10.2 F4：查到条目后**只读这一次**。控制面可能正在用
	 * __zx_ffe_flow_replace() 换节点；这里拿到的是旧节点，但只要按快照
	 * 用，就是自洽的一份（dmac/smac/flags/出口/NAT 输出/MTU 全部来自
	 * 同一次读）。后面判据链、改写、提交都不许再解引用 flow。
	 */
	zx_ffe_flow_snapshot(&rt, flow);
	/*
	 * #13' 目的地址属本机的**延迟判定**（P5 修订二 §B）。
	 *
	 * 本机目的地的帧有两种截然不同的命运，区别只在这个条目带不带 DNAT：
	 *   - 带 ZX_FFE_FLOW_NAT_D：这是端口映射（ORIGINAL 侧 WAN_IP:port →
	 *     LAN 服务器）或 masquerade 的下载方向（REPLY 侧 WAN_IP:port →
	 *     客户端）。pre-NAT 的 daddr 是盒子自己的 WAN IP，但命中后 #16 的
	 *     改写会把 daddr/dport 换成 daddr_out/dport_out，帧必须**放行**；
	 *   - 不带 NAT_D：这帧确实是给本机终结的（SSH 到路由器自己那类），
	 *     回栈。这就是 drop_local 的新口径：**命中条目但该条目没有 DNAT 改写**，
	 *     仍然是 miss 的子集（计数出口在 zx_ffe_rx() 里，并没有多算）。
	 *
	 * 顺序：排在 #14 出口现判之前，与 P5 的"#13 提到最前"一致——本机目的地
	 * 的帧不该被算成 drop_noport。
	 */
	if (daddr_local && !(rt.flags & ZX_FFE_FLOW_NAT_D)) {
		rcu_read_unlock();
		return ZX_FFE_V_DROP_LOCAL;
	}
	/* #14 出口现查现判（P6 起走 egress op）：类型没注册、或注册了但现在的
	 * 条件不满足（交换机口：不存在/down/carrier off），都回栈，不猜。
	 *
	 * 条目里的 egress_type/egress_id 只可能是数据面认识的组合（ffe_add 与
	 * 旁路学习都按 zx_eth_is_port() 复核过），所以**桥/无线出口在条目里不
	 * 存在**：无线帧永远回栈走 mac80211（队列/聚合/功率控制），见
	 * zx_ffe_learn_hook() 头的"覆盖范围"。
	 */
	eops = zx_ffe_egress_lookup(rt.egress_type);
	if (!eops || !eops->usable(adapter, rt.egress_id)) {
		rcu_read_unlock();
		return ZX_FFE_V_DROP_NOPORT;
	}
	/* #15 同口进出：不做 hairpin */
	if ((int)rt.egress_id == rx_port) {
		rcu_read_unlock();
		return ZX_FFE_V_FALLBACK;
	}
	/*
	 * #16 NAT 语义（P3-TASK §0 A.4）：v4 条目带 SNAT/DNAT 位时，对应的
	 * 输出地址与端口必须齐全（add 时允许写 0，数据面在这里挡），缺一个就
	 * 回栈——拿没改写的帧冒充 NAT 输出，比不转发更糟（对端收到一个源地址
	 * 不属于链路的包）。v6 没有 NAT66（P4-TASK §0 "不做"），带 NAT 位的
	 * v6 条目一律回栈；ffe_add 在入口就拒了，这里只是防御。
	 */
	if (rt.flags & ZX_FFE_FLOW_NAT_MASK) {
		if (family ||
		    ((rt.flags & ZX_FFE_FLOW_NAT_S) &&
		     (!rt.saddr_out || !rt.sport_out)) ||
		    ((rt.flags & ZX_FFE_FLOW_NAT_D) &&
		     (!rt.daddr_out || !rt.dport_out))) {
			rcu_read_unlock();
			return ZX_FFE_V_FALLBACK;
		}
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
	 * Packet Too Big（v6），快路两条都做不到，回栈。rt.mtu 是 ffe_add/学习
	 * 时从出口 netdev 快照的 dev->mtu，语义是 **IP 层 MTU**：对 v6 它是
	 * "IPv6 包最大长度"（40 B 固定头已经算在里面，内核 ip6_mtu() 就是
	 * dev->mtu 减去 L2 开销后的这个值），所以两边都拿 IP 层长度跟它比，
	 * 不写成 payload_len > mtu。0 表示"不知道"——一律回栈，绝不猜。
	 * （邻居表里的 nd_mtu 是 RFC 8200 的 PMTU 缓存，数据面要查邻居才能
	 * 读到，且它只反映单条路径；这里用 dev->mtu 这个链路层上界，超了回栈
	 * 让栈去发 Packet Too Big，语义保守且不需要额外加锁。）
	 */
	if (!rt.mtu || pkt_len > rt.mtu) {
		rcu_read_unlock();
		return ZX_FFE_V_FALLBACK;
	}

	/* #19 环容量（设计 §2.2 #15）：满就回栈，让 qdisc 背压 */
	spin_lock_bh(&adapter->tx_lock);
	if (adapter->tx_pending >= ZX_TX_QUEUE_LIMIT) {
		/*
		 * 活跃流：刷新 lastuse，否则老化会把这条件正在用的条目删掉。
		 * P10.2 F4：写的还是查表拿到的那条（可能是正被换下的旧节点，
		 * 写到它上面的运行态在替换瞬间会丢一拍）；条目内容本身不受
		 * 影响，属于可接受的统计误差。
		 */
		flow->lastuse = jiffies;
		spin_unlock_bh(&adapter->tx_lock);
		rcu_read_unlock();
		return ZX_FFE_V_QFULL;
	}

	/* 改写与提交都在 tx_lock 里，帧内容不会裸露在锁外（P2-TASK §1 顺序） */
	zx_ffe_rewrite(frame, &rt, family);
	/*
	 * 池可缓存时，改写过的头必须在 BMU 读之前推到一致性点（设计 §8-1）：
	 * 脏的只有 16 B 池头 + L2 头 + 固定 IP 头——v4 是 34 B，v6 是 54 B
	 * （16 + ETH_HLEN + 40），payload 在收包侧 invalidate 过、这里只读不写。
	 * 默认映射是非缓存，平时不执行。
	 */
	clean = 16 + ETH_HLEN +
		(family ? sizeof(struct ipv6hdr) : sizeof(struct iphdr));
	/*
	 * 带 NAT 的 v4 条目还要动 L4 头的端口与校验和，一致性点扩到整个 L4
	 * 固定头（TCP 20 B / UDP 8 B）。TCP 的 data offset 不影响这里：端口与
	 * 校验和都在前 20 B 里。UDP 校验和为 0 时改写不动 L4，多同步这 8 B
	 * 只是把已经写过的端口推下去，没有坏处。
	 */
	if (!family && (rt.flags & ZX_FFE_FLOW_NAT_MASK)) {
		clean += rt.proto == IPPROTO_UDP ?
			 sizeof(struct udphdr) : sizeof(struct tcphdr);
	}
	if (adapter->pkt_pool_cacheable)
		dma_sync_single_for_device(adapter->dev,
					   adapter->pkt_pool_dma +
					   (u32)bp * ZX_BUF_SIZE,
					   clean, DMA_TO_DEVICE);
	/*
	 * 提交（n == 1：批量接口现在还只喂一帧，见 design §5 第 1 条）。tx_lock 在
	 * 手里、容量刚查过，正常不会失败；P10.2 F3 起 submit() 会在写描述符前
	 * 紧邻处再判一次出口（carrier/running），失败返回 -ENETDOWN。失败一律
	 * 按"丢帧"处理：帧已经改过，绝不能回栈；槽位也没交给硬件，所以 bp 由
	 * 调用方还给 BMU（返回 DROPPED），complete() 计 rx_drop++。
	 */
	pkts[0] = pkt;
	if (eops->submit(adapter, pkts, 1, rt.egress_id)) {
		spin_unlock_bh(&adapter->tx_lock);
		rcu_read_unlock();
		return ZX_FFE_V_RX_DROP;
	}

	/*
	 * 记账：条目 hits/bytes/lastuse 每次命中更新，否则老化会误删活跃流。
	 * P10.2 F4：这里写的仍是查表拿到的 flow（可能已被换成新节点）；在飞
	 * 帧的这几个计数会落在旧节点上，替换时新节点已经继承过一份，最多丢
	 * 一拍增量，不影响转发。**必须放在 submit 成功之后**——fwd++ 由
	 * complete() 在 FWD 出口加，失败路径走 RX_DROP，不会多计。
	 */
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
	 *
	 * 出口那一半（dev_dstats_tx_add）在 egress op 的 submit() 里——它才知道
	 * 出口是谁；这里只管入口。
	 */
	dev_dstats_rx_add(nd_in, len);
	return ZX_FFE_V_FWD;
}

/* ---- ingress op：switch-port（RX 环 → BP） --------------------------- */

/*
 * 交换机入口（ingress）op 的判据段：入参适配 + 判据链 + 提交。返回裁决，
 * 计数与"调用方该怎么收尾"交给 complete()。
 *
 * 结构（P4 起按 family 分派，两族共用同一段提交逻辑）：
 *
 *   前置段（#0-#3，两族共用）：入口口已 probe → 目的 MAC 非组播/广播 →
 *     目的 MAC 在本机集合 → runt（P10.2 起 v4 是 60 B、v6 仍是 64 B，
 *     见 #3 注释）
 *   判据段（按 family 二选一）：zx_ffe_v4_check() / zx_ffe_v6_check()
 *     ——各自的判据顺序详见这两个函数的头注释。v4 的判据段只把
 *     "目的地址属本机"算成 daddr_local，裁决在提交段里（#13'）。
 *   提交段（#13-#19 + 改写/提交/记账，两族共用）：zx_ffe_forward()
 *
 * 以太类型不再由调用方传进来（P6）：switch-port 的帧就在池槽位里，
 * data + 2*ETH_ALEN 与 zx_eth_rx() 原来看的是同一个地址、同一个值。
 */
static enum zx_ffe_verdict
zx_ffe_ingress_switch_run(struct zx_eth_adapter *adapter,
			  struct zx_ffe_pkt *pkt)
{
	struct zx_accel *accel = &adapter->accel;
	u8 *frame = pkt->data;
	u16 len = pkt->len;
	int family = pkt->family;
	int rx_port = (int)pkt->ingress_id;
	struct zx_ffe_cnt *c = &accel->cnt[family];
	struct zx_ffe_key key;
	struct net_device *nd_in;
	enum zx_ffe_verdict v;
	u32 pkt_len = 0;
	bool daddr_local = false;
	u8 dmac[ETH_ALEN];
	__be16 h_proto;

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
	/*
	 * #3 runt，两族下限**刻意不同**（P10.2 F1）：
	 *   v4 60 = 以太最小净荷（64 B 含 4 B FCS）。Windows 关 timestamp 的
	 *     裸 ACK 是 14+20+20=54 B，MAC 补到 60 B；原来 `<64` 把它当 runt
	 *     挡在快路外，于是 v4 只加速了带选项的 66 B ACK。
	 *   v6 保持 64。v6 最小 TCP ACK 74 B、最小 UDP 62 B，而 62 B 量级
	 *     正好是 ICMPv6（ND/RS/RA 的短形态）——ND 必须回栈，现有
	 *     `hop_limit <= 1` 判据挡不住 hop_limit=255 的 ND，所以这里不能
	 *     跟着降到 60。两族下限不同不是笔误。
	 */
	if (len < (family ? 64 : 60))
		goto plain_miss;

	/* 与 zx_eth_rx() 原来传给这里的 h_proto 同址同值（帧的第 12 字节） */
	h_proto = *(__be16 *)(frame + 2 * ETH_ALEN);

	/*
	 * daddr_local 只由 v4 链填写（v6 在 #10 就按本机目的地回栈了，走不到
	 * 这里），初值 false 保证 v6 与"v4 非本机"两条路完全一致。
	 */
	if (!family)
		v = zx_ffe_v4_check(accel, frame, len, h_proto, &key, &pkt_len,
				    &daddr_local);
	else
		v = zx_ffe_v6_check(accel, frame, len, h_proto, &key, &pkt_len);

	if (v == ZX_FFE_V_OK) {
		/* 判据已过、尚未提交（design §5 第 3 条两段式提交的预留位） */
		pkt->prepared = 1;
		v = zx_ffe_forward(adapter, nd_in, pkt, rx_port, &key, pkt_len,
				   daddr_local);
	}
	return v;

plain_miss:
	return ZX_FFE_V_MISS;
}

/*
 * 交换机入口（ingress）op 的收尾：裁决 → cnt[family] 的字段 → 调用方要的
 * RX 结果。原 zx_ffe_rx() 末尾的"统一出口"整段搬到这里，两族口径仍然只有
 * 这一份。
 *
 * 计数口径（设计 §1.5 / P1-TASK §1）：lookup == hit + miss 恒成立；hit 只统计
 * 真正转发出去的帧（P2 起 hit == fwd）；csum_err/ttl_err/drop_local/
 * drop_noport/fallback/fallback_qfull/rx_drop 都是 miss 的子集，用来区分是哪
 * 条判据把它挡回去的。v6 没有头校验和，所以 csum_err 在 v6 上恒为 0；
 * ttl_err 对 v6 是 hop_limit 到期。drop_local 从 P5 修订二起是"命中条目但该
 * 条目没有 DNAT 改写"（v4）与本机终结（v6、非 TCP/UDP 的 v4），见
 * zx_ffe_forward() 的 #13'——仍是 miss 的子集，只是子集的定义变了。
 *
 * bp/描述符不在这里动：switch-port 的 RX 收尾（消耗描述符、DROPPED 时把 bp
 * 还给 BMU）仍由 zx_eth_rx() 按返回值做（见 zx279128s-main.c 的 ffe_done）。
 */
static enum zx_ffe_rx_result
zx_ffe_ingress_switch_complete(struct zx_eth_adapter *adapter,
			       struct zx_ffe_pkt *pkt, enum zx_ffe_verdict v)
{
	struct zx_ffe_cnt *c = &adapter->accel.cnt[pkt->family];

	/* 统一出口：裁决 → 计数 → 返回值（两族口径只有这一份） */
	switch (v) {
	case ZX_FFE_V_FWD:
		c->hit++;
		c->fwd++;
		c->bytes += pkt->len;
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
}

/*
 * ingress 注册表（design/extensibility.md §1）：现在只有 switch-port 一项，
 * accel->ingress 指向它（zx_accel_init）。WiFi 接入时往这里加一项
 * { .name = "wifi-rx", .run = ..., .complete = ... } 就够了：判据链、流表、
 * 老化、学习全部复用，数据面不用改。
 */
static const struct zx_ffe_ingress_ops zx_ffe_ingress_ops[] = {
	{
		.name		= "switch-port",
		.run		= zx_ffe_ingress_switch_run,
		.complete	= zx_ffe_ingress_switch_complete,
	},
};

/*
 * 加速分发点的入口（zx_eth_rx() 调用）：P6 起只做入参适配，剩下的交给 ingress
 * op（design/extensibility.md §1）。
 *
 * 判据链全部通过以后，把**同一个 bp** 写进发送描述符交给硬件，帧不再进
 * zc/skb/协议栈（P2-TASK §0）。返回值告诉调用方怎么收尾（enum
 * zx_ffe_rx_result）：STACK = 帧一个字节都没动、照原路径走；FORWARDED = 已交给
 * 发送硬件、只做 RX 收尾且**不许还 bp**；DROPPED = 快路里确定丢了、收尾时把 bp
 * 还回 BMU。
 *
 * 包描述符（struct zx_ffe_pkt）是入口无关的那一层：data/len/family 在这里填，
 * ingress_id/token 留给 ingress op（switch-port 的 token 就是 bp）。以太类型
 * 也不在这里解析——ingress op 从帧里自己读。
 */
enum zx_ffe_rx_result zx_ffe_rx(struct zx_eth_adapter *adapter, u16 bp, u16 len,
				int rx_port, int family)
{
	struct zx_accel *accel = &adapter->accel;
	const struct zx_ffe_ingress_ops *ops;
	struct zx_ffe_pkt pkt;
	enum zx_ffe_verdict v;

	if (family < 0 || family > 1)
		return ZX_FFE_RX_STACK;
	ops = READ_ONCE(accel->ingress);
	if (!ops)	/* 数据面在 probe 之后才跑；NULL 只防御，回栈不崩 */
		return ZX_FFE_RX_STACK;

	pkt.data = adapter->pkt_pool + (u32)bp * ZX_BUF_SIZE + 16;
	pkt.len = len;
	pkt.ingress_id = (u16)rx_port;
	pkt.family = (u8)family;
	pkt.prepared = 0;
	pkt.token = (void *)(uintptr_t)bp;

	v = ops->run(adapter, &pkt);
	return ops->complete(adapter, &pkt, v);
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
 * 无锁读某一族的三个 64 位计数（P8 D）。32 位 ARM 上 u64 读是两条 ldr，
 * 数据面又是裸 u64++（见 struct zx_ffe_cnt 的注释），两者并发时可能读到
 * "半新半旧"的组合值：增量因此可能被算小（漏判）也可能被算大（提前降级）。
 * 偏漏判不致命——下一拍/下一窗还会再判；偏提前降级也只是回 stack，不丢包，
 * 属于安全侧。为了把这个窗口压到最小，这里连读两遍，两遍相等才认；不等就
 * 返回 false，由调用者"本拍不判"，不拿可疑值去比 0.1% 的阈值。
 *
 * 计数只增，所以两遍相等即视为一致快照（不追求与数据面原子，只求不拿一个
 * 明显撕裂的值当判据）。读失败的代价只是本拍不判，绝不会卡死判据：
 * 窗口起点读失败 → 下一次 tick 再开窗；窗口到点读失败 → 窗口保持，下一拍
 * 再判。
 */
static bool zx_accel_fb_cnt_read(const struct zx_accel *accel, int family,
				 struct zx_ffe_fb_base *out)
{
	const struct zx_ffe_cnt *c = &accel->cnt[family];
	struct zx_ffe_fb_base a, b;

	a.lookup = READ_ONCE(c->lookup);
	a.drop = READ_ONCE(c->drop_noport);
	a.fallback = READ_ONCE(c->fallback);
	b.lookup = READ_ONCE(c->lookup);
	b.drop = READ_ONCE(c->drop_noport);
	b.fallback = READ_ONCE(c->fallback);

	if (a.lookup != b.lookup || a.drop != b.drop ||
	    a.fallback != b.fallback)
		return false;

	*out = a;
	return true;
}

/*
 * 开一个 10 s 的降级判定窗口（P8 C）：把 fb_base[] 快照成当前 cnt[]、把
 * fb_since[] 记成窗口起点 now。窗口到点后由 zx_accel_degraded() 用
 * "当前 - fb_base" 判一次；没到阈值就关窗（fb_since = 0），下一拍再由本函数
 * 从新起点重开。
 *
 * 所以 fb_base 现在是"**窗口**起点的基线"，不再是"进入 ffe 以来的基线"。
 * 进入 ffe 时（zx_accel_set() 的 S5.1）也会调一次，因此第一个窗口从入场那
 * 一刻算起；窗口与入场基线的差别只在这里：入场那一次的起点是入场时刻，
 * 之后每次关窗重开的起点是重开时刻。两者都不背开机以来的历史计数。
 *
 * 返回 false 表示读到不一致的 64 位计数（见 zx_accel_fb_cnt_read()），本拍
 * 不开窗；调用者按"下一拍再来"处理，fb_since 保持 0。
 */
static bool zx_accel_fb_window_open(struct zx_accel *accel, int family,
				    unsigned long now)
{
	if (!zx_accel_fb_cnt_read(accel, family, &accel->fb_base[family]))
		return false;

	accel->fb_since[family] = now;
	return true;
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
	/*
	 * S5.1 切入 ffe 成功，开第一个降级窗口：fb_base[] 与 fb_since[] 都重置
	 * 到这一刻，第一个 10 s 从入场算起，不背开机以来的历史计数
	 * （design/extensibility.md §6.6；P8 C 起 fb_base 是窗口起点，见
	 * zx_accel_fb_window_open()）。读不到一致的 64 位快照时 fb_since 保持
	 * 0，下一次 age tick 会重新开窗，不影响切换本身。
	 */
	if (new_mode == ZX_ACC_FFE)
		(void)zx_accel_fb_window_open(accel, family, jiffies);

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
	/*
	 * P3-TASK §0 B.2：只在 v4 从 hw 切到 ffe 时提示迁移。S4 已经把这一族
	 * 的 FFE 条目全清了（"切表=全清"），所以这里照实报"上次 PPE 快照有 N
	 * 条、表里现在 M 条"，剩下的要用户按 conntrack -L 手工 ffe_add；
	 * mig_ppe_flows 由 ffe_migrate 在 PPE 还绑着时写入。
	 */
	if (family == 0 && old_mode == ZX_ACC_HW && new_mode == ZX_ACC_FFE)
		dev_info(adapter->dev,
			 "accel: hw -> ffe: last PPE snapshot had %u flow(s), the FFE table has %u; re-add the rest with ffe_add (NAT fields from conntrack -L)\n",
			 READ_ONCE(accel->mig_ppe_flows), accel->n_flows);

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
 * P7 上机后改口径（design/extensibility.md §6.6），三条：
 *   1. 分子去掉 drop_local。它是"目的地址属本机、帧正常回栈"（complete() 里
 *      计完仍返回 STACK），一帧没丢，不是失败；管理面 ping -f 打盒子不再
 *      能把它算成快路故障。drop_local 照旧在 cnt[]/debugfs 里显示。
 *   2. 分子分母都改用增量（当前 cnt[] 减去窗口起点的快照 fb_base[]），不再
 *      用开机累计值：累计口径下一次 ping -f 就永久污染，此后每次进 ffe 10 s
 *      内被踢回 stack 并清空流表。
 *   3. 窗口内帧数太少（< ZX_FFE_FB_MIN_LOOKUP）不判定；Δlookup 为零时直接
 *      返回 ZX_FFE_FB_CLEAR（判据用乘法比而不是除法，这里也没有除零）。
 *
 * P8 C（复审 B/C/D）：这里的"窗口"改成真 10 s 固定窗口，和"进入 ffe 以来"
 * 彻底分开——窗口起点的计数快照与时刻由 zx_accel_fb_window_open() 写进
 * fb_base[]/fb_since[]，本函数只在**窗口到点后**被调用（开窗、等窗都在
 * zx_accel_autofallback() 里）：
 *   - 只有"当前 - 窗口起点"的增量进判据，窗口之外的历史一概不算；
 *   - 未达阈值就关窗，下一拍从新起点重开，所以"1 秒抖动 + 之后 9 秒干净"
 *     不会被旧抖动的累计值在第 10 秒踢回 stack；反过来，持续故障的每个窗
 *     都超阈值，最多 10~11 s 就降级（既不会永不降级，也不会一切进就降级）。
 *   - Δlookup < ZX_FFE_FB_MIN_LOOKUP 视同"未达阈值"：关窗重开，帧太少时
 *     0.1% 的置信度不够，也保证乘法比里没有除零。
 *
 * 判为降级时顺便打印 dmesg —— 打印的就是判据用的那份窗口增量（不另算
 * 一遍，避免"日志"和"判据"对不上），另外附上开机累计值便于和历史日志
 * 对照（P7 的脚本只 grep "fell back" 这个子串，格式变化不影响解析）：
 *   d_drop     = Δdrop_noport（与判据同一个分子，**不含** drop_local）
 *   d_fallback = Δfallback
 *   d_lookup   = Δlookup（判据的分母）
 *   cum …      = 老口径的累计值（drop 仍是 drop_noport + drop_local）
 */
enum zx_ffe_fb_verdict {
	ZX_FFE_FB_RETRY = -1,	/* 本拍读到的 64 位计数不一致：不判，窗口保持 */
	ZX_FFE_FB_CLEAR = 0,	/* 未达阈值 / 窗口太小：关窗，下拍重开 */
	ZX_FFE_FB_DEGRADE = 1,	/* 判为降级 */
};

static int zx_accel_degraded(const struct zx_eth_adapter *adapter, int family)
{
	const struct zx_accel *accel = &adapter->accel;
	const struct zx_ffe_fb_base *b = &accel->fb_base[family];
	struct zx_ffe_fb_base now;
	u64 base_lookup = READ_ONCE(b->lookup);
	u64 base_drop = READ_ONCE(b->drop);
	u64 base_fb = READ_ONCE(b->fallback);
	u64 dlookup, ddrop, dfb;

	/*
	 * P8 D：cnt[] 的 64 位计数是无锁读（32 位 ARM 上两条 ldr，可能撕裂），
	 * 读两遍不一致就本拍不判，取舍见 zx_accel_fb_cnt_read()。基线
	 * fb_base[] 同样是 64 位无锁读，但它只在开窗时写（每窗一次），且下面
	 * 有"计数只增"的守卫，最坏是本窗判不出来、下一窗重来，不会永久错判。
	 */
	if (!zx_accel_fb_cnt_read(accel, family, &now))
		return ZX_FFE_FB_RETRY;

	/* 计数只增；倒挂说明快照/读被写坏，不判，关窗重开 */
	if (now.lookup <= base_lookup)
		return ZX_FFE_FB_CLEAR;
	dlookup = now.lookup - base_lookup;
	/* 窗口里帧太少：不判，关窗重开（下一窗重新计时） */
	if (dlookup < ZX_FFE_FB_MIN_LOOKUP)
		return ZX_FFE_FB_CLEAR;
	ddrop = now.drop > base_drop ? now.drop - base_drop : 0;
	dfb = now.fallback > base_fb ? now.fallback - base_fb : 0;
	if (ddrop * 1000 <= dlookup * ZX_FFE_FB_DROP_PERMILLE &&
	    dfb * 1000 <= dlookup * ZX_FFE_FB_FALLBACK_PERMILLE)
		return ZX_FFE_FB_CLEAR;

	dev_info(adapter->dev,
		 "accel: IPv%d fell back to stack: d_drop=%llu d_fallback=%llu of d_lookup=%llu (cum %llu drop, %llu fallback of %llu lookups)\n",
		 family ? 6 : 4, ddrop, dfb, dlookup,
		 now.drop + READ_ONCE(accel->cnt[family].drop_local),
		 now.fallback, now.lookup);
	return ZX_FFE_FB_DEGRADE;
}

bool zx_accel_autofallback(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;
	bool degraded = false;
	int family;

	if (!READ_ONCE(accel->autofallback)) {
		/*
		 * 关掉期间窗口一律作废（每秒都清一次）：重新打开时下一拍从
		 * 那一刻开新窗，不拿关闭期间的旧起点判一个超长窗口——否则
		 * 关掉几分钟再打开，第一拍就会用几分钟的增量去比 0.1%。
		 */
		accel->fb_since[0] = 0;
		accel->fb_since[1] = 0;
		return false;
	}

	for (family = 0; family < 2; family++) {
		int verdict;

		/* 不在 ffe 里就没有窗口；下次切入时由 S5.1 重新开 */
		if (zx_accel_cur_mode(accel, family) != ZX_ACC_FFE) {
			accel->fb_since[family] = 0;
			continue;
		}
		/* 还没有窗口（刚切入，或上一窗已关）：现在开一个 */
		if (!accel->fb_since[family]) {
			/* 读不到一致快照时 fb_since 保持 0，下一拍再试 */
			(void)zx_accel_fb_window_open(accel, family, jiffies);
			continue;
		}
		/* 窗口未满 10 s：这一拍什么都不判 */
		if (time_before(jiffies, accel->fb_since[family] +
				msecs_to_jiffies(ZX_FFE_FB_WINDOW_MS)))
			continue;

		verdict = zx_accel_degraded(adapter, family);
		if (verdict == ZX_FFE_FB_RETRY)
			continue;	/* 本拍不判：窗口保持，下一拍再判 */
		if (verdict == ZX_FFE_FB_CLEAR) {
			accel->fb_since[family] = 0;	/* 关窗，下拍重开 */
			continue;
		}

		/* 已判为降级（dmesg 在 zx_accel_degraded() 里打过了） */
		accel->fb_since[family] = 0;
		if (!zx_accel_set(adapter, family, ZX_ACC_STACK))
			degraded = true;
	}
	return degraded;
}

/*
 * P10.3：学习门控到点自动开一次。写 ffe_learn（debugfs）会清 learn_arm_at，
 * 所以用户手动开/关之后这里不会再插手；只开一次，不重复打印。
 */
static void zx_ffe_learn_arm_tick(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;
	unsigned long arm_at = READ_ONCE(accel->learn_arm_at);

	if (!arm_at || time_before(jiffies, arm_at))
		return;
	WRITE_ONCE(accel->learn_arm_at, 0);
	WRITE_ONCE(accel->learn_on, 1);
	dev_info(adapter->dev,
		 "accel: ffe_learn auto-enabled %u ms after probe (ff_learn gate; see DIAG-P10.2-HANG.md)\n",
		 learn_delay_ms);
}

/*
 * 每秒一拍：学习门控 + 流表老化 + 自动降级判据（设计 §4.1 / §9-4）+
 * 学习环排空（P5）。
 */
static void zx_accel_age_work(struct work_struct *work)
{
	struct zx_eth_adapter *adapter =
		container_of(to_delayed_work(work), struct zx_eth_adapter,
			     accel.age);

	/*
	 * P10.3 门控先于 drain：同一拍把学习打开，紧接着的排空就能学，
	 * 不必白等一拍。age work 与 learn_on 无关、每秒都在跑。
	 */
	zx_ffe_learn_arm_tick(adapter);
	/* 先落表再老化：新学的条目不该在同一个节拍里被老化的时间边界伤到 */
	zx_ffe_learn_drain(adapter);
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
	/* 数据面的 ingress 分派（P6 分层）：现在只有 switch-port 一项 */
	accel->ingress = zx_ffe_ingress_ops;
	accel->v4_mode = ZX_ACC_STACK;
	accel->v6_mode = ZX_ACC_STACK;
	accel->zc_mask = 0;
	WRITE_ONCE(accel->quiesce, false);
	memset(accel->cnt, 0, sizeof(accel->cnt));
	/* 降级窗口与 cnt[] 一起归零：首次进 ffe 时由 S5.1 开窗、取起点快照 */
	memset(accel->fb_base, 0, sizeof(accel->fb_base));
	accel->sw_in = 0;
	accel->sw_out = 0;
	accel->drain_ms = 0;
	accel->drain_frames = 0;
	accel->autofallback = true;
	memset(accel->fb_since, 0, sizeof(accel->fb_since));
	accel->n_flows = 0;
	accel->mig_ppe_flows = 0;
	accel->tbl_full = 0;
	accel->learn_rings = NULL;
	/*
	 * P10.3 交付门控：probe 期不学，等开机窗口过去再由 1 s 节拍自动开一次
	 * （原因见上面 learn_delay_ms 的注释）。learn_arm_at 用 jiffies 记到点
	 * 时刻；写 ffe_learn 会清掉它。
	 */
	accel->learn_on = 0;
	accel->learn_arm_at = learn_delay_ms ?
		jiffies + msecs_to_jiffies(learn_delay_ms) : 0;
	accel->learn_cand = 0;
	accel->learn_add = 0;
	accel->learn_dup = 0;
	accel->learn_drop = 0;
	accel->learn_noneigh = 0;
	accel->learn_nobrport = 0;
	accel->learn_nonport = 0;
	accel->learn_dedup = 0;
	/* P10.1 的按族计数与上面 8 个合计计数一起归零（下标 0 = v4、1 = v6） */
	memset(accel->learn_cnt, 0, sizeof(accel->learn_cnt));
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

	/*
	 * 旁路学习的四件套（P5 三件 + P10 的 v6 钩子）：每 CPU 候选环 + 两条
	 * POST_ROUTING 钩子（IPv4/IPv6 各一条，共用同一个回调）。
	 * 环必须在注册钩子之前就位（钩子第一件事就是取 this_cpu 环）；任何一条
	 * 钩子注册失败都按 init 失败处理——没有学习能力时驱动其余部分照常可用，
	 * 但这里选择整体回滚，避免"看起来在跑、其实学不到"的半状态。
	 */
	accel->learn_rings = alloc_percpu(struct zx_ffe_learn_ring);
	if (!accel->learn_rings) {
		dev_err(adapter->dev, "accel: alloc_percpu(learn) failed\n");
		ret = -ENOMEM;
		goto err_inet6;
	}
	/*
	 * 两条 hook_ops 的字段逐一填（其余字段必须为零，NF_HOOK_OP_UNDEFINED）。
	 * nf_hook_ops 是按 pf 注册/注销的，一次注册只能覆盖一个 pf，所以两族
	 * 各有一条；回调与 priv 相同，state->pf 在钩子里分派。
	 */
	memset(&accel->learn_hook, 0, sizeof(accel->learn_hook));
	memset(&accel->learn_hook6, 0, sizeof(accel->learn_hook6));
	accel->learn_hook.hook = zx_ffe_learn_hook;
	accel->learn_hook.pf = NFPROTO_IPV4;
	accel->learn_hook.hooknum = NF_INET_POST_ROUTING;
	accel->learn_hook6.hook = zx_ffe_learn_hook;
	accel->learn_hook6.pf = NFPROTO_IPV6;
	accel->learn_hook6.hooknum = NF_INET_POST_ROUTING;
	/*
	 * NF_IP_PRI_LAST / NF_IP6_PRI_LAST 数值同为 INT_MAX：在 nf_nat 的
	 * NAT_SRC(100) 之后，也等于 conntrack confirm 的优先级（同优先级时
	 * 后注册的先跑，见钩子注释）。P10.1 起两族各用本族常量：v4 的
	 * confirm 用 NF_IP_PRI_CONNTRACK_CONFIRM、v6 的 confirm 用
	 * NF_IP6_PRI_LAST（nf_conntrack_proto.c:255/412），值相同，但写成
	 * 各族自己的名字，读者不会误以为"NF_IP_PRI_LAST 是 v6 的通用值"。
	 */
	accel->learn_hook.priority = NF_IP_PRI_LAST;
	accel->learn_hook.priv = adapter;
	accel->learn_hook6.priority = NF_IP6_PRI_LAST;
	accel->learn_hook6.priv = adapter;
	ret = nf_register_net_hook(&init_net, &accel->learn_hook);
	if (ret) {
		dev_err(adapter->dev,
			"accel: nf_register_net_hook(IPv4 POST_ROUTING) failed: %d\n",
			ret);
		free_percpu(accel->learn_rings);
		accel->learn_rings = NULL;
		goto err_inet6;
	}
	ret = nf_register_net_hook(&init_net, &accel->learn_hook6);
	if (ret) {
		dev_err(adapter->dev,
			"accel: nf_register_net_hook(IPv6 POST_ROUTING) failed: %d\n",
			ret);
		/*
		 * 回滚已注册的 IPv4 钩子。nf_unregister_net_hook() 只是把旧
		 * hook 数组交给 call_rcu，可能有 v4 包正在跑这个回调，所以
		 * 释放环之前必须等一个 RCU 宽限期（与 zx_accel_exit() 同一条
		 * 理由）。
		 */
		nf_unregister_net_hook(&init_net, &accel->learn_hook);
		synchronize_rcu();
		free_percpu(accel->learn_rings);
		accel->learn_rings = NULL;
		goto err_inet6;
	}

	schedule_delayed_work(&accel->age, msecs_to_jiffies(ZX_FFE_TICK_MS));
	return 0;

err_inet6:
	unregister_inet6addr_notifier(&accel->inet6addr_nb);
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
 * 通知链与 netfilter 钩子，最后按条目释放三张表。本驱动是 builtin、正常没有
 * rmmod，但 probe 失败与将来加 remove 都必须走这里，不能漏。
 */
void zx_accel_exit(struct zx_eth_adapter *adapter)
{
	struct zx_accel *accel = &adapter->accel;

	cancel_delayed_work_sync(&accel->age);
	cancel_work_sync(&accel->port_flush_work);
	unregister_inet6addr_notifier(&accel->inet6addr_nb);
	unregister_inetaddr_notifier(&accel->inetaddr_nb);
	unregister_netdevice_notifier(&accel->netdev_nb);
	if (accel->learn_rings) {
		/*
		 * 注册与注销必须对称（P10 起两条钩子）：先摘 v6、再摘 v4，两条
		 * 都摘完才等宽限期、释放环。只摘一条会让另一个族的钩子继续跑，
		 * 访问已释放的环 = UAF。
		 */
		nf_unregister_net_hook(&init_net, &accel->learn_hook6);
		nf_unregister_net_hook(&init_net, &accel->learn_hook);
		/*
		 * nf_unregister_net_hook() 只是把旧 hook 数组交给 call_rcu，
		 * 并不保证此刻没有钩子正在别的 CPU 上跑；环是 free_percpu
		 * 释放的，所以必须自己等一个 RCU 宽限期（钩子都在
		 * rcu_read_lock 里执行）。
		 */
		synchronize_rcu();
		free_percpu(accel->learn_rings);
		accel->learn_rings = NULL;
	}
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
	u32 qlen_peak;
	bool bound;

	/* 读 flow_block 列表要持 rtnl（TC 绑定/解绑时持着它） */
	rtnl_lock();
	bound = zx_ppe_has_bound_blocks(adapter);
	rtnl_unlock();

	/*
	 * qlen_peak 是**整环共享**统计：v4/v6 共用一个 TX 环，峰值只在
	 * zx279128s-main.c 的 zx_tx_submit_bp() 里写进 cnt[0]（那一层拿不到
	 * family），所以 cnt[1].qlen_peak 恒 0。v6 这一行也显示 cnt[0] 的值，
	 * 免得读成"v6 快路从没占用过 TX 环"；cnt[1] 那个 0 没有意义。
	 */
	qlen_peak = READ_ONCE(accel->cnt[0].qlen_peak);

	seq_printf(s, "%s\n",
		   zx_accel_mode_name(zx_accel_cur_mode(accel, family)));
	seq_printf(s, "lookup=%llu hit=%llu miss=%llu fwd=%llu fallback=%llu fallback_qfull=%llu drop_noport=%llu drop_local=%llu csum_err=%llu ttl_err=%llu bytes=%llu qlen_peak=%u rx_drop=%llu len_short=%llu len_long=%llu\n",
		   c->lookup, c->hit, c->miss, c->fwd, c->fallback,
		   c->fallback_qfull, c->drop_noport, c->drop_local,
		   c->csum_err, c->ttl_err, c->bytes, qlen_peak,
		   c->rx_drop, c->len_short, c->len_long);
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

/* ---- ffe_add / ffe_del / ffe_flush（P1-TASK §2 / P3-TASK §0 A.2）---- */

/*
 * 最多的 token 数：ffe_add 的 10 个固定字段 + nat/dnat 各 3 个 = 16。
 * ffe_del 只用前 8 个、ffe_flush 只用 1 个，同一份缓冲区够用。
 */
#define ZX_FFE_ARG_MAX		16

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

/* 把 NAT 尾段追加到 ffe_last（key_print 已经把键打在前面） */
static void zx_ffe_nat_print(char *buf, size_t len,
			     const struct zx_ffe_nat *nat)
{
	size_t n = strlen(buf);

	if (n >= len)
		return;
	if (nat->flags & ZX_FFE_FLOW_NAT_S)
		scnprintf(buf + n, len - n, " nat %pI4 %u",
			  &nat->saddr_out, ntohs(nat->sport_out));
	n = strlen(buf);
	if (n >= len)
		return;
	if (nat->flags & ZX_FFE_FLOW_NAT_D)
		scnprintf(buf + n, len - n, " dnat %pI4 %u",
			  &nat->daddr_out, ntohs(nat->dport_out));
}

/*
 * ffe_add 的可选 NAT 尾段（P3-TASK §0 A.2）。从 argv[base] 起每 3 个 token
 * 一组，关键字 nat/dnat，顺序任意、可同时给、不可重复：
 *
 *   ... <dmac> [nat <saddr_out> <sport_out>] [dnat <daddr_out> <dport_out>]
 *
 * nat  → ZX_FFE_FLOW_NAT_S + saddr_out/sport_out
 * dnat → ZX_FFE_FLOW_NAT_D + daddr_out/dport_out
 *
 * 地址必须是点分十进制 IPv4（NAT 只属于 v4 条目，v6 在调用方入口就拒了）。
 * 地址/端口写 0 这里不拦：数据面判据 #16 把"置了位但字段不全"的条目回栈，
 * 这样"下发错条目 → 回栈而不是发错包"这条语义在实机上可验证。
 */
static int zx_ffe_nat_parse(char **argv, int base, int argc,
			    struct zx_ffe_nat *nat, const char **err)
{
	int i = base;

	while (i < argc) {
		unsigned int port;
		__be32 addr;
		bool snat;
		u8 bit;

		if (i + 3 > argc) {
			*err = "nat/dnat needs <ipv4> <port>";
			return -EINVAL;
		}
		if (!strcmp(argv[i], "nat"))
			snat = true;
		else if (!strcmp(argv[i], "dnat"))
			snat = false;
		else {
			*err = "unknown keyword (only nat/dnat)";
			return -EINVAL;
		}
		bit = snat ? ZX_FFE_FLOW_NAT_S : ZX_FFE_FLOW_NAT_D;
		if (nat->flags & bit) {
			*err = snat ? "duplicate nat" : "duplicate dnat";
			return -EINVAL;
		}
		if (!in4_pton(argv[i + 1], -1, (u8 *)&addr, '\0', NULL)) {
			*err = "bad NAT IPv4 address";
			return -EINVAL;
		}
		if (kstrtouint(argv[i + 2], 10, &port) || port > 0xffff) {
			*err = "bad NAT port";
			return -EINVAL;
		}

		if (snat) {
			nat->saddr_out = addr;
			nat->sport_out = htons(port);
		} else {
			nat->daddr_out = addr;
			nat->dport_out = htons(port);
		}
		nat->flags |= bit;
		i += 3;
	}
	return 0;
}

/*
 * ffe_add <key 8 个 token> <eport> <dmac> [nat ...] [dnat ...]
 *
 * **幂等**（P5 修订二 §C）：同 key 已存在时用新节点整体替换（见
 * __zx_ffe_flow_replace() 与 __zx_ffe_flow_merge() 的 flags 合并规则）并返回
 * 成功，不再 -EEXIST、也不再产生第二条同键行；
 * add_replace 计数加一，ffe_last 里带 `(replaced ...)` 便于上机分辨。
 * 这与"学习路径先插、用户后钉"互相不打架：谁后写谁赢，NAT 位只增不减。
 */
static int zx_ffe_op_add(struct zx_eth_adapter *adapter, char **argv, int argc)
{
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_nat nat = {};
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
	ret = zx_ffe_nat_parse(argv, 10, argc, &nat, &err);
	if (ret)
		goto fail;
	/* 没有 NAT66：v6 条目带 nat/dnat 直接拒（P4-TASK §0 "不做"） */
	if (key.family == 6 && nat.flags) {
		err = "v6 has no NAT66: nat/dnat are IPv4 only";
		ret = -EOPNOTSUPP;
		goto fail;
	}

	mutex_lock(&accel->lock);
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
	flow->egress_type = ZX_FFE_EGRESS_SWITCH;
	flow->egress_id = eport;
	/*
	 * 出口 MTU 也在 add 时快照：数据面判断据 #18（超 MTU 回栈让栈去分片/发
	 * ICMP(6)），每包读 nd->mtu 没有意义且要多一次跨 cache 访问。语义是
	 * **IP 层 MTU**：dev->mtu 本身就是"IPv4 tot_len / IPv6 payload_len+40
	 * 的上限"（v6 的 40 B 固定头算在里面），所以两族都拿 IP 层长度跟它比。
	 * MTU 后来被改大/改小不会刷新条目——这时宁可条目偏保守（回栈），不值得
	 * 为它加通知链。
	 */
	flow->mtu = min_t(u32, nd->mtu, 0xffff);
	/* NAT 尾段在 key/eport/dmac 都过了之后才写进条目（P3） */
	flow->flags = ZX_FFE_FLOW_STATIC | nat.flags;
	flow->saddr_out = nat.saddr_out;
	flow->daddr_out = nat.daddr_out;
	flow->sport_out = nat.sport_out;
	flow->dport_out = nat.dport_out;
	flow->lastuse = jiffies;
	/*
	 * 落表：同 key 已存在 → 新节点整体换上并返回 1（flow 已属于表，
	 * **不要 free**）；新键才走容量检查（__zx_ffe_store 里，"表满"只
	 * 拒绝新键）。P10.2 F4 起不再原地改活条目，见 __zx_ffe_flow_replace()。
	 */
	ret = __zx_ffe_store(adapter, flow);
	if (ret < 0) {
		kfree(flow);
		snprintf(accel->ffe_last, sizeof(accel->ffe_last),
			 "add: %d (%s)", ret,
			 ret == -ENOSPC ? "table full" : "cannot insert");
		mutex_unlock(&accel->lock);
		return ret;
	}
	if (ret == 1)
		accel->add_replace++;
	zx_ffe_key_print(accel->ffe_last, sizeof(accel->ffe_last), "add",
			 &key, eport, accel->n_flows);
	zx_ffe_nat_print(accel->ffe_last, sizeof(accel->ffe_last), &nat);
	if (ret == 1) {
		size_t n = strlen(accel->ffe_last);

		if (n < sizeof(accel->ffe_last))
			scnprintf(accel->ffe_last + n,
				  sizeof(accel->ffe_last) - n,
				  " (replaced: same key swapped in a new node)");
	}
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
		/*
		 * 骨架 10 个 token，之后每 3 个一组 nat/dnat（P3-TASK §0 A.2）：
		 * 10、13、16 合法，其余都是语法错。第一个 token 是 family，
		 * family 6 + nat 段在 zx_ffe_op_add() 里按 -EOPNOTSUPP 拒。
		 */
		if (argc < 10 || (argc - 10) % 3)
			return zx_ffe_bad_usage(adapter,
				"ffe_add <family> <saddr> <daddr> <sport> <dport> <proto> <ipport> <vid> <eport> <dmac> [nat <saddr_out> <sport_out>] [dnat <daddr_out> <dport_out>]");
		ret = zx_ffe_op_add(adapter, argv, argc);
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
 *   family saddr daddr sport dport proto ipport vid -> eport dmac hits bytes lastuse_ms [S] [SNAT=a:p] [DNAT=a:p]
 * （P1-TASK §2 的读格式；末尾的 S 表示静态条目，P1-TASK §3。）
 * family 4 的地址用 %pI4，family 6 用 %pI6c（P4-TASK §0.3），token 位置与含义
 * 两族一致。hits/bytes 只统计真正转发出去的帧（P2 起 == fwd）；lastuse 在
 * "转发"与"命中但环满回栈"两种情况下都刷新。读侧持
 * RCU 只读段，条目删除用 list_del_rcu + kfree_rcu。
 *
 * P3 的 NAT 标记（P3-TASK §0 A.5）：`SNAT=<saddr_out>:<sport_out>` 对应
 * ZX_FFE_FLOW_NAT_S、`DNAT=<daddr_out>:<dport_out>` 对应 ZX_FFE_FLOW_NAT_D，
 * 只在对应位有效时追加；纯路由条目（P2 形态）的行一点不变，老的解析脚本
 * 不受影响。字母用 SNAT/DNAT 而不单是 S/D：末尾那个 ` S` 从 P1 起就表示
 * "静态条目"，再让 S 表示 SNAT 会读错。
 *
 * P5 再加一个行尾标记：` L` = 旁路学习插进来的条目（两种标记互斥，静态条目
 * 不会被学习改写 flags）。位置在 lastuse 之后、SNAT/DNAT 之前，与 ` S` 并列。
 *
 * P5 修订二的两条不变式（上机核对时就看这两条）：
 *   1. **同一个 key 只会有一行**：所有落表路径（ffe_add、旁路学习）都过
 *      __zx_ffe_store()，同 key 时整体换上一条新节点而不是再插一条，所以
 *      这里不会出现两行同键（P4 验收曾见过）。
 *   2. **带 DNAT 的行是"pre-NAT 目的地址属本机"的那些流**（端口映射的
 *      ORIGINAL 侧、masquerade 的 REPLY 侧）：它们 pre-NAT 的 daddr 是盒子
 *      自己的 WAN IP，数据面靠 ZX_FFE_FLOW_NAT_D 这位放行——没有这一位的
 *      行，本机目的地的帧一律回栈（drop_local），见 zx_ffe_forward() #13'。
 *      ` S`/` L` 与 SNAT/DNAT 的组合都合法：学习条目也可能带 NAT 位。
 */
static int zx_accel_flows_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;
	struct zx_ffe_flow *flow;

	(void)data;
	rcu_read_lock();
	list_for_each_entry_rcu(flow, &accel->flow_list, list) {
		if (flow->key.family == 6) {
			seq_printf(s, "6 %pI6c %pI6c %u %u %u %u %u -> %u %pM %llu %llu %u%s%s",
				   (const struct in6_addr *)&flow->key.v6[0],
				   (const struct in6_addr *)
					&flow->key.v6[sizeof(struct in6_addr)],
				   ntohs(flow->key.sport), ntohs(flow->key.dport),
				   flow->key.proto, flow->key.ipport,
				   flow->key.vid, flow->egress_id, flow->dmac,
				   flow->hits, flow->bytes,
				   jiffies_to_msecs(jiffies - flow->lastuse),
				   (flow->flags & ZX_FFE_FLOW_STATIC) ? " S" : "",
				   (flow->flags & ZX_FFE_FLOW_LEARNED) ? " L" : "");
		} else {
			seq_printf(s, "4 %pI4 %pI4 %u %u %u %u %u -> %u %pM %llu %llu %u%s%s",
				   &flow->key.v4[0], &flow->key.v4[1],
				   ntohs(flow->key.sport), ntohs(flow->key.dport),
				   flow->key.proto, flow->key.ipport,
				   flow->key.vid, flow->egress_id, flow->dmac,
				   flow->hits, flow->bytes,
				   jiffies_to_msecs(jiffies - flow->lastuse),
				   (flow->flags & ZX_FFE_FLOW_STATIC) ? " S" : "",
				   (flow->flags & ZX_FFE_FLOW_LEARNED) ? " L" : "");
			/* NAT 后的输出字段（P3）：v6 没有 NAT66，只有 v4 行会有 */
			if (flow->flags & ZX_FFE_FLOW_NAT_S)
				seq_printf(s, " SNAT=%pI4:%u", &flow->saddr_out,
					   ntohs(flow->sport_out));
			if (flow->flags & ZX_FFE_FLOW_NAT_D)
				seq_printf(s, " DNAT=%pI4:%u", &flow->daddr_out,
					   ntohs(flow->dport_out));
		}
		seq_putc(s, '\n');
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

/* ---- 旁路学习的 debugfs 入口（P5-TASK §1.6/§1.8）------------------- */

/*
 * ffe_learn：既是开关也是读数（P5-TASK §1.6 二选一里的"单独 debugfs"）。
 *
 * 读：
 *   第 1 行  开关 0/1
 *   第 2 行  cand=.. add=.. dup=.. drop=.. noneigh=.. nobrport=.. nonport=..
 *            dedup=.. flows=.. tbl_full=.. add_replace=..
 *   第 3 行  每 CPU 环的占用（还没落表的候选数，逗号分隔）
 *   第 4 行  v4 cand=.. add=.. dup=.. noneigh=.. nobrport=.. nonport=..
 *            dedup=.. drop=..（P10.1 按族计数，口径与第 2 行相同）
 *   第 5 行  v6 同上，末尾多一个 nat6=（v6 带 NAT 位、进环前丢弃）
 *   第 6 行  auto_enable_in_ms=（P10.3 门控：还差多少毫秒自动开；0 = 已经
 *            自动开过、到点后还没轮到节拍、或用户写过 ffe_learn 已接管）
 * 写 0/1（也认 off/on）：写一次就取消 P10.3 的待开（用户意图优先）
 *   0 = 不再把新流写进环（环里剩的候选在下一拍清掉），已有条目照常工作到老化；
 *   1 = 开始学。**默认 0**：probe 期不学，开机窗口过去后由 1 s 节拍自动开
 *       一次（P10.3 门控，见文件上方 learn_delay_ms 的注释）；交付形态
 *       `accel_ipv6=ffe` 靠这次自动开拿到加速。
 *
 * 按族过滤（P10.2 复审整改 1）：学习只为喂 FFE，**该族必须在 ffe 模式才会学；
 * stack/hw/zc 下不产生任何 cand**（钩子在解析前就按 zx_accel_cur_mode() 返回，
 * 不计数、不入环）。交付形态 v4=hw 时因此只有 v6 会学，不会白占两族共用的 1024
 * 条表；把某族切到 ffe 后该族立即开始学，不需要改开关。
 *
 * 计数口径：learn_cand 是钩子"看到并投递"的候选数，learn_drop 是环满没投递
 * 的（所以 cand 不含 drop），learn_noneigh 是候选在 work 里取不到下一跳 MAC
 * 的，learn_dup 是同 key 已存在、新节点整体换上旧节点并继承 hits/bytes、
 * 刷新 lastuse 的（P5 修订二起不只是刷新 dmac/MTU；P10.2 F4 起不再原地改），
 * learn_add 才是真正
 * 新增的条目。learn_nobrport 是**出口**桥在
 * work 里解不出本驱动端口的（FDB 未命中/要泛洪/桥已不在），learn_nonport 是
 * "命中的不是交换机口"这类放弃——**两个方向都算**：出口 FDB 命中无线口等
 * （P5），以及 P9 起**入口**桥的源 MAC 反查失败/命中无线口（`非交换机口`这
 * 条线没变，只是入口方向也归到这里）。learn_dedup 是被 1 s 去重挡下的重复
 * 候选。这几种都是"主动放弃"，不是错误。
 *
 * add_replace 是 ffe_add 的同键替换次数（P5 修订二 §C）；它与学习无关，放在
 * 这一行只是因为 ffe_learn 是唯一常开的计数行（不做额外的 debugfs 文件）。
 * 一行里的 key=value 只做追加，不改已有 key 的名字与顺序。
 *
 * P10 起 cand/add/dup/drop/noneigh/nobrport/nonport/dedup 都是**两族合计**
 * （同一个钩子回调、同一个每 CPU 环、同一张流表）。P10.1 起新增按族计数
 * learn_cnt[2]，**只追加末尾两行**（v4/v6），前两行的字段名与顺序一个字都
 * 没动，这样 P7/P10 的行格式逐字比对仍能对上；debugfs 文件数也不变。
 * 按族计数解决的是"v6 cand 没涨但 v4 把合计掩盖了"这类上机误判。
 * 要分辨族还可以看 ffe_flows 里条目的地址形态（v4 点分/16 进制，v6 冒号
 * 分隔）与键的 family。
 */
static int zx_accel_learn_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;
	unsigned long arm_at;
	int cpu;

	(void)data;
	seq_printf(s, "%u\n", READ_ONCE(accel->learn_on) ? 1u : 0u);
	seq_printf(s, "cand=%llu add=%llu dup=%llu drop=%llu noneigh=%llu nobrport=%llu nonport=%llu dedup=%llu flows=%u tbl_full=%llu add_replace=%llu\n",
		   READ_ONCE(accel->learn_cand), READ_ONCE(accel->learn_add),
		   READ_ONCE(accel->learn_dup), READ_ONCE(accel->learn_drop),
		   READ_ONCE(accel->learn_noneigh),
		   READ_ONCE(accel->learn_nobrport),
		   READ_ONCE(accel->learn_nonport),
		   READ_ONCE(accel->learn_dedup), READ_ONCE(accel->n_flows),
		   READ_ONCE(accel->tbl_full), READ_ONCE(accel->add_replace));
	if (accel->learn_rings) {
		seq_puts(s, "ring=");
		for_each_possible_cpu(cpu) {
			struct zx_ffe_learn_ring *ring =
				per_cpu_ptr(accel->learn_rings, cpu);

			seq_printf(s, "%s%u", cpu ? "," : "",
				   READ_ONCE(ring->head) - READ_ONCE(ring->tail));
		}
		seq_putc(s, '\n');
	} else {
		seq_puts(s, "ring=n/a\n");
	}
	/*
	 * P10.1：追加按族两行。上面 3 行的字节不变（开关 / 两族合计 / 环占用），
	 * 这两行只是多给一组可以对照的读数：第 2 行里 8 个学习计数
	 * （cand/add/dup/noneigh/nobrport/nonport/dedup/drop）都等于 v4+v6 两格；
	 * flows/tbl_full/add_replace 是表级计数，不在按族行里。v6 的 nat6 也不在
	 * 合计口径里（它在进环之前就丢，见钩子的 B1 防护）。
	 */
	seq_printf(s, "v4 cand=%llu add=%llu dup=%llu noneigh=%llu nobrport=%llu nonport=%llu dedup=%llu drop=%llu\n",
		   READ_ONCE(accel->learn_cnt[0].cand),
		   READ_ONCE(accel->learn_cnt[0].add),
		   READ_ONCE(accel->learn_cnt[0].dup),
		   READ_ONCE(accel->learn_cnt[0].noneigh),
		   READ_ONCE(accel->learn_cnt[0].nobrport),
		   READ_ONCE(accel->learn_cnt[0].nonport),
		   READ_ONCE(accel->learn_cnt[0].dedup),
		   READ_ONCE(accel->learn_cnt[0].drop));
	seq_printf(s, "v6 cand=%llu add=%llu dup=%llu noneigh=%llu nobrport=%llu nonport=%llu dedup=%llu drop=%llu nat6=%llu\n",
		   READ_ONCE(accel->learn_cnt[1].cand),
		   READ_ONCE(accel->learn_cnt[1].add),
		   READ_ONCE(accel->learn_cnt[1].dup),
		   READ_ONCE(accel->learn_cnt[1].noneigh),
		   READ_ONCE(accel->learn_cnt[1].nobrport),
		   READ_ONCE(accel->learn_cnt[1].nonport),
		   READ_ONCE(accel->learn_cnt[1].dedup),
		   READ_ONCE(accel->learn_cnt[1].drop),
		   READ_ONCE(accel->learn_cnt[1].nat6));
	/*
	 * P10.3 门控状态，追加在最后一行：已有 key 名与顺序一个字都没动。
	 * 0 表示"已经自动开过"（或到点后还没轮到节拍），也表示"用户写过
	 * ffe_learn，已接管"；与第 1 行的开关一起读就分得清。
	 */
	arm_at = READ_ONCE(accel->learn_arm_at);
	if (arm_at && time_before(jiffies, arm_at))
		seq_printf(s, "auto_enable_in_ms=%u\n",
			   jiffies_to_msecs(arm_at - jiffies));
	else
		seq_printf(s, "auto_enable_in_ms=0\n");
	return 0;
}

static int zx_accel_learn_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_learn_show, inode->i_private);
}

static ssize_t zx_accel_learn_store(struct file *file, const char __user *ubuf,
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

	/* 只在写的那一刻切；钩子与 work 都走 READ_ONCE 读这一个字节 */
	if (!strcmp(cmd, "1") || !strcmp(cmd, "on"))
		WRITE_ONCE(adapter->accel.learn_on, 1);
	else if (!strcmp(cmd, "0") || !strcmp(cmd, "off"))
		WRITE_ONCE(adapter->accel.learn_on, 0);
	else
		return -EINVAL;

	/* P10.3：手动写就是最终决定，取消待开 */
	WRITE_ONCE(adapter->accel.learn_arm_at, 0);

	dev_info(adapter->dev, "accel: ffe_learn %s\n",
		 READ_ONCE(adapter->accel.learn_on) ? "enabled" : "disabled");
	return count;
}

const struct file_operations zx_accel_learn_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_learn_open,
	.read		= seq_read,
	.write		= zx_accel_learn_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- hw→ffe 迁移桥（P3-TASK §0 B，最小可用版）---------------------- */

/*
 * ffe_migrate：写 1 取一个"PPE 当前绑着哪些流"的快照，并把结果打到
 * ffe_last（读回）与 dmesg。
 *
 * **P3 只做到统计与打印，没有真正把条目灌进 FFE 表**，原因是取数接口不在
 * 核心驱动这一侧：
 *   - 核心驱动只留下 adapter->ppe_flows（键是 fw4 的 cookie，值只有
 *     hash/flow/nh/subnet 这 4 个 16 位索引，见 zte/zx279128s-ppe.h 的
 *     struct zx_ppe_entry）；
 *   - 每个条目的五元组、NAT 后地址、出口端口、下一跳 MAC 是
 *     kmod-e2631-ppe 在 flow_cb（zx_ppe_flow_add）里解析完就丢掉的局部变量
 *     （struct zx_ppe_tuple / zx_ppe_rewrite），既没写进 ppe_entry，也没有
 *     暴露给核心驱动的回调。
 *   - 要完整镜像，二选一：给 zx_ppe_entry 加五元组字段（改 PPE 模块 + 它
 *     自己的 commit），或在 struct zx_ppe_hooks 加一条 "dump 一条 flow" 的
 *     回调、由模块暂存 tuple（同样要改模块，还要处理模块卸载与 peek 的竞态）。
 *   两条都超出"最小可用版"，留到 P5。P3-TASK §0 B 允许先只做统计并要求在
 *   报告里说明，这里按那条走。
 *
 * 现在这份快照仍有两个实际用途：
 *   1. 上机验收时拿 ppe_flows 跟 `conntrack -L` 里 hardware-offloaded 的行数
 *      对账（P3-TASK §2 判据 5 的前半）；
 *   2. 快照记进 accel->mig_ppe_flows，v4 从 hw 切到 ffe 时提示"上次 PPE 有
 *      多少条、要你手工搬多少条"（zx_accel_set() 的 S7）。
 *
 * 用法与时机：**必须趁 PPE 还绑着 flowtable 时写**。先关 hardware offload 再
 * 切 ffe 的话，条目已经被 DESTROY 回调删掉，这里只会看到 0——那时候能取的
 * 五元组只剩 `conntrack -L` 里的 conntrack 表项（用户态拿），这也正是目前
 * 上机验收的做法。
 *
 * ppe_flows 的所有增删都在 adapter->ppe_lock 下（trampoline 一圈都持着它），
 * 所以这里持同一把锁取 nelems 与样例 cookie；不取 rtnl 读表，只在判断
 * "有没有绑 flowtable" 时短占 rtnl（与 accel_show 的做法一致）。
 */
static ssize_t zx_accel_migrate_store(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;
	struct rhashtable_iter iter;
	unsigned long cookie[4];
	struct zx_ppe_entry *e;
	u32 n_ppe, n_ffe;
	bool bound;
	unsigned int val;
	char sample[64];
	size_t len = 0;
	int ret, i, n = 0;

	ret = kstrtouint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;
	if (val != 1)
		return -EINVAL;

	/* "有没有东西在喂 PPE" 只能问 TC 的 flow_block 列表（要 rtnl） */
	rtnl_lock();
	bound = zx_ppe_has_bound_blocks(adapter);
	rtnl_unlock();

	mutex_lock(&adapter->ppe_lock);
	n_ppe = atomic_read(&adapter->ppe_flows.nelems);
	rhashtable_walk_enter(&adapter->ppe_flows, &iter);
	rhashtable_walk_start(&iter);
	while (n < ARRAY_SIZE(cookie)) {
		e = rhashtable_walk_next(&iter);
		if (!e || IS_ERR(e))
			/* 没有条目 / 表正在扩容（这里持着 ppe_lock，不该发生） */
			break;
		cookie[n++] = e->cookie;
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	WRITE_ONCE(accel->mig_ppe_flows, n_ppe);
	mutex_unlock(&adapter->ppe_lock);

	sample[0] = 0;
	for (i = 0; i < n; i++)
		len += scnprintf(sample + len, sizeof(sample) - len, " %#lx",
				 cookie[i]);

	mutex_lock(&accel->lock);
	n_ffe = accel->n_flows;
	snprintf(accel->ffe_last, sizeof(accel->ffe_last),
		 "migrate: ppe_bound=%u ppe_flows=%u ffe_flows=%u mirrored=0 (PPE tuple export not implemented, P5) cookies:%s",
		 bound ? 1u : 0u, n_ppe, n_ffe, sample);
	mutex_unlock(&accel->lock);

	dev_info(adapter->dev,
		 "accel: ffe_migrate snapshot: ppe_bound=%u ppe_flows=%u ffe_flows=%u mirrored=0 (needs a PPE hook to export tuples, planned for P5)\n",
		 bound ? 1u : 0u, n_ppe, n_ffe);
	return count;
}

static int zx_accel_migrate_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	struct zx_accel *accel = &adapter->accel;

	(void)data;
	mutex_lock(&accel->lock);
	if (accel->ffe_last[0])
		seq_printf(s, "%s\n", accel->ffe_last);
	else
		seq_puts(s, "(no ffe_migrate yet)\n");
	mutex_unlock(&accel->lock);
	return 0;
}

static int zx_accel_migrate_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_migrate_show, inode->i_private);
}

const struct file_operations zx_accel_migrate_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_migrate_open,
	.read		= seq_read,
	.write		= zx_accel_migrate_store,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- SoC 温度（debugfs soc_temp，只读）-------------------------------- */

/*
 * 传感器在 0x94100034：原厂 DTB soc/temp@94100034 { compatible =
 * "zxic,tempsensor"; reg = <0x94100034 0x0c>; }，原厂驱动是
 * drivers/tempsensor/temp_zx.c，用户态口 /proc/tempsensor。
 *
 * ★ 下面每一条寄存器语义都**由原厂 vmlinux 反汇编得出**，不是猜的，也不是
 *   照抄哪个文档。取证对象：工作文件/out/scratch/npu/vendor-vmlinux.elf
 *   （未 strip，nm 有全符号）。复现命令必须带 -m armv7：
 *     arm-openwrt-linux-objdump -m armv7 -d --start-address=0xc02ad1d8 \
 *         --stop-address=0xc02ad3c8 vendor-vmlinux.elf
 *   不带 -m armv7 时这份 binutils 按老 ARM 解码：movw/movt 显示成 tst/cmp、
 *   blx r3 显示成 msr SP_hyp，照着它读会得出完全错误的结论。
 *
 * 关键符号（vmlinux VMA，nm 确认）：
 *   temp_ctrl_read_zxic 0xc02ad1d8  "触发 + 采样 + 换算"的主体（static）
 *   temp_ctrl_read      0xc02ad3f0  EXPORT_SYMBOL，mode=0：100°C 拐点 + 110°C 钳位
 *   temp_ctrl_read_real 0xc02ad440  EXPORT_SYMBOL，mode=1：不拐点不钳位
 *   bsp_temp_start      0xc02add98  上电使能时序（zx_temp_probe 调）
 *   zx_temp_probe       0xc02ade30  devm_ioremap_resource + /proc + 3 个 IRQ
 *   temp_ctrl_read_proc 0xc02ad528  /proc 读：只回显缓存值，**不触发转换**
 *   g_dout_latch        0xc06757b0  平均后的原始值（/proc 里叫 dout_latch）
 *   g_temper            0xc06757b4  换算后的摄氏温度
 *   arm_delay_ops       0xc064bee4  每一步延时走它 [+4] = const_udelay
 *
 * 寄存器表（下称 base = ioremap 后的 0x94100034；原厂存在
 * zx_bsp_tempsensor+0x2c，printk 里明确写的就是 &(regs->cfg)，即 cfg 在 +0）：
 *   base+0x00 0x94100034  cfg      bit12/bit13 = 转换触发对；bit16/17 = detector 选择
 *   base+0x04 0x94100038  status   bit12 = 就绪位；低 10 bit = ADC 原始值
 *   base+0x08 0x9410003c  det      未命名；原厂 probe 一次性整写 0x3FFFFFFF
 *
 * 原厂读一次温度的完整时序（c02ad1d8 起）。每一步之后都调
 * arm_delay_ops.const_udelay，参数 r0 = 0x0010624C（movw #0x624c / movt #0x10）；
 * 按 ARM 的 __udelay(usec) = __const_udelay(usec * 0x10C7) 折算 ≈ 250 µs：
 *   ① 死等 status.bit12 置位（tst #0x1000 / beq 回跳）——原厂**没有超时**；
 *   ② 循环（有效样本满 10 个退出，累计迭代上限 6000 = 0x1770）：
 *        cfg 清 bit13（bic #0x2000）→ 置 bit12（orr #0x1000）→
 *        清 bit12（bic #0x1000）→ 置 bit13（orr #0x2000），每步之间延时；
 *      四步做完才读 status 的低 10 bit（ubfx r3,r3,#0,#10），**非 0 才计一个
 *      有效样本**（cmp r3,#0 / addne），0 直接丢掉重来；
 *   ③ 有效样本求和后除以 10（0xCCCCCCCD 魔数 >>35，恒除 10，不是除实际样本数）；
 *   ④ t = (avg * 1550) >> 10（定点，斜率 1550/1024 ≈ 1.5137 °C/LSB，全整数），
 *      再按四段线性校正，最后 mode=0 时"t>100 斜率减半 + 钳到 110"。
 *      完整公式见 zx_soc_temp_convert() 的注释。
 * 原厂这条路径**恒返回 0**：轮询撞到 6000 次只 printk
 * "tempsensor get status fail!" 然后拿残缺累加值继续算。我们不复刻这个——
 * 一个有效样本都没有就报错，绝不在残值上继续算温度（会误导散热判断）。
 *
 * 输出格式（第 1 行给脚本，第 2 行给人/给日志）：
 *   temp_c=<摄氏整数>
 *   raw=<平均原始值> samples=<有效样本数> t=<中间值>
 */

/* 0x94100034 不在以太网那块（0x92000000）里，原厂 DTB 的 reg 长度又只有 0xc，
 * 所以按页映射 0x94100000 一页、再把指针挪到 0x34。原厂用
 * devm_ioremap_resource()，且 probe 里**没有任何 clock / reset 框架调用**
 * （没有 clk_get、clk_prepare_enable，也没有 reset_control_*）——这一页的
 * 时钟按常开处理，不需要我们先开 gate。 */
#define ZX_SOC_TEMP_PAGE	0x94100000u
#define ZX_SOC_TEMP_PAGE_SIZE	0x1000u
#define ZX_SOC_TEMP_REG_OFF	0x34u

#define ZX_SOC_TEMP_CFG		0x00	/* 0x94100034 */
#define ZX_SOC_TEMP_STATUS	0x04	/* 0x94100038 */
#define ZX_SOC_TEMP_DET		0x08	/* 0x9410003c */
#define  ZX_SOC_TEMP_TRIG12	BIT(12)	/* cfg：触发相位 A（置 1 → 清 0） */
#define  ZX_SOC_TEMP_TRIG13	BIT(13)	/* cfg：触发相位 B（清 0 → 置 1，收尾保持置位） */
#define  ZX_SOC_TEMP_DET_ALL	0x3FFFFFFFu	/* 原厂 probe 写死在 base+8 */
#define  ZX_SOC_TEMP_RDY	BIT(12)	/* status：就绪位（同名不同寄存器，语义不通用） */
#define  ZX_SOC_TEMP_DATA	0x3FFu	/* status[9:0] = 原始 ADC 值 */

/* 原厂每步 const_udelay(0x10624C) ≈ 250 µs。这里用"睡"而不是忙等：debugfs read
 * 是进程上下文，usleep_range 不烧 CPU。上限给 2 倍，宁可慢一点也要 ≥250 µs。 */
#define ZX_SOC_TEMP_STEP_US	250u
#define ZX_SOC_TEMP_NSAMPLES	10	/* 原厂取 10 个有效样本平均 */
#define ZX_SOC_TEMP_ATTEMPTS	(ZX_SOC_TEMP_NSAMPLES * 3)	/* 允许 3 倍重试余量 */
#define ZX_SOC_TEMP_POLL_US	10u
#define ZX_SOC_TEMP_RDY_POLLS	100	/* 就绪位轮询：100 × 10 µs = 1 ms */
#define ZX_SOC_TEMP_BUDGET_US	40000u	/* 整次读取的墙钟上限 40 ms */

/* 传感器是 SoC 的一份全局硬件，但映射的生命周期挂在 adapter->dev 上
 * (devm_ioremap)，所以指针放在 adapter 里、锁放文件级：这把锁只串行化
 * "懒初始化 + 触发采样"这段硬件操作，防止两个进程同时读 soc_temp 时交错写 cfg。
 * 都是进程上下文，用 mutex（采样期间要 sleep）。 */
static DEFINE_MUTEX(zx_soc_temp_lock);

/* 懒映射。失败只影响 soc_temp 这一个文件，与数据面无关。 */
static int zx_soc_temp_map(struct zx_eth_adapter *adapter)
{
	void __iomem *p;

	if (adapter->soc_temp_base)
		return 0;

	p = devm_ioremap(adapter->dev, ZX_SOC_TEMP_PAGE,
			 ZX_SOC_TEMP_PAGE_SIZE);
	if (!p)
		return -ENOMEM;

	adapter->soc_temp_base = p + ZX_SOC_TEMP_REG_OFF;
	return 0;
}

/* cfg 的写一律读-改-写，**只动 bit12/bit13**：实测空闲值是 0x04001000
 * （bit12 已置位、bit26 置位），bit16/17 是 detector 选择，整写会把它们踩掉。 */
static void zx_soc_temp_cfg_set(void __iomem *base, u32 bits)
{
	writel(readl(base + ZX_SOC_TEMP_CFG) | bits, base + ZX_SOC_TEMP_CFG);
}

static void zx_soc_temp_cfg_clear(void __iomem *base, u32 bits)
{
	writel(readl(base + ZX_SOC_TEMP_CFG) & ~bits, base + ZX_SOC_TEMP_CFG);
}

static void zx_soc_temp_step_delay(void)
{
	usleep_range(ZX_SOC_TEMP_STEP_US, ZX_SOC_TEMP_STEP_US * 2);
}

/*
 * 原厂 ①：死等 status.bit12。我们最多等 1 ms 就放弃，而且**放弃不算失败**——
 * 没有被驱动启动过的机器上这一位可能恒 0（2026-11-01 取证时 status 实测就是 0），
 * 把它写成硬前置条件会让这个文件永远报错。真正的判据在采样循环里：一个有效
 * 样本都拿不到才返回 -ETIMEDOUT。
 */
static void zx_soc_temp_wait_ready(void __iomem *base)
{
	int i;

	for (i = 0; i < ZX_SOC_TEMP_RDY_POLLS; i++) {
		if (readl(base + ZX_SOC_TEMP_STATUS) & ZX_SOC_TEMP_RDY)
			return;
		udelay(ZX_SOC_TEMP_POLL_US);	/* 总共 1 ms 的短忙等，可接受 */
	}
}

/*
 * 一次性把硬件带到"能出数据"的状态，等价于原厂 zx_temp_probe + bsp_temp_start：
 *   (a) writel(0x3FFFFFFF, base+8)          原厂 probe 整写；这里 |=，保留 bit30/31
 *   (b) cfg 清 bit12 → 延时 → cfg 置 bit13  原厂 bsp_temp_start 的顺序
 *   (c) 等 status[9:0] != 0                 原厂在这里也是死等，我们 1 ms 放弃
 * 只在第一次读 soc_temp 时执行：**probe/init 一律不碰传感器**。
 */
static void zx_soc_temp_hw_init(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->soc_temp_base;
	int i;

	writel(readl(base + ZX_SOC_TEMP_DET) | ZX_SOC_TEMP_DET_ALL,
	       base + ZX_SOC_TEMP_DET);

	zx_soc_temp_cfg_clear(base, ZX_SOC_TEMP_TRIG12);
	zx_soc_temp_step_delay();
	zx_soc_temp_cfg_set(base, ZX_SOC_TEMP_TRIG13);

	for (i = 0; i < ZX_SOC_TEMP_RDY_POLLS; i++) {
		if (readl(base + ZX_SOC_TEMP_STATUS) & ZX_SOC_TEMP_DATA)
			break;
		udelay(ZX_SOC_TEMP_POLL_US);
	}
}

/*
 * 原始值 → 摄氏度，逐条对应原厂机器码（c02ad2e8 - c02ad3b4）：
 *
 *   t = (avg * 1550) >> 10        ; mul r3,r3,#1550 ; lsr r3,#10
 *
 *   (u32)(t-1139) <= 9   即 t∈[1139,1148]：out = (t-1104) - ((t-1139) >> 1)
 *   (u32)(t-1149) <= 19  即 t∈[1149,1168]：out = (t-1109) - ((t-1149) >> 2)
 *   (t-1104) > 64        即 t > 1168      ：out = t - 1113
 *   其余（实际就是 t <= 1138）            ：out = t - 1104
 *
 * 四段在 1139/1148/1149/1168 处连续（35/40/40/55 °C），是厂家贴的分段线性
 * 校正，不是抄错。之后 mode=0（temp_ctrl_read，GetCputemperature 走的就是它）：
 *   if (out > 100) out = 100 + (out - 100) / 2;   过 100°C 斜率减半
 *   if (out > 110) out = 110;                      硬钳位
 * 返回的是**钳位前**的值，钳位留给调用方，好让合理性校验看到真实值。
 */
static s32 zx_soc_temp_convert(u32 avg, u32 *t_out)
{
	s32 t = (s32)((avg * 1550u) >> 10);
	s32 out;

	if ((u32)(t - 1139) <= 9u) {
		out = (t - 1104) - ((t - 1139) >> 1);
	} else if ((u32)(t - 1149) <= 19u) {
		/* 到这里的 t 正常都 ≥1149；t<1149 那支是原厂 submi 的补丁路径 */
		s32 adj = (t < 1149) ? (t - 1146) : (t - 1149);

		out = (t - 1109) - (adj >> 2);
	} else if ((t - 1104) > 64) {
		out = t - 1113;
	} else {
		out = t - 1104;
	}

	*t_out = (u32)t;
	return out;
}

/*
 * 采样。失败一律只返回 errno：不 panic、不打印刷屏、不碰任何加速状态，
 * 数据面完全不受影响（这个文件是纯旁路观测）。
 * 所有轮询都有上限：单次就绪 1 ms，整次读取 40 ms（ktime 兜底），
 * 采样最多 30 轮（每轮 4 × ~250 µs 睡眠），远低于原厂的 6000 轮死循环。
 */
static int zx_soc_temp_sample(struct zx_eth_adapter *adapter, u32 *avg_out,
			      u32 *n_out)
{
	void __iomem *base = adapter->soc_temp_base;
	ktime_t deadline = ktime_add_us(ktime_get(), ZX_SOC_TEMP_BUDGET_US);
	u32 sum = 0;
	int n = 0;
	int tries = 0;

	zx_soc_temp_wait_ready(base);

	while (n < ZX_SOC_TEMP_NSAMPLES && tries < ZX_SOC_TEMP_ATTEMPTS) {
		u32 raw;

		if (ktime_after(ktime_get(), deadline))
			break;
		tries++;

		/* 原厂 ② 的四相位，顺序不能换：清 bit13 → 置 bit12 → 清 bit12
		 * → 置 bit13，每步之间等 ~250 µs，采样在四步**之后**。 */
		zx_soc_temp_cfg_clear(base, ZX_SOC_TEMP_TRIG13);
		zx_soc_temp_step_delay();
		zx_soc_temp_cfg_set(base, ZX_SOC_TEMP_TRIG12);
		zx_soc_temp_step_delay();
		zx_soc_temp_cfg_clear(base, ZX_SOC_TEMP_TRIG12);
		zx_soc_temp_step_delay();
		zx_soc_temp_cfg_set(base, ZX_SOC_TEMP_TRIG13);
		zx_soc_temp_step_delay();

		raw = readl(base + ZX_SOC_TEMP_STATUS) & ZX_SOC_TEMP_DATA;
		if (!raw)
			continue;	/* 原厂同样跳过 0（cmp/addne） */

		sum += raw;
		n++;
	}

	if (!n)
		return -ETIMEDOUT;

	/* 原厂恒除 10；这里除实际有效样本数（n 正常也是 10，超时时会 <10，
	 * 那样求平均才不会被 0 拉低）。 */
	*avg_out = sum / (u32)n;
	*n_out = (u32)n;
	return 0;
}

static int zx_accel_soc_temp_show(struct seq_file *s, void *data)
{
	struct zx_eth_adapter *adapter = s->private;
	u32 avg = 0, n = 0, t = 0;
	s32 out;
	int ret;

	(void)data;

	mutex_lock(&zx_soc_temp_lock);

	ret = zx_soc_temp_map(adapter);
	if (ret)
		goto out;

	if (!adapter->soc_temp_inited) {
		zx_soc_temp_hw_init(adapter);
		adapter->soc_temp_inited = true;
	}

	ret = zx_soc_temp_sample(adapter, &avg, &n);
	if (ret)
		goto out;

	out = zx_soc_temp_convert(avg, &t);

	/* 合理性校验：先看**钳位前**的值。−40..150 °C 之外说明传感器/时序
	 * 不对（例如 status 卡在某值、或 bit13 的假设是错的），直接报 -ERANGE，
	 * 不把这个数交给任何人当温度用，并且每个速率上限只告警一次（不刷屏）。 */
	if (out < -40 || out > 150) {
		dev_warn_ratelimited(adapter->dev,
				     "soc_temp: 读数超出 -40..150 °C：temp=%d raw=%u n=%u t=%u，拒绝上报\n",
				     out, avg, n, t);
		ret = -ERANGE;
		goto out;
	}

	/* 原厂 mode=0 的收尾（temp_ctrl_read）：过 100 °C 斜率减半，再钳到 110。 */
	if (out > 100)
		out = 100 + (out - 100) / 2;
	if (out > 110)
		out = 110;

	seq_printf(s, "temp_c=%d\n", out);
	seq_printf(s, "raw=%u samples=%u t=%u\n", avg, n, t);

out:
	mutex_unlock(&zx_soc_temp_lock);
	return ret;
}

static int zx_accel_soc_temp_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_accel_soc_temp_show, inode->i_private);
}

/* 只读：没有 .write，权限 0444（见 zx279128s-main.c 的 debugfs 注册）。
 * 每次 cat 都会真的去触发一次转换——硬件只在读的时候碰。 */
const struct file_operations zx_accel_soc_temp_fops = {
	.owner		= THIS_MODULE,
	.open		= zx_accel_soc_temp_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
