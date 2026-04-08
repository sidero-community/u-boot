// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom STB "UPG GIO" GPIO controller driver
 *
 * Supports the brcm,bcm7445-gpio / brcm,brcmstb-gpio controllers found
 * in BCM2712 (Raspberry Pi 5) and other Broadcom STB SoCs.
 *
 * Based on the Linux gpio-brcmstb.c driver and the U-Boot bcm2835_gpio.c.
 */

#include <dm.h>
#include <errno.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/read.h>

/* Per-bank register offsets (each bank occupies 0x20 bytes) */
#define GIO_BANK_SIZE	0x20
#define GIO_REG_ODEN	0x00	/* open-drain enable */
#define GIO_REG_DATA	0x04	/* data (read = pin level, write = output) */
#define GIO_REG_IODIR	0x08	/* I/O direction: 1 = input, 0 = output */
#define GIO_REG_EC	0x0c	/* edge configuration */
#define GIO_REG_EI	0x10	/* edge interrupt enable */
#define GIO_REG_MASK	0x14	/* interrupt mask */
#define GIO_REG_LEVEL	0x18	/* level interrupt */
#define GIO_REG_STAT	0x1c	/* interrupt status */

#define MAX_BANKS	4

struct brcmstb_gpio_priv {
	void __iomem *base;
	int num_banks;
	u32 bank_widths[MAX_BANKS];
};

static int brcmstb_gpio_to_bank(struct brcmstb_gpio_priv *priv,
				unsigned int gpio, unsigned int *bit)
{
	int bank;
	unsigned int offset = gpio;

	for (bank = 0; bank < priv->num_banks; bank++) {
		if (offset < priv->bank_widths[bank]) {
			*bit = offset;
			return bank;
		}
		offset -= priv->bank_widths[bank];
	}

	return -EINVAL;
}

static int brcmstb_gpio_direction_input(struct udevice *dev, unsigned int gpio)
{
	struct brcmstb_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bit;
	int bank;
	u32 val;

	bank = brcmstb_gpio_to_bank(priv, gpio, &bit);
	if (bank < 0)
		return bank;

	val = readl(priv->base + bank * GIO_BANK_SIZE + GIO_REG_IODIR);
	val |= BIT(bit);
	writel(val, priv->base + bank * GIO_BANK_SIZE + GIO_REG_IODIR);

	return 0;
}

static int brcmstb_gpio_direction_output(struct udevice *dev,
					 unsigned int gpio, int value)
{
	struct brcmstb_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bit;
	int bank;
	u32 val;

	bank = brcmstb_gpio_to_bank(priv, gpio, &bit);
	if (bank < 0)
		return bank;

	/* Set the output value first */
	val = readl(priv->base + bank * GIO_BANK_SIZE + GIO_REG_DATA);
	if (value)
		val |= BIT(bit);
	else
		val &= ~BIT(bit);
	writel(val, priv->base + bank * GIO_BANK_SIZE + GIO_REG_DATA);

	/* Then set direction to output */
	val = readl(priv->base + bank * GIO_BANK_SIZE + GIO_REG_IODIR);
	val &= ~BIT(bit);
	writel(val, priv->base + bank * GIO_BANK_SIZE + GIO_REG_IODIR);

	return 0;
}

static int brcmstb_gpio_get_value(struct udevice *dev, unsigned int gpio)
{
	struct brcmstb_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bit;
	int bank;
	u32 val;

	bank = brcmstb_gpio_to_bank(priv, gpio, &bit);
	if (bank < 0)
		return bank;

	val = readl(priv->base + bank * GIO_BANK_SIZE + GIO_REG_DATA);

	return !!(val & BIT(bit));
}

static int brcmstb_gpio_set_value(struct udevice *dev, unsigned int gpio,
				  int value)
{
	struct brcmstb_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bit;
	int bank;
	u32 val;

	bank = brcmstb_gpio_to_bank(priv, gpio, &bit);
	if (bank < 0)
		return bank;

	val = readl(priv->base + bank * GIO_BANK_SIZE + GIO_REG_DATA);
	if (value)
		val |= BIT(bit);
	else
		val &= ~BIT(bit);
	writel(val, priv->base + bank * GIO_BANK_SIZE + GIO_REG_DATA);

	return 0;
}

static int brcmstb_gpio_get_function(struct udevice *dev, unsigned int gpio)
{
	struct brcmstb_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bit;
	int bank;
	u32 val;

	bank = brcmstb_gpio_to_bank(priv, gpio, &bit);
	if (bank < 0)
		return GPIOF_UNKNOWN;

	val = readl(priv->base + bank * GIO_BANK_SIZE + GIO_REG_IODIR);

	return (val & BIT(bit)) ? GPIOF_INPUT : GPIOF_OUTPUT;
}

static const struct dm_gpio_ops brcmstb_gpio_ops = {
	.direction_input	= brcmstb_gpio_direction_input,
	.direction_output	= brcmstb_gpio_direction_output,
	.get_value		= brcmstb_gpio_get_value,
	.set_value		= brcmstb_gpio_set_value,
	.get_function		= brcmstb_gpio_get_function,
};

static int brcmstb_gpio_probe(struct udevice *dev)
{
	struct brcmstb_gpio_priv *priv = dev_get_priv(dev);
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	int ret, num_banks, i;
	u32 total_gpios = 0;

	priv->base = dev_remap_addr(dev);
	if (!priv->base)
		return -EINVAL;

	num_banks = dev_read_size(dev, "brcm,gpio-bank-widths");
	if (num_banks < 0)
		return num_banks;
	num_banks /= sizeof(u32);

	if (num_banks > MAX_BANKS) {
		dev_err(dev, "too many banks: %d (max %d)\n",
			num_banks, MAX_BANKS);
		return -EINVAL;
	}

	ret = dev_read_u32_array(dev, "brcm,gpio-bank-widths",
				 priv->bank_widths, num_banks);
	if (ret)
		return ret;

	priv->num_banks = num_banks;

	for (i = 0; i < num_banks; i++)
		total_gpios += priv->bank_widths[i];

	uc_priv->gpio_count = total_gpios;
	uc_priv->bank_name = dev_read_name(dev);

	return 0;
}

static const struct udevice_id brcmstb_gpio_ids[] = {
	{ .compatible = "brcm,bcm7445-gpio" },
	{ .compatible = "brcm,brcmstb-gpio" },
	{ }
};

U_BOOT_DRIVER(brcmstb_gpio) = {
	.name		= "brcmstb_gpio",
	.id		= UCLASS_GPIO,
	.of_match	= brcmstb_gpio_ids,
	.ops		= &brcmstb_gpio_ops,
	.probe		= brcmstb_gpio_probe,
	.priv_auto	= sizeof(struct brcmstb_gpio_priv),
};
