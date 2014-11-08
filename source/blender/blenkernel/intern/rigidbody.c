/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2013 Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Joshua Leung, Sergej Reich, Martin Felke
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file rigidbody.c
 *  \ingroup blenkernel
 *  \brief Blender-side interface and methods for dealing with Rigid Body simulations
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <float.h>
#include <math.h>
#include <limits.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_kdtree.h"

#ifdef WITH_BULLET
#  include "RBI_api.h"
#endif

#include "DNA_group_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_object_force.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_effect.h"
#include "BKE_global.h"
#include "BKE_group.h"
#include "BKE_library.h"
#include "BKE_mesh.h"
#include "BKE_object.h"
#include "BKE_pointcache.h"
#include "BKE_rigidbody.h"
#include "BKE_modifier.h"
#include "BKE_depsgraph.h"
#include "BKE_scene.h"

#include "RNA_access.h"

#ifdef WITH_BULLET

static void validateShard(RigidBodyWorld *rbw, MeshIsland *mi, Object *ob, int rebuild);

static bool isModifierActive(FractureModifierData *rmd) {
	return ((rmd != NULL) && (rmd->modifier.mode & (eModifierMode_Realtime | eModifierMode_Render)) && (rmd->refresh == false));
}

static void calc_dist_angle(RigidBodyShardCon *con, float *dist, float *angle)
{
	float q1[4], q2[4], qdiff[4], axis[3];
	if ((con->mi1->rigidbody == NULL) || (con->mi2->rigidbody == NULL)) {
		*dist = 0;
		*angle = 0;
		return;
	}
	
	sub_v3_v3v3(axis, con->mi1->rigidbody->pos, con->mi2->rigidbody->pos);
	*dist = len_v3(axis);
	copy_qt_qt(q1, con->mi1->rigidbody->orn);
	copy_qt_qt(q2, con->mi2->rigidbody->orn);
	invert_qt(q1);
	mul_qt_qtqt(qdiff, q1, q2);
	quat_to_axis_angle(axis, angle, qdiff);
}

void BKE_rigidbody_start_dist_angle(RigidBodyShardCon *con)
{
	/* store starting angle and distance per constraint*/
	float dist, angle;
	calc_dist_angle(con, &dist, &angle);
	con->start_dist = dist;
	con->start_angle = angle;
}

float BKE_rigidbody_calc_max_con_mass(Object *ob)
{
	FractureModifierData *rmd;
	ModifierData *md;
	RigidBodyShardCon *con;
	float max_con_mass = 0, con_mass;

	for (md = ob->modifiers.first; md; md = md->next) {
		if (md->type == eModifierType_Fracture) {
			rmd = (FractureModifierData *)md;
			for (con = rmd->meshConstraints.first; con; con = con->next) {
				if ((con->mi1 != NULL && con->mi1->rigidbody != NULL) &&
				    (con->mi2 != NULL && con->mi2->rigidbody != NULL)) {
					con_mass = con->mi1->rigidbody->mass + con->mi2->rigidbody->mass;
					if (con_mass > max_con_mass) {
						max_con_mass = con_mass;
					}
				}
			}

			return max_con_mass;
		}
	}

	return 0;
}

float BKE_rigidbody_calc_min_con_dist(Object *ob)
{
	FractureModifierData *rmd;
	ModifierData *md;
	RigidBodyShardCon *con;
	float min_con_dist = FLT_MAX, con_dist, con_vec[3];

	for (md = ob->modifiers.first; md; md = md->next) {
		if (md->type == eModifierType_Fracture) {
			rmd = (FractureModifierData *)md;
			for (con = rmd->meshConstraints.first; con; con = con->next) {
				if ((con->mi1 != NULL && con->mi1->rigidbody != NULL) &&
				    (con->mi2 != NULL && con->mi2->rigidbody != NULL)) {
					sub_v3_v3v3(con_vec, con->mi1->centroid, con->mi2->centroid);
					con_dist = len_v3(con_vec);
					if (con_dist < min_con_dist) {
						min_con_dist = con_dist;
					}
				}
			}

			return min_con_dist;
		}
	}

	return FLT_MAX;
}


void BKE_rigidbody_calc_threshold(float max_con_mass, FractureModifierData *rmd, RigidBodyShardCon *con) {

	float max_thresh, thresh = 0.0f, con_mass;
	if ((max_con_mass == 0) && (rmd->use_mass_dependent_thresholds)) {
		return;
	}

	if ((con->mi1 == NULL) || (con->mi2 == NULL)) {
		return;
	}

	max_thresh = rmd->breaking_threshold;
	if ((con->mi1->rigidbody != NULL) && (con->mi2->rigidbody != NULL)) {
		con_mass = con->mi1->rigidbody->mass + con->mi2->rigidbody->mass;

		if (rmd->use_mass_dependent_thresholds)
		{
			thresh = (con_mass / max_con_mass) * max_thresh;
		}

		con->breaking_threshold = thresh;
	}
}

static int DM_mesh_minmax(DerivedMesh *dm, float r_min[3], float r_max[3])
{
	MVert *v;
	int i = 0;
	for (i = 0; i < dm->numVertData; i++) {
		v = CDDM_get_vert(dm, i);
		minmax_v3v3_v3(r_min, r_max, v->co);
	}

	return (dm->numVertData != 0);
}

static void DM_mesh_boundbox(DerivedMesh *bm, float r_loc[3], float r_size[3])
{
	float min[3], max[3];
	float mloc[3], msize[3];

	if (!r_loc) r_loc = mloc;
	if (!r_size) r_size = msize;

	INIT_MINMAX(min, max);
	if (!DM_mesh_minmax(bm, min, max)) {
		min[0] = min[1] = min[2] = -1.0f;
		max[0] = max[1] = max[2] = 1.0f;
	}

	mid_v3_v3v3(r_loc, min, max);

	r_size[0] = (max[0] - min[0]) / 2.0f;
	r_size[1] = (max[1] - min[1]) / 2.0f;
	r_size[2] = (max[2] - min[2]) / 2.0f;
}

/* helper function to calculate volume of rigidbody object */
float BKE_rigidbody_calc_volume(DerivedMesh *dm, RigidBodyOb *rbo)
{
	float loc[3]  = {0.0f, 0.0f, 0.0f};
	float size[3]  = {1.0f, 1.0f, 1.0f};
	float radius = 1.0f;
	float height = 1.0f;

	float volume = 0.0f;

	/* if automatically determining dimensions, use the Object's boundbox
	 *	- assume that all quadrics are standing upright on local z-axis
	 *	- assume even distribution of mass around the Object's pivot
	 *	  (i.e. Object pivot is centralised in boundbox)
	 *	- boundbox gives full width
	 */
	/* XXX: all dimensions are auto-determined now... later can add stored settings for this*/
	DM_mesh_boundbox(dm, loc, size);

	if (ELEM(rbo->shape, RB_SHAPE_CAPSULE, RB_SHAPE_CYLINDER, RB_SHAPE_CONE)) {
		/* take radius as largest x/y dimension, and height as z-dimension */
		radius = MAX2(size[0], size[1]) * 0.5f;
		height = size[2];
	}
	else if (rbo->shape == RB_SHAPE_SPHERE) {
		/* take radius to the the largest dimension to try and encompass everything */
		radius = max_fff(size[0], size[1], size[2]) * 0.5f;
	}

	/* calculate volume as appropriate  */
	switch (rbo->shape) {
	
		case RB_SHAPE_SPHERE:
			volume = 4.0f / 3.0f * (float)M_PI * radius * radius * radius;
			break;

		/* for now, assume that capsule is close enough to a cylinder... */
		case RB_SHAPE_CAPSULE:
		case RB_SHAPE_CYLINDER:
			volume = (float)M_PI * radius * radius * height;
			break;

		case RB_SHAPE_CONE:
			volume = (float)M_PI / 3.0f * radius * radius * height;
			break;

		/* for now, all mesh shapes are just treated as boxes...
		 * NOTE: this may overestimate the volume, but other methods are overkill
		 */
		case RB_SHAPE_BOX:
		case RB_SHAPE_CONVEXH:
		case RB_SHAPE_TRIMESH:
			volume = size[0] * size[1] * size[2];
			if (size[0] == 0) {
				volume = size[1] * size[2];
			}
			else if (size[1] == 0) {
				volume = size[0] * size[2];
			}
			else if (size[2] == 0) {
				volume = size[0] * size[1];
			}
			break;

#if 0 // XXX: not defined yet
		case RB_SHAPE_COMPOUND:
			volume = 0.0f;
			break;
#endif
	}

	/* return the volume calculated */
	return volume;
}

void BKE_rigidbody_calc_shard_mass(Object *ob, MeshIsland *mi, DerivedMesh *orig_dm)
{
	DerivedMesh *dm_ob = orig_dm, *dm_mi;
	float vol_mi = 0, mass_mi = 0, vol_ob = 0, mass_ob = 0;

	if (dm_ob == NULL) {
		/* fallback method */
		if (ob->type == OB_MESH) {
			/* if we have a mesh, determine its volume */
			dm_ob = CDDM_from_mesh(ob->data);
			vol_ob = BKE_rigidbody_calc_volume(dm_ob, ob->rigidbody_object);
		}
		else {
			/* else get object boundbox as last resort */
			float dim[3];
			BKE_object_dimensions_get(ob, dim);
			vol_ob = dim[0] * dim[1] * dim[2];
		}
	}
	else
	{
		vol_ob = BKE_rigidbody_calc_volume(dm_ob, ob->rigidbody_object);
	}

	mass_ob = ob->rigidbody_object->mass;

	if (vol_ob > 0) {
		dm_mi = mi->physics_mesh;
		vol_mi = BKE_rigidbody_calc_volume(dm_mi, mi->rigidbody);
		mass_mi = (vol_mi / vol_ob) * mass_ob;
		mi->rigidbody->mass = mass_mi;
	}
	
	if (mi->rigidbody->type == RBO_TYPE_ACTIVE) {
		if (mi->rigidbody->mass == 0)
			mi->rigidbody->mass = 0.001;  /* set a minimum mass for active objects */
	}

	/* only active bodies need mass update */
	if ((mi->rigidbody->physics_object) && (mi->rigidbody->type == RBO_TYPE_ACTIVE)) {
		RB_body_set_mass(mi->rigidbody->physics_object, RBO_GET_MASS(mi->rigidbody));
	}

	if (orig_dm == NULL && dm_ob != NULL)
	{
		/* free temp dm, if it hasnt been passed in */
		dm_ob->needsFree = 1;
		dm_ob->release(dm_ob);
	}
}

static void initNormals(struct MeshIsland *mi, Object *ob, FractureModifierData *fmd)
{
	/* hrm have to init Normals HERE, because we cant do this in readfile.c in case the file is loaded (have no access to the Object there) */
	if (mi->vertno == NULL && mi->vertices_cached != NULL) {
		KDTreeNearest n;
		int index = 0, i = 0;
		MVert mvrt;

		DerivedMesh *dm = ob->derivedFinal;
		if (dm == NULL) {
			dm = CDDM_from_mesh(ob->data);
		}

		if (fmd->nor_tree == NULL) {
			/* HRRRRRMMMM need to build the kdtree here as well if we start the sim after loading and not refreshing, again, no access to object.... */
			int i = 0, totvert;
			KDTree *tree;
			MVert *mv, *mvert;

			mvert = dm->getVertArray(dm);
			totvert = dm->getNumVerts(dm);
			tree = BLI_kdtree_new(totvert);

			for (i = 0, mv = mvert; i < totvert; i++, mv++) {
				BLI_kdtree_insert(tree, i, mv->co);
			}

			BLI_kdtree_balance(tree);
			fmd->nor_tree = tree;
		}

		mi->vertno = MEM_callocN(sizeof(short) * 3 * mi->vertex_count, "mi->vertno");
		for (i = 0; i < mi->vertex_count; i++) {
			MVert *v = mi->vertices_cached[i];
			index = BLI_kdtree_find_nearest(fmd->nor_tree, v->co, &n);
			dm->getVert(dm, index, &mvrt);
			mi->vertno[i * 3] = mvrt.no[0];
			mi->vertno[i * 3 + 1] = mvrt.no[1];
			mi->vertno[i * 3 + 2] = mvrt.no[2];
		}

		if (ob->derivedFinal == NULL) {
			dm->needsFree = 1;
			dm->release(dm);
			dm = NULL;
		}
	}
}

void BKE_rigidbody_update_cell(struct MeshIsland *mi, Object *ob, float loc[3], float rot[4], FractureModifierData *rmd, int frame)
{
	float startco[3], centr[3], size[3];
	short startno[3];
	int j, i, n;
	bool invalidData;

	/* hrm have to init Normals HERE, because we cant do this in readfile.c in case the file is loaded (have no access to the Object there)*/
	if (mi->vertno == NULL && rmd->fix_normals) {
		initNormals(mi, ob, rmd);
	}
	
	invalidData = (loc[0] == FLT_MIN) || (rot[0] == FLT_MIN);
	
	if (invalidData) {
		return;
	}

	invert_m4_m4(ob->imat, ob->obmat);
	mat4_to_size(size, ob->obmat);

	n = frame - mi->start_frame + 1;

	if (mi->frame_count >= 0 && mi->frame_count < n)
	{
		mi->locs = MEM_reallocN(mi->locs, sizeof(float) * 3 * (mi->frame_count+1));
		mi->rots = MEM_reallocN(mi->rots, sizeof(float) * 4 * (mi->frame_count+1));

		i = mi->frame_count;
		mi->locs[i*3] = loc[0];
		mi->locs[i*3+1] = loc[1];
		mi->locs[i*3+2] = loc[2];

		mi->rots[i*4] = rot[0];
		mi->rots[i*4+1] = rot[1];
		mi->rots[i*4+2] = rot[2];
		mi->rots[i*4+3] = rot[3];

		mi->frame_count = n;
	}
	
	for (j = 0; j < mi->vertex_count; j++) {
		struct MVert *vert;
		float fno[3];
		
		if (!mi->vertices_cached) {
			return;
		}
		
		vert = mi->vertices_cached[j];
		if (vert == NULL) continue;
		if (vert->co == NULL) break;
		if (rmd->refresh == true) break;

		startco[0] = mi->vertco[j * 3];
		startco[1] = mi->vertco[j * 3 + 1];
		startco[2] = mi->vertco[j * 3 + 2];

		if (rmd->fix_normals) {
			startno[0] = mi->vertno[j * 3];
			startno[1] = mi->vertno[j * 3 + 1];
			startno[2] = mi->vertno[j * 3 + 2];

			normal_short_to_float_v3(fno, startno);
			mul_qt_v3(rot, fno);
			normal_float_to_short_v3(vert->no, fno);
		}

		copy_v3_v3(vert->co, startco);
		mul_v3_v3(vert->co, size);
		mul_qt_v3(rot, vert->co);
		copy_v3_v3(centr, mi->centroid);
		mul_v3_v3(centr, size);
		mul_qt_v3(rot, centr);
		sub_v3_v3(vert->co, centr);
		add_v3_v3(vert->co, loc);
		mul_m4_v3(ob->imat, vert->co);

	}

	ob->recalc |= OB_RECALC_ALL;
}

/* ************************************** */
/* Memory Management */

/* Freeing Methods --------------------- */

