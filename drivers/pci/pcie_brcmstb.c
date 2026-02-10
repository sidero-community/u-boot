// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom STB PCIe controller driver
 *
 * Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 * Based on upstream Linux kernel driver:
 * drivers/pci/controller/pcie-brcmstb.c
 * Copyright (C) 2009 - 2017 Broadcom
 *
 * Based driver by Nicolas Saenz Julienne
 * Copyright (C) 2020 Nicolas Saenz Julienne <nsaenzjulienne@suse.de>
 */

#include <asm/arch/acpi/bcm2711.h>
#include <errno.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <pci.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/log2.h>
#include <linux/iopoll.h>

/* PCIe parameters */
#define BRCM_NUM_PCIE_OUT_WINS				4

/* MDIO registers */
#define MDIO_PORT0					0x0
#define MDIO_DATA_MASK					0x7fffffff
#define MDIO_PORT_MASK					0xf0000
#define MDIO_PORT_EXT_MASK				0x200000
#define MDIO_REGAD_MASK					0xffff
#define MDIO_CMD_MASK					0x00100000
#define MDIO_CMD_READ					0x1
#define MDIO_CMD_WRITE					0x0
#define MDIO_DATA_DONE_MASK				0x80000000
#define SSC_REGS_ADDR					0x1100
#define SET_ADDR_OFFSET					0x1f
#define SSC_CNTL_OFFSET					0x2
#define SSC_CNTL_OVRD_EN_MASK				0x8000
#define SSC_CNTL_OVRD_VAL_MASK				0x4000
#define SSC_STATUS_OFFSET				0x1
#define SSC_STATUS_SSC_MASK				0x400
#define SSC_STATUS_PLL_LOCK_MASK			0x800

/* Register offset indices */
enum {
	RGR1_SW_INIT_1,
	EXT_CFG_INDEX,
	EXT_CFG_DATA,
	PCIE_HARD_DEBUG,
};

enum pcie_soc_base {
	BCM2711,
	BCM7712,
};

/**
 * struct pcie_cfg_data - per-SoC configuration data
 * @offsets: register offset table
 * @soc_base: SoC base type
 * @num_inbound_wins: number of supported inbound windows
 * @avoid_bridge_shutdown: if true, don't shut down bridge on remove
 */
struct pcie_cfg_data {
	const int *offsets;
	const enum pcie_soc_base soc_base;
	u8 num_inbound_wins;
	bool avoid_bridge_shutdown;
};

/**
 * struct brcm_pcie - the PCIe controller state
 * @base: Base address of memory mapped IO registers of the controller
 * @gen: Non-zero value indicates limitation of the PCIe controller operation
 *       to a specific generation (1, 2 or 3)
 * @ssc: true indicates active Spread Spectrum Clocking operation
 * @cfg: pointer to per-SoC configuration data
 */
struct brcm_pcie {
	void __iomem		*base;

	int			gen;
	bool			ssc;
	const struct pcie_cfg_data *cfg;
};

/* BCM2711 register offsets */
static const int pcie_offsets[] = {
	[RGR1_SW_INIT_1]	= 0x9210,
	[EXT_CFG_INDEX]		= 0x9000,
	[EXT_CFG_DATA]		= 0x8000,
	[PCIE_HARD_DEBUG]	= 0x4204,
};

/* BCM2712/BCM7712 register offsets */
static const int pcie_offsets_bcm7712[] = {
	[RGR1_SW_INIT_1]	= 0x9210,
	[EXT_CFG_INDEX]		= 0x9000,
	[EXT_CFG_DATA]		= 0x8000,
	[PCIE_HARD_DEBUG]	= 0x4304,
};

