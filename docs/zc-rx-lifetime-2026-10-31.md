# 零拷贝 RX 的数据损坏（本地终结）2026-10-31

> 对象：ZXHN E2631，slot1 serial=68 的镜像（零拷贝默认开）。
> 结论先说：**零拷贝 RX 在“帧要交给本机 socket”时会损坏数据**，不是偶发网络抖动，
> 是 bp 被提前还回 BMU 的结构性错误。20 MB ssh 上传 10 次坏 2 次，关掉零拷贝 6/6 干净。

## 1. 现象

宿主往盒子推 20 MB 随机数据，盒子侧 `cat > /tmp/y.bin` 后比字节数：

| 配置 | 次数 | 成功 | 失败表现 |
| --- | --- | --- | --- |
| `zc=1` | 3 | 2 | `Broken pipe` / `Connection reset by peer` |
| `zc=1` | 3 | 2 | 同上 |
| `zc=1` | 10 | 8 | 同上 |
| `zc=0` | 3 | 3 | —— |
| `zc=0` | 3 | 3 | —— |

盒子 `logread` 里的失败点只有 dropbear 一句：

```
dropbear[...]: Exit (root) from <192.168.1.250:...>: Error decrypting
```

“Error decrypting” = 收到的一串字节 MAC 校验不过，也就是**明文被改过**。内核没有任何
告警（`rx_bad=0`、`bmu_alloc_fail=0`、`q_ovf=0`），TCP 校验和也早已通过——说明数据是在
校验和算完之后、被进程读走之前被改写的。

## 2. 根因

零拷贝 RX 的 bp 归还依赖 `skb->destructor`：

```c
static void zx_zc_destructor(struct sk_buff *skb)  /* bp 进隔离环，2 tick 后归还 */
```

但协议栈把帧交给 socket 时，`skb_set_owner_r()`（`include/net/sock.h`）是这么写的：

```c
static inline void skb_set_owner_r(struct sk_buff *skb, struct sock *sk)
{
	skb_orphan(skb);              /* 触发我们的 destructor：bp 进隔离环 */
	skb->sk = sk;
	skb->destructor = sock_rfree; /* 我们的 destructor 被顶掉 */
	...
}
```

于是：

1. `skb_orphan()` → 我们的 destructor 把 bp 放进隔离环，标记 `DROPQ`，到期时间 `jiffies + 2`；
2. 紧接着 destructor 被换成 `sock_rfree`，**这个 skb 活着的后半生再也没有任何回调**；
3. 2 个 tick 后 `zx_zc_reap()` 看到 `DROPQ` 到期，把 bp 还回 BMU；
4. skb 此时还躺在 socket 接收队列里（大文件传输时队列里能压几百 KB，也就是几十上百个
   skb，停留时间远超 2 tick），硬件把新收到的帧 DMA 进同一个槽位；
5. dropbear 读出来的就是被覆盖过的数据。

代码里的老注释其实已经描述了危险（“本机收包时 skb 会长期留在 socket 队列里”，
`ZX_ZC_OUT_MAX` 闸门就是为它加的），但闸门只限制**同时在外的 bp 数量**，不解决
“skb 还活着但 bp 已经还了”的时序问题。

为什么转发没暴露：转发的 skb 在 `skb_orphan()` 之后很快会被 `zx_zc_tx_take()` 拿走——
发送路径会先把 bp 从隔离环里摘掉再交给硬件，不存在“2 tick 后提前归还”的窗口。
所以只有**本机终结**（ssh / LuCI / DNS / 任何到盒子自己的 TCP、UDP）会坏。

同一类风险还包括任何让 skb 在栈里停留超过 2 tick 的处理：NFQUEUE、`tc` 整形
（netem/tbf）、面板抓包队列等。现在的默认配置里这几样都没开，但都不安全。

## 3. 第一版修法的翻车（skb->users 同样不可靠）

第一版修法（serial=69 的镜像）在隔离环里替槽位持有一份 **skb 引用**：destructor 里
`skb_get(skb)`，`zx_zc_reap()` 里等 `refcount_read(&skb->users) == 1` 才还 bp。损坏确实
消失了，但换来更糟的东西——**内核 panic**：