/* Free rigidbody world */
void BKE_rigidbody_free_world(RigidBodyWorld *rbw)
{
	/* sanity check */
	if (!rbw)
		return;

	if (rbw->physics_world) {
		/* free physics references, we assume that all physics objects in will have been added to the world */
		GroupObject *go;
		if (rbw->constraints) {
			for (go = rbw->constraints->gobject.first; go; go = go->next) {
				if (go->ob && go->ob->rigidbody_constraint) {
					RigidBodyCon *rbc = go->ob->rigidbody_constraint;

					if (rbc->physics_constraint)
						RB_dworld_remove_constraint(rbw->physics_world, rbc->physics_constraint);
				}
			}
		}
		if (rbw->group) {
			for (go = rbw->group->gobject.first; go; go = go->next) {
				if (go->ob && go->ob->rigidbody_object) {
					RigidBodyOb *rbo = go->ob->rigidbody_object;

					if (rbo->physics_object)
						RB_dworld_remove_body(rbw->physics_world, rbo->physics_object);
				}
			}
		}
		/* free dynamics world */
		if (rbw->physics_world != NULL)
			RB_dworld_delete(rbw->physics_world);
	}
	if (rbw->objects)
		MEM_freeN(rbw->objects);

	if (rbw->cache_index_map) {
		MEM_freeN(rbw->cache_index_map);
		rbw->cache_index_map = NULL;
	}

	if (rbw->cache_offset_map) {
		MEM_freeN(rbw->cache_offset_map);
		rbw->cache_offset_map = NULL;
	}


	/* free cache */
	BKE_ptcache_free_list(&(rbw->ptcaches));
	rbw->pointcache = NULL;

	/* free effector weights */
	if (rbw->effector_weights)
		MEM_freeN(rbw->effector_weights);

	/* free rigidbody world itself */
	MEM_freeN(rbw);
}

/* Free RigidBody settings and sim instances */
void BKE_rigidbody_free_object(Object *ob)
{
	RigidBodyOb *rbo = (ob) ? ob->rigidbody_object : NULL;

	/* sanity check */
	if (rbo == NULL)
		return;

	/* free physics references */
	if (rbo->physics_object) {
		RB_body_delete(rbo->physics_object);
		rbo->physics_object = NULL;
	}

	if (rbo->physics_shape) {
		RB_shape_delete(rbo->physics_shape);
		rbo->physics_shape = NULL;
	}

	/* free data itself */
	MEM_freeN(rbo);
	ob->rigidbody_object = NULL;
}

/* Free RigidBody constraint and sim instance */
void BKE_rigidbody_free_constraint(Object *ob)
{
	RigidBodyCon *rbc = (ob) ? ob->rigidbody_constraint : NULL;

	/* sanity check */
	if (rbc == NULL)
		return;

	/* free physics reference */
	if (rbc->physics_constraint) {
		RB_constraint_delete(rbc->physics_constraint);
		rbc->physics_constraint = NULL;
	}

	/* free data itself */
	MEM_freeN(rbc);
	ob->rigidbody_constraint = NULL;
}

/* Copying Methods --------------------- */

/* These just copy the data, clearing out references to physics objects.
 * Anything that uses them MUST verify that the copied object will
 * be added to relevant groups later...
 */

RigidBodyOb *BKE_rigidbody_copy_object(Object *ob)
{
	RigidBodyOb *rboN = NULL;

	if (ob->rigidbody_object) {
		/* just duplicate the whole struct first (to catch all the settings) */
		rboN = MEM_dupallocN(ob->rigidbody_object);

		/* tag object as needing to be verified */
		rboN->flag |= RBO_FLAG_NEEDS_VALIDATE;

		/* clear out all the fields which need to be revalidated later */
		rboN->physics_object = NULL;
		rboN->physics_shape = NULL;
	}

	/* return new copy of settings */
	return rboN;
}

RigidBodyCon *BKE_rigidbody_copy_constraint(Object *ob)
{
	RigidBodyCon *rbcN = NULL;

	if (ob->rigidbody_constraint) {
		/* just duplicate the whole struct first (to catch all the settings) */
		rbcN = MEM_dupallocN(ob->rigidbody_constraint);

		/* tag object as needing to be verified */
		rbcN->flag |= RBC_FLAG_NEEDS_VALIDATE;

		/* clear out all the fields which need to be revalidated later */
		rbcN->physics_constraint = NULL;
	}

	/* return new copy of settings */
	return rbcN;
}

/* preserve relationships between constraints and rigid bodies after duplication */
void BKE_rigidbody_relink_constraint(RigidBodyCon *rbc)
{
	ID_NEW(rbc->ob1);
	ID_NEW(rbc->ob2);
}

/* ************************************** */
/* Setup Utilities - Validate Sim Instances */

/* get the appropriate DerivedMesh based on rigid body mesh source */
static DerivedMesh *rigidbody_get_mesh(Object *ob)
{
	if (ob->rigidbody_object->mesh_source == RBO_MESH_DEFORM) {
		return ob->derivedDeform;
	}
	else if (ob->rigidbody_object->mesh_source == RBO_MESH_FINAL) {
		return ob->derivedFinal;
	}
	else {
		return CDDM_from_mesh(ob->data);
	}
}

/* create collision shape of mesh - convex hull */
static rbCollisionShape *rigidbody_get_shape_convexhull_from_mesh(Mesh *me, float margin, bool *can_embed)
{
	rbCollisionShape *shape = NULL;
	int totvert = me->totvert;
	MVert *mvert = me->mvert;

	if (me && totvert) {
		shape = RB_shape_new_convex_hull((float *)mvert, sizeof(MVert), totvert, margin, can_embed);
	}
	else {
		printf("ERROR: no vertices to define Convex Hull collision shape with\n");
	}

	return shape;
}

static rbCollisionShape *rigidbody_get_shape_convexhull_from_dm(DerivedMesh *dm, float margin, bool *can_embed)
{
	rbCollisionShape *shape = NULL;
	int totvert = dm->getNumVerts(dm);
	MVert *mvert = dm->getVertArray(dm);

	if (dm && totvert) {
		shape = RB_shape_new_convex_hull((float *)mvert, sizeof(MVert), totvert, margin, can_embed);
	}
	else {
		printf("ERROR: no vertices to define Convex Hull collision shape with\n");
	}

	return shape;
}



/* create collision shape of mesh - triangulated mesh
 * returns NULL if creation fails.
 */
static rbCollisionShape *rigidbody_get_shape_trimesh_from_mesh_shard(DerivedMesh *dmm, Object *ob)
{
	rbCollisionShape *shape = NULL;

	if (dmm) {
		DerivedMesh *dm = NULL;
		MVert *mvert;
		MFace *mface;
		int totvert;
		int totface;
		int tottris = 0;
		int triangle_index = 0;

		dm = CDDM_copy(dmm);

		/* ensure mesh validity, then grab data */
		if (dm == NULL)
			return NULL;

		DM_ensure_tessface(dm);

		mvert   = (dm) ? dm->getVertArray(dm) : NULL;
		totvert = (dm) ? dm->getNumVerts(dm) : 0;
		mface   = (dm) ? dm->getTessFaceArray(dm) : NULL;
		totface = (dm) ? dm->getNumTessFaces(dm) : 0;

		/* sanity checking - potential case when no data will be present */
		if ((totvert == 0) || (totface == 0)) {
			printf("WARNING: no geometry data converted for Mesh Collision Shape (ob = %s)\n", ob->id.name + 2);
		}
		else {
			rbMeshData *mdata;
			int i;

			/* count triangles */
			for (i = 0; i < totface; i++) {
				(mface[i].v4) ? (tottris += 2) : (tottris += 1);
			}

			/* init mesh data for collision shape */
			mdata = RB_trimesh_data_new(tottris, totvert);

			RB_trimesh_add_vertices(mdata, (float *)mvert, totvert, sizeof(MVert));

			/* loop over all faces, adding them as triangles to the collision shape
			 * (so for some faces, more than triangle will get added)
			 */
			for (i = 0; (i < totface) && (mface) && (mvert); i++, mface++) {
				/* add first triangle - verts 1,2,3 */
				RB_trimesh_add_triangle_indices(mdata, triangle_index, mface->v1, mface->v2, mface->v3);
				triangle_index++;

				/* add second triangle if needed - verts 1,3,4 */
				if (mface->v4) {
					RB_trimesh_add_triangle_indices(mdata, triangle_index, mface->v1, mface->v3, mface->v4);
					triangle_index++;
				}
			}
			RB_trimesh_finish(mdata);

			/* construct collision shape
			 *
			 * These have been chosen to get better speed/accuracy tradeoffs with regards
			 * to limitations of each:
			 *    - BVH-Triangle Mesh: for passive objects only. Despite having greater
			 *                         speed/accuracy, they cannot be used for moving objects.
			 *    - GImpact Mesh:      for active objects. These are slower and less stable,
			 *                         but are more flexible for general usage.
			 */
			if (ob->rigidbody_object->type == RBO_TYPE_PASSIVE) {
				shape = RB_shape_new_trimesh(mdata);
			}
			else {
				shape = RB_shape_new_gimpact_mesh(mdata);
			}
		}

		/* cleanup temp data */
		if (dm /*&& ob->rigidbody_object->mesh_source == RBO_MESH_BASE*/) {
			dm->needsFree = 1;
		dm->release(dm);
			dm = NULL;
		}
	}
	else {
		printf("ERROR: cannot make Triangular Mesh collision shape for non-Mesh object\n");
	}

	return shape;
}

/* create collision shape of mesh - triangulated mesh
 * returns NULL if creation fails.
 */
static rbCollisionShape *rigidbody_get_shape_trimesh_from_mesh(Object *ob)
{
	rbCollisionShape *shape = NULL;

	if (ob->type == OB_MESH) {
		DerivedMesh *dm = NULL;
		MVert *mvert;
		MFace *mface;
		int totvert;
		int totface;
		int tottris = 0;
		int triangle_index = 0;

		dm = rigidbody_get_mesh(ob);

		/* ensure mesh validity, then grab data */
		if (dm == NULL)
			return NULL;

		DM_ensure_tessface(dm);

		mvert   = dm->getVertArray(dm);
		totvert = dm->getNumVerts(dm);
		mface   = dm->getTessFaceArray(dm);
		totface = dm->getNumTessFaces(dm);

		/* sanity checking - potential case when no data will be present */
		if ((totvert == 0) || (totface == 0)) {
			printf("WARNING: no geometry data converted for Mesh Collision Shape (ob = %s)\n", ob->id.name + 2);
		}
		else {
			rbMeshData *mdata;
			int i;
			
			/* count triangles */
			for (i = 0; i < totface; i++) {
				(mface[i].v4) ? (tottris += 2) : (tottris += 1);
			}

			/* init mesh data for collision shape */
			mdata = RB_trimesh_data_new(tottris, totvert);
			
			RB_trimesh_add_vertices(mdata, (float *)mvert, totvert, sizeof(MVert));

			/* loop over all faces, adding them as triangles to the collision shape
			 * (so for some faces, more than triangle will get added)
			 */
			for (i = 0; (i < totface) && (mface) && (mvert); i++, mface++) {
				/* add first triangle - verts 1,2,3 */
				RB_trimesh_add_triangle_indices(mdata, triangle_index, mface->v1, mface->v2, mface->v3);
				triangle_index++;

				/* add second triangle if needed - verts 1,3,4 */
				if (mface->v4) {
					RB_trimesh_add_triangle_indices(mdata, triangle_index, mface->v1, mface->v3, mface->v4);
					triangle_index++;
				}
			}
			RB_trimesh_finish(mdata);

			/* construct collision shape
			 *
			 * These have been chosen to get better speed/accuracy tradeoffs with regards
			 * to limitations of each:
			 *    - BVH-Triangle Mesh: for passive objects only. Despite having greater
			 *                         speed/accuracy, they cannot be used for moving objects.
			 *    - GImpact Mesh:      for active objects. These are slower and less stable,
			 *                         but are more flexible for general usage.
			 */
			if (ob->rigidbody_object->type == RBO_TYPE_PASSIVE) {
				shape = RB_shape_new_trimesh(mdata);
			}
			else {
				shape = RB_shape_new_gimpact_mesh(mdata);
			}
		}

		/* cleanup temp data */
		if (ob->rigidbody_object->mesh_source == RBO_MESH_BASE) {
			dm->release(dm);
		}
	}
	else {
		printf("ERROR: cannot make Triangular Mesh collision shape for non-Mesh object\n");
	}

	return shape;
}

/* Create new physics sim collision shape for object and store it,
 * or remove the existing one first and replace...
 */
static void rigidbody_validate_sim_shape(Object *ob, bool rebuild)
{
	RigidBodyOb *rbo = ob->rigidbody_object;
	rbCollisionShape *new_shape = NULL;
	BoundBox *bb = NULL;
	float size[3] = {1.0f, 1.0f, 1.0f};
	float radius = 1.0f;
	float height = 1.0f;
	float capsule_height;
	float hull_margin = 0.0f;
	bool can_embed = true;
	bool has_volume;

	/* sanity check */
	if (rbo == NULL)
		return;

	/* don't create a new shape if we already have one and don't want to rebuild it */
	if (rbo->physics_shape && !rebuild)
		return;

	/* if automatically determining dimensions, use the Object's boundbox
	 *	- assume that all quadrics are standing upright on local z-axis
	 *	- assume even distribution of mass around the Object's pivot
	 *	  (i.e. Object pivot is centralized in boundbox)
	 */
	// XXX: all dimensions are auto-determined now... later can add stored settings for this
	/* get object dimensions without scaling */
	bb = BKE_object_boundbox_get(ob);
	if (bb) {
		size[0] = (bb->vec[4][0] - bb->vec[0][0]);
		size[1] = (bb->vec[2][1] - bb->vec[0][1]);
		size[2] = (bb->vec[1][2] - bb->vec[0][2]);
	}
	mul_v3_fl(size, 0.5f);

	if (ELEM(rbo->shape, RB_SHAPE_CAPSULE, RB_SHAPE_CYLINDER, RB_SHAPE_CONE)) {
		/* take radius as largest x/y dimension, and height as z-dimension */
		radius = MAX2(size[0], size[1]);
		height = size[2];
	}
	else if (rbo->shape == RB_SHAPE_SPHERE) {
		/* take radius to the largest dimension to try and encompass everything */
		radius = MAX3(size[0], size[1], size[2]);
	}

	/* create new shape */
	switch (rbo->shape) {
		case RB_SHAPE_BOX:
			new_shape = RB_shape_new_box(size[0], size[1], size[2]);
			break;

		case RB_SHAPE_SPHERE:
			new_shape = RB_shape_new_sphere(radius);
			break;

		case RB_SHAPE_CAPSULE:
			capsule_height = (height - radius) * 2.0f;
			new_shape = RB_shape_new_capsule(radius, (capsule_height > 0.0f) ? capsule_height : 0.0f);
			break;
		case RB_SHAPE_CYLINDER:
			new_shape = RB_shape_new_cylinder(radius, height);
			break;
		case RB_SHAPE_CONE:
			new_shape = RB_shape_new_cone(radius, height * 2.0f);
			break;

		case RB_SHAPE_CONVEXH:
			/* try to emged collision margin */
			has_volume = (MIN3(size[0], size[1], size[2]) > 0.0f);

			if (!(rbo->flag & RBO_FLAG_USE_MARGIN) && has_volume)
				hull_margin = 0.04f;
			if (ob->type == OB_MESH && ob->data) {
				new_shape = rigidbody_get_shape_convexhull_from_mesh((Mesh *)ob->data, hull_margin, &can_embed);
			}
			else {
				printf("ERROR: cannot make Convex Hull collision shape for non-Mesh object\n");
			}

			if (!(rbo->flag & RBO_FLAG_USE_MARGIN))
				rbo->margin = (can_embed && has_volume) ? 0.04f : 0.0f;      /* RB_TODO ideally we shouldn't directly change the margin here */
			break;
		case RB_SHAPE_TRIMESH:
			new_shape = rigidbody_get_shape_trimesh_from_mesh(ob);
			break;
	}
	/* assign new collision shape if creation was successful */
	if (new_shape) {
		if (rbo->physics_shape)
			RB_shape_delete(rbo->physics_shape);
		rbo->physics_shape = new_shape;
		RB_shape_set_margin(rbo->physics_shape, RBO_GET_MARGIN(rbo));
	}
	/* use box shape if we can't fall back to old shape */
	else if (rbo->physics_shape == NULL) {
		rbo->shape = RB_SHAPE_BOX;
		rigidbody_validate_sim_shape(ob, true);
	}
}

