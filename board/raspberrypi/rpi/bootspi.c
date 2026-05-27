// SPDX-License-Identifier: GPL-2.0+
/*
 * Raspberry Pi 5 (BCM2712) bootloader EEPROM reader.
 *
 * The Pi 5 bootloader image lives in a SPI NOR flash on the SoC's "spi10"
 * controller (downstream alias) at 0x10_7d00_4000.  The controller is a
 * standard BCM2835 SPI block ("brcm,bcm2835-spi") and the flash is a W25Q*
 * device.  This file talks to it directly via polled MMIO, with no DT, no
 * driver-model uclass registration, and no SPI flash framework, so the
 * reader works even when the firmware DT omits the blconfig nvmem-rmem node
 * (as is the case on current Pi 5 firmware).
 *
 * Read-only.  Only the SPI NOR READ (0x03) opcode is ever issued; the
 * EEPROM cannot be modified through this path.
 */

#include <env.h>
#include <log.h>
#include <malloc.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "bootspi.h"

#define BCM2712_BOOTSPI_BASE	0x107d004000ULL

/* BCM2835 SPI controller registers */
#define SPI_CS			0x00
#define SPI_FIFO		0x04
#define SPI_CLK			0x08
#define SPI_DLEN		0x0c

#define CS_CPHA			BIT(2)
#define CS_CPOL			BIT(3)
#define CS_CLEAR_TX		BIT(4)
#define CS_CLEAR_RX		BIT(5)
#define CS_TA			BIT(7)
#define CS_DONE			BIT(16)
#define CS_RXD			BIT(17)
#define CS_TXD			BIT(18)

/*
 * clk_vpu drives this controller; on the Pi 5 it runs in the 500 MHz range.
 * SCLK = clk_vpu / CDIV.  CDIV must be even; 0 means 65536.  64 gives ~7.8
 * MHz which is well within W25Q READ-mode timing across all firmware-set
 * clk_vpu rates and leaves margin even if the firmware bumps the clock.
 * Reading a 2 MiB image at this rate takes ~2 s — fine for preboot.
 */
#define BOOTSPI_CDIV		64

/* SPI NOR opcodes (read-only set) */
#define CMD_READ		0x03

/* Pi 5 ships either a 512 KiB or 2 MiB image */
#define PIEEPROM_MAX_SIZE	SZ_2M

static void bootspi_init(void __iomem *base)
{
	writel(BOOTSPI_CDIV, base + SPI_CLK);
	/* CS0, mode 0 (CPOL=CPHA=0), clear both FIFOs, TA=0 */
	writel(CS_CLEAR_RX | CS_CLEAR_TX, base + SPI_CS);
}

/*
 * Polled byte-at-a-time transfer.  The BCM2835 SPI controller is fully
 * bidirectional: every byte written to the TX FIFO produces a byte in the
 * RX FIFO.  Pass tx=NULL to send 0xff dummies and rx=NULL to discard.
 */
static void bootspi_xfer(void __iomem *base, const u8 *tx, u8 *rx, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		while (!(readl(base + SPI_CS) & CS_TXD))
			;
		writel(tx ? tx[i] : 0xff, base + SPI_FIFO);

		while (!(readl(base + SPI_CS) & CS_RXD))
			;
		if (rx)
			rx[i] = readl(base + SPI_FIFO);
		else
			(void)readl(base + SPI_FIFO);
	}
}

static void bootspi_flash_read(void __iomem *base, u32 addr, u8 *buf,
			       size_t len)
{
	u8 cmd[4] = {
		CMD_READ,
		(addr >> 16) & 0xff,
		(addr >> 8) & 0xff,
		addr & 0xff,
	};

	/* Assert CS (TA=1), clear FIFOs */
	writel(CS_TA | CS_CLEAR_RX | CS_CLEAR_TX, base + SPI_CS);

	bootspi_xfer(base, cmd, NULL, sizeof(cmd));
	bootspi_xfer(base, NULL, buf, len);

	/* Wait for in-flight bits to drain, then deassert CS */
	while (!(readl(base + SPI_CS) & CS_DONE))
		;
	writel(0, base + SPI_CS);
}

int rpi5_bootspi_export_pieeprom(void)
{
	void __iomem *base = (void __iomem *)BCM2712_BOOTSPI_BASE;
	u8 *image;

	image = malloc(PIEEPROM_MAX_SIZE);
	if (!image)
		return -ENOMEM;

	bootspi_init(base);
	bootspi_flash_read(base, 0, image, PIEEPROM_MAX_SIZE);

	env_set_hex("pieeprom_addr", (ulong)image);
	env_set_hex("pieeprom_size", (ulong)PIEEPROM_MAX_SIZE);
	log_info("rpi5 bootspi: pieeprom %u bytes @ %p\n",
		 (unsigned int)PIEEPROM_MAX_SIZE, image);

	return 0;
}