```
[  716.500211] Internal error: Oops - undefined instruction: 0 [#1] SMP ARM
[  716.592419] PC is at kfree+0x15c/0x25c
[  716.596179] LR is at skb_release_data+0x15c/0x1e4
[  716.600889] Register r2 information: slab skbuff_small_head start c23ad400 pointer offset 0 size 640
[  716.889564] Call trace:
[  716.889575]  kfree from skb_release_data+0x15c/0x1e4
[  716.897085]  skb_release_data from sk_skb_reason_drop+0x3c/0x84
[  716.903015]  sk_skb_reason_drop from zx_eth_poll+0xb7c/0xce4
```

`skbuff_small_head` 的对象在 `skb_release_data()` 里被二次释放。根因：**`skb->users` 不是
数据生命周期的权威计数**，内核有一整类释放路径根本不经过它：

| 路径 | 是否走 `users` | 说明 |
| --- | --- | --- |
| `kfree_skb()` | 走 | `skb_unref()` → `refcount_dec_and_test` |
| `__kfree_skb()` | **不走** | 直接 `skb_release_all()` + `kfree_skbmem()` |
| `sk_eat_skb()` | **不走** | `net/core/sock.c`，就是 `__kfree_skb()` |
| `kfree_skb_partial(skb, true)` | **不走** | 只 `skb_release_head_state()` + 释放结构体 |
| `skb_data_unref()`（数据） | 走 **`dataref`** | 所有释放路径都经 `skb_release_data()` |

socket 收包恰好走第二条：`tcp_recvmsg_locked()`（`net/ipv4/tcp.c:1585`）每读完一个 skb
就 `__kfree_skb(skb)`。于是 `skb_get()` 拿到的那份“引用”被无声忽略，sk_buff 结构体当场
被释放、slab 立刻被别人复用，隔离环里剩下的只是悬空指针；`reap` 之后 `kfree_skb()` 它，
释放的是**别人的** skb，那个 skb 的主人再释放一次就是上面这个 panic。所以它只在
“帧交给本机 socket”时出现，和损坏问题的触发条件完全一样。

## 4. 最终修法：在 `skb_shared_info->dataref` 上拿影子引用

`dataref` 是内核对“这片数据”唯一的权威引用计数，`skb_release_data()` →
`skb_data_unref()` 是所有路径的必经之路（`__kfree_skb` 也走它，只是不碰 `users`）。
做法等价于克隆一份数据引用，但不分配 `sk_buff`：

- destructor 里（`skb->head == 槽位` 且 `skb->nohdr == 0`）：
  `atomic_inc(&shinfo->dataref)` + `skb->cloned = 1`，把 **bp** 挂进隔离环，
  状态 `SKB → SHADOW`。这两个动作和 `skb_clone()` 内部做的完全一样，只是没有结构体。
- `zx_zc_reap()`：`dataref & SKB_DATAREF_MASK == 1` 说明只剩我们这份（协议栈、克隆体
  都放完了）→ 替 head 还掉那次 `page_frag_free()` 的页引用（`put_page`），再把 bp 还回
  BMU。环里只存 bp，不存 skb 指针，悬空指针这条路彻底消失。
- `zx_zc_tx_take()`：`dataref == 1(+我们那份)` 才允许把槽位交给硬件，否则退回拷贝；
  交给硬件前摘掉环项、`atomic_dec(dataref)`、清掉 tag 和 destructor（bp 归硬件）。
- 陈旧 tag：`zc_skb[bp]` 记着槽位当前的主人，destructor 里指针对不上就不动手。

**代价（实测待补）**：影子引用要 `skb->cloned = 1`，于是这个 skb 再走
`skb_cow_head()` 时 `skb_header_cloned()` 为真、会 `pskb_expand_head()` 拷贝一份。
这就是为什么它只能套在“已经不会再被改写”的 skb 上：
  * 本机终结（socket 队列）：只读，没有 cow；
  * 桥/flowtable 转发：实测**根本不 orphan**——716 s 内 `rx_ok=49 tx_ok=45 free_ok=0`，
    帧在 destructor 之前就被 `zx_zc_tx_take()` 拿走了，所以这条路上没有影子引用、也就
    没有 cloned 位。
反过来说，如果哪天转发路径开始 orphan（换 nft 配置、加 tc 动作），这些帧会退化成
“一次 expand 拷贝 + 一次发送拷贝”，比关掉 zc 更差。观察指标：`zc` 里 `fallback` 和
`lost` 同时涨、`out` 居高不下，就说明这条假设破了。

