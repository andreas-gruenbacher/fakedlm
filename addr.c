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
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "addr.h"

struct addr *
find_addrs(const char *name)
{
	struct addrinfo hints;
	struct addrinfo *ai = NULL;
	struct addr *addrs = NULL;
	struct addr **tail = &addrs;
	int g;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;

	g = getaddrinfo(name, NULL, &hints, &ai);
	if (g != 0) {
		fprintf(stderr, "%s: %s\n", name, gai_strerror(g));
		exit(1);
	}
	for(; ai; ai = ai->ai_next) {
		struct addr *addr;

		if (ai->ai_family == AF_INET) {
			struct sockaddr_in *sin =
				(struct sockaddr_in *)ai->ai_addr;

			if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
				continue;
		} else if (ai->ai_family == AF_INET6) {
			struct sockaddr_in6 *sin6 =
				(struct sockaddr_in6 *)ai->ai_addr;

			if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr) ||
			    IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
				continue;
		} else
			continue;

		addr = malloc(sizeof(*addr) + ai->ai_addrlen);
		if (!addr)
			fail(NULL);
		addr->family = ai->ai_family;
		addr->socktype = ai->ai_socktype;
		addr->protocol = ai->ai_protocol;
		addr->sa_len = ai->ai_addrlen;
		memcpy(addr->sa, ai->ai_addr, ai->ai_addrlen);
		addr->next = NULL;
		*tail = addr;
		tail = &addr->next;
	}
	freeaddrinfo(ai);

	if (!addrs) {
		fprintf(stderr, "%s: %s\n",
			name,
			gai_strerror(EAI_NODATA));
	}
	return addrs;
}

bool
addr_equal(const struct sockaddr *sa1, const struct sockaddr *sa2)
{
	if (sa1->sa_family != sa2->sa_family)
		return false;
	if (sa1->sa_family == AF_INET) {
		const struct sockaddr_in *sin1 =
			(const struct sockaddr_in *)sa1;
		const struct sockaddr_in *sin2 =
			(const struct sockaddr_in *)sa2;

		return memcmp(&sin1->sin_addr, &sin2->sin_addr,
			      sizeof(sin1->sin_addr)) == 0;
	} else if (sa1->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6_1 =
			(const struct sockaddr_in6 *)sa1;
		const struct sockaddr_in6 *sin6_2 =
			(const struct sockaddr_in6 *)sa2;

		return memcmp(&sin6_1->sin6_addr, &sin6_2->sin6_addr,
			      sizeof(sin6_1->sin6_addr)) == 0;
	} else
		return false;
}

bool
has_local_addrs(const struct addr *addrs)
{
	static struct ifaddrs *ifa = NULL;
	const struct addr *addr;

	if (!ifa) {
		if (getifaddrs(&ifa) == -1)
			fail("Cannot determine local network interface addresses\n");
	}

	for (addr = addrs; addr; addr = addr->next) {
		const struct ifaddrs *i;

		for (i = ifa; i; i = i->ifa_next) {
			if (addr_equal(addr->sa, i->ifa_addr))
				return true;
		}
	}
	return false;
}
