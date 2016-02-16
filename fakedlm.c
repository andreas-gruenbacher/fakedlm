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

/*
 * FakeDLM is a replacement for dlm_controld for testing purposes.  It assumes
 * perfect network connectivity and is not intended or suitable for controlling
 * DLM in production use.
 *
 * Start FakeDLM with a list of all the node names or node IP addresses on the
 * command line, in the same order on all nodes.  The cluster nodes will then
 * connect to each other, and FakeDLM will start managing lockspace membership.
 *
 * A very simple TCP-based node coordination protocol independent from DLM's
 * internal protocol is used.  Message types:
 *
 * MSG_CLOSE
 *   Each node creates listening sockets for its peers to connect to and tries
 *   to connect to each of its peers.  Once an incoming or outgoing connection
 *   is accepted, node->outgoing_fd is set to that connection.  If the node
 *   with the lower node ID notices that it has two connections to the same
 *   peer (an accepted incoming and a connected otgoing connection), it sends a
 *   MSG_CLOSE message on one of these connections and sets node->outgoing_fd
 *   to the other connection.  All nodes close connections on which they
 *   receive MSG_CLOSE messages.
 *
 * MSG_STOP_LOCKSPACE [lockspace_name]:
 *   Request to stop a particular lockspace on the receiving node on behalf of
 *   the sending node.  This takes a "lock" on the lockspace.  The receiving
 *   node must reply with MSG_LOCKSPACE_STOPPED.
 *
 * MSG_LOCKSPACE_STOPPED [lockspace_name]:
 *   A lockspace has been stopped on the sending node.  The receiving node must
 *   follow up with either MSG_JOIN_LOCKSPACE or MSG_LEAVE_LOCKSPACE.
 *
 * MSG_JOIN_LOCKSPACE [lockspace_name],
 * MSG_LEAVE_LOCKSPACE [lockspace_name]:
 *   Request to join or leave a lockspace.  The lockspace must have been
 *   stopped with MSG_STOP_LOCKSPACE first.  The receiving node must restart
 *   the lockspace once there are no more pending "locks" on the lockspace by
 *   other nodes.
 *
 * When a node loses connectivity to any of its peers (but not when it closes a
 * connection in response to a MSG_CLOSE * requests), it leaves all lockspaces
 * and waits for full connectivity to be re-established.
 */

#define _GNU_SOURCE
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/dlm_device.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <aio.h>
#include <getopt.h>
#include <assert.h>

#include "common.h"
#include "addr.h"
#include "modprobe.h"
#include "crc.h"
#include "list.h"

#define DLM_SYSFS_DIR "/sys/kernel/dlm"
#define MISC_PREFIX "/dev/misc/"
#define DLM_CONTROL_PATH MISC_PREFIX "dlm-control"
#define DLM_MONITOR_PATH MISC_PREFIX "dlm-monitor"

#define CONFIGFS_PREFIX "/sys/kernel/config/"
#define CONFIG_DLM CONFIGFS_PREFIX "dlm/"
#define CONFIG_DLM_CLUSTER CONFIG_DLM "cluster/"

#define FAKEDLM_PORT 21066
#define DLM_PORT 21064
/* #define DLM_MAX_ADDR_COUNT 3 */

#define MAX_LINE_UEVENT 256

typedef uint32_t node_mask_t;

#define MAX_NODES (sizeof(node_mask_t) * 8)

#define LISTENING_SOCKET_MARKER ((void *)1)

struct node {
	char *name;
	int nodeid;
	struct addr *addrs;
	int outgoing_fd;
	int connecting_fd;
	bool nodir;
	int weight;
	struct node *next;
};

struct lockspace {
	char *name;
	uint32_t global_id;
	short minor;
	int control_fd;
	node_mask_t members;
	node_mask_t stopping;
	node_mask_t stopped;
	node_mask_t joining;
	node_mask_t leaving;
	struct lockspace *next;
};

struct aio_request {
	struct list_head list;
	struct aiocb aiocb;
	void (*complete)(struct aio_request *);
};

struct lockspace_aio_request {
	struct aio_request aio_req;
	struct lockspace *ls;
};

struct poll_callback {
	void (*callback)(int, short, void *);
	void *arg;
};

struct poll_callbacks {
	struct pollfd *pollfds;
	struct poll_callback *callbacks;
	int num;
};

enum msg_type {
	MSG_CLOSE = 1,
	MSG_STOP_LOCKSPACE,
	MSG_LOCKSPACE_STOPPED,
	MSG_JOIN_LOCKSPACE,
	MSG_LEAVE_LOCKSPACE,
};

struct proto_msg {
	uint16_t msg;
	char lockspace_name[DLM_LOCKSPACE_LEN];
};

bool verbose;
bool debug;

static const char *progname;
static char *cluster_name;
static int fakedlm_port = FAKEDLM_PORT;
static int dlm_port = DLM_PORT;
static enum { PROTO_TCP, PROTO_SCTP } dlm_protocol;
static int kernel_monitor_fd = -1;
static int control_fd = -1;
static struct lockspace *lockspaces;
static int joined_lockspaces;
static struct node *nodes, *local_node;
static node_mask_t all_nodes;
static node_mask_t connected_nodes;
static int shut_down;
struct poll_callbacks cbs;
static LIST_HEAD(aio_pending);
LIST_HEAD(aio_completed);

#define MSG_NAME(x) [MSG_ ## x] = #x
static const char *msg_names[] = {
	MSG_NAME(CLOSE),
	MSG_NAME(STOP_LOCKSPACE),
	MSG_NAME(LOCKSPACE_STOPPED),
	MSG_NAME(JOIN_LOCKSPACE),
	MSG_NAME(LEAVE_LOCKSPACE),
};

static const char *msg_name(enum msg_type type)
{
	if (type >= ARRAY_SIZE(msg_names))
		return NULL;
	return msg_names[type];
}

static node_mask_t
nodeid_mask(int nodeid)
{
	return 1U << (nodeid - 1);
}

static node_mask_t
node_mask(struct node *node)
{
	return nodeid_mask(node->nodeid);
}

/*
 * Print a nodes mask.
 */
static void
print_nodes(FILE *file, node_mask_t mask)
{
	int nodeid;
	bool first = true;

	fprintf(file, "[");
	while ((nodeid = ffs(mask))) {
		if (first) {
			fprintf(file, "%u", nodeid);
			first = false;
		} else {
			fprintf(file, ", %u", nodeid);
		}
		mask &= ~nodeid_mask(nodeid);
	}
	fprintf(file, "]");
}