**唯一追不到的情况**：`pskb_expand_head()` 把数据搬出槽位后，`skb->head != 槽位`，
而槽位有没有被别的克隆体引用从这个 skb 上看不出来（克隆过的 expand 会把旧 head 留给
克隆体）。这类槽位直接扣着不还，记 `zc_lost`；累计到 `ZX_ZC_LOST_MAX`(64) 自动把
`zc_rx` 关掉并在串口告警。宁可少 64 个缓冲，也不还一个可能还在被读的缓冲。
正常转发路径不做 expand（16 B headroom 够 L2 重写），本机终结路径也不会——实测待补。

被评估过但没选的路子：

- `SKBFL_ZEROCOPY_ENABLE` + 自建 `ubuf_info`，借 `skb_release_data()` 的
  `skb_zcopy_clear()` 拿“最后一个引用”回调：`__pskb_pull_tail()` 在 `!skb->data_len`
  时会无条件 `skb_zcopy_clear(skb, false)`，对“数据全在线性头里”的我们就是提前归还；
  另外 `pskb_expand_head()` 会对 `ubuf_info` 做 `refcount_inc()`。
- orphan 时 `pskb_expand_head(skb,0,0)` 把帧搬出槽位、立刻还 bp：socket 路径白付一次
  1.5 KB 拷贝 + 2 KB 分配，零拷贝收益全丢；克隆体的情形同样还不了。
- 依赖 `page` 引用计数（page_pool 那套）：池子是一片连续 DMA 内存，一个 4 KB 页里有两个
  槽位，逐槽位的页引用没法区分。

## 5. 复现脚本

```bash
# 宿主：socket 路径（会损坏 / 会 panic）
head -c 20000000 /dev/urandom > /tmp/fw20.bin
for i in $(seq 1 10); do
  cat /tmp/fw20.bin | ssh root@192.168.1.1 'cat > /tmp/y.bin; wc -c < /tmp/y.bin'
done
# 盒子（对比组）
echo 0 > /sys/kernel/debug/92000000.ethernet/zc
```

判定：输出不是 `20000000` 就是坏；`dmesg | tail` 里出现 `skbuff_small_head` / `kfree`
的 Oops 就是 §3 那条二次释放。修好后的判据（`debugfs .../zc`）：

```
rx_ok 涨、free_ok 跟着涨、out 回到 0、lost=0、stale=0、q_n=0
```

`lost` 非 0 说明有帧被 expand 搬走了，需要再想别的办法追。

## 6. 影子引用之后还剩的问题：同一个 bp 被还两次（2026-10-31 夜）

§4 的修法把 skbuff 二次释放（§3 的 panic）消掉了，但**数据损坏仍在**：宿主连续
20 MB ssh 上传，第 2 轮就 md5 不对。把 `zc_canary`（交付快照 + reap 前比对）打开后
抓到的征象是同一个 bp 被 `zx_zc_reap()` 归还了两次：

- 第一次还完，硬件立刻把这个 bp 又发了下来（RX DMA 覆写槽位 → canary 记 `corrupt`），
  于是两个 skb 指向同一个槽位，内容互相覆盖 → 上传的字节坏掉；
- 计数三元组同时出现：`rx_dup`（交付时 state 不是 FREE）、`dup_push`（同一个 bp 在
  隔离环里出现第二份）、`dup_reap`（摘环项时位图已经被清过）。

27k 包一次的量级：`rx_dup=1 dup_push=1 dup_reap=1`，`corrupt` 被 ratelimit 吞掉。
用**旧代码**（环项只在真正归还时清位图）能稳定跑 26k 包不 panic，说明"三件套"是罕见
事件；它一旦发生就连续坏包，因为重复项会连环把活着的槽位还回去。

注意 `badfree` 抓不到这种双还：`reap` 是**先把 `zc_state[bp] = FREE` 再调
`zx_bmu_free_bp()`**，等 `zx_bmu_free_bp()` 里的检查跑起来时状态已经变成 FREE 了。
真正能看见双还的是事件流水（见下）里的两次 `type 9`。

### 6.1 诊断插桩（本轮加的，正式版要能一键关掉）

