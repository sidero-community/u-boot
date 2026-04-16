// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012-2016 Stephen Warren
 */

#define LOG_CATEGORY	LOGC_BOARD

#include <config.h>
#include <dm.h>
#include <env.h>
#include <efi_loader.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <fdt_simplefb.h>
#include <init.h>
#include <memalign.h>
#include <mmc.h>
#include <asm/gpio.h>
#include <asm/arch/mbox.h>
#include <asm/arch/msg.h>
#include <asm/arch/sdhci.h>
#include <asm/global_data.h>
#include <dm/platform_data/serial_bcm283x_mu.h>
#include <broadcom/bcm_board_types.h>
#ifdef CONFIG_ARM64
#include <asm/armv8/mmu.h>
#endif
#include <sysreset.h>
#include <watchdog.h>
#include <dm/pinctrl.h>
#include <dm/ofnode.h>
#include <dm/device-internal.h>
#include <dm/uclass.h>
#include <acpi/acpi_table.h>
#include <acpi/acpigen.h>
#include <dm/lists.h>
#include <tables_csum.h>
#if defined(CONFIG_BCM2712) && defined(CONFIG_CYCLIC)
#include <cyclic.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

/* Assigned in lowlevel_init.S
 * Push the variable into the .data section so that it
 * does not get cleared later.
 */
unsigned long __section(".data") fw_dtb_pointer;

/* TODO(sjg@chromium.org): Move these to the msg.c file */
struct msg_get_arm_mem {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_get_arm_mem get_arm_mem;
	u32 end_tag;
};

struct msg_get_board_rev {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_get_board_rev get_board_rev;
	u32 end_tag;
};

struct msg_get_board_serial {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_get_board_serial get_board_serial;
	u32 end_tag;
};

struct msg_get_mac_address {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_get_mac_address get_mac_address;
	u32 end_tag;
};

struct msg_get_clock_rate {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_get_clock_rate get_clock_rate;
	u32 end_tag;
};

