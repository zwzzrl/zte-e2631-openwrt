# 原厂 CLA 哈希算法与键布局：已复原（2026-10-30）

本文把「谁写 ONU reg 12 / CLA 哈希怎么算 / 槽位怎么定」这条线走完。结论是：
**CLA 的哈希不是硬件黑盒算的，`cla_acl_hash_addr_gen` 里有完整软件实现，已逐指令复原并用原厂自己的解码侧交叉验证。**
ONU reg 12 那组哈希配置**无人写入**（见 `vendor-ipv6-acl-write-sequence-2026-10-30.md` §8.5），因此它不是我们算槽位的前提。它是**八个 2 位选择器**（`cla_get_hash_poly_config` 的拆法：寄存器位 0/2/4/6 → `out[0..3]`，16/18/22 → `out[4..7]`；实测寄存器值 `0x00e400e4`，拆出来是 `0,1,0,3,0,1,0,3`），而本文 §2 找到的四条多项式正好是这些 2 位值能选的全部对象 —— 两边是咬合的（`推测`：硬件里共有 8 处用到哈希，每处从 4 条多项式里挑一条）。

文中每个数字都标注了来源：`代码`（反汇编逐指令）、`实测`（真机 sysfs 写读）、`推断`（推断会明写）。

---

## 1. 一句话结论

```
slot_hash = CRC32_msb_first(poly[type], 0x00 || reverse(key[0..43])) & 0xffff
```

- `key` = 44 字节（第 45 字节只有 1 位有效），布局见 §3。
- `type` = 0..3，由调用方（ACL 通道）给出，对应四条不同的 32 位多项式。
- 初值 0，末尾无 xorout，**输出截断到 16 位**（`uxth`）。
- 消息 = **1 个前导 0 字节 + 44 字节键的完全逆序**，共 45 字节。

来源：`代码`——`tm.ko` 中 `cla_acl_hash_addr_gen`（虚拟地址 0x6cec，长度 0x4b4）完整反汇编。

**验证（2026-10-30，真跑原厂代码）**：`tools/vendor-emu/t_clahash.py` 用 unicorn 执行
`tm.ko` 里这个函数本身（只把 printk 桩掉），对 200 个随机 44 字节键 × 4 种类型共 800 次
调用，与上式**零不一致**：

```
cd tools/vendor-emu
docker run --rm -v <vendor-rootfs>/kmodule-unsquashfs/tm.ko:/ko/tm.ko:ro \
  -v "$PWD":/w -w /w --entrypoint python3 e2631-uni:py312 t_clahash.py 200
=> type 0..3 的四条多项式打印出来，keys 200: mismatches {0: 0, 1: 0, 2: 0, 3: 0}
```

同目录的 `t_hash.py` 对 `sbrg_hash()` 做了同样的事（300 个随机键、init 0 零不一致，
init 0xffff 300 个全错，作为反例）。

---

## 2. 四条多项式（含定位依据）

代码按 `r6`（hash_mode / ACL 类型）选表，四张表都是 256×4 字节，紧挨着放在 `.data`：

| type | 多项式 | 表在 `.data` 内偏移 | 文件偏移 |
|---|---|---|---|
| 0（默认分支） | `0x04C11DB7` | `.LANCHOR0+0xa7c`，`.LANCHOR0=0x70` → `0xaec` | 0x0ad17c |
| 1 | `0x1EDC6F41`（CRC-32C） | 同上 +0xe7c → `0xeec` | 0x0ad57c |
| 2 | `0xF4ACFB13` | `.LANCHOR2+(-0xd7c)`，`.LANCHOR2=0x2068` → `0x12ec` | 0x0ad97c |
| 3 | `0x32583499` | `.LANCHOR2+(-0x97c)` → `0x16ec` | 0x0add7c |

定位方法（可复算）：
1. 全文件穷举「1 KiB 合法 CRC 表」（`t[0]=0` 且 256 项能用 `t[1]` 当多项式重新生成），只有这 4 张，且正好 `0x400` 等距。
2. 用 `.rel.text` 里的 `R_ARM_MOVW_ABS_NC` / `R_ARM_MOVT_ABS` 重定位拿到基址符号（0x6d74→`.LANCHOR0`，0x6df8/0x6e2c→`.LANCHOR2`），加上指令里的 `ldr` 偏移，算出表地址，落到上表 4 个文件偏移上，一一对应。

