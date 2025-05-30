// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2020 Joyent, Inc. All rights reserved.
 * Copyright (c) 2011, 2024 by Delphix. All rights reserved.
 * Copyright 2016 Igor Kozhukhov <ikozhukhov@gmail.com>
 * Copyright (c) 2017 Datto Inc.
 * Copyright (c) 2020 The FreeBSD Foundation
 *
 * Portions of this software were developed by Allan Jude
 * under sponsorship from the FreeBSD Foundation.
 */

/*
 * Internal utility routines for the ZFS library.
 */

#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#if LIBFETCH_DYNAMIC
#include <dlfcn.h>
#endif
#include <sys/fiemap.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#include <libzfs.h>
#include <libzfs_core.h>

#include "libzfs_impl.h"
#include "zfs_prop.h"
#include "zfeature_common.h"
#include <zfs_fletcher.h>
#include <libzutil.h>

/*
 * We only care about the scheme in order to match the scheme
 * with the handler. Each handler should validate the full URI
 * as necessary.
 */
#define	URI_REGEX	"^\\([A-Za-z][A-Za-z0-9+.\\-]*\\):"
#define	STR_NUMS	"0123456789"

int
libzfs_errno(libzfs_handle_t *hdl)
{
	return (hdl->libzfs_error);
}

const char *
libzfs_error_action(libzfs_handle_t *hdl)
{
	return (hdl->libzfs_action);
}

const char *
libzfs_error_description(libzfs_handle_t *hdl)
{
	if (hdl->libzfs_desc[0] != '\0')
		return (hdl->libzfs_desc);

	switch (hdl->libzfs_error) {
	case EZFS_NOMEM:
		return (dgettext(TEXT_DOMAIN, "out of memory"));
	case EZFS_BADPROP:
		return (dgettext(TEXT_DOMAIN, "invalid property value"));
	case EZFS_PROPREADONLY:
		return (dgettext(TEXT_DOMAIN, "read-only property"));
	case EZFS_PROPTYPE:
		return (dgettext(TEXT_DOMAIN, "property doesn't apply to "
		    "datasets of this type"));
	case EZFS_PROPNONINHERIT:
		return (dgettext(TEXT_DOMAIN, "property cannot be inherited"));
	case EZFS_PROPSPACE:
		return (dgettext(TEXT_DOMAIN, "invalid quota or reservation"));
	case EZFS_BADTYPE:
		return (dgettext(TEXT_DOMAIN, "operation not applicable to "
		    "datasets of this type"));
	case EZFS_BUSY:
		return (dgettext(TEXT_DOMAIN, "pool or dataset is busy"));
	case EZFS_EXISTS:
		return (dgettext(TEXT_DOMAIN, "pool or dataset exists"));
	case EZFS_NOENT:
		return (dgettext(TEXT_DOMAIN, "no such pool or dataset"));
	case EZFS_BADSTREAM:
		return (dgettext(TEXT_DOMAIN, "invalid backup stream"));
	case EZFS_DSREADONLY:
		return (dgettext(TEXT_DOMAIN, "dataset is read-only"));
	case EZFS_VOLTOOBIG:
		return (dgettext(TEXT_DOMAIN, "volume size exceeds limit for "
		    "this system"));
	case EZFS_INVALIDNAME:
		return (dgettext(TEXT_DOMAIN, "invalid name"));
	case EZFS_BADRESTORE:
		return (dgettext(TEXT_DOMAIN, "unable to restore to "
		    "destination"));
	case EZFS_BADBACKUP:
		return (dgettext(TEXT_DOMAIN, "backup failed"));
	case EZFS_BADTARGET:
		return (dgettext(TEXT_DOMAIN, "invalid target vdev"));
	case EZFS_NODEVICE:
		return (dgettext(TEXT_DOMAIN, "no such device in pool"));
	case EZFS_BADDEV:
		return (dgettext(TEXT_DOMAIN, "invalid device"));
	case EZFS_NOREPLICAS:
		return (dgettext(TEXT_DOMAIN, "no valid replicas"));
	case EZFS_RESILVERING:
		return (dgettext(TEXT_DOMAIN, "currently resilvering"));
	case EZFS_BADVERSION:
		return (dgettext(TEXT_DOMAIN, "unsupported version or "
		    "feature"));
	case EZFS_POOLUNAVAIL:
		return (dgettext(TEXT_DOMAIN, "pool is unavailable"));
	case EZFS_DEVOVERFLOW:
		return (dgettext(TEXT_DOMAIN, "too many devices in one vdev"));
	case EZFS_BADPATH:
		return (dgettext(TEXT_DOMAIN, "must be an absolute path"));
	case EZFS_CROSSTARGET:
		return (dgettext(TEXT_DOMAIN, "operation crosses datasets or "
		    "pools"));
	case EZFS_ZONED:
		return (dgettext(TEXT_DOMAIN, "dataset in use by local zone"));
	case EZFS_MOUNTFAILED:
		return (dgettext(TEXT_DOMAIN, "mount failed"));
	case EZFS_UMOUNTFAILED:
		return (dgettext(TEXT_DOMAIN, "unmount failed"));
	case EZFS_UNSHARENFSFAILED:
		return (dgettext(TEXT_DOMAIN, "NFS share removal failed"));
	case EZFS_SHARENFSFAILED:
		return (dgettext(TEXT_DOMAIN, "NFS share creation failed"));
	case EZFS_UNSHARESMBFAILED:
		return (dgettext(TEXT_DOMAIN, "SMB share removal failed"));
	case EZFS_SHARESMBFAILED:
		return (dgettext(TEXT_DOMAIN, "SMB share creation failed"));
	case EZFS_PERM:
		return (dgettext(TEXT_DOMAIN, "permission denied"));
	case EZFS_NOSPC:
		return (dgettext(TEXT_DOMAIN, "out of space"));
	case EZFS_FAULT:
		return (dgettext(TEXT_DOMAIN, "bad address"));
	case EZFS_IO:
		return (dgettext(TEXT_DOMAIN, "I/O error"));
	case EZFS_INTR:
		return (dgettext(TEXT_DOMAIN, "signal received"));
	case EZFS_CKSUM:
		return (dgettext(TEXT_DOMAIN, "insufficient replicas"));
	case EZFS_ISSPARE:
		return (dgettext(TEXT_DOMAIN, "device is reserved as a hot "
		    "spare"));
	case EZFS_INVALCONFIG:
		return (dgettext(TEXT_DOMAIN, "invalid vdev configuration"));
	case EZFS_RECURSIVE:
		return (dgettext(TEXT_DOMAIN, "recursive dataset dependency"));
	case EZFS_NOHISTORY:
		return (dgettext(TEXT_DOMAIN, "no history available"));
	case EZFS_POOLPROPS:
		return (dgettext(TEXT_DOMAIN, "failed to retrieve "
		    "pool properties"));
	case EZFS_POOL_NOTSUP:
		return (dgettext(TEXT_DOMAIN, "operation not supported "
		    "on this type of pool"));
	case EZFS_POOL_INVALARG:
		return (dgettext(TEXT_DOMAIN, "invalid argument for "
		    "this pool operation"));
	case EZFS_NAMETOOLONG:
		return (dgettext(TEXT_DOMAIN, "dataset name is too long"));
	case EZFS_OPENFAILED:
		return (dgettext(TEXT_DOMAIN, "open failed"));
	case EZFS_NOCAP:
		return (dgettext(TEXT_DOMAIN,
		    "disk capacity information could not be retrieved"));
	case EZFS_LABELFAILED:
		return (dgettext(TEXT_DOMAIN, "write of label failed"));
	case EZFS_BADWHO:
		return (dgettext(TEXT_DOMAIN, "invalid user/group"));
	case EZFS_BADPERM:
		return (dgettext(TEXT_DOMAIN, "invalid permission"));
	case EZFS_BADPERMSET:
		return (dgettext(TEXT_DOMAIN, "invalid permission set name"));
	case EZFS_NODELEGATION:
		return (dgettext(TEXT_DOMAIN, "delegated administration is "
		    "disabled on pool"));
	case EZFS_BADCACHE:
		return (dgettext(TEXT_DOMAIN, "invalid or missing cache file"));
	case EZFS_ISL2CACHE:
		return (dgettext(TEXT_DOMAIN, "device is in use as a cache"));
	case EZFS_VDEVNOTSUP:
		return (dgettext(TEXT_DOMAIN, "vdev specification is not "
		    "supported"));
	case EZFS_NOTSUP:
		return (dgettext(TEXT_DOMAIN, "operation not supported "
		    "on this dataset"));
	case EZFS_IOC_NOTSUPPORTED:
		return (dgettext(TEXT_DOMAIN, "operation not supported by "
		    "zfs kernel module"));
	case EZFS_ACTIVE_SPARE:
		return (dgettext(TEXT_DOMAIN, "pool has active shared spare "
		    "device"));
	case EZFS_UNPLAYED_LOGS:
		return (dgettext(TEXT_DOMAIN, "log device has unplayed intent "
		    "logs"));
	case EZFS_REFTAG_RELE:
		return (dgettext(TEXT_DOMAIN, "no such tag on this dataset"));
	case EZFS_REFTAG_HOLD:
		return (dgettext(TEXT_DOMAIN, "tag already exists on this "
		    "dataset"));
	case EZFS_TAGTOOLONG:
		return (dgettext(TEXT_DOMAIN, "tag too long"));
	case EZFS_PIPEFAILED:
		return (dgettext(TEXT_DOMAIN, "pipe create failed"));
	case EZFS_THREADCREATEFAILED:
		return (dgettext(TEXT_DOMAIN, "thread create failed"));
	case EZFS_POSTSPLIT_ONLINE:
		return (dgettext(TEXT_DOMAIN, "disk was split from this pool "
		    "into a new one"));
	case EZFS_SCRUB_PAUSED:
		return (dgettext(TEXT_DOMAIN, "scrub is paused; "
		    "use 'zpool scrub' to resume scrub"));
	case EZFS_SCRUB_PAUSED_TO_CANCEL:
		return (dgettext(TEXT_DOMAIN, "scrub is paused; "
		    "use 'zpool scrub' to resume or 'zpool scrub -s' to "
		    "cancel scrub"));
	case EZFS_SCRUBBING:
		return (dgettext(TEXT_DOMAIN, "currently scrubbing; "
		    "use 'zpool scrub -s' to cancel scrub"));
	case EZFS_ERRORSCRUBBING:
		return (dgettext(TEXT_DOMAIN, "currently error scrubbing; "
		    "use 'zpool scrub -s' to cancel error scrub"));
	case EZFS_ERRORSCRUB_PAUSED:
		return (dgettext(TEXT_DOMAIN, "error scrub is paused; "
		    "use 'zpool scrub -e' to resume error scrub"));
	case EZFS_NO_SCRUB:
		return (dgettext(TEXT_DOMAIN, "there is no active scrub"));
	case EZFS_DIFF:
		return (dgettext(TEXT_DOMAIN, "unable to generate diffs"));
	case EZFS_DIFFDATA:
		return (dgettext(TEXT_DOMAIN, "invalid diff data"));
	case EZFS_POOLREADONLY:
		return (dgettext(TEXT_DOMAIN, "pool is read-only"));
	case EZFS_NO_PENDING:
		return (dgettext(TEXT_DOMAIN, "operation is not "
		    "in progress"));
	case EZFS_CHECKPOINT_EXISTS:
		return (dgettext(TEXT_DOMAIN, "checkpoint exists"));
	case EZFS_DISCARDING_CHECKPOINT:
		return (dgettext(TEXT_DOMAIN, "currently discarding "
		    "checkpoint"));
	case EZFS_NO_CHECKPOINT:
		return (dgettext(TEXT_DOMAIN, "checkpoint does not exist"));
	case EZFS_DEVRM_IN_PROGRESS:
		return (dgettext(TEXT_DOMAIN, "device removal in progress"));
	case EZFS_VDEV_TOO_BIG:
		return (dgettext(TEXT_DOMAIN, "device exceeds supported size"));
	case EZFS_ACTIVE_POOL:
		return (dgettext(TEXT_DOMAIN, "pool is imported on a "
		    "different host"));
	case EZFS_CRYPTOFAILED:
		return (dgettext(TEXT_DOMAIN, "encryption failure"));
	case EZFS_TOOMANY:
		return (dgettext(TEXT_DOMAIN, "argument list too long"));
	case EZFS_INITIALIZING:
		return (dgettext(TEXT_DOMAIN, "currently initializing"));
	case EZFS_NO_INITIALIZE:
		return (dgettext(TEXT_DOMAIN, "there is no active "
		    "initialization"));
	case EZFS_WRONG_PARENT:
		return (dgettext(TEXT_DOMAIN, "invalid parent dataset"));
	case EZFS_TRIMMING:
		return (dgettext(TEXT_DOMAIN, "currently trimming"));
	case EZFS_NO_TRIM:
		return (dgettext(TEXT_DOMAIN, "there is no active trim"));
	case EZFS_TRIM_NOTSUP:
		return (dgettext(TEXT_DOMAIN, "trim operations are not "
		    "supported by this device"));
	case EZFS_NO_RESILVER_DEFER:
		return (dgettext(TEXT_DOMAIN, "this action requires the "
		    "resilver_defer feature"));
	case EZFS_EXPORT_IN_PROGRESS:
		return (dgettext(TEXT_DOMAIN, "pool export in progress"));
	case EZFS_REBUILDING:
		return (dgettext(TEXT_DOMAIN, "currently sequentially "
		    "resilvering"));
	case EZFS_VDEV_NOTSUP:
		return (dgettext(TEXT_DOMAIN, "operation not supported "
		    "on this type of vdev"));
	case EZFS_NOT_USER_NAMESPACE:
		return (dgettext(TEXT_DOMAIN, "the provided file "
		    "was not a user namespace file"));
	case EZFS_RESUME_EXISTS:
		return (dgettext(TEXT_DOMAIN, "Resuming recv on existing "
		    "dataset without force"));
	case EZFS_RAIDZ_EXPAND_IN_PROGRESS:
		return (dgettext(TEXT_DOMAIN, "raidz expansion in progress"));
	case EZFS_ASHIFT_MISMATCH:
		return (dgettext(TEXT_DOMAIN, "adding devices with "
		    "different physical sector sizes is not allowed"));
	case EZFS_UNKNOWN:
		return (dgettext(TEXT_DOMAIN, "unknown error"));
	default:
		assert(hdl->libzfs_error == 0);
		return (dgettext(TEXT_DOMAIN, "no error"));
	}
}

