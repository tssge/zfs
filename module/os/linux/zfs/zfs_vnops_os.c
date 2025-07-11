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
 * Copyright (c) 2012, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2015 by Chunwei Chen. All rights reserved.
 * Copyright 2017 Nexenta Systems, Inc.
 */

/* Portions Copyright 2007 Jeremy Teo */
/* Portions Copyright 2010 Robert Milkowski */


#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/kmem.h>
#include <sys/taskq.h>
#include <sys/uio.h>
#include <sys/vmsystm.h>
#include <sys/atomic.h>
#include <sys/pathname.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/policy.h>
#include <sys/sunddi.h>
#include <sys/sid.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_rlock.h>
#include <sys/cred.h>
#include <sys/zpl.h>
#include <sys/zil.h>
#include <sys/sa_impl.h>
#include <linux/mm_compat.h>

/*
 * Programming rules.
 *
 * Each vnode op performs some logical unit of work.  To do this, the ZPL must
 * properly lock its in-core state, create a DMU transaction, do the work,
 * record this work in the intent log (ZIL), commit the DMU transaction,
 * and wait for the intent log to commit if it is a synchronous operation.
 * Moreover, the vnode ops must work in both normal and log replay context.
 * The ordering of events is important to avoid deadlocks and references
 * to freed memory.  The example below illustrates the following Big Rules:
 *
 *  (1) A check must be made in each zfs thread for a mounted file system.
 *	This is done avoiding races using zfs_enter(zfsvfs).
 *      A zfs_exit(zfsvfs) is needed before all returns.  Any znodes
 *      must be checked with zfs_verify_zp(zp).  Both of these macros
 *      can return EIO from the calling function.
 *
 *  (2) zrele() should always be the last thing except for zil_commit() (if
 *	necessary) and zfs_exit(). This is for 3 reasons: First, if it's the
 *	last reference, the vnode/znode can be freed, so the zp may point to
 *	freed memory.  Second, the last reference will call zfs_zinactive(),
 *	which may induce a lot of work -- pushing cached pages (which acquires
 *	range locks) and syncing out cached atime changes.  Third,
 *	zfs_zinactive() may require a new tx, which could deadlock the system
 *	if you were already holding one. This deadlock occurs because the tx
 *	currently being operated on prevents a txg from syncing, which
 *	prevents the new tx from progressing, resulting in a deadlock.  If you
 *	must call zrele() within a tx, use zfs_zrele_async(). Note that iput()
 *	is a synonym for zrele().
 *
 *  (3)	All range locks must be grabbed before calling dmu_tx_assign(),
 *	as they can span dmu_tx_assign() calls.
 *
 *  (4) If ZPL locks are held, pass DMU_TX_NOWAIT as the second argument to
 *      dmu_tx_assign().  This is critical because we don't want to block
 *      while holding locks.
 *
 *	If no ZPL locks are held (aside from zfs_enter()), use DMU_TX_WAIT.
 *	This reduces lock contention and CPU usage when we must wait (note
 *	that if throughput is constrained by the storage, nearly every
 *	transaction must wait).
 *
 *      Note, in particular, that if a lock is sometimes acquired before
 *      the tx assigns, and sometimes after (e.g. z_lock), then failing
 *      to use a non-blocking assign can deadlock the system.  The scenario:
 *
 *	Thread A has grabbed a lock before calling dmu_tx_assign().
 *	Thread B is in an already-assigned tx, and blocks for this lock.
 *	Thread A calls dmu_tx_assign(DMU_TX_WAIT) and blocks in
 *	txg_wait_open() forever, because the previous txg can't quiesce
 *	until B's tx commits.
 *
 *	If dmu_tx_assign() returns ERESTART and zfsvfs->z_assign is
 *	DMU_TX_NOWAIT, then drop all locks, call dmu_tx_wait(), and try
 *	again.  On subsequent calls to dmu_tx_assign(), pass
 *	DMU_TX_NOTHROTTLE in addition to DMU_TX_NOWAIT, to indicate that
 *	this operation has already called dmu_tx_wait().  This will ensure
 *	that we don't retry forever, waiting a short bit each time.
 *
 *  (5)	If the operation succeeded, generate the intent log entry for it
 *	before dropping locks.  This ensures that the ordering of events
 *	in the intent log matches the order in which they actually occurred.
 *	During ZIL replay the zfs_log_* functions will update the sequence
 *	number to indicate the zil transaction has replayed.
 *
 *  (6)	At the end of each vnode op, the DMU tx must always commit,
 *	regardless of whether there were any errors.
 *
 *  (7)	After dropping all locks, invoke zil_commit(zilog, foid)
 *	to ensure that synchronous semantics are provided when necessary.
 *
 * In general, this is how things should be ordered in each vnode op:
 *
 *	zfs_enter(zfsvfs);		// exit if unmounted
 * top:
 *	zfs_dirent_lock(&dl, ...)	// lock directory entry (may igrab())
 *	rw_enter(...);			// grab any other locks you need
 *	tx = dmu_tx_create(...);	// get DMU tx
 *	dmu_tx_hold_*();		// hold each object you might modify
 *	error = dmu_tx_assign(tx,
 *	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
 *	if (error) {
 *		rw_exit(...);		// drop locks
 *		zfs_dirent_unlock(dl);	// unlock directory entry
 *		zrele(...);		// release held znodes
 *		if (error == ERESTART) {
 *			waited = B_TRUE;
 *			dmu_tx_wait(tx);
 *			dmu_tx_abort(tx);
 *			goto top;
 *		}
 *		dmu_tx_abort(tx);	// abort DMU tx
 *		zfs_exit(zfsvfs);	// finished in zfs
 *		return (error);		// really out of space
 *	}
 *	error = do_real_work();		// do whatever this VOP does
 *	if (error == 0)
 *		zfs_log_*(...);		// on success, make ZIL entry
 *	dmu_tx_commit(tx);		// commit DMU tx -- error or not
 *	rw_exit(...);			// drop locks
 *	zfs_dirent_unlock(dl);		// unlock directory entry
 *	zrele(...);			// release held znodes
 *	zil_commit(zilog, foid);	// synchronous when necessary
 *	zfs_exit(zfsvfs);		// finished in zfs
 *	return (error);			// done, report error
 */
