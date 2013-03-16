/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// world.c -- world query functions

#include "bspfile.h"
#include "console.h"
#include "mathlib.h"
#include "model.h"
#include "progs.h"
#include "server.h"
#include "world.h"

#ifdef NQ_HACK
#include "host.h"
#include "quakedef.h"
#include "sys.h"
/* FIXME - quick hack to enable merging of NQ/QWSV shared code */
#define SV_Error Sys_Error
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
#include "qwsvdef.h"
#include "pmove.h"
#endif

/*

entities never clip against themselves, or their owner

line of sight checks trace->crosscontent, but bullets don't

*/

typedef struct {
    vec3_t mins;
    vec3_t maxs;
} bounds_t;

typedef struct {
    bounds_t move;		/* enclose the test object along entire move */
    bounds_t object;		/* size of the moving object */
    bounds_t monster;		/* size when clipping against monsters */
    vec3_t start, end;
    trace_t *trace;
    movetype_t type;
    const edict_t *passedict;
} moveclip_t;

int SV_HullPointContents(const hull_t *hull, int num, const vec3_t point);

/*
===============================================================================

HULL BOXES

===============================================================================
*/

static hull_t box_hull;
static mclipnode_t box_clipnodes[6];
static mplane_t box_planes[6];

/*
===================
SV_InitBoxHull

Set up the planes and clipnodes so that the six floats of a bounding box
can just be stored out and get a proper hull_t structure.
===================
*/
static void
SV_InitBoxHull(void)
{
    int i;
    int side;

    box_hull.clipnodes = box_clipnodes;
    box_hull.planes = box_planes;
    box_hull.firstclipnode = 0;
    box_hull.lastclipnode = 5;

    for (i = 0; i < 6; i++) {
	box_clipnodes[i].planenum = i;

	side = i & 1;

	box_clipnodes[i].children[side] = CONTENTS_EMPTY;
	if (i != 5)
	    box_clipnodes[i].children[side ^ 1] = i + 1;
	else
	    box_clipnodes[i].children[side ^ 1] = CONTENTS_SOLID;

	box_planes[i].type = i >> 1;
	box_planes[i].normal[i >> 1] = 1;
    }
}


/*
===================
SV_HullForBox

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
===================
*/
static const hull_t *
SV_HullForBox(vec3_t mins, vec3_t maxs)
{
    box_planes[0].dist = maxs[0];
    box_planes[1].dist = mins[0];
    box_planes[2].dist = maxs[1];
    box_planes[3].dist = mins[1];
    box_planes[4].dist = maxs[2];
    box_planes[5].dist = mins[2];

    return &box_hull;
}



/*
================
SV_HullForEntity

Returns a hull that can be used for testing or clipping an object of mins/maxs
size.
Offset is filled in to contain the adjustment that must be added to the
testing object's origin to get a point to use with the returned hull.
================
*/
static const hull_t *
SV_HullForEntity(const edict_t *ent, const vec3_t mins, const vec3_t maxs,
		 vec3_t offset)
{
    const model_t *model;
    const hull_t *hull;
    vec3_t hullmins, hullmaxs, size;

// decide which clipping hull to use, based on the size
    if (ent->v.solid == SOLID_BSP) {	// explicit hulls in the BSP model
	if (ent->v.movetype != MOVETYPE_PUSH)
	    SV_Error("SOLID_BSP without MOVETYPE_PUSH");

	model = sv.models[(int)ent->v.modelindex];

	if (!model || model->type != mod_brush)
	    SV_Error("MOVETYPE_PUSH with a non bsp model");

	VectorSubtract(maxs, mins, size);
	if (size[0] < 3)
	    hull = &model->hulls[0];
	else if (size[0] <= 32)
	    hull = &model->hulls[1];
	else
	    hull = &model->hulls[2];

// calculate an offset value to center the origin
	VectorSubtract(hull->clip_mins, mins, offset);
	VectorAdd(offset, ent->v.origin, offset);
    } else {
	/* create a temp hull from bounding box sizes */
	VectorSubtract(ent->v.mins, maxs, hullmins);
	VectorSubtract(ent->v.maxs, mins, hullmaxs);
	hull = SV_HullForBox(hullmins, hullmaxs);

	VectorCopy(ent->v.origin, offset);
    }

    return hull;
}

/*
===============================================================================

ENTITY AREA CHECKING

===============================================================================
*/

typedef struct areanode_s {
    int axis;			// -1 = leaf node
    float dist;
    struct areanode_s *children[2];
    link_t trigger_edicts;
    link_t solid_edicts;
} areanode_t;

