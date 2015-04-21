/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2014, Intel Corporation.
 */
/*
 * lustre/lod/lod_sub_object.c
 *
 * LOD sub object methods
 *
 * This file implements sub-object methods for LOD.
 *
 * LOD is Logic volume layer in the MDS stack, which will handle striping
 * and distribute the update to different OSP/OSD. After directing the updates
 * to one specific OSD/OSP, it also needs to do some thing before calling
 * OSD/OSP API, for example recording updates for cross-MDT operation, get
 * the next level transaction etc.
 *
 * Author: Di Wang <di.wang@intel.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <lustre_fid.h>
#include <lustre_param.h>
#include <md_object.h>
#include <lustre_linkea.h>

#include "lod_internal.h"

struct thandle *lod_sub_get_thandle(const struct lu_env *env,
				    struct thandle *th,
				    const struct dt_object *sub_obj)
{
	struct lod_device	*lod = dt2lod_dev(th->th_dev);
	struct top_thandle	*tth;
	struct thandle		*sub_th;
	int			type = LU_SEQ_RANGE_ANY;
	__u32			mdt_index;
	int			rc;
	ENTRY;

	if (th->th_top == NULL)
		RETURN(th);

	tth = container_of(th, struct top_thandle, tt_super);
	LASSERT(tth->tt_magic == TOP_THANDLE_MAGIC);
	/* local object must be mdt object, Note: during ost object
	 * creation, FID is not assigned until osp_object_create(),
	 * so if the FID of sub_obj is zero, it means OST object. */
	if (!dt_object_remote(sub_obj) ||
	    fid_is_zero(lu_object_fid(&sub_obj->do_lu)))
		RETURN(tth->tt_master_sub_thandle);

	rc = lod_fld_lookup(env, lod, lu_object_fid(&sub_obj->do_lu),
			    &mdt_index, &type);
	if (rc < 0)
		RETURN(ERR_PTR(rc));

	if (type == LU_SEQ_RANGE_OST)
		RETURN(tth->tt_master_sub_thandle);

	sub_th = thandle_get_sub(env, th, sub_obj);

	RETURN(sub_th);
}

/**
 * Declare sub-object creation.
 *
 * Get transaction of next layer and declare the creation of the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	the object being created
 * \param[in] attr	the attributes of the object being created
 * \param[in] hint	the hint of the creation
 * \param[in] dof	the object format of the creation
 * \param[th] th	the transaction handle
 *
 * \retval		0 if the declaration succeeds
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_create(const struct lu_env *env,
				  struct dt_object *dt,
				  struct lu_attr *attr,
				  struct dt_allocation_hint *hint,
				  struct dt_object_format *dof,
				  struct thandle *th)
{
	struct thandle *sub_th;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		return PTR_ERR(sub_th);

	return dt_declare_create(env, dt, attr, hint, dof, sub_th);
}

/**
 * Create sub-object.
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation, and create the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	the object being created
 * \param[in] attr	the attributes of the object being created
 * \param[in] hint	the hint of the creation
 * \param[in] dof	the object format of the creation
 * \param[th] th	the transaction handle
 *
 * \retval		0 if the creation succeeds
 * \retval		negative errno if the creation fails.
 */
int lod_sub_object_create(const struct lu_env *env, struct dt_object *dt,
			  struct lu_attr *attr,
			  struct dt_allocation_hint *hint,
			  struct dt_object_format *dof,
			  struct thandle *th)
{
	struct thandle	   *sub_th;
	int		    rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_create(env, dt, attr, hint, dof, sub_th);

	RETURN(rc);
}

/**
 * Declare adding reference for the sub-object
 *
 * Get transaction of next layer and declare the reference adding.
 *
 * \param[in] env	execution environment
 * \param[in] dt	dt object to add reference
 * \param[in] th	transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_ref_add(const struct lu_env *env,
				   struct dt_object *dt,
				   struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_ref_add(env, dt, sub_th);

	RETURN(rc);
}

/**
 * Add reference for the sub-object
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation and add reference of the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	dt object to add reference
 * \param[in] th	transaction handle
 *
 * \retval		0 if it succeeds.
 * \retval		negative errno if it fails.
 */
int lod_sub_object_ref_add(const struct lu_env *env, struct dt_object *dt,
			   struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_ref_add(env, dt, sub_th);

	RETURN(rc);
}