static const struct pcie_cfg_data bcm2711_cfg = {
	.offsets	= pcie_offsets,
	.soc_base	= BCM2711,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm2712_cfg = {
	.offsets	= pcie_offsets_bcm7712,
	.soc_base	= BCM7712,
	.num_inbound_wins = 10,
	.avoid_bridge_shutdown = true,
};

/**
 * brcm_pcie_encode_ibar_size() - Encode the inbound "BAR" region size
 * @size: The inbound region size
 *
 * This function converts size of the inbound "BAR" region to the non-linear
 * values of the PCIE_MISC_RC_BAR[123]_CONFIG_LO register SIZE field.
 *
 * Return: The encoded inbound region size
 */
static int brcm_pcie_encode_ibar_size(u64 size)
{
	int log2_in = ilog2(size);

	if (log2_in >= 12 && log2_in <= 15)
		/* Covers 4KB to 32KB (inclusive) */
		return (log2_in - 12) + 0x1c;
	else if (log2_in >= 16 && log2_in <= 36)
		/* Covers 64KB to 64GB, (inclusive) */
		return log2_in - 15;

	/* Something is awry so disable */
	return 0;
}

/**
 * brcm_pcie_rc_mode() - Check if PCIe controller is in RC mode
 * @pcie: Pointer to the PCIe controller state
 *
 * The controller is capable of serving in both RC and EP roles.
 *
 * Return: true for RC mode, false for EP mode.
 */
static bool brcm_pcie_rc_mode(struct brcm_pcie *pcie)
{
	u32 val;

	val = readl(pcie->base + PCIE_MISC_PCIE_STATUS);

	return (val & STATUS_PCIE_PORT_MASK) >> STATUS_PCIE_PORT_SHIFT;
}

/**
 * brcm_pcie_link_up() - Check whether the PCIe link is up
 * @pcie: Pointer to the PCIe controller state
 *
 * Return: true if the link is up, false otherwise.
 */
static bool brcm_pcie_link_up(struct brcm_pcie *pcie)
{
	u32 val, dla, plu;

	val = readl(pcie->base + PCIE_MISC_PCIE_STATUS);
	dla = (val & STATUS_PCIE_DL_ACTIVE_MASK) >> STATUS_PCIE_DL_ACTIVE_SHIFT;
	plu = (val & STATUS_PCIE_PHYLINKUP_MASK) >> STATUS_PCIE_PHYLINKUP_SHIFT;

	return dla && plu;
}

static int brcm_pcie_config_address(const struct udevice *dev, pci_dev_t bdf,
				    uint offset, void **paddress)
{
	struct brcm_pcie *pcie = dev_get_priv(dev);
	unsigned int pci_bus = PCI_BUS(bdf);
	unsigned int pci_dev = PCI_DEV(bdf);
	unsigned int pci_func = PCI_FUNC(bdf);
	int idx;

	/*
	 * Busses 0 (host PCIe bridge) and 1 (its immediate child)
	 * are limited to a single device each
	 */
	if (pci_bus < 2 && pci_dev > 0)
		return -EINVAL;

	/* Accesses to the RC go right to the RC registers */
	if (pci_bus == 0) {
		*paddress = pcie->base + offset;
		return 0;
	}

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie))
		return -EINVAL;

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(pci_bus, pci_dev, pci_func, 0);

	writel(idx, pcie->base + pcie->cfg->offsets[EXT_CFG_INDEX]);
	*paddress = pcie->base + pcie->cfg->offsets[EXT_CFG_DATA] + offset;

	return 0;
}

static int brcm_pcie_read_config(const struct udevice *bus, pci_dev_t bdf,
				 uint offset, ulong *valuep,
				 enum pci_size_t size)
{
	return pci_generic_mmap_read_config(bus, brcm_pcie_config_address,
					    bdf, offset, valuep, size);
}

static int brcm_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				  uint offset, ulong value,
				  enum pci_size_t size)
{
	return pci_generic_mmap_write_config(bus, brcm_pcie_config_address,
					     bdf, offset, value, size);
}

static const char *link_speed_to_str(unsigned int cls)
{
	switch (cls) {
	case PCI_EXP_LNKSTA_CLS_2_5GB: return "2.5";
	case PCI_EXP_LNKSTA_CLS_5_0GB: return "5.0";
	case PCI_EXP_LNKSTA_CLS_8_0GB: return "8.0";
	default:
		break;
	}

	return "??";
}

static u32 brcm_pcie_mdio_form_pkt(int port, int regad, int cmd)
{
	u32 pkt = 0;

	pkt |= FIELD_PREP(MDIO_PORT_EXT_MASK, port >> 4);
	pkt |= FIELD_PREP(MDIO_PORT_MASK, port);
	pkt |= FIELD_PREP(MDIO_REGAD_MASK, regad);
	pkt |= FIELD_PREP(MDIO_CMD_MASK, cmd);

	return pkt;
}

