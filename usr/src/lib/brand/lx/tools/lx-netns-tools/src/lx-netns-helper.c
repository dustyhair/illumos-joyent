/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2026 Jonathan Wagner.
 */

#define	_GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/capability.h>
#include <linux/if_addr.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define	LX_NETNS_MAX_LINKS	32
#define	LX_NETNS_REQUEST_SIZE	512
#define	LX_NETNS_REPLY_SIZE	8192
#define	LX_NETNS_DAD_POLLS	50

typedef struct lx_netns_link {
	char lnl_name[IFNAMSIZ];
	struct in_addr lnl_address;
	unsigned int lnl_prefix;
} lx_netns_link_t;

typedef struct lx_netns_request {
	struct nlmsghdr lnr_header;
	union {
		struct ifinfomsg link;
		struct ifaddrmsg address;
		struct rtmsg route;
	} lnr_message;
	char lnr_attributes[LX_NETNS_REQUEST_SIZE];
} lx_netns_request_t;

static uint32_t lx_netns_sequence;

static void
fail(const char *message)
{
	(void) fprintf(stderr, "lx-netns-helper: %s\n", message);
	exit(125);
}

static void
fail_errno(const char *message)
{
	(void) fprintf(stderr, "lx-netns-helper: %s: %s\n", message,
	    strerror(errno));
	exit(125);
}

static int
valid_link_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	size_t length = strlen(name);

	if (length == 0 || length >= IFNAMSIZ)
		return (0);
	for (; *p != '\0'; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
		    *p == '.')
			continue;
		return (0);
	}
	return (1);
}

static size_t
add_attribute(struct nlmsghdr *header, size_t capacity, uint16_t type,
    const void *value, size_t value_size)
{
	struct rtattr *attribute;
	size_t offset = NLMSG_ALIGN(header->nlmsg_len);
	size_t length = RTA_LENGTH(value_size);
	size_t end = offset + RTA_ALIGN(length);

	if (end > capacity)
		fail("internal netlink request overflow");
	attribute = (struct rtattr *)((char *)header + offset);
	attribute->rta_type = type;
	attribute->rta_len = length;
	(void) memcpy(RTA_DATA(attribute), value, value_size);
	header->nlmsg_len = end;
	return (end);
}

static int
send_request(int fd, struct nlmsghdr *header)
{
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	char reply[LX_NETNS_REPLY_SIZE];
	ssize_t received;

	header->nlmsg_seq = ++lx_netns_sequence;
	if (sendto(fd, header, header->nlmsg_len, 0,
	    (struct sockaddr *)&kernel, sizeof (kernel)) == -1)
		return (-1);
	for (;;) {
		struct nlmsghdr *message;
		int remaining;

		received = recv(fd, reply, sizeof (reply), 0);
		if (received == -1)
			return (-1);
		remaining = (int)received;
		for (message = (struct nlmsghdr *)reply;
		    NLMSG_OK(message, remaining);
		    message = NLMSG_NEXT(message, remaining)) {
			struct nlmsgerr *error;

			if (message->nlmsg_seq != header->nlmsg_seq)
				continue;
			if (message->nlmsg_type != NLMSG_ERROR)
				continue;
			if (message->nlmsg_len <
			    NLMSG_LENGTH(sizeof (*error))) {
				errno = EPROTO;
				return (-1);
			}
			error = NLMSG_DATA(message);
			if (error->error == 0)
				return (0);
			errno = -error->error;
			return (-1);
		}
	}
}

static int
claim_link(int fd, const char *name)
{
	lx_netns_request_t request;

	(void) memset(&request, 0, sizeof (request));
	request.lnr_header.nlmsg_len = NLMSG_LENGTH(sizeof (struct ifinfomsg));
	request.lnr_header.nlmsg_type = RTM_NEWLINK;
	request.lnr_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
	    NLM_F_EXCL | NLM_F_CREATE;
	request.lnr_message.link.ifi_family = AF_UNSPEC;
	(void) add_attribute(&request.lnr_header, sizeof (request),
	    IFLA_IFNAME, name, strlen(name) + 1);
	return (send_request(fd, &request.lnr_header));
}

