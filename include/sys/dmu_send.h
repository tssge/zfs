/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
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
 * Copyright (c) 2012, 2014 by Delphix. All rights reserved.
 * Copyright 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2013, Joyent, Inc. All rights reserved.
 */

#ifndef _DMU_SEND_H
#define	_DMU_SEND_H

#include <sys/inttypes.h>
#include <sys/dsl_crypt.h>
#include <sys/spa.h>
#include <sys/dsl_pool.h>
#include <sys/vnode.h>

struct vnode;
struct dsl_dataset;
struct drr_begin;
struct avl_tree;
struct dmu_replay_record;

int dmu_send(dsl_pool_t **dpp, dsl_dataset_t *ds, dsl_dataset_t *fromds,
    char *fromzb, boolean_t embedok, boolean_t large_block_ok,
    boolean_t compressok, boolean_t rawok,
    uint64_t resumeobj, uint64_t resumeoff,
    int outfd, void *tag);
int dmu_send_close(dsl_dataset_t *ds, void *dsa);
int dmu_send_estimate(struct dsl_dataset *ds, struct dsl_dataset *fromds,
    boolean_t stream_compressed, uint64_t *sizep);
int dmu_send_estimate_from_txg(struct dsl_dataset *ds, uint64_t fromtxg,
    boolean_t stream_compressed, uint64_t *sizep);

#endif /* _DMU_SEND_H */