void
zfs_error_aux(libzfs_handle_t *hdl, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	(void) vsnprintf(hdl->libzfs_desc, sizeof (hdl->libzfs_desc),
	    fmt, ap);
	hdl->libzfs_desc_active = 1;

	va_end(ap);
}

static void
zfs_verror(libzfs_handle_t *hdl, int error, const char *fmt, va_list ap)
{
	(void) vsnprintf(hdl->libzfs_action, sizeof (hdl->libzfs_action),
	    fmt, ap);
	hdl->libzfs_error = error;

	if (hdl->libzfs_desc_active)
		hdl->libzfs_desc_active = 0;
	else
		hdl->libzfs_desc[0] = '\0';

	if (hdl->libzfs_printerr) {
		if (error == EZFS_UNKNOWN) {
			(void) fprintf(stderr, dgettext(TEXT_DOMAIN, "internal "
			    "error: %s: %s\n"), hdl->libzfs_action,
			    libzfs_error_description(hdl));
			abort();
		}

		(void) fprintf(stderr, "%s: %s\n", hdl->libzfs_action,
		    libzfs_error_description(hdl));
		if (error == EZFS_NOMEM)
			exit(1);
	}
}

int
zfs_error(libzfs_handle_t *hdl, int error, const char *msg)
{
	return (zfs_error_fmt(hdl, error, "%s", msg));
}

int
zfs_error_fmt(libzfs_handle_t *hdl, int error, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	zfs_verror(hdl, error, fmt, ap);

	va_end(ap);

	return (-1);
}

static int
zfs_common_error(libzfs_handle_t *hdl, int error, const char *fmt,
    va_list ap)
{
	switch (error) {
	case EPERM:
	case EACCES:
		zfs_verror(hdl, EZFS_PERM, fmt, ap);
		return (-1);

	case ECANCELED:
		zfs_verror(hdl, EZFS_NODELEGATION, fmt, ap);
		return (-1);

	case EIO:
		zfs_verror(hdl, EZFS_IO, fmt, ap);
		return (-1);

	case EFAULT:
		zfs_verror(hdl, EZFS_FAULT, fmt, ap);
		return (-1);

	case EINTR:
		zfs_verror(hdl, EZFS_INTR, fmt, ap);
		return (-1);

	case ECKSUM:
		zfs_verror(hdl, EZFS_CKSUM, fmt, ap);
		return (-1);
	}

	return (0);
}

int
zfs_standard_error(libzfs_handle_t *hdl, int error, const char *msg)
{
	return (zfs_standard_error_fmt(hdl, error, "%s", msg));
}

int
zfs_standard_error_fmt(libzfs_handle_t *hdl, int error, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	if (zfs_common_error(hdl, error, fmt, ap) != 0) {
		va_end(ap);
		return (-1);
	}

	switch (error) {
	case ENXIO:
	case ENODEV:
	case EPIPE:
		zfs_verror(hdl, EZFS_IO, fmt, ap);
		break;

	case ENOENT:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "dataset does not exist"));
		zfs_verror(hdl, EZFS_NOENT, fmt, ap);
		break;

	case ENOSPC:
	case EDQUOT:
		zfs_verror(hdl, EZFS_NOSPC, fmt, ap);
		break;

	case EEXIST:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "dataset already exists"));
		zfs_verror(hdl, EZFS_EXISTS, fmt, ap);
		break;

	case EBUSY:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "dataset is busy"));
		zfs_verror(hdl, EZFS_BUSY, fmt, ap);
		break;
	case EROFS:
		zfs_verror(hdl, EZFS_POOLREADONLY, fmt, ap);
		break;
	case ENAMETOOLONG:
		zfs_verror(hdl, EZFS_NAMETOOLONG, fmt, ap);
		break;
	case ENOTSUP:
		zfs_verror(hdl, EZFS_BADVERSION, fmt, ap);
		break;
	case EAGAIN:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "pool I/O is currently suspended"));
		zfs_verror(hdl, EZFS_POOLUNAVAIL, fmt, ap);
		break;
	case EREMOTEIO:
		zfs_verror(hdl, EZFS_ACTIVE_POOL, fmt, ap);
		break;
	case ZFS_ERR_UNKNOWN_SEND_STREAM_FEATURE:
	case ZFS_ERR_IOC_CMD_UNAVAIL:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "the loaded zfs "
		    "module does not support this operation. A reboot may "
		    "be required to enable this operation."));
		zfs_verror(hdl, EZFS_IOC_NOTSUPPORTED, fmt, ap);
		break;
	case ZFS_ERR_IOC_ARG_UNAVAIL:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "the loaded zfs "
		    "module does not support an option for this operation. "
		    "A reboot may be required to enable this option."));
		zfs_verror(hdl, EZFS_IOC_NOTSUPPORTED, fmt, ap);
		break;
	case ZFS_ERR_IOC_ARG_REQUIRED:
	case ZFS_ERR_IOC_ARG_BADTYPE:
		zfs_verror(hdl, EZFS_IOC_NOTSUPPORTED, fmt, ap);
		break;
	case ZFS_ERR_WRONG_PARENT:
		zfs_verror(hdl, EZFS_WRONG_PARENT, fmt, ap);
		break;
	case ZFS_ERR_BADPROP:
		zfs_verror(hdl, EZFS_BADPROP, fmt, ap);
		break;
	case ZFS_ERR_NOT_USER_NAMESPACE:
		zfs_verror(hdl, EZFS_NOT_USER_NAMESPACE, fmt, ap);
		break;
	default:
		zfs_error_aux(hdl, "%s", zfs_strerror(error));
		zfs_verror(hdl, EZFS_UNKNOWN, fmt, ap);
		break;
	}

	va_end(ap);
	return (-1);
}

