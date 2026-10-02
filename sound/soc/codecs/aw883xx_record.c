// SPDX-License-Identifier: GPL-2.0
/*
 * CAT S22 Flip: aw883xx presence handshake for the v30 machine driver.
 *
 * The stock kernel builds the AWINIC aw883xx driver next to aw881xx
 * because the board DT socket ("awinic,aw88xxx_smartpa" at i2c-5 0x34)
 * accepts either part. The S22 unit examined carries an AW88194
 * (aw881xx, chip id 0x1806) and the stock kernel registers no aw883xx
 * I2C driver at run time. Until an aw883xx driver is integrated this
 * only provides the exported handshake, which reports "absent" so the
 * machine driver keeps the aw881xx (or default) codec link.
 */
#include <linux/export.h>
#include <linux/module.h>
#include <sound/aw88xxx.h>

static unsigned char aw883xx_dai_recorded;

unsigned char aw883xx_record_dai(unsigned char flag)
{
	if (flag)
		aw883xx_dai_recorded = 1;

	return aw883xx_dai_recorded;
}
EXPORT_SYMBOL(aw883xx_record_dai);

MODULE_DESCRIPTION("aw883xx DAI presence handshake for CAT S22 Flip");
MODULE_LICENSE("GPL v2");