/**
 * Declare deleting reference for the sub-object
 *
 * Get transaction of next layer and declare the reference deleting.
 *
 * \param[in] env	execution environment
 * \param[in] dt	dt object to delete reference
 * \param[in] th	transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_ref_del(const struct lu_env *env,
				   struct dt_object *dt,
				   struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_ref_del(env, dt, sub_th);

	RETURN(rc);
}

/**
 * Delete reference for the sub-object
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation and delete reference of the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	dt object to delete reference
 * \param[in] th	transaction handle
 *
 * \retval		0 if it succeeds.
 * \retval		negative errno if it fails.
 */
int lod_sub_object_ref_del(const struct lu_env *env, struct dt_object *dt,
			   struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_ref_del(env, dt, sub_th);

	RETURN(rc);
}

/**
 * Declare destroying sub-object
 *
 * Get transaction of next layer and declare the sub-object destroy.
 *
 * \param[in] env	execution environment
 * \param[in] dt	dt object to be destroyed
 * \param[in] th	transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_destroy(const struct lu_env *env,
				   struct dt_object *dt,
				   struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_destroy(env, dt, sub_th);

	RETURN(rc);
}

/**
 * Destroy sub-object
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation and destroy the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	dt object to be destroyed
 * \param[in] th	transaction handle
 *
 * \retval		0 if the destroy succeeds.
 * \retval		negative errno if the destroy fails.
 */
int lod_sub_object_destroy(const struct lu_env *env, struct dt_object *dt,
			   struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_destroy(env, dt, sub_th);

	RETURN(rc);
}

/**
 * Declare sub-object index insert
 *
 * Get transaction of next layer and declare index insert.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object for which to insert index
 * \param[in] rec	record of the index which will be inserted
 * \param[in] key	key of the index which will be inserted
 * \param[in] th	the transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_insert(const struct lu_env *env,
				  struct dt_object *dt,
				  const struct dt_rec *rec,
				  const struct dt_key *key,
				  struct thandle *th)
{
	struct thandle *sub_th;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		return PTR_ERR(sub_th);

	return dt_declare_insert(env, dt, rec, key, sub_th);
}

/**
 * Insert index of sub object
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation, and insert the index.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object for which to insert index
 * \param[in] rec	record of the index to be inserted
 * \param[in] key	key of the index to be inserted
 * \param[in] th	the transaction handle
 * \param[in] ign	whether ignore quota
 *
 * \retval		0 if the insertion succeeds.
 * \retval		negative errno if the insertion fails.
 */
int lod_sub_object_index_insert(const struct lu_env *env, struct dt_object *dt,
				const struct dt_rec *rec,
				const struct dt_key *key, struct thandle *th,
				int ign)
{
	struct thandle *sub_th;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		return PTR_ERR(sub_th);

	return dt_insert(env, dt, rec, key, sub_th, ign);
}

/**
 * Declare sub-object index delete
 *
 * Get transaction of next layer and declare index deletion.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object for which to delete index
 * \param[in] key	key of the index which will be deleted
 * \param[in] th	the transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_delete(const struct lu_env *env,
				  struct dt_object *dt,
				  const struct dt_key *key,
				  struct thandle *th)
{
	struct thandle *sub_th;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		return PTR_ERR(sub_th);

	return dt_declare_delete(env, dt, key, sub_th);
}

/**
 * Delete index of sub object
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation, and delete the index.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object for which to delete index
 * \param[in] key	key of the index to be deleted
 * \param[in] th	the transaction handle
 *
 * \retval		0 if the deletion succeeds.
 * \retval		negative errno if the deletion fails.
 */
int lod_sub_object_delete(const struct lu_env *env, struct dt_object *dt,
			  const struct dt_key *name, struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_delete(env, dt, name, sub_th);
	RETURN(rc);
}

/**
 * Declare xattr_set
 *
 * Get transaction of next layer, and declare xattr set.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object on which to set xattr
 * \param[in] buf	xattr to be set
 * \param[in] name	name of the xattr
 * \param[in] fl	flag for setting xattr
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_xattr_set(const struct lu_env *env,
				     struct dt_object *dt,
				     const struct lu_buf *buf,
				     const char *name, int fl,
				     struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_xattr_set(env, dt, buf, name, fl, sub_th);

	RETURN(rc);
}

/**
 * Set xattr
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation, and set xattr to the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object on which to set xattr
 * \param[in] buf	xattr to be set
 * \param[in] name	name of the xattr
 * \param[in] fl	flag for setting xattr
 * \param[in] th	transaction handle
 *
 * \retval		0 if the xattr setting succeeds.
 * \retval		negative errno if xattr setting fails.
 */
