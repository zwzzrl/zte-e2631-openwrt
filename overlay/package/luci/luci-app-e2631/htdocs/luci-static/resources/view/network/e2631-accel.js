'use strict';
'require view';
'require dom';
'require poll';
'require rpc';
'require ui';

/*
 * 转发加速：显示每族的加速路径，并给一个受框架保护的切换入口。
 *
 * 状态只有驱动里的 accel 框架（zx279128s-accel.c）能改，页面写的是驱动自己的
 * debugfs 旋钮（经 rpcd 的 accel_set）。之所以不直接 echo：IPv4 的硬件 offload
 * 与加速路径是绑在一起的。内核只看 flowtable 绑没绑（ppe_bound），不看
 * accel_ipv4 的取值，所以"选 stack"也必须先把 flow_offloading_hw 关掉，否则
 * PPE 照样接管 v4 流量。rpcd 里那一步是复合动作：按目标模式改 uci
 * （flow_offloading_hw 只在 hw 档为 1）、reload fw4、等 ppe_bound 落到目标值、
 * 再写模式，任一步失败回滚。切回 hw 是对称的。
 *
 * 默认 v4=hw、v6=stack、zc 关；命令行的等价写法见页面下方提示。
 */

var callAccel = rpc.declare({
	object: 'e2631',
	method: 'accel',
	expect: { '': {} }
});

var callAccelSet = rpc.declare({
	object: 'e2631',
	method: 'accel_set',
	params: [ 'family', 'mode' ],
	expect: { '': {} }
});

function badge(text, cls) {
	return E('span', { 'class': 'label' + (cls ? ' ' + cls : '') }, text);
}

/* 计数器是驱动给的原始 u64，缺字段时当 0，不显示 undefined */
function counter(c, k) {
	var v = c ? c[k] : null;
	return (typeof v === 'number') ? String(v) : '0';
}

var MODE_DESC = {
	stack: '标准协议栈',
	ffe: '驱动内快路（P1 只判定、不转发）',
	hw: 'PPE 硬件流表（仅 IPv4）',
	zc: '零拷贝直通'
};

var V4_MODES = [ 'stack', 'hw', 'ffe', 'zc' ];
var V6_MODES = [ 'stack', 'ffe', 'zc' ];

