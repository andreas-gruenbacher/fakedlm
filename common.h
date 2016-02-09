/*
 * Copyright (C) 2016  Red Hat, Inc.
 * Author: Andreas Grünbacher <agruenba@redhat.com>
 */

#ifndef __COMMON_H
#define __COMMON_H

#include <fcntl.h>
#include <stdarg.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

#define offsetof(a, b) \
	__builtin_offsetof(a, b)

#define container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	(type *)( (char *)__mptr - offsetof(type,member) );})

extern void  __attribute__((noreturn)) vfailf(const char *fmt, va_list ap);
extern void  __attribute__((format(printf, 1, 2),noreturn)) failf(const char *fmt, ...);
extern void  __attribute__((noreturn)) fail(const char *s);
extern void __attribute__((format(printf, 2, 3))) mkdirf(mode_t mode, const char *fmt, ...);
extern void __attribute__((format(printf, 1, 2))) rmdirf(const char *fmt, ...);
extern int vopen_pathf(int flags, mode_t mode, const char *fmt, va_list ap);
extern int __attribute__((format(printf, 2, 3))) open_pathf(int flags, const char *fmt, ...);
extern void vwrite_pathf(void *value, int len, const char *path_format, va_list ap);
extern void write_pathf(void *value, int len, const char *path_format, ...);
extern void printf_pathf(const char *value_format, const char *path_format, ...);

#endif  /* __COMMON_H */
