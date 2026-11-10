/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Jonathan Wagner.
 */

#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	EVENT_LIMIT	32
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#define	BITS_PER_LONG	(sizeof (unsigned long) * 8)
#define	NBITS(n)	(((n) + BITS_PER_LONG) / BITS_PER_LONG)

static int uinput_fd = -1;
static bool device_created;

static void
cleanup(void)
{
	if (device_created)
		(void) ioctl(uinput_fd, UI_DEV_DESTROY);
	if (uinput_fd >= 0)
		(void) close(uinput_fd);
}

static void
fail(const char *what)
{
	int error = errno;

	(void) fprintf(stderr, "FAIL: %s: %s\n", what, strerror(error));
	exit(EXIT_FAILURE);
}

static void
check(bool condition, const char *what)
{
	if (!condition) {
		errno = EPROTO;
		fail(what);
	}
}

static bool
bit_is_set(const unsigned long *bits, unsigned int bit)
{
	return ((bits[bit / BITS_PER_LONG] &
	    (1UL << (bit % BITS_PER_LONG))) != 0);
}

static int
open_uevent_socket(void)
{
	struct sockaddr_nl address;
	int fd;

	fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC,
	    NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		fail("open kobject uevent socket");
	(void) memset(&address, 0, sizeof (address));
	address.nl_family = AF_NETLINK;
	address.nl_pid = (uint32_t)getpid();
	address.nl_groups = 1;
	if (bind(fd, (struct sockaddr *)&address, sizeof (address)) != 0)
		fail("bind kobject uevent socket");
	return (fd);
}

static bool
uevent_matches(const char *buffer, size_t size, const char *action,
    const char *field, const char *value)
{
	const char *position = buffer;
	bool found_action = false;
	bool found_value = false;
	size_t field_length = strlen(field);

	while (position < buffer + size && *position != '\0') {
		size_t length = strnlen(position, buffer + size - position);

		if (length == (size_t)(buffer + size - position))
			break;
		if (strncmp(position, "ACTION=", 7) == 0 &&
		    strcmp(position + 7, action) == 0)
			found_action = true;
		if (strncmp(position, field, field_length) == 0 &&
		    position[field_length] == '=' &&
		    strcmp(position + field_length + 1, value) == 0)
			found_value = true;
		position += length + 1;
	}
	return (found_action && found_value);
}

static void
wait_uevent_field(int fd, const char *action, const char *field,
    const char *value)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 20; attempt++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		char buffer[2048];
		ssize_t size;
		int result;

		do {
			result = poll(&pfd, 1, 100);
		} while (result < 0 && errno == EINTR);
		if (result < 0)
			fail("poll kobject uevent socket");
		if (result == 0)
			continue;
		do {
			size = recv(fd, buffer, sizeof (buffer), 0);
		} while (size < 0 && errno == EINTR);
		if (size < 0)
			fail("receive kobject uevent");
		if (uevent_matches(buffer, (size_t)size, action, field, value))
			return;
	}
	errno = ETIMEDOUT;
	fail("matching kobject uevent");
}

static void
wait_uevent(int fd, const char *action, const char *devname)
{
	wait_uevent_field(fd, action, "DEVNAME", devname);
}