struct efi_fw_image fw_images[] = {
	{
		.fw_name = u"RPI_UBOOT",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = "mmc 0=u-boot.bin fat 0 1",
	.num_images = ARRAY_SIZE(fw_images),
	.images = fw_images,
};

#ifdef CONFIG_ARM64
#define DTB_DIR "broadcom/"
#else
#define DTB_DIR ""
#endif

/*
 * https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#raspberry-pi-revision-codes
 */
struct rpi_model {
	const char *name;
	const char * const *fdtfiles;
	size_t fdtcount;
	bool has_onboard_eth;
};

#define FDTFILES(...) \
	(const char *[]){ __VA_ARGS__ }, \
	sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(const char *)

static const struct rpi_model rpi_model_unknown = {
	"Unknown model",
	FDTFILES(DTB_DIR "bcm283x-rpi-other.dtb"),
	false,
};

static const struct rpi_model rpi_models_new_scheme[] = {
	[0x0] = {
		"Model A",
		FDTFILES(DTB_DIR "bcm2835-rpi-a.dtb"),
		false,
	},
	[0x1] = {
		"Model B",
		FDTFILES(DTB_DIR "bcm2835-rpi-b.dtb"),
		true,
	},
	[0x2] = {
		"Model A+",
		FDTFILES(DTB_DIR "bcm2835-rpi-a-plus.dtb"),
		false,
	},
	[0x3] = {
		"Model B+",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-plus.dtb"),
		true,
	},
	[0x4] = {
		"2 Model B",
		FDTFILES(DTB_DIR "bcm2836-rpi-2-b.dtb"),
		true,
	},
	[0x6] = {
		"Compute Module",
		FDTFILES(DTB_DIR "bcm2835-rpi-cm.dtb"),
		false,
	},
	[0x8] = {
		"3 Model B",
		FDTFILES(DTB_DIR "bcm2837-rpi-3-b.dtb"),
		true,
	},
	[0x9] = {
		"Zero",
		FDTFILES(DTB_DIR "bcm2835-rpi-zero.dtb"),
		false,
	},
	[0xA] = {
		"Compute Module 3",
		FDTFILES(DTB_DIR "bcm2837-rpi-cm3.dtb"),
		false,
	},
	[0xC] = {
		"Zero W",
		FDTFILES(DTB_DIR "bcm2835-rpi-zero-w.dtb"),
		false,
	},
	[0xD] = {
		"3 Model B+",
		FDTFILES(DTB_DIR "bcm2837-rpi-3-b-plus.dtb"),
		true,
	},
	[0xE] = {
		"3 Model A+",
		FDTFILES(DTB_DIR "bcm2837-rpi-3-a-plus.dtb"),
		false,
	},
	[0x10] = {
		"Compute Module 3+",
		FDTFILES(DTB_DIR "bcm2837-rpi-cm3.dtb"),
		false,
	},
	[0x11] = {
		"4 Model B",
		FDTFILES(DTB_DIR "bcm2711-rpi-4-b.dtb"),
		true,
	},
	[0x12] = {
		"Zero 2 W",
		FDTFILES(DTB_DIR "bcm2837-rpi-zero-2-w.dtb"),
		false,
	},
	[0x13] = {
		"400",
		FDTFILES(DTB_DIR "bcm2711-rpi-400.dtb"),
		true,
	},
	[0x14] = {
		"Compute Module 4",
		FDTFILES(DTB_DIR "bcm2711-rpi-cm4.dtb"),
		true,
	},
	[0x17] = {
		"5 Model B",
		FDTFILES(
			[0] = DTB_DIR "bcm2712-rpi-5-b.dtb",
			[1] = DTB_DIR "bcm2712-d-rpi-5-b.dtb"
		),
		true,
	},
	[0x18] = {
		"Compute Module 5",
		FDTFILES(DTB_DIR "bcm2712-rpi-cm5-cm5io.dtb"),
		true,
	},
	[0x19] = {
		"500",
		FDTFILES(DTB_DIR "bcm2712-rpi-500.dtb"),
		true,
	},
	[0x1A] = {
		"Compute Module 5 Lite",
		FDTFILES(DTB_DIR "bcm2712-rpi-cm5l-cm5io.dtb"),
		true,
	},
};

static const struct rpi_model rpi_models_old_scheme[] = {
	[0x2] = {
		"Model B",
		FDTFILES(DTB_DIR "bcm2835-rpi-b.dtb"),
		true,
	},
	[0x3] = {
		"Model B",
		FDTFILES(DTB_DIR "bcm2835-rpi-b.dtb"),
		true,
	},
	[0x4] = {
		"Model B rev2",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-rev2.dtb"),
		true,
	},
	[0x5] = {
		"Model B rev2",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-rev2.dtb"),
		true,
	},
	[0x6] = {
		"Model B rev2",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-rev2.dtb"),
		true,
	},
	[0x7] = {
		"Model A",
		FDTFILES(DTB_DIR "bcm2835-rpi-a.dtb"),
		false,
	},
	[0x8] = {
		"Model A",
		FDTFILES(DTB_DIR "bcm2835-rpi-a.dtb"),
		false,
	},
	[0x9] = {
		"Model A",
		FDTFILES(DTB_DIR "bcm2835-rpi-a.dtb"),
		false,
	},
	[0xd] = {
		"Model B rev2",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-rev2.dtb"),
		true,
	},
	[0xe] = {
		"Model B rev2",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-rev2.dtb"),
		true,
	},
	[0xf] = {
		"Model B rev2",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-rev2.dtb"),
		true,
	},
	[0x10] = {
		"Model B+",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-plus.dtb"),
		true,
	},
	[0x11] = {
		"Compute Module",
		FDTFILES(DTB_DIR "bcm2835-rpi-cm.dtb"),
		false,
	},
	[0x12] = {
		"Model A+",
		FDTFILES(DTB_DIR "bcm2835-rpi-a-plus.dtb"),
		false,
	},
	[0x13] = {
		"Model B+",
		FDTFILES(DTB_DIR "bcm2835-rpi-b-plus.dtb"),
		true,
	},
	[0x14] = {
		"Compute Module",
		FDTFILES(DTB_DIR "bcm2835-rpi-cm.dtb"),
		false,
	},
	[0x15] = {
		"Model A+",
		FDTFILES(DTB_DIR "bcm2835-rpi-a-plus.dtb"),
		false,
	},
};

static uint32_t revision;
static uint32_t rev_scheme;
static uint32_t rev_type;
static const struct rpi_model *model;

int dram_init(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct msg_get_arm_mem, msg, 1);
	int ret;

	BCM2835_MBOX_INIT_HDR(msg);
	BCM2835_MBOX_INIT_TAG(&msg->get_arm_mem, GET_ARM_MEMORY);

