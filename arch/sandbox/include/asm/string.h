/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SANDBOX_STRING_H_
#define __SANDBOX_STRING_H_

#ifdef CONFIG_FUZZ_EXTERNAL
#include <linux/types.h>

#define __HAVE_ARCH_STRCASECMP
#define __HAVE_ARCH_STRNCASECMP
#define __HAVE_ARCH_STRCMP
#define __HAVE_ARCH_STRNCMP
#define __HAVE_ARCH_MEMCMP

extern int strcasecmp(const char *s1, const char *s2);
extern int strncasecmp(const char *s1, const char *s2, size_t n);
extern int strcmp(const char *,const char *);
extern int strncmp(const char *,const char *,__kernel_size_t);
extern int memcmp(const void *,const void *,__kernel_size_t);

#endif

#ifdef __PBL__
char * strchr(const char *s, int ch);
#define strchr strchr
#define __HAVE_ARCH_STRCHR
#endif

#endif
