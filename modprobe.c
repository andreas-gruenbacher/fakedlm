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

#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "modprobe.h"

#define MODPROBE "/sbin/modprobe"

void
modprobe(char *name)
{
	int modprobe_pid;

	modprobe_pid = fork();
	if (modprobe_pid == -1) {
		fprintf(stderr, "Failed to run '%s %s': %m\n", MODPROBE, name);
		exit(1);
	} else if (modprobe_pid == 0) {
		char *args[] = {
			MODPROBE,
			name,
			NULL
		};
		execve(MODPROBE, args, NULL);
		fprintf(stderr, "Failed to run '%s %s': %m\n", MODPROBE, name);
		exit(1);
	} else {
		int w, status;

		w = wait(&status);
		if (w != modprobe_pid) {
			fprintf(stderr, "Failed to run '%s %s': %m\n",
				MODPROBE, name);
			if (!WIFEXITED(status) || status != 0)
			fprintf(stderr, "Command '%s %s' failed",
				MODPROBE, name);
			if (WIFEXITED(status))
				fprintf(stderr, " with status %d\n",
					WEXITSTATUS(status));
			else if  (WIFSIGNALED(status))
				fprintf(stderr, " with signal %d\n",
					WTERMSIG(status));
				fprintf(stderr, "\n");
			exit(1);
		}
	}
}
