/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LINUX_GFP_H
#define _LINUX_GFP_H

#include <linux/bits.h>

/* unused in barebox, just bogus values */
#define GFP_KERNEL	0
#define GFP_NOFS	0
#define GFP_USER	0
#define __GFP_NOWARN	0
#define __GFP_ZERO	BIT(8)

#endif
