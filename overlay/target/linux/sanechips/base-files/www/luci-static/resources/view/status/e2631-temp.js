'use strict';
'require view';
'require fs';
'require dom';
'require ui';

/*
 * 状态 → 系统温度（全部已知传感器）
 *
 * 本板内核能报出来的温度一共三路（2026-11-04 在盒子上实测枚举）：
 *
 *   1) SoC（ZX279128S 片内传感器）
 *      /sys/kernel/debug/92000000.ethernet/soc_temp
 *        temp_c=39
 *        raw=758 samples=10 t=1147
 *      temp_c 是驱动分段线性校正后的摄氏度整数（分辨率 1 °C）。cat 这个文件本身就
 *      完成"触发一次转换 + 取平均"，只读，不写任何寄存器、不改任何配置。
 *
 *   2) WiFi 2.4G（mt7915 phy0 的 hwmon）
 *      .../ieee80211/phy0/hwmon0/{name,temp1_input} → name=mt7915_phy0, 31000
 *
 *   3) WiFi 5G（mt7915 phy1 的 hwmon）
 *      .../ieee80211/phy1/hwmon1/{name,temp1_input} → name=mt7915_phy1, 31000
 *
 * hwmon 的 temp*_input 单位是毫摄氏度（31000 = 31.0 °C）。本板没有 thermal zone、
 * 没有 iio 设备、debugfs 里除 soc_temp 外也没有别的温度节点，所以上面就是全部。
 *
 * 为什么这里用 /sys/devices/... 下的真实路径而不是 /sys/class/hwmon/hwmonN：
 * 浏览器不能直读 debugfs/sysfs，读操作走 rpcd 的 file.read，rpcd 的 file 插件会先
 * realpath() 再把**解析后的真实路径**拿去和 ACL 白名单 glob 比对（实测：白名单写
 * /sys/class/hwmon/... 时读 class 路径一律 result[6] 拒绝，写真实路径后同样读
 * class 路径仍然被拒、读真实路径才成功）。所以本文件读真实路径，ACL 也按真实路径
 * 写窄 glob；/sys/class/hwmon/hwmon0 只是给人看的软链接。
 *
 * 白名单：/usr/share/rpcd/acl.d/luci-app-e2631-temp.json（只读、窄 glob）。
 * 某一路读不到（路径不在/权限不够/驱动没注册）时只有那一行显示"不可用"，整页照常
 * 渲染，不抛异常。
 */
var SOC_PATH = '/sys/kernel/debug/92000000.ethernet/soc_temp';

/* mt7915 两个 phy 的 hwmon 真实路径前缀（f000000.pcie = 本板 PCIe 控制器，
 * 0000:01:00.0 = mt7915；换板/换 DT 时这一段要跟着改） */
var WIFI_TREE = '/sys/devices/platform/soc/f000000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0/ieee80211';

/* hwmon 的 name → 人类可读名字（按本板实测的物理含义标注） */
var HWMON_LABEL = {
	'mt7915_phy0': 'WiFi 2.4G (mt7915 / phy0)',
	'mt7915_phy1': 'WiFi 5G (mt7915 / phy1)'
};

/* 已知的三路（实测枚举结果）。 */
var SENSORS = [
	{ id: 'soc',   type: 'soc',   label: 'SoC (ZX279128S)' },
	{ id: 'wifi2', type: 'hwmon', dir: WIFI_TREE + '/phy0/hwmon0', label: 'WiFi 2.4G (mt7915)' },
	{ id: 'wifi5', type: 'hwmon', dir: WIFI_TREE + '/phy1/hwmon1', label: 'WiFi 5G (mt7915)' }
];

function errorText(err) {
	return (err && err.message) ? err.message : String(err);
}

function readText(path) {
	return fs.read(path).then(function(text) {
		return String(text || '');
	});
}

