/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zx279128s-pppoe.h — PPPoE over FFE：会话记录与纯函数探针（M1）
 *
 * M1 只交付「能在宿主上单测的判定逻辑 + 会话结构 + 三个默认关闭的模块参数」，
 * **没有任何调用点**：accel.c 只 #include 本文件、在文件末尾注册参数，报文路径
 * 一条指令都不变（逐函数反汇编比对见
 * .scratch/ffe-fastpath/pppoe-m1/20261102-0400/RESULT.md）。
 *
 * 两条硬约束（负责人拍板，改之前先看 RESULT.md 的验收判据）：
 *
 *   1. struct zx_ffe_key（accel.c 里定义，sizeof == 44）**一个字都不许改**。
 *      PPPoE 的会话信息（sid / generation）只进本文件的 struct zx_pppoe_sess，
 *      不参与键与哈希；accel.c 末尾的 M1 hunk 用 _Static_assert 在每次内核
 *      编译时强制 sizeof == 44（宿主单测看不到内核 TU，见 test/key_size_check.sh）。
 *
 *   2. 本头文件必须能**脱离内核头编译**：内核态只借 <linux/types.h>，用户态
 *      自带 stdint typedef；探针只用移位/比较，不用 ntohs()/__be16，保证同一份
 *      代码在内核与宿主上语义逐字节相同。
 *
 * 帧格式（RFC 1661/2516，长度单位都是字节）：
 *
 *   +-----------+-----------+--------------------------+---------+----------+
 *   | Eth 14 B  | PPPoE 6 B | PPP 协议 1 B(PFC) 或 2 B | IP 头   | payload  |
 *   +-----------+-----------+--------------------------+---------+----------+
 *               ^           ^                          ^
 *               |           |                          |
 *               |           |                          +-- l3_off = 6 + proto_len
 *               |           +-- 14 + 6 = 20
 *               +-- frame[12..13] = 0x8864（会话）/ 0x8863（发现）
 *
 *   PPPoE 头：ver/type(1) = 0x11、code(1) = 0x00（会话）、sid(2, BE)、
 *             length(2, BE) = PPP 协议字段长度 + IP 报文长度（**不含**自己这 6 B）。
 *   PFC（协议字段压缩）：PPP 协议都是奇数，压缩后只剩最后一个字节。所以
 *   frame[20] 的最低位为 1 ⇒ 1 B 协议字段（0x21 = IPv4、0x57 = IPv6），否则
 *   按 2 B 大端读（0x0021 / 0x0057）。
 */

#ifndef _ZX279128S_PPPOE_H
#define _ZX279128S_PPPOE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else /* 宿主单测：不引入任何内核头 */
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* ---- 协议常量（只用宏，不引内核头里的定义）------------------------- */

#define ZX_PPPOE_ETH_HLEN		14	/* 以太网头 */
#define ZX_PPPOE_HDR_LEN		6	/* PPPoE 头 */
#define ZX_PPPOE_MIN_IP			20	/* 最小 IP 头（v4 无选项 / v6 固定头的前 20 B 下界） */

#define ZX_PPPOE_ETH_P_SESSION		0x8864	/* 发现阶段结束后所有会话帧 */
#define ZX_PPPOE_ETH_P_DISCOVERY	0x8863	/* PADI/PADO/PADR/PADS/PADT */
#define ZX_PPPOE_VER_TYPE		0x11	/* version 1 + type 1，会话与发现都必须 */
#define ZX_PPPOE_CODE_SESS		0x00	/* 会话数据 */
#define ZX_PPPOE_CODE_PADT		0xa7	/* 会话终止 */

#define ZX_PPPOE_PROTO_IPV4		0x0021	/* 2 B 形式 */
#define ZX_PPPOE_PROTO_IPV6		0x0057	/* 2 B 形式 */
#define ZX_PPPOE_PROTO_IPV4_PFC		0x21	/* 1 B 形式（PFC） */
#define ZX_PPPOE_PROTO_IPV6_PFC		0x57	/* 1 B 形式（PFC） */

/* ---- 会话记录（M2 才有人写它；M1 只定义布局）----------------------- */