int
zfs_open(struct inode *ip, int mode, int flag, cred_t *cr)
{
	(void) cr;
	znode_t	*zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	/* Honor ZFS_APPENDONLY file attribute */
	if (blk_mode_is_open_write(mode) && (zp->z_pflags & ZFS_APPENDONLY) &&
	    ((flag & O_APPEND) == 0)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Keep a count of the synchronous opens in the znode.  On first
	 * synchronous open we must convert all previous async transactions
	 * into sync to keep correct ordering.
	 */
	if (flag & O_SYNC) {
		if (atomic_inc_32_nv(&zp->z_sync_cnt) == 1)
			zil_async_to_sync(zfsvfs->z_log, zp->z_id);
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

int
zfs_close(struct inode *ip, int flag, cred_t *cr)
{
	(void) cr;
	znode_t	*zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	/* Decrement the synchronous opens in the znode */
	if (flag & O_SYNC)
		atomic_dec_32(&zp->z_sync_cnt);

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

#if defined(_KERNEL)

static int zfs_fillpage(struct inode *ip, struct page *pp);

/*
 * When a file is memory mapped, we must keep the IO data synchronized
 * between the DMU cache and the memory mapped pages.  Update all mapped
 * pages with the contents of the coresponding dmu buffer.
 */
void
update_pages(znode_t *zp, int64_t start, int len, objset_t *os)
{
	struct address_space *mp = ZTOI(zp)->i_mapping;
	int64_t off = start & (PAGE_SIZE - 1);

	for (start &= PAGE_MASK; len > 0; start += PAGE_SIZE) {
		uint64_t nbytes = MIN(PAGE_SIZE - off, len);

		struct page *pp = find_lock_page(mp, start >> PAGE_SHIFT);
		if (pp) {
			if (mapping_writably_mapped(mp))
				flush_dcache_page(pp);

			void *pb = kmap(pp);
			int error = dmu_read(os, zp->z_id, start + off,
			    nbytes, pb + off, DMU_READ_PREFETCH);
			kunmap(pp);

			if (error) {
				SetPageError(pp);
				ClearPageUptodate(pp);
			} else {
				ClearPageError(pp);
				SetPageUptodate(pp);

				if (mapping_writably_mapped(mp))
					flush_dcache_page(pp);

				mark_page_accessed(pp);
			}

			unlock_page(pp);
			put_page(pp);
		}

		len -= nbytes;
		off = 0;
	}
}

/*
 * When a file is memory mapped, we must keep the I/O data synchronized
 * between the DMU cache and the memory mapped pages.  Preferentially read
 * from memory mapped pages, otherwise fallback to reading through the dmu.
 */
int
mappedread(znode_t *zp, int nbytes, zfs_uio_t *uio)
{
	struct inode *ip = ZTOI(zp);
	struct address_space *mp = ip->i_mapping;
	int64_t start = uio->uio_loffset;
	int64_t off = start & (PAGE_SIZE - 1);
	int len = nbytes;
	int error = 0;

	for (start &= PAGE_MASK; len > 0; start += PAGE_SIZE) {
		uint64_t bytes = MIN(PAGE_SIZE - off, len);

		struct page *pp = find_lock_page(mp, start >> PAGE_SHIFT);
		if (pp) {

			/*
			 * If filemap_fault() retries there exists a window
			 * where the page will be unlocked and not up to date.
			 * In this case we must try and fill the page.
			 */
			if (unlikely(!PageUptodate(pp))) {
				error = zfs_fillpage(ip, pp);
				if (error) {
					unlock_page(pp);
					put_page(pp);
					return (error);
				}
			}

			ASSERT(PageUptodate(pp) || PageDirty(pp));

			unlock_page(pp);

			void *pb = kmap(pp);
			error = zfs_uiomove(pb + off, bytes, UIO_READ, uio);
			kunmap(pp);

			if (mapping_writably_mapped(mp))
				flush_dcache_page(pp);

			mark_page_accessed(pp);
			put_page(pp);
		} else {
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, bytes, DMU_READ_PREFETCH);
		}

		len -= bytes;
		off = 0;

		if (error)
			break;
	}

	return (error);
}
#endif /* _KERNEL */

static unsigned long zfs_delete_blocks = DMU_MAX_DELETEBLKCNT;

/*
 * Write the bytes to a file.
 *
 *	IN:	zp	- znode of file to be written to
 *		data	- bytes to write
 *		len	- number of bytes to write
 *		pos	- offset to start writing at
 *
 *	OUT:	resid	- remaining bytes to write
 *
 *	RETURN:	0 if success
 *		positive error code if failure.  EIO is	returned
 *		for a short write when residp isn't provided.
 *
 * Timestamps:
 *	zp - ctime|mtime updated if byte count > 0
 */
int
zfs_write_simple(znode_t *zp, const void *data, size_t len,
    loff_t pos, size_t *residp)
{
	fstrans_cookie_t cookie;
	int error;

	struct iovec iov;
	iov.iov_base = (void *)data;
	iov.iov_len = len;

	zfs_uio_t uio;
	zfs_uio_iovec_init(&uio, &iov, 1, pos, UIO_SYSSPACE, len, 0);

	cookie = spl_fstrans_mark();
	error = zfs_write(zp, &uio, 0, kcred);
	spl_fstrans_unmark(cookie);

	if (error == 0) {
		if (residp != NULL)
			*residp = zfs_uio_resid(&uio);
		else if (zfs_uio_resid(&uio) != 0)
			error = SET_ERROR(EIO);
	}

	return (error);
}

static void
zfs_rele_async_task(void *arg)
{
	iput(arg);
}

void
zfs_zrele_async(znode_t *zp)
{
	struct inode *ip = ZTOI(zp);
	objset_t *os = ITOZSB(ip)->z_os;

	ASSERT(atomic_read(&ip->i_count) > 0);
	ASSERT(os != NULL);

	/*
	 * If decrementing the count would put us at 0, we can't do it inline
	 * here, because that would be synchronous. Instead, dispatch an iput
	 * to run later.
	 *
	 * For more information on the dangers of a synchronous iput, see the
	 * header comment of this file.
	 */
	if (!atomic_add_unless(&ip->i_count, -1, 1)) {
		VERIFY(taskq_dispatch(dsl_pool_zrele_taskq(dmu_objset_pool(os)),
		    zfs_rele_async_task, ip, TQ_SLEEP) != TASKQID_INVALID);
	}
}


/*
 * Lookup an entry in a directory, or an extended attribute directory.
 * If it exists, return a held inode reference for it.
 *
 *	IN:	zdp	- znode of directory to search.
 *		nm	- name of entry to lookup.
 *		flags	- LOOKUP_XATTR set if looking for an attribute.
 *		cr	- credentials of caller.
 *		direntflags - directory lookup flags
 *		realpnp - returned pathname.
 *
 *	OUT:	zpp	- znode of located entry, NULL if not found.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	NA
 */
int
zfs_lookup(znode_t *zdp, char *nm, znode_t **zpp, int flags, cred_t *cr,
    int *direntflags, pathname_t *realpnp)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zdp);
	int error = 0;

	/*
	 * Fast path lookup, however we must skip DNLC lookup
	 * for case folding or normalizing lookups because the
	 * DNLC code only stores the passed in name.  This means
	 * creating 'a' and removing 'A' on a case insensitive
	 * file system would work, but DNLC still thinks 'a'
	 * exists and won't let you create it again on the next
	 * pass through fast path.
	 */
	if (!(flags & (LOOKUP_XATTR | FIGNORECASE))) {

		if (!S_ISDIR(ZTOI(zdp)->i_mode)) {
			return (SET_ERROR(ENOTDIR));
		} else if (zdp->z_sa_hdl == NULL) {
			return (SET_ERROR(EIO));
		}

		if (nm[0] == 0 || (nm[0] == '.' && nm[1] == '\0')) {
			error = zfs_fastaccesschk_execute(zdp, cr);
			if (!error) {
				*zpp = zdp;
				zhold(*zpp);
				return (0);
			}
			return (error);
		}
	}

	if ((error = zfs_enter_verify_zp(zfsvfs, zdp, FTAG)) != 0)
		return (error);

	*zpp = NULL;

	if (flags & LOOKUP_XATTR) {
		/*
		 * We don't allow recursive attributes..
		 * Maybe someday we will.
		 */
		if (zdp->z_pflags & ZFS_XATTR) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EINVAL));
		}

		if ((error = zfs_get_xattrdir(zdp, zpp, cr, flags))) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}

		/*
		 * Do we have permission to get into attribute directory?
		 */

		if ((error = zfs_zaccess(*zpp, ACE_EXECUTE, 0,
		    B_TRUE, cr, zfs_init_idmap))) {
			zrele(*zpp);
			*zpp = NULL;
		}

		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (!S_ISDIR(ZTOI(zdp)->i_mode)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ENOTDIR));
	}

	/*
	 * Check accessibility of directory.
	 */

	if ((error = zfs_zaccess(zdp, ACE_EXECUTE, 0, B_FALSE, cr,
	    zfs_init_idmap))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfsvfs->z_utf8 && u8_validate(nm, strlen(nm),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	error = zfs_dirlook(zdp, nm, zpp, flags, direntflags, realpnp);
	if ((error == 0) && (*zpp))
		zfs_znode_update_vfs(*zpp);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Perform a linear search in directory for the name of specific inode.
 * Note we don't pass in the buffer size of name because it's hardcoded to
 * NAME_MAX+1(256) in Linux.
 *
 *	IN:	dzp	- znode of directory to search.
 *		zp	- znode of the target
 *
 *	OUT:	name	- dentry name of the target
 *
 *	RETURN:	0 on success, error code on failure.
 */
int
zfs_get_name(znode_t *dzp, char *name, znode_t *zp)
{
	zfsvfs_t *zfsvfs = ZTOZSB(dzp);
	int error = 0;

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);

	if ((error = zfs_verify_zp(zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/* ctldir should have got their name in zfs_vget */
	if (dzp->z_is_ctldir || zp->z_is_ctldir) {
		zfs_exit(zfsvfs, FTAG);
		return (ENOENT);
	}

	/* buffer len is hardcoded to 256 in Linux kernel */
	error = zap_value_search(zfsvfs->z_os, dzp->z_id, zp->z_id,
	    ZFS_DIRENT_OBJ(-1ULL), name, ZAP_MAXNAMELEN);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Attempt to create a new entry in a directory.  If the entry
 * already exists, truncate the file if permissible, else return
 * an error.  Return the ip of the created or trunc'd file.
 *
 *	IN:	dzp	- znode of directory to put new file entry in.
 *		name	- name of new file entry.
 *		vap	- attributes of new file.
 *		excl	- flag indicating exclusive or non-exclusive mode.
 *		mode	- mode to open file with.
 *		cr	- credentials of caller.
 *		flag	- file flag.
 *		vsecp	- ACL to be set
 *		mnt_ns	- user namespace of the mount
 *
 *	OUT:	zpp	- znode of created or trunc'd entry.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dzp - ctime|mtime updated if new entry created
 *	 zp - ctime|mtime always, atime if new
 */
int
zfs_create(znode_t *dzp, char *name, vattr_t *vap, int excl,
    int mode, znode_t **zpp, cred_t *cr, int flag, vsecattr_t *vsecp,
    zidmap_t *mnt_ns)
{
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = ZTOZSB(dzp);
	zilog_t		*zilog;
	objset_t	*os;
	zfs_dirlock_t	*dl;
	dmu_tx_t	*tx;
	int		error;
	uid_t		uid;
	gid_t		gid;
	zfs_acl_ids_t   acl_ids;
	boolean_t	fuid_dirtied;
	boolean_t	have_acl = B_FALSE;
	boolean_t	waited = B_FALSE;
	boolean_t	skip_acl = (flag & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */

	gid = crgetgid(cr);
	uid = crgetuid(cr);

	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (vsecp || IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if (name == NULL)
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	os = zfsvfs->z_os;
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (vap->va_mask & ATTR_XVATTR) {
		if ((error = secpolicy_xvattr((xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_mode)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

top:
	*zpp = NULL;
	if (*name == '\0') {
		/*
		 * Null component name refers to the directory itself.
		 */
		zhold(dzp);
		zp = dzp;
		dl = NULL;
		error = 0;
	} else {
		/* possible igrab(zp) */
		int zflg = 0;

		if (flag & FIGNORECASE)
			zflg |= ZCILOOK;

		error = zfs_dirent_lock(&dl, dzp, name, &zp, zflg,
		    NULL, NULL);
		if (error) {
			if (have_acl)
				zfs_acl_ids_free(&acl_ids);
			if (strcmp(name, "..") == 0)
				error = SET_ERROR(EISDIR);
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	if (zp == NULL) {
		uint64_t txtype;
		uint64_t projid = ZFS_DEFAULT_PROJID;

		/*
		 * Create a new file object and update the directory
		 * to reference it.
		 */
		if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, skip_acl, cr,
		    mnt_ns))) {
			if (have_acl)
				zfs_acl_ids_free(&acl_ids);
			goto out;
		}

		/*
		 * We only support the creation of regular files in
		 * extended attribute directories.
		 */

		if ((dzp->z_pflags & ZFS_XATTR) && !S_ISREG(vap->va_mode)) {
			if (have_acl)
				zfs_acl_ids_free(&acl_ids);
			error = SET_ERROR(EINVAL);
			goto out;
		}

		if (!have_acl && (error = zfs_acl_ids_create(dzp, 0, vap,
		    cr, vsecp, &acl_ids, mnt_ns)) != 0)
			goto out;
		have_acl = B_TRUE;

		if (S_ISREG(vap->va_mode) || S_ISDIR(vap->va_mode))
			projid = zfs_inherit_projid(dzp);
		if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, projid)) {
			zfs_acl_ids_free(&acl_ids);
			error = SET_ERROR(EDQUOT);
			goto out;
		}

		tx = dmu_tx_create(os);

		dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
		    ZFS_SA_BASE_ATTR_SIZE);

		fuid_dirtied = zfsvfs->z_fuid_dirty;
		if (fuid_dirtied)
			zfs_fuid_txhold(zfsvfs, tx);
		dmu_tx_hold_zap(tx, dzp->z_id, TRUE, name);
		dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
		if (!zfsvfs->z_use_sa &&
		    acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
			dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
			    0, acl_ids.z_aclp->z_acl_bytes);
		}

		error = dmu_tx_assign(tx,
		    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
		if (error) {
			zfs_dirent_unlock(dl);
			if (error == ERESTART) {
				waited = B_TRUE;
				dmu_tx_wait(tx);
				dmu_tx_abort(tx);
				goto top;
			}
			zfs_acl_ids_free(&acl_ids);
			dmu_tx_abort(tx);
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
		zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

		error = zfs_link_create(dl, zp, tx, ZNEW);
		if (error != 0) {
			/*
			 * Since, we failed to add the directory entry for it,
			 * delete the newly created dnode.
			 */
			zfs_znode_delete(zp, tx);
			remove_inode_hash(ZTOI(zp));
			zfs_acl_ids_free(&acl_ids);
			dmu_tx_commit(tx);
			goto out;
		}

		if (fuid_dirtied)
			zfs_fuid_sync(zfsvfs, tx);

		txtype = zfs_log_create_txtype(Z_FILE, vsecp, vap);
		if (flag & FIGNORECASE)
			txtype |= TX_CI;
		zfs_log_create(zilog, tx, txtype, dzp, zp, name,
		    vsecp, acl_ids.z_fuidp, vap);
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_commit(tx);
	} else {
		int aflags = (flag & O_APPEND) ? V_APPEND : 0;

		if (have_acl)
			zfs_acl_ids_free(&acl_ids);

		/*
		 * A directory entry already exists for this name.
		 */
		/*
		 * Can't truncate an existing file if in exclusive mode.
		 */
		if (excl) {
			error = SET_ERROR(EEXIST);
			goto out;
		}
		/*
		 * Can't open a directory for writing.
		 */
		if (S_ISDIR(ZTOI(zp)->i_mode)) {
			error = SET_ERROR(EISDIR);
			goto out;
		}
		/*
		 * Verify requested access to file.
		 */
		if (mode && (error = zfs_zaccess_rwx(zp, mode, aflags, cr,
		    mnt_ns))) {
			goto out;
		}

		mutex_enter(&dzp->z_lock);
		dzp->z_seq++;
		mutex_exit(&dzp->z_lock);

		/*
		 * Truncate regular files if requested.
		 */
		if (S_ISREG(ZTOI(zp)->i_mode) &&
		    (vap->va_mask & ATTR_SIZE) && (vap->va_size == 0)) {
			/* we can't hold any locks when calling zfs_freesp() */
			if (dl) {
				zfs_dirent_unlock(dl);
				dl = NULL;
			}
			error = zfs_freesp(zp, 0, 0, mode, TRUE);
		}
	}
out:

	if (dl)
		zfs_dirent_unlock(dl);

	if (error) {
		if (zp)
			zrele(zp);
	} else {
		zfs_znode_update_vfs(dzp);
		zfs_znode_update_vfs(zp);
		*zpp = zp;
	}

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_tmpfile(struct inode *dip, vattr_t *vap, int excl,
    int mode, struct inode **ipp, cred_t *cr, int flag, vsecattr_t *vsecp,
    zidmap_t *mnt_ns)
{
	(void) excl, (void) mode, (void) flag;
	znode_t		*zp = NULL, *dzp = ITOZ(dip);
	zfsvfs_t	*zfsvfs = ITOZSB(dip);
	objset_t	*os;
	dmu_tx_t	*tx;
	int		error;
	uid_t		uid;
	gid_t		gid;
	zfs_acl_ids_t   acl_ids;
	uint64_t	projid = ZFS_DEFAULT_PROJID;
	boolean_t	fuid_dirtied;
	boolean_t	have_acl = B_FALSE;
	boolean_t	waited = B_FALSE;

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */

	gid = crgetgid(cr);
	uid = crgetuid(cr);

	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (vsecp || IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	os = zfsvfs->z_os;

	if (vap->va_mask & ATTR_XVATTR) {
		if ((error = secpolicy_xvattr((xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_mode)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

top:
	*ipp = NULL;

	/*
	 * Create a new file object and update the directory
	 * to reference it.
	 */
	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr, mnt_ns))) {
		if (have_acl)
			zfs_acl_ids_free(&acl_ids);
		goto out;
	}

	if (!have_acl && (error = zfs_acl_ids_create(dzp, 0, vap,
	    cr, vsecp, &acl_ids, mnt_ns)) != 0)
		goto out;
	have_acl = B_TRUE;

	if (S_ISREG(vap->va_mode) || S_ISDIR(vap->va_mode))
		projid = zfs_inherit_projid(dzp);
	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, projid)) {
		zfs_acl_ids_free(&acl_ids);
		error = SET_ERROR(EDQUOT);
		goto out;
	}

	tx = dmu_tx_create(os);

	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);

	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	if (!zfsvfs->z_use_sa &&
	    acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
		    0, acl_ids.z_aclp->z_acl_bytes);
	}
	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			goto top;
		}
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zfs_mknode(dzp, vap, tx, cr, IS_TMPFILE, &zp, &acl_ids);

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	/* Add to unlinked set */
	zp->z_unlinked = B_TRUE;
	zfs_unlinked_add(zp, tx);
	zfs_acl_ids_free(&acl_ids);
	dmu_tx_commit(tx);
out:

	if (error) {
		if (zp)
			zrele(zp);
	} else {
		zfs_znode_update_vfs(dzp);
		zfs_znode_update_vfs(zp);
		*ipp = ZTOI(zp);
	}

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Remove an entry from a directory.
 *
 *	IN:	dzp	- znode of directory to remove entry from.
 *		name	- name of entry to remove.
 *		cr	- credentials of caller.
 *		flags	- case flags.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	dzp - ctime|mtime
 *	 ip - ctime (if nlink > 0)
 */

static uint64_t null_xattr = 0;

int
zfs_remove(znode_t *dzp, char *name, cred_t *cr, int flags)
{
	znode_t		*zp;
	znode_t		*xzp;
	zfsvfs_t	*zfsvfs = ZTOZSB(dzp);
	zilog_t		*zilog;
	uint64_t	acl_obj, xattr_obj;
	uint64_t	xattr_obj_unlinked = 0;
	uint64_t	obj = 0;
	uint64_t	links;
	zfs_dirlock_t	*dl;
	dmu_tx_t	*tx;
	boolean_t	may_delete_now, delete_now = FALSE;
	boolean_t	unlinked, toobig = FALSE;
	uint64_t	txtype;
	pathname_t	*realnmp = NULL;
	pathname_t	realnm;
	int		error;
	int		zflg = ZEXISTS;
	boolean_t	waited = B_FALSE;

	if (name == NULL)
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (flags & FIGNORECASE) {
		zflg |= ZCILOOK;
		pn_alloc(&realnm);
		realnmp = &realnm;
	}

top:
	xattr_obj = 0;
	xzp = NULL;
	/*
	 * Attempt to lock directory; fail if entry doesn't exist.
	 */
	if ((error = zfs_dirent_lock(&dl, dzp, name, &zp, zflg,
	    NULL, realnmp))) {
		if (realnmp)
			pn_free(realnmp);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess_delete(dzp, zp, cr, zfs_init_idmap))) {
		goto out;
	}

	/*
	 * Need to use rmdir for removing directories.
	 */
	if (S_ISDIR(ZTOI(zp)->i_mode)) {
		error = SET_ERROR(EPERM);
		goto out;
	}

	mutex_enter(&zp->z_lock);
	may_delete_now = atomic_read(&ZTOI(zp)->i_count) == 1 &&
	    !zn_has_cached_data(zp, 0, LLONG_MAX);
	mutex_exit(&zp->z_lock);

	/*
	 * We may delete the znode now, or we may put it in the unlinked set;
	 * it depends on whether we're the last link, and on whether there are
	 * other holds on the inode.  So we dmu_tx_hold() the right things to
	 * allow for either case.
	 */
	obj = zp->z_id;
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, name);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);
	if (may_delete_now) {
		toobig = zp->z_size > zp->z_blksz * zfs_delete_blocks;
		/* if the file is too big, only hold_free a token amount */
		dmu_tx_hold_free(tx, zp->z_id, 0,
		    (toobig ? DMU_MAX_ACCESS : DMU_OBJECT_END));
	}

	/* are there any extended attributes? */
	error = sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
	    &xattr_obj, sizeof (xattr_obj));
	if (error == 0 && xattr_obj) {
		error = zfs_zget(zfsvfs, xattr_obj, &xzp);
		ASSERT0(error);
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		dmu_tx_hold_sa(tx, xzp->z_sa_hdl, B_FALSE);
	}

	mutex_enter(&zp->z_lock);
	if ((acl_obj = zfs_external_acl(zp)) != 0 && may_delete_now)
		dmu_tx_hold_free(tx, acl_obj, 0, DMU_OBJECT_END);
	mutex_exit(&zp->z_lock);

	/* charge as an update -- would be nice not to charge at all */
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);

	/*
	 * Mark this transaction as typically resulting in a net free of space
	 */
	dmu_tx_mark_netfree(tx);

	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		zfs_dirent_unlock(dl);
		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			zrele(zp);
			if (xzp)
				zrele(xzp);
			goto top;
		}
		if (realnmp)
			pn_free(realnmp);
		dmu_tx_abort(tx);
		zrele(zp);
		if (xzp)
			zrele(xzp);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Remove the directory entry.
	 */
	error = zfs_link_destroy(dl, zp, tx, zflg, &unlinked);

	if (error) {
		dmu_tx_commit(tx);
		goto out;
	}

	if (unlinked) {
		/*
		 * Hold z_lock so that we can make sure that the ACL obj
		 * hasn't changed.  Could have been deleted due to
		 * zfs_sa_upgrade().
		 */
		mutex_enter(&zp->z_lock);
		(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
		    &xattr_obj_unlinked, sizeof (xattr_obj_unlinked));
		delete_now = may_delete_now && !toobig &&
		    atomic_read(&ZTOI(zp)->i_count) == 1 &&
		    !zn_has_cached_data(zp, 0, LLONG_MAX) &&
		    xattr_obj == xattr_obj_unlinked &&
		    zfs_external_acl(zp) == acl_obj;
		VERIFY_IMPLY(xattr_obj_unlinked, xzp);
	}

	if (delete_now) {
		if (xattr_obj_unlinked) {
			ASSERT3U(ZTOI(xzp)->i_nlink, ==, 2);
			mutex_enter(&xzp->z_lock);
			xzp->z_unlinked = B_TRUE;
			clear_nlink(ZTOI(xzp));
			links = 0;
			error = sa_update(xzp->z_sa_hdl, SA_ZPL_LINKS(zfsvfs),
			    &links, sizeof (links), tx);
			ASSERT3U(error,  ==,  0);
			mutex_exit(&xzp->z_lock);
			zfs_unlinked_add(xzp, tx);

			if (zp->z_is_sa)
				error = sa_remove(zp->z_sa_hdl,
				    SA_ZPL_XATTR(zfsvfs), tx);
			else
				error = sa_update(zp->z_sa_hdl,
				    SA_ZPL_XATTR(zfsvfs), &null_xattr,
				    sizeof (uint64_t), tx);
			ASSERT0(error);
		}
		/*
		 * Add to the unlinked set because a new reference could be
		 * taken concurrently resulting in a deferred destruction.
		 */
		zfs_unlinked_add(zp, tx);
		mutex_exit(&zp->z_lock);
	} else if (unlinked) {
		mutex_exit(&zp->z_lock);
		zfs_unlinked_add(zp, tx);
	}

	txtype = TX_REMOVE;
	if (flags & FIGNORECASE)
		txtype |= TX_CI;
	zfs_log_remove(zilog, tx, txtype, dzp, name, obj, unlinked);

	dmu_tx_commit(tx);
out:
	if (realnmp)
		pn_free(realnmp);

	zfs_dirent_unlock(dl);
	zfs_znode_update_vfs(dzp);
	zfs_znode_update_vfs(zp);

	if (delete_now)
		zrele(zp);
	else
		zfs_zrele_async(zp);

	if (xzp) {
		zfs_znode_update_vfs(xzp);
		zfs_zrele_async(xzp);
	}

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Create a new directory and insert it into dzp using the name
 * provided.  Return a pointer to the inserted directory.
 *
 *	IN:	dzp	- znode of directory to add subdir to.
 *		dirname	- name of new directory.
 *		vap	- attributes of new directory.
 *		cr	- credentials of caller.
 *		flags	- case flags.
 *		vsecp	- ACL to be set
 *		mnt_ns	- user namespace of the mount
 *
 *	OUT:	zpp	- znode of created directory.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	dzp - ctime|mtime updated
 *	zpp - ctime|mtime|atime updated
 */
int
zfs_mkdir(znode_t *dzp, char *dirname, vattr_t *vap, znode_t **zpp,
    cred_t *cr, int flags, vsecattr_t *vsecp, zidmap_t *mnt_ns)
{
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = ZTOZSB(dzp);
	zilog_t		*zilog;
	zfs_dirlock_t	*dl;
	uint64_t	txtype;
	dmu_tx_t	*tx;
	int		error;
	int		zf = ZNEW;
	uid_t		uid;
	gid_t		gid = crgetgid(cr);
	zfs_acl_ids_t   acl_ids;
	boolean_t	fuid_dirtied;
	boolean_t	waited = B_FALSE;

	ASSERT(S_ISDIR(vap->va_mode));

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */

	uid = crgetuid(cr);
	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (vsecp || IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if (dirname == NULL)
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (dzp->z_pflags & ZFS_XATTR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	if (zfsvfs->z_utf8 && u8_validate(dirname,
	    strlen(dirname), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}
	if (flags & FIGNORECASE)
		zf |= ZCILOOK;

	if (vap->va_mask & ATTR_XVATTR) {
		if ((error = secpolicy_xvattr((xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_mode)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	if ((error = zfs_acl_ids_create(dzp, 0, vap, cr,
	    vsecp, &acl_ids, mnt_ns)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	/*
	 * First make sure the new directory doesn't exist.
	 *
	 * Existence is checked first to make sure we don't return
	 * EACCES instead of EEXIST which can cause some applications
	 * to fail.
	 */
top:
	*zpp = NULL;

	if ((error = zfs_dirent_lock(&dl, dzp, dirname, &zp, zf,
	    NULL, NULL))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess(dzp, ACE_ADD_SUBDIRECTORY, 0, B_FALSE, cr,
	    mnt_ns))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_dirent_unlock(dl);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, zfs_inherit_projid(dzp))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_dirent_unlock(dl);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EDQUOT));
	}

	/*
	 * Add a new entry to the directory.
	 */
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, dirname);
	dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, FALSE, NULL);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	if (!zfsvfs->z_use_sa && acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
		    acl_ids.z_aclp->z_acl_bytes);
	}

	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE);

	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		zfs_dirent_unlock(dl);
		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			goto top;
		}
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Create new node.
	 */
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	/*
	 * Now put new name in parent dir.
	 */
	error = zfs_link_create(dl, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		remove_inode_hash(ZTOI(zp));
		goto out;
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	*zpp = zp;

	txtype = zfs_log_create_txtype(Z_DIR, vsecp, vap);
	if (flags & FIGNORECASE)
		txtype |= TX_CI;
	zfs_log_create(zilog, tx, txtype, dzp, zp, dirname, vsecp,
	    acl_ids.z_fuidp, vap);

out:
	zfs_acl_ids_free(&acl_ids);

	dmu_tx_commit(tx);

	zfs_dirent_unlock(dl);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	if (error != 0) {
		zrele(zp);
	} else {
		zfs_znode_update_vfs(dzp);
		zfs_znode_update_vfs(zp);
	}
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Remove a directory subdir entry.  If the current working
 * directory is the same as the subdir to be removed, the
 * remove will fail.
 *
 *	IN:	dzp	- znode of directory to remove from.
 *		name	- name of directory to be removed.
 *		cwd	- inode of current working directory.
 *		cr	- credentials of caller.
 *		flags	- case flags
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dzp - ctime|mtime updated
 */
int
zfs_rmdir(znode_t *dzp, char *name, znode_t *cwd, cred_t *cr,
    int flags)
{
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = ZTOZSB(dzp);
	zilog_t		*zilog;
	zfs_dirlock_t	*dl;
	dmu_tx_t	*tx;
	int		error;
	int		zflg = ZEXISTS;
	boolean_t	waited = B_FALSE;

	if (name == NULL)
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (flags & FIGNORECASE)
		zflg |= ZCILOOK;
top:
	zp = NULL;

	/*
	 * Attempt to lock directory; fail if entry doesn't exist.
	 */
	if ((error = zfs_dirent_lock(&dl, dzp, name, &zp, zflg,
	    NULL, NULL))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess_delete(dzp, zp, cr, zfs_init_idmap))) {
		goto out;
	}

	if (!S_ISDIR(ZTOI(zp)->i_mode)) {
		error = SET_ERROR(ENOTDIR);
		goto out;
	}

	if (zp == cwd) {
		error = SET_ERROR(EINVAL);
		goto out;
	}

	/*
	 * Grab a lock on the directory to make sure that no one is
	 * trying to add (or lookup) entries while we are removing it.
	 */
	rw_enter(&zp->z_name_lock, RW_WRITER);

	/*
	 * Grab a lock on the parent pointer to make sure we play well
	 * with the treewalk and directory rename code.
	 */
	rw_enter(&zp->z_parent_lock, RW_WRITER);

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, name);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		rw_exit(&zp->z_parent_lock);
		rw_exit(&zp->z_name_lock);
		zfs_dirent_unlock(dl);
		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			zrele(zp);
			goto top;
		}
		dmu_tx_abort(tx);
		zrele(zp);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	error = zfs_link_destroy(dl, zp, tx, zflg, NULL);

	if (error == 0) {
		uint64_t txtype = TX_RMDIR;
		if (flags & FIGNORECASE)
			txtype |= TX_CI;
		zfs_log_remove(zilog, tx, txtype, dzp, name, ZFS_NO_OBJECT,
		    B_FALSE);
	}

	dmu_tx_commit(tx);

	rw_exit(&zp->z_parent_lock);
	rw_exit(&zp->z_name_lock);
out:
	zfs_dirent_unlock(dl);

	zfs_znode_update_vfs(dzp);
	zfs_znode_update_vfs(zp);
	zrele(zp);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Read directory entries from the given directory cursor position and emit
 * name and position for each entry.
 *
 *	IN:	ip	- inode of directory to read.
 *		ctx	- directory entry context.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	ip - atime updated
 *
 * Note that the low 4 bits of the cookie returned by zap is always zero.
 * This allows us to use the low range for "special" directory entries:
 * We use 0 for '.', and 1 for '..'.  If this is the root of the filesystem,
 * we use the offset 2 for the '.zfs' directory.
 */
int
zfs_readdir(struct inode *ip, struct dir_context *ctx, cred_t *cr)
{
	(void) cr;
	znode_t		*zp = ITOZ(ip);
	zfsvfs_t	*zfsvfs = ITOZSB(ip);
	objset_t	*os;
	zap_cursor_t	zc;
	zap_attribute_t	*zap;
	int		error;
	uint8_t		prefetch;
	uint8_t		type;
	int		done = 0;
	uint64_t	parent;
	uint64_t	offset; /* must be unsigned; checks for < 1 */

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (parent))) != 0)
		goto out;

	/*
	 * Quit if directory has been removed (posix)
	 */
	if (zp->z_unlinked)
		goto out;

	error = 0;
	os = zfsvfs->z_os;
	offset = ctx->pos;
	prefetch = zp->z_zn_prefetch;
	zap = zap_attribute_long_alloc();

	/*
	 * Initialize the iterator cursor.
	 */
	if (offset <= 3) {
		/*
		 * Start iteration from the beginning of the directory.
		 */
		zap_cursor_init(&zc, os, zp->z_id);
	} else {
		/*
		 * The offset is a serialized cursor.
		 */
		zap_cursor_init_serialized(&zc, os, zp->z_id, offset);
	}

	/*
	 * Transform to file-system independent format
	 */
	while (!done) {
		uint64_t objnum;
		/*
		 * Special case `.', `..', and `.zfs'.
		 */
		if (offset == 0) {
			(void) strcpy(zap->za_name, ".");
			zap->za_normalization_conflict = 0;
			objnum = zp->z_id;
			type = DT_DIR;
		} else if (offset == 1) {
			(void) strcpy(zap->za_name, "..");
			zap->za_normalization_conflict = 0;
			objnum = parent;
			type = DT_DIR;
		} else if (offset == 2 && zfs_show_ctldir(zp)) {
			(void) strcpy(zap->za_name, ZFS_CTLDIR_NAME);
			zap->za_normalization_conflict = 0;
			objnum = ZFSCTL_INO_ROOT;
			type = DT_DIR;
		} else {
			/*
			 * Grab next entry.
			 */
			if ((error = zap_cursor_retrieve(&zc, zap))) {
				if (error == ENOENT)
					break;
				else
					goto update;
			}

			/*
			 * Allow multiple entries provided the first entry is
			 * the object id.  Non-zpl consumers may safely make
			 * use of the additional space.
			 *
			 * XXX: This should be a feature flag for compatibility
			 */
			if (zap->za_integer_length != 8 ||
			    zap->za_num_integers == 0) {
				cmn_err(CE_WARN, "zap_readdir: bad directory "
				    "entry, obj = %lld, offset = %lld, "
				    "length = %d, num = %lld\n",
				    (u_longlong_t)zp->z_id,
				    (u_longlong_t)offset,
				    zap->za_integer_length,
				    (u_longlong_t)zap->za_num_integers);
				error = SET_ERROR(ENXIO);
				goto update;
			}

			objnum = ZFS_DIRENT_OBJ(zap->za_first_integer);
			type = ZFS_DIRENT_TYPE(zap->za_first_integer);
		}

		done = !dir_emit(ctx, zap->za_name, strlen(zap->za_name),
		    objnum, type);
		if (done)
			break;

		if (prefetch)
			dmu_prefetch_dnode(os, objnum, ZIO_PRIORITY_SYNC_READ);

		/*
		 * Move to the next entry, fill in the previous offset.
		 */
		if (offset > 2 || (offset == 2 && !zfs_show_ctldir(zp))) {
			zap_cursor_advance(&zc);
			offset = zap_cursor_serialize(&zc);
		} else {
			offset += 1;
		}
		ctx->pos = offset;
	}
	zp->z_zn_prefetch = B_FALSE; /* a lookup will re-enable pre-fetching */

