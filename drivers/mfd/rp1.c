// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 EPAM Systems
 *
 * Derived from linux rp1 driver
 * Copyright (c) 2018-22 Raspberry Pi Ltd.
 */

#include <asm/io.h>
#include <dm.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/of_access.h>
#include <dt-bindings/mfd/rp1.h>
#include <linux/io.h>
#include <linux/types.h>
#include <pci.h>

#define RP1_B0_CHIP_ID 0x10001927
#define RP1_C0_CHIP_ID 0x20001927

#define RP1_PLATFORM_ASIC BIT(1)
#define RP1_PLATFORM_FPGA BIT(0)

#define RP1_DRIVER_NAME "rp1"

#define PCI_DEVICE_REV_RP1_C0 2

#define SYSINFO_CHIP_ID_OFFSET	0x00000000
#define SYSINFO_PLATFORM_OFFSET	0x00000004

struct rp1_dev {
	phys_addr_t bar_start;
};

static inline dma_addr_t rp1_io_to_phys(struct rp1_dev *rp1, unsigned int offset)
{
	return rp1->bar_start + offset;
}

static u32 rp1_reg_read(struct rp1_dev *rp1, unsigned int base_addr, u32 offset)
{
	resource_size_t phys = rp1_io_to_phys(rp1, base_addr);
	void __iomem *regblock = ioremap(phys, 0x1000);
	u32 value = readl(regblock + offset);

	iounmap(regblock);
	return value;
}

static int rp1_get_bar_region(struct udevice *dev, phys_addr_t *bar_start)
{
	void *bar;

	bar = dm_pci_map_bar(dev, PCI_BASE_ADDRESS_1, 0, 0,
			     PCI_REGION_TYPE, PCI_REGION_MEM);
	if (!bar)
		return -ENOMEM;

	*bar_start = (phys_addr_t)bar;
	return 0;
}

static int rp1_probe(struct udevice *dev)
{
	int ret;
	struct rp1_dev *rp1 = dev_get_priv(dev);
	u32 chip_id, platform;
	u32 bar0_addr, bar1_addr;

	/* Turn on bus-mastering */
	dm_pci_clrset_config16(dev, PCI_COMMAND, 0, PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);

	/*
	 * Swap BAR0 and BAR1 addresses to match device tree expectations.
	 *
	 * The rp1_nexus DT node maps RP1 child device addresses (ethernet,
	 * GPIO, clocks, USB) through BAR1 starting at PCI bus address 0.
	 * This matches the Linux PCI subsystem's generic, size-sorted BAR
	 * allocation which places the larger BAR1 at the lowest address.
	 * The Linux RP1 driver itself has no BAR ordering logic — it simply
	 * maps BAR1 by index via pcim_iomap(pdev, 1, 0).
	 *
	 * U-Boot's PCI auto-config (dm_pciauto_setup_device in pci_auto.c)
	 * allocates BARs sequentially by index: BAR0 first at address 0,
	 * then BAR1 at the next naturally-aligned address. This places BAR1
	 * at a non-zero PCI bus address, breaking of_translate_address() for
	 * all RP1 child devices since the DT ranges assume BAR1 starts at 0.
	 *
	 * Fix: swap the auto-assigned addresses so BAR1 gets address 0
	 * (matching the DT) and BAR0 moves to BAR1's old address.
	 * Only BAR1 is used by RP1 drivers; BAR0 is never accessed.
	 */
	dm_pci_read_config32(dev, PCI_BASE_ADDRESS_0, &bar0_addr);
	dm_pci_read_config32(dev, PCI_BASE_ADDRESS_1, &bar1_addr);
	bar0_addr &= PCI_BASE_ADDRESS_MEM_MASK;
	bar1_addr &= PCI_BASE_ADDRESS_MEM_MASK;
	dm_pci_write_config32(dev, PCI_BASE_ADDRESS_1, bar0_addr);
	dm_pci_write_config32(dev, PCI_BASE_ADDRESS_0, bar1_addr);

	ret = rp1_get_bar_region(dev, &rp1->bar_start);
	if (ret)
		return ret;

	/* Get chip id */
	chip_id = rp1_reg_read(rp1, RP1_SYSINFO_BASE, SYSINFO_CHIP_ID_OFFSET);
	platform = rp1_reg_read(rp1, RP1_SYSINFO_BASE, SYSINFO_PLATFORM_OFFSET);
	dev_dbg(dev, "chip_id 0x%x%s\n", chip_id,
		(platform & RP1_PLATFORM_FPGA) ? " FPGA" : "");

	if (chip_id != RP1_C0_CHIP_ID) {
		dev_err(dev, "wrong chip id (%x)\n", chip_id);
		return -EINVAL;
	}

	return 0;
}

static int rp1_bind(struct udevice *dev)
{
	device_set_name(dev, RP1_DRIVER_NAME);

	/*
	 * Bind child devices from the device tree.
	 *
	 * The rp1_nexus DT node contains a pci_ep_bus (simple-bus) child
	 * which in turn holds RP1 peripheral devices: Ethernet (MACB),
	 * GPIO, clocks, and USB controllers. Without this call, the PCI
	 * subsystem binds the RP1 PCI device but never discovers the DT
	 * child nodes, resulting in "No ethernet found" at boot.
	 *
	 * The UCLASS_PCI_GENERIC uclass has no post_bind hook to scan
	 * children automatically (unlike UCLASS_PCI which uses
	 * dm_scan_fdt_dev as its post_bind). We must call it explicitly.
	 */
	return dm_scan_fdt_dev(dev);
}

static const struct pci_device_id dev_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_RPI, PCI_DEVICE_ID_RP1_C0), },
	{ 0, }
};

/*
 * No of_match table: the RP1 device must NOT be bound eagerly during
 * DT scanning of the PCIe controller's children.
 *
 * The rp1_nexus DT node sits directly under pcie@1000120000, but in
 * the actual PCI topology RP1 is behind a bridge on bus 1.  If
 * lists_bind_fdt() matched rp1_nexus during the DT scan (bus 0),
 * pci_uclass_child_post_bind() would assign it devfn=0 (via
 * uninitialized data — no reg property), causing pci_bind_bus_devices()
 * to confuse it with the root port bridge on the same bus/devfn.
 *
 * Instead, PCI enumeration discovers RP1 on bus 1 by hardware scan
 * and matches it through the U_BOOT_PCI_DEVICE table below.  The
 * pci_find_and_bind_driver() fallback search associates the rp1_nexus
 * ofnode (from the controller's DT children) with this PCI-discovered
 * device, so rp1_bind() can then scan the DT subtree for Ethernet,
 * GPIO, clocks, and USB children.
 */

static int rp1_pcie_read_config(const struct udevice *bus, pci_dev_t bdf,
				uint offset, ulong *valuep, enum pci_size_t size)
{
	/*
	 * Leaving this call because pci subsystem calls for read_config
	 * and produces error then this callback is not set.
	 * Just return 0 here.s
	 */
	*valuep = 0;
	return 0;
}

static const struct dm_pci_ops rp1_pcie_ops = {
	.read_config	= rp1_pcie_read_config,
};

U_BOOT_DRIVER(rp1_driver) = {
	.name			 = RP1_DRIVER_NAME,
	.id			   = UCLASS_PCI_GENERIC,
	.probe		 = rp1_probe,
	.bind			 = rp1_bind,
	.priv_auto = sizeof(struct rp1_dev),
	.ops			 = &rp1_pcie_ops,
};

U_BOOT_PCI_DEVICE(rp1_driver, dev_id_table);