void
zfs_setprop_error(libzfs_handle_t *hdl, zfs_prop_t prop, int err,
    char *errbuf)
{
	switch (err) {

	case ENOSPC:
		/*
		 * For quotas and reservations, ENOSPC indicates
		 * something different; setting a quota or reservation
		 * doesn't use any disk space.
		 */
		switch (prop) {
		case ZFS_PROP_QUOTA:
		case ZFS_PROP_REFQUOTA:
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "size is less than current used or "
			    "reserved space"));
			(void) zfs_error(hdl, EZFS_PROPSPACE, errbuf);
			break;

		case ZFS_PROP_RESERVATION:
		case ZFS_PROP_REFRESERVATION:
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "size is greater than available space"));
			(void) zfs_error(hdl, EZFS_PROPSPACE, errbuf);
			break;

		default:
			(void) zfs_standard_error(hdl, err, errbuf);
			break;
		}
		break;

	case EBUSY:
		(void) zfs_standard_error(hdl, EBUSY, errbuf);
		break;

	case EROFS:
		(void) zfs_error(hdl, EZFS_DSREADONLY, errbuf);
		break;

	case E2BIG:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "property value too long"));
		(void) zfs_error(hdl, EZFS_BADPROP, errbuf);
		break;

	case ENOTSUP:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "pool and or dataset must be upgraded to set this "
		    "property or value"));
		(void) zfs_error(hdl, EZFS_BADVERSION, errbuf);
		break;

	case ERANGE:
		if (prop == ZFS_PROP_COMPRESSION ||
		    prop == ZFS_PROP_DNODESIZE ||
		    prop == ZFS_PROP_RECORDSIZE) {
			(void) zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "property setting is not allowed on "
			    "bootable datasets"));
			(void) zfs_error(hdl, EZFS_NOTSUP, errbuf);
		} else if (prop == ZFS_PROP_CHECKSUM ||
		    prop == ZFS_PROP_DEDUP) {
			(void) zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "property setting is not allowed on "
			    "root pools"));
			(void) zfs_error(hdl, EZFS_NOTSUP, errbuf);
		} else {
			(void) zfs_standard_error(hdl, err, errbuf);
		}
		break;

	case EINVAL:
		if (prop == ZPROP_INVAL) {
			(void) zfs_error(hdl, EZFS_BADPROP, errbuf);
		} else {
			(void) zfs_standard_error(hdl, err, errbuf);
		}
		break;

	case ZFS_ERR_BADPROP:
		(void) zfs_error(hdl, EZFS_BADPROP, errbuf);
		break;

	case EACCES:
		if (prop == ZFS_PROP_KEYLOCATION) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "keylocation may only be set on encryption roots"));
			(void) zfs_error(hdl, EZFS_BADPROP, errbuf);
		} else {
			(void) zfs_standard_error(hdl, err, errbuf);
		}
		break;

	case EOVERFLOW:
		/*
		 * This platform can't address a volume this big.
		 */
#ifdef _ILP32
		if (prop == ZFS_PROP_VOLSIZE) {
			(void) zfs_error(hdl, EZFS_VOLTOOBIG, errbuf);
			break;
		}
		zfs_fallthrough;
#endif
	default:
		(void) zfs_standard_error(hdl, err, errbuf);
	}
}

int
zpool_standard_error(libzfs_handle_t *hdl, int error, const char *msg)
{
	return (zpool_standard_error_fmt(hdl, error, "%s", msg));
}

int
zpool_standard_error_fmt(libzfs_handle_t *hdl, int error, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	if (zfs_common_error(hdl, error, fmt, ap) != 0) {
		va_end(ap);
		return (-1);
	}

	switch (error) {
	case ENODEV:
		zfs_verror(hdl, EZFS_NODEVICE, fmt, ap);
		break;

	case ENOENT:
		zfs_error_aux(hdl,
		    dgettext(TEXT_DOMAIN, "no such pool or dataset"));
		zfs_verror(hdl, EZFS_NOENT, fmt, ap);
		break;

	case EEXIST:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "pool already exists"));
		zfs_verror(hdl, EZFS_EXISTS, fmt, ap);
		break;

	case EBUSY:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "pool is busy"));
		zfs_verror(hdl, EZFS_BUSY, fmt, ap);
		break;

	/* There is no pending operation to cancel */
	case ENOTACTIVE:
		zfs_verror(hdl, EZFS_NO_PENDING, fmt, ap);
		break;

	case ENXIO:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "one or more devices is currently unavailable"));
		zfs_verror(hdl, EZFS_BADDEV, fmt, ap);
		break;

	case ENAMETOOLONG:
		zfs_verror(hdl, EZFS_DEVOVERFLOW, fmt, ap);
		break;

	case ENOTSUP:
		zfs_verror(hdl, EZFS_POOL_NOTSUP, fmt, ap);
		break;

	case EINVAL:
		zfs_verror(hdl, EZFS_POOL_INVALARG, fmt, ap);
		break;

	case ENOSPC:
	case EDQUOT:
		zfs_verror(hdl, EZFS_NOSPC, fmt, ap);
		break;

	case EAGAIN:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "pool I/O is currently suspended"));
		zfs_verror(hdl, EZFS_POOLUNAVAIL, fmt, ap);
		break;

	case EROFS:
		zfs_verror(hdl, EZFS_POOLREADONLY, fmt, ap);
		break;
	case EDOM:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "block size out of range or does not match"));
		zfs_verror(hdl, EZFS_BADPROP, fmt, ap);
		break;
	case EREMOTEIO:
		zfs_verror(hdl, EZFS_ACTIVE_POOL, fmt, ap);
		break;
	case ZFS_ERR_CHECKPOINT_EXISTS:
		zfs_verror(hdl, EZFS_CHECKPOINT_EXISTS, fmt, ap);
		break;
	case ZFS_ERR_DISCARDING_CHECKPOINT:
		zfs_verror(hdl, EZFS_DISCARDING_CHECKPOINT, fmt, ap);
		break;
	case ZFS_ERR_NO_CHECKPOINT:
		zfs_verror(hdl, EZFS_NO_CHECKPOINT, fmt, ap);
		break;
	case ZFS_ERR_DEVRM_IN_PROGRESS:
		zfs_verror(hdl, EZFS_DEVRM_IN_PROGRESS, fmt, ap);
		break;
	case ZFS_ERR_VDEV_TOO_BIG:
		zfs_verror(hdl, EZFS_VDEV_TOO_BIG, fmt, ap);
		break;
	case ZFS_ERR_EXPORT_IN_PROGRESS:
		zfs_verror(hdl, EZFS_EXPORT_IN_PROGRESS, fmt, ap);
		break;
	case ZFS_ERR_RESILVER_IN_PROGRESS:
		zfs_verror(hdl, EZFS_RESILVERING, fmt, ap);
		break;
	case ZFS_ERR_REBUILD_IN_PROGRESS:
		zfs_verror(hdl, EZFS_REBUILDING, fmt, ap);
		break;
	case ZFS_ERR_BADPROP:
		zfs_verror(hdl, EZFS_BADPROP, fmt, ap);
		break;
	case ZFS_ERR_VDEV_NOTSUP:
		zfs_verror(hdl, EZFS_VDEV_NOTSUP, fmt, ap);
		break;
	case ZFS_ERR_IOC_CMD_UNAVAIL:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "the loaded zfs "
		    "module does not support this operation. A reboot may "
		    "be required to enable this operation."));
		zfs_verror(hdl, EZFS_IOC_NOTSUPPORTED, fmt, ap);
		break;
	case ZFS_ERR_IOC_ARG_UNAVAIL:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "the loaded zfs "
		    "module does not support an option for this operation. "
		    "A reboot may be required to enable this option."));
		zfs_verror(hdl, EZFS_IOC_NOTSUPPORTED, fmt, ap);
		break;
	case ZFS_ERR_IOC_ARG_REQUIRED:
	case ZFS_ERR_IOC_ARG_BADTYPE:
		zfs_verror(hdl, EZFS_IOC_NOTSUPPORTED, fmt, ap);
		break;
	case ZFS_ERR_RAIDZ_EXPAND_IN_PROGRESS:
		zfs_verror(hdl, EZFS_RAIDZ_EXPAND_IN_PROGRESS, fmt, ap);
		break;
	case ZFS_ERR_ASHIFT_MISMATCH:
		zfs_verror(hdl, EZFS_ASHIFT_MISMATCH, fmt, ap);
		break;
	default:
		zfs_error_aux(hdl, "%s", zfs_strerror(error));
		zfs_verror(hdl, EZFS_UNKNOWN, fmt, ap);
	}

	va_end(ap);
	return (-1);
}

int
zfs_ioctl(libzfs_handle_t *hdl, int request, zfs_cmd_t *zc)
{
	return (lzc_ioctl_fd(hdl->libzfs_fd, request, zc));
}

/*
 * Display an out of memory error message and abort the current program.
 */
int
no_memory(libzfs_handle_t *hdl)
{
	return (zfs_error(hdl, EZFS_NOMEM, "internal error"));
}

/*
 * A safe form of malloc() which will die if the allocation fails.
 */
void *
zfs_alloc(libzfs_handle_t *hdl, size_t size)
{
	void *data;

	if ((data = calloc(1, size)) == NULL)
		(void) no_memory(hdl);

	return (data);
}

/*
 * A safe form of asprintf() which will die if the allocation fails.
 */
char *
zfs_asprintf(libzfs_handle_t *hdl, const char *fmt, ...)
{
	va_list ap;
	char *ret;
	int err;

	va_start(ap, fmt);

	err = vasprintf(&ret, fmt, ap);

	va_end(ap);

	if (err < 0) {
		(void) no_memory(hdl);
		ret = NULL;
	}

	return (ret);
}

/*
 * A safe form of realloc(), which also zeroes newly allocated space.
 */
void *
zfs_realloc(libzfs_handle_t *hdl, void *ptr, size_t oldsize, size_t newsize)
{
	void *ret;

	if ((ret = realloc(ptr, newsize)) == NULL) {
		(void) no_memory(hdl);
		return (NULL);
	}

	memset((char *)ret + oldsize, 0, newsize - oldsize);
	return (ret);
}

/*
 * A safe form of strdup() which will die if the allocation fails.
 */