注：`type 0` 与 `cla_list_hash_addr_gen`（0x71a0，60 字节那个短函数）用的是**同一张表、同一个结构**，区别只在后者不截断 16 位、键长度不同。

### 2.1 与 ONU reg 12 的关系

reg 12 是 8 个 2 位字段，实测值 `0x00e400e4` 拆成 `0,1,0,3,0,1,0,3`，每个字段就是一个「用哪条多项式」的选择器（值域正好 0..3，与上表四行一一对应）。
所以「谁写 reg 12」这个问题即使答案变成「有人写」，也不改变软件算法：`cla_acl_hash_addr_gen` 的 `r6` 参数是调用方直接给的 `hash_mode`，它不读寄存器。两者一致（`推断`），因为原厂 init 写进 reg 12 的那组选择器与各调用点传的 `r6` 必须相同，否则硬件和软件会算出不同槽位。

---

## 3. CLA 键布局（353 位有效）

键缓冲区是 **12 个字 = 48 字节**（`sp+0x8c` 起），但只有低 **353 位** 由参数填充，哈希只吃 `key[0..43]`。

位偏移（LSB 优先，字节内小端）：

| 位偏移 | 宽度 | 字段（字段名取自原厂调试 printk `.LC266`~`.LC271`） | 键字节 |
|---|---|---|---|
| 0 | 5 | `outport` | 0x8c |
| 5 | 12 | `inport` | 0x8c..0x8e |
| 17 | 3 | `tag_level` | 0x8e |
| 20 | 2 | `l2_type` | 0x8e |
| 22 | 1 | `ex_rule_id` | 0x8e |
| 23 | 8 | `pppoe_flag` | 0x8e..0x8f |
| 31 | 1 | `ex_rule_mode` | 0x8f |
| 32 | 1 | `direct` | 0x90 |
| 33+16k | 16 | `extra_data[k]`，k=0..19 | 0x90..0xb6 |
| 352 | 1 | `extra_data[19]` 的最高位 | 0xb8 |

`extra_data[19]` 只有最高位落在键里（位 352），低 15 位在软件侧没有对应来源——这是原厂测试工具的参数个数（29）与键结构之间的空档，不是我们的误读（`代码` + §5 的复现测试都指向这一点）。

### 3.1 复原方法（可复现）

`cla_info_store` 里 `calculatehashaddr` 分支（0x666d8..0x6697c，169 条指令）把 `sscanf` 的 29 个 `%x` 结果按位塞进键缓冲区。写了一个只含必要子集的 ARM 模拟器（字节寻址内存，支持 `bfi/ubfx/orr/bic/lsl/ldrh/strh/...`），169 条指令全部执行。然后**逐位 one-hot 探测**：对每个输入变量的每一位置 1，看键区哪一位变化。

结果：全部是「输入位 b → 键位 33+b」这类纯单比特搬运，**没有任何一处非线性**，于是得到上表的连续排布。空隙只有 `5..16`（`inport` 12 位，覆盖）之外的两段：`33` 之前无洞，`337..351`（15 位，无来源）、`353..383`（31 位，未用）。

参数序号到变量的映射（`代码`，注意 `0x12c` 出现两次，后写的 arg24 覆盖 arg2）：

```
arg1  -> sp+0xd0      arg2  -> sp+0x12c     arg3..arg23 -> sp+0xd8,0xdc,...,0x128
arg24 -> sp+0x12c     arg25..arg29 -> sp+0x130,0x134,0x138,0x13c,0x140
```

`arg2` 与 `arg24` 同址、`arg29` 的槽（`sp+0x140`）不参与键，所以真实可达的参数只有 26 个不同槽位。

### 3.2 编码器自检（与原厂解码器对拍）

`cla_acl_hash_addr_gen` 自己在 `debug_level>6` 时会 printk 出各字段值，用的是一组「从键字节反解字段」的公式，例如：

```
outport      = key[0] & 0x1f
inport       = (key[1]<<3) | (key[0]>>5) | ((key[2]&1)<<11)
tag_level    = (key[2]>>1) & 7
l2_type      = (key[2]>>4) & 3
pppoe_flag   = ((key[3]&0x7f)<<1) | (key[2]>>7)
extra_data[j]= (key[4+2j+1]<<7) | (key[4+2j]>>1) | ((key[4+2j+2]&1)<<15)
```