/*
 * 一条 PPPoE 会话的现场。字段口径：
 *   valid       会话已学到且未被 PADT/sid 变化作废
 *   pfc         该会话是否压缩 PPP 协议字段（决定 l3_off 是 7 还是 8）
 *   wan_port    WAN 侧端口号（学到的，或 pppoe_wan_port 参数钉住的）
 *   sid         会话号（0 无效）
 *   ppp_ifindex ppp0 的 ifindex（M9a 识别出口用）
 *   our_mac     本端 MAC；peer_mac 对端 MAC（**从帧学，不从 dev_addr 读**）
 *   generation  代会话号：sid 变化 / valid 0→1 / PADT 时 ++，旧 flow 靠它
 *               在命中前比对（P0-4），避免"重拨后旧条目继续改写新会话"
 *   seen/bad/padt  观测计数（M6 的 pppoe_seen 从这里取）
 *
 * M2 会在本结构上再加一个写侧序列号（seqlock），字段**只追加**，不动已有字段
 * 的顺序与名字——那时会同步改本注释与 M2 的验收，不回头改 M1 的向量。
 */
struct zx_pppoe_sess {
	u8	valid;
	u8	pfc;
	u16	wan_port;
	u16	sid;
	u32	ppp_ifindex;
	u8	our_mac[6];
	u8	peer_mac[6];
	u32	generation;
	u64	seen;
	u64	bad;
	u64	padt;
};

/*
 * 探针的分类结果。l3_off 相对**PPPoE 头起点**（即 frame + 14），值 7 或 8；
 * family 是内层 IP 版本（4/6），PADT/NONE 时为 0。调用方按
 * frame + ZX_PPPOE_ETH_HLEN + l3_off 取内层 IP 头。
 */
struct zx_pppoe_hdr {
	u16	sid;
	u8	family;
	u8	l3_off;
	u8	pfc;
};

enum zx_pppoe_kind {
	ZX_PPPOE_NONE = 0,	/* 不是本探针认的 PPPoE 会话/终止帧 */
	ZX_PPPOE_SESS,		/* 0x8864：可加速的 IPv4/IPv6 会话帧 */
	ZX_PPPOE_PADT,		/* 0x8863 + code 0xa7：会话终止 */
};