/*
 * Add a file descriptor, poll event mask, and associated callback for polling.
 */
static void
add_poll_callback(struct poll_callbacks *cbs, int fd, short events,
		  void (*callback)(int, short, void *), void *arg)
{
	cbs->pollfds = realloc(cbs->pollfds,
			       (cbs->num + 1) * sizeof(*cbs->pollfds));
	cbs->callbacks = realloc(cbs->callbacks,
				 (cbs->num + 1) * sizeof(*cbs->callbacks));
	if (!cbs->pollfds || !cbs->callbacks)
		fail(NULL);
	memset(cbs->pollfds + cbs->num, 0, sizeof(*cbs->pollfds));
	memset(cbs->callbacks + cbs->num, 0, sizeof(*cbs->callbacks));
	cbs->pollfds[cbs->num].fd = fd;
	cbs->pollfds[cbs->num].events = events;
	cbs->callbacks[cbs->num].callback = callback;
	cbs->callbacks[cbs->num].arg = arg;
	cbs->num++;
}

/*
 * Remove a file descriptor from polling.
 */
static void
remove_poll_callback(struct poll_callbacks *cbs, int fd)
{
	int n;

	for (n = 0; n < cbs->num; n++) {
		if (cbs->pollfds[n].fd == fd) {
			memmove(cbs->pollfds + n, cbs->pollfds + n + 1,
				(cbs->num - n - 1) * sizeof(*cbs->pollfds));
			memmove(cbs->callbacks + n, cbs->callbacks + n + 1,
				(cbs->num - n - 1) * sizeof(*cbs->callbacks));
			cbs->num--;
			cbs->pollfds =
				realloc(cbs->pollfds,
					cbs->num * sizeof(*cbs->pollfds));
			cbs->callbacks =
				realloc(cbs->callbacks,
					cbs->num * sizeof(*cbs->callbacks));
			break;
		}
	}
}

/*
 * Update the poll event mask and/or callback for a given file descriptor.
 */
static void
update_poll_callback(struct poll_callbacks *cbs, int fd, short events,
		     void (*callback)(int, short, void *), void *arg)
{
	int n;

	for (n = 0; n < cbs->num; n++) {
		if (cbs->pollfds[n].fd == fd) {
			cbs->pollfds[n].events = events;
			cbs->callbacks[n].callback = callback;
			cbs->callbacks[n].arg = arg;
			break;
		}
	}
}

/*
 * Close the connections to a peer node.
 */
static void
close_connections(struct node *node)
{
	if (node->outgoing_fd != -1) {
		close(node->outgoing_fd);
		remove_poll_callback(&cbs, node->outgoing_fd);
		node->outgoing_fd = -1;
	}
	if (node->connecting_fd != -1) {
		close(node->connecting_fd);
		remove_poll_callback(&cbs, node->connecting_fd);
		node->connecting_fd = -1;
	}
	connected_nodes &= ~node_mask(node);
}

/*
 * Close the connections to all peer nodes.
 */
static void
close_all_connections(void)
{
	struct node *node;
	int n = 0;

	while (n < cbs.num) {
		if (cbs.callbacks[n].arg == LISTENING_SOCKET_MARKER) {
			remove_poll_callback(&cbs, cbs.pollfds[n].fd);
			continue;
		}
		n++;
	}

	for (node = nodes; node; node = node->next)
		close_connections(node);
}

/*
 * Send a FakeDLM message to a peer node.
 */
static bool
send_msg(struct node *node, enum msg_type type, const char *lockspace_name)
{
	struct proto_msg msg = {
		.msg = htons(type),
	};
	int ret;

	if (node->outgoing_fd == -1)
		return false;

	if (verbose) {
		printf("> %u %s", node->nodeid,
		       msg_name(type));
		if (lockspace_name)
			printf(" %s", lockspace_name);
		printf("\n");
		fflush(stdout);
	}
	if (lockspace_name)
		strncpy(msg.lockspace_name, lockspace_name,
			DLM_LOCKSPACE_LEN);
	ret = write(node->outgoing_fd, &msg, sizeof(msg));
	if (ret != sizeof(msg)) {
		if (ret > 0)
			errno = EIO;
		fprintf(stderr, "%u: %m\n", node->nodeid);
		close_connections(node);
		return false;
	}
	return true;
}

/*
 * Create a new node in-memory object and look up the node's addresses.
 */
static struct node *
new_node(const char *name)
{
	struct node *node;

	node = malloc(sizeof(*node));
	if (!node)
		fail(NULL);
	memset(node, 0, sizeof(*node));
	node->name = strdup(name);
	if (!node->name)
		fail(NULL);
	node->weight = 1;
	node->addrs = find_addrs(name);
	node->nodeid = -1;
	node->outgoing_fd = -1;
	node->connecting_fd = -1;
	return node;
}

static struct lockspace *
find_lockspace(const char *name)
{
	struct lockspace *ls;

	for (ls = lockspaces; ls; ls = ls->next) {
		if (strcmp(name, ls->name) == 0)
			return ls;
	}
	return NULL;
}

static uint32_t
global_id(const char *name)
{
	char full_name[strlen(name) + 8];

	snprintf(full_name, sizeof(full_name), "dlm:ls:%s", name);
	return cpgname_to_crc(full_name, strlen(full_name) + 1);
}

/*
 * Create a new lockspace in-memory object.
 */
static struct lockspace *
new_lockspace(const char *name)
{
	struct lockspace *ls;

	ls = malloc(sizeof(*ls));
	if (!ls)
		fail(NULL);
	memset(ls, 0, sizeof(*ls));
	ls->name = strdup(name);
	if (!ls->name)
		fail(NULL);
	ls->global_id = global_id(name);
	ls->minor = -1;
	ls->control_fd = -1;
	ls->stopped = node_mask(local_node);
	ls->next = lockspaces;
	lockspaces = ls;
	printf("New lockspace '%s' [%04x]\n", ls->name, ls->global_id);
	fflush(stdout);
	return ls;
}

/*
 * Completion of release_lockspace().
 */