	ret = bcm2835_mbox_call_prop(BCM2835_MBOX_PROP_CHAN, &msg->hdr);
	if (ret) {
		printf("bcm2835: Could not query ARM memory size\n");
		return -1;
	}

	gd->ram_size = msg->get_arm_mem.body.resp.mem_size;

	/*
	 * In some configurations the memory size returned by VideoCore
	 * is not aligned to the section size, what is mandatory for
	 * the u-boot's memory setup.
	 */
	gd->ram_size &= ~MMU_SECTION_SIZE;

	return 0;
}

#ifdef CONFIG_OF_BOARD
int dram_init_banksize(void)
{
	phys_addr_t total_size = 0;
	int i;
	int ret;

	ret = fdtdec_setup_memory_banksize();
	if (ret)
		return ret;

	ret = fdtdec_setup_mem_size_base();
	if (ret)
		return ret;

	/* Update gd->ram_size to reflect total RAM across all banks */
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (gd->bd->bi_dram[i].size == 0)
			break;
		total_size += gd->bd->bi_dram[i].size;
	}
	gd->ram_size = total_size;

	return 0;
}
#endif

static void set_fdtfile(void)
{
	const char *fdtfile;
	int rev = revision & 0x0f;

	if (env_get("fdtfile"))
		return;

	/* set the first entry as default */
	fdtfile = model->fdtfiles[0];

	if (rev < model->fdtcount)
		fdtfile = model->fdtfiles[rev];

	env_set("fdtfile", fdtfile);
}

/*
 * Allow U-Boot to use its control FDT with extlinux if one is not provided.
 * This will then go through the usual fixups that U-Boot does, before being
 * handed off to Linux
 */
static void set_fdt_addr(void)
{
	env_set_hex("fdt_addr", (ulong)gd->fdt_blob);
}

/*
 * Prevent relocation from stomping on a firmware provided FDT blob.
 */
phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	if ((gd->ram_top - fw_dtb_pointer) > SZ_64M)
		return gd->ram_top;
	return fw_dtb_pointer & ~0xffff;
}

static void set_usbethaddr(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct msg_get_mac_address, msg, 1);
	int ret;

	if (!model->has_onboard_eth)
		return;

	if (env_get("usbethaddr"))
		return;

	BCM2835_MBOX_INIT_HDR(msg);
	BCM2835_MBOX_INIT_TAG(&msg->get_mac_address, GET_MAC_ADDRESS);

	ret = bcm2835_mbox_call_prop(BCM2835_MBOX_PROP_CHAN, &msg->hdr);
	if (ret) {
		printf("bcm2835: Could not query MAC address\n");
		/* Ignore error; not critical */
		return;
	}

	eth_env_set_enetaddr("usbethaddr", msg->get_mac_address.body.resp.mac);

	if (!env_get("ethaddr"))
		env_set("ethaddr", env_get("usbethaddr"));

	return;
}

#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
static void set_board_info(void)
{
	char s[11];

	snprintf(s, sizeof(s), "0x%X", revision);
	env_set("board_revision", s);
	snprintf(s, sizeof(s), "%u", rev_scheme);
	env_set("board_rev_scheme", s);
	/* Can't rename this to board_rev_type since it's an ABI for scripts */
	snprintf(s, sizeof(s), "0x%X", rev_type);
	env_set("board_rev", s);
	env_set("board_name", model->name);
}
#endif /* CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG */

static void set_serial_number(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct msg_get_board_serial, msg, 1);
	int ret;
	char serial_string[17] = { 0 };

	if (env_get("serial#"))
		return;

	BCM2835_MBOX_INIT_HDR(msg);
	BCM2835_MBOX_INIT_TAG_NO_REQ(&msg->get_board_serial, GET_BOARD_SERIAL);

	ret = bcm2835_mbox_call_prop(BCM2835_MBOX_PROP_CHAN, &msg->hdr);
	if (ret) {
		printf("bcm2835: Could not query board serial\n");
		/* Ignore error; not critical */
		return;
	}

	snprintf(serial_string, sizeof(serial_string), "%016llx",
		 msg->get_board_serial.body.resp.serial);
	env_set("serial#", serial_string);
}

int misc_init_r(void)
{
	set_fdt_addr();
	set_fdtfile();
	set_usbethaddr();
#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	set_board_info();
#endif
	set_serial_number();

	return 0;
}

