/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AWINIC AW8155 hearing-aid-compatibility (HAC) amplifier control.
 * The prototype is part of the CAT S22 Flip v30 vendor module ABI.
 *
 * Copyright (C) 2026 cat-s22
 */
#ifndef __SOUND_AW8155_H
#define __SOUND_AW8155_H

#include <linux/types.h>

#ifdef CONFIG_HAC_AW8155
int aw8155_set_en(bool en);
#else
static inline int aw8155_set_en(bool en)
{
	return -ENODEV;
}
#endif

#endif /* __SOUND_AW8155_H */