static void
set_link_up(int fd, unsigned int index)
{
	lx_netns_request_t request;

	(void) memset(&request, 0, sizeof (request));
	request.lnr_header.nlmsg_len = NLMSG_LENGTH(sizeof (struct ifinfomsg));
	request.lnr_header.nlmsg_type = RTM_NEWLINK;
	request.lnr_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	request.lnr_message.link.ifi_family = AF_UNSPEC;
	request.lnr_message.link.ifi_index = index;
	request.lnr_message.link.ifi_flags = IFF_UP;
	request.lnr_message.link.ifi_change = IFF_UP;
	if (send_request(fd, &request.lnr_header) == -1)
		fail_errno("cannot bring claimed link up");
}

static void
add_address(int fd, unsigned int index, const lx_netns_link_t *link)
{
	lx_netns_request_t request;

	(void) memset(&request, 0, sizeof (request));
	request.lnr_header.nlmsg_len = NLMSG_LENGTH(sizeof (struct ifaddrmsg));
	request.lnr_header.nlmsg_type = RTM_NEWADDR;
	request.lnr_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
	    NLM_F_EXCL | NLM_F_CREATE;
	request.lnr_message.address.ifa_family = AF_INET;
	request.lnr_message.address.ifa_prefixlen = link->lnl_prefix;
	request.lnr_message.address.ifa_index = index;
	(void) add_attribute(&request.lnr_header, sizeof (request), IFA_LOCAL,
	    &link->lnl_address, sizeof (link->lnl_address));
	(void) add_attribute(&request.lnr_header, sizeof (request), IFA_ADDRESS,
	    &link->lnl_address, sizeof (link->lnl_address));
	if (send_request(fd, &request.lnr_header) == -1)
		fail_errno("cannot add the private IPv4 address");
}

static void
add_default_route(int fd, unsigned int index, struct in_addr gateway)
{
	lx_netns_request_t request;

	(void) memset(&request, 0, sizeof (request));
	request.lnr_header.nlmsg_len = NLMSG_LENGTH(sizeof (struct rtmsg));
	request.lnr_header.nlmsg_type = RTM_NEWROUTE;
	request.lnr_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
	    NLM_F_EXCL | NLM_F_CREATE;
	request.lnr_message.route.rtm_family = AF_INET;
	request.lnr_message.route.rtm_table = RT_TABLE_MAIN;
	request.lnr_message.route.rtm_protocol = RTPROT_BOOT;
	request.lnr_message.route.rtm_scope = RT_SCOPE_UNIVERSE;
	request.lnr_message.route.rtm_type = RTN_UNICAST;
	(void) add_attribute(&request.lnr_header, sizeof (request), RTA_GATEWAY,
	    &gateway, sizeof (gateway));
	(void) add_attribute(&request.lnr_header, sizeof (request), RTA_OIF,
	    &index, sizeof (index));
	if (send_request(fd, &request.lnr_header) == -1)
		fail_errno("cannot add the private default route");
}