static int
find_event_device(const char *wanted_name, char *path, size_t path_size)
{
	unsigned int i;

	for (i = 0; i < EVENT_LIMIT; i++) {
		char candidate[64];
		char name[UINPUT_MAX_NAME_SIZE];
		int fd;

		(void) snprintf(candidate, sizeof (candidate),
		    "/dev/input/event%u", i);
		fd = open(candidate, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		(void) memset(name, 0, sizeof (name));
		if (ioctl(fd, EVIOCGNAME(sizeof (name)), name) >= 0 &&
		    strcmp(name, wanted_name) == 0) {
			(void) snprintf(path, path_size, "%s", candidate);
			return (fd);
		}
		(void) close(fd);
	}

	errno = ENODEV;
	return (-1);
}

static void
read_text(const char *path, char *buffer, size_t size)
{
	ssize_t got;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail(path);
	do {
		got = read(fd, buffer, size - 1);
	} while (got < 0 && errno == EINTR);
	if (got < 0)
		fail(path);
	buffer[got] = '\0';
	(void) close(fd);
}

static void
trigger_uevent(const char *path, const char *action)
{
	ssize_t size = (ssize_t)strlen(action);
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		fail(path);
	do {
		written = write(fd, action, (size_t)size);
	} while (written < 0 && errno == EINTR);
	if (written != size)
		fail(path);
	(void) close(fd);
}

static void
check_uinput_sysfs(int uevent_fd)
{
	char target[256];
	char text[256];
	ssize_t size;

	size = readlink("/sys/class/misc/uinput", target,
	    sizeof (target) - 1);
	if (size < 0)
		fail("read uinput class link");
	target[size] = '\0';
	check(strcmp(target, "../../devices/virtual/misc/uinput") == 0,
	    "uinput class link has an unexpected target");
	size = readlink("/sys/class/misc/uinput/subsystem", target,
	    sizeof (target) - 1);
	if (size < 0)
		fail("read uinput subsystem link");
	target[size] = '\0';
	check(strcmp(target, "../../../../class/misc") == 0,
	    "uinput subsystem link has an unexpected target");
	read_text("/sys/class/misc/uinput/dev", text, sizeof (text));
	check(strcmp(text, "10:223\n") == 0,
	    "uinput sysfs device number mismatch");
	read_text("/sys/class/misc/uinput/uevent", text, sizeof (text));
	check(strstr(text, "DEVNAME=uinput\n") != NULL,
	    "uinput sysfs uevent data is incomplete");
	size = readlink("/sys/dev/char/10:223", target,
	    sizeof (target) - 1);
	if (size < 0)
		fail("read uinput dev char link");
	target[size] = '\0';
	check(strcmp(target, "../../devices/virtual/misc/uinput") == 0,
	    "uinput dev char link has an unexpected target");

	trigger_uevent("/sys/class/misc/uinput/uevent", "change\n");
	wait_uevent(uevent_fd, "change", "uinput");
}

static void
check_sysfs(const char *sysname, const char *event_path,
    const char *wanted_name, int uevent_fd, unsigned int event_minor)
{
	const char *event_name = strrchr(event_path, '/');
	char path[128];
	char text[256];
	char target[256];
	ssize_t size;
	int fd;

	check(event_name != NULL, "event path has no basename");
	event_name++;
	(void) snprintf(path, sizeof (path), "/sys/class/input/%s", sysname);
	size = readlink(path, target, sizeof (target) - 1);
	if (size < 0)
		fail("read input class link");
	target[size] = '\0';
	check(strstr(target, "/devices/virtual/input/") != NULL,
	    "input class link has an unexpected target");
	(void) snprintf(path, sizeof (path), "/sys/class/input/%s/name",
	    sysname);
	read_text(path, text, sizeof (text));
	check(strncmp(text, wanted_name, strlen(wanted_name)) == 0,
	    "sysfs input name mismatch");
	(void) snprintf(path, sizeof (path), "/sys/class/input/%s/dev",
	    event_name);
	read_text(path, text, sizeof (text));
	check(strncmp(text, "13:", 3) == 0,
	    "sysfs event device number mismatch");
	(void) snprintf(path, sizeof (path),
	    "/sys/class/input/%s/capabilities/key", sysname);
	read_text(path, text, sizeof (text));
	check(text[0] != '\0' && strcmp(text, "0\n") != 0,
	    "sysfs key capabilities are empty");

	(void) snprintf(path, sizeof (path), "/sys/dev/char/13:%u",
	    event_minor);
	size = readlink(path, target, sizeof (target) - 1);
	if (size < 0)
		fail("read event dev char link");
	target[size] = '\0';
	check(strstr(target, "/devices/virtual/input/") != NULL,
	    "event dev char link has an unexpected target");
	(void) snprintf(path, sizeof (path),
	    "/sys/class/input/%s/subsystem", event_name);
	size = readlink(path, target, sizeof (target) - 1);
	if (size < 0)
		fail("read event subsystem link");
	target[size] = '\0';
	check(strstr(target, "/class/input") != NULL,
	    "event subsystem link has an unexpected target");

	(void) snprintf(path, sizeof (path),
	    "/sys/class/input/%s/uevent", event_name);
	trigger_uevent(path, "change\n");
	wait_uevent(uevent_fd, "change", event_path + strlen("/dev/"));

	(void) snprintf(path, sizeof (path),
	    "/sys/class/input/%s/uevent", sysname);
	trigger_uevent(path, "change\n");
	(void) snprintf(text, sizeof (text),
	    "/devices/virtual/input/%s", sysname);
	wait_uevent_field(uevent_fd, "change", "DEVPATH", text);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		fail("open input uevent for invalid action");
	errno = 0;
	check(write(fd, "invalid\n", 8) < 0 && errno == EINVAL,
	    "invalid uevent action was accepted");
	(void) close(fd);
}

static void
write_key(int value)
{
	struct input_event events[] = {
		{ .type = EV_KEY, .code = KEY_A, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
	};
	ssize_t size = sizeof (events);

	if (write(uinput_fd, events, sizeof (events)) != size)
		fail("write input events");
}

static void
wait_readable(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int result;

	do {
		result = poll(&pfd, 1, 1000);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		fail("poll event device");
	check(result == 1 && (pfd.revents & POLLIN) != 0,
	    "event device did not become readable");
}

static void
read_key(int fd, int wanted_value)
{
	struct input_event events[2];
	ssize_t size = sizeof (events);
	ssize_t got;

	wait_readable(fd);
	do {
		got = read(fd, events, sizeof (events));
	} while (got < 0 && errno == EINTR);
	if (got < 0)
		fail("read input events");
	check(got == size, "unexpected input event byte count");
	check(events[0].type == EV_KEY && events[0].code == KEY_A &&
	    events[0].value == wanted_value, "unexpected key event");
	check(events[1].type == EV_SYN && events[1].code == SYN_REPORT,
	    "missing SYN_REPORT");
}

int
main(void)
{
	static const char device_name[] = "lx-uinput-smoke";
	struct uinput_setup setup;
	struct input_id id;
	struct stat st;
	unsigned long evbits[NBITS(EV_MAX)];
	unsigned long keybits[NBITS(KEY_MAX)];
	struct pollfd pfd;
	char event_path[64];
	char name[UINPUT_MAX_NAME_SIZE];
	char sysname[64];
	char uevent_devname[64];
	int version;
	int queued;
	int event_fd;
	int second_fd;
	int uevent_fd;
	int clock_id = CLOCK_MONOTONIC;
	unsigned long sys_slot;
	char *sys_end;

	if (atexit(cleanup) != 0)
		fail("atexit");
	uevent_fd = open_uevent_socket();
	if (stat("/dev/uinput", &st) != 0)
		fail("stat /dev/uinput");
	check(S_ISCHR(st.st_mode), "/dev/uinput is not a character device");
	check(major(st.st_rdev) == 10 && minor(st.st_rdev) == 223,
	    "/dev/uinput has the wrong Linux device number");
	check_uinput_sysfs(uevent_fd);

	uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uinput_fd < 0)
		fail("open /dev/uinput");
	if (ioctl(uinput_fd, UI_GET_VERSION, &version) != 0)
		fail("UI_GET_VERSION");
	check(version == 5, "unexpected uinput ABI version");
	if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) != 0)
		fail("UI_SET_EVBIT EV_KEY");
	if (ioctl(uinput_fd, UI_SET_KEYBIT, KEY_A) != 0)
		fail("UI_SET_KEYBIT KEY_A");

	(void) memset(&setup, 0, sizeof (setup));
	(void) snprintf(setup.name, sizeof (setup.name), "%s", device_name);
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1;
	setup.id.product = 0x2;
	setup.id.version = 0x3;
	if (ioctl(uinput_fd, UI_DEV_SETUP, &setup) != 0)
		fail("UI_DEV_SETUP");
	if (ioctl(uinput_fd, UI_DEV_CREATE) != 0)
		fail("UI_DEV_CREATE");
	device_created = true;
	(void) memset(sysname, 0, sizeof (sysname));
	if (ioctl(uinput_fd, UI_GET_SYSNAME(sizeof (sysname)), sysname) < 0)
		fail("UI_GET_SYSNAME");
	check(strncmp(sysname, "input", 5) == 0,
	    "unexpected uinput sysname");
	errno = 0;
	sys_slot = strtoul(sysname + 5, &sys_end, 10);
	check(errno == 0 && *sys_end == '\0' && sys_slot < EVENT_LIMIT,
	    "invalid uinput sysname slot");
	(void) snprintf(uevent_devname, sizeof (uevent_devname),
	    "input/event%lu", sys_slot);
	wait_uevent(uevent_fd, "add", uevent_devname);

	event_fd = find_event_device(device_name, event_path,
	    sizeof (event_path));
	if (event_fd < 0)
		fail("find matching event device");
	second_fd = open(event_path, O_RDONLY | O_NONBLOCK);
	if (second_fd < 0)
		fail("open second event client");
	if (fstat(event_fd, &st) != 0)
		fail("fstat event device");
	check(major(st.st_rdev) == 13 && minor(st.st_rdev) >= 64 &&
	    minor(st.st_rdev) < 64 + EVENT_LIMIT,
	    "event device has the wrong Linux device number");
	check_sysfs(sysname, event_path, device_name, uevent_fd,
	    (unsigned int)minor(st.st_rdev));

	if (ioctl(event_fd, EVIOCGVERSION, &version) != 0)
		fail("EVIOCGVERSION");
	check(version == EV_VERSION, "unexpected evdev ABI version");
	if (ioctl(event_fd, EVIOCGID, &id) != 0)
		fail("EVIOCGID");
	check(memcmp(&id, &setup.id, sizeof (id)) == 0,
	    "evdev identity does not match uinput setup");
	(void) memset(name, 0, sizeof (name));
	if (ioctl(event_fd, EVIOCGNAME(sizeof (name)), name) < 0)
		fail("EVIOCGNAME");
	check(strcmp(name, device_name) == 0, "evdev name mismatch");
	(void) memset(evbits, 0, sizeof (evbits));
	if (ioctl(event_fd, EVIOCGBIT(0, sizeof (evbits)), evbits) < 0)
		fail("EVIOCGBIT types");
	check(bit_is_set(evbits, EV_SYN), "EV_SYN capability missing");
	check(bit_is_set(evbits, EV_KEY), "EV_KEY capability missing");
	(void) memset(keybits, 0, sizeof (keybits));
	if (ioctl(event_fd, EVIOCGBIT(EV_KEY, sizeof (keybits)), keybits) < 0)
		fail("EVIOCGBIT keys");
	check(bit_is_set(keybits, KEY_A), "KEY_A capability missing");
	if (ioctl(event_fd, EVIOCSCLOCKID, &clock_id) != 0)
		fail("EVIOCSCLOCKID");

	if (ioctl(event_fd, FIONREAD, &queued) != 0)
		fail("FIONREAD empty queue");
	check(queued == 0, "new event queue is not empty");
	write_key(1);
	if (ioctl(event_fd, FIONREAD, &queued) != 0)
		fail("FIONREAD queued events");
	check(queued == (int)(2 * sizeof (struct input_event)),
	    "FIONREAD returned the wrong byte count");
	read_key(event_fd, 1);
	read_key(second_fd, 1);

	if (ioctl(event_fd, EVIOCGRAB, 1) != 0)
		fail("EVIOCGRAB");
	write_key(0);
	read_key(event_fd, 0);
	pfd.fd = second_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, 50) < 0)
		fail("poll grabbed event device");
	check((pfd.revents & POLLIN) == 0,
	    "grabbed events leaked to another client");
	if (ioctl(event_fd, EVIOCGRAB, 0) != 0)
		fail("release EVIOCGRAB");

	if (ioctl(uinput_fd, UI_DEV_DESTROY) != 0)
		fail("UI_DEV_DESTROY");
	device_created = false;
	wait_uevent(uevent_fd, "remove", uevent_devname);
	pfd.fd = event_fd;
	pfd.events = 0;
	pfd.revents = 0;
	if (poll(&pfd, 1, 1000) < 0)
		fail("poll destroyed event device");
	check((pfd.revents & (POLLHUP | POLLERR)) != 0,
	    "destroyed event device did not signal hangup");
	errno = 0;
	check(read(event_fd, &id, sizeof (id)) < 0 && errno == ENODEV,
	    "destroyed event device remained readable");

	(void) close(second_fd);
	(void) close(event_fd);
	(void) close(uevent_fd);
	(void) printf("PASS: %s (%s, %s)\n", device_name, sysname,
	    event_path);
	return (EXIT_SUCCESS);
}
