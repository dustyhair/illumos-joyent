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

#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/poll.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EVENT_LIMIT	64
#define CLOCK_SLOP_USEC	(1000LL * 1000LL)

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME	7
#endif

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

static int64_t
timespec_usec(const struct timespec *time)
{
	return ((int64_t)time->tv_sec * 1000 * 1000 +
	    time->tv_nsec / 1000);
}

static int64_t
event_usec(const struct input_event *event)
{
	return ((int64_t)event->time.tv_sec * 1000 * 1000 +
	    event->time.tv_usec);
}

static void
check_event_time(const struct input_event *event,
    const struct timespec *before, const struct timespec *after,
    const char *what)
{
	int64_t stamp = event_usec(event);
	int64_t lower = timespec_usec(before) - CLOCK_SLOP_USEC;
	int64_t upper = timespec_usec(after) + CLOCK_SLOP_USEC;

	check(stamp >= lower && stamp <= upper, what);
}

static int
find_event_device(const char *wanted_name)
{
	struct timespec delay = { .tv_nsec = 20 * 1000 * 1000 };
	unsigned int attempt;

	for (attempt = 0; attempt < 100; attempt++) {
		unsigned int slot;

		for (slot = 0; slot < EVENT_LIMIT; slot++) {
			char path[64];
			char name[UINPUT_MAX_NAME_SIZE];
			int fd;

			(void) snprintf(path, sizeof (path),
			    "/dev/input/event%u", slot);
			fd = open(path, O_RDONLY | O_NONBLOCK);
			if (fd < 0)
				continue;
			(void) memset(name, 0, sizeof (name));
			if (ioctl(fd, EVIOCGNAME(sizeof (name)), name) >= 0 &&
			    strcmp(name, wanted_name) == 0)
				return (fd);
			(void) close(fd);
		}
		(void) nanosleep(&delay, NULL);
	}

	errno = ENODEV;
	return (-1);
}