char *
zfs_strdup(libzfs_handle_t *hdl, const char *str)
{
	char *ret;

	if ((ret = strdup(str)) == NULL)
		(void) no_memory(hdl);

	return (ret);
}

void
libzfs_print_on_error(libzfs_handle_t *hdl, boolean_t printerr)
{
	hdl->libzfs_printerr = printerr;
}

/*
 * Read lines from an open file descriptor and store them in an array of
 * strings until EOF.  lines[] will be allocated and populated with all the
 * lines read.  All newlines are replaced with NULL terminators for
 * convenience.  lines[] must be freed after use with libzfs_free_str_array().
 *
 * Returns the number of lines read.
 */
static int
libzfs_read_stdout_from_fd(int fd, char **lines[])
{

	FILE *fp;
	int lines_cnt = 0;
	size_t len = 0;
	char *line = NULL;
	char **tmp_lines = NULL, **tmp;

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		close(fd);
		return (0);
	}
	while (getline(&line, &len, fp) != -1) {
		tmp = realloc(tmp_lines, sizeof (*tmp_lines) * (lines_cnt + 1));
		if (tmp == NULL) {
			/* Return the lines we were able to process */
			break;
		}
		tmp_lines = tmp;

		/* Remove newline if not EOF */
		if (line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';

		tmp_lines[lines_cnt] = strdup(line);
		if (tmp_lines[lines_cnt] == NULL)
			break;
		++lines_cnt;
	}
	free(line);
	fclose(fp);
	*lines = tmp_lines;
	return (lines_cnt);
}

static int
libzfs_run_process_impl(const char *path, char *argv[], char *env[], int flags,
    char **lines[], int *lines_cnt)
{
	pid_t pid;
	int error, devnull_fd;
	int link[2];

	/*
	 * Setup a pipe between our child and parent process if we're
	 * reading stdout.
	 */
	if (lines != NULL && pipe2(link, O_NONBLOCK | O_CLOEXEC) == -1)
		return (-EPIPE);

	pid = fork();
	if (pid == 0) {
		/* Child process */
		setpgid(0, 0);
		devnull_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

		if (devnull_fd < 0)
			_exit(-1);

		if (!(flags & STDOUT_VERBOSE) && (lines == NULL))
			(void) dup2(devnull_fd, STDOUT_FILENO);
		else if (lines != NULL) {
			/* Save the output to lines[] */
			dup2(link[1], STDOUT_FILENO);
		}

		if (!(flags & STDERR_VERBOSE))
			(void) dup2(devnull_fd, STDERR_FILENO);

		if (flags & NO_DEFAULT_PATH) {
			if (env == NULL)
				execv(path, argv);
			else
				execve(path, argv, env);
		} else {
			if (env == NULL)
				execvp(path, argv);
			else
				execvpe(path, argv, env);
		}

		_exit(-1);
	} else if (pid > 0) {
		/* Parent process */
		int status;

		while ((error = waitpid(pid, &status, 0)) == -1 &&
		    errno == EINTR)
			;
		if (error < 0 || !WIFEXITED(status))
			return (-1);

		if (lines != NULL) {
			close(link[1]);
			*lines_cnt = libzfs_read_stdout_from_fd(link[0], lines);
		}
		return (WEXITSTATUS(status));
	}

	return (-1);
}

int
libzfs_run_process(const char *path, char *argv[], int flags)
{
	return (libzfs_run_process_impl(path, argv, NULL, flags, NULL, NULL));
}

/*
 * Run a command and store its stdout lines in an array of strings (lines[]).
 * lines[] is allocated and populated for you, and the number of lines is set in
 * lines_cnt.  lines[] must be freed after use with libzfs_free_str_array().
 * All newlines (\n) in lines[] are terminated for convenience.
 */
int
libzfs_run_process_get_stdout(const char *path, char *argv[], char *env[],
    char **lines[], int *lines_cnt)
{
	return (libzfs_run_process_impl(path, argv, env, 0, lines, lines_cnt));
}

/*
 * Same as libzfs_run_process_get_stdout(), but run without $PATH set.  This
 * means that *path needs to be the full path to the executable.
 */
int
libzfs_run_process_get_stdout_nopath(const char *path, char *argv[],
    char *env[], char **lines[], int *lines_cnt)
{
	return (libzfs_run_process_impl(path, argv, env, NO_DEFAULT_PATH,
	    lines, lines_cnt));
}

/*
 * Free an array of strings.  Free both the strings contained in the array and
 * the array itself.
 */
void
libzfs_free_str_array(char **strs, int count)
{
	while (--count >= 0)
		free(strs[count]);

	free(strs);
}

/*
 * Returns 1 if environment variable is set to "YES", "yes", "ON", "on", or
 * a non-zero number.
 *
 * Returns 0 otherwise.
 */
boolean_t
libzfs_envvar_is_set(const char *envvar)
{
	char *env = getenv(envvar);
	return (env && (strtoul(env, NULL, 0) > 0 ||
	    (!strncasecmp(env, "YES", 3) && strnlen(env, 4) == 3) ||
	    (!strncasecmp(env, "ON", 2) && strnlen(env, 3) == 2)));
}

libzfs_handle_t *
libzfs_init(void)
{
	libzfs_handle_t *hdl;
	int error;
	char *env;

	if ((error = libzfs_load_module()) != 0) {
		errno = error;
		return (NULL);
	}

	if ((hdl = calloc(1, sizeof (libzfs_handle_t))) == NULL) {
		return (NULL);
	}

	if (regcomp(&hdl->libzfs_urire, URI_REGEX, 0) != 0) {
		free(hdl);
		return (NULL);
	}

	if ((hdl->libzfs_fd = open(ZFS_DEV, O_RDWR|O_EXCL|O_CLOEXEC)) < 0) {
		free(hdl);
		return (NULL);
	}

	if (libzfs_core_init() != 0) {
		(void) close(hdl->libzfs_fd);
		free(hdl);
		return (NULL);
	}

	zfs_prop_init();
	zpool_prop_init();
	zpool_feature_init();
	vdev_prop_init();
	libzfs_mnttab_init(hdl);
	fletcher_4_init();

	if (getenv("ZFS_PROP_DEBUG") != NULL) {
		hdl->libzfs_prop_debug = B_TRUE;
	}
	if ((env = getenv("ZFS_SENDRECV_MAX_NVLIST")) != NULL) {
		if ((error = zfs_nicestrtonum(hdl, env,
		    &hdl->libzfs_max_nvlist))) {
			errno = error;
			(void) close(hdl->libzfs_fd);
			free(hdl);
			return (NULL);
		}
	} else {
		hdl->libzfs_max_nvlist = (SPA_MAXBLOCKSIZE * 4);
	}

	/*
	 * For testing, remove some settable properties and features
	 */
	if (libzfs_envvar_is_set("ZFS_SYSFS_PROP_SUPPORT_TEST")) {
		zprop_desc_t *proptbl;

		proptbl = zpool_prop_get_table();
		proptbl[ZPOOL_PROP_COMMENT].pd_zfs_mod_supported = B_FALSE;

		proptbl = zfs_prop_get_table();
		proptbl[ZFS_PROP_DNODESIZE].pd_zfs_mod_supported = B_FALSE;

		zfeature_info_t *ftbl = spa_feature_table;
		ftbl[SPA_FEATURE_LARGE_BLOCKS].fi_zfs_mod_supported = B_FALSE;
	}

	return (hdl);
}

void
libzfs_fini(libzfs_handle_t *hdl)
{
	(void) close(hdl->libzfs_fd);
	zpool_free_handles(hdl);
	namespace_clear(hdl);
	libzfs_mnttab_fini(hdl);
	libzfs_core_fini();
	regfree(&hdl->libzfs_urire);
	fletcher_4_fini();
#if LIBFETCH_DYNAMIC
	if (hdl->libfetch != (void *)-1 && hdl->libfetch != NULL)
		(void) dlclose(hdl->libfetch);
	free(hdl->libfetch_load_error);
#endif
	free(hdl);
}

libzfs_handle_t *
zpool_get_handle(zpool_handle_t *zhp)
{
	return (zhp->zpool_hdl);
}

libzfs_handle_t *
zfs_get_handle(zfs_handle_t *zhp)
{
	return (zhp->zfs_hdl);
}

zpool_handle_t *
zfs_get_pool_handle(const zfs_handle_t *zhp)
{
	return (zhp->zpool_hdl);
}

/*
 * Given a name, determine whether or not it's a valid path
 * (starts with '/' or "./").  If so, walk the mnttab trying
 * to match the device number.  If not, treat the path as an
 * fs/vol/snap/bkmark name.
 */
zfs_handle_t *
zfs_path_to_zhandle(libzfs_handle_t *hdl, const char *path, zfs_type_t argtype)
{
	struct stat64 statbuf;
	struct extmnttab entry;

	if (path[0] != '/' && strncmp(path, "./", strlen("./")) != 0) {
		/*
		 * It's not a valid path, assume it's a name of type 'argtype'.
		 */
		return (zfs_open(hdl, path, argtype));
	}

	if (getextmntent(path, &entry, &statbuf) != 0)
		return (NULL);

	if (strcmp(entry.mnt_fstype, MNTTYPE_ZFS) != 0) {
		(void) fprintf(stderr, gettext("'%s': not a ZFS filesystem\n"),
		    path);
		return (NULL);
	}

	return (zfs_open(hdl, entry.mnt_special, ZFS_TYPE_FILESYSTEM));
}

/*
 * Initialize the zc_nvlist_dst member to prepare for receiving an nvlist from
 * an ioctl().
 */
void
zcmd_alloc_dst_nvlist(libzfs_handle_t *hdl, zfs_cmd_t *zc, size_t len)
{
	if (len == 0)
		len = 256 * 1024;
	zc->zc_nvlist_dst_size = len;
	zc->zc_nvlist_dst =
	    (uint64_t)(uintptr_t)zfs_alloc(hdl, zc->zc_nvlist_dst_size);
}

/*
 * Called when an ioctl() which returns an nvlist fails with ENOMEM.  This will
 * expand the nvlist to the size specified in 'zc_nvlist_dst_size', which was
 * filled in by the kernel to indicate the actual required size.
 */
