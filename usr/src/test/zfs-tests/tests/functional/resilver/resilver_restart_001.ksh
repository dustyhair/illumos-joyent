#!/bin/ksh -p

#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#

#
# Copyright (c) 2019, Datto Inc. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/resilver/resilver.cfg

SYSEVENT=$STF_SUITE/tests/functional/resilver/sysevent

#
# DESCRIPTION:
# Testing resilver restart logic both with and without the deferred resilver
# feature enabled, verifying that resilver is not restarted when it is
# unecessary.
#
# STRATEGY:
# 1. Create a pool
# 2. Create four filesystems with the primary cache disable to force reads
# 3. Write four files simultaneously, one to each filesystem
# 4. Do with and without deferred resilvers enabled
#    a. Replace a vdev with a spare & suspend resilver immediately
#    b. Verify resilver starts properly
#    c. Offline / online another vdev to introduce a new DTL range
#    d. Verify resilver restart restart or defer
#    e. Inject read errors on vdev that was offlined / onlned
#    f. Verify that resilver did not restart
#    g. Unsuspend resilver and wait for it to finish
#    h. Verify that there are two resilvers and nothing is deferred
#

function cleanup
{
	log_must set_tunable32 zfs_resilver_min_time_ms $ORIG_RESILVER_MIN_TIME
	log_must set_tunable32 zfs_scan_suspend_progress \
	    $ORIG_SCAN_SUSPEND_PROGRESS
	log_must zinject -c all
	destroy_pool $TESTPOOL
	rm -f ${VDEV_FILES[@]} $SPARE_VDEV_FILE
	[[ -n "$EVTFILE" ]] && rm -f "$EVTFILE"
	[[ -n "$EVTPID" ]] && kill "$EVTPID"
}

# count resilver events in zpool and number of deferred rsilvers on vdevs
function verify_restarts # <msg> <cnt> <defer>
{
	msg=$1
	cnt=$2
	defer=$3

	# check the number of resilver start in events log
	RESILVERS=$(wc -l $EVTFILE | awk '{ print $1 }')
	log_note "expected $cnt resilver start(s)$msg, found $RESILVERS"
	[[ "$RESILVERS" -ne "$cnt" ]] &&
	    log_fail "expected $cnt resilver start(s)$msg, found $RESILVERS"

	[[ -z "$defer" ]] && return

	# use zdb to find which vdevs have the resilver defer flag
	VDEV_DEFERS=$(zdb -C $TESTPOOL | awk '
	    /children/ { gsub(/[^0-9]/, ""); child = $0 }
	    /com\.datto:resilver_defer$/ { print child }
	')

	if [[ "$defer" == "-" ]]
	then
		[[ -n $VDEV_DEFERS ]] &&
		    log_fail "didn't expect any vdevs to have resilver deferred"
		return
	fi

	[[ $VDEV_DEFERS -eq $defer ]] ||
	    log_fail "resilver deferred set on unexpected vdev: $VDEV_DEFERS"
}

log_assert "Check for unnecessary resilver restarts"

ORIG_RESILVER_MIN_TIME=$(get_tunable zfs_resilver_min_time_ms)
ORIG_SCAN_SUSPEND_PROGRESS=$(get_tunable zfs_scan_suspend_progress)

set -A RESTARTS -- '1' '2' '2' '2'
set -A VDEVS -- '' '' '' ''
set -A DEFER_RESTARTS -- '1' '1' '1' '2'
set -A DEFER_VDEVS -- '-' '2' '2' '-'

VDEV_REPLACE="${VDEV_FILES[1]} $SPARE_VDEV_FILE"

log_onexit cleanup

# Monitor for resilver start events and log them to $EVTFILE as they occur
EVTFILE=$(mktemp /tmp/resilver_events.XXXXXX)
EVTPID=$($SYSEVENT $EVTFILE)
log_must test -n "$EVTPID"

log_must truncate -s $VDEV_FILE_SIZE ${VDEV_FILES[@]} $SPARE_VDEV_FILE

log_must zpool create -f -o feature@resilver_defer=disabled $TESTPOOL \
    raidz ${VDEV_FILES[@]}

# create 4 filesystems
for fs in fs{0..3}
do
	log_must zfs create -o primarycache=none -o recordsize=1k $TESTPOOL/$fs
done

# simultaneously write 16M to each of them
set -A DATAPATHS /$TESTPOOL/fs{0..3}/dat.0
log_note "Writing data files"
for path in ${DATAPATHS[@]}
do
	dd if=/dev/urandom of=$path bs=1M count=16 > /dev/null 2>&1 &
done
wait

# test without and with deferred resilve feature enabled
for test in "without" "with"
do
	log_note "Testing $test deferred resilvers"

	if [[ $test == "with" ]]
	then
		log_must zpool set feature@resilver_defer=enabled $TESTPOOL
		RESTARTS=( "${DEFER_RESTARTS[@]}" )
		VDEVS=( "${DEFER_VDEVS[@]}" )
		VDEV_REPLACE="$SPARE_VDEV_FILE ${VDEV_FILES[1]}"
	fi

	# clear the events
	cp /dev/null $EVTFILE

	# limit scanning time
	log_must set_tunable32 zfs_resilver_min_time_ms 50

	# initiate a resilver and suspend the scan as soon as possible
	log_must zpool replace $TESTPOOL $VDEV_REPLACE
	log_must set_tunable32 zfs_scan_suspend_progress 1

	# there should only be 1 resilver start
	verify_restarts '' "${RESTARTS[0]}" "${VDEVS[0]}"

	# offline then online a vdev to introduce a new DTL range after current
	# scan, which should restart (or defer) the resilver
	log_must zpool offline $TESTPOOL ${VDEV_FILES[2]}
	log_must zpool sync $TESTPOOL
	log_must zpool online $TESTPOOL ${VDEV_FILES[2]}
	log_must zpool sync $TESTPOOL

	# there should now be 2 resilver starts w/o defer, 1 with defer
	verify_restarts ' after offline/online' "${RESTARTS[1]}" "${VDEVS[1]}"

	# inject read io errors on vdev and verify resilver does not restart
	log_must zinject -a -d ${VDEV_FILES[2]} -e io -T read -f 0.25 $TESTPOOL
	log_must cat ${DATAPATHS[1]} > /dev/null
	log_must zinject -c all

	# there should still be 2 resilver starts w/o defer, 1 with defer
	verify_restarts ' after zinject' "${RESTARTS[2]}" "${VDEVS[2]}"

	# unsuspend resilver
	log_must set_tunable32 zfs_scan_suspend_progress 0
	log_must set_tunable32 zfs_resilver_min_time_ms 3000

	# wait for resilver to finish
	for iter in {0..59}
	do
		is_pool_resilvered $TESTPOOL && break
		sleep 1
	done
	is_pool_resilvered $TESTPOOL ||
	    log_fail "resilver timed out"

	# wait for a few txg's to see if a resilver happens
	log_must zpool sync $TESTPOOL
	log_must zpool sync $TESTPOOL

	# there should now be 2 resilver starts
	verify_restarts ' after resilver' "${RESTARTS[3]}" "${VDEVS[3]}"
done

log_pass "Resilver did not restart unnecessarily"