/* --------------------- */

/* Create new physics sim collision shape for object and store it,
 * or remove the existing one first and replace...
 */
void BKE_rigidbody_validate_sim_shard_shape(MeshIsland *mi, Object *ob, short rebuild)
{
	RigidBodyOb *rbo = mi->rigidbody;
	rbCollisionShape *new_shape = NULL;
	float size[3] = {1.0f, 1.0f, 1.0f}, loc[3] = {0.0f, 0.0f, 0.0f};
	float radius = 1.0f;
	float height = 1.0f;
	float capsule_height;
	float hull_margin = 0.0f;
	bool can_embed = true;
	bool has_volume;
	float min[3], max[3];
	
	/* sanity check */
	if (rbo == NULL)
		return;

	/* don't create a new shape if we already have one and don't want to rebuild it */
	if (rbo->physics_shape && !rebuild)
		return;
	
	/* if automatically determining dimensions, use the Object's boundbox
	 *	- assume that all quadrics are standing upright on local z-axis
	 *	- assume even distribution of mass around the Object's pivot
	 *	  (i.e. Object pivot is centralized in boundbox)
	 *	- boundbox gives full width
	 */
	// XXX: all dimensions are auto-determined now... later can add stored settings for this
	/* get object dimensions without scaling */

	INIT_MINMAX(min, max);
	if (!DM_mesh_minmax(mi->physics_mesh, min, max)) {
		min[0] = min[1] = min[2] = -1.0f;
		max[0] = max[1] = max[2] = 1.0f;
	}

	mid_v3_v3v3(loc, min, max);
	size[0] = (max[0] - min[0]) / 2.0f;
	size[1] = (max[1] - min[1]) / 2.0f;
	size[2] = (max[2] - min[2]) / 2.0f;

	if (ELEM(rbo->shape, RB_SHAPE_CAPSULE, RB_SHAPE_CYLINDER, RB_SHAPE_CONE)) {
		/* take radius as largest x/y dimension, and height as z-dimension */
		radius = MAX2(size[0], size[1]);
		height = size[2];
	}
	else if (rbo->shape == RB_SHAPE_SPHERE) {

		/* take radius to the largest dimension to try and encompass everything */
		radius = max_fff(size[0], size[1], size[2]) * 0.5f;
	}
	
	/* create new shape */
	switch (rbo->shape) {
		case RB_SHAPE_BOX:
			new_shape = RB_shape_new_box(size[0], size[1], size[2]);
			break;
	
		case RB_SHAPE_SPHERE:
			new_shape = RB_shape_new_sphere(radius);
			break;
	
		case RB_SHAPE_CAPSULE:
			capsule_height = (height - radius) * 2.0f;
			new_shape = RB_shape_new_capsule(radius, (capsule_height > 0.0f) ? capsule_height : 0.0f);
			break;
		case RB_SHAPE_CYLINDER:
			new_shape = RB_shape_new_cylinder(radius, height);
			break;

		case RB_SHAPE_CONE:
			new_shape = RB_shape_new_cone(radius, height * 2.0f);
			break;
	
		case RB_SHAPE_CONVEXH:
			/* try to emged collision margin */
			has_volume = (MIN3(size[0], size[1], size[2]) > 0.0f);

			if (!(rbo->flag & RBO_FLAG_USE_MARGIN) && has_volume)
				hull_margin = 0.04f;
			new_shape = rigidbody_get_shape_convexhull_from_dm(mi->physics_mesh, hull_margin, &can_embed);
			if (!(rbo->flag & RBO_FLAG_USE_MARGIN))
				rbo->margin = (can_embed && has_volume) ? 0.04f : 0.0f;      /* RB_TODO ideally we shouldn't directly change the margin here */
			break;
		case RB_SHAPE_TRIMESH:
			new_shape = rigidbody_get_shape_trimesh_from_mesh_shard(mi->physics_mesh, ob);
			break;
	}
	/* assign new collision shape if creation was successful */
	if (new_shape) {
		if (rbo->physics_shape)
			RB_shape_delete(rbo->physics_shape);
		rbo->physics_shape = new_shape;
		RB_shape_set_margin(rbo->physics_shape, RBO_GET_MARGIN(rbo));
	}
	else { /* otherwise fall back to box shape */
		rbo->shape = RB_SHAPE_BOX;
		BKE_rigidbody_validate_sim_shard_shape(mi, ob, true);
	}
}

#if 0 // XXX: not defined yet
		case RB_SHAPE_COMPOUND:
			volume = 0.0f;
			break;
#endif

/* --------------------- */

/* Create physics sim representation of shard given RigidBody settings
 * < rebuild: even if an instance already exists, replace it
 */
void BKE_rigidbody_validate_sim_shard(RigidBodyWorld *rbw, MeshIsland *mi, Object *ob, short rebuild)
{
	RigidBodyOb *rbo = (mi) ? mi->rigidbody : NULL;
	float loc[3];
	float rot[4];

	/* sanity checks:
	 *	- object doesn't have RigidBody info already: then why is it here?
	 */
	if (rbo == NULL)
		return;

	/* at validation, reset frame count as well */
	mi->start_frame = rbw->pointcache->startframe;
	mi->frame_count = 0;

	/* make sure collision shape exists */
	/* FIXME we shouldn't always have to rebuild collision shapes when rebuilding objects, but it's needed for constraints to update correctly */
	if (rbo->physics_shape == NULL || rebuild)
		BKE_rigidbody_validate_sim_shard_shape(mi, ob, true);
	
	if (rbo->physics_object) {
		if (rebuild == false || mi->rigidbody->flag & RBO_FLAG_KINEMATIC_REBUILD)
			RB_dworld_remove_body(rbw->physics_world, rbo->physics_object);
	}
	if (!rbo->physics_object || rebuild) {
		/* remove rigid body if it already exists before creating a new one */
		if (rbo->physics_object) {
			RB_body_delete(rbo->physics_object);
		}

		copy_v3_v3(loc, rbo->pos);
		copy_v4_v4(rot, rbo->orn);
		
		rbo->physics_object = RB_body_new(rbo->physics_shape, loc, rot);

		RB_body_set_friction(rbo->physics_object, rbo->friction);
		RB_body_set_restitution(rbo->physics_object, rbo->restitution);

		RB_body_set_damping(rbo->physics_object, rbo->lin_damping, rbo->ang_damping);
		RB_body_set_sleep_thresh(rbo->physics_object, rbo->lin_sleep_thresh, rbo->ang_sleep_thresh);
		RB_body_set_activation_state(rbo->physics_object, rbo->flag & RBO_FLAG_USE_DEACTIVATION);

		if (rbo->type == RBO_TYPE_PASSIVE || rbo->flag & RBO_FLAG_START_DEACTIVATED)
			RB_body_deactivate(rbo->physics_object);


		RB_body_set_linear_factor(rbo->physics_object,
		                          (ob->protectflag & OB_LOCK_LOCX) == 0,
		                          (ob->protectflag & OB_LOCK_LOCY) == 0,
		                          (ob->protectflag & OB_LOCK_LOCZ) == 0);
		RB_body_set_angular_factor(rbo->physics_object,
		                           (ob->protectflag & OB_LOCK_ROTX) == 0,
		                           (ob->protectflag & OB_LOCK_ROTY) == 0,
		                           (ob->protectflag & OB_LOCK_ROTZ) == 0);

		RB_body_set_mass(rbo->physics_object, RBO_GET_MASS(rbo));
		RB_body_set_kinematic_state(rbo->physics_object, rbo->flag & RBO_FLAG_KINEMATIC || rbo->flag & RBO_FLAG_DISABLED);
	}

	if (rbw && rbw->physics_world && rbo->physics_object)
		RB_dworld_add_body(rbw->physics_world, rbo->physics_object, rbo->col_groups, mi, ob);

	rbo->flag &= ~RBO_FLAG_NEEDS_VALIDATE;
	rbo->flag &= ~RBO_FLAG_KINEMATIC_REBUILD;
}



#if 0 // XXX: not defined yet
		case RB_SHAPE_COMPOUND:
			volume = 0.0f;
			break;
#endif

/* --------------------- */

/**
 * Create physics sim representation of object given RigidBody settings
 *
 * < rebuild: even if an instance already exists, replace it
 */
static void rigidbody_validate_sim_object(RigidBodyWorld *rbw, Object *ob, bool rebuild)
{
	RigidBodyOb *rbo = (ob) ? ob->rigidbody_object : NULL;
	float loc[3];
	float rot[4];

	/* sanity checks:
	 *	- object doesn't have RigidBody info already: then why is it here?
	 */
	if (rbo == NULL)
		return;

	/* make sure collision shape exists */
	/* FIXME we shouldn't always have to rebuild collision shapes when rebuilding objects, but it's needed for constraints to update correctly */
	if (rbo->physics_shape == NULL || rebuild)
		rigidbody_validate_sim_shape(ob, true);

	if (rbo->physics_object && rebuild == false) {
		RB_dworld_remove_body(rbw->physics_world, rbo->physics_object);
	}
	if (!rbo->physics_object || rebuild) {
		/* remove rigid body if it already exists before creating a new one */
		if (rbo->physics_object) {
			RB_body_delete(rbo->physics_object);
		}

		mat4_to_loc_quat(loc, rot, ob->obmat);

		rbo->physics_object = RB_body_new(rbo->physics_shape, loc, rot);

		RB_body_set_friction(rbo->physics_object, rbo->friction);
		RB_body_set_restitution(rbo->physics_object, rbo->restitution);

		RB_body_set_damping(rbo->physics_object, rbo->lin_damping, rbo->ang_damping);
		RB_body_set_sleep_thresh(rbo->physics_object, rbo->lin_sleep_thresh, rbo->ang_sleep_thresh);
		RB_body_set_activation_state(rbo->physics_object, rbo->flag & RBO_FLAG_USE_DEACTIVATION);

		if (rbo->type == RBO_TYPE_PASSIVE || rbo->flag & RBO_FLAG_START_DEACTIVATED)
			RB_body_deactivate(rbo->physics_object);


		RB_body_set_linear_factor(rbo->physics_object,
		                          (ob->protectflag & OB_LOCK_LOCX) == 0,
		                          (ob->protectflag & OB_LOCK_LOCY) == 0,
		                          (ob->protectflag & OB_LOCK_LOCZ) == 0);
		RB_body_set_angular_factor(rbo->physics_object,
		                           (ob->protectflag & OB_LOCK_ROTX) == 0,
		                           (ob->protectflag & OB_LOCK_ROTY) == 0,
		                           (ob->protectflag & OB_LOCK_ROTZ) == 0);

		RB_body_set_mass(rbo->physics_object, RBO_GET_MASS(rbo));
		RB_body_set_kinematic_state(rbo->physics_object, rbo->flag & RBO_FLAG_KINEMATIC || rbo->flag & RBO_FLAG_DISABLED);
	}

	if (rbw && rbw->physics_world)
		RB_dworld_add_body(rbw->physics_world, rbo->physics_object, rbo->col_groups, NULL, ob);
}

/* --------------------- */

/**
 * Create physics sim representation of constraint given rigid body constraint settings
 *
 * < rebuild: even if an instance already exists, replace it
 */