static void get_board_revision(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct msg_get_board_rev, msg, 1);
	int ret;
	const struct rpi_model *models;
	uint32_t models_count;
	ofnode node;

	BCM2835_MBOX_INIT_HDR(msg);
	BCM2835_MBOX_INIT_TAG(&msg->get_board_rev, GET_BOARD_REV);

	ret = bcm2835_mbox_call_prop(BCM2835_MBOX_PROP_CHAN, &msg->hdr);
	if (ret) {
		/* Ignore error; not critical */
		node = ofnode_path("/system");
		if (!ofnode_valid(node)) {
			printf("bcm2835: Could not find /system node\n");
			return;
		}

		ret = ofnode_read_u32(node, "linux,revision", &revision);
		if (ret) {
			printf("bcm2835: Could not find linux,revision\n");
			return;
		}
	} else {
		revision = msg->get_board_rev.body.resp.rev;
	}

	/*
	 * For details of old-vs-new scheme, see:
	 * https://github.com/pimoroni/RPi.version/blob/master/RPi/version.py
	 * http://www.raspberrypi.org/forums/viewtopic.php?f=63&t=99293&p=690282
	 * (a few posts down)
	 *
	 * For the RPi 1, bit 24 is the "warranty bit", so we mask off just the
	 * lower byte to use as the board rev:
	 * http://www.raspberrypi.org/forums/viewtopic.php?f=63&t=98367&start=250
	 * http://www.raspberrypi.org/forums/viewtopic.php?f=31&t=20594
	 */
	if (revision & 0x800000) {
		rev_scheme = 1;
		rev_type = (revision >> 4) & 0xff;
		models = rpi_models_new_scheme;
		models_count = ARRAY_SIZE(rpi_models_new_scheme);
	} else {
		rev_scheme = 0;
		rev_type = revision & 0xff;
		models = rpi_models_old_scheme;
		models_count = ARRAY_SIZE(rpi_models_old_scheme);
	}
	if (rev_type >= models_count) {
		printf("RPI: Board rev 0x%x outside known range\n", rev_type);
		model = &rpi_model_unknown;
	} else if (!models[rev_type].name) {
		printf("RPI: Board rev 0x%x unknown\n", rev_type);
		model = &rpi_model_unknown;
	} else {
		model = &models[rev_type];
	}

	#ifdef CONFIG_BOARD_TYPES
		gd->board_type = rev_type;
	#endif

	printf("RPI %s (0x%x)\n", model->name, revision);
}

int board_init(void)
{
	get_board_revision();

	gd->bd->bi_boot_params = 0x100;

	return bcm2835_power_on_module(BCM2835_MBOX_POWER_DEVID_USB_HCD);
}

/*
 * If the firmware passed a device tree use it for U-Boot.
 */
int board_fdt_blob_setup(void **fdtp)
{
	if (fdt_magic(fw_dtb_pointer) != FDT_MAGIC)
		return -ENXIO;

	*fdtp = (void *)fw_dtb_pointer;

	return 0;
}

int copy_property(void *dst, void *src, char *path, char *property)
{
	int dst_offset, src_offset;
	const fdt32_t *prop;
	int len;

	src_offset = fdt_path_offset(src, path);
	dst_offset = fdt_path_offset(dst, path);

	if (src_offset < 0 || dst_offset < 0)
		return -1;

	prop = fdt_getprop(src, src_offset, property, &len);
	if (!prop)
		return -1;

	return fdt_setprop(dst, dst_offset, property, prop, len);
}

