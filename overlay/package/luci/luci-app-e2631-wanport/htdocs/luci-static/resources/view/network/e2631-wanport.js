'use strict';
'require view';
'require dom';
'require form';
'require rpc';
'require poll';
'require ui';

/*
 * WAN/LAN adaptive: which wired port is the WAN.
 *
 * The vendor calls this "WAN/LAN 自适应" and drives a switch-chip ioctl from
 * cspd (see docs/wanlan-adaptive-逆向记录-2026-10-27.md). This board has four
 * separate GMAC netdevs, so back here the same feature is pure userspace: rebind
 * network.wan.device and move that port out of br-lan. /sbin/e2631-wanport does
 * the work; this page only reads its state and asks it to run.
 */

var callStatus = rpc.declare({
	object: 'e2631-wanport',
	method: 'status',
	expect: { '': {} }
});

var callSetPort = rpc.declare({
	object: 'e2631-wanport',
	method: 'set_port',
	params: [ 'port' ],
	expect: { '': {} }
});

var callRedetect = rpc.declare({
	object: 'e2631-wanport',
	method: 'redetect',
	expect: { '': {} }
});

var callReload = rpc.declare({
	object: 'e2631-wanport',
	method: 'reload',
	expect: { '': {} }
});

/* LuCI's themes style ".label.success" / ".label.warning"; there is no
 * ".label.danger", so "off/down" is a warning, not an error. */
function badge(text, cls) {
	return E('span', { 'class': 'label' + (cls ? ' ' + cls : '') }, text);
}

function linkText(p) {
	if (p.carrier !== true)
		return _('no cable');
	return p.speed && p.speed !== '-'
		? _('connected, %s Mb/s').format(p.speed)
		: _('connected');
}

function modeText(s) {
	return (s.mode === 'manual')
		? _('fixed port')
		: _('automatic (blind plug)');
}