static int
address_ready(int fd, unsigned int index)
{
	lx_netns_request_t request;
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	char reply[LX_NETNS_REPLY_SIZE];
	int found = 0;
	int tentative = 0;

	(void) memset(&request, 0, sizeof (request));
	request.lnr_header.nlmsg_len = NLMSG_LENGTH(sizeof (struct ifaddrmsg));
	request.lnr_header.nlmsg_type = RTM_GETADDR;
	request.lnr_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.lnr_header.nlmsg_seq = ++lx_netns_sequence;
	request.lnr_message.address.ifa_family = AF_INET;
	if (sendto(fd, &request, request.lnr_header.nlmsg_len, 0,
	    (struct sockaddr *)&kernel, sizeof (kernel)) == -1)
		return (-1);
	for (;;) {
		struct nlmsghdr *message;
		ssize_t received;
		int remaining;

		received = recv(fd, reply, sizeof (reply), 0);
		if (received == -1)
			return (-1);
		remaining = (int)received;
		for (message = (struct nlmsghdr *)reply;
		    NLMSG_OK(message, remaining);
		    message = NLMSG_NEXT(message, remaining)) {
			struct ifaddrmsg *address;

			if (message->nlmsg_seq != request.lnr_header.nlmsg_seq)
				continue;
			if (message->nlmsg_type == NLMSG_DONE) {
				if (!found) {
					errno = EADDRNOTAVAIL;
					return (-1);
				}
				return (tentative ? 0 : 1);
			}
			if (message->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *error = NLMSG_DATA(message);

				errno = error->error == 0 ? EPROTO :
				    -error->error;
				return (-1);
			}
			if (message->nlmsg_type != RTM_NEWADDR ||
			    message->nlmsg_len <
			    NLMSG_LENGTH(sizeof (*address)))
				continue;
			address = NLMSG_DATA(message);
			if (address->ifa_family != AF_INET ||
			    address->ifa_index != index)
				continue;
			found = 1;
			if ((address->ifa_flags & IFA_F_DADFAILED) != 0) {
				errno = EADDRINUSE;
				return (-1);
			}
			if ((address->ifa_flags & IFA_F_TENTATIVE) != 0)
				tentative = 1;
		}
	}
}

static void
wait_for_address(int fd, unsigned int index)
{
	struct timespec delay = { .tv_nsec = 100000000 };
	unsigned int poll;
	int ready;

	for (poll = 0; poll < LX_NETNS_DAD_POLLS; poll++) {
		ready = address_ready(fd, index);
		if (ready == 1)
			return;
		if (ready == -1)
			fail_errno("cannot read private IPv4 DAD state");
		(void) nanosleep(&delay, NULL);
	}
	errno = ETIMEDOUT;
	fail_errno("private IPv4 address stayed tentative");
}

static void
drop_setup_capabilities(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
		.pid = 0
	};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	const unsigned int capabilities[] = { CAP_NET_ADMIN, CAP_SETPCAP };
	size_t i;

	for (i = 0; i < sizeof (capabilities) / sizeof (capabilities[0]); i++) {
		if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER,
		    capabilities[i], 0, 0) == -1)
			fail_errno("cannot lower a private network setup "
			    "capability");
		if (prctl(PR_CAPBSET_DROP, capabilities[i], 0, 0, 0) == -1)
			fail_errno("cannot drop a private network setup "
			    "capability");
	}
	if (syscall(SYS_capget, &header, data) == -1)
		fail_errno("cannot read private network setup capabilities");
	for (i = 0; i < sizeof (capabilities) / sizeof (capabilities[0]); i++) {
		unsigned int word = capabilities[i] / 32;
		uint32_t bit = UINT32_C(1) << (capabilities[i] % 32);

		data[word].effective &= ~bit;
		data[word].permitted &= ~bit;
		data[word].inheritable &= ~bit;
	}
	if (syscall(SYS_capset, &header, data) == -1)
		fail_errno("cannot clear private network setup capabilities");
	if (syscall(SYS_capget, &header, data) == -1)
		fail_errno("cannot verify private network setup capabilities");
	for (i = 0; i < sizeof (capabilities) / sizeof (capabilities[0]); i++) {
		unsigned int word = capabilities[i] / 32;
		uint32_t bit = UINT32_C(1) << (capabilities[i] % 32);

		if (((data[word].effective | data[word].permitted |
		    data[word].inheritable) & bit) != 0 ||
		    prctl(PR_CAPBSET_READ, capabilities[i], 0, 0, 0) != 0 ||
		    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET,
		    capabilities[i], 0, 0) != 0) {
			errno = EPERM;
			fail_errno("private network setup capability survived");
		}
	}
}