/* Copy tweaks from the firmware dtb to the loaded dtb */
void  update_fdt_from_fw(void *fdt, void *fw_fdt)
{
	/* Using dtb from firmware directly; leave it alone */
	if (fdt == fw_fdt)
		return;

	/* The firmware provides a more precise model; so copy that */
	copy_property(fdt, fw_fdt, "/", "model");

	/* memory reserve as suggested by the firmware */
	copy_property(fdt, fw_fdt, "/", "memreserve");

	/* copy the CMA memory setting from the firmware DT to linux */
	copy_property(fdt, fw_fdt, "/reserved-memory/linux,cma", "size");

	/* Adjust dma-ranges for the SD card and PCI bus as they can depend on
	 * the SoC revision
	 */
	copy_property(fdt, fw_fdt, "emmc2bus", "dma-ranges");
	copy_property(fdt, fw_fdt, "pcie0", "dma-ranges");

	/* Bootloader configuration template exposes as nvmem */
	if (copy_property(fdt, fw_fdt, "blconfig", "reg") == 0)
		copy_property(fdt, fw_fdt, "blconfig", "status");

	/* kernel address randomisation seed as provided by the firmware */
	copy_property(fdt, fw_fdt, "/chosen", "kaslr-seed");

	/* warnings from the firmware (if any) */
	copy_property(fdt, fw_fdt, "/chosen", "user-warnings");

	/* firmware logs - used by the vclog utility */
	copy_property(fdt, fw_fdt, "/chosen", "log");

	/* address of the PHY device as provided by the firmware  */
	copy_property(fdt, fw_fdt, "ethernet0/mdio@e14/ethernet-phy@1", "reg");

	/* Bluetooth device address as provided by the firmware */
	copy_property(fdt, fw_fdt, "/soc/serial@7e201000/bluetooth", "local-bd-address");

	/* copy uart clk as provided by the firmware */
	copy_property(fdt, fw_fdt, "/clocks/clk-uart", "clock-frequency");
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	int node;

	if (blob == gd->fdt_blob)
		log_debug("Same FDT: nothing to do\n");
	else
		update_fdt_from_fw(blob, (void *)gd->fdt_blob);

	if (CONFIG_IS_ENABLED(FDT_SIMPLEFB)) {
		node = fdt_node_offset_by_compatible(blob, -1, "simple-framebuffer");
		if (node < 0)
			fdt_simplefb_add_node(blob);
		else
			fdt_simplefb_enable_and_mem_rsv(blob);
	}

#ifdef CONFIG_EFI_LOADER
	/* Reserve the spin table */
	efi_add_memory_map(0, CONFIG_RPI_EFI_NR_SPIN_PAGES << EFI_PAGE_SHIFT,
			   EFI_RESERVED_MEMORY_TYPE);
#endif

	return 0;
}

/* TODO: Using late_init to initialize pci device with ID_RP1.
 * RP1 pci device should be initialized by the PCI subsystem because
 * it is under develop right now and depends from the final device-tree
 * format from the Linux Kernel. Current device-tree format violates
 * pci driver model. So this should be changed after upstreaming RP1
 * to the Linux Kernel source code.
 * This initialization should be done only for RPI5 board.
 */
#ifdef CONFIG_BCM2712

/*
 * Power button monitoring via GIO edge detection.
 *
 * The RPi 5 power button (J2 header / on-board switch) goes through the
 * PMIC, which signals the VPU firmware, which drives GIO GPIO20 low.
 * The pulse can be very short (~100-200ms from KVM devices), so simple
 * level-polling at 100ms intervals misses it.
 *
 * Instead we configure the GIO hardware for falling-edge detection.
 * The STAT register latches the edge and stays set until explicitly
 * cleared (write-1-to-clear), so even a microsecond pulse is captured.
 * We poll STAT (plus a level fallback) to reliably detect presses.
 */
#if defined(CONFIG_CYCLIC) && defined(CONFIG_DM_GPIO)

/* GIO per-bank register offsets (each bank is 0x20 bytes) */
#define GIO_BANK_SIZE	0x20
#define GIO_REG_EC	0x0c	/* edge config: 0 = rising, 1 = falling */
#define GIO_REG_EI	0x10	/* edge interrupt enable (gates STAT latch) */
#define GIO_REG_STAT	0x1c	/* interrupt status (sticky, write-1-to-clear) */

#define RPI_PWR_BTN_POLL_US	50000	/* 50ms polling interval */

struct rpi_power_btn_ctx {
	struct cyclic_info cyclic;
	struct gpio_desc gpio;		/* DM GPIO handle (level reads + diagnostics) */
	void __iomem *bank_base;	/* direct GIO bank register base (edge detect) */
	u32 bit_mask;			/* bit within the bank for the power GPIO */
};

static struct rpi_power_btn_ctx rpi_pwr_btn;

static void rpi_power_btn_poll(struct cyclic_info *c)
{
	struct rpi_power_btn_ctx *ctx =
		container_of(c, struct rpi_power_btn_ctx, cyclic);

	/* Primary: check hardware edge-triggered status (catches short pulses) */
	if (ctx->bank_base) {
		u32 stat = readl(ctx->bank_base + GIO_REG_STAT);

		if (stat & ctx->bit_mask) {
			writel(ctx->bit_mask, ctx->bank_base + GIO_REG_STAT);
			printf("\nRPI: Power button pressed, resetting...\n");
			reset_cpu();
			return;
		}
	}

	/* Fallback: level-based detection for sustained holds */
	if (dm_gpio_get_value(&ctx->gpio) > 0) {
		printf("\nRPI: Power button held, resetting...\n");
		reset_cpu();
	}
}

