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
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	EVENT_LIMIT	64

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

static void
check_directory(const char *path, const char *wanted)
{
	struct dirent *entry;
	DIR *dir;
	bool found = false;
	char message[256];

	dir = opendir(path);
	if (dir == NULL)
		fail(path);
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		(void) snprintf(message, sizeof (message),
		    "%s returned an empty directory name", path);
		check(entry->d_name[0] != '\0', message);
		if (strcmp(entry->d_name, wanted) == 0)
			found = true;
	}
	if (errno != 0)
		fail(path);
	if (closedir(dir) != 0)
		fail(path);
	(void) snprintf(message, sizeof (message), "%s did not enumerate %s",
	    path, wanted);
	check(found, message);
}

static void
check_link_contains(const char *path, const char *wanted)
{
	char target[512];
	ssize_t size;

	size = readlink(path, target, sizeof (target) - 1);
	if (size < 0)
		fail(path);
	target[size] = '\0';
	check(strstr(target, wanted) != NULL,
	    "sysfs link has an unexpected target");
}

static void
check_stat_pair(const char *path, int fd, unsigned int wanted_major,
    int wanted_minor)
{
	struct stat path_stat;
	struct stat fd_stat;

	if (stat(path, &path_stat) != 0)
		fail(path);
	if (fstat(fd, &fd_stat) != 0)
		fail("fstat input device");
	check(S_ISCHR(path_stat.st_mode) && S_ISCHR(fd_stat.st_mode),
	    "input path or descriptor is not a character device");
	check(path_stat.st_rdev == fd_stat.st_rdev,
	    "input path stat and descriptor fstat device numbers differ");
	check(major(fd_stat.st_rdev) == wanted_major,
	    "input descriptor has the wrong Linux major number");
	if (wanted_minor >= 0) {
		check(minor(fd_stat.st_rdev) == (unsigned int)wanted_minor,
		    "input descriptor has the wrong Linux minor number");
	}
}

static int
find_event_device(const char *wanted_name, char *path, size_t path_size)
{
	struct timespec delay = { .tv_nsec = 20 * 1000 * 1000 };
	unsigned int attempt;

	for (attempt = 0; attempt < 100; attempt++) {
		unsigned int slot;

		for (slot = 0; slot < EVENT_LIMIT; slot++) {
			char candidate[64];
			char name[UINPUT_MAX_NAME_SIZE];
			int fd;

			(void) snprintf(candidate, sizeof (candidate),
			    "/dev/input/event%u", slot);
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
		(void) nanosleep(&delay, NULL);
	}

	errno = ENODEV;
	return (-1);
}

int
main(void)
{
	char device_name[UINPUT_MAX_NAME_SIZE];
	char event_path[64];
	char event_name[32];
	char sysname[64];
	char sysfs_path[256];
	char devchar_name[32];
	struct uinput_setup setup;
	struct stat event_stat;
	const char *base;
	int event_fd;

	if (atexit(cleanup) != 0)
		fail("atexit");

	check_directory("/sys/class/misc", "uinput");
	check_directory("/sys/devices/virtual/misc", "uinput");
	check_directory("/sys/dev/char", "10:223");
	check_link_contains("/sys/class/misc/uinput",
	    "/devices/virtual/misc/uinput");
	check_link_contains("/sys/dev/char/10:223",
	    "/devices/virtual/misc/uinput");

	uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uinput_fd < 0)
		fail("open /dev/uinput");
	check_stat_pair("/dev/uinput", uinput_fd, 10, 223);

	if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) != 0)
		fail("UI_SET_EVBIT EV_KEY");
	if (ioctl(uinput_fd, UI_SET_KEYBIT, KEY_A) != 0)
		fail("UI_SET_KEYBIT KEY_A");

	(void) memset(&setup, 0, sizeof (setup));
	(void) snprintf(device_name, sizeof (device_name),
	    "lx-input-abi-%ld", (long)getpid());
	(void) snprintf(setup.name, sizeof (setup.name), "%s", device_name);
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1;
	setup.id.product = 0x52;
	setup.id.version = 0x1;
	if (ioctl(uinput_fd, UI_DEV_SETUP, &setup) != 0)
		fail("UI_DEV_SETUP");
	if (ioctl(uinput_fd, UI_DEV_CREATE) != 0)
		fail("UI_DEV_CREATE");
	device_created = true;

	(void) memset(sysname, 0, sizeof (sysname));
	if (ioctl(uinput_fd, UI_GET_SYSNAME(sizeof (sysname)), sysname) < 0)
		fail("UI_GET_SYSNAME");
	event_fd = find_event_device(device_name, event_path,
	    sizeof (event_path));
	if (event_fd < 0)
		fail("find matching event device");
	check_stat_pair(event_path, event_fd, 13, -1);
	if (stat(event_path, &event_stat) != 0)
		fail(event_path);

	base = strrchr(event_path, '/');
	check(base != NULL && base[1] != '\0', "event path has no basename");
	(void) snprintf(event_name, sizeof (event_name), "%s", base + 1);
	(void) snprintf(devchar_name, sizeof (devchar_name), "13:%u",
	    minor(event_stat.st_rdev));

	check_directory("/sys/class/input", sysname);
	check_directory("/sys/class/input", event_name);
	check_directory("/sys/devices/virtual/input", sysname);
	check_directory("/sys/dev/char", devchar_name);
	(void) snprintf(sysfs_path, sizeof (sysfs_path),
	    "/sys/devices/virtual/input/%s", sysname);
	check_directory(sysfs_path, event_name);
	(void) snprintf(sysfs_path, sizeof (sysfs_path),
	    "/sys/class/input/%s", sysname);
	check_link_contains(sysfs_path, "/devices/virtual/input/");
	(void) snprintf(sysfs_path, sizeof (sysfs_path),
	    "/sys/class/input/%s", event_name);
	check_link_contains(sysfs_path, "/devices/virtual/input/");
	(void) snprintf(sysfs_path, sizeof (sysfs_path),
	    "/sys/dev/char/%s", devchar_name);
	check_link_contains(sysfs_path, "/devices/virtual/input/");

	if (ioctl(uinput_fd, UI_DEV_DESTROY) != 0)
		fail("UI_DEV_DESTROY");
	device_created = false;
	(void) close(event_fd);
	(void) close(uinput_fd);
	uinput_fd = -1;

	(void) printf("PASS: input stat/fstat and sysfs enumeration (%s, %s)\n",
	    sysname, event_path);
	return (EXIT_SUCCESS);
}