return view.extend({
	load: function() {
		return callStatus();
	},

	renderPorts: function(node, s) {
		var self = this;
		var rows = [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Port')),
				E('th', { 'class': 'th' }, _('Role')),
				E('th', { 'class': 'th' }, _('Link')),
				E('th', { 'class': 'th' }, _('Bridge')),
				E('th', { 'class': 'th' }, _('MAC address')),
				E('th', { 'class': 'th' }, '')
			])
		];

		var ports = s.ports || [];
		if (!ports.length) {
			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'colspan': 6 },
					E('em', _('No candidate ports found: /sbin/e2631-wanport is not installed on this router.')))
			]));
		}

		ports.forEach(function(p) {
			var action = '';
			if (p.role !== 'wan') {
				action = E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(self, function() {
						return callSetPort(p.name).then(function(res) {
							if (res && res.ok)
								ui.addNotification(null, E('p',
									_('Switching the WAN to %s. The uplink drops for a few seconds while the bridge and the WAN interface are moved.').format(p.label)), 'info');
							else
								ui.addNotification(null, E('p',
									_('The switch could not be started: %s').format(res && res.error ? res.error : '?')), 'danger');
						});
					})
				}, _('Use as WAN'));
			}

			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', {}, p.label) ]),
				E('td', { 'class': 'td' }, [
					p.role === 'wan' ? badge(_('WAN'), 'success') : badge(_('LAN'))
				]),
				E('td', { 'class': 'td' }, linkText(p)),
				E('td', { 'class': 'td' }, p.master ? p.master : E('em', _('not in a bridge'))),
				E('td', { 'class': 'td' }, E('code', {}, p.mac || '-')),
				E('td', { 'class': 'td cbi-section-actions' }, action)
			]));
		});

		dom.content(node, [
			E('table', { 'class': 'table' }, rows),
			E('p', { 'class': 'cbi-section-descr' },
				_('LAN and WAN are roles here, not hardware. In automatic mode the router DHCP-probes the ports that have a cable and gives the WAN role to the one that is answered; in fixed mode it always uses the chosen port. The WAN MAC stays the same either way, so the lease and the IPv6 identity of this router survive a port change.'))
		]);
	},

	renderStatus: function(node, s) {
		var state = [];
		state.push(E('p', {}, [
			_('Mode'), ': ', E('strong', {}, modeText(s)),
			s.mode === 'manual' ? ' — ' + _('port %s').format(s.manual_port) : '',
			' (', E('em', {}, _('settings: %s').format(s.mode || 'auto')), ')'
		]));
		state.push(E('p', {}, [
			_('WAN'), ': ', E('strong', {}, s.device || '-'), ' — ',
			s.wan_up ? badge(_('up'), 'success') : badge(_('down'), 'warning')
		]));
		state.push(E('p', {}, [
			_('WAN MAC'), ': ', E('code', {}, s.wan_mac || '-'), ' ',
			_('Bridge'), ': ', E('code', {}, s.bridge || '-'), ' ',
			_('members'), ': ', E('code', {}, s.bridge_ports || '-')
		]));
		if (s.enabled === '0')
			state.push(E('p', {}, E('em', _('The service is switched off: nothing happens automatically until it is enabled below.'))));

		dom.content(node, state);
	},

	render: function(s) {
		var self = this;

		var statusBox = E('div', { 'class': 'cbi-section', 'id': 'e2631-wanport-status' });
		var portsBox = E('div', { 'class': 'cbi-section', 'id': 'e2631-wanport-ports' });

		this.renderStatus(statusBox, s);
		this.renderPorts(portsBox, s);

		poll.add(function() {
			return callStatus().then(function(res) {
				dom.content(statusBox, []);
				self.renderStatus(statusBox, res);
				self.renderPorts(portsBox, res);
			});
		}, 5);

		var redetect = E('button', {
			'class': 'btn cbi-button cbi-button-action',
			'click': ui.createHandlerFn(this, function() {
				return callRedetect().then(function(res) {
					if (res && res.ok)
						ui.addNotification(null, E('p',
							_('Detection started: the ports with a cable are probed one by one. The result appears in the table above within a few seconds.')), 'info');
					else
						ui.addNotification(null, E('p',
							_('Detection could not be started: %s').format(res && res.error ? res.error : '?')), 'danger');
				});
			})
		}, _('Detect now'));

		var m = new form.Map('e2631_ports', _('WAN port'),
			_('Any of the four wired ports can carry the uplink. This is the same feature the factory calls "WAN/LAN adaptive", implemented without touching the switch: a port is a WAN port when the WAN interface is bound to it and it is out of the LAN bridge.'));

		var s2 = m.section(form.NamedSection, 'global', 'ports');
		s2.addremove = false;
		var o;

		o = s2.option(form.Flag, 'enabled', _('Enable'));
		o.default = '1';
		o.rmempty = false;
		o.description = _('Off: the WAN stays where it is, however the cables are plugged.');

		o = s2.option(form.ListValue, 'mode', _('Mode'));
		o.value('auto', _('Automatic (blind plug)'));
		o.value('manual', _('Fixed port'));
		o.default = 'auto';
		o.rmempty = false;
		o.description = _('Automatic: whichever port has the uplink cable takes the WAN role. Fixed: always the port below (use this for PPPoE or a static address upstream, which cannot be detected).');

		o = s2.option(form.ListValue, 'manual_port', _('Fixed WAN port'));
		(s.ports || []).forEach(function(p) {
			o.value(p.name, p.label);
		});
		o.default = 'wan';
		o.depends('mode', 'manual');
		o.description = _('Only used in fixed mode.');

		o = s2.option(form.Value, 'probe_timeout', _('Probe timeout (seconds)'));
		o.datatype = 'range(1,15)';
		o.default = '3';
		o.optional = true;
		o.description = _('How long each port is given to answer the probe. The worst case of a full scan is this times four.');

		o = s2.option(form.Flag, 'assume_single_link', _('Guess when only one cable is plugged in'));
		o.default = '0';
		o.optional = true;
		o.description = _('Automatic mode only finds an uplink that answers DHCP; PPPoE and static uplinks do not. With this on, a single cable that is not in the current WAN port is taken as the uplink. It also fires when a lone computer is the only thing plugged in, which would then lose its LAN connection — that is why it is off by default.');

		return m.render().then(function(formNode) {
			var head = E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Current state')), statusBox,
				E('div', { 'class': 'cbi-page-actions', 'style': 'text-align:left' }, redetect)
			]);
			var ports = E('div', { 'class': 'cbi-section' }, [ E('h3', _('Ports')), portsBox ]);
			return E([ head, ports, formNode ]);
		});
	},

	/* The saved settings only take effect when the service runs once more, and
	 * the page reloads right after an apply, so start it before applying: manual
	 * mode then binds the chosen port immediately, automatic mode probes again. */
	handleSaveApply: function(ev, mode) {
		return this.handleSave(ev).then(function() {
			return callReload();
		}).then(function() {
			ui.changes.apply(mode == '0');
		});
	}
});
