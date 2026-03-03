#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Setup for bclone_dedup (zfs recv -B) tests.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.cfg

verify_runnable "global"

if tunable_exists BCLONE_ENABLED ; then
	# Remove stale tunable file from a previous interrupted run
	rm -f "$TEST_BASE_DIR/tunable-BCLONE_ENABLED" 2>/dev/null
	log_must save_tunable BCLONE_ENABLED
	log_must set_tunable32 BCLONE_ENABLED 1
fi

log_pass