/**
 * brcm_pcie_mdio_read() - Perform a register read on the internal MDIO bus
 * @base: Pointer to the PCIe controller IO registers
 * @port: The MDIO port number
 * @regad: The register address
 * @val: A pointer at which to store the read value
 *
 * Return: 0 on success and register value in @val, negative error value
 *         on failure.
 */
static int brcm_pcie_mdio_read(void __iomem *base, unsigned int port,
			       unsigned int regad, u32 *val)
{
	u32 data, addr;
	int ret;

	addr = brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_READ);
	writel(addr, base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR);

	ret = readl_poll_timeout(base + PCIE_RC_DL_MDIO_RD_DATA, data,
				 (data & MDIO_DATA_DONE_MASK), 100);

	*val = data & MDIO_DATA_MASK;

	return ret;
}

/**
 * brcm_pcie_mdio_write() - Perform a register write on the internal MDIO bus
 * @base: Pointer to the PCIe controller IO registers
 * @port: The MDIO port number
 * @regad: Address of the register
 * @wrdata: The value to write
 *
 * Return: 0 on success, negative error value on failure.
 */
static int brcm_pcie_mdio_write(void __iomem *base, unsigned int port,
				unsigned int regad, u16 wrdata)
{
	u32 data, addr;

	addr = brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_WRITE);
	writel(addr, base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR);
	writel(MDIO_DATA_DONE_MASK | wrdata, base + PCIE_RC_DL_MDIO_WR_DATA);

	return readl_poll_timeout(base + PCIE_RC_DL_MDIO_WR_DATA, data,
				  !(data & MDIO_DATA_DONE_MASK), 100);
}

/**
 * brcm_pcie_set_ssc() - Configure the controller for Spread Spectrum Clocking
 * @base: pointer to the PCIe controller IO registers
 *
 * Return: 0 on success, negative error value on failure.
 */
static int brcm_pcie_set_ssc(void __iomem *base)
{
	int pll, ssc;
	int ret;
	u32 tmp;

	ret = brcm_pcie_mdio_write(base, MDIO_PORT0, SET_ADDR_OFFSET,
				   SSC_REGS_ADDR);
	if (ret < 0)
		return ret;

	ret = brcm_pcie_mdio_read(base, MDIO_PORT0, SSC_CNTL_OFFSET, &tmp);
	if (ret < 0)
		return ret;

	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_EN_MASK);
	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_VAL_MASK);

	ret = brcm_pcie_mdio_write(base, MDIO_PORT0, SSC_CNTL_OFFSET, tmp);
	if (ret < 0)
		return ret;

	udelay(1000);
	ret = brcm_pcie_mdio_read(base, MDIO_PORT0, SSC_STATUS_OFFSET, &tmp);
	if (ret < 0)
		return ret;

	ssc = FIELD_GET(SSC_STATUS_SSC_MASK, tmp);
	pll = FIELD_GET(SSC_STATUS_PLL_LOCK_MASK, tmp);

	return ssc && pll ? 0 : -EIO;
}

/**
 * brcm_pcie_set_gen() - Limits operation to a specific generation (1, 2 or 3)
 * @pcie: pointer to the PCIe controller state
 * @gen: PCIe generation to limit the controller's operation to
 */
static void brcm_pcie_set_gen(struct brcm_pcie *pcie, unsigned int gen)
{
	void __iomem *cap_base = pcie->base + BRCM_PCIE_CAP_REGS;

	u16 lnkctl2 = readw(cap_base + PCI_EXP_LNKCTL2);
	u32 lnkcap = readl(cap_base + PCI_EXP_LNKCAP);

	lnkcap = (lnkcap & ~PCI_EXP_LNKCAP_SLS) | gen;
	writel(lnkcap, cap_base + PCI_EXP_LNKCAP);

	lnkctl2 = (lnkctl2 & ~0xf) | gen;
	writew(lnkctl2, cap_base + PCI_EXP_LNKCTL2);
}

