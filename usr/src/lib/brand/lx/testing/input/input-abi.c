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
#include <sys/wait.h>

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

typedef struct attribute_snapshot {
	ino_t as_ino;
	mode_t as_mode;
	uid_t as_uid;
	gid_t as_gid;
	ssize_t as_size;
	char as_content[1024];
} attribute_snapshot_t;

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
check_uevent_action(const char *path, int flags, const char *action,
    int wanted_error)
{
	ssize_t result;
	size_t size = strlen(action);
	int error;
	int fd;

	fd = (flags & O_CREAT) != 0 ? open(path, flags, 0600) :
	    open(path, flags);
	if (fd < 0)
		fail(path);
	errno = 0;
	result = write(fd, action, size);
	error = errno;
	if (close(fd) != 0)
		fail("close sysfs uevent");

	if (wanted_error == 0) {
		check(result == (ssize_t)size,
		    "valid sysfs uevent action failed");
	} else {
		check(result < 0 && error == wanted_error,
		    "invalid sysfs uevent action returned the wrong error");
	}
}

static void
check_readonly_open(const char *path, int flags)
{
	int error;
	int fd;

	errno = 0;
	fd = (flags & O_CREAT) != 0 ? open(path, flags, 0600) :
	    open(path, flags);
	error = errno;
	if (fd >= 0)
		(void) close(fd);
	check(fd < 0 && (error == EACCES || error == EROFS),
	    "read-only sysfs attribute accepted a writable open");
}

static void
snapshot_attribute(const char *path, attribute_snapshot_t *snapshot)
{
	struct stat st;
	int fd;

	if (stat(path, &st) != 0)
		fail(path);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail(path);
	snapshot->as_size = read(fd, snapshot->as_content,
	    sizeof (snapshot->as_content));
	if (snapshot->as_size < 0)
		fail("read sysfs attribute");
	if (close(fd) != 0)
		fail("close sysfs attribute");
	snapshot->as_ino = st.st_ino;
	snapshot->as_mode = st.st_mode;
	snapshot->as_uid = st.st_uid;
	snapshot->as_gid = st.st_gid;
}

static void
check_attribute_unchanged(const attribute_snapshot_t *before,
    const attribute_snapshot_t *after)
{
	check(before->as_ino == after->as_ino,
	    "sysfs O_CREAT replaced the existing inode");
	check(before->as_mode == after->as_mode,
	    "sysfs O_CREAT changed the existing mode");
	check(before->as_uid == after->as_uid &&
	    before->as_gid == after->as_gid,
	    "sysfs O_CREAT changed attribute ownership");
	check(before->as_size == after->as_size &&
	    memcmp(before->as_content, after->as_content,
	    (size_t)before->as_size) == 0,
	    "sysfs O_CREAT changed synthetic attribute content");
}

static void
check_shell_redirect(const char *path)
{
	int status;
	pid_t child;

	child = fork();
	if (child < 0)
		fail("fork sysfs shell redirect");
	if (child == 0) {
		(void) execl("/bin/sh", "sh", "-c",
		    "printf 'change\\n' > \"$1\"", "sh", path, NULL);
		_exit(127);
	}
	if (waitpid(child, &status, 0) != child)
		fail("waitpid sysfs shell redirect");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "shell redirect to sysfs uevent failed");
}

static void
check_absent_create(const char *existing_path)
{
	char absent_path[256];
	char *base;
	struct stat st;
	int error;
	int fd;

	(void) snprintf(absent_path, sizeof (absent_path), "%s", existing_path);
	base = strrchr(absent_path, '/');
	check(base != NULL, "sysfs attribute path has no parent");
	(void) snprintf(base + 1, sizeof (absent_path) -
	    (size_t)(base + 1 - absent_path), "lx-no-such-attribute");
	errno = 0;
	fd = open(absent_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	error = errno;
	if (fd >= 0)
		(void) close(fd);
	check(fd < 0 && error == ENOENT,
	    "sysfs created an absent synthetic attribute");
	errno = 0;
	check(lstat(absent_path, &st) < 0 && errno == ENOENT,
	    "absent sysfs attribute appeared after O_CREAT");
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
				(void) snprintf(path, path_size, "%s",
				    candidate);
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
	char uevent_path[256];
	char readonly_path[256];
	char devchar_name[32];
	struct uinput_setup setup;
	attribute_snapshot_t before;
	attribute_snapshot_t after;
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

	(void) snprintf(uevent_path, sizeof (uevent_path),
	    "/sys/class/input/%s/uevent", event_name);
	snapshot_attribute(uevent_path, &before);
	check_uevent_action(uevent_path, O_WRONLY, "change\n", 0);
	check_uevent_action(uevent_path, O_WRONLY | O_TRUNC, "change\n", 0);
	check_uevent_action(uevent_path, O_WRONLY | O_CREAT, "change\n", 0);
	check_uevent_action(uevent_path, O_WRONLY | O_CREAT | O_TRUNC,
	    "change\n", 0);
	check_uevent_action(uevent_path, O_WRONLY | O_CREAT | O_TRUNC,
	    "invalid\n", EINVAL);
	check_shell_redirect(uevent_path);
	check_absent_create(uevent_path);
	snapshot_attribute(uevent_path, &after);
	check_attribute_unchanged(&before, &after);

	(void) snprintf(readonly_path, sizeof (readonly_path),
	    "/sys/class/input/%s/name", sysname);
	check_readonly_open(readonly_path, O_WRONLY);
	check_readonly_open(readonly_path, O_WRONLY | O_TRUNC);
	check_readonly_open(readonly_path, O_WRONLY | O_CREAT);
	check_readonly_open(readonly_path, O_WRONLY | O_CREAT | O_TRUNC);

	if (ioctl(uinput_fd, UI_DEV_DESTROY) != 0)
		fail("UI_DEV_DESTROY");
	device_created = false;
	(void) close(event_fd);
	(void) close(uinput_fd);
	uinput_fd = -1;

	(void) printf("PASS: input ABI, sysfs discovery, and uevent writes "
	    "(%s, %s)\n", sysname, event_path);
	return (EXIT_SUCCESS);
}