update:
	zap_cursor_fini(&zc);
	zap_attribute_free(zap);
	if (error == ENOENT)
		error = 0;
out:
	zfs_exit(zfsvfs, FTAG);

	return (error);
}

/*
 * Get the basic file attributes and place them in the provided kstat
 * structure.  The inode is assumed to be the authoritative source
 * for most of the attributes.  However, the znode currently has the
 * authoritative atime, blksize, and block count.
 *
 *	IN:	ip	- inode of file.
 *
 *	OUT:	sp	- kstat values.
 *
 *	RETURN:	0 (always succeeds)
 */
int
#ifdef HAVE_GENERIC_FILLATTR_IDMAP_REQMASK
zfs_getattr_fast(zidmap_t *user_ns, u32 request_mask, struct inode *ip,
    struct kstat *sp)
#else
zfs_getattr_fast(zidmap_t *user_ns, struct inode *ip, struct kstat *sp)
#endif
{
	znode_t *zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	uint32_t blksize;
	u_longlong_t nblocks;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	mutex_enter(&zp->z_lock);

#ifdef HAVE_GENERIC_FILLATTR_IDMAP_REQMASK
	zpl_generic_fillattr(user_ns, request_mask, ip, sp);
#else
	zpl_generic_fillattr(user_ns, ip, sp);
#endif
	/*
	 * +1 link count for root inode with visible '.zfs' directory.
	 */
	if ((zp->z_id == zfsvfs->z_root) && zfs_show_ctldir(zp))
		if (sp->nlink < ZFS_LINK_MAX)
			sp->nlink++;

	sa_object_size(zp->z_sa_hdl, &blksize, &nblocks);
	sp->blksize = blksize;
	sp->blocks = nblocks;

	if (unlikely(zp->z_blksz == 0)) {
		/*
		 * Block size hasn't been set; suggest maximal I/O transfers.
		 */
		sp->blksize = zfsvfs->z_max_blksz;
	}

	mutex_exit(&zp->z_lock);

	/*
	 * Required to prevent NFS client from detecting different inode
	 * numbers of snapshot root dentry before and after snapshot mount.
	 */
	if (zfsvfs->z_issnap) {
		if (ip->i_sb->s_root->d_inode == ip)
			sp->ino = ZFSCTL_INO_SNAPDIRS -
			    dmu_objset_id(zfsvfs->z_os);
	}

	zfs_exit(zfsvfs, FTAG);

	return (0);
}

/*
 * For the operation of changing file's user/group/project, we need to
 * handle not only the main object that is assigned to the file directly,
 * but also the ones that are used by the file via hidden xattr directory.
 *
 * Because the xattr directory may contains many EA entries, as to it may
 * be impossible to change all of them via the transaction of changing the
 * main object's user/group/project attributes. Then we have to change them
 * via other multiple independent transactions one by one. It may be not good
 * solution, but we have no better idea yet.
 */
static int
zfs_setattr_dir(znode_t *dzp)
{
	struct inode	*dxip = ZTOI(dzp);
	struct inode	*xip = NULL;
	zfsvfs_t	*zfsvfs = ZTOZSB(dzp);
	objset_t	*os = zfsvfs->z_os;
	zap_cursor_t	zc;
	zap_attribute_t	*zap;
	zfs_dirlock_t	*dl;
	znode_t		*zp = NULL;
	dmu_tx_t	*tx = NULL;
	uint64_t	uid, gid;
	sa_bulk_attr_t	bulk[4];
	int		count;
	int		err;

	zap = zap_attribute_alloc();
	zap_cursor_init(&zc, os, dzp->z_id);
	while ((err = zap_cursor_retrieve(&zc, zap)) == 0) {
		count = 0;
		if (zap->za_integer_length != 8 || zap->za_num_integers != 1) {
			err = ENXIO;
			break;
		}

		err = zfs_dirent_lock(&dl, dzp, (char *)zap->za_name, &zp,
		    ZEXISTS, NULL, NULL);
		if (err == ENOENT)
			goto next;
		if (err)
			break;

		xip = ZTOI(zp);
		if (KUID_TO_SUID(xip->i_uid) == KUID_TO_SUID(dxip->i_uid) &&
		    KGID_TO_SGID(xip->i_gid) == KGID_TO_SGID(dxip->i_gid) &&
		    zp->z_projid == dzp->z_projid)
			goto next;

		tx = dmu_tx_create(os);
		if (!(zp->z_pflags & ZFS_PROJID))
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		else
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);

		err = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (err)
			break;

		mutex_enter(&dzp->z_lock);

		if (KUID_TO_SUID(xip->i_uid) != KUID_TO_SUID(dxip->i_uid)) {
			xip->i_uid = dxip->i_uid;
			uid = zfs_uid_read(dxip);
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
			    &uid, sizeof (uid));
		}

		if (KGID_TO_SGID(xip->i_gid) != KGID_TO_SGID(dxip->i_gid)) {
			xip->i_gid = dxip->i_gid;
			gid = zfs_gid_read(dxip);
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
			    &gid, sizeof (gid));
		}


		uint64_t projid = dzp->z_projid;
		if (zp->z_projid != projid) {
			if (!(zp->z_pflags & ZFS_PROJID)) {
				err = sa_add_projid(zp->z_sa_hdl, tx, projid);
				if (unlikely(err == EEXIST)) {
					err = 0;
				} else if (err != 0) {
					goto sa_add_projid_err;
				} else {
					projid = ZFS_INVALID_PROJID;
				}
			}

			if (projid != ZFS_INVALID_PROJID) {
				zp->z_projid = projid;
				SA_ADD_BULK_ATTR(bulk, count,
				    SA_ZPL_PROJID(zfsvfs), NULL, &zp->z_projid,
				    sizeof (zp->z_projid));
			}
		}

sa_add_projid_err:
		mutex_exit(&dzp->z_lock);

		if (likely(count > 0)) {
			err = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
			dmu_tx_commit(tx);
		} else if (projid == ZFS_INVALID_PROJID) {
			dmu_tx_commit(tx);
		} else {
			dmu_tx_abort(tx);
		}
		tx = NULL;
		if (err != 0 && err != ENOENT)
			break;

next:
		if (zp) {
			zrele(zp);
			zp = NULL;
			zfs_dirent_unlock(dl);
		}
		zap_cursor_advance(&zc);
	}

	if (tx)
		dmu_tx_abort(tx);
	if (zp) {
		zrele(zp);
		zfs_dirent_unlock(dl);
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(zap);

	return (err == ENOENT ? 0 : err);
}

/*
 * Set the file attributes to the values contained in the
 * vattr structure.
 *
 *	IN:	zp	- znode of file to be modified.
 *		vap	- new attribute values.
 *			  If ATTR_XVATTR set, then optional attrs are being set
 *		flags	- ATTR_UTIME set if non-default time values provided.
 *			- ATTR_NOACLCHECK (CIFS context only).
 *		cr	- credentials of caller.
 *		mnt_ns	- user namespace of the mount
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	ip - ctime updated, mtime updated if size changed.
 */