static void
complete_release(struct aio_request *aio_req)
{
	struct dlm_write_request *req = (void *)aio_req->aiocb.aio_buf;
	struct lockspace *ls;

	for (ls = lockspaces; ls; ls = ls->next) {
		if (ls->minor == req->i.lspace.minor)
			break;
	}
	if (ls) {
		/*
		 * Lockspaces are reference counted in the kernel.  The first
		 * DLM_USER_CREATE_LOCKSPACE request creates a lockspace; the
		 * last DLM_USER_REMOVE_LOCKSPACE request removes it.  Continue
		 * removing the lockspace until it disappears.
		 */
		list_add(&aio_req->list, &aio_pending);
		if (aio_write(&aio_req->aiocb) == 0)
			return;
		list_del(&aio_req->list);
		fail(NULL);
	}
	free((void *)aio_req->aiocb.aio_buf);
	free(aio_req);
}

/*
 * Ask the kernel to release / remove a lockspace.  Lockspaces are reference
 * counted and are only removed once their reference count drops to zero.
 *
 * The force parameter forces releasing a lockspace even when there are active
 * locks; otherwise, releasing a lockspace with active locks fails with
 * errno == EBUSY.
 *
 * Triggers an offline@/kernel/dlm/<name> uevent when the lockspace is removed
 * which FakeDLM / dlm_controld the uses for leaving the lockspace cluster-wide
 * and for removing its configuration.
 */
static void
release_lockspace(struct lockspace *ls, bool force)
{
	struct aio_request *aio_req;
	struct dlm_write_request *req;

	req = malloc(sizeof(*req));
	if (!req)
		fail(NULL);
	memset(req, 0, sizeof(*req));
	req->version[0] = DLM_DEVICE_VERSION_MAJOR;
	req->version[1] = DLM_DEVICE_VERSION_MINOR;
	req->version[2] = DLM_DEVICE_VERSION_PATCH;
	req->cmd = DLM_USER_REMOVE_LOCKSPACE;
	req->is64bit = sizeof(long) == sizeof(long long);
	req->i.lspace.minor = ls->minor;
	if (force)
		req->i.lspace.flags = DLM_USER_LSFLG_FORCEFREE;

	if (control_fd == -1) {
		control_fd = open(DLM_CONTROL_PATH, O_RDWR);
		if (control_fd == -1)
			fail(DLM_CONTROL_PATH);
	}

	/* A normal write would block until the uevent has been marked as done.  */

	aio_req = malloc(sizeof(*aio_req));
	if (!aio_req)
		fail(NULL);
	memset(aio_req, 0, sizeof(*aio_req));
	aio_req->aiocb.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
	aio_req->aiocb.aio_sigevent.sigev_signo = SIGUSR1;
	aio_req->aiocb.aio_sigevent.sigev_value.sival_ptr = aio_req;
	aio_req->aiocb.aio_fildes = control_fd;
	aio_req->aiocb.aio_nbytes = sizeof(*req);
	aio_req->aiocb.aio_buf = req;
	aio_req->complete = complete_release;
	list_add(&aio_req->list, &aio_pending);
	if (aio_write(&aio_req->aiocb) == 0)
		return;
	list_del(&aio_req->list);
	fail(NULL);
}

static void
release_lockspaces(bool force)
{
	struct lockspace *ls;

	for (ls = lockspaces; ls; ls = ls->next)
		release_lockspace(ls, force);
}

static void
lockspace_status(struct lockspace *ls, const char *status)
{
	if (debug) {
		printf("Lockspace %s %s: stopping=", ls->name, status);
		print_nodes(stdout, ls->stopping);
		printf(", stopped=");
		print_nodes(stdout, ls->stopped);
		printf(", joining=");
		print_nodes(stdout, ls->joining);
		printf(", leaving=");
		print_nodes(stdout, ls->leaving);
		printf(", members=");
		print_nodes(stdout, ls->members);
		printf("\n");
		fflush(stdout);
	}
}

/*
 * Triggered to update the local configuration of a logspace once it has been
 * stopped cluster-wide.  When the local node is joining a lockspace, add all
 * lockspace members to the lockspace configuration including the local node.
 * When the local node is leaving a lockspace, remove the entire lockspace
 * configuration.  When the membership of the local node doesn't change, only
 * add the joining and remove the leaving nodes.
 *
 * Once the configuration has been updated, start / restart the lockspace
 * locally with:
 *
 *   echo 1 > /sys/kernel/dlm/<name>/control
 *
 * Requests for the local node to join or leave the lockspace are triggered by
 * uevents which are completed by:
 *
 *   echo 0 > /sys/kernel/dlm/<name>/event_done
 */
static void
update_lockspace(struct lockspace *ls)
{
	node_mask_t joining = 0;
	node_mask_t leaving = 0;
	node_mask_t new_members;
	struct node *node;

	if (ls->joining & node_mask(local_node)) {
		printf_pathf("%u", "%s/%s/id", ls->global_id, DLM_SYSFS_DIR, ls->name);
		if (local_node->nodir)
			printf_pathf("%d", "%s/%s/nodir", 1, DLM_SYSFS_DIR, ls->name);
		mkdirf(0777, "%sspaces/%s", CONFIG_DLM_CLUSTER, ls->name);
		joining = ls->members | ls->joining;
	} else if (ls->members & node_mask(local_node)) {
		joining = ls->joining;
	}
	if (ls->leaving & node_mask(local_node)) {
		leaving = ls->members | ls->leaving;
	} else if (ls->members & node_mask(local_node)) {
		leaving = ls->leaving;
	}
	for (node = nodes; node; node = node->next) {
		if (joining & node_mask(node)) {
			mkdirf(0777, "%sspaces/%s/nodes/%d", CONFIG_DLM_CLUSTER,
			       ls->name, node->nodeid);
			printf_pathf("%d", "%sspaces/%s/nodes/%d/nodeid", node->nodeid,
				     CONFIG_DLM_CLUSTER, ls->name, node->nodeid);
			if (node->weight != 1)
				printf_pathf("%d", "%sspaces/%s/nodes/%d/weight",
					     node->weight, CONFIG_DLM_CLUSTER,
					     ls->name, node->nodeid);
		} else if (leaving & node_mask(node)) {
			rmdirf("%sspaces/%s/nodes/%d", CONFIG_DLM_CLUSTER,
			       ls->name, node->nodeid);
		}
	}
	if (ls->joining & node_mask(local_node)) {
		joined_lockspaces++;
	}
	if (ls->leaving & node_mask(local_node)) {
		joined_lockspaces--;
		rmdirf("%sspaces/%s", CONFIG_DLM_CLUSTER, ls->name);
	}
	new_members = (ls->members | ls->joining) & ~ls->leaving;
	if (new_members & node_mask(local_node)) {
		/* (Re)start the kernel recovery daemon. */
		if (ls->control_fd == -1) {
			ls->control_fd = open_pathf(O_WRONLY, "%s/%s/control",
						    DLM_SYSFS_DIR, ls->name);
			if (ls->control_fd == -1)
				failf("%s/%s/control", DLM_SYSFS_DIR, ls->name);
		}
		if (write(ls->control_fd, "1", 1) != 1)
			failf("%s/%s/control", DLM_SYSFS_DIR, ls->name);
		ls->stopped &= ~node_mask(local_node);
	}
	if ((ls->joining | ls->leaving) & node_mask(local_node)) {
		/* Complete the lockspace online / offline uevent. */
		printf_pathf("%d", "%s/%s/event_done", 0, DLM_SYSFS_DIR, ls->name);
	}
	ls->members = new_members;
	ls->stopping = 0;
	ls->joining = 0;
	ls->leaving = 0;
	lockspace_status(ls, "updated");
}