return view.extend({
	load: function() {
		return callAccel();
	},

	renderStatus: function(node, s) {
		var c = s && s.counters ? s.counters : {};
		var avail = s && s.avail === true;
		var v4 = (s && s.v4) || '?';
		var v6 = (s && s.v6) || '?';
		var bound = s && (s.ppe_bound === 1 || s.ppe_bound === '1');
		var lines = [];

		lines.push(E('p', {}, [
			'IPv4：', avail ? badge(v4 + '（' + (MODE_DESC[v4] || '未知模式') + '）',
				v4 === 'hw' ? 'success' : (v4 === 'zc' ? 'warning' : '')) : badge('无法读取', 'warning'),
			'   ',
			'IPv6：', avail ? badge(v6 + '（' + (MODE_DESC[v6] || '未知模式') + '）',
				v6 === 'zc' ? 'warning' : '') : badge('无法读取', 'warning')
		]));

		if (avail) {
			lines.push(E('p', {}, [
				'PPE 流表：', badge(bound ? '已绑定（flow_offloading_hw 开）' : '未绑定',
					bound ? 'success' : ''),
				'   ',
				'自动降级：', badge(s.autofallback === 1 || s.autofallback === '1' ? '开' : '关', ''),
				'   ',
				'硬件 offload：', E('code', {}, 'flow_offloading_hw=' + (s.flow_hw === undefined ? '?' : s.flow_hw)),
				'   ',
				'zc 族掩码：', E('code', {}, 'zc_mask=' + (s.zc_mask === undefined ? '?' : s.zc_mask))
			]));

			if (bound)
				lines.push(E('p', { 'class': 'cbi-section-descr' },
					'PPE 正绑着 flowtable：此时切 IPv4 到 ffe 或 zc 会被驱动拒绝（EBUSY）。' +
					'下面的切换动作会先把 flow_offloading_hw 关掉、重载防火墙，等 PPE 解绑后再切。'));

			lines.push(E('p', { 'class': 'cbi-section-descr' },
				'flow_offloading_hw 由加速框架维护：只有 IPv4 选 hw 时才保持为 1；选 stack、ffe 或 zc ' +
				'都会把它关掉。内核只看 flowtable 绑没绑、不看 accel_ipv4 的取值，所以"stack"如果不关' +
				'硬件 offload，PPE 其实还在接管 v4 流量。开机默认 IPv4=hw，因此默认是开着的。'));
		}

		lines.push(E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, '接收直通'),
				E('th', { 'class': 'th' }, '发送直通'),
				E('th', { 'class': 'th' }, '栈内释放'),
				E('th', { 'class': 'th' }, '未归还'),
				E('th', { 'class': 'th' }, '回退拷贝'),
				E('th', { 'class': 'th' }, '队列溢出'),
				E('th', { 'class': 'th' }, '被限流'),
				E('th', { 'class': 'th' }, '关闭次数')
			]),
			E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, counter(c, 'rx_ok')),
				E('td', { 'class': 'td' }, counter(c, 'tx_ok')),
				E('td', { 'class': 'td' }, counter(c, 'free_ok')),
				E('td', { 'class': 'td' }, counter(c, 'out')),
				E('td', { 'class': 'td' }, counter(c, 'fallback')),
				E('td', { 'class': 'td' }, counter(c, 'q_ovf')),
				E('td', { 'class': 'td' }, counter(c, 'capped')),
				E('td', { 'class': 'td' }, counter(c, 'off'))
			])
		]));

		if (!avail)
			lines.push(E('p', { 'class': 'cbi-section-descr' },
				'读不到 /sys/kernel/debug/92000000.ethernet/accel_ipv4。debugfs 没挂载时属正常：' +
				'mount -t debugfs none /sys/kernel/debug 之后再回来看。'));

		dom.content(node, lines);
	},

	/* 一个族的模式选择 + 应用按钮 */
	renderFamily: function(box, s, family, modes, label) {
		var self = this;
		var cur = (s && (family === 'v4' ? s.v4 : s.v6)) || 'stack';

		var sel = E('select', { 'class': 'cbi-input-select' },
			modes.map(function(m) {
				return E('option', { 'value': m, 'selected': m === cur ? '' : null },
					m + '（' + (MODE_DESC[m] || '?') + '）');
			}));

		var btn = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(self, function(ev) {
				var mode = sel.value;

				ev.target.disabled = true;
				return callAccelSet(family, mode).then(function(res) {
					if (res && res.ok === true) {
						ui.addNotification(null, E('p', {},
							label + ' 已切到 ' + mode + '，立即生效（无需重启）。'), 'info');
					} else if (res && res.ok === false) {
						ui.addNotification(null, E('p', {},
							label + ' 切换失败：' + (res.error || '?')), 'danger');
					} else {
						ui.addNotification(null, E('p', {},
							label + ' 切换结果不明：驱动没有回应。'), 'warning');
					}
					return callAccel().then(function(r2) {
						self.renderStatus(box, r2);
					});
				}).finally(function() { ev.target.disabled = false; });
			})
		}, '应用');

		return E('div', { 'style': 'display:flex;gap:.6em;align-items:center;flex-wrap:wrap;margin:.4em 0' }, [
			E('span', {}, label + '：'), sel, btn
		]);
	},

	render: function(s) {
		var self = this;
		var box = E('div', { 'class': 'cbi-section' });

		this.renderStatus(box, s);

		poll.add(function() {
			return callAccel().then(function(res) {
				self.renderStatus(box, res);
			});
		}, 5);

		return E('div', {}, [
			E('h2', {}, '转发加速'),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, '每族加速路径'),
				E('p', { 'class': 'cbi-section-descr' },
					'每种协议族各自选一条路径：标准栈、驱动内快路（ffe）、PPE 硬件流表（仅 IPv4）、' +
					'或零拷贝直通（zc）。状态只由驱动里的 accel 框架维护，页面经它的 debugfs 旋钮切换。'),
				this.renderFamily(box, s, 'v4', V4_MODES, 'IPv4'),
				this.renderFamily(box, s, 'v6', V6_MODES, 'IPv6'),
				E('p', { 'class': 'cbi-section-descr' },
					'注意：IPv4 切到 stack、ffe 或 zc 都会先关掉硬件 offload（flow_offloading_hw=0 并重载防火墙），' +
					'只有切到 hw 才重新打开它——因为内核只看 flowtable 绑没绑，不看模式名。任一步失败会回滚 uci，' +
					'盒子上不会留下"ffe + PPE 还绑着"或"stack + PPE 还在跑"的组合。'),
				E('p', { 'class': 'cbi-section-descr' },
					'命令行等价写法：ubus call e2631 accel_set \'{"family":"v4","mode":"ffe"}\'；' +
					'只想直接看/写驱动旋钮也可以：cat /sys/kernel/debug/92000000.ethernet/accel_ipv4 。')
			]),
			box
		]);
	}
});