int
zfs_setattr(znode_t *zp, vattr_t *vap, int flags, cred_t *cr, zidmap_t *mnt_ns)
{
	struct inode	*ip;
	zfsvfs_t	*zfsvfs = ZTOZSB(zp);
	objset_t	*os;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	vattr_t		oldva;
	xvattr_t	*tmpxvattr;
	uint_t		mask = vap->va_mask;
	uint_t		saved_mask = 0;
	int		trim_mask = 0;
	uint64_t	new_mode;
	uint64_t	new_kuid = 0, new_kgid = 0, new_uid, new_gid;
	uint64_t	xattr_obj;
	uint64_t	mtime[2], ctime[2], atime[2];
	uint64_t	projid = ZFS_INVALID_PROJID;
	znode_t		*attrzp;
	int		need_policy = FALSE;
	int		err, err2 = 0;
	zfs_fuid_info_t *fuidp = NULL;
	xvattr_t *xvap = (xvattr_t *)vap;	/* vap may be an xvattr_t * */
	xoptattr_t	*xoap;
	zfs_acl_t	*aclp;
	boolean_t skipaclchk = (flags & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	boolean_t	fuid_dirtied = B_FALSE;
	boolean_t	handle_eadir = B_FALSE;
	sa_bulk_attr_t	*bulk, *xattr_bulk;
	int		count = 0, xattr_count = 0, bulks = 8;

	if (mask == 0)
		return (0);

	if ((err = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (err);
	ip = ZTOI(zp);
	os = zfsvfs->z_os;

	/*
	 * If this is a xvattr_t, then get a pointer to the structure of
	 * optional attributes.  If this is NULL, then we have a vattr_t.
	 */
	xoap = xva_getxoptattr(xvap);
	if (xoap != NULL && (mask & ATTR_XVATTR)) {
		if (XVA_ISSET_REQ(xvap, XAT_PROJID)) {
			if (!dmu_objset_projectquota_enabled(os) ||
			    (!S_ISREG(ip->i_mode) && !S_ISDIR(ip->i_mode))) {
				zfs_exit(zfsvfs, FTAG);
				return (SET_ERROR(ENOTSUP));
			}

			projid = xoap->xoa_projid;
			if (unlikely(projid == ZFS_INVALID_PROJID)) {
				zfs_exit(zfsvfs, FTAG);
				return (SET_ERROR(EINVAL));
			}

			if (projid == zp->z_projid && zp->z_pflags & ZFS_PROJID)
				projid = ZFS_INVALID_PROJID;
			else
				need_policy = TRUE;
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT) &&
		    (xoap->xoa_projinherit !=
		    ((zp->z_pflags & ZFS_PROJINHERIT) != 0)) &&
		    (!dmu_objset_projectquota_enabled(os) ||
		    (!S_ISREG(ip->i_mode) && !S_ISDIR(ip->i_mode)))) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(ENOTSUP));
		}
	}

	zilog = zfsvfs->z_log;

	/*
	 * Make sure that if we have ephemeral uid/gid or xvattr specified
	 * that file system is at proper version level
	 */

	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (((mask & ATTR_UID) && IS_EPHEMERAL(vap->va_uid)) ||
	    ((mask & ATTR_GID) && IS_EPHEMERAL(vap->va_gid)) ||
	    (mask & ATTR_XVATTR))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	if (mask & ATTR_SIZE && S_ISDIR(ip->i_mode)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EISDIR));
	}

	if (mask & ATTR_SIZE && !S_ISREG(ip->i_mode) && !S_ISFIFO(ip->i_mode)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	tmpxvattr = kmem_alloc(sizeof (xvattr_t), KM_SLEEP);
	xva_init(tmpxvattr);

	bulk = kmem_alloc(sizeof (sa_bulk_attr_t) * bulks, KM_SLEEP);
	xattr_bulk = kmem_alloc(sizeof (sa_bulk_attr_t) * bulks, KM_SLEEP);

	/*
	 * Immutable files can only alter immutable bit and atime
	 */
	if ((zp->z_pflags & ZFS_IMMUTABLE) &&
	    ((mask & (ATTR_SIZE|ATTR_UID|ATTR_GID|ATTR_MTIME|ATTR_MODE)) ||
	    ((mask & ATTR_XVATTR) && XVA_ISSET_REQ(xvap, XAT_CREATETIME)))) {
		err = SET_ERROR(EPERM);
		goto out3;
	}

	if ((mask & ATTR_SIZE) && (zp->z_pflags & ZFS_READONLY)) {
		err = SET_ERROR(EPERM);
		goto out3;
	}

	/*
	 * Verify timestamps doesn't overflow 32 bits.
	 * ZFS can handle large timestamps, but 32bit syscalls can't
	 * handle times greater than 2039.  This check should be removed
	 * once large timestamps are fully supported.
	 */
	if (mask & (ATTR_ATIME | ATTR_MTIME)) {
		if (((mask & ATTR_ATIME) &&
		    TIMESPEC_OVERFLOW(&vap->va_atime)) ||
		    ((mask & ATTR_MTIME) &&
		    TIMESPEC_OVERFLOW(&vap->va_mtime))) {
			err = SET_ERROR(EOVERFLOW);
			goto out3;
		}
	}

top:
	attrzp = NULL;
	aclp = NULL;

	/* Can this be moved to before the top label? */
	if (zfs_is_readonly(zfsvfs)) {
		err = SET_ERROR(EROFS);
		goto out3;
	}

	/*
	 * First validate permissions
	 */

	if (mask & ATTR_SIZE) {
		err = zfs_zaccess(zp, ACE_WRITE_DATA, 0, skipaclchk, cr,
		    mnt_ns);
		if (err)
			goto out3;

		/*
		 * XXX - Note, we are not providing any open
		 * mode flags here (like FNDELAY), so we may
		 * block if there are locks present... this
		 * should be addressed in openat().
		 */
		/* XXX - would it be OK to generate a log record here? */
		err = zfs_freesp(zp, vap->va_size, 0, 0, FALSE);
		if (err)
			goto out3;
	}

	if (mask & (ATTR_ATIME|ATTR_MTIME) ||
	    ((mask & ATTR_XVATTR) && (XVA_ISSET_REQ(xvap, XAT_HIDDEN) ||
	    XVA_ISSET_REQ(xvap, XAT_READONLY) ||
	    XVA_ISSET_REQ(xvap, XAT_ARCHIVE) ||
	    XVA_ISSET_REQ(xvap, XAT_OFFLINE) ||
	    XVA_ISSET_REQ(xvap, XAT_SPARSE) ||
	    XVA_ISSET_REQ(xvap, XAT_CREATETIME) ||
	    XVA_ISSET_REQ(xvap, XAT_SYSTEM)))) {
		need_policy = zfs_zaccess(zp, ACE_WRITE_ATTRIBUTES, 0,
		    skipaclchk, cr, mnt_ns);
	}

	if (mask & (ATTR_UID|ATTR_GID)) {
		int	idmask = (mask & (ATTR_UID|ATTR_GID));
		int	take_owner;
		int	take_group;
		uid_t	uid;
		gid_t	gid;

		/*
		 * NOTE: even if a new mode is being set,
		 * we may clear S_ISUID/S_ISGID bits.
		 */

		if (!(mask & ATTR_MODE))
			vap->va_mode = zp->z_mode;

		/*
		 * Take ownership or chgrp to group we are a member of
		 */

		uid = zfs_uid_to_vfsuid(mnt_ns, zfs_i_user_ns(ip),
		    vap->va_uid);
		gid = zfs_gid_to_vfsgid(mnt_ns, zfs_i_user_ns(ip),
		    vap->va_gid);
		take_owner = (mask & ATTR_UID) && (uid == crgetuid(cr));
		take_group = (mask & ATTR_GID) &&
		    zfs_groupmember(zfsvfs, gid, cr);

		/*
		 * If both ATTR_UID and ATTR_GID are set then take_owner and
		 * take_group must both be set in order to allow taking
		 * ownership.
		 *
		 * Otherwise, send the check through secpolicy_vnode_setattr()
		 *
		 */

		if (((idmask == (ATTR_UID|ATTR_GID)) &&
		    take_owner && take_group) ||
		    ((idmask == ATTR_UID) && take_owner) ||
		    ((idmask == ATTR_GID) && take_group)) {
			if (zfs_zaccess(zp, ACE_WRITE_OWNER, 0,
			    skipaclchk, cr, mnt_ns) == 0) {
				/*
				 * Remove setuid/setgid for non-privileged users
				 */
				(void) secpolicy_setid_clear(vap, cr);
				trim_mask = (mask & (ATTR_UID|ATTR_GID));
			} else {
				need_policy =  TRUE;
			}
		} else {
			need_policy =  TRUE;
		}
	}

	mutex_enter(&zp->z_lock);
	oldva.va_mode = zp->z_mode;
	zfs_fuid_map_ids(zp, cr, &oldva.va_uid, &oldva.va_gid);
	if (mask & ATTR_XVATTR) {
		/*
		 * Update xvattr mask to include only those attributes
		 * that are actually changing.
		 *
		 * the bits will be restored prior to actually setting
		 * the attributes so the caller thinks they were set.
		 */
		if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
			if (xoap->xoa_appendonly !=
			    ((zp->z_pflags & ZFS_APPENDONLY) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_APPENDONLY);
				XVA_SET_REQ(tmpxvattr, XAT_APPENDONLY);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT)) {
			if (xoap->xoa_projinherit !=
			    ((zp->z_pflags & ZFS_PROJINHERIT) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_PROJINHERIT);
				XVA_SET_REQ(tmpxvattr, XAT_PROJINHERIT);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
			if (xoap->xoa_nounlink !=
			    ((zp->z_pflags & ZFS_NOUNLINK) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_NOUNLINK);
				XVA_SET_REQ(tmpxvattr, XAT_NOUNLINK);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
			if (xoap->xoa_immutable !=
			    ((zp->z_pflags & ZFS_IMMUTABLE) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_IMMUTABLE);
				XVA_SET_REQ(tmpxvattr, XAT_IMMUTABLE);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
			if (xoap->xoa_nodump !=
			    ((zp->z_pflags & ZFS_NODUMP) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_NODUMP);
				XVA_SET_REQ(tmpxvattr, XAT_NODUMP);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
			if (xoap->xoa_av_modified !=
			    ((zp->z_pflags & ZFS_AV_MODIFIED) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_AV_MODIFIED);
				XVA_SET_REQ(tmpxvattr, XAT_AV_MODIFIED);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
			if ((!S_ISREG(ip->i_mode) &&
			    xoap->xoa_av_quarantined) ||
			    xoap->xoa_av_quarantined !=
			    ((zp->z_pflags & ZFS_AV_QUARANTINED) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_AV_QUARANTINED);
				XVA_SET_REQ(tmpxvattr, XAT_AV_QUARANTINED);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_REPARSE)) {
			mutex_exit(&zp->z_lock);
			err = SET_ERROR(EPERM);
			goto out3;
		}

		if (need_policy == FALSE &&
		    (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP) ||
		    XVA_ISSET_REQ(xvap, XAT_OPAQUE))) {
			need_policy = TRUE;
		}
	}

	mutex_exit(&zp->z_lock);

	if (mask & ATTR_MODE) {
		if (zfs_zaccess(zp, ACE_WRITE_ACL, 0, skipaclchk, cr,
		    mnt_ns) == 0) {
			err = secpolicy_setid_setsticky_clear(ip, vap,
			    &oldva, cr, mnt_ns, zfs_i_user_ns(ip));
			if (err)
				goto out3;
			trim_mask |= ATTR_MODE;
		} else {
			need_policy = TRUE;
		}
	}

	if (need_policy) {
		/*
		 * If trim_mask is set then take ownership
		 * has been granted or write_acl is present and user
		 * has the ability to modify mode.  In that case remove
		 * UID|GID and or MODE from mask so that
		 * secpolicy_vnode_setattr() doesn't revoke it.
		 */

		if (trim_mask) {
			saved_mask = vap->va_mask;
			vap->va_mask &= ~trim_mask;
		}
		err = secpolicy_vnode_setattr(cr, ip, vap, &oldva, flags,
		    zfs_zaccess_unix, zp);
		if (err)
			goto out3;

		if (trim_mask)
			vap->va_mask |= saved_mask;
	}

	/*
	 * secpolicy_vnode_setattr, or take ownership may have
	 * changed va_mask
	 */
	mask = vap->va_mask;

	if ((mask & (ATTR_UID | ATTR_GID)) || projid != ZFS_INVALID_PROJID) {
		handle_eadir = B_TRUE;
		err = sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
		    &xattr_obj, sizeof (xattr_obj));

		if (err == 0 && xattr_obj) {
			err = zfs_zget(ZTOZSB(zp), xattr_obj, &attrzp);
			if (err)
				goto out2;
		}
		if (mask & ATTR_UID) {
			new_kuid = zfs_fuid_create(zfsvfs,
			    (uint64_t)vap->va_uid, cr, ZFS_OWNER, &fuidp);
			if (new_kuid != KUID_TO_SUID(ZTOI(zp)->i_uid) &&
			    zfs_id_overquota(zfsvfs, DMU_USERUSED_OBJECT,
			    new_kuid)) {
				if (attrzp)
					zrele(attrzp);
				err = SET_ERROR(EDQUOT);
				goto out2;
			}
		}

		if (mask & ATTR_GID) {
			new_kgid = zfs_fuid_create(zfsvfs,
			    (uint64_t)vap->va_gid, cr, ZFS_GROUP, &fuidp);
			if (new_kgid != KGID_TO_SGID(ZTOI(zp)->i_gid) &&
			    zfs_id_overquota(zfsvfs, DMU_GROUPUSED_OBJECT,
			    new_kgid)) {
				if (attrzp)
					zrele(attrzp);
				err = SET_ERROR(EDQUOT);
				goto out2;
			}
		}

		if (projid != ZFS_INVALID_PROJID &&
		    zfs_id_overquota(zfsvfs, DMU_PROJECTUSED_OBJECT, projid)) {
			if (attrzp)
				zrele(attrzp);
			err = EDQUOT;
			goto out2;
		}
	}
	tx = dmu_tx_create(os);

	if (mask & ATTR_MODE) {
		uint64_t pmode = zp->z_mode;
		uint64_t acl_obj;
		new_mode = (pmode & S_IFMT) | (vap->va_mode & ~S_IFMT);

		if (ZTOZSB(zp)->z_acl_mode == ZFS_ACL_RESTRICTED &&
		    !(zp->z_pflags & ZFS_ACL_TRIVIAL)) {
			err = EPERM;
			goto out;
		}

		if ((err = zfs_acl_chmod_setattr(zp, &aclp, new_mode)))
			goto out;

		mutex_enter(&zp->z_lock);
		if (!zp->z_is_sa && ((acl_obj = zfs_external_acl(zp)) != 0)) {
			/*
			 * Are we upgrading ACL from old V0 format
			 * to V1 format?
			 */
			if (zfsvfs->z_version >= ZPL_VERSION_FUID &&
			    zfs_znode_acl_version(zp) ==
			    ZFS_ACL_VERSION_INITIAL) {
				dmu_tx_hold_free(tx, acl_obj, 0,
				    DMU_OBJECT_END);
				dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
				    0, aclp->z_acl_bytes);
			} else {
				dmu_tx_hold_write(tx, acl_obj, 0,
				    aclp->z_acl_bytes);
			}
		} else if (!zp->z_is_sa && aclp->z_acl_bytes > ZFS_ACE_SPACE) {
			dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
			    0, aclp->z_acl_bytes);
		}
		mutex_exit(&zp->z_lock);
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
	} else {
		if (((mask & ATTR_XVATTR) &&
		    XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP)) ||
		    (projid != ZFS_INVALID_PROJID &&
		    !(zp->z_pflags & ZFS_PROJID)))
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		else
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	}

	if (attrzp) {
		dmu_tx_hold_sa(tx, attrzp->z_sa_hdl, B_FALSE);
	}

	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);

	zfs_sa_upgrade_txholds(tx, zp);

	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err)
		goto out;

	count = 0;
	/*
	 * Set each attribute requested.
	 * We group settings according to the locks they need to acquire.
	 *
	 * Note: you cannot set ctime directly, although it will be
	 * updated as a side-effect of calling this function.
	 */

	if (projid != ZFS_INVALID_PROJID && !(zp->z_pflags & ZFS_PROJID)) {
		/*
		 * For the existed object that is upgraded from old system,
		 * its on-disk layout has no slot for the project ID attribute.
		 * But quota accounting logic needs to access related slots by
		 * offset directly. So we need to adjust old objects' layout
		 * to make the project ID to some unified and fixed offset.
		 */
		if (attrzp)
			err = sa_add_projid(attrzp->z_sa_hdl, tx, projid);
		if (err == 0)
			err = sa_add_projid(zp->z_sa_hdl, tx, projid);

		if (unlikely(err == EEXIST))
			err = 0;
		else if (err != 0)
			goto out;
		else
			projid = ZFS_INVALID_PROJID;
	}

	if (mask & (ATTR_UID|ATTR_GID|ATTR_MODE))
		mutex_enter(&zp->z_acl_lock);
	mutex_enter(&zp->z_lock);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));

	if (attrzp) {
		if (mask & (ATTR_UID|ATTR_GID|ATTR_MODE))
			mutex_enter(&attrzp->z_acl_lock);
		mutex_enter(&attrzp->z_lock);
		SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
		    SA_ZPL_FLAGS(zfsvfs), NULL, &attrzp->z_pflags,
		    sizeof (attrzp->z_pflags));
		if (projid != ZFS_INVALID_PROJID) {
			attrzp->z_projid = projid;
			SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
			    SA_ZPL_PROJID(zfsvfs), NULL, &attrzp->z_projid,
			    sizeof (attrzp->z_projid));
		}
	}

	if (mask & (ATTR_UID|ATTR_GID)) {

		if (mask & ATTR_UID) {
			ZTOI(zp)->i_uid = SUID_TO_KUID(new_kuid);
			new_uid = zfs_uid_read(ZTOI(zp));
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
			    &new_uid, sizeof (new_uid));
			if (attrzp) {
				SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
				    SA_ZPL_UID(zfsvfs), NULL, &new_uid,
				    sizeof (new_uid));
				ZTOI(attrzp)->i_uid = SUID_TO_KUID(new_uid);
			}
		}

		if (mask & ATTR_GID) {
			ZTOI(zp)->i_gid = SGID_TO_KGID(new_kgid);
			new_gid = zfs_gid_read(ZTOI(zp));
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs),
			    NULL, &new_gid, sizeof (new_gid));
			if (attrzp) {
				SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
				    SA_ZPL_GID(zfsvfs), NULL, &new_gid,
				    sizeof (new_gid));
				ZTOI(attrzp)->i_gid = SGID_TO_KGID(new_kgid);
			}
		}
		if (!(mask & ATTR_MODE)) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs),
			    NULL, &new_mode, sizeof (new_mode));
			new_mode = zp->z_mode;
		}
		err = zfs_acl_chown_setattr(zp);
		ASSERT(err == 0);
		if (attrzp) {
			err = zfs_acl_chown_setattr(attrzp);
			ASSERT(err == 0);
		}
	}

	if (mask & ATTR_MODE) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL,
		    &new_mode, sizeof (new_mode));
		zp->z_mode = ZTOI(zp)->i_mode = new_mode;
		ASSERT3P(aclp, !=, NULL);
		err = zfs_aclset_common(zp, aclp, cr, tx);
		ASSERT0(err);
		if (zp->z_acl_cached)
			zfs_acl_free(zp->z_acl_cached);
		zp->z_acl_cached = aclp;
		aclp = NULL;
	}

	if ((mask & ATTR_ATIME) || zp->z_atime_dirty) {
		zp->z_atime_dirty = B_FALSE;
		inode_timespec_t tmp_atime = zpl_inode_get_atime(ip);
		ZFS_TIME_ENCODE(&tmp_atime, atime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
		    &atime, sizeof (atime));
	}

	if (mask & (ATTR_MTIME | ATTR_SIZE)) {
		ZFS_TIME_ENCODE(&vap->va_mtime, mtime);
		zpl_inode_set_mtime_to_ts(ZTOI(zp),
		    zpl_inode_timestamp_truncate(vap->va_mtime, ZTOI(zp)));

		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
	}

	if (mask & (ATTR_CTIME | ATTR_SIZE)) {
		ZFS_TIME_ENCODE(&vap->va_ctime, ctime);
		zpl_inode_set_ctime_to_ts(ZTOI(zp),
		    zpl_inode_timestamp_truncate(vap->va_ctime, ZTOI(zp)));
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    ctime, sizeof (ctime));
	}

	if (projid != ZFS_INVALID_PROJID) {
		zp->z_projid = projid;
		SA_ADD_BULK_ATTR(bulk, count,
		    SA_ZPL_PROJID(zfsvfs), NULL, &zp->z_projid,
		    sizeof (zp->z_projid));
	}

	if (attrzp && mask) {
		SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
		    SA_ZPL_CTIME(zfsvfs), NULL, &ctime,
		    sizeof (ctime));
	}

	/*
	 * Do this after setting timestamps to prevent timestamp
	 * update from toggling bit
	 */

	if (xoap && (mask & ATTR_XVATTR)) {

		/*
		 * restore trimmed off masks
		 * so that return masks can be set for caller.
		 */

		if (XVA_ISSET_REQ(tmpxvattr, XAT_APPENDONLY)) {
			XVA_SET_REQ(xvap, XAT_APPENDONLY);
		}
		if (XVA_ISSET_REQ(tmpxvattr, XAT_NOUNLINK)) {
			XVA_SET_REQ(xvap, XAT_NOUNLINK);
		}
		if (XVA_ISSET_REQ(tmpxvattr, XAT_IMMUTABLE)) {
			XVA_SET_REQ(xvap, XAT_IMMUTABLE);
		}
		if (XVA_ISSET_REQ(tmpxvattr, XAT_NODUMP)) {
			XVA_SET_REQ(xvap, XAT_NODUMP);
		}
		if (XVA_ISSET_REQ(tmpxvattr, XAT_AV_MODIFIED)) {
			XVA_SET_REQ(xvap, XAT_AV_MODIFIED);
		}
		if (XVA_ISSET_REQ(tmpxvattr, XAT_AV_QUARANTINED)) {
			XVA_SET_REQ(xvap, XAT_AV_QUARANTINED);
		}
		if (XVA_ISSET_REQ(tmpxvattr, XAT_PROJINHERIT)) {
			XVA_SET_REQ(xvap, XAT_PROJINHERIT);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP))
			ASSERT(S_ISREG(ip->i_mode));

		zfs_xvattr_set(zp, xvap, tx);
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	if (mask != 0)
		zfs_log_setattr(zilog, tx, TX_SETATTR, zp, vap, mask, fuidp);

	mutex_exit(&zp->z_lock);
	if (mask & (ATTR_UID|ATTR_GID|ATTR_MODE))
		mutex_exit(&zp->z_acl_lock);

	if (attrzp) {
		if (mask & (ATTR_UID|ATTR_GID|ATTR_MODE))
			mutex_exit(&attrzp->z_acl_lock);
		mutex_exit(&attrzp->z_lock);
	}
out:
	if (err == 0 && xattr_count > 0) {
		err2 = sa_bulk_update(attrzp->z_sa_hdl, xattr_bulk,
		    xattr_count, tx);
		ASSERT(err2 == 0);
	}

	if (aclp)
		zfs_acl_free(aclp);

	if (fuidp) {
		zfs_fuid_info_free(fuidp);
		fuidp = NULL;
	}

	if (err) {
		dmu_tx_abort(tx);
		if (attrzp)
			zrele(attrzp);
		if (err == ERESTART)
			goto top;
	} else {
		if (count > 0)
			err2 = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
		dmu_tx_commit(tx);
		if (attrzp) {
			if (err2 == 0 && handle_eadir)
				err = zfs_setattr_dir(attrzp);
			zrele(attrzp);
		}
		zfs_znode_update_vfs(zp);
	}

out2:
	if (os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

out3:
	kmem_free(xattr_bulk, sizeof (sa_bulk_attr_t) * bulks);
	kmem_free(bulk, sizeof (sa_bulk_attr_t) * bulks);
	kmem_free(tmpxvattr, sizeof (xvattr_t));
	zfs_exit(zfsvfs, FTAG);
	return (err);
}

typedef struct zfs_zlock {
	krwlock_t	*zl_rwlock;	/* lock we acquired */
	znode_t		*zl_znode;	/* znode we held */
	struct zfs_zlock *zl_next;	/* next in list */
} zfs_zlock_t;

/*
 * Drop locks and release vnodes that were held by zfs_rename_lock().
 */
static void
zfs_rename_unlock(zfs_zlock_t **zlpp)
{
	zfs_zlock_t *zl;

	while ((zl = *zlpp) != NULL) {
		if (zl->zl_znode != NULL)
			zfs_zrele_async(zl->zl_znode);
		rw_exit(zl->zl_rwlock);
		*zlpp = zl->zl_next;
		kmem_free(zl, sizeof (*zl));
	}
}

/*
 * Search back through the directory tree, using the ".." entries.
 * Lock each directory in the chain to prevent concurrent renames.
 * Fail any attempt to move a directory into one of its own descendants.
 * XXX - z_parent_lock can overlap with map or grow locks
 */
