/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Raspberry Pi 5 bootloader EEPROM reader — see bootspi.c.
 */

#ifndef __RPI_BOOTSPI_H
#define __RPI_BOOTSPI_H

/*
 * Read the full Pi 5 bootloader EEPROM image into a persistent buffer and
 * publish its location/size as pieeprom_addr / pieeprom_size environment
 * variables.  Preboot uses these to fatwrite pieeprom.bin to USB.  Returns
 * 0 on success, -errno otherwise.
 */
int rpi5_bootspi_export_pieeprom(void);

#endif /* __RPI_BOOTSPI_H */
