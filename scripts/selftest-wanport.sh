#!/bin/sh
# e2631-wanport 的逻辑自检：在 PC 上（不是路由器上）跑，用假的 uci / ip /
# udhcpc / ubus / logger 和一棵假的 /sys，把「探测 → 选口 → 改配置」这条链路走一遍。
#
# 为什么值得写：这段逻辑真正跑起来要么在路由器的启动过程中、要么在换线的瞬间，
# 那时候出错就是断网，而且路由器上不好复现。这里把可以离线验证的部分（配置读写、
# 候选口选择、优先级、兜底猜测、MAC 固化、探测期间的网桥/MAC 借用与恢复）钉住；
# 真机上才能验的部分（PHY 行为、上游 DHCP 应答）不在这里。
#
# 用法：
#   sh scripts/selftest-wanport.sh      # 退出码 0 = 全部通过
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT="$HERE/../overlay/target/linux/sanechips/base-files/sbin/e2631-wanport"
[ -f "$SCRIPT" ] || { echo "找不到 $SCRIPT" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/e2631-wanport-selftest.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

BIN="$TMP/bin"
SYS="$TMP/sys/class/net"
DB="$TMP/uci.db"
mkdir -p "$BIN" "$SYS/lan1" "$SYS/lan2" "$SYS/lan3" "$SYS/wan" "$TMP/lock"
printf '#!/bin/sh\necho "$1" >>"${E2631_PROBE_FLAG:-/dev/null}"\nexit 0\n' >"$TMP/nop.sh"
chmod +x "$TMP/nop.sh"

# ---- 假的 uci -------------------------------------------------------------
# 键里带 @ [ ] 这种字符，不能拿 sed/grep 当正则用，统一走 awk 的前缀匹配。
cat >"$BIN/uci" <<'EOF'
#!/bin/sh
while [ $# -gt 0 ]; do
	case "$1" in
	-q) shift ;;
	*) break ;;
	esac
done
cmd=${1:-}; shift || true
dropkey() { awk -v k="$1=" 'index($0, k) != 1' "$DB" >"$DB.tmp" 2>/dev/null; mv "$DB.tmp" "$DB"; }
case "$cmd" in
get)
	k=${1:-}
	vals=$(awk -v k="$k=" 'index($0, k) == 1 { print substr($0, length(k) + 1) }' "$DB" 2>/dev/null)
	[ -n "$vals" ] || exit 1
	printf '%s' "$vals" | tr '\n' ' ' | sed 's/ $//'
	echo
	;;
set)
	kv=${1:-}; k=${kv%%=*}; v=${kv#*=}
	dropkey "$k"
	printf '%s=%s\n' "$k" "$v" >>"$DB"
	;;
add_list)
	kv=${1:-}; k=${kv%%=*}; v=${kv#*=}
	printf '%s=%s\n' "$k" "$v" >>"$DB"
	;;
delete) dropkey "${1:-}" ;;
commit) : ;;
*) : ;;
esac
exit 0
EOF