static int
zfs_rename_lock(znode_t *szp, znode_t *tdzp, znode_t *sdzp, zfs_zlock_t **zlpp)
{
	zfs_zlock_t	*zl;
	znode_t		*zp = tdzp;
	uint64_t	rootid = ZTOZSB(zp)->z_root;
	uint64_t	oidp = zp->z_id;
	krwlock_t	*rwlp = &szp->z_parent_lock;
	krw_t		rw = RW_WRITER;

	/*
	 * First pass write-locks szp and compares to zp->z_id.
	 * Later passes read-lock zp and compare to zp->z_parent.
	 */
	do {
		if (!rw_tryenter(rwlp, rw)) {
			/*
			 * Another thread is renaming in this path.
			 * Note that if we are a WRITER, we don't have any
			 * parent_locks held yet.
			 */
			if (rw == RW_READER && zp->z_id > szp->z_id) {
				/*
				 * Drop our locks and restart
				 */
				zfs_rename_unlock(&zl);
				*zlpp = NULL;
				zp = tdzp;
				oidp = zp->z_id;
				rwlp = &szp->z_parent_lock;
				rw = RW_WRITER;
				continue;
			} else {
				/*
				 * Wait for other thread to drop its locks
				 */
				rw_enter(rwlp, rw);
			}
		}

		zl = kmem_alloc(sizeof (*zl), KM_SLEEP);
		zl->zl_rwlock = rwlp;
		zl->zl_znode = NULL;
		zl->zl_next = *zlpp;
		*zlpp = zl;

		if (oidp == szp->z_id)		/* We're a descendant of szp */
			return (SET_ERROR(EINVAL));

		if (oidp == rootid)		/* We've hit the top */
			return (0);

		if (rw == RW_READER) {		/* i.e. not the first pass */
			int error = zfs_zget(ZTOZSB(zp), oidp, &zp);
			if (error)
				return (error);
			zl->zl_znode = zp;
		}
		(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(ZTOZSB(zp)),
		    &oidp, sizeof (oidp));
		rwlp = &zp->z_parent_lock;
		rw = RW_READER;

	} while (zp->z_id != sdzp->z_id);

	return (0);
}

/*
 * Move an entry from the provided source directory to the target
 * directory.  Change the entry name as indicated.
 *
 *	IN:	sdzp	- Source directory containing the "old entry".
 *		snm	- Old entry name.
 *		tdzp	- Target directory to contain the "new entry".
 *		tnm	- New entry name.
 *		cr	- credentials of caller.
 *		flags	- case flags
 *		rflags  - RENAME_* flags
 *		wa_vap  - attributes for RENAME_WHITEOUT (must be a char 0:0).
 *		mnt_ns	- user namespace of the mount
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	sdzp,tdzp - ctime|mtime updated
 */
int
zfs_rename(znode_t *sdzp, char *snm, znode_t *tdzp, char *tnm,
    cred_t *cr, int flags, uint64_t rflags, vattr_t *wo_vap, zidmap_t *mnt_ns)
{
	znode_t		*szp, *tzp;
	zfsvfs_t	*zfsvfs = ZTOZSB(sdzp);
	zilog_t		*zilog;
	zfs_dirlock_t	*sdl, *tdl;
	dmu_tx_t	*tx;
	zfs_zlock_t	*zl;
	int		cmp, serr, terr;
	int		error = 0;
	int		zflg = 0;
	boolean_t	waited = B_FALSE;
	/* Needed for whiteout inode creation. */
	boolean_t	fuid_dirtied;
	zfs_acl_ids_t	acl_ids;
	boolean_t	have_acl = B_FALSE;
	znode_t		*wzp = NULL;


	if (snm == NULL || tnm == NULL)
		return (SET_ERROR(EINVAL));

	if (rflags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
		return (SET_ERROR(EINVAL));

	/* Already checked by Linux VFS, but just to make sure. */
	if (rflags & RENAME_EXCHANGE &&
	    (rflags & (RENAME_NOREPLACE | RENAME_WHITEOUT)))
		return (SET_ERROR(EINVAL));

	/*
	 * Make sure we only get wo_vap iff. RENAME_WHITEOUT and that it's the
	 * right kind of vattr_t for the whiteout file. These are set
	 * internally by ZFS so should never be incorrect.
	 */
	VERIFY_EQUIV(rflags & RENAME_WHITEOUT, wo_vap != NULL);
	VERIFY_IMPLY(wo_vap, wo_vap->va_mode == S_IFCHR);
	VERIFY_IMPLY(wo_vap, wo_vap->va_rdev == makedevice(0, 0));

	if ((error = zfs_enter_verify_zp(zfsvfs, sdzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if ((error = zfs_verify_zp(tdzp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * We check i_sb because snapshots and the ctldir must have different
	 * super blocks.
	 */
	if (ZTOI(tdzp)->i_sb != ZTOI(sdzp)->i_sb ||
	    zfsctl_is_node(ZTOI(tdzp))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EXDEV));
	}

	if (zfsvfs->z_utf8 && u8_validate(tnm,
	    strlen(tnm), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (flags & FIGNORECASE)
		zflg |= ZCILOOK;

top:
	szp = NULL;
	tzp = NULL;
	zl = NULL;

	/*
	 * This is to prevent the creation of links into attribute space
	 * by renaming a linked file into/outof an attribute directory.
	 * See the comment in zfs_link() for why this is considered bad.
	 */
	if ((tdzp->z_pflags & ZFS_XATTR) != (sdzp->z_pflags & ZFS_XATTR)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Lock source and target directory entries.  To prevent deadlock,
	 * a lock ordering must be defined.  We lock the directory with
	 * the smallest object id first, or if it's a tie, the one with
	 * the lexically first name.
	 */
	if (sdzp->z_id < tdzp->z_id) {
		cmp = -1;
	} else if (sdzp->z_id > tdzp->z_id) {
		cmp = 1;
	} else {
		/*
		 * First compare the two name arguments without
		 * considering any case folding.
		 */
		int nofold = (zfsvfs->z_norm & ~U8_TEXTPREP_TOUPPER);

		cmp = u8_strcmp(snm, tnm, 0, nofold, U8_UNICODE_LATEST, &error);
		ASSERT(error == 0 || !zfsvfs->z_utf8);
		if (cmp == 0) {
			/*
			 * POSIX: "If the old argument and the new argument
			 * both refer to links to the same existing file,
			 * the rename() function shall return successfully
			 * and perform no other action."
			 */
			zfs_exit(zfsvfs, FTAG);
			return (0);
		}
		/*
		 * If the file system is case-folding, then we may
		 * have some more checking to do.  A case-folding file
		 * system is either supporting mixed case sensitivity
		 * access or is completely case-insensitive.  Note
		 * that the file system is always case preserving.
		 *
		 * In mixed sensitivity mode case sensitive behavior
		 * is the default.  FIGNORECASE must be used to
		 * explicitly request case insensitive behavior.
		 *
		 * If the source and target names provided differ only
		 * by case (e.g., a request to rename 'tim' to 'Tim'),
		 * we will treat this as a special case in the
		 * case-insensitive mode: as long as the source name
		 * is an exact match, we will allow this to proceed as
		 * a name-change request.
		 */
		if ((zfsvfs->z_case == ZFS_CASE_INSENSITIVE ||
		    (zfsvfs->z_case == ZFS_CASE_MIXED &&
		    flags & FIGNORECASE)) &&
		    u8_strcmp(snm, tnm, 0, zfsvfs->z_norm, U8_UNICODE_LATEST,
		    &error) == 0) {
			/*
			 * case preserving rename request, require exact
			 * name matches
			 */
			zflg |= ZCIEXACT;
			zflg &= ~ZCILOOK;
		}
	}

	/*
	 * If the source and destination directories are the same, we should
	 * grab the z_name_lock of that directory only once.
	 */
	if (sdzp == tdzp) {
		zflg |= ZHAVELOCK;
		rw_enter(&sdzp->z_name_lock, RW_READER);
	}

	if (cmp < 0) {
		serr = zfs_dirent_lock(&sdl, sdzp, snm, &szp,
		    ZEXISTS | zflg, NULL, NULL);
		terr = zfs_dirent_lock(&tdl,
		    tdzp, tnm, &tzp, ZRENAMING | zflg, NULL, NULL);
	} else {
		terr = zfs_dirent_lock(&tdl,
		    tdzp, tnm, &tzp, zflg, NULL, NULL);
		serr = zfs_dirent_lock(&sdl,
		    sdzp, snm, &szp, ZEXISTS | ZRENAMING | zflg,
		    NULL, NULL);
	}

	if (serr) {
		/*
		 * Source entry invalid or not there.
		 */
		if (!terr) {
			zfs_dirent_unlock(tdl);
			if (tzp)
				zrele(tzp);
		}

		if (sdzp == tdzp)
			rw_exit(&sdzp->z_name_lock);

		if (strcmp(snm, "..") == 0)
			serr = EINVAL;
		zfs_exit(zfsvfs, FTAG);
		return (serr);
	}
	if (terr) {
		zfs_dirent_unlock(sdl);
		zrele(szp);

		if (sdzp == tdzp)
			rw_exit(&sdzp->z_name_lock);

		if (strcmp(tnm, "..") == 0)
			terr = EINVAL;
		zfs_exit(zfsvfs, FTAG);
		return (terr);
	}

	/*
	 * If we are using project inheritance, means if the directory has
	 * ZFS_PROJINHERIT set, then its descendant directories will inherit
	 * not only the project ID, but also the ZFS_PROJINHERIT flag. Under
	 * such case, we only allow renames into our tree when the project
	 * IDs are the same.
	 */
	if (tdzp->z_pflags & ZFS_PROJINHERIT &&
	    tdzp->z_projid != szp->z_projid) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	/*
	 * Must have write access at the source to remove the old entry
	 * and write access at the target to create the new entry.
	 * Note that if target and source are the same, this can be
	 * done in a single check.
	 */
	if ((error = zfs_zaccess_rename(sdzp, szp, tdzp, tzp, cr, mnt_ns)))
		goto out;

	if (S_ISDIR(ZTOI(szp)->i_mode)) {
		/*
		 * Check to make sure rename is valid.
		 * Can't do a move like this: /usr/a/b to /usr/a/b/c/d
		 */
		if ((error = zfs_rename_lock(szp, tdzp, sdzp, &zl)))
			goto out;
	}

	/*
	 * Does target exist?
	 */
	if (tzp) {
		if (rflags & RENAME_NOREPLACE) {
			error = SET_ERROR(EEXIST);
			goto out;
		}
		/*
		 * Source and target must be the same type (unless exchanging).
		 */
		if (!(rflags & RENAME_EXCHANGE)) {
			boolean_t s_is_dir = S_ISDIR(ZTOI(szp)->i_mode) != 0;
			boolean_t t_is_dir = S_ISDIR(ZTOI(tzp)->i_mode) != 0;

			if (s_is_dir != t_is_dir) {
				error = SET_ERROR(s_is_dir ? ENOTDIR : EISDIR);
				goto out;
			}
		}
		/*
		 * POSIX dictates that when the source and target
		 * entries refer to the same file object, rename
		 * must do nothing and exit without error.
		 */
		if (szp->z_id == tzp->z_id) {
			error = 0;
			goto out;
		}
	} else if (rflags & RENAME_EXCHANGE) {
		/* Target must exist for RENAME_EXCHANGE. */
		error = SET_ERROR(ENOENT);
		goto out;
	}

	/* Set up inode creation for RENAME_WHITEOUT. */
	if (rflags & RENAME_WHITEOUT) {
		/*
		 * Whiteout files are not regular files or directories, so to
		 * match zfs_create() we do not inherit the project id.
		 */
		uint64_t wo_projid = ZFS_DEFAULT_PROJID;

		error = zfs_zaccess(sdzp, ACE_ADD_FILE, 0, B_FALSE, cr, mnt_ns);
		if (error)
			goto out;

		if (!have_acl) {
			error = zfs_acl_ids_create(sdzp, 0, wo_vap, cr, NULL,
			    &acl_ids, mnt_ns);
			if (error)
				goto out;
			have_acl = B_TRUE;
		}

		if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, wo_projid)) {
			error = SET_ERROR(EDQUOT);
			goto out;
		}
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_sa(tx, sdzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, sdzp->z_id,
	    (rflags & RENAME_EXCHANGE) ? TRUE : FALSE, snm);
	dmu_tx_hold_zap(tx, tdzp->z_id, TRUE, tnm);
	if (sdzp != tdzp) {
		dmu_tx_hold_sa(tx, tdzp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, tdzp);
	}
	if (tzp) {
		dmu_tx_hold_sa(tx, tzp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, tzp);
	}
	if (rflags & RENAME_WHITEOUT) {
		dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
		    ZFS_SA_BASE_ATTR_SIZE);

		dmu_tx_hold_zap(tx, sdzp->z_id, TRUE, snm);
		dmu_tx_hold_sa(tx, sdzp->z_sa_hdl, B_FALSE);
		if (!zfsvfs->z_use_sa &&
		    acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
			dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
			    0, acl_ids.z_aclp->z_acl_bytes);
		}
	}
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	zfs_sa_upgrade_txholds(tx, szp);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);
	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		if (zl != NULL)
			zfs_rename_unlock(&zl);
		zfs_dirent_unlock(sdl);
		zfs_dirent_unlock(tdl);

		if (sdzp == tdzp)
			rw_exit(&sdzp->z_name_lock);

		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			zrele(szp);
			if (tzp)
				zrele(tzp);
			goto top;
		}
		dmu_tx_abort(tx);
		zrele(szp);
		if (tzp)
			zrele(tzp);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Unlink the source.
	 */
	szp->z_pflags |= ZFS_AV_MODIFIED;
	if (tdzp->z_pflags & ZFS_PROJINHERIT)
		szp->z_pflags |= ZFS_PROJINHERIT;

	error = sa_update(szp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
	    (void *)&szp->z_pflags, sizeof (uint64_t), tx);
	VERIFY0(error);

	error = zfs_link_destroy(sdl, szp, tx, ZRENAMING, NULL);
	if (error)
		goto commit;

	/*
	 * Unlink the target.
	 */
	if (tzp) {
		int tzflg = zflg;

		if (rflags & RENAME_EXCHANGE) {
			/* This inode will be re-linked soon. */
			tzflg |= ZRENAMING;

			tzp->z_pflags |= ZFS_AV_MODIFIED;
			if (sdzp->z_pflags & ZFS_PROJINHERIT)
				tzp->z_pflags |= ZFS_PROJINHERIT;

			error = sa_update(tzp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
			    (void *)&tzp->z_pflags, sizeof (uint64_t), tx);
			ASSERT0(error);
		}
		error = zfs_link_destroy(tdl, tzp, tx, tzflg, NULL);
		if (error)
			goto commit_link_szp;
	}

	/*
	 * Create the new target links:
	 *   * We always link the target.
	 *   * RENAME_EXCHANGE: Link the old target to the source.
	 *   * RENAME_WHITEOUT: Create a whiteout inode in-place of the source.
	 */
	error = zfs_link_create(tdl, szp, tx, ZRENAMING);
	if (error) {
		/*
		 * If we have removed the existing target, a subsequent call to
		 * zfs_link_create() to add back the same entry, but with a new
		 * dnode (szp), should not fail.
		 */
		ASSERT3P(tzp, ==, NULL);
		goto commit_link_tzp;
	}

	switch (rflags & (RENAME_EXCHANGE | RENAME_WHITEOUT)) {
	case RENAME_EXCHANGE:
		error = zfs_link_create(sdl, tzp, tx, ZRENAMING);
		/*
		 * The same argument as zfs_link_create() failing for
		 * szp applies here, since the source directory must
		 * have had an entry we are replacing.
		 */
		ASSERT0(error);
		if (error)
			goto commit_unlink_td_szp;
		break;
	case RENAME_WHITEOUT:
		zfs_mknode(sdzp, wo_vap, tx, cr, 0, &wzp, &acl_ids);
		error = zfs_link_create(sdl, wzp, tx, ZNEW);
		if (error) {
			zfs_znode_delete(wzp, tx);
			remove_inode_hash(ZTOI(wzp));
			goto commit_unlink_td_szp;
		}
		break;
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	switch (rflags & (RENAME_EXCHANGE | RENAME_WHITEOUT)) {
	case RENAME_EXCHANGE:
		zfs_log_rename_exchange(zilog, tx,
		    (flags & FIGNORECASE ? TX_CI : 0), sdzp, sdl->dl_name,
		    tdzp, tdl->dl_name, szp);
		break;
	case RENAME_WHITEOUT:
		zfs_log_rename_whiteout(zilog, tx,
		    (flags & FIGNORECASE ? TX_CI : 0), sdzp, sdl->dl_name,
		    tdzp, tdl->dl_name, szp, wzp);
		break;
	default:
		ASSERT0(rflags & ~RENAME_NOREPLACE);
		zfs_log_rename(zilog, tx, (flags & FIGNORECASE ? TX_CI : 0),
		    sdzp, sdl->dl_name, tdzp, tdl->dl_name, szp);
		break;
	}

commit:
	dmu_tx_commit(tx);
out:
	if (have_acl)
		zfs_acl_ids_free(&acl_ids);

	zfs_znode_update_vfs(sdzp);
	if (sdzp == tdzp)
		rw_exit(&sdzp->z_name_lock);

	if (sdzp != tdzp)
		zfs_znode_update_vfs(tdzp);

	zfs_znode_update_vfs(szp);
	zrele(szp);
	if (wzp) {
		zfs_znode_update_vfs(wzp);
		zrele(wzp);
	}
	if (tzp) {
		zfs_znode_update_vfs(tzp);
		zrele(tzp);
	}

	if (zl != NULL)
		zfs_rename_unlock(&zl);

	zfs_dirent_unlock(sdl);
	zfs_dirent_unlock(tdl);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);

	/*
	 * Clean-up path for broken link state.
	 *
	 * At this point we are in a (very) bad state, so we need to do our
	 * best to correct the state. In particular, all of the nlinks are
	 * wrong because we were destroying and creating links with ZRENAMING.
	 *
	 * In some form, all of these operations have to resolve the state:
	 *
	 *  * link_destroy() *must* succeed. Fortunately, this is very likely
	 *    since we only just created it.
	 *
	 *  * link_create()s are allowed to fail (though they shouldn't because
	 *    we only just unlinked them and are putting the entries back
	 *    during clean-up). But if they fail, we can just forcefully drop
	 *    the nlink value to (at the very least) avoid broken nlink values
	 *    -- though in the case of non-empty directories we will have to
	 *    panic (otherwise we'd have a leaked directory with a broken ..).
	 */
commit_unlink_td_szp:
	VERIFY0(zfs_link_destroy(tdl, szp, tx, ZRENAMING, NULL));
commit_link_tzp:
	if (tzp) {
		if (zfs_link_create(tdl, tzp, tx, ZRENAMING))
			VERIFY0(zfs_drop_nlink(tzp, tx, NULL));
	}
commit_link_szp:
	if (zfs_link_create(sdl, szp, tx, ZRENAMING))
		VERIFY0(zfs_drop_nlink(szp, tx, NULL));
	goto commit;
}

/*
 * Insert the indicated symbolic reference entry into the directory.
 *
 *	IN:	dzp	- Directory to contain new symbolic link.
 *		name	- Name of directory entry in dip.
 *		vap	- Attributes of new entry.
 *		link	- Name for new symlink entry.
 *		cr	- credentials of caller.
 *		flags	- case flags
 *		mnt_ns	- user namespace of the mount
 *
 *	OUT:	zpp	- Znode for new symbolic link.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dip - ctime|mtime updated
 */
int
zfs_symlink(znode_t *dzp, char *name, vattr_t *vap, char *link,
    znode_t **zpp, cred_t *cr, int flags, zidmap_t *mnt_ns)
{
	znode_t		*zp;
	zfs_dirlock_t	*dl;
	dmu_tx_t	*tx;
	zfsvfs_t	*zfsvfs = ZTOZSB(dzp);
	zilog_t		*zilog;
	uint64_t	len = strlen(link);
	int		error;
	int		zflg = ZNEW;
	zfs_acl_ids_t	acl_ids;
	boolean_t	fuid_dirtied;
	uint64_t	txtype = TX_SYMLINK;
	boolean_t	waited = B_FALSE;

	ASSERT(S_ISLNK(vap->va_mode));

	if (name == NULL)
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}
	if (flags & FIGNORECASE)
		zflg |= ZCILOOK;

	if (len > MAXPATHLEN) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ENAMETOOLONG));
	}

	if ((error = zfs_acl_ids_create(dzp, 0,
	    vap, cr, NULL, &acl_ids, mnt_ns)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
top:
	*zpp = NULL;

	/*
	 * Attempt to lock directory; fail if entry already exists.
	 */
	error = zfs_dirent_lock(&dl, dzp, name, &zp, zflg, NULL, NULL);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr, mnt_ns))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_dirent_unlock(dl);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, ZFS_DEFAULT_PROJID)) {
		zfs_acl_ids_free(&acl_ids);
		zfs_dirent_unlock(dl);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EDQUOT));
	}
	tx = dmu_tx_create(zfsvfs->z_os);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, MAX(1, len));
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, name);
	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE + len);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	if (!zfsvfs->z_use_sa && acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
		    acl_ids.z_aclp->z_acl_bytes);
	}
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		zfs_dirent_unlock(dl);
		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			goto top;
		}
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Create a new object for the symlink.
	 * for version 4 ZPL datasets the symlink will be an SA attribute
	 */
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	mutex_enter(&zp->z_lock);
	if (zp->z_is_sa)
		error = sa_update(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    link, len, tx);
	else
		zfs_sa_symlink(zp, link, len, tx);
	mutex_exit(&zp->z_lock);

	zp->z_size = len;
	(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
	    &zp->z_size, sizeof (zp->z_size), tx);
	/*
	 * Insert the new object into the directory.
	 */
	error = zfs_link_create(dl, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		remove_inode_hash(ZTOI(zp));
	} else {
		if (flags & FIGNORECASE)
			txtype |= TX_CI;
		zfs_log_symlink(zilog, tx, txtype, dzp, zp, name, link);

		zfs_znode_update_vfs(dzp);
		zfs_znode_update_vfs(zp);
	}

	zfs_acl_ids_free(&acl_ids);

	dmu_tx_commit(tx);

	zfs_dirent_unlock(dl);

	if (error == 0) {
		*zpp = zp;

		if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
			zil_commit(zilog, 0);
	} else {
		zrele(zp);
	}

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Return, in the buffer contained in the provided uio structure,
 * the symbolic path referred to by ip.
 *
 *	IN:	ip	- inode of symbolic link
 *		uio	- structure to contain the link path.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	ip - atime updated
 */