#define	AREA_DEPTH	4
#define	AREA_NODES	32

static areanode_t sv_areanodes[AREA_NODES];
static int sv_numareanodes;

#if defined(QW_HACK) && defined(SERVERONLY)
/*
====================
AddLinksToPmove

====================
*/
static void
SV_AddLinksToPmove_r(const areanode_t *node, const vec3_t mins,
		     const vec3_t maxs)
{
    const link_t *l, *next;
    const edict_t *check;
    int pl;
    int i;
    physent_t *pe;

    pl = EDICT_TO_PROG(sv_player);

    // touch linked edicts
    for (l = node->solid_edicts.next; l != &node->solid_edicts; l = next) {
	next = l->next;
	check = EDICT_FROM_AREA(l);

	if (check->v.owner == pl)
	    continue;		// player's own missile
	if (check->v.solid == SOLID_BSP
	    || check->v.solid == SOLID_BBOX
	    || check->v.solid == SOLID_SLIDEBOX) {
	    if (check == sv_player)
		continue;

	    for (i = 0; i < 3; i++)
		if (check->v.absmin[i] > maxs[i]
		    || check->v.absmax[i] < mins[i])
		    break;
	    if (i != 3)
		continue;
	    if (pmove.numphysent == MAX_PHYSENTS)
		return;
	    pe = &pmove.physents[pmove.numphysent];
	    pmove.numphysent++;

	    VectorCopy(check->v.origin, pe->origin);
	    pe->info = NUM_FOR_EDICT(check);
	    if (check->v.solid == SOLID_BSP)
		pe->model = sv.models[(int)(check->v.modelindex)];
	    else {
		pe->model = NULL;
		VectorCopy(check->v.mins, pe->mins);
		VectorCopy(check->v.maxs, pe->maxs);
	    }
	}
    }

// recurse down both sides
    if (node->axis == -1)
	return;

    if (maxs[node->axis] > node->dist)
	SV_AddLinksToPmove_r(node->children[0], mins, maxs);
    if (mins[node->axis] < node->dist)
	SV_AddLinksToPmove_r(node->children[1], mins, maxs);
}

void
SV_AddLinksToPmove(const vec3_t mins, const vec3_t maxs)
{
    SV_AddLinksToPmove_r(sv_areanodes, mins, maxs);
}
#endif

/*
===============
SV_CreateAreaNode

===============
*/
static areanode_t *
SV_CreateAreaNode(int depth, const vec3_t mins, const vec3_t maxs)
{
    areanode_t *anode;
    vec3_t size;
    vec3_t mins1, maxs1, mins2, maxs2;

    if (sv_numareanodes == AREA_NODES)
	SV_Error("%s: sv_numareanodes == AREA_NODES", __func__);

    anode = &sv_areanodes[sv_numareanodes];
    sv_numareanodes++;

    ClearLink(&anode->trigger_edicts);
    ClearLink(&anode->solid_edicts);

    if (depth == AREA_DEPTH) {
	anode->axis = -1;
	anode->children[0] = anode->children[1] = NULL;
	return anode;
    }

    VectorSubtract(maxs, mins, size);
    if (size[0] > size[1])
	anode->axis = 0;
    else
	anode->axis = 1;

    anode->dist = 0.5 * (maxs[anode->axis] + mins[anode->axis]);
    VectorCopy(mins, mins1);
    VectorCopy(mins, mins2);
    VectorCopy(maxs, maxs1);
    VectorCopy(maxs, maxs2);

    maxs1[anode->axis] = mins2[anode->axis] = anode->dist;

    anode->children[0] = SV_CreateAreaNode(depth + 1, mins2, maxs2);
    anode->children[1] = SV_CreateAreaNode(depth + 1, mins1, maxs1);

    return anode;
}

/*
===============
SV_ClearWorld

===============
*/
void
SV_ClearWorld(void)
{
    SV_InitBoxHull();

    memset(sv_areanodes, 0, sizeof(sv_areanodes));
    sv_numareanodes = 0;
    SV_CreateAreaNode(0, sv.worldmodel->mins, sv.worldmodel->maxs);
}


/*
===============
SV_UnlinkEdict

===============
*/
void
SV_UnlinkEdict(edict_t *ent)
{
    if (!ent->area.prev)
	return;			// not linked in anywhere
    RemoveLink(&ent->area);
    ent->area.prev = ent->area.next = NULL;
}


