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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "modprobe.h"

#define MODPROBE "/sbin/modprobe"
#define RMMOD "/sbin/rmmod"

static int
run(char *args[])
{
	int modprobe_pid;

	if (verbose) {
		char **arg;

		for (arg = args; *arg; arg++)
			printf("%s%s", arg == args ? "" : " ", *arg);
		printf("\n");
		fflush(stdout);
	}

	modprobe_pid = fork();
	if (modprobe_pid == -1) {
		return W_EXITCODE(1, 0);
	} else if (modprobe_pid == 0) {
		execve(args[0], args, NULL);
		exit(1);
	} else {
		int status;

		if (waitpid(modprobe_pid, &status, 0) == -1)
			return W_EXITCODE(1, 0);
		return status;
	}
}

static void
check_status(char *args[], int status)
{
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return;

	fprintf(stderr, "Command '%s %s' failed", args[0], args[1]);
	if (WIFEXITED(status)) {
		fprintf(stderr, " with status %d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, " with signal %d", WTERMSIG(status));
	}
	fprintf(stderr, "\n");
	exit(1);
}

void
modprobe(char *name)
{
	char *args[] = { MODPROBE, name, NULL };
	int status;

	status = run(args);
	check_status(args, status);
}

void
rmmod(char *name)
{
	char *args[] = { RMMOD, name, NULL };
	int status;

	status = run(args);
	check_status(args, status);
}