static void brcm_pcie_set_outbound_win(struct brcm_pcie *pcie,
				       unsigned int win, u64 phys_addr,
				       u64 pcie_addr, u64 size)
{
	void __iomem *base = pcie->base;
	u32 phys_addr_mb_high, limit_addr_mb_high;
	phys_addr_t phys_addr_mb, limit_addr_mb;
	int high_addr_shift;
	u32 tmp;

	/* Set the base of the pcie_addr window */
	writel(lower_32_bits(pcie_addr), base + PCIE_MEM_WIN0_LO(win));
	writel(upper_32_bits(pcie_addr), base + PCIE_MEM_WIN0_HI(win));

	/* Write the addr base & limit lower bits (in MBs) */
	phys_addr_mb = phys_addr / SZ_1M;
	limit_addr_mb = (phys_addr + size - 1) / SZ_1M;

	tmp = readl(base + PCIE_MEM_WIN0_BASE_LIMIT(win));
	u32p_replace_bits(&tmp, phys_addr_mb,
			  MEM_WIN0_BASE_LIMIT_BASE_MASK);
	u32p_replace_bits(&tmp, limit_addr_mb,
			  MEM_WIN0_BASE_LIMIT_LIMIT_MASK);
	writel(tmp, base + PCIE_MEM_WIN0_BASE_LIMIT(win));

	/* Write the cpu & limit addr upper bits */
	high_addr_shift = MEM_WIN0_BASE_LIMIT_BASE_HI_SHIFT;
	phys_addr_mb_high = phys_addr_mb >> high_addr_shift;
	tmp = readl(base + PCIE_MEM_WIN0_BASE_HI(win));
	u32p_replace_bits(&tmp, phys_addr_mb_high,
			  MEM_WIN0_BASE_HI_BASE_MASK);
	writel(tmp, base + PCIE_MEM_WIN0_BASE_HI(win));

	limit_addr_mb_high = limit_addr_mb >> high_addr_shift;
	tmp = readl(base + PCIE_MEM_WIN0_LIMIT_HI(win));
	u32p_replace_bits(&tmp, limit_addr_mb_high,
			  PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK);
	writel(tmp, base + PCIE_MEM_WIN0_LIMIT_HI(win));
}

/**
 * brcm_pcie_perst_set() - Assert or deassert PERST# for the given SoC
 * @pcie: pointer to the PCIe controller state
 * @val: 1 to assert, 0 to deassert
 *
 * BCM2711 uses RGR1_SW_INIT_1 PERST bit.
 * BCM2712 uses PCIE_MISC_PCIE_CTRL PERSTB bit (inverted polarity).
 */
static void brcm_pcie_perst_set(struct brcm_pcie *pcie, u32 val)
{
	void __iomem *base = pcie->base;
	u32 tmp;

	if (pcie->cfg->soc_base == BCM7712) {
		/* BCM2712/7278-style: PERSTB in PCIE_MISC_PCIE_CTRL, inverted */
		tmp = readl(base + PCIE_MISC_PCIE_CTRL);
		u32p_replace_bits(&tmp, !val,
				  PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK);
		writel(tmp, base + PCIE_MISC_PCIE_CTRL);
	} else {
		/* BCM2711: PERST in RGR1_SW_INIT_1 */
		tmp = readl(base + pcie->cfg->offsets[RGR1_SW_INIT_1]);
		u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_PERST_MASK);
		writel(tmp, base + pcie->cfg->offsets[RGR1_SW_INIT_1]);
	}
}

/**
 * brcm_pcie_bridge_sw_init_set() - Assert or deassert bridge software init
 * @pcie: pointer to the PCIe controller state
 * @val: 1 to assert (reset), 0 to deassert (run)
 */
static void brcm_pcie_bridge_sw_init_set(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp;

	tmp = readl(pcie->base + pcie->cfg->offsets[RGR1_SW_INIT_1]);
	u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_INIT_MASK);
	writel(tmp, pcie->base + pcie->cfg->offsets[RGR1_SW_INIT_1]);
}

/**
 * brcm_pcie_post_setup_bcm2712() - BCM2712-specific post-setup for 54MHz refclk
 * @pcie: pointer to the PCIe controller state
 *
 * Return: 0 on success, negative error value on failure.
 */