static void rpi_register_power_button(void)
{
	ofnode keys_node, btn_node;
	struct udevice *gpio_dev;
	void __iomem *gio_base;
	u32 bank_widths[4];
	int num_banks, i, bank;
	u32 gpio_offset, offset;
	u32 val;
	int ret;

	keys_node = ofnode_by_compatible(ofnode_null(), "gpio-keys");
	if (!ofnode_valid(keys_node))
		return;

	ofnode_for_each_subnode(btn_node, keys_node) {
		const char *label = ofnode_read_string(btn_node, "label");

		if (label && !strcmp(label, "pwr_button"))
			break;
	}

	if (!ofnode_valid(btn_node)) {
		log_debug("RPI: power button node not found\n");
		return;
	}

	/* Request GPIO through DM — ensures it is claimed and set as input */
	ret = gpio_request_by_name_nodev(btn_node, "gpios", 0,
					 &rpi_pwr_btn.gpio, GPIOD_IS_IN);
	if (ret) {
		log_debug("RPI: failed to request power button GPIO: %d\n", ret);
		return;
	}

	/*
	 * Set up edge detection directly on the GIO hardware registers.
	 * The DM GPIO layer doesn't expose interrupt/edge functionality,
	 * so we access the controller registers via the parent device.
	 */
	gpio_dev = rpi_pwr_btn.gpio.dev;
	gpio_offset = rpi_pwr_btn.gpio.offset;

	gio_base = dev_remap_addr(gpio_dev);
	if (!gio_base) {
		printf("RPI: cannot map GIO registers, edge detect unavailable\n");
		goto register_cyclic;
	}

	/* Determine which bank and bit position this GPIO falls in */
	num_banks = dev_read_size(gpio_dev, "brcm,gpio-bank-widths");
	if (num_banks < 0) {
		printf("RPI: missing bank-widths, edge detect unavailable\n");
		goto register_cyclic;
	}
	num_banks /= sizeof(u32);
	if (num_banks > ARRAY_SIZE(bank_widths)) {
		printf("RPI: too many banks, edge detect unavailable\n");
		goto register_cyclic;
	}
	dev_read_u32_array(gpio_dev, "brcm,gpio-bank-widths",
			   bank_widths, num_banks);

	offset = gpio_offset;
	bank = -1;
	for (i = 0; i < num_banks; i++) {
		if (offset < bank_widths[i]) {
			bank = i;
			break;
		}
		offset -= bank_widths[i];
	}
	if (bank < 0) {
		printf("RPI: GPIO%u out of range, edge detect unavailable\n",
		       gpio_offset);
		goto register_cyclic;
	}

	rpi_pwr_btn.bank_base = gio_base + bank * GIO_BANK_SIZE;
	rpi_pwr_btn.bit_mask = BIT(offset);

	/* Configure falling-edge detection (active-low button) */
	val = readl(rpi_pwr_btn.bank_base + GIO_REG_EC);
	val |= rpi_pwr_btn.bit_mask;
	writel(val, rpi_pwr_btn.bank_base + GIO_REG_EC);

	/* Enable edge interrupt status latching (not routed to GIC) */
	val = readl(rpi_pwr_btn.bank_base + GIO_REG_EI);
	val |= rpi_pwr_btn.bit_mask;
	writel(val, rpi_pwr_btn.bank_base + GIO_REG_EI);

	/* Clear any stale edge status */
	writel(rpi_pwr_btn.bit_mask, rpi_pwr_btn.bank_base + GIO_REG_STAT);

register_cyclic:
	cyclic_register(&rpi_pwr_btn.cyclic, rpi_power_btn_poll,
			RPI_PWR_BTN_POLL_US, "rpi_pwr_btn");

	printf("RPI: Power button monitoring enabled (GPIO%u, %s)\n",
	       gpio_offset, rpi_pwr_btn.bank_base ? "edge+level" : "level-only");
}
#else
static inline void rpi_register_power_button(void) {}
#endif /* CONFIG_CYCLIC && CONFIG_DM_GPIO */