/*
 * Once a lockspace has stopped cluster wide, request to join or leave the
 * lockspace on per nodes as required, update the local lockspace
 * configuration, and restart the lockspace.
 */
static void
lockspace_stopped(struct lockspace *ls)
{
	lockspace_status(ls, "stopped");
	if (ls->joining & node_mask(local_node)) {
		struct node *node;

		for (node = nodes; node; node = node->next) {
			if (node == local_node)
				continue;
			send_msg(node, MSG_JOIN_LOCKSPACE, ls->name);
			ls->stopped &= ~node_mask(node);
		}
	}
	if (ls->leaving & node_mask(local_node)) {
		struct node *node;

		for (node = nodes; node; node = node->next) {
			if (node == local_node)
				continue;
			send_msg(node, MSG_LEAVE_LOCKSPACE, ls->name);
			ls->stopped &= ~node_mask(node);
		}
	}
	update_lockspace(ls);
}

/*
 * Completion of stop_lockspace().
 *
 * If other nodes are waiting for the lockspace on this node to stop, notify
 * them.  Once the lockspace has stopped cluster wide, update the logspace
 * membership cluster-wide and restart the logspace with:
 *
 *   echo 1 > /sys/kernel/dlm/<name>/control
 */
static void
complete_stop_lockspace(struct aio_request *aio_req)
{
	struct lockspace_aio_request *ls_aio_req =
		container_of(aio_req, struct lockspace_aio_request, aio_req);
	struct lockspace *ls = ls_aio_req->ls;
	struct node *node;

	for (node = nodes; node; node = node->next) {
		if (node == local_node)
			continue;
		if (ls->stopping & node_mask(node))
			send_msg(node, MSG_LOCKSPACE_STOPPED, ls->name);
	}
	ls->stopping &= ~node_mask(local_node);
	ls->stopped |= node_mask(local_node);
	if (!(~ls->stopped & connected_nodes))
		lockspace_stopped(ls);
	free(ls_aio_req);
}

/*
 * Request to stop a lockspace locally.
 *
 * Triggered by peer nodes.
 *
 * This can take a long time to complete, so use an asynchronous request.
 *
 * Essentially equivalent to:
 *
 *   echo 0 > /sys/kernel/dlm/<name>/control
 */
static void
stop_lockspace(struct lockspace *ls)
{
	struct lockspace_aio_request *ls_aio_req;
	struct aio_request *aio_req;

	ls->stopping |= node_mask(local_node);
	ls_aio_req = malloc(sizeof(*ls_aio_req));
	if (!ls_aio_req)
		fail(NULL);
	memset(ls_aio_req, 0, sizeof(*ls_aio_req));
	ls_aio_req->ls = ls;
	aio_req = &ls_aio_req->aio_req;
	aio_req->aiocb.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
	aio_req->aiocb.aio_sigevent.sigev_signo = SIGUSR1;
	aio_req->aiocb.aio_sigevent.sigev_value.sival_ptr = aio_req;
	aio_req->aiocb.aio_fildes = ls->control_fd;
	aio_req->aiocb.aio_nbytes = 1;
	aio_req->aiocb.aio_buf = "0";
	aio_req->complete = complete_stop_lockspace;
	list_add(&aio_req->list, &aio_pending);
	if (aio_write(&aio_req->aiocb) == 0)
		return;
	list_del(&aio_req->list);
	failf("%s/%s/control", DLM_SYSFS_DIR, ls->name);
}

/*
 * Request to add / join a lockspace.
 *
 * This event is triggered by DLM_USER_CREATE_LOCKSPACE requests by the user
 * (via libdlm).
 *
 * Stop the lockspace on all nodes in the cluster, join the lockspace, and
 * start the lockspace locally with:
 *
 *   echo 1 > /sys/kernel/dlm/<name>/control
 *
 * Complete the uevent with:
 *
 *   echo 0 > /sys/kernel/dlm/<name>/event_done
 */
static void
lockspace_online_uevent(const char *name)
{
	struct lockspace *ls;
	struct node *node;
	bool sent = false;

	ls = find_lockspace(name);
	if (!ls)
		ls = new_lockspace(name);
	if (connected_nodes != all_nodes) {
		/* Refuse to create lockspaces when not fully connected. */
		fprintf(stderr, "Not joining lockspace '%s': "
			"not connected to node(s) ", name);
		print_nodes(stderr, all_nodes & ~connected_nodes);
		fprintf(stderr, "\n");
		fflush(stderr);
		printf_pathf("%d", "%s/%s/event_done", EBUSY, DLM_SYSFS_DIR, ls->name);
		return;
	}
	if (ls->members & node_mask(local_node)) {
		fprintf(stderr, "Already in lockspace '%s'\n", name);
		fflush(stderr);
		printf_pathf("%d", "%s/%s/event_done", 0, DLM_SYSFS_DIR, ls->name);
		return;
	}
	printf("Joining lockspace '%s'\n", name);
	fflush(stdout);
	/* (Lockspace not started, yet.) */
	ls->joining |= node_mask(local_node);
	for (node = nodes; node; node = node->next) {
		if (node == local_node)
			continue;
		sent |= send_msg(node, MSG_STOP_LOCKSPACE, name);
	}
	if (!sent)
		update_lockspace(ls);
}