static int brcm_pcie_post_setup_bcm2712(struct brcm_pcie *pcie)
{
	static const u16 data[] = { 0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030,
				    0x5030, 0x0007 };
	static const u8 regs[] = { 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e };
	int ret, i;
	u32 tmp;

	/* Allow a 54MHz (xosc) refclk source */
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET,
				   0x1600);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, regs[i],
					   data[i]);
		if (ret < 0)
			return ret;
	}

	udelay(200);

	/*
	 * Set L1SS sub-state timers to avoid lengthy state transitions,
	 * PM clock period is 18.52ns (1/54MHz, round down).
	 */
	tmp = readl(pcie->base + PCIE_RC_PL_PHY_CTL_15);
	tmp &= ~PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK;
	tmp |= 0x12;
	writel(tmp, pcie->base + PCIE_RC_PL_PHY_CTL_15);

	return 0;
}

/**
 * brcm_pcie_set_inbound_wins() - Set up inbound BAR windows
 * @pcie: pointer to the PCIe controller state
 * @region: DMA region from device tree
 *
 * For BCM2711, sets up the traditional BAR2-based inbound window.
 * For BCM2712 (BCM7712 lineage), sets up per-BAR inbound windows with
 * UBUS remap registers.
 */
static void brcm_pcie_set_inbound_wins(struct brcm_pcie *pcie,
					struct pci_region *region)
{
	void __iomem *base = pcie->base;
	u64 rc_bar2_offset, rc_bar2_size;
	unsigned int scb_size_val;
	u32 tmp;

	if (pcie->cfg->soc_base == BCM7712) {
		/*
		 * BCM2712/BCM7712: Each BAR provides a non-overlapping
		 * viewport to system memory. Set BAR1 to map the DMA region.
		 */
		u64 size = 1ULL << fls64(region->size - 1);
		u64 pci_offset = region->bus_start - region->phys_start;
		u64 cpu_addr = region->phys_start;
		u32 bar_reg;

		/* BAR1 configuration */
		bar_reg = PCIE_MISC_RC_BAR1_CONFIG_LO;
		tmp = lower_32_bits(pci_offset);
		u32p_replace_bits(&tmp, brcm_pcie_encode_ibar_size(size),
				  RC_BAR1_CONFIG_LO_SIZE_MASK);
		writel(tmp, base + bar_reg);
		writel(upper_32_bits(pci_offset), base + bar_reg + 4);

		/* UBUS BAR1 remap */
		tmp = lower_32_bits(cpu_addr) & ~0xfff;
		tmp |= PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK;
		writel(tmp, base + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP);
		writel(upper_32_bits(cpu_addr),
		       base + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP + 4);
	} else {
		/*
		 * BCM2711: Traditional single inbound window via BAR2.
		 * Disable BAR1 and BAR3.
		 */
		rc_bar2_offset = region->bus_start - region->phys_start;
		rc_bar2_size = 1ULL << fls64(region->size - 1);

		tmp = lower_32_bits(rc_bar2_offset);
		u32p_replace_bits(&tmp,
				  brcm_pcie_encode_ibar_size(rc_bar2_size),
				  RC_BAR2_CONFIG_LO_SIZE_MASK);
		writel(tmp, base + PCIE_MISC_RC_BAR2_CONFIG_LO);
		writel(upper_32_bits(rc_bar2_offset),
		       base + PCIE_MISC_RC_BAR2_CONFIG_HI);

		scb_size_val = rc_bar2_size ?
			       ilog2(rc_bar2_size) - 15 : 0xf;

		tmp = readl(base + PCIE_MISC_MISC_CTRL);
		u32p_replace_bits(&tmp, scb_size_val,
				  MISC_CTRL_SCB0_SIZE_MASK);
		writel(tmp, base + PCIE_MISC_MISC_CTRL);

		/* Disable the PCIe->GISB memory window (RC_BAR1) */
		clrbits_le32(base + PCIE_MISC_RC_BAR1_CONFIG_LO,
			     RC_BAR1_CONFIG_LO_SIZE_MASK);

		/* Disable the PCIe->SCB memory window (RC_BAR3) */
		clrbits_le32(base + PCIE_MISC_RC_BAR3_CONFIG_LO,
			     RC_BAR3_CONFIG_LO_SIZE_MASK);
	}
}

