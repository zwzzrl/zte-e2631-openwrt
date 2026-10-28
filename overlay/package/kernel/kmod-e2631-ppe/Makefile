# SPDX-License-Identifier: GPL-2.0-only
#
# The packet processor (PPE) engine of the zx279128s switch driver, built as a
# module instead of as part of the built-in driver.
#
# Why: the classifier tables, the two hash banks, the rewrite tables and the
# IPv6 tables are the part that keeps being worked on, and every edit under
# target/linux/sanechips/files/... invalidates the kernel's prepared stamp, so
# the whole kernel plus all ~1900 objects are rebuilt (~45 minutes on this
# machine).  As a package the loop is
#
#   make package/kernel/kmod-e2631-ppe/compile     (seconds)
#   scp build_dir/.../kmod-e2631-ppe/zx279128s-ppe.ko root@router:/tmp/
#   modprobe -r zx279128s_ppe; insmod /tmp/zx279128s-ppe.ko; /etc/init.d/firewall restart
#
# What cannot move across the boundary is anything the kernel holds a pointer
# to.  That is why ndo_setup_tc and the flow_block callbacks stay in the
# built-in driver (zx279128s-main.c) and reach this module through the hooks
# table described in zx279128s-ppe.h: an unload then leaves TC with a valid
# callback that simply reports -EOPNOTSUPP, instead of a dangling one.
#
# The shared headers stay where the built-in driver needs them, in the kernel
# tree at target/linux/sanechips/files/drivers/net/ethernet/zte/; src/Makefile
# adds that directory to the include path rather than keeping a second copy
# that could drift from the one the kernel compiles against.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=kmod-e2631-ppe
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/e2631-ppe
  SUBMENU:=Network Support
  TITLE:=ZTE zx279128s packet processor (hardware flow offload)
  FILES:=$(PKG_BUILD_DIR)/zx279128s-ppe.ko
  # NOT autoloaded.  /sbin/kmodloader would insmod this from /etc/init.d/boot,
  # i.e. before the network exists - and 2026-10-29 showed what that costs: an
  # image that autoloaded the engine did not come up at all, no ARP, no ssh,
  # and the only way back in was a serial console.  Loading is now a late,
  # guarded action of its own (base-files: /etc/init.d/e2631-ppe, START=99),
  # which writes a pending marker first and clears it only after the box has
  # demonstrably stayed alive; a boot that finds the marker still set skips the
  # load, so a bad engine can never lock the box out twice in a row.
  AUTOLOAD:=
  DEPENDS:=@TARGET_sanechips
endef

define KernelPackage/e2631-ppe/description
 Programs the classifier, the packet modifier and the flow tables of the
 zx279128s switch, so established flows are forwarded without the CPU.
 Part of the Ethernet switch support; without it the switch driver works and
 every flow takes the software path.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,e2631-ppe))
