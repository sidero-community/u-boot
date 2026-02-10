/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _BCM283X_CLK_H_
#define _BCM283X_CLK_H_

/*
 * Stub for BCM283x - the MACB driver uses the CLK framework
 * (CONFIG_CLK) at runtime to obtain the clock rate.
 */
static inline unsigned long get_macb_pclk_rate(unsigned int dev_id)
{
	return 0;
}

#endif /* _BCM283X_CLK_H_ */