/*
====================
SV_TouchLinks
====================
*/
static void
SV_TouchLinks(edict_t *ent, const areanode_t *node)
{
    link_t *l, *next;
    edict_t *touch;
    int old_self, old_other;

    /* touch linked edicts */
    for (l = node->trigger_edicts.next; l != &node->trigger_edicts; l = next) {
	/*
	 * FIXME - Just paranoia? Check if this can really happen...
	 *         (I think it was related to the E2M2 drawbridge bug)
	 */
	if (!l || !l->next)
#ifdef NQ_HACK
	    Host_Error("%s: encountered NULL link", __func__);
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
	    SV_Error("%s: encountered NULL link", __func__);
#endif

	next = l->next;
	touch = EDICT_FROM_AREA(l);
	if (touch == ent)
	    continue;
	if (!touch->v.touch || touch->v.solid != SOLID_TRIGGER)
	    continue;
	if (ent->v.absmin[0] > touch->v.absmax[0]
	    || ent->v.absmin[1] > touch->v.absmax[1]
	    || ent->v.absmin[2] > touch->v.absmax[2]
	    || ent->v.absmax[0] < touch->v.absmin[0]
	    || ent->v.absmax[1] < touch->v.absmin[1]
	    || ent->v.absmax[2] < touch->v.absmin[2])
	    continue;

	old_self = pr_global_struct->self;
	old_other = pr_global_struct->other;

	pr_global_struct->self = EDICT_TO_PROG(touch);
	pr_global_struct->other = EDICT_TO_PROG(ent);
	pr_global_struct->time = sv.time;
	PR_ExecuteProgram(touch->v.touch);

	/* the PR_ExecuteProgram above can alter the linked edicts */
	/* FIXME:
	 * - what if (touch->free == true && l->next == NULL) ? (etc.)
	 */
	if (next != l->next && l->next) {
	    Con_DPrintf("Warning: fixed up link in %s\n", __func__);
	    next = l->next;
	}
	pr_global_struct->self = old_self;
	pr_global_struct->other = old_other;
    }

    /* recurse down both sides */
    if (node->axis == -1)
	return;

    if (ent->v.absmax[node->axis] > node->dist)
	SV_TouchLinks(ent, node->children[0]);
    if (ent->v.absmin[node->axis] < node->dist)
	SV_TouchLinks(ent, node->children[1]);
}


/*
===============
SV_FindTouchedLeafs

===============
*/
static void
SV_FindTouchedLeafs(edict_t *ent, const mnode_t *node)
{
    const mplane_t *splitplane;
    const mleaf_t *leaf;
    int sides;
    int leafnum;

    if (node->contents == CONTENTS_SOLID)
	return;

// add an efrag if the node is a leaf

    if (node->contents < 0) {
	if (ent->num_leafs == MAX_ENT_LEAFS)
	    return;

	leaf = (mleaf_t *)node;
	leafnum = leaf - sv.worldmodel->leafs - 1;

	ent->leafnums[ent->num_leafs] = leafnum;
	ent->num_leafs++;
	return;
    }
// NODE_MIXED

    splitplane = node->plane;
    sides = BOX_ON_PLANE_SIDE(ent->v.absmin, ent->v.absmax, splitplane);

// recurse down the contacted sides
    if (sides & PSIDE_FRONT)
	SV_FindTouchedLeafs(ent, node->children[0]);

    if (sides & PSIDE_BACK)
	SV_FindTouchedLeafs(ent, node->children[1]);
}

