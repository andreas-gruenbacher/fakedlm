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
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/dlm_device.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <alloca.h>

#include "common.h"

#define MISC_PREFIX "/dev/misc/"
#define DLM_CONTROL_PATH MISC_PREFIX "dlm-control"

static const char *progname;
static int control_fd = -1;

static void
create_lockspace(const char *name)
{
	struct dlm_write_request *req;
	int len = strlen(name);
	int minor;

	req = alloca(sizeof(*req) + len);
	if (!req)
		fail(NULL);
	memset(req, 0, sizeof(*req) + len);
	req->version[0] = DLM_DEVICE_VERSION_MAJOR;
	req->version[1] = DLM_DEVICE_VERSION_MINOR;
	req->version[2] = DLM_DEVICE_VERSION_PATCH;
	req->cmd = DLM_USER_CREATE_LOCKSPACE;
	req->is64bit = sizeof(long) == sizeof(long long);
	/* req->i.lspace.flags = DLM_LSFL_TIMEWARN; */
	memcpy(req->i.lspace.name, name, len);

	if (control_fd == -1) {
		control_fd = open(DLM_CONTROL_PATH, O_RDWR);
		if (control_fd == -1)
			fail(DLM_CONTROL_PATH);
	}

	minor = write(control_fd, req, sizeof(*req) + len);
	if (minor < 0)
		failf("%s: %s", name, DLM_CONTROL_PATH);
	printf("Minor device number %u created\n", minor);
}

static void
remove_lockspace(const char *name, bool force)
{
	struct dlm_write_request req;
	struct stat st;
	char *path;
	int minor;

	if (asprintf(&path, "%sdlm_%s", MISC_PREFIX, name) == -1)
		fail(NULL);
	if (stat(path, &st) == -1)
		fail(path);
	free(path);
	minor = minor(st.st_rdev);

	memset(&req, 0, sizeof(req));
	req.version[0] = DLM_DEVICE_VERSION_MAJOR;
	req.version[1] = DLM_DEVICE_VERSION_MINOR;
	req.version[2] = DLM_DEVICE_VERSION_PATCH;
	req.cmd = DLM_USER_REMOVE_LOCKSPACE;
	req.is64bit = sizeof(long) == sizeof(long long);
	req.i.lspace.minor = minor;
	if (force)
		req.i.lspace.flags = DLM_USER_LSFLG_FORCEFREE;

	if (control_fd == -1) {
		control_fd = open(DLM_CONTROL_PATH, O_RDWR);
		if (control_fd == -1)
			fail(DLM_CONTROL_PATH);
	}

	printf("Removing minor device number %u\n", minor);
	if (write(control_fd, &req, sizeof(req)) == -1)
		failf("%s: %s", name, DLM_CONTROL_PATH);
}

static void
usage(int status)
{
	fprintf(status ? stderr : stdout,
		"USAGE: %s {--create | --remove [--force]} lockspace ...\n",
		progname);
	exit(status);
}

static struct option long_options[] = {
	{ "create", no_argument, NULL, 'c' },
	{ "remove", no_argument, NULL, 'r' },
	{ "force", no_argument, NULL, 'f' },
	{ }
};

int main(int argc, char *argv[])
{
	enum { NONE, CREATE, REMOVE } op = NONE;
	bool force = false;
	int opt;

	progname = argv[0];
	while ((opt = getopt_long(argc, argv, "crf", long_options, NULL)) != -1) {
		switch(opt) {
		case 'c':  /* --create */
			if (op == REMOVE)
				usage(2);
			op = CREATE;
			break;

		case 'r': /* --remove */
			if (op == CREATE)
				usage(2);
			op = REMOVE;
			break;

		case 'f':  /* --force */
			force = true;
			break;

		case '?':  /*  bad option */
			usage(2);
		}
	}
	if (op == NONE || (op == CREATE && force) || optind == argc)
		usage(2);
	for (; optind < argc; optind++) {
		if (op == CREATE)
			create_lockspace(argv[optind]);
		else if (op == REMOVE)
			remove_lockspace(argv[optind], force);
	}
	return 0;
}