/* 读大端 16 位。写成移位而不是 ntohs()，好让宿主单测与内核走同一份代码。 */
static inline u16 zx_pppoe_get_be16(const u8 *p)
{
	return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

/*
 * zx_pppoe_probe() — 纯函数：判定一帧是不是 PPPoE 会话帧/终止帧。
 *
 * 参数：
 *   frame  指向以太网帧首字节；**必须非 NULL**
 *   len    从帧首算起的可用长度（可以是描述符里的大缓冲，只要 >= 帧长）
 *   out    非 NULL；SESS/PADT 时写入 sid/family/l3_off/pfc，NONE 时不写
 *          （全部分支都先算局部量，NONE 返回前不会留下半截结果）
 *
 * 返回：ZX_PPPOE_SESS / ZX_PPPOE_PADT / ZX_PPPOE_NONE。
 *
 * 判据顺序（每一条都对应 test/probe_selftest.c 里的向量）：
 *   1. len >= 14 + 6；
 *   2. frame[12..13] == 0x8864/0x8863（VLAN 打头 0x8100 直接 NONE）；
 *   3. ver/type == 0x11；会话 code == 0x00，发现只认 PADT 的 0xa7；
 *   4. 会话 sid != 0；
 *   5. length 字段（PPP 协议 + IP）不越过 len；至少 1 B（要读 PFC 判别位）；
 *   6. PPP 协议：PFC 1 B 或 2 B，只认 0x21/0x0021 与 0x57/0x0057；
 *   7. plen >= proto_len + 20（P1-7：1 B proto + 20 B IP 的合法 PFC 小包
 *      不能被误拒，所以下界不是 2 + 20）；
 *   8. 内层 version 必须与 PPP 协议一致（0x0021 ⇒ 4，0x0057 ⇒ 6）；
 *   9. l3_off = 6 + proto_len（2 B 协议 ⇒ 8，PFC ⇒ 7）。
 *
 * 不做的事（留给调用方/M2）：不校验 IP 头长度、不校验 MAC、不看 VLAN、
 * 不读写任何全局状态。所以它是可重入的，也可以在任意上下文里调用。
 */
static inline enum zx_pppoe_kind
zx_pppoe_probe(const u8 *frame, u32 len, struct zx_pppoe_hdr *out)
{
	u32 plen, proto_len;
	u16 ethertype, sid, proto;
	u8 ver, code, first, family, pfc;

	if (len < ZX_PPPOE_ETH_HLEN + ZX_PPPOE_HDR_LEN)
		return ZX_PPPOE_NONE;

	ethertype = zx_pppoe_get_be16(frame + 12);
	ver = frame[14];
	code = frame[15];

	/*
	 * 发现阶段：PADI(0x09)/PADO(0x07)/PADR(0x19)/PADS(0x65) 都不归快路管，
	 * 只有 PADT 要拿去作废会话（M2：generation++ 并排全清）。PADT 的
	 * length 字段允许为 0（RFC 2516 的 PADT 一般不带 tag），所以这里不查
	 * plen，只要求 6 B PPPoE 头完整（由上面的 len >= 20 保证）。
	 */
	if (ethertype == ZX_PPPOE_ETH_P_DISCOVERY) {
		if (ver != ZX_PPPOE_VER_TYPE || code != ZX_PPPOE_CODE_PADT)
			return ZX_PPPOE_NONE;
		out->sid = zx_pppoe_get_be16(frame + 16);
		out->family = 0;
		out->l3_off = 0;
		out->pfc = 0;
		return ZX_PPPOE_PADT;
	}

	if (ethertype != ZX_PPPOE_ETH_P_SESSION)
		return ZX_PPPOE_NONE;
	if (ver != ZX_PPPOE_VER_TYPE || code != ZX_PPPOE_CODE_SESS)
		return ZX_PPPOE_NONE;

	sid = zx_pppoe_get_be16(frame + 16);
	if (!sid)
		return ZX_PPPOE_NONE;

	plen = zx_pppoe_get_be16(frame + 18);
	/* 会话帧的 length 字段不允许越界；20 = 14 B 以太头 + 6 B PPPoE 头 */
	if (ZX_PPPOE_ETH_HLEN + ZX_PPPOE_HDR_LEN + plen > len)
		return ZX_PPPOE_NONE;
	/* 连 1 B PPP 协议字段都没有：下面要读 frame[20] 判 PFC，先挡掉 */
	if (plen < 1)
		return ZX_PPPOE_NONE;

	first = frame[20];
	if (first & 0x01) {
		proto_len = 1;
		pfc = 1;
		proto = first;
	} else {
		proto_len = 2;
		pfc = 0;
		/* !pfc 时 proto_len + 20 == 22 > plen，由第 7 条兜住，不会越界读 */
		proto = zx_pppoe_get_be16(frame + 20);
	}

	if (proto != ZX_PPPOE_PROTO_IPV4 && proto != ZX_PPPOE_PROTO_IPV6)
		return ZX_PPPOE_NONE;

	if (plen < proto_len + ZX_PPPOE_MIN_IP)
		return ZX_PPPOE_NONE;

	/*
	 * 内层 version 与 PPP 协议必须一致，否则按"不是我们认识的帧"处理。
	 * v4/v6 共用一个 return：既少一条分支，也让"内层 version 错"这条判据
	 * 在 18 条向量里只需要一条负例就能覆盖（见 test/probe_selftest.c）。
	 */
	family = (proto == ZX_PPPOE_PROTO_IPV4) ? 4 : 6;
	if ((frame[ZX_PPPOE_ETH_HLEN + ZX_PPPOE_HDR_LEN + proto_len] >> 4) != family)
		return ZX_PPPOE_NONE;

	out->sid = sid;
	out->family = family;
	out->l3_off = (u8)(ZX_PPPOE_HDR_LEN + proto_len);
	out->pfc = pfc;

	return ZX_PPPOE_SESS;
}

#endif /* _ZX279128S_PPPOE_H */
