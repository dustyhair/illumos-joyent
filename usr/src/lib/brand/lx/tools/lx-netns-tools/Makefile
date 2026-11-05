VERSION = 0.1.2
XBPS_REVISION = 1
ARCH = amd64
XBPS_ARCH = x86_64
BUILD = build
ROOT = $(BUILD)/root
CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Werror

.PHONY: all clean test root deb xbps

all: $(BUILD)/lx-bwrap-wrapper $(BUILD)/lx-netns-helper

$(BUILD):
	mkdir -p $@

$(BUILD)/lx-bwrap-wrapper: src/lx-bwrap-wrapper.c | $(BUILD)
	$(CC) -nostdlib -static -fno-pie -no-pie -Wall -Wextra -Werror \
	    -o $@ $<

$(BUILD)/lx-netns-helper: src/lx-netns-helper.c | $(BUILD)
	$(CC) -static $(CFLAGS) -o $@ $<

test: all
	tests/test.sh

root: all
	rm -rf $(ROOT)
	mkdir -p $(ROOT)/usr/bin $(ROOT)/usr/libexec/lx-netns-tools \
	    $(ROOT)/usr/share/doc/lx-netns-tools
	install -m 755 src/lx-bwrap $(ROOT)/usr/bin/lx-bwrap
	install -m 755 $(BUILD)/lx-bwrap-wrapper \
	    $(ROOT)/usr/libexec/lx-netns-tools/lx-bwrap-wrapper
	install -m 755 $(BUILD)/lx-netns-helper \
	    $(ROOT)/usr/libexec/lx-netns-tools/lx-netns-helper
	install -m 644 /dev/null \
	    $(ROOT)/usr/libexec/lx-netns-tools/run.lock
	install -m 644 README.md $(ROOT)/usr/share/doc/lx-netns-tools/README.md

deb: test root
	mkdir -p $(ROOT)/DEBIAN
	install -m 644 pkg/debian/control $(ROOT)/DEBIAN/control
	dpkg-deb --root-owner-group --build $(ROOT) \
	    $(BUILD)/lx-netns-tools_$(VERSION)_$(ARCH).deb

xbps: test root
	mkdir -p $(BUILD)/xbps
	cd $(BUILD)/xbps && xbps-create -A $(XBPS_ARCH) \
	    -n lx-netns-tools-$(VERSION)_$(XBPS_REVISION) \
	    -s "SmartOS LX private network namespace tools" \
	    -D "bash>=0 bubblewrap>=0 util-linux>=0" ../root

clean:
	rm -rf $(BUILD)