/*
 * The control device for a new lockspace has been created.  Remember the new
 * minor number assigned.
 */
static void
lockspace_add_device_uevent(const char *buf, int len)
{
	const char *token = buf, *end = buf + len;
	struct lockspace *ls;

	ls = find_lockspace(buf);
	if (!ls)
		return;
	while (token < end) {
		const char *t;

		t = memchr(token, 0, end - token);
		if (!t)
			break;
		if (strncmp(token, "MINOR=", 6) == 0)
			ls->minor = atoi(token + 6);
		token = t + 1;
	}
}

/*
 * Request to leave / remove a lockspace.
 *
 * This event is triggered by DLM_USER_REMOVE_LOCKSPACE requests, either by a
 * user or by FakeDLM itself when shutting down or when we lose cluster
 * connectivity (see release_lockspace()).  The lockspace is already stopped
 * locally.
 *
 * Stop the lockspace on all nodes in the cluster, leave the lockspace, and
 * then the lockspace locally be completing the uevent with:
 *
 *   echo 0 > /sys/kernel/dlm/<name>/event_done
 */
static void
lockspace_offline_uevent(const char *name)
{
	struct lockspace *ls;
	struct node *node;
	bool sent = false;

	ls = find_lockspace(name);
	if (!ls) {
		printf("Lockspace '%s' doesn't exist\n",
		       name);
		return;
	}
	if (!(ls->members & node_mask(local_node))) {
		printf("Not in lockspace '%s'\n", ls->name);
		fflush(stdout);
		return;
	}
	printf("Leaving lockspace '%s'\n", name);
	fflush(stdout);

	if (ls->control_fd != -1 && close(ls->control_fd) == -1)
		failf("%s/%s/control", DLM_SYSFS_DIR, ls->name);
	ls->control_fd = -1;
	ls->minor = -1;

	ls->leaving |= node_mask(local_node);
	ls->stopped |= node_mask(local_node);
	if (connected_nodes == all_nodes) {
		for (node = nodes; node; node = node->next) {
			if (node == local_node)
				continue;
			sent |= send_msg(node, MSG_STOP_LOCKSPACE, name);
		}
	}
	if (!sent)
		update_lockspace(ls);
}

/*
 * Repeatedly try opening a file until it gets created, with exponential
 * backoff and a timeout (in microseconds).
 */
static int
open_udev_device(const char *path, int flags, int timeout)
{
	int fd, step = 10000;

	fd = open(path, flags);
	while (fd == -1 && errno == ENOENT && timeout >= step) {
		usleep(step);
		timeout -= step;
		step *= 2;
		fd = open(path, flags);
	}
	return fd;
}

/*
 * The kernel expects the DLM control daemon (in this case FakeDLM) to keep
 * DLM_MONITOR_PATH open while it is running.  This allows to detect when the
 * control daemon dies unexpectedly.
 */
static void
monitor_kernel(void)
{
	kernel_monitor_fd = open_udev_device(DLM_MONITOR_PATH, O_RDONLY, 0);
	if (kernel_monitor_fd != -1)
		return;
	if (access(CONFIG_DLM, X_OK) == -1) {
		modprobe("dlm");
		if (access(CONFIG_DLM, X_OK) == -1)
			fail(CONFIG_DLM);
	}
	kernel_monitor_fd = open_udev_device(DLM_MONITOR_PATH, O_RDONLY, 5000000);
	if (kernel_monitor_fd == -1)
		fail(DLM_MONITOR_PATH);
}

static bool
node_is_local(const struct node *node)
{
	return has_local_addrs(node->addrs);
}

/*
 * Create a list of node objects along with all the network addresses
 * associated with each node.  Determine which of the nodes is local.
 * Assign unique node IDs starting from 1.
 */
static void
parse_nodes(char *node_names[], int count)
{
	struct node **last = &nodes;
	int n;

	local_node = NULL;
	for (n = 0; n < count; n++) {
		struct node *node;

		if (strcmp(node_names[n], "-") == 0)
			continue;
		node = new_node(node_names[n]);
		*last = node;
		last = &node->next;

		node->nodeid = n + 1;
		if (node_is_local(node)) {
			if (local_node) {
				fprintf(stderr, "Nodes %s and %s are both "
					"local", local_node->name, node->name);
				exit(2);
			}
			local_node = node;
		}
		all_nodes |= node_mask(node);
	}
	if (!local_node) {
		fprintf(stderr, "None of the specified nodes has a local "
			"network address\n");
		exit(2);
	}
	connected_nodes |= node_mask(local_node);
}

/*
 * A network connection should be closed because EOF was reached, an error
 * occurred, or because a MSG_CLOSE message was received.  If the primary
 * connection to a node is lost, the cluster has degenerated and we shut
 * all lockspaces down.
 */
static void
proto_close(int fd, struct node *node)
{
	close(fd);
	remove_poll_callback(&cbs, fd);
	if (node->outgoing_fd == fd) {
		struct lockspace *ls;

		node->outgoing_fd = -1;
		connected_nodes &= ~node_mask(node);
		for (ls = lockspaces; ls; ls = ls->next) {
			ls->joining = 0;
			ls->leaving = ls->members & ~node_mask(local_node);
			if (ls->leaving)
				update_lockspace(ls);
			if (ls->members & node_mask(local_node))
				release_lockspace(ls, true);
		}
	}
}

/*
 * A MSG_LOCKSPACE_STOPPED message has been received; the lockspace on the
 * sending node has been stopped (or was already stopped) and will remain
 * stopped until we send a MSG_JOIN_LOCKSPACE or MSG_LEAVE_LOCKSPACE message.
 */
static void
proto_lockspace_stopped(struct node *node, const char *name)
{
	struct lockspace *ls;

	ls = find_lockspace(name);
	if (!ls)
		return;
	ls->stopped |= node_mask(node);
	if (!(~ls->stopped & connected_nodes))
		lockspace_stopped(ls);
}

/*
 * A MSG_STOP_LOCKSPACE message has been received.
 */