static int brcm_pcie_probe(struct udevice *dev)
{
	struct udevice *ctlr = pci_get_controller(dev);
	struct pci_controller *hose = dev_get_uclass_priv(ctlr);
	struct brcm_pcie *pcie = dev_get_priv(dev);
	void __iomem *base = pcie->base;
	struct pci_region region;
	bool ssc_good = false;
	int num_out_wins = 0;
	int i, ret;
	u16 nlw, cls, lnksta;
	u32 tmp, burst;
	int hard_debug = pcie->cfg->offsets[PCIE_HARD_DEBUG];

	/* Reset the bridge */
	brcm_pcie_bridge_sw_init_set(pcie, 1);

	/*
	 * Ensure that PERST# is asserted; some bootloaders may deassert it.
	 * Only needed for BCM2711.
	 */
	if (pcie->cfg->soc_base == BCM2711)
		brcm_pcie_perst_set(pcie, 1);

	/*
	 * The delay is a safety precaution to preclude the reset signal
	 * from looking like a glitch.
	 */
	udelay(100);

	/* Take the bridge out of reset */
	brcm_pcie_bridge_sw_init_set(pcie, 0);

	clrbits_le32(base + hard_debug, PCIE_HARD_DEBUG_SERDES_IDDQ_MASK);

	/* Wait for SerDes to be stable */
	udelay(100);

	/*
	 * SCB_MAX_BURST_SIZE is a two bit field. For BCM2711 it is
	 * encoded as 0=128, 1=256, 2=512, 3=Rsvd. For BCM7712-based
	 * chips (BCM2712), use 512 bytes.
	 */
	if (pcie->cfg->soc_base == BCM7712)
		burst = 0x2; /* 512 bytes */
	else
		burst = 0x0; /* 128 bytes */

	/* Set SCB_MAX_BURST_SIZE, CFG_READ_UR_MODE, SCB_ACCESS_EN */
	tmp = readl(base + PCIE_MISC_MISC_CTRL);
	u32p_replace_bits(&tmp, 1, MISC_CTRL_SCB_ACCESS_EN_MASK);
	u32p_replace_bits(&tmp, 1, MISC_CTRL_CFG_READ_UR_MODE_MASK);
	u32p_replace_bits(&tmp, burst, MISC_CTRL_MAX_BURST_SIZE_MASK);
	u32p_replace_bits(&tmp, 1, MISC_CTRL_PCIE_RCB_MPS_MODE_MASK);
	u32p_replace_bits(&tmp, 1, MISC_CTRL_PCIE_RCB_64B_MODE_MASK);
	writel(tmp, base + PCIE_MISC_MISC_CTRL);

	/* Set up inbound windows */
	pci_get_dma_regions(dev, &region, 0);
	brcm_pcie_set_inbound_wins(pcie, &region);

	if (!brcm_pcie_rc_mode(pcie)) {
		printf("PCIe misconfigured; is in EP mode\n");
		return -EINVAL;
	}

	/* Mask all interrupts since we are not handling any yet */
	writel(0xffffffff, base + PCIE_MSI_INTR2_MASK_SET);

	/* Clear any interrupts we find on boot */
	writel(0xffffffff, base + PCIE_MSI_INTR2_CLR);

	if (pcie->gen)
		brcm_pcie_set_gen(pcie, pcie->gen);

	/* Unassert the fundamental reset */
	brcm_pcie_perst_set(pcie, 0);

	/*
	 * Wait for 100ms after PERST# deassertion; see PCIe CEM specification
	 * sections 2.2, PCIe r5.0, 6.6.1.
	 */
	mdelay(100);

	/* Give the RC/EP time to wake up, before trying to configure RC.
	 * Intermittently check status for link-up, up to a total of 100ms.
	 */
	for (i = 0; i < 100 && !brcm_pcie_link_up(pcie); i += 5)
		mdelay(5);

	if (!brcm_pcie_link_up(pcie)) {
		printf("PCIe BRCM: link down\n");
		return -EINVAL;
	}

	for (i = 0; i < hose->region_count; i++) {
		struct pci_region *reg = &hose->regions[i];

		if (reg->flags != PCI_REGION_MEM)
			continue;

		if (num_out_wins >= BRCM_NUM_PCIE_OUT_WINS)
			return -EINVAL;

		brcm_pcie_set_outbound_win(pcie, num_out_wins, reg->phys_start,
					   reg->bus_start, reg->size);

		num_out_wins++;
	}

	/*
	 * For config space accesses on the RC, show the right class for
	 * a PCIe-PCIe bridge (the default setting is to be EP mode).
	 */
	clrsetbits_le32(base + PCIE_RC_CFG_PRIV1_ID_VAL3,
			PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK, 0x060400);

	/* BCM2712 post-setup: configure 54MHz refclk source */
	if (pcie->cfg->soc_base == BCM7712) {
		ret = brcm_pcie_post_setup_bcm2712(pcie);
		if (ret < 0)
			printf("PCIe BRCM: post-setup failed\n");
	}

	if (pcie->ssc) {
		ret = brcm_pcie_set_ssc(pcie->base);
		if (!ret)
			ssc_good = true;
		else
			printf("PCIe BRCM: failed attempt to enter SSC mode\n");
	}

	lnksta = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA);
	cls = lnksta & PCI_EXP_LNKSTA_CLS;
	nlw = (lnksta & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;

	printf("PCIe BRCM: link up, %s Gbps x%u %s\n", link_speed_to_str(cls),
	       nlw, ssc_good ? "(SSC)" : "(!SSC)");

	/* PCIe->SCB endian mode for BAR */
	clrsetbits_le32(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1,
			PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK,
			VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN);

	/*
	 * We used to enable the CLKREQ# input here, but a few PCIe cards don't
	 * attach anything to the CLKREQ# line, so we shouldn't assume that
	 * it's connected and working. The controller does allow detecting
	 * whether the port on the other side of our link is/was driving this
	 * signal, so we could check before we assume. But because this signal
	 * is for power management, which doesn't make sense in a bootloader,
	 * let's instead just unadvertise ASPM support.
	 */
	clrbits_le32(base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY,
		     LINK_CAPABILITY_ASPM_SUPPORT_MASK);

	return 0;
}