void
zcmd_expand_dst_nvlist(libzfs_handle_t *hdl, zfs_cmd_t *zc)
{
	free((void *)(uintptr_t)zc->zc_nvlist_dst);
	zc->zc_nvlist_dst =
	    (uint64_t)(uintptr_t)zfs_alloc(hdl, zc->zc_nvlist_dst_size);
}

/*
 * Called to free the src and dst nvlists stored in the command structure.
 */
void
zcmd_free_nvlists(zfs_cmd_t *zc)
{
	free((void *)(uintptr_t)zc->zc_nvlist_conf);
	free((void *)(uintptr_t)zc->zc_nvlist_src);
	free((void *)(uintptr_t)zc->zc_nvlist_dst);
	zc->zc_nvlist_conf = 0;
	zc->zc_nvlist_src = 0;
	zc->zc_nvlist_dst = 0;
}

static void
zcmd_write_nvlist_com(libzfs_handle_t *hdl, uint64_t *outnv, uint64_t *outlen,
    nvlist_t *nvl)
{
	char *packed;

	size_t len = fnvlist_size(nvl);
	packed = zfs_alloc(hdl, len);

	verify(nvlist_pack(nvl, &packed, &len, NV_ENCODE_NATIVE, 0) == 0);

	*outnv = (uint64_t)(uintptr_t)packed;
	*outlen = len;
}

void
zcmd_write_conf_nvlist(libzfs_handle_t *hdl, zfs_cmd_t *zc, nvlist_t *nvl)
{
	zcmd_write_nvlist_com(hdl, &zc->zc_nvlist_conf,
	    &zc->zc_nvlist_conf_size, nvl);
}

void
zcmd_write_src_nvlist(libzfs_handle_t *hdl, zfs_cmd_t *zc, nvlist_t *nvl)
{
	zcmd_write_nvlist_com(hdl, &zc->zc_nvlist_src,
	    &zc->zc_nvlist_src_size, nvl);
}

/*
 * Unpacks an nvlist from the ZFS ioctl command structure.
 */
int
zcmd_read_dst_nvlist(libzfs_handle_t *hdl, zfs_cmd_t *zc, nvlist_t **nvlp)
{
	if (nvlist_unpack((void *)(uintptr_t)zc->zc_nvlist_dst,
	    zc->zc_nvlist_dst_size, nvlp, 0) != 0)
		return (no_memory(hdl));

	return (0);
}

/*
 * ================================================================
 * API shared by zfs and zpool property management
 * ================================================================
 */

void
zcmd_print_json(nvlist_t *nvl)
{
	nvlist_print_json(stdout, nvl);
	(void) putchar('\n');
	nvlist_free(nvl);
}

static void
zprop_print_headers(zprop_get_cbdata_t *cbp, zfs_type_t type)
{
	zprop_list_t *pl;
	int i;
	char *title;
	size_t len;

	cbp->cb_first = B_FALSE;
	if (cbp->cb_scripted)
		return;

	/*
	 * Start with the length of the column headers.
	 */
	cbp->cb_colwidths[GET_COL_NAME] = strlen(dgettext(TEXT_DOMAIN, "NAME"));
	cbp->cb_colwidths[GET_COL_PROPERTY] = strlen(dgettext(TEXT_DOMAIN,
	    "PROPERTY"));
	cbp->cb_colwidths[GET_COL_VALUE] = strlen(dgettext(TEXT_DOMAIN,
	    "VALUE"));
	cbp->cb_colwidths[GET_COL_RECVD] = strlen(dgettext(TEXT_DOMAIN,
	    "RECEIVED"));
	cbp->cb_colwidths[GET_COL_SOURCE] = strlen(dgettext(TEXT_DOMAIN,
	    "SOURCE"));

	/* first property is always NAME */
	assert(cbp->cb_proplist->pl_prop ==
	    ((type == ZFS_TYPE_POOL) ? ZPOOL_PROP_NAME :
	    ((type == ZFS_TYPE_VDEV) ? VDEV_PROP_NAME : ZFS_PROP_NAME)));

	/*
	 * Go through and calculate the widths for each column.  For the
	 * 'source' column, we kludge it up by taking the worst-case scenario of
	 * inheriting from the longest name.  This is acceptable because in the
	 * majority of cases 'SOURCE' is the last column displayed, and we don't
	 * use the width anyway.  Note that the 'VALUE' column can be oversized,
	 * if the name of the property is much longer than any values we find.
	 */
	for (pl = cbp->cb_proplist; pl != NULL; pl = pl->pl_next) {
		/*
		 * 'PROPERTY' column
		 */
		if (pl->pl_prop != ZPROP_USERPROP) {
			const char *propname = (type == ZFS_TYPE_POOL) ?
			    zpool_prop_to_name(pl->pl_prop) :
			    ((type == ZFS_TYPE_VDEV) ?
			    vdev_prop_to_name(pl->pl_prop) :
			    zfs_prop_to_name(pl->pl_prop));

			assert(propname != NULL);
			len = strlen(propname);
			if (len > cbp->cb_colwidths[GET_COL_PROPERTY])
				cbp->cb_colwidths[GET_COL_PROPERTY] = len;
		} else {
			assert(pl->pl_user_prop != NULL);
			len = strlen(pl->pl_user_prop);
			if (len > cbp->cb_colwidths[GET_COL_PROPERTY])
				cbp->cb_colwidths[GET_COL_PROPERTY] = len;
		}

		/*
		 * 'VALUE' column.  The first property is always the 'name'
		 * property that was tacked on either by /sbin/zfs's
		 * zfs_do_get() or when calling zprop_expand_list(), so we
		 * ignore its width.  If the user specified the name property
		 * to display, then it will be later in the list in any case.
		 */
		if (pl != cbp->cb_proplist &&
		    pl->pl_width > cbp->cb_colwidths[GET_COL_VALUE])
			cbp->cb_colwidths[GET_COL_VALUE] = pl->pl_width;

		/* 'RECEIVED' column. */
		if (pl != cbp->cb_proplist &&
		    pl->pl_recvd_width > cbp->cb_colwidths[GET_COL_RECVD])
			cbp->cb_colwidths[GET_COL_RECVD] = pl->pl_recvd_width;

		/*
		 * 'NAME' and 'SOURCE' columns
		 */
		if (pl->pl_prop == ((type == ZFS_TYPE_POOL) ? ZPOOL_PROP_NAME :
		    ((type == ZFS_TYPE_VDEV) ? VDEV_PROP_NAME :
		    ZFS_PROP_NAME)) && pl->pl_width >
		    cbp->cb_colwidths[GET_COL_NAME]) {
			cbp->cb_colwidths[GET_COL_NAME] = pl->pl_width;
			cbp->cb_colwidths[GET_COL_SOURCE] = pl->pl_width +
			    strlen(dgettext(TEXT_DOMAIN, "inherited from"));
		}
	}

	/*
	 * Now go through and print the headers.
	 */
	for (i = 0; i < ZFS_GET_NCOLS; i++) {
		switch (cbp->cb_columns[i]) {
		case GET_COL_NAME:
			title = dgettext(TEXT_DOMAIN, "NAME");
			break;
		case GET_COL_PROPERTY:
			title = dgettext(TEXT_DOMAIN, "PROPERTY");
			break;
		case GET_COL_VALUE:
			title = dgettext(TEXT_DOMAIN, "VALUE");
			break;
		case GET_COL_RECVD:
			title = dgettext(TEXT_DOMAIN, "RECEIVED");
			break;
		case GET_COL_SOURCE:
			title = dgettext(TEXT_DOMAIN, "SOURCE");
			break;
		default:
			title = NULL;
		}

		if (title != NULL) {
			if (i == (ZFS_GET_NCOLS - 1) ||
			    cbp->cb_columns[i + 1] == GET_COL_NONE)
				(void) printf("%s", title);
			else
				(void) printf("%-*s  ",
				    cbp->cb_colwidths[cbp->cb_columns[i]],
				    title);
		}
	}
	(void) printf("\n");
}

/*
 * Add property value and source to provided nvlist, according to
 * settings in cb structure. Later to be printed in JSON format.
 */
int
zprop_nvlist_one_property(const char *propname,
    const char *value, zprop_source_t sourcetype, const char *source,
    const char *recvd_value, nvlist_t *nvl, boolean_t as_int)
{
	int ret = 0;
	nvlist_t *src_nv, *prop;
	boolean_t all_numeric = strspn(value, STR_NUMS) == strlen(value);
	src_nv = prop = NULL;

	if ((nvlist_alloc(&prop, NV_UNIQUE_NAME, 0) != 0) ||
	    (nvlist_alloc(&src_nv, NV_UNIQUE_NAME, 0) != 0)) {
		ret = -1;
		goto err;
	}

	if (as_int && all_numeric) {
		uint64_t val;
		sscanf(value, "%lld", (u_longlong_t *)&val);
		if (nvlist_add_uint64(prop, "value", val) != 0) {
			ret = -1;
			goto err;
		}
	} else {
		if (nvlist_add_string(prop, "value", value) != 0) {
			ret = -1;
			goto err;
		}
	}

	switch (sourcetype) {
	case ZPROP_SRC_NONE:
		if (nvlist_add_string(src_nv, "type", "NONE") != 0 ||
		    (nvlist_add_string(src_nv, "data", "-") != 0)) {
			ret = -1;
			goto err;
		}
		break;
	case ZPROP_SRC_DEFAULT:
		if (nvlist_add_string(src_nv, "type", "DEFAULT") != 0 ||
		    (nvlist_add_string(src_nv, "data", "-") != 0)) {
			ret = -1;
			goto err;
		}
		break;
	case ZPROP_SRC_LOCAL:
		if (nvlist_add_string(src_nv, "type", "LOCAL") != 0 ||
		    (nvlist_add_string(src_nv, "data", "-") != 0)) {
			ret = -1;
			goto err;
		}
		break;
	case ZPROP_SRC_TEMPORARY:
		if (nvlist_add_string(src_nv, "type", "TEMPORARY") != 0 ||
		    (nvlist_add_string(src_nv, "data", "-") != 0)) {
			ret = -1;
			goto err;
		}
		break;
	case ZPROP_SRC_INHERITED:
		if (nvlist_add_string(src_nv, "type", "INHERITED") != 0 ||
		    (nvlist_add_string(src_nv, "data", source) != 0)) {
			ret = -1;
			goto err;
		}
		break;
	case ZPROP_SRC_RECEIVED:
		if (nvlist_add_string(src_nv, "type", "RECEIVED") != 0 ||
		    (nvlist_add_string(src_nv, "data",
		    (recvd_value == NULL ? "-" : recvd_value)) != 0)) {
			ret = -1;
			goto err;
		}
		break;
	default:
		assert(!"unhandled zprop_source_t");
		if (nvlist_add_string(src_nv, "type",
		    "unhandled zprop_source_t") != 0) {
			ret = -1;
			goto err;
		}
	}
	if ((nvlist_add_nvlist(prop, "source", src_nv) != 0) ||
	    (nvlist_add_nvlist(nvl, propname, prop)) != 0) {
		ret = -1;
		goto err;
	}
err:
	nvlist_free(src_nv);
	nvlist_free(prop);
	return (ret);
}