static void
proto_stop_lockspace(struct node *node, const char *name)
{
	struct lockspace *ls;

	ls = find_lockspace(name);
	if (!ls)
		ls = new_lockspace(name);
	/*
	 * The lockspace will not be restarted until all bits in ls->stopping
	 * (one for each peer node) have been cleared again.
	 */
	ls->stopping |= node_mask(node);
	/*
	 * The ls->stopped bit for the local node indicates whether the
	 * lockspace is active or stopped locally; new lockspaces start out
	 * stopped.  The ls->stopping bit for the local node indicates whether
	 * we have already requested the kernel to stop the lockspace locally.
	 */
	if (ls->stopped & node_mask(local_node))
		send_msg(node, MSG_LOCKSPACE_STOPPED, ls->name);
	else if (!(ls->stopping & node_mask(local_node)))
		stop_lockspace(ls);
}

/*
 * A MSG_JOIN_LOCKSPACE message has been received.
 */
static void
proto_join_lockspace(struct node *node, const char *name)
{
	struct lockspace *ls;

	ls = find_lockspace(name);
	if (!ls)
		return;
	if (ls->members & node_mask(node)) {
		warn("MSG_LEAVE_LOCKSPACE: Node %u already is a member",
		     node->nodeid);
		return;
	}
	ls->joining |= node_mask(node);
	ls->stopping &= ~node_mask(node);
	if (!(ls->stopping & connected_nodes))
		update_lockspace(ls);
}

/*
 * A MSG_LEAVE_LOCKSPACE  message has been received.
 */
static void
proto_leave_lockspace(struct node *node, const char *name)
{
	struct lockspace *ls;

	ls = find_lockspace(name);
	if (!ls)
		return;
	if (!(ls->members & node_mask(node))) {
		warn("MSG_LEAVE_LOCKSPACE: Node %u is not a member",
		     node->nodeid);
		return;
	}
	ls->leaving |= node_mask(node);
	ls->stopping &= ~node_mask(node);
	if (!(ls->stopping & connected_nodes))
		update_lockspace(ls);
}

/*
 * The incoming or outgoing socket of a node can be read from.  We try to
 * connect to peer nodes asynchronously, so we can get ECONNREFUSED errors
 * here.
 */
static void
proto_read(int fd, short revents, void *arg)
{
	char buf[sizeof(struct proto_msg) + 1];
	struct proto_msg *msg = (void *)buf;
	struct node *node = arg;
	ssize_t ret;

	buf[sizeof(struct proto_msg)] = 0;
	for(;;) {
		ret = read(fd, buf, sizeof(struct proto_msg));
		if (ret == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == ECONNREFUSED)
				ret = 0;
		}
		if (ret == 0) {
			proto_close(fd, node);
			return;
		}
		if (ret != sizeof(struct proto_msg))
			fail(NULL);
		if (verbose) {
			printf("< %u %s", node->nodeid,
			       msg_name(ntohs(msg->msg)));
			if (msg->lockspace_name)
				printf(" %s", msg->lockspace_name);
			printf("\n");
			fflush(stdout);
		}
		switch(ntohs(msg->msg)) {
		case MSG_CLOSE:
			proto_close(fd, node);
			return;

		case MSG_LOCKSPACE_STOPPED:
			proto_lockspace_stopped(node, msg->lockspace_name);
			break;

		case MSG_STOP_LOCKSPACE:
			proto_stop_lockspace(node, msg->lockspace_name);
			break;

		case MSG_JOIN_LOCKSPACE:
			proto_join_lockspace(node, msg->lockspace_name);
			break;

		case MSG_LEAVE_LOCKSPACE:
			proto_leave_lockspace(node, msg->lockspace_name);
			break;

		default:
			failf("Unknown message %u received", ntohs(msg->msg));
		}
	}
}

/*
 * Find the node belonging to a given socket address (for incoming
 * connections).
 */
static struct node *
sockaddr_to_node(struct sockaddr *sa, socklen_t sa_len)
{
	struct node *node;

	for (node = nodes; node; node = node->next) {
		struct addr *addr;

		for (addr = node->addrs; addr; addr = addr->next) {
			if (addr_equal(sa, addr->sa))
				return node;
		}
	}
	return NULL;
}

/*
 * Add an incoming (accepted) or outgoing (connecting) socket.
 */
static void
add_connection(int fd, struct node *node)
{
	if (node->outgoing_fd == -1) {
		node->outgoing_fd = fd;
	} else if (local_node->nodeid < node->nodeid) {
		send_msg(node, MSG_CLOSE, NULL);
		node->outgoing_fd = fd;
	}
	connected_nodes |= node_mask(node);
}

/*
 * We have a new incoming connection.  Accept the connection and start polling
 * for incoming packets.
 */
static void
incoming_connection(int fd, short revents, void *arg)
{
	struct sockaddr sa;
	socklen_t sa_len;
	int client_fd;
	struct node *node;

	for(;;) {
		sa_len = sizeof(sa);
		client_fd = accept4(fd, &sa, &sa_len, SOCK_NONBLOCK);
		if (client_fd == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			fail(NULL);
		}

		node = sockaddr_to_node(&sa, sa_len);
		if (!node) {
			char hbuf[NI_MAXHOST];
			int g;

			g = getnameinfo(&sa, sa_len, hbuf, sizeof(hbuf), NULL, 0,
					NI_NUMERICHOST);
			if (g) {
				fprintf(stderr, "%s\n", gai_strerror(g));
				exit(1);
			}
			fprintf(stderr, "Could not determine node-id for node at %s\n",
				hbuf);
			close(client_fd);
		} else {
			if (node->connecting_fd != -1) {
				close(node->connecting_fd);
				remove_poll_callback(&cbs, node->connecting_fd);
				node->connecting_fd = -1;
			}
			add_poll_callback(&cbs, client_fd, POLLIN, proto_read, node);
			add_connection(client_fd, node);
		}
	}
}

/*
 * An outgoing connection has become ready to write to.  Start polling for
 * incoming packets.
 */
static void
outgoing_connection(int fd, short revents, void *arg)
{
	struct node *node = arg;

	assert(fd == node->connecting_fd);
	node->connecting_fd = -1;
	if (revents & POLLERR) {
		int socket_error;
		socklen_t len;

		remove_poll_callback(&cbs, fd);
		len = sizeof(socket_error);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) == -1)
			fail(NULL);
		close(fd);
		if (socket_error != ECONNREFUSED) {
			errno = socket_error;
			fail(NULL);
		}
	} else {
		update_poll_callback(&cbs, fd, POLLIN, proto_read, node);
		add_connection(fd, node);
	}
}