/* 整数就显示整数，有小数才显示一位（不伪造精度） */
function fmtCelsius(c) {
	if (c === null || c === undefined || isNaN(c))
		return null;
	var r = Math.round(c);
	return (Math.abs(c - r) < 1e-6) ? String(r) : c.toFixed(1);
}

function parseSocRaw(text) {
	var s = String(text || '');
	var c = /\btemp_c=(-?[0-9]+)\b/.exec(s);
	var r = /\braw=([0-9]+)\s+samples=([0-9]+)\s+t=(-?[0-9]+)\b/.exec(s);
	var det = [];

	/* 3-detector 驱动会追加 det1_c/det2_c 两行；旧驱动没有这两行，
	 * 这里就什么都不返回，页面结构与现在完全一致。 */
	[1, 2].forEach(function(n) {
		var m = new RegExp('\\bdet' + n + '_c=(-?[0-9]+|unavailable)\\b').exec(s);

		if (!m)
			return;

		det.push({
			n: n,
			label: 'SoC detector' + n,
			ok: m[1] !== 'unavailable',
			value: m[1] === 'unavailable' ? null : fmtCelsius(parseInt(m[1], 10)),
			why: m[1] === 'unavailable' ? '驱动这一路采样失败或超时' : null
		});
	});

	return {
		c: c ? parseInt(c[1], 10) : null,
		raw: r ? { raw: r[1], samples: r[2], t: r[3] } : null,
		det: det
	};
}

/* 读一路。任何失败都 resolve 成 ok:false，绝不 reject。 */
function readSensor(s) {
	if (s.type === 'soc') {
		return readText(SOC_PATH).then(function(text) {
			var p = parseSocRaw(text);
			return {
				id: s.id, label: s.label, ok: p.c !== null,
				value: fmtCelsius(p.c), raw: p.raw, text: text,
				det: p.det || [],
				source: SOC_PATH,
				note: p.c === null ? '输出里没有 temp_c 字段' : null
			};
		}).catch(function(err) {
			return {
				id: s.id, label: s.label, ok: false, value: null,
				source: SOC_PATH, note: errorText(err)
			};
		});
	}

	return readText(s.dir + '/name').then(function(name) {
		name = name.replace(/\s+$/, '');
		var label = HWMON_LABEL[name] || (name + ' (hwmon)');

		return readText(s.dir + '/temp1_input').then(function(v) {
			var milli = parseInt(String(v).replace(/\s+$/, ''), 10);
			var c = isNaN(milli) ? null : milli / 1000;
			return {
				id: s.id, label: label, ok: c !== null,
				value: fmtCelsius(c), name: name,
				source: s.dir + '/temp1_input',
				note: c === null ? 'temp1_input 不是数字：' + String(v).replace(/\s+$/, '') : null
			};
		}).catch(function(err) {
			return {
				id: s.id, label: label, ok: false, value: null, name: name,
				source: s.dir + '/temp1_input', note: errorText(err)
			};
		});
	}).catch(function(err) {
		return {
			id: s.id, label: s.label, ok: false, value: null,
			source: s.dir, note: '读不到 hwmon：' + errorText(err)
		};
	});
}

function two(n) {
	return (n < 10 ? '0' : '') + n;
}

function hhmmss(d) {
	return two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds());
}

