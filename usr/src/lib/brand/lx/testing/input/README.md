# LX uinput and evdev smoke test

This test is compiled and run inside an LX zone. It deliberately uses the
Linux system headers rather than private illumos definitions, so it checks the
ABI seen by unmodified Linux software.

Compile it in the zone with:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -O2 -o uinput-smoke uinput-smoke.c
```

Run it as a user that can open `/dev/uinput`:

```sh
./uinput-smoke
```

The test creates a virtual keyboard, finds its evdev node by name, verifies
Linux device numbers and capability ioctls, sends events to two clients,
checks exclusive grabs, destroys the device, and verifies hangup behavior.