int
zfs_readlink(struct inode *ip, zfs_uio_t *uio, cred_t *cr)
{
	(void) cr;
	znode_t		*zp = ITOZ(ip);
	zfsvfs_t	*zfsvfs = ITOZSB(ip);
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	mutex_enter(&zp->z_lock);
	if (zp->z_is_sa)
		error = sa_lookup_uio(zp->z_sa_hdl,
		    SA_ZPL_SYMLINK(zfsvfs), uio);
	else
		error = zfs_sa_readlink(zp, uio);
	mutex_exit(&zp->z_lock);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Insert a new entry into directory tdzp referencing szp.
 *
 *	IN:	tdzp	- Directory to contain new entry.
 *		szp	- znode of new entry.
 *		name	- name of new entry.
 *		cr	- credentials of caller.
 *		flags	- case flags.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	tdzp - ctime|mtime updated
 *	 szp - ctime updated
 */
int
zfs_link(znode_t *tdzp, znode_t *szp, char *name, cred_t *cr,
    int flags)
{
	struct inode *sip = ZTOI(szp);
	znode_t		*tzp;
	zfsvfs_t	*zfsvfs = ZTOZSB(tdzp);
	zilog_t		*zilog;
	zfs_dirlock_t	*dl;
	dmu_tx_t	*tx;
	int		error;
	int		zf = ZNEW;
	uint64_t	parent;
	uid_t		owner;
	boolean_t	waited = B_FALSE;
	boolean_t	is_tmpfile = 0;
	uint64_t	txg;

	is_tmpfile = (sip->i_nlink == 0 && (sip->i_state & I_LINKABLE));

	ASSERT(S_ISDIR(ZTOI(tdzp)->i_mode));

	if (name == NULL)
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, tdzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	/*
	 * POSIX dictates that we return EPERM here.
	 * Better choices include ENOTSUP or EISDIR.
	 */
	if (S_ISDIR(sip->i_mode)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((error = zfs_verify_zp(szp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * If we are using project inheritance, means if the directory has
	 * ZFS_PROJINHERIT set, then its descendant directories will inherit
	 * not only the project ID, but also the ZFS_PROJINHERIT flag. Under
	 * such case, we only allow hard link creation in our tree when the
	 * project IDs are the same.
	 */
	if (tdzp->z_pflags & ZFS_PROJINHERIT &&
	    tdzp->z_projid != szp->z_projid) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EXDEV));
	}

	/*
	 * We check i_sb because snapshots and the ctldir must have different
	 * super blocks.
	 */
	if (sip->i_sb != ZTOI(tdzp)->i_sb || zfsctl_is_node(sip)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EXDEV));
	}

	/* Prevent links to .zfs/shares files */

	if ((error = sa_lookup(szp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (uint64_t))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	if (parent == zfsvfs->z_shares_dir) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if (zfsvfs->z_utf8 && u8_validate(name,
	    strlen(name), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}
	if (flags & FIGNORECASE)
		zf |= ZCILOOK;

	/*
	 * We do not support links between attributes and non-attributes
	 * because of the potential security risk of creating links
	 * into "normal" file space in order to circumvent restrictions
	 * imposed in attribute space.
	 */
	if ((szp->z_pflags & ZFS_XATTR) != (tdzp->z_pflags & ZFS_XATTR)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	owner = zfs_fuid_map_id(zfsvfs, KUID_TO_SUID(sip->i_uid),
	    cr, ZFS_OWNER);
	if (owner != crgetuid(cr) && secpolicy_basic_link(cr) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((error = zfs_zaccess(tdzp, ACE_ADD_FILE, 0, B_FALSE, cr,
	    zfs_init_idmap))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

top:
	/*
	 * Attempt to lock directory; fail if entry already exists.
	 */
	error = zfs_dirent_lock(&dl, tdzp, name, &tzp, zf, NULL, NULL);
	if (error) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, tdzp->z_id, TRUE, name);
	if (is_tmpfile)
		dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);

	zfs_sa_upgrade_txholds(tx, szp);
	zfs_sa_upgrade_txholds(tx, tdzp);
	error = dmu_tx_assign(tx,
	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
	if (error) {
		zfs_dirent_unlock(dl);
		if (error == ERESTART) {
			waited = B_TRUE;
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			goto top;
		}
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	/* unmark z_unlinked so zfs_link_create will not reject */
	if (is_tmpfile)
		szp->z_unlinked = B_FALSE;
	error = zfs_link_create(dl, szp, tx, 0);

	if (error == 0) {
		uint64_t txtype = TX_LINK;
		/*
		 * tmpfile is created to be in z_unlinkedobj, so remove it.
		 * Also, we don't log in ZIL, because all previous file
		 * operation on the tmpfile are ignored by ZIL. Instead we
		 * always wait for txg to sync to make sure all previous
		 * operation are sync safe.
		 */
		if (is_tmpfile) {
			VERIFY(zap_remove_int(zfsvfs->z_os,
			    zfsvfs->z_unlinkedobj, szp->z_id, tx) == 0);
		} else {
			if (flags & FIGNORECASE)
				txtype |= TX_CI;
			zfs_log_link(zilog, tx, txtype, tdzp, szp, name);
		}
	} else if (is_tmpfile) {
		/* restore z_unlinked since when linking failed */
		szp->z_unlinked = B_TRUE;
	}
	txg = dmu_tx_get_txg(tx);
	dmu_tx_commit(tx);

	zfs_dirent_unlock(dl);

	if (!is_tmpfile && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	if (is_tmpfile && zfsvfs->z_os->os_sync != ZFS_SYNC_DISABLED) {
		txg_wait_flag_t wait_flags =
		    spa_get_failmode(dmu_objset_spa(zfsvfs->z_os)) ==
		    ZIO_FAILURE_MODE_CONTINUE ? TXG_WAIT_SUSPEND : 0;
		error = txg_wait_synced_flags(dmu_objset_pool(zfsvfs->z_os),
		    txg, wait_flags);
		if (error != 0) {
			ASSERT3U(error, ==, ESHUTDOWN);
			error = SET_ERROR(EIO);
		}
	}

	zfs_znode_update_vfs(tdzp);
	zfs_znode_update_vfs(szp);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static void
zfs_putpage_sync_commit_cb(void *arg)
{
	struct page *pp = arg;

	ClearPageError(pp);
	end_page_writeback(pp);
}

static void
zfs_putpage_async_commit_cb(void *arg)
{
	struct page *pp = arg;
	znode_t *zp = ITOZ(pp->mapping->host);

	ClearPageError(pp);
	end_page_writeback(pp);
	atomic_dec_32(&zp->z_async_writes_cnt);
}

/*
 * Push a page out to disk, once the page is on stable storage the
 * registered commit callback will be run as notification of completion.
 *
 *	IN:	ip	 - page mapped for inode.
 *		pp	 - page to push (page is locked)
 *		wbc	 - writeback control data
 *		for_sync - does the caller intend to wait synchronously for the
 *			   page writeback to complete?
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	ip - ctime|mtime updated
 */
int
zfs_putpage(struct inode *ip, struct page *pp, struct writeback_control *wbc,
    boolean_t for_sync)
{
	znode_t		*zp = ITOZ(ip);
	zfsvfs_t	*zfsvfs = ITOZSB(ip);
	loff_t		offset;
	loff_t		pgoff;
	unsigned int	pglen;
	dmu_tx_t	*tx;
	caddr_t		va;
	int		err = 0;
	uint64_t	mtime[2], ctime[2];
	inode_timespec_t tmp_ts;
	sa_bulk_attr_t	bulk[3];
	int		cnt = 0;
	struct address_space *mapping;

	if ((err = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (err);

	ASSERT(PageLocked(pp));

	pgoff = page_offset(pp);	/* Page byte-offset in file */
	offset = i_size_read(ip);	/* File length in bytes */
	pglen = MIN(PAGE_SIZE,		/* Page length in bytes */
	    P2ROUNDUP(offset, PAGE_SIZE)-pgoff);

	/* Page is beyond end of file */
	if (pgoff >= offset) {
		unlock_page(pp);
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	/* Truncate page length to end of file */
	if (pgoff + pglen > offset)
		pglen = offset - pgoff;

#if 0
	/*
	 * FIXME: Allow mmap writes past its quota.  The correct fix
	 * is to register a page_mkwrite() handler to count the page
	 * against its quota when it is about to be dirtied.
	 */
	if (zfs_id_overblockquota(zfsvfs, DMU_USERUSED_OBJECT,
	    KUID_TO_SUID(ip->i_uid)) ||
	    zfs_id_overblockquota(zfsvfs, DMU_GROUPUSED_OBJECT,
	    KGID_TO_SGID(ip->i_gid)) ||
	    (zp->z_projid != ZFS_DEFAULT_PROJID &&
	    zfs_id_overblockquota(zfsvfs, DMU_PROJECTUSED_OBJECT,
	    zp->z_projid))) {
		err = EDQUOT;
	}
#endif

	/*
	 * The ordering here is critical and must adhere to the following
	 * rules in order to avoid deadlocking in either zfs_read() or
	 * zfs_free_range() due to a lock inversion.
	 *
	 * 1) The page must be unlocked prior to acquiring the range lock.
	 *    This is critical because zfs_read() calls find_lock_page()
	 *    which may block on the page lock while holding the range lock.
	 *
	 * 2) Before setting or clearing write back on a page the range lock
	 *    must be held in order to prevent a lock inversion with the
	 *    zfs_free_range() function.
	 *
	 * This presents a problem because upon entering this function the
	 * page lock is already held.  To safely acquire the range lock the
	 * page lock must be dropped.  This creates a window where another
	 * process could truncate, invalidate, dirty, or write out the page.
	 *
	 * Therefore, after successfully reacquiring the range and page locks
	 * the current page state is checked.  In the common case everything
	 * will be as is expected and it can be written out.  However, if
	 * the page state has changed it must be handled accordingly.
	 */
	mapping = pp->mapping;
	redirty_page_for_writepage(wbc, pp);
	unlock_page(pp);

	zfs_locked_range_t *lr = zfs_rangelock_enter(&zp->z_rangelock,
	    pgoff, pglen, RL_WRITER);
	lock_page(pp);

	/* Page mapping changed or it was no longer dirty, we're done */
	if (unlikely((mapping != pp->mapping) || !PageDirty(pp))) {
		unlock_page(pp);
		zfs_rangelock_exit(lr);
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	/* Another process started write block if required */
	if (PageWriteback(pp)) {
		unlock_page(pp);
		zfs_rangelock_exit(lr);

		if (wbc->sync_mode != WB_SYNC_NONE) {
			/*
			 * Speed up any non-sync page writebacks since
			 * they may take several seconds to complete.
			 * Refer to the comment in zpl_fsync() for details.
			 */
			if (atomic_load_32(&zp->z_async_writes_cnt) > 0) {
				zil_commit(zfsvfs->z_log, zp->z_id);
			}

			if (PageWriteback(pp))
#ifdef HAVE_PAGEMAP_FOLIO_WAIT_BIT
				folio_wait_bit(page_folio(pp), PG_writeback);
#else
				wait_on_page_bit(pp, PG_writeback);
#endif
		}

		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	/* Clear the dirty flag the required locks are held */
	if (!clear_page_dirty_for_io(pp)) {
		unlock_page(pp);
		zfs_rangelock_exit(lr);
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	/*
	 * Counterpart for redirty_page_for_writepage() above.  This page
	 * was in fact not skipped and should not be counted as if it were.
	 */
	wbc->pages_skipped--;
	if (!for_sync)
		atomic_inc_32(&zp->z_async_writes_cnt);
	set_page_writeback(pp);
	unlock_page(pp);

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_write(tx, zp->z_id, pgoff, pglen);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);

	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
#ifdef HAVE_VFS_FILEMAP_DIRTY_FOLIO
		filemap_dirty_folio(page_mapping(pp), page_folio(pp));
#else
		__set_page_dirty_nobuffers(pp);
#endif
		ClearPageError(pp);
		end_page_writeback(pp);
		if (!for_sync)
			atomic_dec_32(&zp->z_async_writes_cnt);
		zfs_rangelock_exit(lr);
		zfs_exit(zfsvfs, FTAG);
		return (err);
	}

	va = kmap(pp);
	ASSERT3U(pglen, <=, PAGE_SIZE);
	dmu_write(zfsvfs->z_os, zp->z_id, pgoff, pglen, va, tx);
	kunmap(pp);

	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MTIME(zfsvfs), NULL, &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_CTIME(zfsvfs), NULL, &ctime, 16);
	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, 8);

	/* Preserve the mtime and ctime provided by the inode */
	tmp_ts = zpl_inode_get_mtime(ip);
	ZFS_TIME_ENCODE(&tmp_ts, mtime);
	tmp_ts = zpl_inode_get_ctime(ip);
	ZFS_TIME_ENCODE(&tmp_ts, ctime);
	zp->z_atime_dirty = B_FALSE;
	zp->z_seq++;

	err = sa_bulk_update(zp->z_sa_hdl, bulk, cnt, tx);

	boolean_t commit = B_FALSE;
	if (wbc->sync_mode != WB_SYNC_NONE) {
		/*
		 * Note that this is rarely called under writepages(), because
		 * writepages() normally handles the entire commit for
		 * performance reasons.
		 */
		commit = B_TRUE;
	} else if (!for_sync && atomic_load_32(&zp->z_sync_writes_cnt) > 0) {
		/*
		 * If the caller does not intend to wait synchronously
		 * for this page writeback to complete and there are active
		 * synchronous calls on this file, do a commit so that
		 * the latter don't accidentally end up waiting for
		 * our writeback to complete. Refer to the comment in
		 * zpl_fsync() (when HAVE_FSYNC_RANGE is defined) for details.
		 */
		commit = B_TRUE;
	}

	zfs_log_write(zfsvfs->z_log, tx, TX_WRITE, zp, pgoff, pglen, commit,
	    B_FALSE, for_sync ? zfs_putpage_sync_commit_cb :
	    zfs_putpage_async_commit_cb, pp);

	dmu_tx_commit(tx);

	zfs_rangelock_exit(lr);

	if (commit)
		zil_commit(zfsvfs->z_log, zp->z_id);

	dataset_kstats_update_write_kstats(&zfsvfs->z_kstat, pglen);

	zfs_exit(zfsvfs, FTAG);
	return (err);
}

/*
 * Update the system attributes when the inode has been dirtied.  For the
 * moment we only update the mode, atime, mtime, and ctime.
 */
int
zfs_dirty_inode(struct inode *ip, int flags)
{
	znode_t		*zp = ITOZ(ip);
	zfsvfs_t	*zfsvfs = ITOZSB(ip);
	dmu_tx_t	*tx;
	uint64_t	mode, atime[2], mtime[2], ctime[2];
	inode_timespec_t tmp_ts;
	sa_bulk_attr_t	bulk[4];
	int		error = 0;
	int		cnt = 0;

	if (zfs_is_readonly(zfsvfs) || dmu_objset_is_snapshot(zfsvfs->z_os))
		return (0);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

#ifdef I_DIRTY_TIME
	/*
	 * This is the lazytime semantic introduced in Linux 4.0
	 * This flag will only be called from update_time when lazytime is set.
	 * (Note, I_DIRTY_SYNC will also set if not lazytime)
	 * Fortunately mtime and ctime are managed within ZFS itself, so we
	 * only need to dirty atime.
	 */
	if (flags == I_DIRTY_TIME) {
		zp->z_atime_dirty = B_TRUE;
		goto out;
	}
#endif

	tx = dmu_tx_create(zfsvfs->z_os);

	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		goto out;
	}

	mutex_enter(&zp->z_lock);
	zp->z_atime_dirty = B_FALSE;

	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MODE(zfsvfs), NULL, &mode, 8);
	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_ATIME(zfsvfs), NULL, &atime, 16);
	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MTIME(zfsvfs), NULL, &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_CTIME(zfsvfs), NULL, &ctime, 16);

	/* Preserve the mode, mtime and ctime provided by the inode */
	tmp_ts = zpl_inode_get_atime(ip);
	ZFS_TIME_ENCODE(&tmp_ts, atime);
	tmp_ts = zpl_inode_get_mtime(ip);
	ZFS_TIME_ENCODE(&tmp_ts, mtime);
	tmp_ts = zpl_inode_get_ctime(ip);
	ZFS_TIME_ENCODE(&tmp_ts, ctime);
	mode = ip->i_mode;

	zp->z_mode = mode;

	error = sa_bulk_update(zp->z_sa_hdl, bulk, cnt, tx);
	mutex_exit(&zp->z_lock);

	dmu_tx_commit(tx);
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

void
zfs_inactive(struct inode *ip)
{
	znode_t	*zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	uint64_t atime[2];
	int error;
	int need_unlock = 0;

	/* Only read lock if we haven't already write locked, e.g. rollback */
	if (!RW_WRITE_HELD(&zfsvfs->z_teardown_inactive_lock)) {
		need_unlock = 1;
		rw_enter(&zfsvfs->z_teardown_inactive_lock, RW_READER);
	}
	if (zp->z_sa_hdl == NULL) {
		if (need_unlock)
			rw_exit(&zfsvfs->z_teardown_inactive_lock);
		return;
	}

	if (zp->z_atime_dirty && zp->z_unlinked == B_FALSE) {
		dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);

		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, zp);
		error = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (error) {
			dmu_tx_abort(tx);
		} else {
			inode_timespec_t tmp_atime;
			tmp_atime = zpl_inode_get_atime(ip);
			ZFS_TIME_ENCODE(&tmp_atime, atime);
			mutex_enter(&zp->z_lock);
			(void) sa_update(zp->z_sa_hdl, SA_ZPL_ATIME(zfsvfs),
			    (void *)&atime, sizeof (atime), tx);
			zp->z_atime_dirty = B_FALSE;
			mutex_exit(&zp->z_lock);
			dmu_tx_commit(tx);
		}
	}

	zfs_zinactive(zp);
	if (need_unlock)
		rw_exit(&zfsvfs->z_teardown_inactive_lock);
}

/*
 * Fill pages with data from the disk.
 */
static int
zfs_fillpage(struct inode *ip, struct page *pp)
{
	znode_t *zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	loff_t i_size = i_size_read(ip);
	u_offset_t io_off = page_offset(pp);
	size_t io_len = PAGE_SIZE;

	ASSERT3U(io_off, <, i_size);

	if (io_off + io_len > i_size)
		io_len = i_size - io_off;

	void *va = kmap(pp);
	int error = dmu_read(zfsvfs->z_os, zp->z_id, io_off,
	    io_len, va, DMU_READ_PREFETCH);
	if (io_len != PAGE_SIZE)
		memset((char *)va + io_len, 0, PAGE_SIZE - io_len);
	kunmap(pp);

	if (error) {
		/* convert checksum errors into IO errors */
		if (error == ECKSUM)
			error = SET_ERROR(EIO);

		SetPageError(pp);
		ClearPageUptodate(pp);
	} else {
		ClearPageError(pp);
		SetPageUptodate(pp);
	}

	return (error);
}

/*
 * Uses zfs_fillpage to read data from the file and fill the page.
 *
 *	IN:	ip	 - inode of file to get data from.
 *		pp	 - page to read
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	vp - atime updated
 */
int
zfs_getpage(struct inode *ip, struct page *pp)
{
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	znode_t *zp = ITOZ(ip);
	int error;
	loff_t i_size = i_size_read(ip);
	u_offset_t io_off = page_offset(pp);
	size_t io_len = PAGE_SIZE;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	ASSERT3U(io_off, <, i_size);

	if (io_off + io_len > i_size)
		io_len = i_size - io_off;

	/*
	 * It is important to hold the rangelock here because it is possible
	 * a Direct I/O write or block clone might be taking place at the same
	 * time that a page is being faulted in through filemap_fault(). With
	 * Direct I/O writes and block cloning db->db_data will be set to NULL
	 * with dbuf_clear_data() in dmu_buif_will_clone_or_dio(). If the
	 * rangelock is not held, then there is a race between faulting in a
	 * page and writing out a Direct I/O write or block cloning. Without
	 * the rangelock a NULL pointer dereference can occur in
	 * dmu_read_impl() for db->db_data during the mempcy operation when
	 * zfs_fillpage() calls dmu_read().
	 */
	zfs_locked_range_t *lr = zfs_rangelock_tryenter(&zp->z_rangelock,
	    io_off, io_len, RL_READER);
	if (lr == NULL) {
		/*
		 * It is important to drop the page lock before grabbing the
		 * rangelock to avoid another deadlock between here and
		 * zfs_write() -> update_pages(). update_pages() holds both the
		 * rangelock and the page lock.
		 */
		get_page(pp);
		unlock_page(pp);
		lr = zfs_rangelock_enter(&zp->z_rangelock, io_off,
		    io_len, RL_READER);
		lock_page(pp);
		put_page(pp);
	}
	error = zfs_fillpage(ip, pp);
	zfs_rangelock_exit(lr);

	if (error == 0)
		dataset_kstats_update_read_kstats(&zfsvfs->z_kstat, PAGE_SIZE);

	zfs_exit(zfsvfs, FTAG);

	return (error);
}

/*
 * Check ZFS specific permissions to memory map a section of a file.
 *
 *	IN:	ip	- inode of the file to mmap
 *		off	- file offset
 *		addrp	- start address in memory region
 *		len	- length of memory region
 *		vm_flags- address flags
 *
 *	RETURN:	0 if success
 *		error code if failure
 */
int
zfs_map(struct inode *ip, offset_t off, caddr_t *addrp, size_t len,
    unsigned long vm_flags)
{
	(void) addrp;
	znode_t  *zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ITOZSB(ip);
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((vm_flags & VM_WRITE) && (vm_flags & VM_SHARED) &&
	    (zp->z_pflags & (ZFS_IMMUTABLE | ZFS_READONLY | ZFS_APPENDONLY))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((vm_flags & (VM_READ | VM_EXEC)) &&
	    (zp->z_pflags & ZFS_AV_QUARANTINED)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EACCES));
	}

	if (off < 0 || len > MAXOFFSET_T - off) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ENXIO));
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

/*
 * Free or allocate space in a file.  Currently, this function only
 * supports the `F_FREESP' command.  However, this command is somewhat
 * misnamed, as its functionality includes the ability to allocate as
 * well as free space.
 *
 *	IN:	zp	- znode of file to free data in.
 *		cmd	- action to take (only F_FREESP supported).
 *		bfp	- section of file to free/alloc.
 *		flag	- current file open mode flags.
 *		offset	- current file offset.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	zp - ctime|mtime updated
 */
int
zfs_space(znode_t *zp, int cmd, flock64_t *bfp, int flag,
    offset_t offset, cred_t *cr)
{
	(void) offset;
	zfsvfs_t	*zfsvfs = ZTOZSB(zp);
	uint64_t	off, len;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (cmd != F_FREESP) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Callers might not be able to detect properly that we are read-only,
	 * so check it explicitly here.
	 */
	if (zfs_is_readonly(zfsvfs)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	if (bfp->l_len < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Permissions aren't checked on Solaris because on this OS
	 * zfs_space() can only be called with an opened file handle.
	 * On Linux we can get here through truncate_range() which
	 * operates directly on inodes, so we need to check access rights.
	 */
	if ((error = zfs_zaccess(zp, ACE_WRITE_DATA, 0, B_FALSE, cr,
	    zfs_init_idmap))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	off = bfp->l_start;
	len = bfp->l_len; /* 0 means from off to end of file */

	error = zfs_freesp(zp, off, len, flag, TRUE);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_fid(struct inode *ip, fid_t *fidp)
{
	znode_t		*zp = ITOZ(ip);
	zfsvfs_t	*zfsvfs = ITOZSB(ip);
	uint32_t	gen;
	uint64_t	gen64;
	uint64_t	object = zp->z_id;
	zfid_short_t	*zfid;
	int		size, i, error;

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);

	if (fidp->fid_len < SHORT_FID_LEN) {
		fidp->fid_len = SHORT_FID_LEN;
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ENOSPC));
	}

	if ((error = zfs_verify_zp(zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(zfsvfs),
	    &gen64, sizeof (uint64_t))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	gen = (uint32_t)gen64;

	size = SHORT_FID_LEN;

	zfid = (zfid_short_t *)fidp;

	zfid->zf_len = size;

	for (i = 0; i < sizeof (zfid->zf_object); i++)
		zfid->zf_object[i] = (uint8_t)(object >> (8 * i));

	/* Must have a non-zero generation number to distinguish from .zfs */
	if (gen == 0)
		gen = 1;
	for (i = 0; i < sizeof (zfid->zf_gen); i++)
		zfid->zf_gen[i] = (uint8_t)(gen >> (8 * i));

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

/*
 * Convert the provided block pointer in to an extent.  This may result in
 * a new extent being created or an existing extent being extended.
 */
static int
zfs_fiemap_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	zfs_fiemap_t *fm = (zfs_fiemap_t *)arg;
	blkptr_t bp_copy = *bp;

	if (BP_GET_LEVEL(bp) != 0)
		return (0);

	/*
	 * Indirect block pointers must be remapped to reflect the real
	 * physical offset and length.  The remapping is transparent to
	 * the fiemap interface so no additional extent flags are set.
	 */
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	if (spa_remap_blkptr(spa, &bp_copy, NULL, NULL))
		bp = &bp_copy;
	spa_config_exit(spa, SCL_VDEV, FTAG);

	for (int i = 0; i < fm->fm_copies; i++) {
		zfs_fiemap_entry_t *fe, *pfe;
		avl_index_t idx;

		/*
		 * N.B. Embedded block pointers and holes are only added to
		 * the fm_extents_trees[0], the additional trees are used
		 * for redundant copies of data blocks.
		 */
		if (i > 0 && (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp)))
			continue;

		fe = kmem_zalloc(sizeof (zfs_fiemap_entry_t), KM_SLEEP);
		fe->fe_logical_start = zb->zb_blkid * fm->fm_block_size;

		if (BP_IS_HOLE(bp)) {
			fe->fe_logical_len = fm->fm_block_size;
			fe->fe_flags |= FIEMAP_EXTENT_UNWRITTEN;
		} else if (BP_IS_EMBEDDED(bp)) {
			fe->fe_logical_len = BPE_GET_LSIZE(bp);
			fe->fe_physical_start = 0;
			fe->fe_physical_len = BPE_GET_PSIZE(bp);
			fe->fe_flags |= FIEMAP_EXTENT_DATA_INLINE |
			    FIEMAP_EXTENT_NOT_ALIGNED;

			if (BP_IS_ENCRYPTED(bp))
				fe->fe_flags |= FIEMAP_EXTENT_DATA_ENCRYPTED;
			if (BP_GET_COMPRESS(bp) != ZIO_COMPRESS_OFF)
				fe->fe_flags |= FIEMAP_EXTENT_ENCODED;
		} else {
			if (i >= BP_GET_NDVAS(bp)) {
				kmem_free(fe, sizeof (zfs_fiemap_entry_t));
				continue;
			}

			if (BP_IS_ENCRYPTED(bp))
				fe->fe_flags |= FIEMAP_EXTENT_DATA_ENCRYPTED;
			if (BP_GET_COMPRESS(bp) != ZIO_COMPRESS_OFF)
				fe->fe_flags |= FIEMAP_EXTENT_ENCODED;
			if (BP_GET_DEDUP(bp))
				fe->fe_flags |= FIEMAP_EXTENT_SHARED;

			/*
			 * Report gang blocks as a single unknown extent.
			 * Ideally we should be walking the gang block tree and
			 * reporting all component-blocks as physical extents.
			 */
			if (BP_IS_GANG(bp)) {
				fe->fe_flags |= FIEMAP_EXTENT_UNKNOWN;
				fe->fe_physical_start = 0;
				fe->fe_physical_len = 0;
				fe->fe_vdev = 0;
			} else {
				fe->fe_physical_len = BP_GET_PSIZE(bp);

				if (DVA_IS_VALID(&bp->blk_dva[i])) {
					fe->fe_vdev =
					    DVA_GET_VDEV(&bp->blk_dva[i]);
					fe->fe_physical_start =
					    DVA_GET_OFFSET(&bp->blk_dva[i]);
				}
			}

			fe->fe_logical_len = BP_GET_LSIZE(bp);
		}

		/*
		 * By default merge compatible adjacent block pointers in to a
		 * single extent.  Embedded block pointers can never be merged.
		 *
		 * N.B. Block pointers provided by the iterator will always
		 * be in logical offset order.  Therefore, it is sufficient
		 * to check only the previously inserted entry when merging.
		 */
		pfe = avl_last(&fm->fm_extent_trees[i]);
		if (pfe != NULL && !BP_IS_EMBEDDED(bp) &&
		    !(fm->fm_flags & FIEMAP_FLAG_NOMERGE)) {
			ASSERT3U(pfe->fe_logical_start + pfe->fe_logical_len,
			    ==, fe->fe_logical_start);

			if (BP_IS_HOLE(bp) && fe->fe_flags ==
			    (pfe->fe_flags & ~FIEMAP_EXTENT_MERGED)) {
				pfe->fe_logical_len += fe->fe_logical_len;
				pfe->fe_flags |= FIEMAP_EXTENT_MERGED;
				kmem_free(fe, sizeof (zfs_fiemap_entry_t));
				continue;
			}

			if (!BP_IS_HOLE(bp) && fe->fe_flags ==
			    (pfe->fe_flags & ~FIEMAP_EXTENT_MERGED) &&
			    fe->fe_physical_start ==
			    pfe->fe_physical_start + pfe->fe_physical_len &&
			    fe->fe_vdev == pfe->fe_vdev) {
				pfe->fe_logical_len += fe->fe_logical_len;
				pfe->fe_physical_len += fe->fe_physical_len;
				pfe->fe_flags |= FIEMAP_EXTENT_MERGED;
				kmem_free(fe, sizeof (zfs_fiemap_entry_t));
				continue;
			}
		}

		/*
		 * The FIEMAP documentation specifies that all encrypted
		 * extents must also set the encoded flag.
		 */
		if (fe->fe_flags & FIEMAP_EXTENT_DATA_ENCRYPTED)
			fe->fe_flags |= FIEMAP_EXTENT_ENCODED;

		/*
		 * Add the new extent to the copies tree.  This should never
		 * conflict with an existing logical extent, but is handled
		 * none the less by discarding the overlapping extent.
		 */
		if (avl_find(&fm->fm_extent_trees[i], fe, &idx) == NULL) {
			avl_insert(&fm->fm_extent_trees[i], fe, idx);
		} else {
			kmem_free(fe, sizeof (zfs_fiemap_entry_t));
		}
	}

	return (0);
}

/*
 * Recursively walk the indirect block tree for a dnode_phys_t and call
 * the provided callback for all block pointers traversed.
 */
static int
zfs_fiemap_visit_indirect(spa_t *spa, const dnode_phys_t *dnp,
    blkptr_t *bp, const zbookmark_phys_t *zb,
    blkptr_cb_t func, void *arg)
{
	int error = 0;

	if (zb->zb_blkid > dnp->dn_maxblkid)
		return (0);

	error = func(spa, NULL, bp, zb, dnp, arg);
	if (error)
		return (error);

	if (BP_GET_LEVEL(bp) > 0 && !BP_IS_HOLE(bp)) {
		arc_flags_t flags = ARC_FLAG_WAIT;
		blkptr_t *cbp;
		int epb = BP_GET_LSIZE(bp) >> SPA_BLKPTRSHIFT;
		arc_buf_t *buf;

		error = arc_read(NULL, spa, bp, arc_getbuf_func, &buf,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &flags, zb);
		if (error)
			return (error);

		cbp = buf->b_data;
		for (int i = 0; i < epb; i++, cbp++) {
			zbookmark_phys_t czb;

			SET_BOOKMARK(&czb, zb->zb_objset, zb->zb_object,
			    zb->zb_level - 1, zb->zb_blkid * epb + i);
			error = zfs_fiemap_visit_indirect(spa, dnp, cbp, &czb,
			    func, arg);
			if (error)
				break;
		}

		arc_buf_destroy(buf, &buf);
	}

	return (error);
}

/*
 * Allocate and insert a new extent.  Duplicates are never allowed so make
 * sure to clear the range with zfs_fiemap_clear() as needed.
 */
static void
zfs_fiemap_add_impl(avl_tree_t *t, uint64_t logical_start,
    uint64_t logical_len, uint64_t physical_start, uint64_t physical_len,
    uint64_t vdev, uint64_t flags)
{
	zfs_fiemap_entry_t *fe;

	fe = kmem_zalloc(sizeof (zfs_fiemap_entry_t), KM_SLEEP);
	fe->fe_logical_start = logical_start;
	fe->fe_logical_len = logical_len;
	fe->fe_physical_start = physical_start;
	fe->fe_physical_len = physical_len;
	fe->fe_vdev = vdev;
	fe->fe_flags = flags;

	avl_add(t, fe);
}

/*
 * Clear a range from the extent tree.  This allows new extents to be
 * added to the cleared region.
 */
static void
zfs_fiemap_clear(avl_tree_t *t, uint64_t start, uint64_t len)
{
	zfs_fiemap_entry_t search;
	zfs_fiemap_entry_t *fe, *next_fe;
	avl_index_t idx;
	uint64_t end = start + len;

	search.fe_logical_start = start;
	fe = avl_find(t, &search, &idx);
	if (fe == NULL) {
		fe = avl_nearest(t, idx, AVL_BEFORE);
		if (fe == NULL)
			fe = avl_first(t);
	}

	while (fe != NULL && fe->fe_logical_start < end) {
		uint64_t extent_len = fe->fe_logical_len;
		uint64_t extent_start = fe->fe_logical_start;
		uint64_t extent_end = extent_start + extent_len;

		/*
		 * Region to be cleared does not overlap the extent.
		 */
		if (extent_end <= start || extent_start >= end) {
			fe = AVL_NEXT(t, fe);
			continue;
		/*
		 * Region to be cleared overlaps with the end of an extent.
		 * Truncate the extent to the new correct length.
		 */
		} else if (extent_start < start && extent_end <= end) {
			fe->fe_logical_len = start - extent_start;
		/*
		 * Extent fits entirely within the region to be cleared.
		 * It can be entirely removed and freed.
		 */
		} else if (extent_start >= start && extent_end <= end) {
			next_fe = AVL_NEXT(t, fe);
			avl_remove(t, fe);
			kmem_free(fe, sizeof (zfs_fiemap_entry_t));
			fe = next_fe;
			continue;
		/*
		 * Region to be cleared overlaps with the start of an extent.
		 * Advance the starting offset of the extent and re-size.
		 */
		} else if (extent_start >= start && extent_end > end) {
			fe->fe_logical_len = extent_end - end;
			fe->fe_logical_start = end;
		/*
		 * Extent spans before and after the region to be clearer.
		 * Split the extent in to a before and after portion.
		 */
		} else if (extent_start < start && extent_end > end) {
			fe->fe_logical_len = start - extent_start;
			zfs_fiemap_add_impl(t, end, extent_end - end,
			    0, 0, fe->fe_vdev, fe->fe_flags);
		} else {
			fe = AVL_NEXT(t, fe);
			continue;
		}

		/*
		 * Zero the physical start and length which are no longer
		 * meaningful after modifying the logical start or length.
		 *
		 * N.B. Ideally we should keep a list the block pointers
		 * comprising the extent.  This would allow us to properly
		 * trim it and correctly update the physical start and length.
		 */
		fe->fe_physical_start = 0;
		fe->fe_physical_len = 0;

		fe = AVL_NEXT(t, fe);
	}
}

/*
 * Pending dirty extents set FIEMAP_EXTENT_DELALLOC to indicate they have
 * not yet been written.  The FIEMAP_EXTENT_UNKNOWN flag must be set when
 * FIEMAP_EXTENT_DELALLOC is set.  Dirty extents are only inserted in to
 * the first extent tree.
 */
static void
zfs_fiemap_add_dirty(void *arg, uint64_t start, uint64_t size)
{
	zfs_fiemap_t *fm = (zfs_fiemap_t *)arg;
	avl_tree_t *t = &fm->fm_extent_trees[0];

	zfs_fiemap_clear(t, start, size);

	if (fm->fm_flags & FIEMAP_FLAG_NOMERGE) {
		uint64_t blksz = fm->fm_block_size;

		for (uint64_t i = start; i < start + size; i += blksz) {
			zfs_fiemap_add_impl(t, i, blksz, 0, 0, 0,
			    FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_UNKNOWN);
		}
	} else {
		zfs_fiemap_add_impl(t, start, size, 0, 0, 0,
		    FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_UNKNOWN |
		    FIEMAP_EXTENT_MERGED);
	}
}

/*
 * Pending free extents set FIEMAP_EXTENT_UNWRITTEN since they will be a hole.
 * FIEMAP_EXTENT_DELALLOC is set to indicate it has not yet been written.  The
 * FIEMAP_EXTENT_UNKNOWN flag must be set when FIEMAP_EXTENT_DELALLOC is set.
 * Free extents are only inserted in to the first extent tree.
 */
static void
zfs_fiemap_add_free(void *arg, uint64_t start, uint64_t size)
{
	zfs_fiemap_t *fm = (zfs_fiemap_t *)arg;
	avl_tree_t *t = &fm->fm_extent_trees[0];

	zfs_fiemap_clear(t, start, size);

	if (fm->fm_flags & FIEMAP_FLAG_NOMERGE) {
		uint64_t blksz = fm->fm_block_size;

		for (uint64_t i = start; i < start + size; i += blksz) {
			zfs_fiemap_add_impl(t, i, blksz, 0, 0, 0,
			    FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_DELALLOC |
			    FIEMAP_EXTENT_UNKNOWN);
		}
	} else {
		zfs_fiemap_add_impl(t, start, size, 0, 0, 0,
		    FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_DELALLOC |
		    FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_MERGED);
	}
}

/*
 * The entire file is sparse and there are no level zero blocks with data.
 * In this case pretend that hole block pointers exist to maintain consistency
 * in the reported output.  Either add a single unwritten extent for the
 * entire length of the file.  Or when no merging is requested add the
 * correct number of hole block pointers.  Only the first extent tree should
 * be populated since only holes are being added.
 */
static void
zfs_fiemap_add_sparse(zfs_fiemap_t *fm)
{
	avl_tree_t *t = &fm->fm_extent_trees[0];
	uint64_t blksz = fm->fm_block_size;
	uint64_t size = P2ROUNDUP(fm->fm_file_size, blksz);

	if (fm->fm_flags & FIEMAP_FLAG_NOMERGE) {
		for (uint64_t i = 0; i < size; i += blksz) {
			zfs_fiemap_add_impl(t, i, blksz, 0, 0, 0,
			    FIEMAP_EXTENT_UNWRITTEN);
		}
	} else {
		zfs_fiemap_add_impl(t, 0, size, 0, 0, 0,
		    size == blksz ? FIEMAP_EXTENT_UNWRITTEN :
		    FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_MERGED);
	}
}

/*
 * Walk the block pointers for the provided object and assemble a tree
 * of extents which describe the logical to physical mapping.  Additionally
 * include dirty buffers for the object which will be written but have
 * not yet have had space allocated on disk.
 */
int
zfs_fiemap_assemble(struct inode *ip, zfs_fiemap_t *fm)
{
	znode_t *zp = ITOZ(ip);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	zbookmark_phys_t czb;
	txg_handle_t th;
	dnode_t *dn;
	spa_t *spa;
	uint64_t open_txg, syncing_txg, dirty_txg;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	error = dnode_hold(zfsvfs->z_os, zp->z_id, FTAG, &dn);
	if (error) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	spa = dmu_objset_spa(dn->dn_objset);

	if (fm->fm_flags & FIEMAP_FLAG_SYNC)
		txg_wait_synced(spa_get_dsl(spa), 0);

	/*
	 * Lock the entire file against changes while assembling the FIEMAP.
	 * Then hold open the TXG while generating a map of all pending frees
	 * and dirty blocks.  This isn't strictly necessary but it is a
	 * convenient way to determine the range of TXGs to check.
	 */
	zfs_locked_range_t *lr = zfs_rangelock_enter(&zp->z_rangelock, 0,
	    UINT64_MAX, RL_READER);
	open_txg = txg_hold_open(spa_get_dsl(spa), &th);
	syncing_txg = dirty_txg = spa_syncing_txg(spa);

	(void) dbuf_generate_dirty_maps(dn, fm->fm_dirty_tree,
	    fm->fm_free_tree, &dirty_txg, open_txg);

	/*
	 * When the currently syncing TXG could not be checked, likely because
	 * the dnode was already synced, we need to wait for the syncing TXG
	 * to fully complete in order to avoid using stale block pointers.
	 */
	if (dirty_txg > syncing_txg)
		txg_wait_synced(spa_get_dsl(spa), syncing_txg);

	rw_enter(&dn->dn_struct_rwlock, RW_READER);
	mutex_enter(&dn->dn_mtx);

	dnode_phys_t *dnp = dn->dn_phys;
	fm->fm_file_size = i_size_read(ip);
	fm->fm_block_size = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	fm->fm_fill_count = 0;

	/*
	 * When there are only pending dirty buffers the block size will not
	 * yet have been determined.  Assume the maximum block size.
	 */
	if (fm->fm_block_size == 0)
		fm->fm_block_size = zfsvfs->z_max_blksz;

	/*
	 * When FIEMAP_FLAG_NOMERGE is set and the number of extents for the
	 * entire file are being requested.  Then in this special case walking
	 * the entire indirect block tree is not required.
	 *
	 * The fill count can be used to determine the number of extents, all
	 * dirty blocks are assumed to fill holes, and free blocks are assumed
	 * to overlap with existing free blocks.  This is a safe worst case
	 * estimate which may slightly over report the number of extents for
	 * a file being actively overwritten.
	 *
	 * Otherwise, the entire block tree needs to be walked to determine
	 * exactly how the block pointer will be merged.
	 */
	if (fm->fm_extents_max == 0 && fm->fm_flags & FIEMAP_FLAG_NOMERGE &&
	    fm->fm_start == 0 && fm->fm_length == FIEMAP_MAX_OFFSET) {
		for (int i = 0; i < MIN(dnp->dn_nblkptr, fm->fm_copies); i++)
			fm->fm_fill_count += BP_GET_FILL(&dnp->dn_blkptr[i]);

		fm->fm_fill_count += P2ROUNDUP(zfs_range_tree_space(
		    fm->fm_dirty_tree), fm->fm_block_size) / fm->fm_block_size;
	} else {
		SET_BOOKMARK(&czb, dmu_objset_id(dn->dn_objset),
		    dn->dn_object, dnp->dn_nlevels - 1, 0);

		for (int i = 0; i < MIN(dnp->dn_nblkptr, fm->fm_copies); i++) {
			blkptr_t *bp = &dnp->dn_blkptr[i];

			if (BP_GET_FILL(bp) > 0) {
				czb.zb_blkid = i;
				error = zfs_fiemap_visit_indirect(spa, dnp, bp,
				    &czb, zfs_fiemap_cb, (void *)fm);
			} else {
				zfs_fiemap_add_sparse(fm);
			}
		}

		for (int i = 0; i < fm->fm_copies; i++) {
			avl_tree_t *t = &fm->fm_extent_trees[i];
			zfs_fiemap_entry_t *fe;

			if (i == 0) {
				zfs_range_tree_walk(fm->fm_dirty_tree,
				    zfs_fiemap_add_dirty, fm);
				zfs_range_tree_walk(fm->fm_free_tree,
				    zfs_fiemap_add_free, fm);
			}

			if ((fe = avl_last(t)) != NULL)
				fe->fe_flags |= FIEMAP_EXTENT_LAST;
		}
	}

	mutex_exit(&dn->dn_mtx);
	rw_exit(&dn->dn_struct_rwlock);

	txg_rele_to_quiesce(&th);
	txg_rele_to_sync(&th);
	zfs_rangelock_exit(lr);

	dnode_rele(dn, FTAG);
	zfs_exit(zfsvfs, FTAG);

	return (error);
}

/*
 * Fill the fiemap_extent_info structure with an extent.  It has been
 * requested that the following fields be reserved in future kernels.
 *
 * - fe_physical_len - reserved for physical length
 * - fe_device - reserved for device identifier
 *
 * Returns:
 *   ESRCH  - FIEMAP_EXTENT_LAST entry added
 *   ENOSPC - additional entries cannot be added
 *   EFAULT - bad address
 */
static int
zfs_fiemap_fill_next_extent(zfs_fiemap_t *fm, struct fiemap_extent_info *fei,
    uint64_t logical_start, uint64_t physical_start,
    uint64_t logical_len, uint64_t physical_len,
    uint32_t device, uint32_t flags)
{
	boolean_t is_last = !!(flags & FIEMAP_EXTENT_LAST);
	int error;

	if (fei->fi_extents_max == 0) {
		fei->fi_extents_mapped++;
		return (is_last ? SET_ERROR(ESRCH) : 0);
	}

	if (fei->fi_extents_mapped >= fei->fi_extents_max)
		return (SET_ERROR(ENOSPC));

	uint64_t end = logical_start + logical_len;
	if (end > fm->fm_file_size)
		logical_len = logical_len - (end - fm->fm_file_size);

	struct fiemap_extent extent;
	memset(&extent, 0, sizeof (extent));
	extent.fe_logical = logical_start;
	extent.fe_physical = physical_start;
	extent.fe_length = logical_len;
	extent.fe_physical_length_reserved = physical_len;
	extent.fe_flags = flags;
	extent.fe_device_reserved = device;

	error = copy_to_user(fei->fi_extents_start + fei->fi_extents_mapped,
	    &extent, sizeof (extent));
	if (error)
		return (SET_ERROR(EFAULT));

	fei->fi_extents_mapped++;
	if (fei->fi_extents_mapped >= fei->fi_extents_max)
		return (SET_ERROR(ENOSPC));

	return (is_last ? SET_ERROR(ESRCH) : 0);
}

/*
 * Inclusively add all data and holes extents in the requested range from
 * the assembled zfs_fiemap_tree to the user fiemap_extent_info.
 */
static int
zfs_fiemap_tree_fill(zfs_fiemap_t *fm, int idx, struct fiemap_extent_info *fei,
    uint64_t start, uint64_t length)
{
	avl_tree_t *t = &fm->fm_extent_trees[idx];
	zfs_fiemap_entry_t *fe;
	boolean_t skip_holes = B_TRUE;
	int error = 0;

	if (fm->fm_flags & FIEMAP_FLAG_HOLES)
		skip_holes = B_FALSE;

	if (start == 0) {
		fe = avl_first(t);
	} else {
		zfs_fiemap_entry_t search;
		avl_index_t idx;

		search.fe_logical_start = start;
		fe = avl_find(t, &search, &idx);
		if (fe == NULL)
			fe = avl_nearest(t, idx, AVL_BEFORE);
		if (fe == NULL)
			fe = avl_first(t);
	}

	while (fe != NULL) {

		if (skip_holes && fe->fe_flags & FIEMAP_EXTENT_UNWRITTEN) {
			fe = AVL_NEXT(t, fe);
			continue;
		}

		if (fe->fe_logical_start > start + length)
			return (SET_ERROR(ESRCH));

		error = zfs_fiemap_fill_next_extent(fm, fei,
		    fe->fe_logical_start, fe->fe_physical_start,
		    fe->fe_logical_len, fe->fe_physical_len,
		    fe->fe_vdev, fe->fe_flags);
		if (error)
			return (error);

		fe = AVL_NEXT(t, fe);
	}

	return (error);
}

/*
 * Given the requested logical starting offset and length, find all inclusive
 * extents and populate the provided fiemap_extent_info.  For compatibility,
 * the default behavior is to only report extents using a block pointer's
 * first DVA.  When the FIEMAP_FLAG_COPIES is set all extents are reported.
 */
int
zfs_fiemap_fill(zfs_fiemap_t *fm, struct fiemap_extent_info *fei,
    uint64_t start, uint64_t length)
{
	int error = 0;

	/*
	 * See FIEMAP_FLAG_NOMERGE comment block in zfs_fiemap_assemble().
	 */
	if (fm->fm_extents_max == 0 && fm->fm_flags & FIEMAP_FLAG_NOMERGE &&
	    fm->fm_start == 0 && fm->fm_length == FIEMAP_MAX_OFFSET) {
		fei->fi_extents_mapped = fm->fm_fill_count;
		return (0);
	}

	if (fm->fm_flags & FIEMAP_FLAG_COPIES) {
		for (int i = 0; i < fm->fm_copies; i++) {
			error = zfs_fiemap_tree_fill(fm, i, fei, start, length);
			if (error == ESRCH)
				continue;
			else if (error)
				break;
		}
	} else {
		error = zfs_fiemap_tree_fill(fm, 0, fei, start, length);
	}

	if (error == ESRCH || error == ENOSPC)
		return (0);

	return (error);
}

/*
 * Comparison function for FIEMAP extent trees.
 */
static int
zfs_fiemap_compare(const void *x1, const void *x2)
{
	const zfs_fiemap_entry_t *fe1 = (const zfs_fiemap_entry_t *)x1;
	const zfs_fiemap_entry_t *fe2 = (const zfs_fiemap_entry_t *)x2;

	return (TREE_CMP(fe1->fe_logical_start, fe2->fe_logical_start));
}

/*
 * Allocate a zfs_fiemap_t which contains the extent trees.
 */
zfs_fiemap_t *
zfs_fiemap_create(uint64_t start, uint64_t len, uint64_t flags, uint64_t max)
{
	zfs_fiemap_t *fm;

	fm = kmem_zalloc(sizeof (zfs_fiemap_t), KM_SLEEP);
	fm->fm_copies = 1;
	fm->fm_start = start;
	fm->fm_length = len;
	fm->fm_flags = flags;
	fm->fm_extents_max = max;

	if (fm->fm_flags & FIEMAP_FLAG_COPIES)
		fm->fm_copies = SPA_DVAS_PER_BP;

	for (int i = 0; i < SPA_DVAS_PER_BP; i++) {
		avl_create(&fm->fm_extent_trees[i], zfs_fiemap_compare,
		    sizeof (struct zfs_fiemap_entry),
		    offsetof(struct zfs_fiemap_entry, fe_node));
	}

	fm->fm_dirty_tree = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL,
	    start, 0);
	fm->fm_free_tree = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, start, 0);

	return (fm);
}

/*
 * Destroy a zfs_fiemap_t.
 */
void
zfs_fiemap_destroy(zfs_fiemap_t *fm)
{
	for (int i = 0; i < SPA_DVAS_PER_BP; i++) {
		avl_tree_t *t = &fm->fm_extent_trees[i];
		zfs_fiemap_entry_t *fe;
		void *cookie = NULL;

		while ((fe = avl_destroy_nodes(t, &cookie)) != NULL)
			kmem_free(fe, sizeof (zfs_fiemap_entry_t));

		avl_destroy(&fm->fm_extent_trees[i]);
	}

	zfs_range_tree_vacate(fm->fm_dirty_tree, NULL, NULL);
	zfs_range_tree_destroy(fm->fm_dirty_tree);

	zfs_range_tree_vacate(fm->fm_free_tree, NULL, NULL);
	zfs_range_tree_destroy(fm->fm_free_tree);

	kmem_free(fm, sizeof (zfs_fiemap_t));
}

#if defined(_KERNEL)
EXPORT_SYMBOL(zfs_open);
EXPORT_SYMBOL(zfs_close);
EXPORT_SYMBOL(zfs_lookup);
EXPORT_SYMBOL(zfs_create);
EXPORT_SYMBOL(zfs_tmpfile);
EXPORT_SYMBOL(zfs_remove);
EXPORT_SYMBOL(zfs_mkdir);
EXPORT_SYMBOL(zfs_rmdir);
EXPORT_SYMBOL(zfs_readdir);
EXPORT_SYMBOL(zfs_getattr_fast);
EXPORT_SYMBOL(zfs_setattr);
EXPORT_SYMBOL(zfs_rename);
EXPORT_SYMBOL(zfs_symlink);
EXPORT_SYMBOL(zfs_readlink);
EXPORT_SYMBOL(zfs_link);
EXPORT_SYMBOL(zfs_inactive);
EXPORT_SYMBOL(zfs_space);
EXPORT_SYMBOL(zfs_fid);
EXPORT_SYMBOL(zfs_getpage);
EXPORT_SYMBOL(zfs_putpage);
EXPORT_SYMBOL(zfs_dirty_inode);
EXPORT_SYMBOL(zfs_map);
EXPORT_SYMBOL(zfs_fiemap_create);
EXPORT_SYMBOL(zfs_fiemap_destroy);
EXPORT_SYMBOL(zfs_fiemap_assemble);
EXPORT_SYMBOL(zfs_fiemap_fill);

module_param(zfs_delete_blocks, ulong, 0644);
MODULE_PARM_DESC(zfs_delete_blocks, "Delete files larger than N blocks async");
#endif
