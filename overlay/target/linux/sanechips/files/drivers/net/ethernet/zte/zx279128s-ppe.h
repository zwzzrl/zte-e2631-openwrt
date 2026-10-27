/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ZTE zx279128s Ethernet switch: the boundary between the built-in switch
 * driver and the packet-processor (PPE) engine.
 *
 * The switch driver is built into the kernel; the engine that drives the
 * classifier, the packet modifier and the flow tables is package
 * kmod-e2631-ppe, so that work on it rebuilds one .ko instead of the whole
 * kernel.  Everything the core calls into the engine goes through
 * zx_ppe_hooks.  The core owns the storage of that table, never the module:
 * a trampoline therefore always has a valid object to look at, even while the
 * module is being unloaded, and it holds a module reference for the duration
 * of every call (zx_ppe_hooks_get()).  A module that cannot be referenced
 * (on its way out) makes the call return -EOPNOTSUPP, which turns the offload
 * for that flow off instead of jumping into freed code.
 *
 * The engine owns the hardware: the classifier tables, the flow and rewrite
 * slots, and the adapter fields that track them.  The core owns everything
 * that runs before the module is loaded: the hash table those flows live in
 * (initialised at probe) and the net_device and DMA side of the driver.
 */

#ifndef _ZX279128S_PPE_H
#define _ZX279128S_PPE_H

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <net/flow_offload.h>

#include "zx279128s-eth.h"

/* One offloaded flow: a slot in one of the two classifier hash banks plus the
 * rewrite-table entry the packet modifier applies to its frames.
 */
struct zx_ppe_entry {
	struct rhash_head	node;
	unsigned long		cookie;
	u16			hash;	/* 0-255 bank 0, 256-383 bank 1 */
	u16			flow;
	u16			nh;
	u8			subnet;
};

/* Keyed by cookie.  Defined in zx279128s-main.c and not in the module: the
 * core driver initialises and destroys adapter->ppe_flows at probe and
 * teardown time, which happens whether or not this module is loaded.
 */
extern const struct rhashtable_params zx_ppe_ht_params;

struct zx_ppe_hooks {
	struct module	*owner;
	/* FLOW_CLS_REPLACE / DESTROY / STATS for one flow, called with the
	 * adapter's ppe_lock held by the caller.
	 */
	int  (*flow_cb)(struct zx_eth_adapter *adapter,
			struct flow_cls_offload *f);
	/* Program/undo the classifier, the ONU MAC table and the traps. */
	int  (*start)(struct zx_eth_adapter *adapter);
	void (*stop)(struct zx_eth_adapter *adapter);
};

/* Called from module_init/module_exit.  The core copies the table, so the
 * caller's object may live in the module; unregister only clears it if the
 * registered owner is the caller.
 */
int  zx_ppe_hooks_register(const struct zx_ppe_hooks *hooks);
void zx_ppe_hooks_unregister(const struct zx_ppe_hooks *hooks);

#endif /* _ZX279128S_PPE_H */