static void rigidbody_validate_sim_constraint(RigidBodyWorld *rbw, Object *ob, bool rebuild)
{
	RigidBodyCon *rbc = (ob) ? ob->rigidbody_constraint : NULL;
	float loc[3];
	float rot[4];
	float lin_lower;
	float lin_upper;
	float ang_lower;
	float ang_upper;

	/* sanity checks:
	 *	- object should have a rigid body constraint
	 *  - rigid body constraint should have at least one constrained object
	 */
	if (rbc == NULL) {
		return;
	}

	if (ELEM(NULL, rbc->ob1, rbc->ob1->rigidbody_object, rbc->ob2, rbc->ob2->rigidbody_object)) {
		if (rbc->physics_constraint) {
			RB_dworld_remove_constraint(rbw->physics_world, rbc->physics_constraint);
			RB_constraint_delete(rbc->physics_constraint);
			rbc->physics_constraint = NULL;
		}
		return;
	}

	if (rbc->physics_constraint && rebuild == false) {
		RB_dworld_remove_constraint(rbw->physics_world, rbc->physics_constraint);
	}
	if (rbc->physics_constraint == NULL || rebuild) {
		rbRigidBody *rb1 = rbc->ob1->rigidbody_object->physics_object;
		rbRigidBody *rb2 = rbc->ob2->rigidbody_object->physics_object;

		/* remove constraint if it already exists before creating a new one */
		if (rbc->physics_constraint) {
			RB_constraint_delete(rbc->physics_constraint);
			rbc->physics_constraint = NULL;
		}

		mat4_to_loc_quat(loc, rot, ob->obmat);

		if (rb1 && rb2) {
			switch (rbc->type) {
				case RBC_TYPE_POINT:
					rbc->physics_constraint = RB_constraint_new_point(loc, rb1, rb2);
					break;
				case RBC_TYPE_FIXED:
					rbc->physics_constraint = RB_constraint_new_fixed(loc, rot, rb1, rb2);
					break;
				case RBC_TYPE_HINGE:
					rbc->physics_constraint = RB_constraint_new_hinge(loc, rot, rb1, rb2);
					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Z) {
						RB_constraint_set_limits_hinge(rbc->physics_constraint, rbc->limit_ang_z_lower, rbc->limit_ang_z_upper);
					}
					else
						RB_constraint_set_limits_hinge(rbc->physics_constraint, 0.0f, -1.0f);
					break;
				case RBC_TYPE_SLIDER:
					rbc->physics_constraint = RB_constraint_new_slider(loc, rot, rb1, rb2);
					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X)
						RB_constraint_set_limits_slider(rbc->physics_constraint, rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
					else
						RB_constraint_set_limits_slider(rbc->physics_constraint, 0.0f, -1.0f);
					break;
				case RBC_TYPE_PISTON:
					rbc->physics_constraint = RB_constraint_new_piston(loc, rot, rb1, rb2);
					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X) {
						lin_lower = rbc->limit_lin_x_lower;
						lin_upper = rbc->limit_lin_x_upper;
					}
					else {
						lin_lower = 0.0f;
						lin_upper = -1.0f;
					}
					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_X) {
						ang_lower = rbc->limit_ang_x_lower;
						ang_upper = rbc->limit_ang_x_upper;
					}
					else {
						ang_lower = 0.0f;
						ang_upper = -1.0f;
					}
					RB_constraint_set_limits_piston(rbc->physics_constraint, lin_lower, lin_upper, ang_lower, ang_upper);
					break;
				case RBC_TYPE_6DOF_SPRING:
					rbc->physics_constraint = RB_constraint_new_6dof_spring(loc, rot, rb1, rb2);

					RB_constraint_set_spring_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->flag & RBC_FLAG_USE_SPRING_X);
					RB_constraint_set_stiffness_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->spring_stiffness_x);
					RB_constraint_set_damping_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->spring_damping_x);

					RB_constraint_set_spring_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->flag & RBC_FLAG_USE_SPRING_Y);
					RB_constraint_set_stiffness_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->spring_stiffness_y);
					RB_constraint_set_damping_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->spring_damping_y);

					RB_constraint_set_spring_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->flag & RBC_FLAG_USE_SPRING_Z);
					RB_constraint_set_stiffness_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->spring_stiffness_z);
					RB_constraint_set_damping_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->spring_damping_z);

					RB_constraint_set_equilibrium_6dof_spring(rbc->physics_constraint);
				/* fall-through */
				case RBC_TYPE_6DOF:
					if (rbc->type == RBC_TYPE_6DOF)     /* a litte awkward but avoids duplicate code for limits */
						rbc->physics_constraint = RB_constraint_new_6dof(loc, rot, rb1, rb2);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_X, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_Y)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->limit_lin_y_lower, rbc->limit_lin_y_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Y, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_Z)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->limit_lin_z_lower, rbc->limit_lin_z_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Z, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_X)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_X, rbc->limit_ang_x_lower, rbc->limit_ang_x_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_X, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Y)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Y, rbc->limit_ang_y_lower, rbc->limit_ang_y_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Y, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Z)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Z, rbc->limit_ang_z_lower, rbc->limit_ang_z_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Z, 0.0f, -1.0f);
					break;
				case RBC_TYPE_MOTOR:
					rbc->physics_constraint = RB_constraint_new_motor(loc, rot, rb1, rb2);

					RB_constraint_set_enable_motor(rbc->physics_constraint, rbc->flag & RBC_FLAG_USE_MOTOR_LIN, rbc->flag & RBC_FLAG_USE_MOTOR_ANG);
					RB_constraint_set_max_impulse_motor(rbc->physics_constraint, rbc->motor_lin_max_impulse, rbc->motor_ang_max_impulse);
					RB_constraint_set_target_velocity_motor(rbc->physics_constraint, rbc->motor_lin_target_velocity, rbc->motor_ang_target_velocity);
					break;
			}
		}
		else { /* can't create constraint without both rigid bodies */
			return;
		}

		RB_constraint_set_enabled(rbc->physics_constraint, rbc->flag & RBC_FLAG_ENABLED);

		if (rbc->flag & RBC_FLAG_USE_BREAKING)
			RB_constraint_set_breaking_threshold(rbc->physics_constraint, rbc->breaking_threshold);
		else
			RB_constraint_set_breaking_threshold(rbc->physics_constraint, FLT_MAX);

		if (rbc->flag & RBC_FLAG_OVERRIDE_SOLVER_ITERATIONS)
			RB_constraint_set_solver_iterations(rbc->physics_constraint, rbc->num_solver_iterations);
		else
			RB_constraint_set_solver_iterations(rbc->physics_constraint, -1);
	}

	if (rbw && rbw->physics_world && rbc->physics_constraint) {
		RB_dworld_add_constraint(rbw->physics_world, rbc->physics_constraint, rbc->flag & RBC_FLAG_DISABLE_COLLISIONS);
	}
}

/* Create physics sim representation of constraint given rigid body constraint settings
 * < rebuild: even if an instance already exists, replace it
 */
void BKE_rigidbody_validate_sim_shard_constraint(RigidBodyWorld *rbw, RigidBodyShardCon *rbc, short rebuild)
{
	float loc[3];
	float rot[4];
	float lin_lower;
	float lin_upper;
	float ang_lower;
	float ang_upper;
	rbRigidBody *rb1;
	rbRigidBody *rb2;

	/* sanity checks:
	 *	- object should have a rigid body constraint
	 *  - rigid body constraint should have at least one constrained object
	 */
	if (rbc == NULL) {
		return;
	}

	if (ELEM(NULL, rbc->mi1, rbc->mi1->rigidbody, rbc->mi2, rbc->mi2->rigidbody)) {
		if (rbc->physics_constraint) {
			RB_dworld_remove_constraint(rbw->physics_world, rbc->physics_constraint);
			RB_constraint_delete(rbc->physics_constraint);
			rbc->physics_constraint = NULL;
		}
		return;
	}
	
	if (rbc->mi1->rigidbody)
	{
		rb1 = rbc->mi1->rigidbody->physics_object;
	}
	
	if (rbc->mi2->rigidbody)
	{
		rb2 = rbc->mi2->rigidbody->physics_object;
	}

	if (rbc->physics_constraint) {
		if (rebuild == false)
		{
			if (!(rbc->flag & RBC_FLAG_USE_KINEMATIC_DEACTIVATION))
			{
				RB_dworld_remove_constraint(rbw->physics_world, rbc->physics_constraint);
			}
		}
	}
	if (rbc->physics_constraint == NULL || rebuild || (rbc->flag & RBC_FLAG_USE_KINEMATIC_DEACTIVATION)) {

		/* remove constraint if it already exists before creating a new one */
		if (rbc->physics_constraint) {
			RB_constraint_delete(rbc->physics_constraint);
			rbc->physics_constraint = NULL;
		}

		/* do this for all constraints */
		copy_v3_v3(loc, rbc->mi1->rigidbody->pos);
		copy_v4_v4(rot, rbc->mi1->rigidbody->orn);

		if (rb1 && rb2) {
			switch (rbc->type) {
				case RBC_TYPE_POINT:
					rbc->physics_constraint = RB_constraint_new_point(loc, rb1, rb2);
					break;
				case RBC_TYPE_FIXED:
					rbc->physics_constraint = RB_constraint_new_fixed(loc, rot, rb1, rb2);
					break;
				case RBC_TYPE_HINGE:
					rbc->physics_constraint = RB_constraint_new_hinge(loc, rot, rb1, rb2);
					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Z) {
						RB_constraint_set_limits_hinge(rbc->physics_constraint, rbc->limit_ang_z_lower, rbc->limit_ang_z_upper);
					}
					else
						RB_constraint_set_limits_hinge(rbc->physics_constraint, 0.0f, -1.0f);
					break;
				case RBC_TYPE_SLIDER:
					rbc->physics_constraint = RB_constraint_new_slider(loc, rot, rb1, rb2);
					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X)
						RB_constraint_set_limits_slider(rbc->physics_constraint, rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
					else
						RB_constraint_set_limits_slider(rbc->physics_constraint, 0.0f, -1.0f);
					break;
				case RBC_TYPE_PISTON:
					rbc->physics_constraint = RB_constraint_new_piston(loc, rot, rb1, rb2);
					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X) {
						lin_lower = rbc->limit_lin_x_lower;
						lin_upper = rbc->limit_lin_x_upper;
					}
					else {
						lin_lower = 0.0f;
						lin_upper = -1.0f;
					}
					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_X) {
						ang_lower = rbc->limit_ang_x_lower;
						ang_upper = rbc->limit_ang_x_upper;
					}
					else {
						ang_lower = 0.0f;
						ang_upper = -1.0f;
					}
					RB_constraint_set_limits_piston(rbc->physics_constraint, lin_lower, lin_upper, ang_lower, ang_upper);
					break;
				case RBC_TYPE_6DOF_SPRING:
					rbc->physics_constraint = RB_constraint_new_6dof_spring(loc, rot, rb1, rb2);

					RB_constraint_set_spring_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->flag & RBC_FLAG_USE_SPRING_X);
					RB_constraint_set_stiffness_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->spring_stiffness_x);
					RB_constraint_set_damping_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->spring_damping_x);

					RB_constraint_set_spring_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->flag & RBC_FLAG_USE_SPRING_Y);
					RB_constraint_set_stiffness_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->spring_stiffness_y);
					RB_constraint_set_damping_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->spring_damping_y);

					RB_constraint_set_spring_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->flag & RBC_FLAG_USE_SPRING_Z);
					RB_constraint_set_stiffness_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->spring_stiffness_z);
					RB_constraint_set_damping_6dof_spring(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->spring_damping_z);

					RB_constraint_set_equilibrium_6dof_spring(rbc->physics_constraint);
				/* fall through */
				case RBC_TYPE_6DOF:
					if (rbc->type == RBC_TYPE_6DOF)     /* a litte awkward but avoids duplicate code for limits */
						rbc->physics_constraint = RB_constraint_new_6dof(loc, rot, rb1, rb2);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_X, rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_X, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_Y)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Y, rbc->limit_lin_y_lower, rbc->limit_lin_y_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Y, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_Z)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Z, rbc->limit_lin_z_lower, rbc->limit_lin_z_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_LIN_Z, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_X)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_X, rbc->limit_ang_x_lower, rbc->limit_ang_x_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_X, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Y)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Y, rbc->limit_ang_y_lower, rbc->limit_ang_y_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Y, 0.0f, -1.0f);

					if (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Z)
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Z, rbc->limit_ang_z_lower, rbc->limit_ang_z_upper);
					else
						RB_constraint_set_limits_6dof(rbc->physics_constraint, RB_LIMIT_ANG_Z, 0.0f, -1.0f);
					break;
				case RBC_TYPE_MOTOR:
					rbc->physics_constraint = RB_constraint_new_motor(loc, rot, rb1, rb2);

					RB_constraint_set_enable_motor(rbc->physics_constraint, rbc->flag & RBC_FLAG_USE_MOTOR_LIN, rbc->flag & RBC_FLAG_USE_MOTOR_ANG);
					RB_constraint_set_max_impulse_motor(rbc->physics_constraint, rbc->motor_lin_max_impulse, rbc->motor_ang_max_impulse);
					RB_constraint_set_target_velocity_motor(rbc->physics_constraint, rbc->motor_lin_target_velocity, rbc->motor_ang_target_velocity);
					break;
			}
		}
		else { /* can't create constraint without both rigid bodies */
			return;
		}

		RB_constraint_set_enabled(rbc->physics_constraint, rbc->flag & RBC_FLAG_ENABLED);

		if (rbc->flag & RBC_FLAG_USE_BREAKING)
			RB_constraint_set_breaking_threshold(rbc->physics_constraint, rbc->breaking_threshold);
		else
			RB_constraint_set_breaking_threshold(rbc->physics_constraint, FLT_MAX);

		if (rbc->flag & RBC_FLAG_OVERRIDE_SOLVER_ITERATIONS)
			RB_constraint_set_solver_iterations(rbc->physics_constraint, rbc->num_solver_iterations);
		else
			RB_constraint_set_solver_iterations(rbc->physics_constraint, -1);
	}

	if ((rbw && rbw->physics_world && rbc->physics_constraint)) {
		RB_dworld_add_constraint(rbw->physics_world, rbc->physics_constraint, rbc->flag & RBC_FLAG_DISABLE_COLLISIONS);
	}

	rbc->flag &= ~RBC_FLAG_USE_KINEMATIC_DEACTIVATION;
}

static bool colgroup_check(int group1, int group2)
{
	int i = 0;
	for (i = 0; i < 20; i++)
	{
		int v1, v2;
		v1 = (group1 & (1 << i));
		v2 = (group2 & (1 << i));

		//printf("%d, %d, %d\n", i, v1, v2);

		if ((v1 > 0) && (v1 == v2))
		{
			return true;
		}
	}

	return false;
}

//this allows partial object activation, only some shards will be activated, called from bullet(!)
static int filterCallback(void* world, void* island1, void* island2, void *blenderOb1, void* blenderOb2) {
	MeshIsland* mi1, *mi2;
	RigidBodyWorld *rbw = (RigidBodyWorld*)world;
	Object* ob1, *ob2;
	int ob_index1, ob_index2;
	FractureModifierData *fmd1, *fmd2;
	bool validOb = true;

	mi1 = (MeshIsland*)island1;
	mi2 = (MeshIsland*)island2;

	if (rbw == NULL)
	{
		return 1;
	}

	/*if ((mi1 == NULL) || (mi2 == NULL)) {
		return 1;
	}*/

	//cache offset map is a dull name for that...
	if (mi1 != NULL)
	{
		ob_index1 = rbw->cache_offset_map[mi1->linear_index];
		ob1 = rbw->objects[ob_index1];
	}
	else
	{
		ob1 = blenderOb1;
	}

	if (mi2 != NULL)
	{
		ob_index2 = rbw->cache_offset_map[mi2->linear_index];
		ob2 = rbw->objects[ob_index2];
	}
	else
	{
		ob2 = blenderOb2;
	}

	if ((mi1 != NULL) && (mi2 != NULL)) {
		validOb = (ob_index1 != ob_index2 && colgroup_check(ob1->rigidbody_object->col_groups, ob2->rigidbody_object->col_groups) &&
				  ((mi1->rigidbody->flag & RBO_FLAG_KINEMATIC) || (mi2->rigidbody->flag & RBO_FLAG_KINEMATIC)));
	}
	else if ((mi1 == NULL) && (mi2 != NULL)) {
		validOb = (colgroup_check(ob1->rigidbody_object->col_groups, ob2->rigidbody_object->col_groups) &&
		          ((ob1->rigidbody_object->flag & RBO_FLAG_KINEMATIC) || (mi2->rigidbody->flag & RBO_FLAG_KINEMATIC)));
	}
	else if ((mi1 != NULL) && (mi2 == NULL)) {
		validOb = (colgroup_check(ob1->rigidbody_object->col_groups, ob2->rigidbody_object->col_groups) &&
		          ((mi1->rigidbody->flag & RBO_FLAG_KINEMATIC) || (ob2->rigidbody_object->flag & RBO_FLAG_KINEMATIC)));
	}
	else
	{
		validOb = (colgroup_check(ob1->rigidbody_object->col_groups, ob2->rigidbody_object->col_groups) &&
		          ((ob1->rigidbody_object->flag & RBO_FLAG_KINEMATIC) || (ob2->rigidbody_object->flag & RBO_FLAG_KINEMATIC)));
	}

	if (validOb)
	{
		MeshIsland *mi;

		if (ob1->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION)
		{
			bool valid = true, valid2 = true;
			RigidBodyShardCon *con;
			fmd1 = (FractureModifierData*)modifiers_findByType(ob1, eModifierType_Fracture);
			valid = valid && (fmd1 != NULL);
			valid = valid && (ob1->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION);
			valid = valid && (ob2->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION);

			valid2 = valid2 && (fmd1 != NULL);
			valid2 = valid2 && (fmd1->use_constraints == false);

			if (valid || valid2)
			{
				for (mi = fmd1->meshIslands.first; mi; mi = mi->next)
				{
					RigidBodyOb* rbo = mi->rigidbody;
					if ((mi->rigidbody->flag & RBO_FLAG_KINEMATIC) && ((mi1 == mi)))
					{
						rbo->flag &= ~RBO_FLAG_KINEMATIC;
						rbo->flag |= RBO_FLAG_KINEMATIC_REBUILD;
						rbo->flag |= RBO_FLAG_NEEDS_VALIDATE;
					}
				}

				for (con = fmd1->meshConstraints.first; con; con = con->next)
				{
					RB_dworld_remove_constraint(rbw->physics_world, con->physics_constraint);
					con->flag |= RBC_FLAG_NEEDS_VALIDATE;
					con->flag |= RBC_FLAG_USE_KINEMATIC_DEACTIVATION;
				}
			}
			else if (!fmd1)
			{
				RigidBodyOb* rbo = ob1->rigidbody_object;

				if (rbo)
				{
					rbo->flag &= ~RBO_FLAG_KINEMATIC;
					rbo->flag |= RBO_FLAG_KINEMATIC_REBUILD;
					rbo->flag |= RBO_FLAG_NEEDS_VALIDATE;
				}
			}
		}

		if (ob2->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION)
		{
			bool valid = true, valid2 = true;
			RigidBodyShardCon *con;
			fmd2 = (FractureModifierData*)modifiers_findByType(ob2, eModifierType_Fracture);
			valid = valid && (fmd2 != NULL);
			valid = valid && (ob2->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION);
			valid = valid && (ob1->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION);

			valid2 = valid2 && (fmd2 != NULL);
			valid2 = valid2 && (fmd2->use_constraints == false);

			if (valid || valid2)
			{
				for (mi = fmd2->meshIslands.first; mi; mi = mi->next)
				{
					RigidBodyOb* rbo = mi->rigidbody;

					if ((mi->rigidbody->flag & RBO_FLAG_KINEMATIC) && ((mi2 == mi)))
					{
						rbo->flag &= ~RBO_FLAG_KINEMATIC;
						rbo->flag |= RBO_FLAG_KINEMATIC_REBUILD;
						rbo->flag |= RBO_FLAG_NEEDS_VALIDATE;
					}
				}

				for (con = fmd2->meshConstraints.first; con; con = con->next)
				{
					RB_dworld_remove_constraint(rbw->physics_world, con->physics_constraint);
					con->flag |= RBC_FLAG_NEEDS_VALIDATE;
					con->flag |= RBC_FLAG_USE_KINEMATIC_DEACTIVATION;
				}
			}
			else if (!fmd2)
			{
				RigidBodyOb* rbo = ob2->rigidbody_object;

				if (rbo)
				{
					rbo->flag &= ~RBO_FLAG_KINEMATIC;
					rbo->flag |= RBO_FLAG_KINEMATIC_REBUILD;
					rbo->flag |= RBO_FLAG_NEEDS_VALIDATE;
				}
			}
		}
	}

	return colgroup_check(ob1->rigidbody_object->col_groups, ob2->rigidbody_object->col_groups);
}

