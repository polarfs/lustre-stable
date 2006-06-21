/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/fid/fid_request.c
 *  Lustre Sequence Manager
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Yury Umanets <umka@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_FID

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
#include "fid_internal.h"

/* client seq mgr interface */
static int 
seq_client_rpc(struct lu_client_seq *seq, 
               struct lu_range *range,
               __u32 opc)
{
        struct obd_export *exp = seq->seq_exp;
        int repsize = sizeof(struct lu_range);
        int rc, reqsize = sizeof(__u32);
        struct ptlrpc_request *req;
        struct lu_range *ran;
        __u32 *op;
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), 
			      LUSTRE_MDS_VERSION,
                              SEQ_QUERY, 1, &reqsize,
                              NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        op = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*op));
        *op = opc;

        req->rq_replen = lustre_msg_size(1, &repsize);
        req->rq_request_portal = MDS_SEQ_PORTAL;

        rc = ptlrpc_queue_wait(req);
        if (rc)
                GOTO(out_req, rc);

        ran = lustre_swab_repbuf(req, 0, sizeof(*ran),
                                 lustre_swab_lu_range);

        if (ran == NULL) {
                CERROR("invalid range is returned\n");
                GOTO(out_req, rc = -EPROTO);
        }
        *range = *ran;
        
        LASSERT(range_is_sane(range));
        LASSERT(!range_is_exhausted(range));
        
        EXIT;
out_req:
        ptlrpc_req_finished(req); 
        return rc;
}

/* request sequence-controller node to allocate new super-sequence. */
static int
__seq_client_alloc_super(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        rc = seq_client_rpc(seq, &seq->seq_range, SEQ_ALLOC_SUPER);
        if (rc == 0) {
                CDEBUG(D_INFO|D_WARNING, "SEQ-MGR(cli): allocated super-sequence "
                       "["LPX64"-"LPX64"]\n", seq->seq_range.lr_start,
                       seq->seq_range.lr_end);
        }
        RETURN(rc);
}

int
seq_client_alloc_super(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;
        
        down(&seq->seq_sem);
        rc = __seq_client_alloc_super(seq);
        up(&seq->seq_sem);

        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_super);

/* request sequence-controller node to allocate new meta-sequence. */
static int
__seq_client_alloc_meta(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        rc = seq_client_rpc(seq, &seq->seq_range, SEQ_ALLOC_META);
        if (rc == 0) {
                CDEBUG(D_INFO|D_WARNING, "SEQ-MGR(cli): allocated meta-sequence "
                       "["LPX64"-"LPX64"]\n", seq->seq_range.lr_start,
                       seq->seq_range.lr_end);
        }
        RETURN(rc);
}

int
seq_client_alloc_meta(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        down(&seq->seq_sem);
        rc = __seq_client_alloc_meta(seq);
        up(&seq->seq_sem);

        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_meta);

/* allocate new sequence for client (llite or MDC are expected to use this) */
static int
__seq_client_alloc_seq(struct lu_client_seq *seq, __u64 *seqnr)
{
        int rc = 0;
        ENTRY;

        LASSERT(range_is_sane(&seq->seq_range));

        /* if we still have free sequences in meta-sequence we allocate new seq
         * from given range, if not - allocate new meta-sequence. */
        if (range_space(&seq->seq_range) == 0) {
                rc = __seq_client_alloc_meta(seq);
                if (rc) {
                        CERROR("can't allocate new meta-sequence, "
                               "rc %d\n", rc);
                }
        }
        
        *seqnr = seq->seq_range.lr_start;
        seq->seq_range.lr_start += 1;
        
        if (rc == 0) {
                CDEBUG(D_INFO|D_WARNING, "SEQ-MGR(cli): allocated "
                       "sequence ["LPX64"]\n", *seqnr);
        }
        RETURN(rc);
}

int
seq_client_alloc_seq(struct lu_client_seq *seq, __u64 *seqnr)
{
        int rc = 0;
        ENTRY;

        down(&seq->seq_sem);
        rc = __seq_client_alloc_seq(seq, seqnr);
        up(&seq->seq_sem);

        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_seq);

int
seq_client_alloc_fid(struct lu_client_seq *seq, struct lu_fid *fid)
{
        __u64 seqnr = 0;
        int rc;
        ENTRY;

        LASSERT(fid != NULL);

        down(&seq->seq_sem);

        if (!fid_is_sane(&seq->seq_fid) ||
            fid_oid(&seq->seq_fid) >= LUSTRE_SEQ_WIDTH)
        {
                /* allocate new sequence for case client hass no sequence at all
                 * or sequnece is exhausted and should be switched. */
                rc = __seq_client_alloc_seq(seq, &seqnr);
                if (rc) {
                        CERROR("can't allocate new sequence, "
                               "rc %d\n", rc);
                        GOTO(out, rc);
                }

                /* init new fid */
                seq->seq_fid.f_oid = LUSTRE_FID_INIT_OID;
                seq->seq_fid.f_seq = seqnr;
                seq->seq_fid.f_ver = 0;

                /* inform caller that sequnece switch is performed to allow it
                 * to setup FLD for it. */
                rc = -ERESTART;
        } else {
                seq->seq_fid.f_oid += 1;
                rc = 0;
        }

        *fid = seq->seq_fid;
        LASSERT(fid_is_sane(fid));
        
        CDEBUG(D_INFO, "SEQ-MGR(cli): allocated FID "DFID3"\n",
               PFID3(fid));

        EXIT;
out:
        up(&seq->seq_sem);
        return rc;
}
EXPORT_SYMBOL(seq_client_alloc_fid);

int 
seq_client_init(struct lu_client_seq *seq, 
                struct obd_export *exp,
                int flags)
{
        ENTRY;

        LASSERT(exp != NULL);
        
        seq->seq_flags = flags;
        fid_zero(&seq->seq_fid);
        range_zero(&seq->seq_range);
        sema_init(&seq->seq_sem, 1);
        seq->seq_exp = class_export_get(exp);

        CDEBUG(D_INFO|D_WARNING, "Client Sequence "
               "Manager initialized\n");
        RETURN(0);
}
EXPORT_SYMBOL(seq_client_init);

void seq_client_fini(struct lu_client_seq *seq)
{
        ENTRY;
        if (seq->seq_exp != NULL) {
                class_export_put(seq->seq_exp);
                seq->seq_exp = NULL;
        }
        CDEBUG(D_INFO|D_WARNING, "Client Sequence Manager finalized\n");
        EXIT;
}
EXPORT_SYMBOL(seq_client_fini);