static void
write_key(int value)
{
	struct input_event events[] = {
		{ .type = EV_KEY, .code = KEY_A, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
	};

	if (write(uinput_fd, events, sizeof (events)) != sizeof (events))
		fail("write input events");
}

static void
read_events(int fd, struct input_event *events, size_t count)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	size_t size = count * sizeof (*events);
	ssize_t got;
	int result;

	do {
		result = poll(&pfd, 1, 1000);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		fail("poll event device");
	check(result == 1 && (pfd.revents & POLLIN) != 0,
	    "event device did not become readable");
	do {
		got = read(fd, events, size);
	} while (got < 0 && errno == EINTR);
	if (got < 0)
		fail("read input events");
	check(got == (ssize_t)size, "unexpected input event count");
}

static void
check_packet(const struct input_event events[2], int value,
    const struct timespec *before, const struct timespec *after,
    const char *clock_name)
{
	char message[128];

	check(events[0].type == EV_KEY && events[0].code == KEY_A &&
	    events[0].value == value, "unexpected key event");
	check(events[1].type == EV_SYN && events[1].code == SYN_REPORT,
	    "missing SYN_REPORT");
	(void) snprintf(message, sizeof (message),
	    "%s key timestamp is outside its clock domain", clock_name);
	check_event_time(&events[0], before, after, message);
	(void) snprintf(message, sizeof (message),
	    "%s report timestamp is outside its clock domain", clock_name);
	check_event_time(&events[1], before, after, message);
}

static void
check_two_clocks(int realtime_fd, int other_fd, int other_clock,
    int value, const char *other_name)
{
	struct input_event realtime_events[2];
	struct input_event other_events[2];
	struct timespec realtime_before, realtime_after;
	struct timespec other_before, other_after;
	int realtime_clock = CLOCK_REALTIME;

	if (ioctl(realtime_fd, EVIOCSCLOCKID, &realtime_clock) != 0)
		fail("select realtime clock");
	if (ioctl(other_fd, EVIOCSCLOCKID, &other_clock) != 0)
		fail("select alternate clock");
	if (clock_gettime(CLOCK_REALTIME, &realtime_before) != 0 ||
	    clock_gettime(other_clock, &other_before) != 0)
		fail("sample clocks before event");
	write_key(value);
	if (clock_gettime(CLOCK_REALTIME, &realtime_after) != 0 ||
	    clock_gettime(other_clock, &other_after) != 0)
		fail("sample clocks after event");
	read_events(realtime_fd, realtime_events, 2);
	read_events(other_fd, other_events, 2);
	check_packet(realtime_events, value, &realtime_before,
	    &realtime_after, "realtime");
	check_packet(other_events, value, &other_before, &other_after,
	    other_name);
}

static void
check_invalid_clocks(int realtime_fd, int event_fd)
{
	struct input_event realtime_events[2];
	struct input_event events[2];
	struct timespec before, after;
	int invalid_clocks[] = { CLOCK_PROCESS_CPUTIME_ID, -1, 12345 };
	int monotonic_clock = CLOCK_MONOTONIC;
	size_t i;

	if (ioctl(event_fd, EVIOCSCLOCKID, &monotonic_clock) != 0)
		fail("select monotonic clock before invalid requests");
	for (i = 0; i < sizeof (invalid_clocks) / sizeof (invalid_clocks[0]);
	    i++) {
		errno = 0;
		check(ioctl(event_fd, EVIOCSCLOCKID, &invalid_clocks[i]) < 0 &&
		    errno == EINVAL, "EVIOCSCLOCKID accepted an invalid clock");
	}
	if (clock_gettime(CLOCK_MONOTONIC, &before) != 0)
		fail("sample monotonic clock before invalid-ID event");
	write_key(0);
	if (clock_gettime(CLOCK_MONOTONIC, &after) != 0)
		fail("sample monotonic clock after invalid-ID event");
	read_events(realtime_fd, realtime_events, 2);
	read_events(event_fd, events, 2);
	check_packet(events, 0, &before, &after, "preserved monotonic");
}

static void
check_clock_change_flush(int realtime_fd, int event_fd)
{
	struct input_event realtime_events[2];
	struct input_event events[3];
	struct timespec before, after;
	int monotonic_clock = CLOCK_MONOTONIC;
	int realtime_clock = CLOCK_REALTIME;
	size_t i;

	if (ioctl(event_fd, EVIOCSCLOCKID, &monotonic_clock) != 0)
		fail("select monotonic clock before queued event");
	write_key(1);
	read_events(realtime_fd, realtime_events, 2);
	if (clock_gettime(CLOCK_REALTIME, &before) != 0)
		fail("sample realtime clock before queue flush");
	if (ioctl(event_fd, EVIOCSCLOCKID, &realtime_clock) != 0)
		fail("change clock with queued events");
	write_key(0);
	if (clock_gettime(CLOCK_REALTIME, &after) != 0)
		fail("sample realtime clock after queue flush");
	read_events(realtime_fd, realtime_events, 2);
	read_events(event_fd, events, 3);
	check(events[0].type == EV_SYN && events[0].code == SYN_DROPPED,
	    "clock change did not replace queued events with SYN_DROPPED");
	check(events[1].type == EV_KEY && events[1].code == KEY_A &&
	    events[1].value == 0, "event before clock change was not dropped");
	check(events[2].type == EV_SYN && events[2].code == SYN_REPORT,
	    "clock-change packet is missing SYN_REPORT");
	for (i = 0; i < sizeof (events) / sizeof (events[0]); i++) {
		check_event_time(&events[i], &before, &after,
		    "clock-change event is outside the new clock domain");
	}
}

int
main(void)
{
	static const char device_name[] = "lx-input-clock";
	struct uinput_setup setup;
	int realtime_fd;
	int other_fd;

	if (atexit(cleanup) != 0)
		fail("atexit");
	uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uinput_fd < 0)
		fail("open /dev/uinput");
	if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) != 0)
		fail("UI_SET_EVBIT EV_KEY");
	if (ioctl(uinput_fd, UI_SET_KEYBIT, KEY_A) != 0)
		fail("UI_SET_KEYBIT KEY_A");

	(void) memset(&setup, 0, sizeof (setup));
	(void) snprintf(setup.name, sizeof (setup.name), "%s", device_name);
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1;
	setup.id.product = 0x53;
	setup.id.version = 0x1;
	if (ioctl(uinput_fd, UI_DEV_SETUP, &setup) != 0)
		fail("UI_DEV_SETUP");
	if (ioctl(uinput_fd, UI_DEV_CREATE) != 0)
		fail("UI_DEV_CREATE");
	device_created = true;

	realtime_fd = find_event_device(device_name);
	if (realtime_fd < 0)
		fail("find matching event device");
	other_fd = find_event_device(device_name);
	if (other_fd < 0)
		fail("open second event client");

	check_two_clocks(realtime_fd, other_fd, CLOCK_MONOTONIC, 1,
	    "monotonic");
	check_two_clocks(realtime_fd, other_fd, CLOCK_BOOTTIME, 0,
	    "boottime");
	check_invalid_clocks(realtime_fd, other_fd);
	check_clock_change_flush(realtime_fd, other_fd);

	(void) close(other_fd);
	(void) close(realtime_fd);
	(void) printf("PASS: realtime, monotonic, boottime, invalid IDs, "
	    "and queued clock changes\n");
	return (EXIT_SUCCESS);
}