#if 0
static void contactCallback(rbContactPoint* cp, void* world)
{
	MeshIsland* mi1, *mi2;
	RigidBodyWorld *rbw = (RigidBodyWorld*)world;
	Object* ob1, *ob2;
	int ob_index1, ob_index2;
	FractureModifierData *fmd1, *fmd2;
	float force = cp->contact_force;

	mi1 = (MeshIsland*)cp->contact_bodyA;
	mi2 = (MeshIsland*)cp->contact_bodyB;

	if (rbw == NULL)
	{
		return;
	}

	if ((mi1 == NULL) || (mi2 == NULL)) {
		return;
	}

	//cache offset map is a dull name for that...
	ob_index1 = rbw->cache_offset_map[mi1->linear_index];
	ob_index2 = rbw->cache_offset_map[mi2->linear_index];

	if (ob_index1 != ob_index2) // &&
	   //((mi1->rigidbody->flag & RBO_FLAG_KINEMATIC) ||
	   //(mi2->rigidbody->flag & RBO_FLAG_KINEMATIC)))
	{
		float linvel[3], angvel[3];
		MeshIsland *mi;
		ob1 = rbw->objects[ob_index1];
		if (ob1->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION)
		{
			fmd1 = (FractureModifierData*)modifiers_findByType(ob1, eModifierType_Fracture);
			RB_body_get_linear_velocity(mi1->rigidbody->physics_object, linvel);
			RB_body_get_angular_velocity(mi1->rigidbody->physics_object, angvel);

			mul_v3_fl(linvel, force);
			mul_v3_fl(angvel, force);

			for (mi = fmd1->meshIslands.first; mi; mi = mi->next)
			{
				RigidBodyOb* rbo = mi->rigidbody;
				//if (mi->rigidbody->flag & RBO_FLAG_KINEMATIC)
				{
					//rbo->flag &= ~RBO_FLAG_KINEMATIC;
					//rbo->flag |= RBO_FLAG_KINEMATIC_REBUILD;
					RB_body_set_linear_velocity(rbo->physics_object, linvel);
					RB_body_set_angular_velocity(rbo->physics_object, angvel);
				}
			}
		}

		ob2 = rbw->objects[ob_index2];
		if (ob2->rigidbody_object->flag & RBO_FLAG_USE_KINEMATIC_DEACTIVATION)
		{
			fmd2 = (FractureModifierData*)modifiers_findByType(ob2, eModifierType_Fracture);
			RB_body_get_linear_velocity(mi2->rigidbody->physics_object, linvel);
			RB_body_get_angular_velocity(mi2->rigidbody->physics_object, angvel);

			mul_v3_fl(linvel, force);
			mul_v3_fl(angvel, force);

			for (mi = fmd2->meshIslands.first; mi; mi = mi->next)
			{
				RigidBodyOb* rbo = mi->rigidbody;

				//if (mi->rigidbody->flag & RBO_FLAG_KINEMATIC)
				{
					//rbo->flag &= ~RBO_FLAG_KINEMATIC;
					//rbo->flag |= RBO_FLAG_KINEMATIC_REBUILD;
					RB_body_set_linear_velocity(rbo->physics_object, linvel);
					RB_body_set_angular_velocity(rbo->physics_object, angvel);
				}
			}
		}
	}
}
#endif

/* --------------------- */

/* Create physics sim world given RigidBody world settings */
// NOTE: this does NOT update object references that the scene uses, in case those aren't ready yet!
void BKE_rigidbody_validate_sim_world(Scene *scene, RigidBodyWorld *rbw, bool rebuild)
{
	/* sanity checks */
	if (rbw == NULL)
		return;

	/* create new sim world */
	if (rebuild || rbw->physics_world == NULL) {
		if (rbw->physics_world)
			RB_dworld_delete(rbw->physics_world);
		rbw->physics_world = RB_dworld_new(scene->physics_settings.gravity, rbw, filterCallback, NULL);
	}

	RB_dworld_set_solver_iterations(rbw->physics_world, rbw->num_solver_iterations);
	RB_dworld_set_split_impulse(rbw->physics_world, rbw->flag & RBW_FLAG_USE_SPLIT_IMPULSE);
}

/* ************************************** */
/* Setup Utilities - Create Settings Blocks */

/* Set up RigidBody world */
RigidBodyWorld *BKE_rigidbody_create_world(Scene *scene)
{
	/* try to get whatever RigidBody world that might be representing this already */
	RigidBodyWorld *rbw;

	/* sanity checks
	 *	- there must be a valid scene to add world to
	 *	- there mustn't be a sim world using this group already
	 */
	if (scene == NULL)
		return NULL;

	/* create a new sim world */
	rbw = MEM_callocN(sizeof(RigidBodyWorld), "RigidBodyWorld");

	/* set default settings */
	rbw->effector_weights = BKE_add_effector_weights(NULL);

	rbw->ltime = PSFRA;

	rbw->time_scale = 1.0f;

	rbw->steps_per_second = 60; /* Bullet default (60 Hz) */
	rbw->num_solver_iterations = 10; /* 10 is bullet default */

	rbw->pointcache = BKE_ptcache_add(&(rbw->ptcaches));
	rbw->pointcache->step = 1;
	rbw->object_changed = false;
	rbw->refresh_modifiers = false;

	rbw->objects = MEM_mallocN(sizeof(Object *), "objects");
	rbw->cache_index_map = MEM_mallocN(sizeof(RigidBodyOb *), "cache_index_map");
	rbw->cache_offset_map = MEM_mallocN(sizeof(int), "cache_offset_map");

	/* return this sim world */
	return rbw;
}

/* Add rigid body settings to the specified shard */
RigidBodyOb *BKE_rigidbody_create_shard(Scene *scene, Object *ob, MeshIsland *mi)
{
	RigidBodyOb *rbo;
	RigidBodyWorld *rbw = BKE_rigidbody_get_world(scene);
	float centr[3], size[3];

	/* sanity checks
	 *	- rigidbody world must exist
	 *	- shard must exist
	 *	- cannot add rigid body if it already exists
	 */
	if (mi == NULL || (mi->rigidbody != NULL))
		return NULL;

	if (ob->type != OB_MESH && ob->type != OB_FONT && ob->type != OB_CURVE && ob->type != OB_SURF) {
		return NULL;
	}
	
	if ((((Mesh *)ob->data)->totvert == 0) && (ob->type == OB_MESH)) {
		return NULL;
	}

	/* Add rigid body world and group if they don't exist for convenience */
	if (rbw == NULL) {
		rbw = BKE_rigidbody_create_world(scene);
		BKE_rigidbody_validate_sim_world(scene, rbw, false);
		scene->rigidbody_world = rbw;
	}
	if (rbw->group == NULL) {
		rbw->group = BKE_group_add(G.main, "RigidBodyWorld");
	}

	/* make rigidbody object settings */
	if (ob->rigidbody_object == NULL) {
		ob->rigidbody_object = BKE_rigidbody_create_object(scene, ob, mi->ground_weight > 0.5f ? RBO_TYPE_PASSIVE : RBO_TYPE_ACTIVE);
	}
	else {
		ob->rigidbody_object->type = mi->ground_weight > 0.5f ? RBO_TYPE_PASSIVE : RBO_TYPE_ACTIVE;
		ob->rigidbody_object->flag |= RBO_FLAG_NEEDS_VALIDATE;
	}

	if (!BKE_group_object_exists(rbw->group, ob))
		BKE_group_object_add(rbw->group, ob, scene, NULL);

	DAG_id_tag_update(&ob->id, OB_RECALC_OB);

	/* since we are always member of an object, dupe its settings,
	 * create new settings data, and link it up */
	rbo = BKE_rigidbody_copy_object(ob);
	rbo->type = mi->ground_weight > 0.5f ? RBO_TYPE_PASSIVE : RBO_TYPE_ACTIVE;

	/* set initial transform */
	mat4_to_loc_quat(rbo->pos, rbo->orn, ob->obmat);
	mat4_to_size(size, ob->obmat);

	//add initial "offset" (centroid), maybe subtract ob->obmat ?? (not sure)
	copy_v3_v3(centr, mi->centroid);
	mul_v3_v3(centr, size);
	mul_qt_v3(rbo->orn, centr);
	add_v3_v3(rbo->pos, centr);

	/* return this object */
	return rbo;
}

RigidBodyWorld *BKE_rigidbody_world_copy(RigidBodyWorld *rbw)
{
	RigidBodyWorld *rbwn = MEM_dupallocN(rbw);

	if (rbw->effector_weights)
		rbwn->effector_weights = MEM_dupallocN(rbw->effector_weights);
	if (rbwn->group)
		id_us_plus(&rbwn->group->id);
	if (rbwn->constraints)
		id_us_plus(&rbwn->constraints->id);

	rbwn->pointcache = BKE_ptcache_copy_list(&rbwn->ptcaches, &rbw->ptcaches, true);

	rbwn->objects = NULL;
	rbwn->physics_world = NULL;
	rbwn->numbodies = 0;

	rbwn->cache_index_map = NULL;
	rbwn->cache_offset_map = NULL;

	return rbwn;
}

void BKE_rigidbody_world_groups_relink(RigidBodyWorld *rbw)
{
	if (rbw->group && rbw->group->id.newid)
		rbw->group = (Group *)rbw->group->id.newid;
	if (rbw->constraints && rbw->constraints->id.newid)
		rbw->constraints = (Group *)rbw->constraints->id.newid;
	if (rbw->effector_weights->group && rbw->effector_weights->group->id.newid)
		rbw->effector_weights->group = (Group *)rbw->effector_weights->group->id.newid;
}

/* Add rigid body settings to the specified object */
RigidBodyOb *BKE_rigidbody_create_object(Scene *scene, Object *ob, short type)
{
	RigidBodyOb *rbo;
	RigidBodyWorld *rbw = scene->rigidbody_world;

	/* sanity checks
	 *	- rigidbody world must exist
	 *	- object must exist
	 *	- cannot add rigid body if it already exists
	 */
	if (ob == NULL || (ob->rigidbody_object != NULL))
		return NULL;

	/* create new settings data, and link it up */
	rbo = MEM_callocN(sizeof(RigidBodyOb), "RigidBodyOb");

	/* set default settings */
	rbo->type = type;

	rbo->mass = 1.0f;

	rbo->friction = 0.5f; /* best when non-zero. 0.5 is Bullet default */
	rbo->restitution = 0.0f; /* best when zero. 0.0 is Bullet default */

	rbo->margin = 0.04f; /* 0.04 (in meters) is Bullet default */

	rbo->lin_sleep_thresh = 0.4f; /* 0.4 is half of Bullet default */
	rbo->ang_sleep_thresh = 0.5f; /* 0.5 is half of Bullet default */

	rbo->lin_damping = 0.04f; /* 0.04 is game engine default */
	rbo->ang_damping = 0.1f; /* 0.1 is game engine default */

	rbo->col_groups = 1;

	/* use triangle meshes for passive objects
	 * use convex hulls for active objects since dynamic triangle meshes are very unstable
	 */
	if (type == RBO_TYPE_ACTIVE)
		rbo->shape = RB_SHAPE_CONVEXH;
	else
		rbo->shape = RB_SHAPE_TRIMESH;

	rbo->mesh_source = RBO_MESH_DEFORM;

	/* set initial transform */
	mat4_to_loc_quat(rbo->pos, rbo->orn, ob->obmat);

	/* flag cache as outdated */
	BKE_rigidbody_cache_reset(rbw);

	/* return this object */
	return rbo;
}

