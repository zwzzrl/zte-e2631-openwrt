'use strict';
'require view';
'require dom';
'require poll';
'require rpc';
'require ui';

/*
 * 转发加速：零拷贝直通的开关键。
 *
 * 这一页只做三件事：读驱动当前状态、改 uci 里的意图、把 uci 立刻落到驱动上
 * （/etc/init.d/e2631-netune reload）。所有真正的写入都在那个初始化脚本里，
 * 页面不直接写 debugfs，免得开机与"网页点一下"两条路径各写一套。
 *
 * 默认开：2026-10-30 的实测显示转发场景收益明显（路由 UDP 900M 单流
 * 612 -> 741 Mbit/s，8 流 TCP 565 -> 700 Mbit/s），代价是本机收包场景
 * 吞吐下降（本机作为服务端 471 -> 218 Mbit/s），所以把它做成用户可选项而
 * 不是写死。
 */

var callAccel = rpc.declare({
	object: 'e2631',
	method: 'accel',
	expect: { '': {} }
});

var callAccelSet = rpc.declare({
	object: 'e2631',
	method: 'accel_set',
	params: [ 'enabled' ],
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

return view.extend({
	load: function() {
		return callAccel();
	},

	renderStatus: function(node, s) {
		var c = s && s.counters ? s.counters : {};
		var avail = s && s.avail === true;
		var on = s && s.drv === '1';
		var lines = [];

		lines.push(E('p', {}, [
			'当前模式：',
			avail
				? (on ? badge('零拷贝直通', 'success') : badge('驱动内拷贝', 'warning'))
				: badge('无法读取驱动状态', 'warning'),
			'  ',
			'配置值：', E('code', {}, (s && s.uci) || '1')
		]));

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
				'驱动没有给出 /sys/kernel/debug/92000000.ethernet/zc。若 debugfs 没挂载属正常：开机时由 /etc/init.d/e2631-netune 挂载并应用这里的设置。'));

		dom.content(node, lines);
	},

	render: function(s) {
		var self = this;
		var box = E('div', { 'class': 'cbi-section' });

		this.renderStatus(box, s);

		var sel = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { 'value': '1', 'selected': (s && s.drv === '1') ? '' : null }, '开启（默认）'),
			E('option', { 'value': '0', 'selected': (s && s.drv === '0') ? '' : null }, '关闭')
		]);

		var btn = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(self, function(ev) {
				var v = sel.value;
				ev.target.disabled = true;
				return callAccelSet(v).then(function(res) {
					if (res && res.avail === true) {
						ui.addNotification(null, E('p', {},
							v === '1' ? '零拷贝已开启，立即生效（无需重启）。'
							          : '零拷贝已关闭，立即生效（无需重启），GRO 同时恢复。'), 'info');
					} else if (res && res.ok === false) {
						ui.addNotification(null, E('p', {},
							'设置失败：' + (res.error || '?')), 'danger');
					} else {
						ui.addNotification(null, E('p', {},
							'已保存到 /etc/config/e2631，但驱动现在不可写（debugfs 未挂载？）。重启后由 e2631-netune 应用。'), 'warning');
					}
					return callAccel().then(function(r2) {
						self.renderStatus(box, r2);
					});
				}).finally(function() { ev.target.disabled = false; });
			})
		}, '应用');

		poll.add(function() {
			return callAccel().then(function(res) {
				self.renderStatus(box, res);
			});
		}, 5);

		return E('div', {}, [
			E('h2', {}, '转发加速'),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, '零拷贝直通'),
				E('p', { 'class': 'cbi-section-descr' },
					'开启后，收包时网卡 DMA 池里的缓冲直接当成内核 skb 的数据区交给协议栈，发包时把同一个缓冲直接交回硬件，省掉每包两次内存搬运（接收拷入、发送拷出）。' +
					'代价是本机自己消费数据的场景（SMB/FTP 到本机、本机做大流量服务端）吞吐会下降，而这正是它做成开关的原因。'),
				E('div', { 'style': 'display:flex;gap:.6em;align-items:center;flex-wrap:wrap' }, [
					E('span', {}, '零拷贝：'), sel, btn
				]),
				E('p', { 'class': 'cbi-section-descr' },
					'转发（路由/桥接经过本机）场景实测：UDP 900M 单流 612 → 741 Mbit/s，8 流 TCP 565 → 700 Mbit/s。' +
					'关闭后行为与旧版一致。两种状态都不影响原厂槽，也不影响 IPv4/IPv6 硬件快转。')
			]),
			box
		]);
	}
});