/*
===============
SV_LinkEdict

===============
*/
void
SV_LinkEdict(edict_t *ent, qboolean touch_triggers)
{
    areanode_t *node;

    if (ent->area.prev)
	SV_UnlinkEdict(ent);	// unlink from old position

    if (ent == sv.edicts)
	return;			// don't add the world

    if (ent->free)
	return;

    /* set the abs box */
    VectorAdd(ent->v.origin, ent->v.mins, ent->v.absmin);
    VectorAdd(ent->v.origin, ent->v.maxs, ent->v.absmax);

    /*
     * To make items easier to pick up and allow them to be grabbed off of
     * shelves, the abs sizes are expanded
     */
    if ((int)ent->v.flags & FL_ITEM) {
	ent->v.absmin[0] -= 15;
	ent->v.absmin[1] -= 15;
	ent->v.absmax[0] += 15;
	ent->v.absmax[1] += 15;
    } else {
	/*
	 * because movement is clipped an epsilon away from an actual edge, we
	 * must fully check even when bounding boxes don't quite touch
	 */
	ent->v.absmin[0] -= 1;
	ent->v.absmin[1] -= 1;
	ent->v.absmin[2] -= 1;
	ent->v.absmax[0] += 1;
	ent->v.absmax[1] += 1;
	ent->v.absmax[2] += 1;
    }

    /* link to PVS leafs */
    ent->num_leafs = 0;
    if (ent->v.modelindex)
	SV_FindTouchedLeafs(ent, sv.worldmodel->nodes);

    if (ent->v.solid == SOLID_NOT)
	return;

    /* find the first node that the ent's box crosses */
    node = sv_areanodes;
    while (1) {
	if (node->axis == -1)
	    break;
	if (ent->v.absmin[node->axis] > node->dist)
	    node = node->children[0];
	else if (ent->v.absmax[node->axis] < node->dist)
	    node = node->children[1];
	else
	    break;		// crosses the node
    }

    /* link it in */
    if (ent->v.solid == SOLID_TRIGGER)
	InsertLinkBefore(&ent->area, &node->trigger_edicts);
    else
	InsertLinkBefore(&ent->area, &node->solid_edicts);

    if (touch_triggers)
	/* touch all entities at this node and decend for more */
	SV_TouchLinks(ent, sv_areanodes);
}



/*
===============================================================================

				POINT TESTING IN HULLS

===============================================================================
*/

#ifndef USE_X86_ASM

/*
==================
SV_HullPointContents

==================
*/
int
SV_HullPointContents(const hull_t *hull, int num, const vec3_t point)
{
    float dist;
    const mclipnode_t *node;
    const mplane_t *plane;

    while (num >= 0) {
	if (num < hull->firstclipnode || num > hull->lastclipnode)
	    SV_Error("%s: bad node number (%i)", __func__, num);

	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	    dist = point[plane->type] - plane->dist;
	else
	    dist = DotProduct(plane->normal, point) - plane->dist;
	if (dist < 0)
	    num = node->children[1];
	else
	    num = node->children[0];
    }

    return num;
}

#endif /* USE_X86_ASM */


/*
==================
SV_PointContents

==================
*/
int
SV_PointContents(const vec3_t point)
{
#ifdef NQ_HACK
    int contents;

    contents = SV_HullPointContents(&sv.worldmodel->hulls[0], 0, point);
    if (contents <= CONTENTS_CURRENT_0 && contents >= CONTENTS_CURRENT_DOWN)
	contents = CONTENTS_WATER;

    return contents;
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
    return SV_HullPointContents(&sv.worldmodel->hulls[0], 0, point);
#endif
}

//===========================================================================

/*
============
SV_TestEntityPosition

A small wrapper around SV_BoxInSolidEntity that never clips against the
supplied entity.  TODO: This could be a lot more efficient?
============
*/
edict_t *
SV_TestEntityPosition(const edict_t *ent)
{
    const entvars_t *v = &ent->v;
    edict_t *ret = NULL;
    trace_t trace;

    SV_Move(v->origin, v->mins, v->maxs, v->origin, MOVE_NORMAL, ent, &trace);
    if (trace.startsolid)
	ret = sv.edicts;

    return ret;
}

/*
===============================================================================

LINE TESTING IN HULLS

===============================================================================
*/

// 1/32 epsilon to keep floating point happy
#define	DIST_EPSILON	(0.03125)

