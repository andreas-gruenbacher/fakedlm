/*
 * Copyright (C) 2016  Red Hat, Inc.
 * Author: Andreas Grünbacher <agruenba@redhat.com>
 */

#ifndef __ADDR_H
#define __ADDR_H

#include <sys/types.h>
#include <sys/socket.h>
#include <stdbool.h>

struct addr {
	struct addr *next;
	int family;
	int socktype;
	int protocol;
	socklen_t sa_len;
	struct sockaddr sa[0];
};

extern struct addr *find_addrs(const char *name);
extern bool addr_equal(const struct sockaddr *sa1, const struct sockaddr *sa2);
extern bool has_local_addrs(const struct addr *addrs);

#endif  /* __ADDR_H */
