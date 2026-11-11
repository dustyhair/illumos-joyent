# LX input ABI tests

These tests are compiled and run inside an LX zone. They deliberately use
Linux system headers rather than private illumos definitions, so they check
the ABI seen by unmodified Linux software. They can also run as Linux-host
controls when `/dev/uinput` is available and writable.

Compile them with:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -O2 -o uinput-smoke uinput-smoke.c
cc -std=gnu11 -Wall -Wextra -Werror -O2 -o input-abi input-abi.c
cc -std=gnu11 -Wall -Wextra -Werror -O2 -o input-clock input-clock.c
```

Run them as a user that can open `/dev/uinput`:

```sh
./input-abi
./input-clock
./uinput-smoke
```

`input-abi` is the narrow discovery regression probe. It checks that path
`stat` and opened-descriptor `fstat` return identical Linux device numbers for
both uinput and evdev. It also enumerates the misc, input, virtual-device, and
device-number sysfs directories, rejects empty names, and verifies that the
enumerated entries resolve through their public symlinks.

`input-clock` checks the Linux clock identifiers accepted by
`EVIOCSCLOCKID`. It verifies realtime, monotonic, and boottime timestamps on
separate evdev clients, rejects unsupported identifiers without changing the
selected clock, and checks that a clock change replaces queued events with
`SYN_DROPPED` before reporting events in the new clock domain.

`uinput-smoke` creates a virtual keyboard, finds its evdev node by name, verifies
Linux device numbers, sysfs discovery data, and capability ioctls, sends
events to two clients, checks zone-local netlink hotplug notifications and
exclusive grabs, destroys the device, and verifies hangup behavior.