return view.extend({
	load: function() {
		return this.readAll();
	},

	/* 读全部。逐路 catch，任何一路失败都不影响其它行。 */
	readAll: function() {
		return Promise.all(SENSORS.map(readSensor)).then(function(res) {
			return { at: new Date(), sensors: res };
		});
	},

	render: function(st) {
		var self = this;

		this.box = E('div', { 'class': 'cbi-section' });
		dom.content(this.box, this.renderSection(st));

		return E('div', {}, [
			E('h2', {}, '系统温度'),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, '全部已知温度传感器'),
				E('p', { 'class': 'cbi-section-descr' },
					'本页把内核能报出来的温度全部列出来：SoC 片内传感器（驱动 debugfs）' +
					'和 mt7915 WiFi 芯片的两个 hwmon 传感器。全部只读，不写任何寄存器、不改任何配置。'),
				this.box
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, '说明'),
				E('p', { 'class': 'cbi-section-descr' },
					'SoC 温度的分辨率是 1 °C（驱动整数运算），所以这一路不显示小数；' +
					'hwmon 的 temp*_input 是毫摄氏度，整数度也只显示整数，有小数才显示一位。'),
				E('p', { 'class': 'cbi-section-descr' },
					'本页只读，不提供任何设置项。命令行等价写法：cat ' + SOC_PATH +
					'；cat /sys/class/hwmon/hwmon0/temp1_input（单位 m°C）。')
			])
		]);
	},

	renderSection: function(st) {
		var self = this;
		var lines = [];
		var soc = null;

		st.sensors.forEach(function(s) {
			if (s.id === 'soc')
				soc = s;
		});

		/* 1) 传感器表 */
		var rows = [ E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, '传感器'),
			E('th', { 'class': 'th' }, '当前温度'),
			E('th', { 'class': 'th' }, '状态')
		]) ];

		st.sensors.forEach(function(s) {
			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'style': 'white-space:nowrap', 'title': s.source || '' }, s.label),
				E('td', { 'class': 'td' }, s.ok ? s.value + ' °C' : '—'),
				E('td', { 'class': 'td' }, s.ok ? '可用' : '不可用' + (s.note ? '（' + s.note + '）' : ''))
			]));

			/* 只有驱动真的输出了 detN_c 才多渲染这一行：旧驱动的输出里没有
			 * 这两行，s.det 为空，页面与改动前完全一致。 */
			(s.det || []).forEach(function(d) {
				rows.push(E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td', 'style': 'white-space:nowrap', 'title': s.source || '' }, d.label),
					E('td', { 'class': 'td' }, d.ok ? d.value + ' °C' : '—'),
					E('td', { 'class': 'td' }, d.ok ? '可用' : '不可用' + (d.why ? '（' + d.why + '）' : ''))
				]));
			});
		});

		lines.push(E('table', { 'class': 'table', 'style': 'width:100%' }, rows));

		/* 2) SoC 驱动原始输出（保留原来那行） */
		if (soc && soc.ok) {
			lines.push(E('p', { 'class': 'cbi-section-descr' }, [
				'SoC 驱动原始输出：',
				E('code', {}, String(soc.text).replace(/\s+$/, '').replace(/\s*\n\s*/g, ' '))
			]));
			if (soc.raw)
				lines.push(E('p', { 'class': 'cbi-section-descr' },
					'ADC 平均值 ' + soc.raw.raw + '，有效样本 ' + soc.raw.samples + ' 个，中间值 t=' + soc.raw.t +
					'（驱动温度换算的分辨率是 1 °C，所以这一路小数位恒为空）。'));
		}
		else if (soc) {
			lines.push(E('p', { 'class': 'cbi-section-descr' },
				'读不到 ' + SOC_PATH + (soc.note ? '（' + soc.note + '）' : '') + '。' +
				'常见原因：debugfs 没挂载、内核里没有这个文件，或 rpcd 缺少该路径的读取权限。' +
				'其它传感器不受影响。'));
		}

		/* 3) 刷新 */
		lines.push(E('p', {
			'style': 'display:flex;gap:.6em;align-items:center;flex-wrap:wrap;margin:.6em 0'
		}, [
			E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'click': ui.createHandlerFn(self, function(ev) {
					var btn = ev.target;

					btn.disabled = true;
					return self.readAll().then(function(next) {
						dom.content(self.box, self.renderSection(next));
					}).catch(function() {
						btn.disabled = false;
					});
				})
			}, '刷新'),
			E('span', { 'class': 'cbi-section-descr' }, '最后读取：' + hhmmss(st.at))
		]));

		return lines;
	}
});