把这组公式当独立解码器，喂 200 组随机 32 位参数给我的编码器模拟，**除 `extra_data[19]`（key[44]，不在哈希范围）外 200/200 完全一致**。
两条不同的代码路径（打包 vs 调试打印）对同一份布局给出相同结果，这是布局正确性的强证据。

---

## 4. 硬件调试面 `calculatehashaddr` 的对拍结果（重要：只能当部分基准）

`/sys/devices/platform/tm/ppCla/calculatehashaddr`（`echo "29 个 %x"` → printk `calculate hash addr is :0x%x`）。
它内部**不含任何 CRC 循环**（`cla_info_store` 里 `eor` 零命中），而是把打包好的 12 个字写到 `0xe30b1+i`（`fpga_write_reg`，i=0..11），再读 `0xe30bf`（`fpga_read_reg`）拿结果 —— 也就是说它走的是芯片的 FPGA/测试寄存器窗口，不是生产数据面。

### 4.1 吻合的探针（21 项，全部 16 位精确相等）

| 探针 | 硬件 | 本文模型 |
|---|---|---|
| 全零 | 0x0000 | 0x0000 |
| arg1=1 / 2 / 4 / 8 | 0x1db7 / 0x3b6e / 0x76dc / 0xedb8 | 同 |
| arg5=1（1 位字段） | 0x21c0 | 0x21c0 |
| arg5=0xffffffff | 0x21c0 | 0x21c0（1 位截断） |
| arg5=2 | 0x0000 | 0x0000（1 位截断） |
| arg7=1 / arg8=1 | 0x3d1d / 0x678d | 同 |
| arg3=8 | 0x0000 | 0x0000（3 位截断） |
| arg4=4 / arg4=8 | 0x0000 / 0x0000 | 0x0000（2 位截断） |
| arg13=1 / 2 / 4 / 8 | 0xb1bd / 0x7ecd / 0xe02d / 0xc05a | 同 |
| arg21=1 | 0x8d6e | 0x8d6e |
| arg1=0x3 | 0x26d9 | 0x26d9 |
| arg1=0x7 | 0x5005 | 0x5005 |
| arg1=1 且 arg5=1 | 0x3c77 | 0x3c77（跨字段线性） |
| arg13=1 且 arg21=1 | 0x3cd3 | 0x3cd3（跨字段线性） |

单比特探针覆盖了键字节 0、2、4、28 等多个位置，**16 位精确命中不可能是巧合**（2^-80 量级）。

### 4.2 不吻合的探针（10 项，均已复测确认可复现、与写入历史无关）

| 探针 | 硬件 | 本文模型 |
|---|---|---|
| arg1=0x10 | 0x96c2 | 0xdb70 |
| arg1=0x11 | 0x8b75 | 0xc6c7 |
| arg1=0xf | 0xb01b | 0xbdbd |
| arg1=0x1f | 0xc6c7 | 0x66cd |
| arg1=0xffff | 0xb01b | 0x66cd |
| arg13=0xf | 0xcc93 | 0xef07 |
| arg13=0x10 | 0x03e3 | 0x9d03 |
| arg13=0x11 | 0xb25e | 0x2cbe |
| arg13=0xffff | 0xe350 | 0xd137 |
| arg3=0x10 | 0xd624 | 0x0000 |
| arg28=0xffff | 0x7387 | 0x0000 |

判据：复测六项（arg1=1、arg1=0x8、arg1=0xf、arg1=0x1f、arg13=1、全零）全部与首测逐位相同 ⇒ 结果只取决于本次写入，不是寄存器残留。

### 4.3 为什么不吻合，以及为什么这不影响本文结论

关键反证：`arg3=0x10`。按打包代码，`tag_level` 是 3 位字段，16 & 7 = 0，**键与全零输入逐字节相同**；若该调试面只是「把 12 个字丢给硬件算哈希」，输出必须等于全零的 `0x0000`。实测是 `0xd624`。
同样，`arg28=0xffff` 只能动 key[44] 的 bit0（位 352），而哈希只吃 `key[0..43]`，输出必须等于 `0x0000`；实测 `0x7387`。

也就是说：**硬件调试面的输出不只取决于那 12 个键字**（很可能 `fpga_write_reg` 的「寄存器号」也参与，或该窗口走的是另一条测试通路）。
所以它不能当全量基准，只能当「算法族正确」的部分验证 —— 而 §4.1 的 21 项精确命中已经把「CRC32 + 前导 0 + 44 字节逆序 + 截 16 位」这个结构钉死了。