/*
==================
SV_RecursiveHullCheck

==================
*/
qboolean
SV_RecursiveHullCheck(const hull_t *hull, int num,
		      const float p1f, const float p2f,
		      const vec3_t p1, const vec3_t p2, trace_t *trace)
{
    mclipnode_t *node;
    mplane_t *plane;
    float t1, t2;
    float frac;
    int i, child;
    vec3_t mid;
    int side;
    float midf;

// check for empty
    if (num < 0) {
	if (num != CONTENTS_SOLID) {
	    trace->allsolid = false;
	    if (num == CONTENTS_EMPTY)
		trace->inopen = true;
	    else
		trace->inwater = true;
	} else
	    trace->startsolid = true;
	return true;		// empty
    }

    if (num < hull->firstclipnode || num > hull->lastclipnode)
	SV_Error("%s: bad node number", __func__);

//
// find the point distances
//
    node = hull->clipnodes + num;
    plane = hull->planes + node->planenum;

    if (plane->type < 3) {
	t1 = p1[plane->type] - plane->dist;
	t2 = p2[plane->type] - plane->dist;
    } else {
	t1 = DotProduct(plane->normal, p1) - plane->dist;
	t2 = DotProduct(plane->normal, p2) - plane->dist;
    }

#if 1
    if (t1 >= 0 && t2 >= 0) {
	child = node->children[0];
	return SV_RecursiveHullCheck(hull, child, p1f, p2f, p1, p2, trace);
    }
    if (t1 < 0 && t2 < 0) {
	child = node->children[1];
	return SV_RecursiveHullCheck(hull, child, p1f, p2f, p1, p2, trace);
    }
#else
    if ((t1 >= DIST_EPSILON && t2 >= DIST_EPSILON) || (t2 > t1 && t1 >= 0)) {
	child = node->children[0];
	return SV_RecursiveHullCheck(hull, child, p1f, p2f, p1, p2, trace);
    }
    if ((t1 <= -DIST_EPSILON && t2 <= -DIST_EPSILON) || (t2 < t1 && t1 <= 0)) {
	child = node->children[1];
	return SV_RecursiveHullCheck(hull, child, p1f, p2f, p1, p2, trace);
    }
#endif

// put the crosspoint DIST_EPSILON pixels on the near side
    if (t1 < 0)
	frac = (t1 + DIST_EPSILON) / (t1 - t2);
    else
	frac = (t1 - DIST_EPSILON) / (t1 - t2);
    if (frac < 0)
	frac = 0;
    if (frac > 1)
	frac = 1;

    midf = p1f + (p2f - p1f) * frac;
    for (i = 0; i < 3; i++)
	mid[i] = p1[i] + frac * (p2[i] - p1[i]);

    side = (t1 < 0);

// move up to the node
    child = node->children[side];
    if (!SV_RecursiveHullCheck(hull, child, p1f, midf, p1, mid, trace))
	return false;

#ifdef PARANOID
    if (SV_HullPointContents(sv_hullmodel, mid, child) == CONTENTS_SOLID) {
	Con_Printf("mid PointInHullSolid\n");
	return false;
    }
#endif

    child = node->children[side ^ 1];
    if (SV_HullPointContents(hull, child, mid) != CONTENTS_SOLID)
	/* go past the node */
	return SV_RecursiveHullCheck(hull, child, midf, p2f, mid, p2, trace);

    if (trace->allsolid)
	return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
    if (!side) {
	VectorCopy(plane->normal, trace->plane.normal);
	trace->plane.dist = plane->dist;
    } else {
	VectorSubtract(vec3_origin, plane->normal, trace->plane.normal);
	trace->plane.dist = -plane->dist;
    }

    /* shouldn't really happen, but does occasionally */
    while (SV_HullPointContents(hull, hull->firstclipnode, mid) == CONTENTS_SOLID) {
	frac -= 0.1;
	if (frac < 0) {
	    trace->fraction = midf;
	    VectorCopy(mid, trace->endpos);
	    Con_DPrintf("backup past 0\n");
	    return false;
	}
	midf = p1f + (p2f - p1f) * frac;
	for (i = 0; i < 3; i++)
	    mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    }

    trace->fraction = midf;
    VectorCopy(mid, trace->endpos);

    return false;
}


/*
==================
SV_ClipMoveToEntity

Handles selection or creation of a clipping hull, and offseting (and
eventually rotation) of the end points
==================
*/
static void
SV_ClipMoveToEntity(const edict_t *ent, const vec3_t start, const vec3_t mins,
		    const vec3_t maxs, const vec3_t end, trace_t *trace)
{
    const hull_t *hull;
    vec3_t offset;
    vec3_t start_l, end_l;

    /* fill in a default trace */
    memset(trace, 0, sizeof(trace_t));
    trace->fraction = 1;
    trace->allsolid = true;
    VectorCopy(end, trace->endpos);

    /* get the clipping hull */
    hull = SV_HullForEntity(ent, mins, maxs, offset);

    VectorSubtract(start, offset, start_l);
    VectorSubtract(end, offset, end_l);

    /* trace a line through the apropriate clipping hull */
    SV_RecursiveHullCheck(hull, hull->firstclipnode, 0, 1, start_l, end_l,
			  trace);

    /* fix trace up by the offset */
    if (trace->fraction != 1)
	VectorAdd(trace->endpos, offset, trace->endpos);

    /* did we clip the move? */
    if (trace->fraction < 1 || trace->startsolid)
	trace->ent = ent;
}

//===========================================================================

