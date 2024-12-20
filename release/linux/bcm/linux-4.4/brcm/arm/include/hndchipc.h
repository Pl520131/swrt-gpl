/*
 * HND SiliconBackplane chipcommon support - OS independent.
 *
 * Copyright (C) 2016, Broadcom. All Rights Reserved.
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *
 * <<Broadcom-WL-IPTag/Open:>>
 *
 * $Id: hndchipc.h 523133 2014-12-27 05:50:30Z $
 */

#ifndef _hndchipc_h_
#define _hndchipc_h_

#include <typedefs.h>
#include <siutils.h>

#ifdef RTE_UART
typedef void (*si_serial_init_fn)(si_t *sih, void *regs, uint irq, uint baud_base, uint reg_shift);
#else
typedef void (*si_serial_init_fn)(void *regs, uint irq, uint baud_base, uint reg_shift);
#endif
extern void si_serial_init(si_t *sih, si_serial_init_fn add);

extern void *hnd_jtagm_init(si_t *sih, uint clkd, bool exttap);
extern void hnd_jtagm_disable(si_t *sih, void *h);
extern uint32 jtag_scan(si_t *sih, void *h, uint irsz, uint32 ir0, uint32 ir1,
                        uint drsz, uint32 dr0, uint32 *dr1, bool rti);

#endif /* _hndchipc_h_ */
