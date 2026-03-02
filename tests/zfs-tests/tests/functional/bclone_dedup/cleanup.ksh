#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Cleanup for bclone_dedup (zfs recv -B) tests.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.cfg

verify_runnable "global"

default_cleanup_noexit

if tunable_exists BCLONE_ENABLED ; then
	log_must restore_tunable BCLONE_ENABLED
fi

log_pass