# ---- 假的 ip / udhcpc / ubus / logger -------------------------------------
cat >"$BIN/ip" <<'EOF'
#!/bin/sh
echo "ip $*" >>"$TMP/ip.log"
exit 0
EOF
# udhcpc：$TMP/answer 里写的那个口才有 DHCP 应答。有应答的口写 bound 之后就一直
# 挂着（等脚本把它 kill 掉），没应答的口立刻失败退出——两条分支都能覆盖，也不拖时间。
cat >"$BIN/udhcpc" <<'EOF'
#!/bin/sh
port=""
while [ $# -gt 0 ]; do
	[ "$1" = "-i" ] && { port=$2; shift; }
	shift
done
echo "probe $port" >>"$TMP/probe.log"
if [ -f "$TMP/answer" ] && [ "$(cat "$TMP/answer")" = "$port" ]; then
	[ -n "${E2631_PROBE_FLAG:-}" ] && echo bound >>"$E2631_PROBE_FLAG"
	# 阻塞在一个没人写的 fifo 上，模拟"客户端还活着"
	mkfifo "$TMP/hold.$port" 2>/dev/null && exec cat "$TMP/hold.$port"
fi
exit 1
EOF
# ubus：wan 是否 up 由 $TMP/wanup 决定；其它对象一律"在"。
cat >"$BIN/ubus" <<'EOF'
#!/bin/sh
if [ "${2:-}" = "network.interface.wan" ]; then
	if [ -f "$TMP/wanup" ]; then echo '{"up":true,"proto":"dhcp"}'; else echo '{"up":false}'; fi
fi
exit 0
EOF
cat >"$BIN/logger" <<'EOF'
#!/bin/sh
echo "log $*" >>"$TMP/syslog"
exit 0
EOF
chmod +x "$BIN/uci" "$BIN/ip" "$BIN/udhcpc" "$BIN/ubus" "$BIN/logger"

export TMP DB
export PATH="$BIN:$PATH"
export E2631_SYSFS="$TMP/sys"
export E2631_LOCKDIR="$TMP/lock/e2631-wanport.lock"
export E2631_NOPSCRIPT="$TMP/nop.sh"

FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok  : $*"; }

# ---- 助手 ----------------------------------------------------------------
setcfg() { "$BIN/uci" set "$1=$2"; }
addlist() { "$BIN/uci" add_list "$1=$2"; }
deletelist() { "$BIN/uci" delete "$1"; }
getdb() { "$BIN/uci" -q get "$1" 2>/dev/null; }
newmac() { echo "$2" >"$SYS/$1/address"; }
link() {
	echo "$2" >"$SYS/$1/carrier"
	if [ "$2" = "1" ]; then echo up >"$SYS/$1/operstate"; else echo down >"$SYS/$1/operstate"; fi
}
bridge() {
	if [ "$2" = "1" ]; then
		mkdir -p "$SYS/br-lan"
		ln -sfn "../../br-lan" "$SYS/$1/master" 2>/dev/null
	else
		rm -f "$SYS/$1/master"
	fi
}
answer() { echo "$1" >"$TMP/answer"; }
wanup() { : >"$TMP/wanup"; }
wanno() { rm -f "$TMP/wanup"; }
run() { sh "$SCRIPT" "$@" 2>>"$TMP/syslog"; }

reset() {
	: >"$DB"
	: >"$TMP/ip.log"
	: >"$TMP/probe.log"
	: >"$TMP/syslog"
	rm -f "$TMP/answer" "$TMP/wanup" "$TMP"/hold.*
	rm -rf "$E2631_LOCKDIR"
	setcfg e2631_ports.global.enabled 1
	setcfg e2631_ports.global.mode auto
	setcfg e2631_ports.global.probe_timeout 1
	setcfg e2631_ports.global.manual_port wan
	setcfg e2631_ports.global.assume_single_link 0
	setcfg network.wan.device wan
	setcfg network.wan6.device wan
	setcfg e2631_ports.global.candidates "lan1 lan2 lan3 wan"
	setcfg network.@device[0].name br-lan
	deletelist network.@device[0].ports
	for p in lan1 lan2 lan3 wan; do
		addlist network.@device[0].ports "$p"
		newmac "$p" "00:11:22:33:44:0${p#lan}"
		link "$p" 0
		bridge "$p" 1
	done
	rm -f "$SYS/wan/master"
}

# ---- 场景 ----------------------------------------------------------------

echo "== 1. netifd 说 WAN 还是 up：不探测、不动配置 =="
reset
wanup
link wan 1
# 故意让另一个口也插着线并且能拿到租约：只有真的按「WAN 已 up 就别动」处理，
# 才不会去抢它。
link lan2 1
answer lan2
out=$(run detect)
[ "$out" = "wan" ] || fail "detect 应为 wan，实际 '$out'"
[ -s "$TMP/probe.log" ] && fail "WAN 已经 up 却还去探测了：$(tr '\n' ' ' <"$TMP/probe.log")"
run auto >/dev/null
[ "$(getdb network.wan.device)" = "wan" ] || fail "wan.device 被改成了 $(getdb network.wan.device)"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan2 lan3 wan" ] || fail "网桥成员变了"
pass "现用上行口保持，不抢 netifd 的 DHCP 客户端"

echo "== 2. WAN 没起来，上行在 lan2：改绑 lan2，网桥只剩 lan1 lan3 wan =="
reset
wanno
link lan2 1
link wan 0
answer lan2
out=$(run detect)
[ "$out" = "lan2" ] || fail "detect 应为 lan2，实际 '$out'"
run auto >/dev/null
[ "$(getdb network.wan.device)" = "lan2" ] || fail "wan.device 应为 lan2，实际 $(getdb network.wan.device)"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan3 wan" ] || fail "网桥应为 'lan1 lan3 wan'，实际 '$(getdb 'network.@device[0].ports')'"
[ "$(getdb network.wan.macaddr)" = "$(cat "$SYS/wan/address")" ] || fail "WAN MAC 没有固化成原 wan 口的地址"
[ "$(getdb e2631_ports.global.wan_mac)" = "$(cat "$SYS/wan/address")" ] || fail "配置里没有记住 wan_mac"
pass "自动选口 + 网桥重排 + MAC 固化"

echo "== 3. netifd 认为 WAN 没起来，但现用口其实是通的：不许再去探它 =="
# 这条是为了钉住「同一个口上不能同时有两个 DHCP 客户端」。哪怕现用口真能拿到租约，
# 也不该由我们插手——那是 netifd 的事；我们只找别的口。
reset
wanno
link wan 1
link lan1 1
answer wan
out=$(run detect)
[ -z "$out" ] || fail "现用口不应被探测，detect 应为空，实际 '$out'"
grep -q "probe wan" "$TMP/probe.log" && fail "不该用 DHCP 去探现用 WAN 口"
pass "只探测其他口"

echo "== 4. 谁都没应答：不动作，且被探测的口要回到网桥 =="
reset
wanno
link lan1 1
link lan3 1
run auto >/dev/null
[ "$(getdb network.wan.device)" = "wan" ] || fail "没有应答却改了 wan.device"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan2 lan3 wan" ] || fail "没有应答却改了网桥"
grep -q "ip link set dev lan1 nomaster" "$TMP/ip.log" || fail "探测前没有把 lan1 摘下来"
grep -q "ip link set dev lan1 master br-lan" "$TMP/ip.log" || fail "探测后没有把 lan1 放回 br-lan"
pass "探测失败保持现状 + 网桥成员复原"

echo "== 5. manual 模式：固定绑 manual_port =="
reset
setcfg e2631_ports.global.mode manual
setcfg e2631_ports.global.manual_port lan3
link lan3 1
run auto >/dev/null
[ "$(getdb network.wan.device)" = "lan3" ] || fail "manual 模式应绑 lan3，实际 $(getdb network.wan.device)"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan2 wan" ] || fail "网桥应为 'lan1 lan2 wan'，实际 '$(getdb 'network.@device[0].ports')'"
pass "手动模式"

echo "== 6. assume_single_link=1、只有一根线、无 DHCP 应答（PPPoE/静态场景）=="
reset
wanno
setcfg e2631_ports.global.assume_single_link 1
link lan1 1
out=$(run detect)
[ "$out" = "lan1" ] || fail "单线兜底应选 lan1，实际 '$out'"
pass "单线兜底"

echo "== 7. assume_single_link=1 但有两根线：不猜 =="
reset
wanno
setcfg e2631_ports.global.assume_single_link 1
link lan1 1
link lan2 1
out=$(run detect)
[ -z "$out" ] || fail "两根线时不该猜，实际 '$out'"
pass "多线不猜"

echo "== 8. enabled=0：完全不动作 =="
reset
wanno
setcfg e2631_ports.global.enabled 0
link lan2 1
answer lan2
run auto >/dev/null
[ "$(getdb network.wan.device)" = "wan" ] || fail "禁用状态下仍然改了配置"
pass "总开关生效"

echo "== 9. DRY_RUN=1：只打印 uci 命令，不改配置 =="
reset
out=$(DRY_RUN=1 sh "$SCRIPT" apply lan2 2>/dev/null)
echo "$out" | grep -q "uci set network.wan.device=lan2" || fail "DRY_RUN 没打印设置 wan.device"
echo "$out" | grep -q "uci add_list network.@device\[0\].ports=lan1" || fail "DRY_RUN 没打印桥成员"
[ "$(getdb network.wan.device)" = "wan" ] || fail "DRY_RUN 竟然真的改了配置"
pass "dry-run 不改配置"

echo "== 10. 探测期间借用 WAN 的 MAC，结束后还原 =="
reset
wanno
link lan2 1
answer lan2
: >"$TMP/ip.log"
run detect >/dev/null
grep -q "ip link set dev lan2 address $(cat "$SYS/wan/address")" "$TMP/ip.log" \
	|| fail "探测时没有临时换成 WAN 的 MAC"
grep -q "ip link set dev lan2 address $(cat "$SYS/lan2/address")" "$TMP/ip.log" \
	|| fail "探测后没有把 MAC 还回去"
pass "MAC 借用/恢复"

echo "== 11. LuCI 的 rpcd 后端：JSON 必须是对的 =="
# 页面拿到的就是这个 JSON，字段写错在设备上只会表现成"页面空白"，
# 所以在这里用 python 解一遍，顺带核对几个关键字段。
RPCD="$HERE/../overlay/package/luci/luci-app-e2631-wanport/root/usr/libexec/rpcd/e2631-wanport"
if [ -f "$RPCD" ] && command -v python3 >/dev/null 2>&1; then
	reset
	wanup
	setcfg e2631_ports.global.wan_mac "00:11:22:33:44:55"
	out=$(E2631_WANPORT="$SCRIPT" sh "$RPCD" call status 2>>"$TMP/syslog")
	echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["installed"] is True, d
assert d["device"] == "wan", d
assert d["wan_up"] is True, d
assert d["bridge"] == "br-lan", d
assert d["wan_mac"] == "00:11:22:33:44:55", d
assert len(d["ports"]) == 4, d
names = [ p["name"] for p in d["ports"] ]
assert names == [ "lan1", "lan2", "lan3", "wan" ], names
roles = { p["name"]: p["role"] for p in d["ports"] }
assert roles["wan"] == "wan" and roles["lan1"] == "lan", roles
labels = { p["name"]: p["label"] for p in d["ports"] }
assert labels["lan1"] == "LAN1" and labels["wan"] == "WAN", labels
' 2>>"$TMP/syslog" || fail "status 返回的不是预期 JSON：$out"

	# 未知端口必须被拒，而且不能把服务拉起来
	out=$(E2631_WANPORT="$SCRIPT" sh "$RPCD" call set_port <<'JSON' 2>>"$TMP/syslog"
{"port":"lan9"}
JSON
)
	echo "$out" | grep -q '"ok":false' || fail "未知端口没有被拒绝：$out"
	out=$(E2631_WANPORT="$SCRIPT" sh "$RPCD" list)
	echo "$out" | grep -q '"set_port"' || fail "list 里没有 set_port：$out"
	pass "rpcd 后端的 JSON 与参数校验"
else
	echo "skip: 没有 $RPCD 或 python3"
fi

echo "== 12. wan6 跟着走，但别把 @wan 别名写成设备名 =="
# 显式绑设备时 wan6 必须跟着换；绑 @wan 时整条都不能碰——写成设备名等于把别名
# 换成固定 device，之后 IPv6 就静默哑掉。
reset
run apply lan2 >/dev/null
[ "$(getdb network.wan6.device)" = "lan2" ] || fail "显式绑设备的 wan6 没跟上：$(getdb network.wan6.device)"
reset
setcfg network.wan6.device "@wan"
run apply lan3 >/dev/null
[ "$(getdb network.wan6.device)" = "@wan" ] || fail "wan6 的 @wan 别名被改成了 $(getdb network.wan6.device)"
[ "$(getdb network.wan.device)" = "lan3" ] || fail "wan 没换到 lan3：$(getdb network.wan.device)"
pass "wan6 显式设备跟随、@wan 别名不动"

echo "== 13. status / selftest 能跑通 =="
reset
wanup
run status >/dev/null 2>&1 || fail "status 退出码非 0"
run selftest >/dev/null 2>&1 || fail "selftest 退出码非 0"
pass "status / selftest"

echo
if [ "$FAILS" -eq 0 ]; then
	echo "全部通过"
	exit 0
fi
echo "$FAILS 项失败"
exit 1