/*
 * Display a single line of output, according to the settings in the callback
 * structure.
 */
void
zprop_print_one_property(const char *name, zprop_get_cbdata_t *cbp,
    const char *propname, const char *value, zprop_source_t sourcetype,
    const char *source, const char *recvd_value)
{
	int i;
	const char *str = NULL;
	char buf[128];

	/*
	 * Ignore those source types that the user has chosen to ignore.
	 */
	if ((sourcetype & cbp->cb_sources) == 0)
		return;

	if (cbp->cb_first)
		zprop_print_headers(cbp, cbp->cb_type);

	for (i = 0; i < ZFS_GET_NCOLS; i++) {
		switch (cbp->cb_columns[i]) {
		case GET_COL_NAME:
			str = name;
			break;

		case GET_COL_PROPERTY:
			str = propname;
			break;

		case GET_COL_VALUE:
			str = value;
			break;

		case GET_COL_SOURCE:
			switch (sourcetype) {
			case ZPROP_SRC_NONE:
				str = "-";
				break;

			case ZPROP_SRC_DEFAULT:
				str = "default";
				break;

			case ZPROP_SRC_LOCAL:
				str = "local";
				break;

			case ZPROP_SRC_TEMPORARY:
				str = "temporary";
				break;

			case ZPROP_SRC_INHERITED:
				(void) snprintf(buf, sizeof (buf),
				    "inherited from %s", source);
				str = buf;
				break;
			case ZPROP_SRC_RECEIVED:
				str = "received";
				break;

			default:
				str = NULL;
				assert(!"unhandled zprop_source_t");
			}
			break;

		case GET_COL_RECVD:
			str = (recvd_value == NULL ? "-" : recvd_value);
			break;

		default:
			continue;
		}

		if (i == (ZFS_GET_NCOLS - 1) ||
		    cbp->cb_columns[i + 1] == GET_COL_NONE)
			(void) printf("%s", str);
		else if (cbp->cb_scripted)
			(void) printf("%s\t", str);
		else
			(void) printf("%-*s  ",
			    cbp->cb_colwidths[cbp->cb_columns[i]],
			    str);
	}

	(void) printf("\n");
}

int
zprop_collect_property(const char *name, zprop_get_cbdata_t *cbp,
    const char *propname, const char *value, zprop_source_t sourcetype,
    const char *source, const char *recvd_value, nvlist_t *nvl)
{
	if (cbp->cb_json) {
		if ((sourcetype & cbp->cb_sources) == 0)
			return (0);
		else {
			return (zprop_nvlist_one_property(propname, value,
			    sourcetype, source, recvd_value, nvl,
			    cbp->cb_json_as_int));
		}
	} else {
		zprop_print_one_property(name, cbp,
		    propname, value, sourcetype, source, recvd_value);
		return (0);
	}
}

/*
 * Given a numeric suffix, convert the value into a number of bits that the
 * resulting value must be shifted.
 */
static int
str2shift(libzfs_handle_t *hdl, const char *buf)
{
	const char *ends = "BKMGTPEZ";
	int i, len;

	if (buf[0] == '\0')
		return (0);

	len = strlen(ends);
	for (i = 0; i < len; i++) {
		if (toupper(buf[0]) == ends[i])
			break;
	}
	if (i == len) {
		if (hdl)
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "invalid numeric suffix '%s'"), buf);
		return (-1);
	}

	/*
	 * Allow 'G' = 'GB' = 'GiB', case-insensitively.
	 * However, 'BB' and 'BiB' are disallowed.
	 */
	if (buf[1] == '\0' ||
	    (toupper(buf[0]) != 'B' &&
	    ((toupper(buf[1]) == 'B' && buf[2] == '\0') ||
	    (toupper(buf[1]) == 'I' && toupper(buf[2]) == 'B' &&
	    buf[3] == '\0'))))
		return (10 * i);

	if (hdl)
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "invalid numeric suffix '%s'"), buf);
	return (-1);
}

/*
 * Convert a string of the form '100G' into a real number.  Used when setting
 * properties or creating a volume.  'buf' is used to place an extended error
 * message for the caller to use.
 */
int
zfs_nicestrtonum(libzfs_handle_t *hdl, const char *value, uint64_t *num)
{
	char *end;
	int shift;

	*num = 0;

	/* Check to see if this looks like a number.  */
	if ((value[0] < '0' || value[0] > '9') && value[0] != '.') {
		if (hdl)
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "bad numeric value '%s'"), value);
		return (-1);
	}

	/* Rely on strtoull() to process the numeric portion.  */
	errno = 0;
	*num = strtoull(value, &end, 10);

	/*
	 * Check for ERANGE, which indicates that the value is too large to fit
	 * in a 64-bit value.
	 */
	if (errno == ERANGE) {
		if (hdl)
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "numeric value is too large"));
		return (-1);
	}

	/*
	 * If we have a decimal value, then do the computation with floating
	 * point arithmetic.  Otherwise, use standard arithmetic.
	 */
	if (*end == '.') {
		double fval = strtod(value, &end);

		if ((shift = str2shift(hdl, end)) == -1)
			return (-1);

		fval *= pow(2, shift);

		/*
		 * UINT64_MAX is not exactly representable as a double.
		 * The closest representation is UINT64_MAX + 1, so we
		 * use a >= comparison instead of > for the bounds check.
		 */
		if (fval >= (double)UINT64_MAX) {
			if (hdl)
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "numeric value is too large"));
			return (-1);
		}

		*num = (uint64_t)fval;
	} else {
		if ((shift = str2shift(hdl, end)) == -1)
			return (-1);

		/* Check for overflow */
		if (shift >= 64 || (*num << shift) >> shift != *num) {
			if (hdl)
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "numeric value is too large"));
			return (-1);
		}

		*num <<= shift;
	}

	return (0);
}

/*
 * Given a propname=value nvpair to set, parse any numeric properties
 * (index, boolean, etc) if they are specified as strings and add the
 * resulting nvpair to the returned nvlist.
 *
 * At the DSL layer, all properties are either 64-bit numbers or strings.
 * We want the user to be able to ignore this fact and specify properties
 * as native values (numbers, for example) or as strings (to simplify
 * command line utilities).  This also handles converting index types
 * (compression, checksum, etc) from strings to their on-disk index.
 */
int
zprop_parse_value(libzfs_handle_t *hdl, nvpair_t *elem, int prop,
    zfs_type_t type, nvlist_t *ret, const char **svalp, uint64_t *ivalp,
    const char *errbuf)
{
	data_type_t datatype = nvpair_type(elem);
	zprop_type_t proptype;
	const char *propname;
	const char *value;
	boolean_t isnone = B_FALSE;
	boolean_t isauto = B_FALSE;
	int err = 0;

	if (type == ZFS_TYPE_POOL) {
		proptype = zpool_prop_get_type(prop);
		propname = zpool_prop_to_name(prop);
	} else if (type == ZFS_TYPE_VDEV) {
		proptype = vdev_prop_get_type(prop);
		propname = vdev_prop_to_name(prop);
	} else {
		proptype = zfs_prop_get_type(prop);
		propname = zfs_prop_to_name(prop);
	}

	/*
	 * Convert any properties to the internal DSL value types.
	 */
	*svalp = NULL;
	*ivalp = 0;

	switch (proptype) {
	case PROP_TYPE_STRING:
		if (datatype != DATA_TYPE_STRING) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "'%s' must be a string"), nvpair_name(elem));
			goto error;
		}
		err = nvpair_value_string(elem, svalp);
		if (err != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "'%s' is invalid"), nvpair_name(elem));
			goto error;
		}
		if (strlen(*svalp) >= ZFS_MAXPROPLEN) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "'%s' is too long"), nvpair_name(elem));
			goto error;
		}
		break;

	case PROP_TYPE_NUMBER:
		if (datatype == DATA_TYPE_STRING) {
			(void) nvpair_value_string(elem, &value);
			if (strcmp(value, "none") == 0) {
				isnone = B_TRUE;
			} else if (strcmp(value, "auto") == 0) {
				isauto = B_TRUE;
			} else if (zfs_nicestrtonum(hdl, value, ivalp) != 0) {
				goto error;
			}
		} else if (datatype == DATA_TYPE_UINT64) {
			(void) nvpair_value_uint64(elem, ivalp);
		} else {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "'%s' must be a number"), nvpair_name(elem));
			goto error;
		}

		/*
		 * Quota special: force 'none' and don't allow 0.
		 */
		if ((type & ZFS_TYPE_DATASET) && *ivalp == 0 && !isnone &&
		    (prop == ZFS_PROP_QUOTA || prop == ZFS_PROP_REFQUOTA)) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "use 'none' to disable quota/refquota"));
			goto error;
		}
		/*
		 * Pool dedup table quota; force use of 'none' instead of 0
		 */
		if ((type & ZFS_TYPE_POOL) && *ivalp == 0 &&
		    (!isnone && !isauto) &&
		    prop == ZPOOL_PROP_DEDUP_TABLE_QUOTA) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "use 'none' to disable ddt table quota"));
			goto error;
		}

		/*
		 * Special handling for "*_limit=none". In this case it's not
		 * 0 but UINT64_MAX.
		 */
		if ((type & ZFS_TYPE_DATASET) && isnone &&
		    (prop == ZFS_PROP_FILESYSTEM_LIMIT ||
		    prop == ZFS_PROP_SNAPSHOT_LIMIT)) {
			*ivalp = UINT64_MAX;
		}

		/*
		 * Special handling for "checksum_*=none". In this case it's not
		 * 0 but UINT64_MAX.
		 */
		if ((type & ZFS_TYPE_VDEV) && isnone &&
		    (prop == VDEV_PROP_CHECKSUM_N ||
		    prop == VDEV_PROP_CHECKSUM_T ||
		    prop == VDEV_PROP_IO_N ||
		    prop == VDEV_PROP_IO_T ||
		    prop == VDEV_PROP_SLOW_IO_N ||
		    prop == VDEV_PROP_SLOW_IO_T)) {
			*ivalp = UINT64_MAX;
		}

		/*
		 * Special handling for setting 'refreservation' to 'auto'.  Use
		 * UINT64_MAX to tell the caller to use zfs_fix_auto_resv().
		 * 'auto' is only allowed on volumes.
		 */
		if (isauto) {
			switch (prop) {
			case ZFS_PROP_REFRESERVATION:
				if ((type & ZFS_TYPE_VOLUME) == 0) {
					zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
					    "'%s=auto' only allowed on "
					    "volumes"), nvpair_name(elem));
					goto error;
				}
				*ivalp = UINT64_MAX;
				break;
			case ZPOOL_PROP_DEDUP_TABLE_QUOTA:
				ASSERT(type & ZFS_TYPE_POOL);
				*ivalp = UINT64_MAX;
				break;
			default:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "'auto' is invalid value for '%s'"),
				    nvpair_name(elem));
				goto error;
			}
		}

		break;

	case PROP_TYPE_INDEX:
		if (datatype != DATA_TYPE_STRING) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "'%s' must be a string"), nvpair_name(elem));
			goto error;
		}

		(void) nvpair_value_string(elem, &value);

		if (zprop_string_to_index(prop, value, ivalp, type) != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "'%s' must be one of '%s'"), propname,
			    zprop_values(prop, type));
			goto error;
		}
		break;

	default:
		abort();
	}

	/*
	 * Add the result to our return set of properties.
	 */
	if (*svalp != NULL) {
		if (nvlist_add_string(ret, propname, *svalp) != 0) {
			(void) no_memory(hdl);
			return (-1);
		}
	} else {
		if (nvlist_add_uint64(ret, propname, *ivalp) != 0) {
			(void) no_memory(hdl);
			return (-1);
		}
	}

	return (0);
