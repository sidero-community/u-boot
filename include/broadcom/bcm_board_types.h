/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This header defines BCM board types.
 *
 * Copyright (c) 2024 EPAM Systems
 *
 */
#ifndef _BCM_BOARD_TYPES_H_
#define _BCM_BOARD_TYPES_H_

/*
 * TODO: This enumerator defines the main BCM types.
 * These types are defined by the BCM firmware and requested
 * via mbox. Currently only one board type was added.
 * This is needed to determine whether current board is bcm2712
 * or other to control the behaviour of the following drivers:
 * macb and pcie_brcmstb. New board enumerators can be added here
 * if needed.
 * See https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#raspberry-pi-revision-codes
 */
typedef enum {
	BCM2712_RPI_5_B_NEW = 0x17,
	BCM2712_CM5 = 0x18,
	BCM2712_RPI_500 = 0x19,
	BCM2712_CM5_LITE = 0x1A,
} bcm_board_type_t;

/* Range constants for Raspberry Pi 5 family boards */
#define RPI_BOARD_TYPE_RPI5_FAMILY_MIN	BCM2712_RPI_5_B_NEW
#define RPI_BOARD_TYPE_RPI5_FAMILY_MAX	BCM2712_CM5_LITE

#endif /* _BCM_BOARD_TYPES_H_ */
