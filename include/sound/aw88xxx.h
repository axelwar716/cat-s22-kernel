/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AWINIC aw881xx/aw883xx smart PA presence handshake with the machine
 * driver. Prototypes are part of the CAT S22 Flip v30 vendor module ABI
 * (audio_machine_sdm450.ko calls them with flag 0 to query).
 *
 * flag != 0: record that a PA of that family has registered its DAI.
 * Returns 1 if a PA of that family has registered its DAI, else 0.
 *
 * Copyright (C) 2026 cat-s22
 */
#ifndef __SOUND_AW88XXX_H
#define __SOUND_AW88XXX_H

unsigned char aw881xx_record_dai(unsigned char flag);
unsigned char aw883xx_record_dai(unsigned char flag);

#endif /* __SOUND_AW88XXX_H */
