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
	int family;
	int socktype;
	int protocol;
	socklen_t sa_len;
	struct sockaddr sa[0];
};

extern struct addr *find_addr(const char *name);
extern bool addr_equal(const struct sockaddr *sa1, const struct sockaddr *sa2);
extern bool is_local_addr(const struct addr *addr);

#endif  /* __ADDR_H */