int board_late_init(void)
{
	struct udevice *dev;
	int err;

	/* Only scan for RP1 on RPi 5 family boards (BCM2712)
	 * Board types: RPi 5B, CM5, RPi 500, CM5 Lite
	 */
#ifdef CONFIG_BOARD_TYPES
	if (gd->board_type < RPI_BOARD_TYPE_RPI5_FAMILY_MIN ||
	    gd->board_type > RPI_BOARD_TYPE_RPI5_FAMILY_MAX) {
		/* Not a RPi 5 board, skip RP1 detection */
		return 0;
	}
#endif

	err = dm_pci_find_device(PCI_VENDOR_ID_RPI, PCI_DEVICE_ID_RP1_C0,
				 0, &dev);
	if (err) {
		printf("RPI: RP1 device not found\n");
		return 0;
	}

	/* Probe the RP1 MFD device to initialize its children
	 * (GPIO, clocks, UART, USB, Ethernet, etc.)
	 */
	err = device_probe(dev);
	if (err) {
		printf("RPI: Failed to probe RP1 device: %d\n", err);
		return err;
	}

	printf("RPI: RP1 initialized successfully\n");

	rpi_register_power_button();

	return 0;
}
#endif

#if CONFIG_IS_ENABLED(GENERATE_ACPI_TABLE)
static bool is_rpi5(void)
{
	return of_machine_is_compatible("brcm,bcm2712");
}

static bool is_rpi4(void)
{
	return of_machine_is_compatible("brcm,bcm2711");
}

static bool is_rpi3(void)
{
	return of_machine_is_compatible("brcm,bcm2837");
}