/* Add rigid body constraint to the specified object */
RigidBodyCon *BKE_rigidbody_create_constraint(Scene *scene, Object *ob, short type)
{
	RigidBodyCon *rbc;
	RigidBodyWorld *rbw = scene->rigidbody_world;

	/* sanity checks
	 *	- rigidbody world must exist
	 *	- object must exist
	 *	- cannot add constraint if it already exists
	 */
	if (ob == NULL || (ob->rigidbody_constraint != NULL))
		return NULL;

	/* create new settings data, and link it up */
	rbc = MEM_callocN(sizeof(RigidBodyCon), "RigidBodyCon");

	/* set default settings */
	rbc->type = type;

	rbc->ob1 = NULL;
	rbc->ob2 = NULL;

	rbc->flag |= RBC_FLAG_ENABLED;
	rbc->flag |= RBC_FLAG_DISABLE_COLLISIONS;

	rbc->breaking_threshold = 10.0f; /* no good default here, just use 10 for now */
	rbc->num_solver_iterations = 10; /* 10 is Bullet default */

	rbc->limit_lin_x_lower = -1.0f;
	rbc->limit_lin_x_upper = 1.0f;
	rbc->limit_lin_y_lower = -1.0f;
	rbc->limit_lin_y_upper = 1.0f;
	rbc->limit_lin_z_lower = -1.0f;
	rbc->limit_lin_z_upper = 1.0f;
	rbc->limit_ang_x_lower = -M_PI_4;
	rbc->limit_ang_x_upper = M_PI_4;
	rbc->limit_ang_y_lower = -M_PI_4;
	rbc->limit_ang_y_upper = M_PI_4;
	rbc->limit_ang_z_lower = -M_PI_4;
	rbc->limit_ang_z_upper = M_PI_4;

	rbc->spring_damping_x = 0.5f;
	rbc->spring_damping_y = 0.5f;
	rbc->spring_damping_z = 0.5f;
	rbc->spring_stiffness_x = 10.0f;
	rbc->spring_stiffness_y = 10.0f;
	rbc->spring_stiffness_z = 10.0f;

	rbc->motor_lin_max_impulse = 1.0f;
	rbc->motor_lin_target_velocity = 1.0f;
	rbc->motor_ang_max_impulse = 1.0f;
	rbc->motor_ang_target_velocity = 1.0f;

	/* flag cache as outdated */
	BKE_rigidbody_cache_reset(rbw);

	/* return this object */
	return rbc;
}

/* Add rigid body constraint to the specified object */
RigidBodyShardCon *BKE_rigidbody_create_shard_constraint(Scene *scene, short type)
{
	RigidBodyShardCon *rbc;
	RigidBodyWorld *rbw = scene->rigidbody_world;

	/* sanity checks
	 *	- rigidbody world must exist
	 *	- object must exist
	 *	- cannot add constraint if it already exists
	 */

	/* create new settings data, and link it up */
	rbc = MEM_callocN(sizeof(RigidBodyShardCon), "RigidBodyCon");

	/* set default settings */
	rbc->type = type;

	rbc->mi1 = NULL;
	rbc->mi2 = NULL;

	rbc->flag |= RBC_FLAG_ENABLED;
	rbc->flag &= ~RBC_FLAG_DISABLE_COLLISIONS;
	rbc->flag |= RBC_FLAG_USE_BREAKING;

	rbc->breaking_threshold = 1.0f; /* no good default here, just use 10 for now */
	rbc->num_solver_iterations = 10; /* 10 is Bullet default */

	rbc->limit_lin_x_lower = -1.0f;
	rbc->limit_lin_x_upper = 1.0f;
	rbc->limit_lin_y_lower = -1.0f;
	rbc->limit_lin_y_upper = 1.0f;
	rbc->limit_lin_z_lower = -1.0f;
	rbc->limit_lin_z_upper = 1.0f;
	rbc->limit_ang_x_lower = -M_PI_4;
	rbc->limit_ang_x_upper = M_PI_4;
	rbc->limit_ang_y_lower = -M_PI_4;
	rbc->limit_ang_y_upper = M_PI_4;
	rbc->limit_ang_z_lower = -M_PI_4;
	rbc->limit_ang_z_upper = M_PI_4;

	rbc->spring_damping_x = 0.5f;
	rbc->spring_damping_y = 0.5f;
	rbc->spring_damping_z = 0.5f;
	rbc->spring_stiffness_x = 10.0f;
	rbc->spring_stiffness_y = 10.0f;
	rbc->spring_stiffness_z = 10.0f;

	rbc->motor_lin_max_impulse = 1.0f;
	rbc->motor_lin_target_velocity = 1.0f;
	rbc->motor_ang_max_impulse = 1.0f;
	rbc->motor_ang_target_velocity = 1.0f;

	/* flag cache as outdated */
	BKE_rigidbody_cache_reset(rbw);

	/* return this object */
	return rbc;
}

/* ************************************** */
/* Utilities API */

/* Get RigidBody world for the given scene, creating one if needed
 *
 * \param scene Scene to find active Rigid Body world for
 */
RigidBodyWorld *BKE_rigidbody_get_world(Scene *scene)
{
	/* sanity check */
	if (scene == NULL)
		return NULL;

	return scene->rigidbody_world;
}

void BKE_rigidbody_remove_shard_con(Scene *scene, RigidBodyShardCon *con)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	if (rbw && rbw->physics_world && con->physics_constraint) {
		RB_dworld_remove_constraint(rbw->physics_world, con->physics_constraint);
		RB_constraint_delete(con->physics_constraint);
		con->physics_constraint = NULL;
	}
}

void BKE_rigidbody_remove_shard(Scene *scene, MeshIsland *mi)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	int i = 0;
	
	/* rbw can be NULL directly after linking / appending objects without their original scenes
	 * if an attempt to refracture is done then, this would crash here with null pointer access */
	if (mi->rigidbody != NULL && rbw != NULL) {
		
		RigidBodyShardCon *con;
		for (i = 0; i < mi->participating_constraint_count; i++) {
			con = mi->participating_constraints[i];
			BKE_rigidbody_remove_shard_con(scene, con);
		}
		
		if (rbw->physics_world && mi->rigidbody && mi->rigidbody->physics_object)
			RB_dworld_remove_body(rbw->physics_world, mi->rigidbody->physics_object);

		if (mi->rigidbody->physics_object) {
			RB_body_delete(mi->rigidbody->physics_object);
			mi->rigidbody->physics_object = NULL;
		}

		if (mi->rigidbody->physics_shape) {
			RB_shape_delete(mi->rigidbody->physics_shape);
			mi->rigidbody->physics_shape = NULL;
		}
		
		/* this SHOULD be the correct global index */
		/* need to check whether we didnt create the rigidbody world manually already, prior to fracture, in this
		 * case cache_index_map might be not initialized ! checking numbodies here, they should be 0 in a fresh
		 * rigidbody world */
		if ((rbw->cache_index_map != NULL) && (rbw->numbodies > 0))
			rbw->cache_index_map[mi->linear_index] = NULL;
	}
}

void BKE_rigidbody_remove_object(Scene *scene, Object *ob)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	RigidBodyOb *rbo = ob->rigidbody_object;
	RigidBodyCon *rbc;
	GroupObject *go;
	ModifierData *md;
	FractureModifierData *rmd;
	RigidBodyShardCon *con;
	MeshIsland *mi;
	int i;
	bool modFound = false;

	if (rbw) {
		for (md = ob->modifiers.first; md; md = md->next) {

			if (md->type == eModifierType_Fracture)
			{
				rmd = (FractureModifierData *)md;
				modFound = true;
				for (con = rmd->meshConstraints.first; con; con = con->next) {
					if (rbw && rbw->physics_world && con->physics_constraint) {
						RB_dworld_remove_constraint(rbw->physics_world, con->physics_constraint);
						RB_constraint_delete(con->physics_constraint);
						con->physics_constraint = NULL;
					}
				}

				for (mi = rmd->meshIslands.first; mi; mi = mi->next) {
					if (mi->rigidbody != NULL) {
						if (rbw->physics_world && mi->rigidbody && mi->rigidbody->physics_object)
							RB_dworld_remove_body(rbw->physics_world, mi->rigidbody->physics_object);
						if (mi->rigidbody->physics_object) {
							RB_body_delete(mi->rigidbody->physics_object);
							mi->rigidbody->physics_object = NULL;
						}

						if (mi->rigidbody->physics_shape) {
							RB_shape_delete(mi->rigidbody->physics_shape);
							mi->rigidbody->physics_shape = NULL;
						}
						
						/* this SHOULD be the correct global index*/
						if (rbw->cache_index_map)
							rbw->cache_index_map[mi->linear_index] = NULL;
						MEM_freeN(mi->rigidbody);
						mi->rigidbody = NULL;
					}
				}
			}
		}

		if (!modFound) {
			/* remove from rigidbody world, free object won't do this */
			if (rbw->physics_world && rbo->physics_object)
				RB_dworld_remove_body(rbw->physics_world, rbo->physics_object);

			/* remove object from array */
			if (rbw && rbw->objects) {
				for (i = 0; i < rbw->numbodies; i++) {
					int index = rbw->cache_offset_map[i];
					if (rbw->objects[index] == ob) {
						rbw->objects[index] = NULL;
					}
					
					if (rbo == rbw->cache_index_map[i]) {
						rbw->cache_index_map[i] = NULL;
						break;
					}
				}
			}

			/* remove object from rigid body constraints */
			if (rbw->constraints) {
				for (go = rbw->constraints->gobject.first; go; go = go->next) {
					Object *obt = go->ob;
					if (obt && obt->rigidbody_constraint) {
						rbc = obt->rigidbody_constraint;
						if (rbc->ob1 == ob) {
							rbc->ob1 = NULL;
							rbc->flag |= RBC_FLAG_NEEDS_VALIDATE;
						}
						if (rbc->ob2 == ob) {
							rbc->ob2 = NULL;
							rbc->flag |= RBC_FLAG_NEEDS_VALIDATE;
						}
					}
				}
			}
			
			/* remove object's settings */
			BKE_rigidbody_free_object(ob);
		}
	}

	/* flag cache as outdated */
	BKE_rigidbody_cache_reset(rbw);
}

void BKE_rigidbody_remove_constraint(Scene *scene, Object *ob)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	RigidBodyCon *rbc = ob->rigidbody_constraint;

	/* remove from rigidbody world, free object won't do this */
	if (rbw && rbw->physics_world && rbc->physics_constraint) {
		RB_dworld_remove_constraint(rbw->physics_world, rbc->physics_constraint);
	}
	/* remove object's settings */
	BKE_rigidbody_free_constraint(ob);

	/* flag cache as outdated */
	BKE_rigidbody_cache_reset(rbw);
}

static int rigidbody_group_count_items(const ListBase *group, int *r_num_objects, int *r_num_shards)
{
	int num_gobjects = 0;
	ModifierData *md;
	FractureModifierData *rmd;
	GroupObject *gob;

	if (r_num_objects == NULL || r_num_shards == NULL)
	{
		return num_gobjects;
	}

	*r_num_objects = 0;
	*r_num_shards = 0;

	for (gob = group->first; gob; gob = gob->next) {
		bool found_modifiers = false;
		for (md = gob->ob->modifiers.first; md; md = md->next) {
			if (md->type == eModifierType_Fracture) {
				rmd = (FractureModifierData *)md;
				if (isModifierActive(rmd))
				{
					found_modifiers = true;
					*r_num_shards += BLI_countlist(&rmd->meshIslands);
				}
			}
		}
		if (found_modifiers == false) {
			(*r_num_objects)++;
		}
		num_gobjects++;
	}

	return num_gobjects;
}

/* ************************************** */
/* Simulation Interface - Bullet */

/* Update object array and rigid body count so they're in sync with the rigid body group */
static void rigidbody_update_ob_array(RigidBodyWorld *rbw)
{
	GroupObject *go;
	ModifierData *md;
	FractureModifierData *rmd;
	MeshIsland *mi;
	int i, j, l = 0, m = 0, n = 0, counter = 0;
	bool ismapped = false;
	
	if (rbw->objects != NULL) {
		MEM_freeN(rbw->objects);
		rbw->objects = NULL;
	}
	
	if (rbw->cache_index_map != NULL) {
		MEM_freeN(rbw->cache_index_map);
		rbw->cache_index_map = NULL;
	}
	
	if (rbw->cache_offset_map != NULL) {
		MEM_freeN(rbw->cache_offset_map);
		rbw->cache_offset_map = NULL;
	}

	l = rigidbody_group_count_items(&rbw->group->gobject, &m, &n);

	rbw->numbodies = m + n;
	rbw->objects = MEM_mallocN(sizeof(Object *) * l, "objects");
	rbw->cache_index_map = MEM_mallocN(sizeof(RigidBodyOb *) * rbw->numbodies, "cache_index_map");
	rbw->cache_offset_map = MEM_mallocN(sizeof(int) * rbw->numbodies, "cache_offset_map");
	printf("RigidbodyCount changed: %d\n", rbw->numbodies);

	for (go = rbw->group->gobject.first, i = 0; go; go = go->next, i++) {
		Object *ob = go->ob;
		rbw->objects[i] = ob;

		for (md = ob->modifiers.first; md; md = md->next) {

			if (md->type == eModifierType_Fracture) {
				rmd = (FractureModifierData *)md;
				if (isModifierActive(rmd)) {
					for (mi = rmd->meshIslands.first, j = 0; mi; mi = mi->next) {
						rbw->cache_index_map[counter] = mi->rigidbody; /* map all shards of an object to this object index*/
						rbw->cache_offset_map[counter] = i;
						mi->linear_index = counter;
						counter++;
						j++;
					}
					ismapped = true;
					break;
				}
			}
		}

		if (!ismapped) {
			rbw->cache_index_map[counter] = ob->rigidbody_object; /*1 object 1 index here (normal case)*/
			rbw->cache_offset_map[counter] = i;
			counter++;
		}

		ismapped = false;
	}
}

static void rigidbody_update_sim_world(Scene *scene, RigidBodyWorld *rbw)
{
	float adj_gravity[3];

	/* adjust gravity to take effector weights into account */
	if (scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
		copy_v3_v3(adj_gravity, scene->physics_settings.gravity);
		mul_v3_fl(adj_gravity, rbw->effector_weights->global_gravity * rbw->effector_weights->weight[0]);
	}
	else {
		zero_v3(adj_gravity);
	}

	/* update gravity, since this RNA setting is not part of RigidBody settings */
	RB_dworld_set_gravity(rbw->physics_world, adj_gravity);

	/* update object array in case there are changes */
	rigidbody_update_ob_array(rbw);
}

