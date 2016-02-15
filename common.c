/*
 * Copyright (C) 2016  Red Hat, Inc.
 * Author: Andreas Grünbacher <agruenba@redhat.com>
 *
 * This file is part of FakeDLM.
 *
 * FakeDLM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FakeDLM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FakeDLM.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"

void
vfailf(const char *fmt, va_list ap)
{
	if (fmt) {
		vfprintf(stderr, fmt, ap);
		fputs(": ", stderr);
	}
	perror(NULL);
	exit(1);
}

void
failf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfailf(fmt, ap);
	va_end(ap);
}

void
fail(const char *s)
{
	perror(s);
	exit(1);
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void
mkdirf(mode_t mode, const char *fmt, ...)
{
	va_list ap, aq;
	char *path;
	int len;

	va_start(ap, fmt);
	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, ap);
	if (len < 0)
		fail(NULL);
	path = alloca(len + 1);
	len = vsnprintf(path, len + 1, fmt, aq);
	if (mkdir(path, mode) == -1)
		fail(path);
	va_end(aq);
	va_end(ap);
}

void
rmdirf(const char *fmt, ...)
{
	va_list ap, aq;
	char *path;
	int len;

	va_start(ap, fmt);
	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, ap);
	if (len < 0)
		fail(NULL);
	path = alloca(len + 1);
	len = vsnprintf(path, len + 1, fmt, aq);
	if (rmdir(path) == -1)
		fail(path);
	va_end(aq);
	va_end(ap);
}

int
vopen_pathf(int flags, mode_t mode, const char *fmt, va_list ap)
{
	va_list aq;
	char *path;
	int len;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, ap);
	if (len < 0)
		fail(NULL);
	path = alloca(len + 1);
	len = vsnprintf(path, len + 1, fmt, aq);
	va_end(aq);
	return open(path, flags, mode);
}

int
open_pathf(int flags, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vopen_pathf(flags, 0, fmt, ap);
	va_end(ap);
	return ret;
}

void
vwrite_pathf(void *value, int len, const char *path_format, va_list ap)
{
	va_list aq;
	int fd;

	va_copy(aq, ap);
	fd = vopen_pathf(O_WRONLY, 0, path_format, ap);
	if (fd == -1 ||
	    write(fd, value, len) == -1 ||
	    close(fd) == -1)
		vfailf(path_format, aq);
	va_end(aq);
}

void
write_pathf(void *value, int len, const char *path_format, ...)
{
	va_list ap;

	va_start(ap, path_format);
	vwrite_pathf(value, len, path_format, ap);
	va_end(ap);
}

void
printf_pathf(const char *value_format, const char *path_format, ...)
{
	char *value;
	int len;
	va_list ap;

	va_start(ap, path_format);
	len = vasprintf(&value, value_format, ap);
	if (len == -1)
		fail(NULL);
	vwrite_pathf(value, len, path_format, ap);
	free(value);
	va_end(ap);
}