static int acpi_rpi_board_fill_ssdt(struct acpi_ctx *ctx)
{
	int node, ret, uart_in_use, mini_clock_rate;
	bool enabled;
	struct udevice *dev;
	struct {
		const char *fdt_compatible;
		const char *acpi_scope;
		bool on_rpi5;
		bool on_rpi4;
		bool on_rpi3;
		u32 mmio_address;
	} map[] = {
		{"brcm,bcm2711-pcie", "\\_SB.PCI0", false, true, false},
		{"brcm,bcm2711-emmc2", "\\_SB.GDV1.SDC3", false, true, false},
		{"brcm,bcm2835-pwm", "\\_SB.GDV0.PWM0", false, true, true},
		{"brcm,bcm2711-genet-v5",  "\\_SB.ETH0", false, true, false},
		{"raspberrypi,rp1-gem", "\\_SB.ETH0", true, false, false},
		{"brcm,bcm2711-thermal", "\\_SB.EC00", false, true, true},
		{"brcm,bcm2835-sdhci", "\\_SB.SDC1", false, true, true},
		{"brcm,bcm2835-sdhost", "\\_SB.SDC2", false, false, true},
		{"brcm,bcm2835-mbox", "\\_SB.GDV0.RPIQ", false, true, true},
		{"brcm,bcm2835-i2c", "\\_SB.GDV0.I2C1", false, true, true, 0xfe205000},
		{"brcm,bcm2835-i2c", "\\_SB.GDV0.I2C2", false, true, true, 0xfe804000},
		{"brcm,bcm2835-spi", "\\_SB.GDV0.SPI0", false, true, true},
		{"brcm,bcm2835-aux-spi", "\\_SB.GDV0.SPI1", false, true, true, 0xfe215080},
		{"arm,pl011", "\\_SB.URT0", false, true, true},
		{"brcm,bcm2835-aux-uart", "\\_SB.URTM", false, true, true},
		{ /* Sentinel */ }
	};

	/* Device enable */
	for (int i = 0; map[i].fdt_compatible; i++) {
		if ((is_rpi5() && !map[i].on_rpi5) ||
		    (is_rpi4() && !map[i].on_rpi4) ||
		    (is_rpi3() && !map[i].on_rpi3)) {
			enabled = false;
		} else {
			node = fdt_node_offset_by_compatible(gd->fdt_blob, -1,
							     map[i].fdt_compatible);
			while (node != -FDT_ERR_NOTFOUND && map[i].mmio_address) {
				struct fdt_resource r;

				ret = fdt_get_resource(gd->fdt_blob, node, "reg", 0, &r);
				if (ret) {
					node = -FDT_ERR_NOTFOUND;
					break;
				}

				if (r.start == map[i].mmio_address)
					break;

				node = fdt_node_offset_by_compatible(gd->fdt_blob, node,
								     map[i].fdt_compatible);
			}

			enabled = (node > 0) ? fdtdec_get_is_enabled(gd->fdt_blob, node) : 0;
		}
		acpigen_write_scope(ctx, map[i].acpi_scope);
		acpigen_write_name_integer(ctx, "_STA", enabled ? 0xf : 0);
		acpigen_pop_len(ctx);
	}

	/* GPIO quirks */
	node = fdt_node_offset_by_compatible(gd->fdt_blob, -1, "brcm,bcm2835-gpio");
	if (node <= 0)
		node = fdt_node_offset_by_compatible(gd->fdt_blob, -1, "brcm,bcm2711-gpio");

	acpigen_write_scope(ctx, "\\_SB.GDV0.GPI0");
	enabled = (node > 0) ? fdtdec_get_is_enabled(gd->fdt_blob, node) : 0;
	acpigen_write_name_integer(ctx, "_STA", enabled ? 0xf : 0);
	acpigen_pop_len(ctx);

	if (is_rpi4() || is_rpi5()) {
		/* eMMC quirks */
		node = fdt_node_offset_by_compatible(gd->fdt_blob, -1, "brcm,bcm2711-emmc2");
		if (node) {
			phys_addr_t cpu;
			dma_addr_t bus;
			u64 size;

			ret = fdt_get_dma_range(gd->fdt_blob, node, &cpu, &bus, &size);

			acpigen_write_scope(ctx, "\\_SB.GDV1");
			acpigen_write_method_serialized(ctx, "_DMA", 0);
			acpigen_emit_byte(ctx, RETURN_OP);

			if (!ret && bus != cpu)		/* Translated DMA range */
				acpigen_emit_namestring(ctx, "\\_SB.GDV1.DMTR");
			else if (!ret && bus == cpu)	/* Non translated DMA */
				acpigen_emit_namestring(ctx, "\\_SB.GDV1.DMNT");
			else	/* Silicon revisions older than C0: Translated DMA range */
				acpigen_emit_namestring(ctx, "\\_SB.GDV1.DMTR");
			acpigen_pop_len(ctx);
		}
	}

	/* Serial */
	uart_in_use = ~0;
	mini_clock_rate = 0x1000000;

	ret = uclass_get_device_by_driver(UCLASS_SERIAL,
					  DM_DRIVER_GET(bcm283x_pl011_uart),
					  &dev);
	if (!ret)
		uart_in_use = 0;

	ret = uclass_get_device_by_driver(UCLASS_SERIAL,
					  DM_DRIVER_GET(serial_bcm283x_mu),
					  &dev);
	if (!ret) {
		if (uart_in_use == 0)
			log_err("Invalid config: PL011 and MiniUART are both enabled.");
		else
			uart_in_use = 1;

		mini_clock_rate = dev_read_u32_default(dev, "clock", 0x1000000);
	}
	if (uart_in_use > 1)
		log_err("No working serial: PL011 and MiniUART are both disabled.");

	acpigen_write_scope(ctx, "\\_SB.BTH0");
	acpigen_write_name_integer(ctx, "URIU", uart_in_use);
	acpigen_pop_len(ctx);

	acpigen_write_scope(ctx, "\\_SB.URTM");
	acpigen_write_name_integer(ctx, "MUCR", mini_clock_rate);
	acpigen_pop_len(ctx);

	return 0;
}

static int rpi_acpi_write_ssdt(struct acpi_ctx *ctx, const struct acpi_writer *entry)
{
	struct acpi_table_header *ssdt;
	int ret;

	ssdt = ctx->current;
	memset(ssdt, '\0', sizeof(struct acpi_table_header));

	acpi_fill_header(ssdt, "SSDT");
	ssdt->revision = acpi_get_table_revision(ACPITAB_SSDT);
	ssdt->creator_revision = 1;
	ssdt->length = sizeof(struct acpi_table_header);

	acpi_inc(ctx, sizeof(struct acpi_table_header));

	ret = acpi_rpi_board_fill_ssdt(ctx);
	if (ret) {
		ctx->current = ssdt;
		return log_msg_ret("fill", ret);
	}

	/* (Re)calculate length and checksum */
	ssdt->length = ctx->current - (void *)ssdt;
	acpi_update_checksum(ssdt);
	log_debug("SSDT at %p, length %x\n", ssdt, ssdt->length);

	/* Drop the table if it is empty */
	if (ssdt->length == sizeof(struct acpi_table_header))
		return log_msg_ret("fill", -ENOENT);
	acpi_add_table(ctx, ssdt);

	return 0;
}

ACPI_WRITER(5ssdt, "SSDT", rpi_acpi_write_ssdt, 0);
#endif