static void rigidbody_update_sim_ob(Scene *scene, RigidBodyWorld *rbw, Object *ob, RigidBodyOb *rbo, float centroid[3])
{
	float loc[3];
	float rot[4];
	float scale[3], centr[3];

	/* only update if rigid body exists */
	if (rbo->physics_object == NULL)
		return;

	if (rbo->shape == RB_SHAPE_TRIMESH && rbo->flag & RBO_FLAG_USE_DEFORM) {
		DerivedMesh *dm = ob->derivedDeform;
		if (dm) {
			MVert *mvert = dm->getVertArray(dm);
			int totvert = dm->getNumVerts(dm);
			BoundBox *bb = BKE_object_boundbox_get(ob);

			RB_shape_trimesh_update(rbo->physics_shape, (float *)mvert, totvert, sizeof(MVert), bb->vec[0], bb->vec[6]);
		}
	}
	copy_v3_v3(centr, centroid);
	
	mat4_decompose(loc, rot, scale, ob->obmat);

	/* update scale for all objects */
	RB_body_set_scale(rbo->physics_object, scale);
	/* compensate for embedded convex hull collision margin */
	if (!(rbo->flag & RBO_FLAG_USE_MARGIN) && rbo->shape == RB_SHAPE_CONVEXH)
		RB_shape_set_margin(rbo->physics_shape, RBO_GET_MARGIN(rbo) * MIN3(scale[0], scale[1], scale[2]));

	/* make transformed objects temporarily kinmatic so that they can be moved by the user during simulation */
	if (ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ) {
		RB_body_set_kinematic_state(rbo->physics_object, true);
		RB_body_set_mass(rbo->physics_object, 0.0f);
	}

	/* update rigid body location and rotation for kinematic bodies */
	if (rbo->flag & RBO_FLAG_KINEMATIC || (ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ)) {
		mul_v3_v3(centr, scale);
		mul_qt_v3(rot, centr);
		add_v3_v3(loc, centr);
		RB_body_activate(rbo->physics_object);
		RB_body_set_loc_rot(rbo->physics_object, loc, rot);
	}
	/* update influence of effectors - but don't do it on an effector */
	/* only dynamic bodies need effector update */
	else if (rbo->type == RBO_TYPE_ACTIVE && ((ob->pd == NULL) || (ob->pd->forcefield == PFIELD_NULL))) {
		EffectorWeights *effector_weights = rbw->effector_weights;
		EffectedPoint epoint;
		ListBase *effectors;

		/* get effectors present in the group specified by effector_weights */
		effectors = pdInitEffectors(scene, ob, NULL, effector_weights, true);
		if (effectors) {
			float eff_force[3] = {0.0f, 0.0f, 0.0f};
			float eff_loc[3], eff_vel[3];

			/* create dummy 'point' which represents last known position of object as result of sim */
			// XXX: this can create some inaccuracies with sim position, but is probably better than using unsimulated vals?
			RB_body_get_position(rbo->physics_object, eff_loc);
			//mul_v3_v3(centr, scale);
			//add_v3_v3(eff_loc, centr);

			RB_body_get_linear_velocity(rbo->physics_object, eff_vel);

			pd_point_from_loc(scene, eff_loc, eff_vel, 0, &epoint);

			/* calculate net force of effectors, and apply to sim object
			 *	- we use 'central force' since apply force requires a "relative position" which we don't have...
			 */
			pdDoEffectors(effectors, NULL, effector_weights, &epoint, eff_force, NULL);
			if (G.f & G_DEBUG)
				printf("\tapplying force (%f,%f,%f) to '%s'\n", eff_force[0], eff_force[1], eff_force[2], ob->id.name + 2);
			/* activate object in case it is deactivated */
			if (!is_zero_v3(eff_force))
				RB_body_activate(rbo->physics_object);
			RB_body_apply_central_force(rbo->physics_object, eff_force);
		}
		else if (G.f & G_DEBUG)
			printf("\tno forces to apply to '%s'\n", ob->id.name + 2);

		/* cleanup */
		pdEndEffectors(&effectors);
	}
	/* NOTE: passive objects don't need to be updated since they don't move */

	/* NOTE: no other settings need to be explicitly updated here,
	 * since RNA setters take care of the rest :)
	 */
}

static void validateShard(RigidBodyWorld *rbw, MeshIsland *mi, Object *ob, int rebuild)
{
	if (mi == NULL || mi->rigidbody == NULL) {
		return;
	}

	if (rebuild || (mi->rigidbody->flag & RBO_FLAG_KINEMATIC_REBUILD)) {
		/* World has been rebuilt so rebuild object */
		BKE_rigidbody_validate_sim_shard(rbw, mi, ob, true);
	}
	else if (mi->rigidbody->flag & RBO_FLAG_NEEDS_VALIDATE) {
		BKE_rigidbody_validate_sim_shard(rbw, mi, ob, false);
	}
	/* refresh shape... */
	if (mi->rigidbody->flag & RBO_FLAG_NEEDS_RESHAPE) {
		/* mesh/shape data changed, so force shape refresh */
		BKE_rigidbody_validate_sim_shard_shape(mi, ob, true);
		/* now tell RB sim about it */
		// XXX: we assume that this can only get applied for active/passive shapes that will be included as rigidbodies
		RB_body_set_collision_shape(mi->rigidbody->physics_object, mi->rigidbody->physics_shape);
	}
	mi->rigidbody->flag &= ~(RBO_FLAG_NEEDS_VALIDATE | RBO_FLAG_NEEDS_RESHAPE);
}

/* Updates and validates world, bodies and shapes.
 * < rebuild: rebuild entire simulation
 */
static void rigidbody_update_simulation(Scene *scene, RigidBodyWorld *rbw, bool rebuild)
{
	GroupObject *go;
	MeshIsland *mi = NULL;
	float centroid[3] = {0, 0, 0};
	RigidBodyShardCon *rbsc;

	/* update world */
	if (rebuild) {
		BKE_rigidbody_validate_sim_world(scene, rbw, true);
		rigidbody_update_sim_world(scene, rbw);
	}

	/* update objects */
	for (go = rbw->group->gobject.first; go; go = go->next) {
		Object *ob = go->ob;
		ModifierData *md = NULL;
		FractureModifierData *rmd = NULL;

		if (ob && (ob->type == OB_MESH || ob->type == OB_CURVE || ob->type == OB_SURF || ob->type == OB_FONT)) {

			/* check for fractured objects which want to participate first, then handle other normal objects*/
			for (md = ob->modifiers.first; md; md = md->next) {
				if (md->type == eModifierType_Fracture) {
					rmd = (FractureModifierData *)md;
					break;
				}
			}

			if (isModifierActive(rmd)) {
				float max_con_mass = 0;
			
				int count = BLI_countlist(&rmd->meshIslands);
				for (mi = rmd->meshIslands.first; mi; mi = mi->next) {
					if (mi->rigidbody == NULL) {
						continue;
					}
					else {  /* as usual, but for each shard now, and no constraints*/
						/* perform simulation data updates as tagged */
						/* refresh object... */
						int do_rebuild = rebuild;
						float weight = mi->thresh_weight;
						int breaking_percentage = rmd->breaking_percentage_weighted ? (rmd->breaking_percentage * weight) : rmd->breaking_percentage;
						
						if (rmd->breaking_percentage > 0 || (rmd->breaking_percentage_weighted && weight > 0)) {
							int broken_cons = 0, cons = 0, i = 0;
							RigidBodyShardCon *con;
							
							cons = mi->participating_constraint_count;
							/* calc ratio of broken cons here, per MeshIsland and flag the rest to be broken too*/
							for (i = 0; i < cons; i++) {
								con = mi->participating_constraints[i];
								if (con && con->physics_constraint) {
									if (!RB_constraint_is_enabled(con->physics_constraint)) {
										broken_cons++;
									}
								}
							}
							
							if (cons > 0) {
								if ((float)broken_cons / (float)cons * 100 >= breaking_percentage) {
									/* break all cons if over percentage */
									for (i = 0; i < cons; i++) {
										con = mi->participating_constraints[i];
										if (con) {
											con->flag &= ~RBC_FLAG_ENABLED;
											con->flag |= RBC_FLAG_NEEDS_VALIDATE;
											
											if (con->physics_constraint) {
												RB_constraint_set_enabled(con->physics_constraint, false);
											}
										}
									}
								}
							}
						}
						
						validateShard(rbw, count == 0 ? NULL : mi, ob, do_rebuild);
					}

					/* update simulation object... */
					rigidbody_update_sim_ob(scene, rbw, ob, mi->rigidbody, mi->centroid);
				}

				if (rmd->use_mass_dependent_thresholds) {
					max_con_mass = BKE_rigidbody_calc_max_con_mass(ob);
				}

				for (rbsc = rmd->meshConstraints.first; rbsc; rbsc = rbsc->next) {
					float weight = MIN2(rbsc->mi1->thresh_weight, rbsc->mi2->thresh_weight);
					float breaking_angle = rmd->breaking_angle_weighted ? rmd->breaking_angle * weight : rmd->breaking_angle;
					float breaking_distance = rmd->breaking_distance_weighted ? rmd->breaking_distance * weight : rmd->breaking_distance;
					int iterations;

					if (rmd->solver_iterations_override == 0) {
						iterations = rbw->num_solver_iterations;
					}
					else {
						iterations = rmd->solver_iterations_override;
					}
					
					if (iterations > 0) {
						rbsc->flag |= RBC_FLAG_OVERRIDE_SOLVER_ITERATIONS;
						rbsc->num_solver_iterations = iterations;
					}
					
					if ((rmd->use_mass_dependent_thresholds)) {
						BKE_rigidbody_calc_threshold(max_con_mass, rmd, rbsc);
					}
					
					if (((rmd->breaking_angle) > 0) || (rmd->breaking_angle_weighted && weight > 0) ||
					    (((rmd->breaking_distance > 0) || (rmd->breaking_distance_weighted && weight > 0)) && !rebuild))
					{
						float dist, angle, distdiff, anglediff;
						calc_dist_angle(rbsc, &dist, &angle);
						
						anglediff = fabs(angle - rbsc->start_angle);
						distdiff = fabs(dist - rbsc->start_dist);
						
						if ((rmd->breaking_angle > 0 || (rmd->breaking_angle_weighted && weight > 0)) &&
						    (anglediff > breaking_angle))
						{
							rbsc->flag &= ~RBC_FLAG_ENABLED;
							rbsc->flag |= RBC_FLAG_NEEDS_VALIDATE;
							
							if (rbsc->physics_constraint) {
								RB_constraint_set_enabled(rbsc->physics_constraint, false);
							}
						}
						
						if ((rmd->breaking_distance > 0 || (rmd->breaking_distance_weighted && weight > 0)) &&
						    (distdiff > breaking_distance))
						{
							rbsc->flag &= ~RBC_FLAG_ENABLED;
							rbsc->flag |= RBC_FLAG_NEEDS_VALIDATE;
							
							if (rbsc->physics_constraint) {
								RB_constraint_set_enabled(rbsc->physics_constraint, false);
							}
						}
					}

					if (rebuild || rbsc->mi1->rigidbody->flag & RBO_FLAG_KINEMATIC_REBUILD ||
					    rbsc->mi2->rigidbody->flag & RBO_FLAG_KINEMATIC_REBUILD) {
						/* World has been rebuilt so rebuild constraint */
						BKE_rigidbody_validate_sim_shard_constraint(rbw, rbsc, true);
						BKE_rigidbody_start_dist_angle(rbsc);
					}

					else if (rbsc->flag & RBC_FLAG_NEEDS_VALIDATE) {
						BKE_rigidbody_validate_sim_shard_constraint(rbw, rbsc, false);
					}

					if (rbsc->physics_constraint && rbw && rbw->rebuild_comp_con) {
						RB_constraint_set_enabled(rbsc->physics_constraint, true);
					}

					rbsc->flag &= ~RBC_FLAG_NEEDS_VALIDATE;
				}
			}
			else {
				/* validate that we've got valid object set up here... */
				RigidBodyOb *rbo = ob->rigidbody_object;
				/* update transformation matrix of the object so we don't get a frame of lag for simple animations */
				BKE_object_where_is_calc(scene, ob);

				if (rbo == NULL) {
					/* Since this object is included in the sim group but doesn't have
					 * rigid body settings (perhaps it was added manually), add!
					 *	- assume object to be active? That is the default for newly added settings...
					 */
					ob->rigidbody_object = BKE_rigidbody_create_object(scene, ob, RBO_TYPE_ACTIVE);
					rigidbody_validate_sim_object(rbw, ob, true);

					rbo = ob->rigidbody_object;
				}
				else {
					/* perform simulation data updates as tagged */
					/* refresh object... */
					if (rebuild) {
						/* World has been rebuilt so rebuild object */
						rigidbody_validate_sim_object(rbw, ob, true);
					}
					else if (rbo->flag & RBO_FLAG_NEEDS_VALIDATE) {
						rigidbody_validate_sim_object(rbw, ob, false);
					}
					/* refresh shape... */
					if (rbo->flag & RBO_FLAG_NEEDS_RESHAPE) {
						/* mesh/shape data changed, so force shape refresh */
						rigidbody_validate_sim_shape(ob, true);
						/* now tell RB sim about it */
						// XXX: we assume that this can only get applied for active/passive shapes that will be included as rigidbodies
						RB_body_set_collision_shape(rbo->physics_object, rbo->physics_shape);
					}
					rbo->flag &= ~(RBO_FLAG_NEEDS_VALIDATE | RBO_FLAG_NEEDS_RESHAPE);
				}

				/* update simulation object... */
				rigidbody_update_sim_ob(scene, rbw, ob, rbo, centroid);
			}
		}
		rbw->refresh_modifiers = false;
	}

	/* update constraints */
	if (rbw->constraints == NULL) /* no constraints, move on */
		return;
	for (go = rbw->constraints->gobject.first; go; go = go->next) {
		Object *ob = go->ob;

		if (ob) {
			/* validate that we've got valid object set up here... */
			RigidBodyCon *rbc = ob->rigidbody_constraint;
			/* update transformation matrix of the object so we don't get a frame of lag for simple animations */
			BKE_object_where_is_calc(scene, ob);

			if (rbc == NULL) {
				/* Since this object is included in the group but doesn't have
				 * constraint settings (perhaps it was added manually), add!
				 */
				ob->rigidbody_constraint = BKE_rigidbody_create_constraint(scene, ob, RBC_TYPE_FIXED);
				rigidbody_validate_sim_constraint(rbw, ob, true);

				rbc = ob->rigidbody_constraint;
			}
			else {
				/* perform simulation data updates as tagged */
				if (rebuild) {
					/* World has been rebuilt so rebuild constraint */
					rigidbody_validate_sim_constraint(rbw, ob, true);
				}
				else if (rbc->flag & RBC_FLAG_NEEDS_VALIDATE) {
					rigidbody_validate_sim_constraint(rbw, ob, false);
				}
				rbc->flag &= ~RBC_FLAG_NEEDS_VALIDATE;
			}
		}
	}
}

static void rigidbody_update_simulation_post_step(RigidBodyWorld *rbw)
{
	GroupObject *go;
	ModifierData *md;
	FractureModifierData *rmd;
	int modFound = false;
	RigidBodyOb *rbo;
	MeshIsland *mi;

	for (go = rbw->group->gobject.first; go; go = go->next) {

		Object *ob = go->ob;
		//handle fractured rigidbodies, maybe test for psys as well ?
		for (md = ob->modifiers.first; md; md = md->next) {
			if (md->type == eModifierType_Fracture) {
				rmd = (FractureModifierData *)md;
				if (isModifierActive(rmd)) {
					for (mi = rmd->meshIslands.first; mi; mi = mi->next) {
						rbo = mi->rigidbody;
						if (!rbo) continue;
						/* reset kinematic state for transformed objects */
						if (ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ) {
							RB_body_set_kinematic_state(rbo->physics_object, rbo->flag & RBO_FLAG_KINEMATIC || rbo->flag & RBO_FLAG_DISABLED);
							RB_body_set_mass(rbo->physics_object, RBO_GET_MASS(rbo));
							/* deactivate passive objects so they don't interfere with deactivation of active objects */
							if (rbo->type == RBO_TYPE_PASSIVE)
								RB_body_deactivate(rbo->physics_object);
						}
					}
					modFound = true;
					break;
				}
			}
		}

		/* handle regular rigidbodies */
		if (ob && !modFound) {
			RigidBodyOb *rbo = ob->rigidbody_object;
			/* reset kinematic state for transformed objects */
			if (rbo && (ob->flag & SELECT) && (G.moving & G_TRANSFORM_OBJ)) {
				RB_body_set_kinematic_state(rbo->physics_object, rbo->flag & RBO_FLAG_KINEMATIC || rbo->flag & RBO_FLAG_DISABLED);
				RB_body_set_mass(rbo->physics_object, RBO_GET_MASS(rbo));
				/* deactivate passive objects so they don't interfere with deactivation of active objects */
				if (rbo->type == RBO_TYPE_PASSIVE)
					RB_body_deactivate(rbo->physics_object);
			}
		}
		modFound = false;
	}
}