int lod_sub_object_xattr_set(const struct lu_env *env, struct dt_object *dt,
			     const struct lu_buf *buf, const char *name, int fl,
			     struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_xattr_set(env, dt, buf, name, fl, sub_th);

	RETURN(rc);
}

/**
 * Declare attr_set
 *
 * Get transaction of next layer, and declare attr set.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object on which to set attr
 * \param[in] attr	attributes to be set
 * \param[in] th	transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_attr_set(const struct lu_env *env,
				    struct dt_object *dt,
				    const struct lu_attr *attr,
				    struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_attr_set(env, dt, attr, sub_th);

	RETURN(rc);
}

/**
 * attributes set
 *
 * Get transaction of next layer, record updates if it belongs to cross-MDT
 * operation, and set attributes to the object.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object on which to set attr
 * \param[in] attr	attrbutes to be set
 * \param[in] th	transaction handle
 *
 * \retval		0 if attributes setting succeeds.
 * \retval		negative errno if the attributes setting fails.
 */
int lod_sub_object_attr_set(const struct lu_env *env,
			    struct dt_object *dt,
			    const struct lu_attr *attr,
			    struct thandle *th)
{
	struct thandle	   *sub_th;
	int		    rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_attr_set(env, dt, attr, sub_th);

	RETURN(rc);
}

/**
 * Declare xattr_del
 *
 * Get transaction of next layer, and declare xattr deletion.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object on which to delete xattr
 * \param[in] name	name of the xattr to be deleted
 * \param[in] th	transaction handle
 *
 * \retval		0 if the declaration succeeds.
 * \retval		negative errno if the declaration fails.
 */
int lod_sub_object_declare_xattr_del(const struct lu_env *env,
				     struct dt_object *dt,
				     const char *name,
				     struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_xattr_del(env, dt, name, sub_th);

	RETURN(rc);
}

/**
 * xattribute deletion
 *
 * Get transaction of next layer, record update if it belongs to cross-MDT
 * operation and delete xattr.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object on which to delete xattr
 * \param[in] name	name of the xattr to be deleted
 * \param[in] th	transaction handle
 *
 * \retval		0 if the deletion succeeds.
 * \retval		negative errno if the deletion fails.
 */
int lod_sub_object_xattr_del(const struct lu_env *env,
			     struct dt_object *dt,
			     const char *name,
			     struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_xattr_del(env, dt, name, sub_th);

	RETURN(rc);
}

/**
 * Declare buffer write
 *
 * Get transaction of next layer and declare buffer write.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object to be written
 * \param[in] buf	buffer to write which includes an embedded size field
 * \param[in] pos	offet in the object to start writing at
 * \param[in] th	transaction handle
 *
 * \retval		0 if the insertion succeeds.
 * \retval		negative errno if the insertion fails.
 */
int lod_sub_object_declare_write(const struct lu_env *env,
				 struct dt_object *dt,
				 const struct lu_buf *buf, loff_t pos,
				 struct thandle *th)
{
	struct thandle	*sub_th;
	int		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_declare_write(env, dt, buf, pos, sub_th);

	RETURN(rc);
}

/**
 * Write buffer to sub object
 *
 * Get transaction of next layer, records buffer write if it belongs to
 * Cross-MDT operation, and write buffer.
 *
 * \param[in] env	execution environment
 * \param[in] dt	object to be written
 * \param[in] buf	buffer to write which includes an embedded size field
 * \param[in] pos	offet in the object to start writing at
 * \param[in] th	transaction handle
 * \param[in] rq	enforcement for this write
 *
 * \retval		the buffer size in bytes if it succeeds.
 * \retval		negative errno if it fails.
 */
ssize_t lod_sub_object_write(const struct lu_env *env, struct dt_object *dt,
			     const struct lu_buf *buf, loff_t *pos,
			     struct thandle *th, int rq)
{
	struct thandle	*sub_th;
	ssize_t		rc;
	ENTRY;

	sub_th = lod_sub_get_thandle(env, th, dt);
	if (IS_ERR(sub_th))
		RETURN(PTR_ERR(sub_th));

	rc = dt_write(env, dt, buf, pos, sub_th, rq);
	RETURN(rc);
}