/*
 * Connect to the first address of each of our peers in non-blocking mode.
 */
static void
connect_to_peers(void)
{
	struct node *node;

	for (node = nodes; node; node = node->next) {
		struct sockaddr_storage ss;
		struct addr *addr = node->addrs;
		int fd;

		if (node == local_node)
			continue;
		memset(&ss, 0, sizeof(ss));
		memcpy(&ss, addr->sa, addr->sa_len);
		if (ss.ss_family == AF_INET) {
			struct sockaddr_in *sin =
				(struct sockaddr_in *)&ss;

			sin->sin_port = htons(fakedlm_port);
		} else if (ss.ss_family == AF_INET6) {
			struct sockaddr_in6 *sin6 =
				(struct sockaddr_in6 *)&ss;

			sin6->sin6_port = htons(fakedlm_port);
		}
		fd = socket(addr->family, addr->socktype | SOCK_NONBLOCK,
			    addr->protocol);
		if (fd == -1)
			fail(NULL);
		if (connect(fd, (struct sockaddr *)&ss, addr->sa_len) == -1) {
			if (errno != EINPROGRESS)
				fail(NULL);
			node->connecting_fd = fd;
			add_poll_callback(&cbs, fd, POLLOUT, outgoing_connection, node);
		} else {
			/* Connections shouldn't be established immediately ... */
			update_poll_callback(&cbs, fd, POLLIN, proto_read, node);
			add_connection(fd, node);
		}
	}
}

/*
 * Listen on IPv4 and/or IPv6 depending on how the local node is configured.
 */
static void
listen_to_peers(void)
{
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE | AI_ADDRCONFIG,
	};
	const int yes = 1;
	struct addrinfo *res, *addr;
	char *port_str;
	int g;

	if (asprintf(&port_str, "%u", fakedlm_port) == -1)
		fail(NULL);
	g = getaddrinfo(NULL, port_str, &hints, &res);
	if (g) {
		fprintf(stderr, "%s\n", gai_strerror(g));
		exit(1);
	}
	free(port_str);
	for (addr = res; addr; addr = addr->ai_next) {
		int fd;

		fd = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK,
			    addr->ai_protocol);
		if (fd == -1)
			fail(NULL);
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes,
			       sizeof(yes)) == -1)
			fail(NULL);
		if (addr->ai_family == AF_INET6) {
			/* Prevent IPv6 sockets from conflicting with IPv4. */
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
				       (void *)&yes, sizeof(yes)) == -1)
				fail(NULL);
		}
		if (bind(fd, addr->ai_addr, addr->ai_addrlen) == -1)
			fail(NULL);
		if (listen(fd, MAX_NODES - 1) == -1)
			fail(NULL);
		add_poll_callback(&cbs, fd, POLLIN, incoming_connection,
				  LISTENING_SOCKET_MARKER);
	}
	freeaddrinfo(res);
}

/*
 * Tell DLM about a new node's ID, addresses, and whether the node is local.
 */
static void
configure_node(struct node *node)
{
	struct addr *addr;

	mkdirf(0777, "%scomms/%d", CONFIG_DLM_CLUSTER, node->nodeid);
	printf_pathf("%d", "%scomms/%d/nodeid", node->nodeid, CONFIG_DLM_CLUSTER,
		     node->nodeid);
	if (node == local_node) {
		printf_pathf("1", "%scomms/%d/local", CONFIG_DLM_CLUSTER,
			     node->nodeid);
	}
	for (addr = node->addrs; addr; addr = addr->next) {
		struct sockaddr_storage ss;

		memcpy(&ss, addr->sa, addr->sa_len);
		memset((char *)&ss + addr->sa_len, 0, sizeof(ss) - addr->sa_len);
		write_pathf(&ss, sizeof(ss), "%scomms/%d/addr",
			    CONFIG_DLM_CLUSTER, node->nodeid);
	}
}

/*
 * Load and configure the DLM kernel module.  This does not start any
 * lockspaces, yet.
 */
static void
configure_dlm(void)
{
	struct node *node;

	if (mkdir(CONFIG_DLM_CLUSTER, 0777) == -1 && errno != EEXIST) {
		modprobe("dlm");
		if (mkdir(CONFIG_DLM_CLUSTER, 0777) == -1 && errno != EEXIST)
			fail(CONFIG_DLM_CLUSTER);
	}
	if (cluster_name)
		printf_pathf("%s", "%s", cluster_name,
			     CONFIG_DLM_CLUSTER "cluster_name");
	if (dlm_port != DLM_PORT) {
		printf_pathf("%d", "%s", DLM_PORT,
			     CONFIG_DLM_CLUSTER "tcp_port");
	}
	if (dlm_protocol != PROTO_TCP) {
		printf_pathf("%d", "%s", dlm_protocol,
			     CONFIG_DLM_CLUSTER "protocol");
	}
	for (node = nodes; node; node = node->next) {
		configure_node(node);
	}
}

/*
 * Remove the DLM configuration so that the kernel module can be removed and/or
 * a different configuration can be created.
 */
static void
remove_dlm(void)
{
	struct node *node;

	for (node = nodes; node; node = node->next)
		rmdirf("%scomms/%d", CONFIG_DLM_CLUSTER, node->nodeid);
	rmdirf("%s", CONFIG_DLM_CLUSTER);

	if (control_fd != -1)
		close(control_fd);
	close(kernel_monitor_fd);
	rmmod("dlm");
}

/*
 * Print a uevent and its parameters (mostly for debugging purposes).
 */
static void
print_uevent(const char *buf, int len)
{
	printf("Uevent '%s'", buf);
	if (verbose) {
		const char *end = buf + len;
		bool first = true;

		for (buf = strchr(buf, 0) + 1;
		     buf < end;
		     buf = strchr(buf, 0) + 1) {
			if (first) {
				printf(" (%s", buf);
				first = false;
			} else {
				printf(", %s", buf);
			}
		}
		if (!first)
			 printf(")");
	}
	printf("\n");
	fflush(stdout);
}

/*
 * Receive and dispatch a uevent.
 */