bool BKE_rigidbody_check_sim_running(RigidBodyWorld *rbw, float ctime)
{
	return (rbw && (rbw->flag & RBW_FLAG_MUTED) == 0 && ctime > rbw->pointcache->startframe);
}

/* Sync rigid body and object transformations */
void BKE_rigidbody_sync_transforms(RigidBodyWorld *rbw, Object *ob, float ctime)
{
	RigidBodyOb *rbo = NULL;
	FractureModifierData *rmd = NULL;
	MeshIsland *mi;
	ModifierData *md;
	float centr[3], size[3];
	int modFound = false;
	bool exploOK = false;

	if (rbw == NULL)
		return;

	for (md = ob->modifiers.first; md; md = md->next) {
		if (md->type == eModifierType_Fracture) {
			rmd = (FractureModifierData *)md;
			exploOK = !rmd->explo_shared || (rmd->explo_shared && rmd->frac_mesh && rmd->dm);
			
			if (isModifierActive(rmd) && exploOK) {
				modFound = true;
				
				if ((ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ) ||
				    ((ob->rigidbody_object) && (ob->rigidbody_object->flag & RBO_FLAG_KINEMATIC)))
				{
					/* update "original" matrix */
					copy_m4_m4(rmd->origmat, ob->obmat);
					if (ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ && rbw) {
						RigidBodyShardCon *con;
						
						rbw->object_changed = true;
						BKE_rigidbody_cache_reset(rbw);
						/* re-enable all constraints as well */
						for (con = rmd->meshConstraints.first; con; con = con->next) {
							con->flag |= RBC_FLAG_ENABLED;
							con->flag |= RBC_FLAG_NEEDS_VALIDATE;
						}
					}
				}

				if (!is_zero_m4(rmd->origmat) && rbw && !rbw->object_changed) {
					copy_m4_m4(ob->obmat, rmd->origmat);
				}

				for (mi = rmd->meshIslands.first; mi; mi = mi->next) {

					rbo = mi->rigidbody;
					if (!rbo) {
						continue;
					}
					
					/* use rigid body transform after cache start frame if objects is not being transformed */
					if (BKE_rigidbody_check_sim_running(rbw, ctime) && !(ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ)) {

						/* keep original transform when the simulation is muted */
						if (rbw->flag & RBW_FLAG_MUTED)
							return;
					}
					/* otherwise set rigid body transform to current obmat*/
					else {

						mat4_to_loc_quat(rbo->pos, rbo->orn, ob->obmat);
						mat4_to_size(size, ob->obmat);
						copy_v3_v3(centr, mi->centroid);
						mul_v3_v3(centr, size);
						mul_qt_v3(rbo->orn, centr);
						add_v3_v3(rbo->pos, centr);
					}

					//frame = (int)BKE_scene_frame_get(md->scene);
					BKE_rigidbody_update_cell(mi, ob, rbo->pos, rbo->orn, rmd, (int)ctime);
				}
				
				break;
			}
		}

		modFound = false;
	}

	if (!modFound)
	{
		rbo = ob->rigidbody_object;

		/* keep original transform for kinematic and passive objects */
		if (ELEM(NULL, rbw, rbo) || rbo->flag & RBO_FLAG_KINEMATIC || rbo->type == RBO_TYPE_PASSIVE)
			return;

		/* use rigid body transform after cache start frame if objects is not being transformed */
		if (BKE_rigidbody_check_sim_running(rbw, ctime) && !(ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ)) {
			float mat[4][4], size_mat[4][4], size[3];

			normalize_qt(rbo->orn); // RB_TODO investigate why quaternion isn't normalized at this point
			quat_to_mat4(mat, rbo->orn);
			copy_v3_v3(mat[3], rbo->pos);

			/* keep original transform when the simulation is muted */
			if (rbw->flag & RBW_FLAG_MUTED)
				return;

			/*normalize_qt(rbo->orn); // RB_TODO investigate why quaternion isn't normalized at this point
			   quat_to_mat4(mat, rbo->orn);
			   copy_v3_v3(mat[3], rbo->pos);*/

			mat4_to_size(size, ob->obmat);
			size_to_mat4(size_mat, size);
			mul_m4_m4m4(mat, mat, size_mat);

			copy_m4_m4(ob->obmat, mat);
		}
		/* otherwise set rigid body transform to current obmat */
		else {
			if (ob->flag & SELECT && G.moving & G_TRANSFORM_OBJ)
				rbw->object_changed = true;

			mat4_to_loc_quat(rbo->pos, rbo->orn, ob->obmat);
		}
	}
}

/* Used when cancelling transforms - return rigidbody and object to initial states */
void BKE_rigidbody_aftertrans_update(Object *ob, float loc[3], float rot[3], float quat[4], float rotAxis[3], float rotAngle)
{
	RigidBodyOb *rbo;
	ModifierData *md;
	FractureModifierData *rmd;
	
	md = modifiers_findByType(ob, eModifierType_Fracture);
	if (md != NULL)
	{
		MeshIsland *mi;
		rmd = (FractureModifierData *)md;
		copy_m4_m4(rmd->origmat, ob->obmat);
		for (mi = rmd->meshIslands.first; mi; mi = mi->next)
		{
			rbo = mi->rigidbody;
			/* return rigid body and object to their initial states */
			copy_v3_v3(rbo->pos, ob->loc);
			add_v3_v3(rbo->pos, mi->centroid);
			copy_v3_v3(ob->loc, loc);
		
			if (ob->rotmode > 0) {
				eulO_to_quat(rbo->orn, ob->rot, ob->rotmode);
				copy_v3_v3(ob->rot, rot);
			}
			else if (ob->rotmode == ROT_MODE_AXISANGLE) {
				axis_angle_to_quat(rbo->orn, ob->rotAxis, ob->rotAngle);
				copy_v3_v3(ob->rotAxis, rotAxis);
				ob->rotAngle = rotAngle;
			}
			else {
				copy_qt_qt(rbo->orn, ob->quat);
				copy_qt_qt(ob->quat, quat);
			}
			if (rbo->physics_object) {
				/* allow passive objects to return to original transform */
				if (rbo->type == RBO_TYPE_PASSIVE)
					RB_body_set_kinematic_state(rbo->physics_object, true);
				RB_body_set_loc_rot(rbo->physics_object, rbo->pos, rbo->orn);
			}
		}
	}
	else {
		rbo = ob->rigidbody_object;
		/* return rigid body and object to their initial states */
		copy_v3_v3(rbo->pos, ob->loc);
		copy_v3_v3(ob->loc, loc);
	
		if (ob->rotmode > 0) {
			eulO_to_quat(rbo->orn, ob->rot, ob->rotmode);
			copy_v3_v3(ob->rot, rot);
		}
		else if (ob->rotmode == ROT_MODE_AXISANGLE) {
			axis_angle_to_quat(rbo->orn, ob->rotAxis, ob->rotAngle);
			copy_v3_v3(ob->rotAxis, rotAxis);
			ob->rotAngle = rotAngle;
		}
		else {
			copy_qt_qt(rbo->orn, ob->quat);
			copy_qt_qt(ob->quat, quat);
		}
		if (rbo->physics_object) {
			/* allow passive objects to return to original transform */
			if (rbo->type == RBO_TYPE_PASSIVE)
				RB_body_set_kinematic_state(rbo->physics_object, true);
			RB_body_set_loc_rot(rbo->physics_object, rbo->pos, rbo->orn);
		}
		// RB_TODO update rigid body physics object's loc/rot for dynamic objects here as well (needs to be done outside bullet's update loop)
	}
	// RB_TODO update rigid body physics object's loc/rot for dynamic objects here as well (needs to be done outside bullet's update loop)
}

static void restoreKinematic(RigidBodyWorld *rbw)
{
	GroupObject *go;

	/*restore kinematic state of shards if object is kinematic*/
	for (go = rbw->group->gobject.first; go; go = go->next)	{
		if ((go->ob) && (go->ob->rigidbody_object) && (go->ob->rigidbody_object->flag & RBO_FLAG_KINEMATIC))
		{
			FractureModifierData *fmd = (FractureModifierData*)modifiers_findByType(go->ob, eModifierType_Fracture);
			if (fmd)
			{
				MeshIsland* mi;
				for (mi = fmd->meshIslands.first; mi; mi = mi->next)
				{
					if (mi->rigidbody)
					{
						mi->rigidbody->flag |= RBO_FLAG_KINEMATIC;
						mi->rigidbody->flag |= RBO_FLAG_NEEDS_VALIDATE;
					}
				}
			}
		}
	}
}

void BKE_rigidbody_cache_reset(RigidBodyWorld *rbw)
{
	if (rbw)
		rbw->pointcache->flag |= PTCACHE_OUTDATED;

	restoreKinematic(rbw);
}

/* ------------------ */

/* Rebuild rigid body world */
/* NOTE: this needs to be called before frame update to work correctly */
void BKE_rigidbody_rebuild_world(Scene *scene, float ctime)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	PointCache *cache;
	PTCacheID pid;
	int startframe, endframe;
	int shards = 0, objects = 0;

	BKE_ptcache_id_from_rigidbody(&pid, NULL, rbw);
	BKE_ptcache_id_time(&pid, scene, ctime, &startframe, &endframe, NULL);
	cache = rbw->pointcache;

	/* flag cache as outdated if we don't have a world or number of objects in the simulation has changed */
	rigidbody_group_count_items(&rbw->group->gobject, &shards, &objects);
	if (rbw->physics_world == NULL || rbw->numbodies != (shards + objects)) {
		cache->flag |= PTCACHE_OUTDATED;
	}

	if (ctime == startframe + 1 && rbw->ltime == startframe) {
		if (cache->flag & PTCACHE_OUTDATED) {
			BKE_ptcache_id_reset(scene, &pid, PTCACHE_RESET_OUTDATED);
			rigidbody_update_simulation(scene, rbw, true);
			BKE_ptcache_validate(cache, (int)ctime);
			cache->last_exact = 0;
			cache->flag &= ~PTCACHE_REDO_NEEDED;
		}
	}
}

/* Run RigidBody simulation for the specified physics world */
void BKE_rigidbody_do_simulation(Scene *scene, float ctime)
{
	float timestep;
	RigidBodyWorld *rbw = scene->rigidbody_world;
	PointCache *cache;
	PTCacheID pid;
	int startframe, endframe;

	BKE_ptcache_id_from_rigidbody(&pid, NULL, rbw);
	BKE_ptcache_id_time(&pid, scene, ctime, &startframe, &endframe, NULL);
	cache = rbw->pointcache;

	if (ctime <= startframe) {
		/* rebuild constraints */
		rbw->rebuild_comp_con = true;

		rbw->ltime = startframe;
		if ((rbw->object_changed))
		{       /* flag modifier refresh at their next execution XXX TODO -> still used ? */
			rbw->refresh_modifiers = true;
			rbw->object_changed = false;
			rigidbody_update_simulation(scene, rbw, true);
		}
		return;
	}
	/* make sure we don't go out of cache frame range */
	else if (ctime > endframe) {
		ctime = endframe;
	}

	/* don't try to run the simulation if we don't have a world yet but allow reading baked cache */
	if (rbw->physics_world == NULL && !(cache->flag & PTCACHE_BAKED))
		return;
	else if ((rbw->objects == NULL) || (rbw->cache_index_map == NULL))
		rigidbody_update_ob_array(rbw);

	/* try to read from cache */
	// RB_TODO deal with interpolated, old and baked results
	if (BKE_ptcache_read(&pid, ctime)) {
		BKE_ptcache_validate(cache, (int)ctime);

		rbw->ltime = ctime;
		return;
	}
	else if (rbw->ltime == startframe)
	{
		restoreKinematic(rbw);
		rigidbody_update_simulation(scene, rbw, true);
	}

	/* advance simulation, we can only step one frame forward */
	if (ctime == rbw->ltime + 1 && !(cache->flag & PTCACHE_BAKED)) {
		/* write cache for first frame when on second frame */
		if (rbw->ltime == startframe && (cache->flag & PTCACHE_OUTDATED || cache->last_exact == 0)) {
			BKE_ptcache_write(&pid, startframe);
		}

		if (rbw->ltime > startframe) {
			rbw->rebuild_comp_con = false;
		}

		/* update and validate simulation */
		rigidbody_update_simulation(scene, rbw, false);

		/* calculate how much time elapsed since last step in seconds */
		timestep = 1.0f / (float)FPS * (ctime - rbw->ltime) * rbw->time_scale;
		/* step simulation by the requested timestep, steps per second are adjusted to take time scale into account */
		RB_dworld_step_simulation(rbw->physics_world, timestep, INT_MAX, 1.0f / (float)rbw->steps_per_second * min_ff(rbw->time_scale, 1.0f));

		rigidbody_update_simulation_post_step(rbw);

		/* write cache for current frame */
		BKE_ptcache_validate(cache, (int)ctime);
		BKE_ptcache_write(&pid, (unsigned int)ctime);

		rbw->ltime = ctime;
	}
}
/* ************************************** */

#else  /* WITH_BULLET */

/* stubs */
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

void BKE_rigidbody_free_world(RigidBodyWorld *rbw) {}
void BKE_rigidbody_free_object(Object *ob) {}
void BKE_rigidbody_free_constraint(Object *ob) {}
struct RigidBodyOb *BKE_rigidbody_copy_object(Object *ob) { return NULL; }
struct RigidBodyCon *BKE_rigidbody_copy_constraint(Object *ob) { return NULL; }
void BKE_rigidbody_relink_constraint(RigidBodyCon *rbc) {}
void BKE_rigidbody_validate_sim_world(Scene *scene, RigidBodyWorld *rbw, bool rebuild) {}
void BKE_rigidbody_calc_volume(Object *ob, float *r_vol) { if (r_vol) *r_vol = 0.0f; }
void BKE_rigidbody_calc_center_of_mass(Object *ob, float r_com[3]) { zero_v3(r_com); }
struct RigidBodyWorld *BKE_rigidbody_create_world(Scene *scene) { return NULL; }
struct RigidBodyWorld *BKE_rigidbody_world_copy(RigidBodyWorld *rbw) { return NULL; }
void BKE_rigidbody_world_groups_relink(struct RigidBodyWorld *rbw) {}
struct RigidBodyOb *BKE_rigidbody_create_object(Scene *scene, Object *ob, short type) { return NULL; }
struct RigidBodyCon *BKE_rigidbody_create_constraint(Scene *scene, Object *ob, short type) { return NULL; }
struct RigidBodyWorld *BKE_rigidbody_get_world(Scene *scene) { return NULL; }
void BKE_rigidbody_remove_object(Scene *scene, Object *ob) {}
void BKE_rigidbody_remove_constraint(Scene *scene, Object *ob) {}
void BKE_rigidbody_sync_transforms(RigidBodyWorld *rbw, Object *ob, float ctime) {}
void BKE_rigidbody_aftertrans_update(Object *ob, float loc[3], float rot[3], float quat[4], float rotAxis[3], float rotAngle) {}
bool BKE_rigidbody_check_sim_running(RigidBodyWorld *rbw, float ctime) { return false; }
void BKE_rigidbody_cache_reset(RigidBodyWorld *rbw) {}
void BKE_rigidbody_rebuild_world(Scene *scene, float ctime) {}
void BKE_rigidbody_do_simulation(Scene *scene, float ctime) {}

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#endif  /* WITH_BULLET */