/*
====================
SV_ClipToLinks

Mins and maxs enclose the entire area swept by the move
====================
*/
static void
SV_ClipToLinks(const areanode_t *node, moveclip_t *clip)
{
    link_t *l, *next;
    edict_t *touch;
    trace_t trace;

// touch linked edicts
    for (l = node->solid_edicts.next; l != &node->solid_edicts; l = next) {
	next = l->next;
	touch = EDICT_FROM_AREA(l);
	if (touch->v.solid == SOLID_NOT)
	    continue;
	if (touch == clip->passedict)
	    continue;
	if (touch->v.solid == SOLID_TRIGGER)
	    SV_Error("Trigger in clipping list");

	if (clip->type == MOVE_NOMONSTERS && touch->v.solid != SOLID_BSP)
	    continue;

	if (clip->move.mins[0] > touch->v.absmax[0]
	    || clip->move.mins[1] > touch->v.absmax[1]
	    || clip->move.mins[2] > touch->v.absmax[2]
	    || clip->move.maxs[0] < touch->v.absmin[0]
	    || clip->move.maxs[1] < touch->v.absmin[1]
	    || clip->move.maxs[2] < touch->v.absmin[2])
	    continue;

	if (clip->passedict && clip->passedict->v.size[0]
	    && !touch->v.size[0])
	    continue;		// points never interact

	// might intersect, so do an exact clip
	if (clip->trace->allsolid)
	    return;
	if (clip->passedict) {
	    if (PROG_TO_EDICT(touch->v.owner) == clip->passedict)
		continue;	// don't clip against own missiles
	    if (PROG_TO_EDICT(clip->passedict->v.owner) == touch)
		continue;	// don't clip against owner
	}

	if ((int)touch->v.flags & FL_MONSTER)
	    SV_ClipMoveToEntity(touch, clip->start, clip->monster.mins,
				clip->monster.maxs, clip->end, &trace);
	else
	    SV_ClipMoveToEntity(touch, clip->start, clip->object.mins,
				clip->object.maxs, clip->end, &trace);
	if (trace.allsolid || trace.startsolid
	    || trace.fraction < clip->trace->fraction) {
	    trace.ent = touch;
	    if (clip->trace->startsolid) {
		*clip->trace = trace;
		clip->trace->startsolid = true;
	    } else
		*clip->trace = trace;
	} else if (trace.startsolid)
	    clip->trace->startsolid = true;
    }

// recurse down both sides
    if (node->axis == -1)
	return;

    if (clip->move.maxs[node->axis] > node->dist)
	SV_ClipToLinks(node->children[0], clip);
    if (clip->move.mins[node->axis] < node->dist)
	SV_ClipToLinks(node->children[1], clip);
}


/*
==================
SV_MoveBounds
==================
*/
static void
SV_MoveBounds(const bounds_t *object, const vec3_t start, const vec3_t end,
	      bounds_t *move)
{
    int i;

    for (i = 0; i < 3; i++) {
	if (end[i] > start[i]) {
	    move->mins[i] = start[i] + object->mins[i] - 1;
	    move->maxs[i] = end[i] + object->maxs[i] + 1;
	} else {
	    move->mins[i] = end[i] + object->mins[i] - 1;
	    move->maxs[i] = start[i] + object->maxs[i] + 1;
	}
    }
}

/*
==================
SV_Move
==================
*/
void
SV_Move(const vec3_t start, const vec3_t mins, const vec3_t maxs,
	const vec3_t end, movetype_t type, const edict_t *passedict,
	trace_t *trace)
{
    moveclip_t clip;
    int i;

    memset(&clip, 0, sizeof(moveclip_t));

    clip.trace = trace;
    VectorCopy(start, clip.start);
    VectorCopy(end, clip.end);
    VectorCopy(mins, clip.object.mins);
    VectorCopy(maxs, clip.object.maxs);
    clip.type = type;
    clip.passedict = passedict;

    /* clip to world */
    SV_ClipMoveToEntity(sv.edicts, start, mins, maxs, end, clip.trace);

    if (type == MOVE_MISSILE) {
	for (i = 0; i < 3; i++) {
	    clip.monster.mins[i] = -15;
	    clip.monster.maxs[i] = 15;
	}
    } else {
	VectorCopy(mins, clip.monster.mins);
	VectorCopy(maxs, clip.monster.maxs);
    }

    /* create the bounding box of the entire move */
    SV_MoveBounds(&clip.monster, start, end, &clip.move);

    /* clip to entities */
    SV_ClipToLinks(sv_areanodes, &clip);
}