反过来看：生产路径算槽位用的是软件函数（`cla_acl_hash_addr_gen` 的返回值），硬件只要在同一个地址找到表项即可；所以**驱动按下式实现即与生产路径一致**，无需关心调试面为何偏离。
而且 §1 的公式已经**用 unicorn 真跑原厂这个函数验证过 800/800**（不是从反汇编推的），所以这里的偏离只可能是调试面/硬件侧的问题，不可能是公式错。

---

## 5. 对驱动的影响

1. 我们的 `zx_ppe_sbrg_hash()`（IPv6 组播表，CRC16-CCITT over 34 字节逆序 & 0x3ff）属于**同一套设计套路**：逆序拷贝 + 前导 0 + 查表 CRC + 截断。这条已由 `sbrg_hash` 反汇编确认，且我们的实现位对位一致。
2. CLA 侧若要落地 IPv6 单播加速，需要三个新件：键构造（§3）、哈希（§1）、以及 CLA 表写入协议（`cla_set_hash_table` 0x5a24 / ONU reg 1/2 的握手，见 `vendor-ipv6-acl-write-sequence-2026-10-30.md` §4/§5）。本次只解决前两件。
3. **不要**为了算槽位去读 ONU reg 12：没有人写它，读了也没用（它是硬件复位默认值，原厂自己也只在 `cla_get_hash_poly_config` 里回读打印）。读它在语义上给的是「8 个 2 位选择器」，即每处哈希用哪条多项式，不是多项式本身。
4. 若将来要把调试面接进我们的自检：记住 §4.3 的坑——`calculatehashaddr` 不能当全量基准。

---

## 6. 诚实未知

- §4.2 偏离的确切成因未查清（推断：`fpga_write_reg` 的寄存器号/状态字参与运算，或该窗口是 NP 侧另一套哈希）。这**不影响**生产路径算法的可信度（§1 已由真跑原厂代码验证），但如果将来要靠这个调试面做回归测试，必须先解开。
  已排除的方向：`fpga_write_reg`/`fpga_read_reg` 定义在 `zx_ponreg.ko`，就是 `base[reg] = val` 的一次读写，`base = 0xf4000000`（`fpga_module_init` 里 `mov lr,#0xf4000000`），是 SoC 的固定硬件窗口 —— 所以读回值确实由硬件产生，不是内存里的残留。
- `outport`/`inport`/`extra_data` 的**语义**（哪个是 vlan、哪个是 sip 片段）尚未与真实 ACL 规则对上。我们只知道位宽与排布，字段名来自原厂 printk。要落地 IPv6 单播，还得把 `sw_acl_l3_hardfast_session_add` 里「规则 → 44 字节键」的那一步也解出来。
- `type` 2/3 的多项式（`0xF4ACFB13` / `0x32583499`）对应哪类通道未确认；生产路径由调用方传 `r6`，需要追 0x4ccd0 / 0x4cdb4 两个调用点的实参。

---

## 7. 复核命令

```bash
# 1) 取函数反汇编（宿主，工具链前缀见 docs/architecture.md）
TC=工作文件/08-源码与构建树/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-
${TC}objdump -d 工作文件/03-固件备份/vendor-rootfs-decrypted/kmodule-unsquashfs/tm.ko > /tmp/tm.asm
awk '/^00006cec </{f=1} /^000071a0 </{f=0} f' /tmp/tm.asm

# 2) 四张表的定位（本文件 §2 的方法，脚本化）
python3 - <<'EOF'
import struct
d=open('tm.ko','rb').read()
def gen(p):
    t=[]
    for b in range(256):
        c=b<<24
        for _ in range(8):
            c=((c<<1)^p)&0xffffffff if c&0x80000000 else (c<<1)&0xffffffff
        t.append(c)
    return t
for off in range(0,len(d)-1024,4):
    if d[off:off+4]!=b'\0\0\0\0': continue
    p=struct.unpack('<I',d[off+4:off+8])[0]
    if p and list(struct.unpack('<256I',d[off:off+1024]))==gen(p):
        print(hex(off), hex(p))
EOF

# 3) 真机对拍（每行一个探针）
./vsh.py 'N=/sys/devices/platform/tm/ppCla/calculatehashaddr; echo "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0" > $N; timeout -t 2 cat /dev/logger_main > /tmp/lg.bin; grep -a -o "calculate hash addr is :0x[0-9a-f]*" /tmp/lg.bin'
```