static void
recv_uevent(int uevent_fd, short revents, void *arg)
{
	char buf[MAX_LINE_UEVENT + 1];
	int len;

	len = recv(uevent_fd, &buf, MAX_LINE_UEVENT, 0);
	if (len < 0)
		fail(NULL);
	buf[len] = 0;
	print_uevent(buf, len);
	if (len >= 19 &&
	    strncmp(buf, "online@/kernel/dlm/", 19) == 0)
		lockspace_online_uevent(buf + 19);
	if (len >= 30 &&
	    strncmp(buf, "add@/devices/virtual/misc/dlm_", 30) == 0)
		lockspace_add_device_uevent(buf + 30, len - 30);
	if (len >= 20 &&
	    strncmp(buf, "offline@/kernel/dlm/", 20) == 0)
		lockspace_offline_uevent(buf + 20);
}

/*
 * Start listening to uevents.
 */
static void
listen_to_uvents(void)
{
	struct sockaddr_nl snl;
	int uevent_fd;
	uevent_fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (uevent_fd < 0)
		fail(NULL);
	memset(&snl, 0, sizeof(snl));
	snl.nl_family = AF_NETLINK;
	snl.nl_pid = getpid();
	snl.nl_groups = 1;
	if (bind(uevent_fd, (struct sockaddr *) &snl, sizeof(snl)) < 0)
		fail(NULL);
	add_poll_callback(&cbs, uevent_fd, POLLIN, recv_uevent, NULL);
}

/*
 * The main event loop.
 */
static void
event_loop(void)
{
	node_mask_t old_connected_nodes = 0;
	int old_shut_down = 0;

	while (!old_shut_down ||
	       joined_lockspaces ||
	       !list_empty(&aio_pending) ||
	       !list_empty(&aio_completed)) {
		int ret, n;

		if (connected_nodes != old_connected_nodes) {
			if (verbose) {
				print_nodes(stdout, connected_nodes);
				printf("\n");
				fflush(stdout);
			}
			if (connected_nodes == all_nodes) {
				printf("DLM ready\n");
				fflush(stdout);
			} else if (old_connected_nodes == all_nodes) {
				printf("DLM not ready\n");
				fflush(stdout);
			}
			old_connected_nodes = connected_nodes;
		}

		if (old_shut_down != shut_down) {
			switch(shut_down) {
			case 1:
				printf("Shutting down (press ^C to enforce)\n");
				break;
			case 2:
				printf("Shutting down\n");
				break;
			default:
				printf("Aborting\n");
				break;
			}
			fflush(stdout);
			close_all_connections();
			if (joined_lockspaces && shut_down <= 2)
				release_lockspaces(shut_down > 1);
			else
				break;
			old_shut_down = shut_down;
			continue;
		}

		if (!list_empty(&aio_completed)) {
			while (!list_empty(&aio_completed)) {
				struct aio_request *req =
					list_first_entry(&aio_completed,
							 struct aio_request,
							 list);
				int err;

				list_del(&req->list);
				err = aio_error(&req->aiocb);
				if (err > 0)
					errno = err;
				req->complete(req);
			}
			continue;
		}

		ret = poll(cbs.pollfds, cbs.num, -1);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			fail(NULL);
		}

		for (n = 0; n < cbs.num; n++) {
			struct pollfd *pfd = &cbs.pollfds[n];

			if (pfd->revents) {
				struct poll_callback *pcb = &cbs.callbacks[n];

				pcb->callback(pfd->fd, pfd->revents, pcb->arg);
			}
		}
	}
}

/*
 * SIGINT / SIGTERM signal handler.
 */
static void
handle_shutdown(int signo)
{
	shut_down++;
}

/*
 * SIGUSR1 handler for asynchronous I/O completion notifications.
 */
static void
handle_aio(int sig, siginfo_t *si, void *ucontext)
{
	if (si->si_code == SI_ASYNCIO) {
		struct aio_request *req = si->si_value.sival_ptr;
		int err;

		err = aio_error(&req->aiocb);
		if (err != EINPROGRESS) {
			list_del(&req->list);
			list_add_tail(&req->list, &aio_completed);
		}
	}
}

static void
setup_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);

	sa.sa_flags = SA_RESTART;
	sa.sa_handler = handle_shutdown;
	if (sigaction(SIGINT, &sa, NULL) == -1 ||
	    sigaction(SIGTERM, &sa, NULL) == -1)
		fail(NULL);

	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sa.sa_sigaction = handle_aio;
	if (sigaction(SIGUSR1, &sa, NULL) == -1)
		fail(NULL);
}

static void
usage(int status)
{
	fprintf(status ? stderr : stdout,
		"USAGE: %s [--verbose] [--cluster-name=name] "
		"[--fakedlm-port=port] [--dlm-port=port] node ...\n",
		progname);
	exit(status);
}

static struct option long_options[] = {
	{ "cluster-name", required_argument, NULL, 'n' },
	{ "fakedlm-port", required_argument, NULL, 'P' },
	{ "dlm-port", required_argument, NULL, 'p' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "sctp", no_argument, NULL, 2 },
	{ "debug", no_argument, NULL, 3 },
	{ }
};

int main(int argc, char *argv[])
{
	char *node_names[argc - 1];
	int opt, count = 0;

	progname = argv[0];
	while ((opt = getopt_long(argc, argv, "-n:P:p:v", long_options, NULL)) != -1) {
		switch(opt) {
		case 1:  /* node */
			node_names[count++] = optarg;
			break;

		case 2:  /* --sctp */
			dlm_protocol = PROTO_SCTP;
			break;

		case 3:  /* --debug */
			debug = true;
			break;

		case 'n':  /* --cluster-name */
			cluster_name = optarg;
			break;

		case 'P': /* --fakedlm-port */
			fakedlm_port = atol(optarg);
			break;

		case 'p':  /* --dlm-port */
			dlm_port = atol(optarg);
			break;

		case 'v':  /* --verbose */
			verbose = true;
			break;

		case '?':  /*  bad option */
			usage(2);
		}
	}
	if (count == 0)
		usage(0);

	parse_nodes(node_names, count);
	setup_signals();
	if (all_nodes & (all_nodes - 1)) {
		/* More than one bit set in all_nodes. */
		listen_to_peers();
		connect_to_peers();
	}
	monitor_kernel();
	listen_to_uvents();
	configure_dlm();
	event_loop();
	remove_dlm();
	return 0;
}