static int brcm_pcie_remove(struct udevice *dev)
{
	struct brcm_pcie *pcie = dev_get_priv(dev);
	void __iomem *base = pcie->base;
	int hard_debug = pcie->cfg->offsets[PCIE_HARD_DEBUG];

	/* Assert fundamental reset */
	brcm_pcie_perst_set(pcie, 1);

	/* Turn off SerDes */
	setbits_le32(base + hard_debug, PCIE_HARD_DEBUG_SERDES_IDDQ_MASK);

	/*
	 * BCM2712 must not shut down the bridge to avoid hardware issues.
	 * The kernel will manage the bridge lifecycle.
	 */
	if (!pcie->cfg->avoid_bridge_shutdown)
		brcm_pcie_bridge_sw_init_set(pcie, 1);

	return 0;
}

static int brcm_pcie_of_to_plat(struct udevice *dev)
{
	struct brcm_pcie *pcie = dev_get_priv(dev);
	ofnode dn = dev_ofnode(dev);
	u32 max_link_speed;
	int ret;

	/* Get the controller base address */
	pcie->base = dev_read_addr_ptr(dev);
	if (!pcie->base)
		return -EINVAL;

	pcie->cfg = (const struct pcie_cfg_data *)dev_get_driver_data(dev);

	pcie->ssc = ofnode_read_bool(dn, "brcm,enable-ssc");

	ret = ofnode_read_u32(dn, "max-link-speed", &max_link_speed);
	if (ret < 0 || max_link_speed > 4)
		pcie->gen = 0;
	else
		pcie->gen = max_link_speed;

	return 0;
}

static const struct dm_pci_ops brcm_pcie_ops = {
	.read_config	= brcm_pcie_read_config,
	.write_config	= brcm_pcie_write_config,
};

static const struct udevice_id brcm_pcie_ids[] = {
	{ .compatible = "brcm,bcm2711-pcie",
	  .data = (ulong)&bcm2711_cfg },
	{ .compatible = "brcm,bcm2712-pcie",
	  .data = (ulong)&bcm2712_cfg },
	{ }
};

U_BOOT_DRIVER(pcie_brcm_base) = {
	.name			= "pcie_brcm",
	.id			= UCLASS_PCI,
	.ops			= &brcm_pcie_ops,
	.of_match		= brcm_pcie_ids,
	.probe			= brcm_pcie_probe,
	.remove			= brcm_pcie_remove,
	.of_to_plat	= brcm_pcie_of_to_plat,
	.priv_auto	= sizeof(struct brcm_pcie),
	.flags		= DM_FLAG_OS_PREPARE,
};