static size_t
parse_links(const char *value, lx_netns_link_t *links)
{
	char *copy, *last, *token;
	size_t count = 0;

	if ((copy = strdup(value)) == NULL)
		fail_errno("cannot copy the link configuration");
	for (token = strtok_r(copy, ",", &last); token != NULL;
	    token = strtok_r(NULL, ",", &last)) {
		char *equals, *slash, *end;
		long prefix;

		if (count == LX_NETNS_MAX_LINKS)
			fail("too many private network links");
		if ((equals = strchr(token, '=')) == NULL ||
		    (slash = strchr(equals + 1, '/')) == NULL) {
			fail("link entries must use name=address/prefix");
		}
		*equals = '\0';
		*slash = '\0';
		if (!valid_link_name(token))
			fail("invalid private network link name");
		(void) strcpy(links[count].lnl_name, token);
		if (inet_pton(AF_INET, equals + 1,
		    &links[count].lnl_address) != 1)
			fail("invalid private IPv4 address");
		errno = 0;
		prefix = strtol(slash + 1, &end, 10);
		if (errno != 0 || *end != '\0' || prefix < 1 || prefix > 32)
			fail("invalid private IPv4 prefix length");
		links[count].lnl_prefix = (unsigned int)prefix;
		count++;
	}
	free(copy);
	if (count == 0)
		fail("private network link list is empty");
	return (count);
}

int
main(int argc, char **argv)
{
	lx_netns_link_t links[LX_NETNS_MAX_LINKS];
	struct sockaddr_nl local = { .nl_family = AF_NETLINK };
	struct timeval timeout = { .tv_sec = 5 };
	struct in_addr gateway;
	unsigned int index;
	size_t count, selected;
	char *command;
	int fd;

	if (argc < 9 || strcmp(argv[1], "--payload-argv0") != 0 ||
	    strcmp(argv[3], "--links") != 0 ||
	    strcmp(argv[5], "--gateway") != 0 ||
	    strcmp(argv[7], "--") != 0 || argv[2][0] == '\0') {
		fail("usage: --payload-argv0 value --links "
		    "name=address/prefix,... --gateway address -- command");
	}
	count = parse_links(argv[4], links);
	if (inet_pton(AF_INET, argv[6], &gateway) != 1)
		fail("invalid private IPv4 gateway");
	if ((fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
	    NETLINK_ROUTE)) == -1)
		fail_errno("cannot open the route netlink socket");
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
	    sizeof (timeout)) == -1)
		fail_errno("cannot set the route netlink timeout");
	if (bind(fd, (struct sockaddr *)&local, sizeof (local)) == -1)
		fail_errno("cannot bind the route netlink socket");

	for (selected = 0; selected < count; selected++) {
		if (claim_link(fd, links[selected].lnl_name) == 0)
			break;
		if (errno != ENODEV && errno != ENOENT && errno != EBUSY &&
		    errno != EEXIST)
			fail_errno("cannot claim a private network link");
	}
	if (selected == count)
		fail("no private network link is available");
	if ((index = if_nametoindex(links[selected].lnl_name)) == 0)
		fail_errno("cannot resolve the claimed link index");
	set_link_up(fd, index);
	add_address(fd, index, &links[selected]);
	wait_for_address(fd, index);
	add_default_route(fd, index, gateway);
	(void) fprintf(stderr, "lx-netns-helper: ready link=%s address=%s/%u "
	    "gateway=%s\n", links[selected].lnl_name,
	    inet_ntoa(links[selected].lnl_address),
	    links[selected].lnl_prefix, argv[6]);
	(void) close(fd);
	drop_setup_capabilities();
	command = argv[8];
	argv[8] = argv[2];
	execvp(command, &argv[8]);
	fail_errno("cannot execute the sandbox command");
}