| 东西 | 位置 | 说明 |
|---|---|---|
| `zc_canary`（module_param，默认 1） | `zx279128s-main.c` | 交付时快照槽位前 64 B，reap 归还前比对，被写过就 `corrupt++` |
| 事件流水 `zc_ev[bp][8]` | 同上 | `type<<24 \| aux<<16 \| (jiffies & 0xffff)`。type：1=RX交付(aux=len>>6) 2=影子入环 3=reap归还(aux=dataref) 4=发送申请(aux=state) 5=zc发送 6=陈旧tag 7=追不到 **9=还bp给BMU(aux=state\|0x10 在环里)** |
| debugfs `zc_ev` | `92000000.ethernet/zc_ev` | 把所有"不在 BMU 空闲表里 / 还在环上"的 bp 连同 8 条事件打出来，一次读就是一张快照，不用重启 |
| 异常点告警 | 同上 | `rx_dup`/`dup_push`/`dup_reap`/`badstate`/canary 命中时 `zx_zc_ev_dump()` 把 state、dataref、tag、`zc_skb[bp]`、destructor、`q_n`、位图、8 条事件一起打进 dmesg（`net_ratelimit()` 限流，会吞掉一部分） |

读事件流水要注意：8 条是**环形**写入（`zc_ev_n[bp]++ & 7`），所以 `n>8` 时打印出来的
顺序是被旋转过的，最早的一条在 `n & 7` 位置上。

### 6.2 两道闸：一个 bp 只能被还一次

现在的规则（顺序本身就是不变量，写错就出事）：

1. `reap` 先看 `dataref`：`!= 1` 就 `break`，**原位不动** —— 不清位图、不摘环项。
2. 真要归还时：位图必须还是 set（否则是同一 bp 的第二份环项 → 丢项不还）；
   并且 `zc_state[bp]` 必须是 `SHADOW`（否则槽位已经被别人接管/归还 → 丢项不还）。
3. "丢项"等于把一个缓冲留在 BMU 外面：记 `zc_lost`（`zx_zc_leak()` 统一记账），
   累计到 `ZX_ZC_LOST_MAX`(64) 自动 `zc_rx=false` 并告警 —— 宁漏不双还。
4. RX 交付前 `zc_state[bp]` 必须是 `FREE`，否则整帧丢弃、**也不还 bp**
   （`rx_dup` + `rxdrop`）：BMU 空闲表里出现了重复项，这一帧和别的 skb 共用槽位，
   交付出去就是串包；把 bp 还回去又会把别人正在读的槽位交给硬件。

**踩过的坑（顺序写反的代价）**：第一版把位图测试放在 `dataref` 检查**之前**。队头那条
常常因为 skb 还在 socket 队列里而 `dataref != 1`，这时应该原地 `break`，但代码先清了
位图再 break —— 下一次进来就把自己这半截动作判成"重复环项"，一次 20 MB 上传（0.48 s、
1743 个包）立刻爆出 `dup_reap=67`、`badstate` 之外的 `out=68`（泄漏 68 个缓冲），
`corrupt=9`、`rx_dup=3`。改回"先 dataref、后位图"之后误判消失。**这说明位图和状态闸
只能在"确定要归还"的那一刻用，不能当探针。**

### 6.3 现状与下一步（交接）

- 已构建未验证：镜像 `out/e2631-openwrt-factory.bin`（02:54，`make world` rc=0），
  含 §6.2 的正确顺序 + type 9 归还流水。盒子当前跑的是**顺序写反的那一版**
  （所以它现在会误判、会泄漏），要么刷 02:54 那版，要么先把 `zc` 关掉再离开。
- 待验证判据（`debugfs .../zc`，跑 100 轮 20 MB 上传 = 2 GB）：

  ```
  rx_ok 涨、free_ok 跟着涨、md5 全对、out 回 0
  corrupt=rx_dup=tx_dup=badfree=badstate=rxdrop=dup_push=dup_reap=lost=q_ovf=0
  ```
- 若 `rx_dup`/`rxdrop` 还会涨：那就是 BMU 空闲表真的会重复发同一个 bp。下一步查
  **厂商的归还协议**（`ZX_BMU_BUF_FREE` 的写时序、`ZX_BMU_FREE_STATUS` 里
  `bmu_free_slots` 缓存是否可信、reap 一次连还 256 个是不是太快），再考虑给 bp 加
  世代 epoch（tag 里带 epoch，环项存 epoch，三处都要求匹配）。epoch 只能把"双还"
  降级成"泄漏一次"，治不了根，但能让损坏彻底消失。
- 长期：按主线的 page_pool 思路重做，把"页引用计数"恢复成唯一真相（现在
  `ZX_ZC_PARK = 1<<20` 把页计数垫成了假账，所以只能自己维护 dataref 影子）；
  顺带评估收包侧换 `napi_build_skb()`（每 CPU 批量 sk_buff 缓存，省一次
  `skbuff_head_cache` 分配，现在 `napi_gro_receive` 单项 2.94 µs 是最大开销）。
