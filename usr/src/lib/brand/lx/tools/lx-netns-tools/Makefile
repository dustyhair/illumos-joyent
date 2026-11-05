#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
# Copyright 2026 Jonathan Wagner.
#

VERSION = 0.1.4
XBPS_REVISION = 1
ARCH = amd64
XBPS_ARCH = x86_64
BUILD = build
ROOT = $(BUILD)/root
CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Werror

.PHONY: all clean test root deb xbps check-version

all: $(BUILD)/lx-bwrap-wrapper $(BUILD)/lx-netns-helper

$(BUILD):
	mkdir -p $@

$(BUILD)/lx-bwrap-wrapper: src/lx-bwrap-wrapper.c | $(BUILD)
	$(CC) -nostdlib -static -fno-pie -no-pie -Wall -Wextra -Werror \
	    -o $@ $<

$(BUILD)/lx-netns-helper: src/lx-netns-helper.c | $(BUILD)
	$(CC) -static $(CFLAGS) -o $@ $<

check-version:
	grep -Fxq 'version=$(VERSION)' src/lx-bwrap
	grep -Fxq 'Version: $(VERSION)' pkg/debian/control

test: all check-version
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
	install -m 644 README.md $(ROOT)/usr/share/doc/lx-netns-tools/README.md
	install -m 644 LICENSE $(ROOT)/usr/share/doc/lx-netns-tools/LICENSE
	install -m 644 pkg/debian/copyright \
	    $(ROOT)/usr/share/doc/lx-netns-tools/copyright

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
	    -l CDDL-1.0 -m "Jonathan Wagner <dustyhair@gmail.com>" \
	    -D "bash>=0 bubblewrap>=0 util-linux>=0" ../root

clean:
	rm -rf $(BUILD)