error:
	(void) zfs_error(hdl, EZFS_BADPROP, errbuf);
	return (-1);
}

static int
addlist(libzfs_handle_t *hdl, const char *propname, zprop_list_t **listp,
    zfs_type_t type)
{
	int prop = zprop_name_to_prop(propname, type);
	if (prop != ZPROP_INVAL && !zprop_valid_for_type(prop, type, B_FALSE))
		prop = ZPROP_INVAL;

	/*
	 * Return failure if no property table entry was found and this isn't
	 * a user-defined property.
	 */
	if (prop == ZPROP_USERPROP && ((type == ZFS_TYPE_POOL &&
	    !zfs_prop_user(propname) &&
	    !zpool_prop_feature(propname) &&
	    !zpool_prop_unsupported(propname)) ||
	    ((type == ZFS_TYPE_DATASET) && !zfs_prop_user(propname) &&
	    !zfs_prop_userquota(propname) && !zfs_prop_written(propname)) ||
	    ((type == ZFS_TYPE_VDEV) && !vdev_prop_user(propname)))) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "invalid property '%s'"), propname);
		return (zfs_error(hdl, EZFS_BADPROP,
		    dgettext(TEXT_DOMAIN, "bad property list")));
	}

	zprop_list_t *entry = zfs_alloc(hdl, sizeof (*entry));

	entry->pl_prop = prop;
	if (prop == ZPROP_USERPROP) {
		entry->pl_user_prop = zfs_strdup(hdl, propname);
		entry->pl_width = strlen(propname);
	} else {
		entry->pl_width = zprop_width(prop, &entry->pl_fixed,
		    type);
	}

	*listp = entry;

	return (0);
}

/*
 * Given a comma-separated list of properties, construct a property list
 * containing both user-defined and native properties.  This function will
 * return a NULL list if 'all' is specified, which can later be expanded
 * by zprop_expand_list().
 */
int
zprop_get_list(libzfs_handle_t *hdl, char *props, zprop_list_t **listp,
    zfs_type_t type)
{
	*listp = NULL;

	/*
	 * If 'all' is specified, return a NULL list.
	 */
	if (strcmp(props, "all") == 0)
		return (0);

	/*
	 * If no props were specified, return an error.
	 */
	if (props[0] == '\0') {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "no properties specified"));
		return (zfs_error(hdl, EZFS_BADPROP, dgettext(TEXT_DOMAIN,
		    "bad property list")));
	}

	for (char *p; (p = strsep(&props, ",")); )
		if (strcmp(p, "space") == 0) {
			static const char *const spaceprops[] = {
				"name", "avail", "used", "usedbysnapshots",
				"usedbydataset", "usedbyrefreservation",
				"usedbychildren"
			};

			for (int i = 0; i < ARRAY_SIZE(spaceprops); i++) {
				if (addlist(hdl, spaceprops[i], listp, type))
					return (-1);
				listp = &(*listp)->pl_next;
			}
		} else {
			if (addlist(hdl, p, listp, type))
				return (-1);
			listp = &(*listp)->pl_next;
		}

	return (0);
}

void
zprop_free_list(zprop_list_t *pl)
{
	zprop_list_t *next;

	while (pl != NULL) {
		next = pl->pl_next;
		free(pl->pl_user_prop);
		free(pl);
		pl = next;
	}
}

typedef struct expand_data {
	zprop_list_t	**last;
	libzfs_handle_t	*hdl;
	zfs_type_t type;
} expand_data_t;

static int
zprop_expand_list_cb(int prop, void *cb)
{
	zprop_list_t *entry;
	expand_data_t *edp = cb;

	entry = zfs_alloc(edp->hdl, sizeof (zprop_list_t));

	entry->pl_prop = prop;
	entry->pl_width = zprop_width(prop, &entry->pl_fixed, edp->type);
	entry->pl_all = B_TRUE;

	*(edp->last) = entry;
	edp->last = &entry->pl_next;

	return (ZPROP_CONT);
}

int
zprop_expand_list(libzfs_handle_t *hdl, zprop_list_t **plp, zfs_type_t type)
{
	zprop_list_t *entry;
	zprop_list_t **last;
	expand_data_t exp;

	if (*plp == NULL) {
		/*
		 * If this is the very first time we've been called for an 'all'
		 * specification, expand the list to include all native
		 * properties.
		 */
		last = plp;

		exp.last = last;
		exp.hdl = hdl;
		exp.type = type;

		if (zprop_iter_common(zprop_expand_list_cb, &exp, B_FALSE,
		    B_FALSE, type) == ZPROP_INVAL)
			return (-1);

		/*
		 * Add 'name' to the beginning of the list, which is handled
		 * specially.
		 */
		entry = zfs_alloc(hdl, sizeof (zprop_list_t));
		entry->pl_prop = ((type == ZFS_TYPE_POOL) ?  ZPOOL_PROP_NAME :
		    ((type == ZFS_TYPE_VDEV) ? VDEV_PROP_NAME : ZFS_PROP_NAME));
		entry->pl_width = zprop_width(entry->pl_prop,
		    &entry->pl_fixed, type);
		entry->pl_all = B_TRUE;
		entry->pl_next = *plp;
		*plp = entry;
	}
	return (0);
}

int
zprop_iter(zprop_func func, void *cb, boolean_t show_all, boolean_t ordered,
    zfs_type_t type)
{
	return (zprop_iter_common(func, cb, show_all, ordered, type));
}

const char *
zfs_version_userland(void)
{
	return (ZFS_META_ALIAS);
}

/*
 * Prints both zfs userland and kernel versions
 * Returns 0 on success, and -1 on error
 */
int
zfs_version_print(void)
{
	(void) puts(ZFS_META_ALIAS);

	char *kver = zfs_version_kernel();
	if (kver == NULL) {
		fprintf(stderr, "zfs_version_kernel() failed: %s\n",
		    zfs_strerror(errno));
		return (-1);
	}

	(void) printf("zfs-kmod-%s\n", kver);
	free(kver);
	return (0);
}

/*
 * Returns an nvlist with both zfs userland and kernel versions.
 * Returns NULL on error.
 */
nvlist_t *
zfs_version_nvlist(void)
{
	nvlist_t *nvl;
	char kmod_ver[64];
	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0)
		return (NULL);
	if (nvlist_add_string(nvl, "userland", ZFS_META_ALIAS) != 0)
		goto err;
	char *kver = zfs_version_kernel();
	if (kver == NULL) {
		fprintf(stderr, "zfs_version_kernel() failed: %s\n",
		    zfs_strerror(errno));
		goto err;
	}
	(void) snprintf(kmod_ver, 64, "zfs-kmod-%s", kver);
	if (nvlist_add_string(nvl, "kernel", kmod_ver) != 0)
		goto err;
	return (nvl);
err:
	nvlist_free(nvl);
	return (NULL);
}

/*
 * zfs_get_hole_count() retrieves the number of holes (blocks which are
 * zero-filled) in the specified file using the FS_IOC_FIEMAP ioctl.  It
 * also optionally fetches the block size when bs is non-NULL.  With hole
 * count and block size the full space consumed by the holes of a file can
 * be calculated.
 *
 * On success, zero is returned, the count argument is set to the number of
 * unallocated blocks (holes), and the bs argument is set to the block size
 * (if it is not NULL). On error, a non-zero errno is returned and the values
 * in count and bs are undefined.
 */
