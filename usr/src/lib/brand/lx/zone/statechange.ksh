#!/bin/ksh -p
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

#
# Copyright (c) 2018 Joyent, Inc.  All rights reserved.
#

# Do we support vrrp?
# jst_vrrp="yes"

# Do we support unfiltered promiscuous mode
# jst_ufpromisc="yes"

# Do we use vnd devices
# jst_createvnd="yes"

# Do we not need to have various SMF and ccs stuff created for us?
jst_simplefs="yes"

# Should we show snapshots?
jst_showsnap="yes"

# Where does the mdata socket live?
jst_mdatapath="/native/.zonecontrol/"

# Ensure docker zfd(7D) log is not put in the wrong place
if [[ "$_ZONECFG_attr_docker" == true ]]; then
	typeset -A FORCED_ATTRS
	FORCED_ATTRS["zlog-name"]=
fi

# A private network is host infrastructure associated with the zone lifecycle.
# Prepare it after zoneadmd has made the zone ready but before the brand boot
# hook runs.  Remove its rules while the host VNIC still has an IP Filter
# interface identity, then remove the transient links after halt.  The
# post-halt cleanup is an idempotent fallback for interrupted transitions.
# vmadm and zoneadm remain responsible for deciding whether the zone should
# run.
if [[ "$1" == pre && "$5" == 4 ]]; then
	/usr/lib/brand/lx/lx_private_network quiesce "$2" "$3" || exit 1
elif [[ "$1" == post && "$5" == 0 ]]; then
	/usr/lib/brand/lx/lx_private_network prepare "$2" "$3" || exit 1
elif [[ "$1" == post && "$5" == 4 ]]; then
	/usr/lib/brand/lx/lx_private_network cleanup "$2" "$3" || exit 1
fi

. /usr/lib/brand/jcommon/statechange
