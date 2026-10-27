# SPDX-License-Identifier: GPL-2.0-only
#
# Built out of tree by the kernel's own Kbuild (see ../Makefile), because the
# module has to match the kernel it loads into.
obj-m += zx279128s-ppe.o

# zx279128s-eth.h and zx279128s-ppe.h live next to the built-in driver that
# includes them, and that directory is the single source for both sides.
ccflags-y += -I$(srctree)/drivers/net/ethernet/zte