int
zfs_get_hole_count(const char *path, uint64_t *count, uint64_t *bs)
{
	struct fiemap *fiemap;
	struct stat64 ss;
	uint64_t fill;
	int fd, error;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return (errno);

	fiemap = calloc(1, sizeof (struct fiemap));
	if (fiemap == NULL) {
		error = errno;
		(void) close(fd);
		return (error);
	}

	fiemap->fm_start = 0;
	fiemap->fm_length = FIEMAP_MAX_OFFSET;
	fiemap->fm_flags = FIEMAP_FLAG_NOMERGE;
	fiemap->fm_extent_count = 0;
	fiemap->fm_mapped_extents = 0;

	error = ioctl(fd, FS_IOC_FIEMAP, fiemap);
	if (error < 0) {
		error = errno;
		free(fiemap);
		(void) close(fd);
		return (error);
	}

	fill = fiemap->fm_mapped_extents;
	free(fiemap);

	if (fstat64(fd, &ss) == -1) {
		error = errno;
		(void) close(fd);
		return (error);
	}

	if (ss.st_blksize == 0) {
		(void) close(fd);
		return (EINVAL);
	}

	/*
	 * The number of blocks times the block size may exceed the file size
	 * when there are pending dirty blocks which have not be written.
	 */
	*count = (ss.st_size + ss.st_blksize - 1) / ss.st_blksize - fill;
	if ((longlong_t)*count < 0)
		*count = 0;

	if (bs != NULL)
		*bs = ss.st_blksize;

	if (close(fd) == -1)
		return (errno);

	return (0);
}

/*
 * Return 1 if the user requested ANSI color output, and our terminal supports
 * it.  Return 0 for no color.
 */
int
use_color(void)
{
	static int use_color = -1;
	char *term;

	/*
	 * Optimization:
	 *
	 * For each zpool invocation, we do a single check to see if we should
	 * be using color or not, and cache that value for the lifetime of the
	 * the zpool command.  That makes it cheap to call use_color() when
	 * we're printing with color.  We assume that the settings are not going
	 * to change during the invocation of a zpool command (the user isn't
	 * going to change the ZFS_COLOR value while zpool is running, for
	 * example).
	 */
	if (use_color != -1) {
		/*
		 * We've already figured out if we should be using color or
		 * not.  Return the cached value.
		 */
		return (use_color);
	}

	term = getenv("TERM");
	/*
	 * The user sets the ZFS_COLOR env var set to enable zpool ANSI color
	 * output.  However if NO_COLOR is set (https://no-color.org/) then
	 * don't use it.  Also, don't use color if terminal doesn't support
	 * it.
	 */
	if (libzfs_envvar_is_set("ZFS_COLOR") &&
	    !libzfs_envvar_is_set("NO_COLOR") &&
	    isatty(STDOUT_FILENO) && term && strcmp("dumb", term) != 0 &&
	    strcmp("unknown", term) != 0) {
		/* Color supported */
		use_color = 1;
	} else {
		use_color = 0;
	}

	return (use_color);
}

/*
 * The functions color_start() and color_end() are used for when you want
 * to colorize a block of text.
 *
 * For example:
 * color_start(ANSI_RED)
 * printf("hello");
 * printf("world");
 * color_end();
 */
void
color_start(const char *color)
{
	if (color && use_color()) {
		fputs(color, stdout);
		fflush(stdout);
	}
}

void
color_end(void)
{
	if (use_color()) {
		fputs(ANSI_RESET, stdout);
		fflush(stdout);
	}

}

/*
 * printf() with a color. If color is NULL, then do a normal printf.
 */
int
printf_color(const char *color, const char *format, ...)
{
	va_list aptr;
	int rc;

	if (color)
		color_start(color);

	va_start(aptr, format);
	rc = vprintf(format, aptr);
	va_end(aptr);

	if (color)
		color_end();

	return (rc);
}

/* PATH + 5 env vars + a NULL entry = 7 */
#define	ZPOOL_VDEV_SCRIPT_ENV_COUNT 7

/*
 * There's a few places where ZFS will call external scripts (like the script
 * in zpool.d/ and `zfs_prepare_disk`).  These scripts are called with a
 * reduced $PATH, and some vdev specific environment vars set.  This function
 * will allocate an populate the environment variable array that is passed to
 * these scripts.  The user must free the arrays with zpool_vdev_free_env() when
 * they are done.
 *
 * The following env vars will be set (but value could be blank):
 *
 * POOL_NAME
 * VDEV_PATH
 * VDEV_UPATH
 * VDEV_ENC_SYSFS_PATH
 *
 * In addition, you can set an optional environment variable named 'opt_key'
 * to 'opt_val' if you want.
 *
 * Returns allocated env[] array on success, NULL otherwise.
 */
char **
zpool_vdev_script_alloc_env(const char *pool_name,
    const char *vdev_path, const char *vdev_upath,
    const char *vdev_enc_sysfs_path, const char *opt_key, const char *opt_val)
{
	char **env = NULL;
	int rc;

	env = calloc(ZPOOL_VDEV_SCRIPT_ENV_COUNT, sizeof (*env));
	if (!env)
		return (NULL);

	env[0] = strdup("PATH=/bin:/sbin:/usr/bin:/usr/sbin");
	if (!env[0])
		goto error;

	/* Setup our custom environment variables */
	rc = asprintf(&env[1], "POOL_NAME=%s", pool_name ? pool_name : "");
	if (rc == -1) {
		env[1] = NULL;
		goto error;
	}

	rc = asprintf(&env[2], "VDEV_PATH=%s", vdev_path ? vdev_path : "");
	if (rc == -1) {
		env[2] = NULL;
		goto error;
	}

	rc = asprintf(&env[3], "VDEV_UPATH=%s", vdev_upath ? vdev_upath : "");
	if (rc == -1) {
		env[3] = NULL;
		goto error;
	}

	rc = asprintf(&env[4], "VDEV_ENC_SYSFS_PATH=%s",
	    vdev_enc_sysfs_path ?  vdev_enc_sysfs_path : "");
	if (rc == -1) {
		env[4] = NULL;
		goto error;
	}

	if (opt_key != NULL) {
		rc = asprintf(&env[5], "%s=%s", opt_key,
		    opt_val ? opt_val : "");
		if (rc == -1) {
			env[5] = NULL;
			goto error;
		}
	}

	return (env);

error:
	for (int i = 0; i < ZPOOL_VDEV_SCRIPT_ENV_COUNT; i++)
		free(env[i]);

	free(env);

	return (NULL);
}

/*
 * Free the env[] array that was allocated by zpool_vdev_script_alloc_env().
 */
void
zpool_vdev_script_free_env(char **env)
{
	for (int i = 0; i < ZPOOL_VDEV_SCRIPT_ENV_COUNT; i++)
		free(env[i]);

	free(env);
}

/*
 * Prepare a disk by (optionally) running a program before labeling the disk.
 * This can be useful for installing disk firmware or doing some pre-flight
 * checks on the disk before it becomes part of the pool.  The program run is
 * located at ZFSEXECDIR/zfs_prepare_disk
 * (E.x: /usr/local/libexec/zfs/zfs_prepare_disk).
 *
 * Return 0 on success, non-zero on failure.
 */
int
zpool_prepare_disk(zpool_handle_t *zhp, nvlist_t *vdev_nv,
    const char *prepare_str, char **lines[], int *lines_cnt)
{
	const char *script_path = ZFSEXECDIR "/zfs_prepare_disk";
	const char *pool_name;
	int rc = 0;

	/* Path to script and a NULL entry */
	char *argv[2] = {(char *)script_path};
	char **env = NULL;
	const char *path = NULL, *enc_sysfs_path = NULL;
	char *upath;
	*lines_cnt = 0;

	if (access(script_path, X_OK) != 0) {
		/* No script, nothing to do */
		return (0);
	}

	(void) nvlist_lookup_string(vdev_nv, ZPOOL_CONFIG_PATH, &path);
	(void) nvlist_lookup_string(vdev_nv, ZPOOL_CONFIG_VDEV_ENC_SYSFS_PATH,
	    &enc_sysfs_path);

	upath = zfs_get_underlying_path(path);
	pool_name = zhp ? zpool_get_name(zhp) : NULL;

	env = zpool_vdev_script_alloc_env(pool_name, path, upath,
	    enc_sysfs_path, "VDEV_PREPARE", prepare_str);

	free(upath);

	if (env == NULL) {
		return (ENOMEM);
	}

	rc = libzfs_run_process_get_stdout(script_path, argv, env, lines,
	    lines_cnt);

	zpool_vdev_script_free_env(env);

	return (rc);
}

/*
 * Optionally run a script and then label a disk.  The script can be used to
 * prepare a disk for inclusion into the pool.  For example, it might update
 * the disk's firmware or check its health.
 *
 * The 'name' provided is the short name, stripped of any leading
 * /dev path, and is passed to zpool_label_disk. vdev_nv is the nvlist for
 * the vdev.  prepare_str is a string that gets passed as the VDEV_PREPARE
 * env variable to the script.
 *
 * The following env vars are passed to the script:
 *
 * POOL_NAME:		The pool name (blank during zpool create)
 * VDEV_PREPARE:	Reason why the disk is being prepared for inclusion:
 *			"create", "add", "replace", or "autoreplace"
 * VDEV_PATH:		Path to the disk
 * VDEV_UPATH:		One of the 'underlying paths' to the disk.  This is
 * 			useful for DM devices.
 * VDEV_ENC_SYSFS_PATH:	Path to the disk's enclosure sysfs path, if available.
 *
 * Note, some of these values can be blank.
 *
 * Return 0 on success, non-zero otherwise.
 */
int
zpool_prepare_and_label_disk(libzfs_handle_t *hdl, zpool_handle_t *zhp,
    const char *name, nvlist_t *vdev_nv, const char *prepare_str,
    char **lines[], int *lines_cnt)
{
	int rc;
	char vdev_path[MAXPATHLEN];
	(void) snprintf(vdev_path, sizeof (vdev_path), "%s/%s", DISK_ROOT,
	    name);

	/* zhp will be NULL when creating a pool */
	rc = zpool_prepare_disk(zhp, vdev_nv, prepare_str, lines, lines_cnt);
	if (rc != 0)
		return (rc);

	rc = zpool_label_disk(hdl, zhp, name);
	return (rc);
}
