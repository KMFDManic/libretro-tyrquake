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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include <float.h>
#include <stdint.h>

#include "cmd.h"
#include "common.h"
#include "console.h"
#include "model.h"

#ifdef GLQUAKE
#include "glquake.h"
#endif

#ifdef SERVERONLY
#include "qwsvdef.h"
/* A dummy texture to point to. FIXME - should server care about textures? */
static texture_t r_notexture_mip_qwsv;
#else
#include "quakedef.h"
#include "render.h"
#include "sys.h"
#ifdef QW_HACK
#include "crc.h"
#endif
/* FIXME - quick hack to enable merging of NQ/QWSV shared code */
#define SV_Error Sys_Error
#endif

static char loadname[MAX_QPATH];	/* for hunk tags */

static void Mod_LoadBrushModel(model_t *model, void *buffer, size_t size);
static model_t *Mod_LoadModel(model_t *model, qboolean crash);

#define MAX_MOD_KNOWN 512
static model_t mod_known[MAX_MOD_KNOWN];
static int mod_numknown;

#ifdef GLQUAKE
cvar_t gl_subdivide_size = { "gl_subdivide_size", "128", true };
#ifdef QW_HACK
byte player_8bit_texels[320 * 200];
#endif
#endif

static const model_loader_t *mod_loader;

static void PVSCache_f(void);
/*
===============
Mod_Init
===============
*/
void
Mod_Init(const model_loader_t *loader)
{
#ifdef GLQUAKE
    Cvar_RegisterVariable(&gl_subdivide_size);
#endif
    Cmd_AddCommand("pvscache", PVSCache_f);
    mod_loader = loader;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *
Mod_PointInLeaf(const model_t *model, const vec3_t point)
{
    mplane_t *plane;
    mnode_t *node;
    float dist;

    if (!model || !model->nodes)
	SV_Error("%s: bad model", __func__);

    node = model->nodes;
    while (1) {
	if (node->contents < 0)
	    return (mleaf_t *)node;
	plane = node->plane;
	dist = DotProduct(point, plane->normal) - plane->dist;
	if (dist > 0)
	    node = node->children[0];
	else
	    node = node->children[1];
    }

    return NULL;		// never reached
}

void
Mod_AddLeafBits(leafbits_t *dst, const leafbits_t *src)
{
    int i, leafblocks;
    const leafblock_t *srcblock;
    leafblock_t *dstblock;

    if (src->numleafs != dst->numleafs)
	SV_Error("%s: src->numleafs (%d) != dst->numleafs (%d)",
		 __func__, src->numleafs, dst->numleafs);

    srcblock = src->bits;
    dstblock = dst->bits;
    leafblocks = (src->numleafs + LEAFMASK) >> LEAFSHIFT;
    for (i = 0; i < leafblocks; i++)
	*dstblock++ |= *srcblock++;
}

#ifdef SERVERONLY
int
Mod_CountLeafBits(const leafbits_t *leafbits)
{
    int i, leafblocks, count;
    leafblock_t block;

    count = 0;
    leafblocks = (leafbits->numleafs + LEAFMASK) >> LEAFSHIFT;
    for (i = 0; i < leafblocks; i++) {
	block = leafbits->bits[i];
	while (block) {
	    count++;
	    block &= (block - 1); /* remove least significant bit */
	}
    }

    return count;
};
#endif

/*
 * Simple LRU cache for decompressed vis data
 */
typedef struct {
    const model_t *model;
    const mleaf_t *leaf;
    leafbits_t *leafbits;
} pvscache_t;
static pvscache_t pvscache[2];
static leafbits_t *fatpvs;
static int pvscache_numleafs;
static int pvscache_bytes;
static int pvscache_blocks;

static int c_cachehit, c_cachemiss;

#define PVSCACHE_SIZE ARRAY_SIZE(pvscache)

static void
Mod_InitPVSCache(int numleafs)
{
    int i;
    int memsize;
    byte *leafmem;

    pvscache_numleafs = numleafs;
    pvscache_bytes = ((numleafs + LEAFMASK) & ~LEAFMASK) >> 3;
    pvscache_blocks = pvscache_bytes / sizeof(leafblock_t);
    memsize = Mod_LeafbitsSize(numleafs);
    fatpvs = Hunk_AllocName(memsize, "fatpvs");

    memset(pvscache, 0, sizeof(pvscache));
    leafmem = Hunk_AllocName(PVSCACHE_SIZE * memsize, "pvscache");
    for (i = 0; i < PVSCACHE_SIZE; i++)
	pvscache[i].leafbits = (leafbits_t *)(leafmem + i * memsize);
}

/*
===================
Mod_DecompressVis
===================
*/

static void
Mod_DecompressVis(const byte *in, const model_t *model, leafbits_t *dest)
{
    leafblock_t *out;
    int num_out;
    int shift;
    int count;

    dest->numleafs = model->numleafs;
    out = dest->bits;

    if (!in) {
	/* no vis info, so make all visible */
	memset(out, 0xff, pvscache_bytes);
	return;
    }

    memset(out, 0, pvscache_bytes);
    num_out = 0;
    shift = 0;
    do {
	if (*in) {
	    *out |= (leafblock_t)*in++ << shift;
	    shift += 8;
	    num_out += 8;
	    if (shift == (1 << LEAFSHIFT)) {
		shift = 0;
		out++;
	    }
	    continue;
	}

	/* Run of zeros - skip over */
	count = in[1];
	in += 2;
	out += count / sizeof(leafblock_t);
	shift += (count % sizeof(leafblock_t)) << 3;
	num_out += count << 3;
	if (shift >= (1 << LEAFSHIFT)) {
	    shift -= (1 << LEAFSHIFT);
	    out++;
	}
    } while (num_out < dest->numleafs);
}

const leafbits_t *
Mod_LeafPVS(const model_t *model, const mleaf_t *leaf)
{
    int slot;
    pvscache_t tmp;

    for (slot = 0; slot < PVSCACHE_SIZE; slot++)
	if (pvscache[slot].model == model && pvscache[slot].leaf == leaf) {
	    c_cachehit++;
	    break;
	}

    if (slot) {
	if (slot == PVSCACHE_SIZE) {
	    slot--;
	    tmp.model = model;
	    tmp.leaf = leaf;
	    tmp.leafbits = pvscache[slot].leafbits;
	    if (leaf == model->leafs) {
		/* return set with everything visible */
		tmp.leafbits->numleafs = model->numleafs;
		memset(tmp.leafbits->bits, 0xff, pvscache_bytes);
	    } else {
		Mod_DecompressVis(leaf->compressed_vis, model, tmp.leafbits);
	    }
	    c_cachemiss++;
	} else {
	    tmp = pvscache[slot];
	}
	memmove(pvscache + 1, pvscache, slot * sizeof(pvscache_t));
	pvscache[0] = tmp;
    }

    return pvscache[0].leafbits;
}

static void
PVSCache_f(void)
{
    Con_Printf("PVSCache: %7d hits %7d misses\n", c_cachehit, c_cachemiss);
}

static void
Mod_AddToFatPVS(const model_t *model, const vec3_t point, const mnode_t *node)
{
    const leafbits_t *pvs;
    mplane_t *plane;
    float d;

    while (1) {
	// if this is a leaf, accumulate the pvs bits
	if (node->contents < 0) {
	    if (node->contents != CONTENTS_SOLID) {
		pvs = Mod_LeafPVS(model, (const mleaf_t *)node);
		Mod_AddLeafBits(fatpvs, pvs);
	    }
	    return;
	}

	plane = node->plane;
	d = DotProduct(point, plane->normal) - plane->dist;
	if (d > 8)
	    node = node->children[0];
	else if (d < -8)
	    node = node->children[1];
	else {			// go down both
	    Mod_AddToFatPVS(model, point, node->children[0]);
	    node = node->children[1];
	}
    }
}

/*
=============
Mod_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.

The FatPVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.
=============
*/
const leafbits_t *
Mod_FatPVS(const model_t *model, const vec3_t point)
{
    fatpvs->numleafs = model->numleafs;
    memset(fatpvs->bits, 0, pvscache_bytes);
    Mod_AddToFatPVS(model, point, model->nodes);

    return fatpvs;
}

/*
===================
Mod_ClearAll
===================
*/
void
Mod_ClearAll(void)
{
    int i;
    model_t *model;

    for (i = 0, model = mod_known; i < mod_numknown; i++, model++) {
	if (model->type != mod_alias)
	    model->needload = true;
	/*
	 * FIXME: sprites use the cache data pointer for their own purposes,
	 *        bypassing the Cache_Alloc/Free functions.
	 */
	if (model->type == mod_sprite)
	    model->cache.data = NULL;
    }

    fatpvs = NULL;
    memset(pvscache, 0, sizeof(pvscache));
    pvscache_numleafs = 0;
    pvscache_bytes = pvscache_blocks = 0;
    c_cachehit = c_cachemiss = 0;
}

/*
==================
Mod_FindName

==================
*/
static model_t *
Mod_FindName(const char *name)
{
    int i;
    model_t *model;

    if (!name[0])
	SV_Error("%s: NULL name", __func__);

//
// search the currently loaded models
//
    for (i = 0, model = mod_known; i < mod_numknown; i++, model++)
	if (!strcmp(model->name, name))
	    break;

    if (i == mod_numknown) {
	if (mod_numknown == MAX_MOD_KNOWN)
	    SV_Error("mod_numknown == MAX_MOD_KNOWN");
	snprintf(model->name, sizeof(model->name), "%s", name);
	model->needload = true;
	mod_numknown++;
    }

    return model;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *
Mod_LoadModel(model_t *model, qboolean crash)
{
    unsigned *buf;
    byte stackbuf[1024];	// avoid dirtying the cache heap
    size_t size;

    if (!model->needload) {
	if (model->type == mod_alias) {
	    if (Cache_Check(&model->cache))
		return model;
	} else
	    return model;		// not cached at all
    }
//
// load the file
//
    buf = COM_LoadStackFile(model->name, stackbuf, sizeof(stackbuf), &size);
    if (!buf) {
	if (crash)
	    SV_Error("%s: %s not found", __func__, model->name);
	return NULL;
    }
//
// allocate a new model
//
    COM_FileBase(model->name, loadname, sizeof(loadname));

//
// fill it in
//

// call the apropriate loader
    model->needload = false;

    switch (LittleLong(*(unsigned *)buf)) {
#ifndef SERVERONLY
    case IDPOLYHEADER:
	Mod_LoadAliasModel(mod_loader, model, buf, loadname);
	break;

    case IDSPRITEHEADER:
	Mod_LoadSpriteModel(model, buf, loadname);
	break;
#endif
    default:
	Mod_LoadBrushModel(model, buf, size);
	break;
    }

    return model;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *
Mod_ForName(const char *name, qboolean crash)
{
    model_t *model;

    model = Mod_FindName(name);

    return Mod_LoadModel(model, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static byte *mod_base;


/*
=================
Mod_LoadTextures
=================
*/
static void
Mod_LoadTextures(model_t *model, const lump_t *l)
{
    int i, j, pixels, num, max, altmax;
    miptex_t *mt;
    texture_t *tx, *tx2;
    texture_t *anims[10];
    texture_t *altanims[10];
    dmiptexlump_t *m;

    if (!l->filelen) {
	model->textures = NULL;
	return;
    }
    m = (dmiptexlump_t *)(mod_base + l->fileofs);

    m->nummiptex = LittleLong(m->nummiptex);

    model->numtextures = m->nummiptex;
    model->textures =
	Hunk_AllocName(m->nummiptex * sizeof(*model->textures), loadname);

    for (i = 0; i < m->nummiptex; i++) {
	m->dataofs[i] = LittleLong(m->dataofs[i]);
	if (m->dataofs[i] == -1)
	    continue;
	mt = (miptex_t *)((byte *)m + m->dataofs[i]);
	mt->width = (uint32_t)LittleLong(mt->width);
	mt->height = (uint32_t)LittleLong(mt->height);
	for (j = 0; j < MIPLEVELS; j++)
	    mt->offsets[j] = (uint32_t)LittleLong(mt->offsets[j]);

	if ((mt->width & 15) || (mt->height & 15))
	    SV_Error("Texture %s is not 16 aligned", mt->name);
	pixels = mt->width * mt->height / 64 * 85;
	tx = Hunk_AllocName(sizeof(texture_t) + pixels, loadname);
	model->textures[i] = tx;

	memcpy(tx->name, mt->name, sizeof(tx->name));
	tx->width = mt->width;
	tx->height = mt->height;
	for (j = 0; j < MIPLEVELS; j++)
	    tx->offsets[j] =
		mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t);
	// the pixels immediately follow the structures
	memcpy(tx + 1, mt + 1, pixels);

#ifndef SERVERONLY
	if (!strncmp(mt->name, "sky", 3))
	    R_InitSky(tx);
#ifdef GLQUAKE
	else
	    tx->gl_texturenum =	GL_LoadTexture(mt->name, tx->width, tx->height,
					       (byte *)(tx + 1), true, false);
#endif
#endif
    }

//
// sequence the animations
//
    for (i = 0; i < m->nummiptex; i++) {
	tx = model->textures[i];
	if (!tx || tx->name[0] != '+')
	    continue;
	if (tx->anim_next)
	    continue;		// allready sequenced

	// find the number of frames in the animation
	memset(anims, 0, sizeof(anims));
	memset(altanims, 0, sizeof(altanims));

	max = tx->name[1];
	if (max >= 'a' && max <= 'z')
	    max -= 'a' - 'A';
	if (max >= '0' && max <= '9') {
	    max -= '0';
	    altmax = 0;
	    anims[max] = tx;
	    max++;
	} else if (max >= 'A' && max <= 'J') {
	    altmax = max - 'A';
	    max = 0;
	    altanims[altmax] = tx;
	    altmax++;
	} else
	    SV_Error("Bad animating texture %s", tx->name);

	for (j = i + 1; j < m->nummiptex; j++) {
	    tx2 = model->textures[j];
	    if (!tx2 || tx2->name[0] != '+')
		continue;
	    if (strcmp(tx2->name + 2, tx->name + 2))
		continue;

	    num = tx2->name[1];
	    if (num >= 'a' && num <= 'z')
		num -= 'a' - 'A';
	    if (num >= '0' && num <= '9') {
		num -= '0';
		anims[num] = tx2;
		if (num + 1 > max)
		    max = num + 1;
	    } else if (num >= 'A' && num <= 'J') {
		num = num - 'A';
		altanims[num] = tx2;
		if (num + 1 > altmax)
		    altmax = num + 1;
	    } else
		SV_Error("Bad animating texture %s", tx->name);
	}

#define	ANIM_CYCLE	2
	// link them all together
	for (j = 0; j < max; j++) {
	    tx2 = anims[j];
	    if (!tx2)
		SV_Error("Missing frame %i of %s", j, tx->name);
	    tx2->anim_total = max * ANIM_CYCLE;
	    tx2->anim_min = j * ANIM_CYCLE;
	    tx2->anim_max = (j + 1) * ANIM_CYCLE;
	    tx2->anim_next = anims[(j + 1) % max];
	    if (altmax)
		tx2->alternate_anims = altanims[0];
	}
	for (j = 0; j < altmax; j++) {
	    tx2 = altanims[j];
	    if (!tx2)
		SV_Error("Missing frame %i of %s", j, tx->name);
	    tx2->anim_total = altmax * ANIM_CYCLE;
	    tx2->anim_min = j * ANIM_CYCLE;
	    tx2->anim_max = (j + 1) * ANIM_CYCLE;
	    tx2->anim_next = altanims[(j + 1) % altmax];
	    if (max)
		tx2->alternate_anims = anims[0];
	}
    }
}

/*
=================
Mod_LoadLighting
=================
*/
static void
Mod_LoadLighting(model_t *model, const lump_t *l)
{
    if (!l->filelen) {
	model->lightdata = NULL;
	return;
    }
    model->lightdata = Hunk_AllocName(l->filelen, loadname);
    memcpy(model->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
static void
Mod_LoadVisibility(model_t *model, const lump_t *l)
{
    if (!l->filelen) {
	model->visdata = NULL;
	return;
    }
    model->visdata = Hunk_AllocName(l->filelen, loadname);
    memcpy(model->visdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadEntities
=================
*/
static void
Mod_LoadEntities(model_t *model, const lump_t *l)
{
    if (!l->filelen) {
	model->entities = NULL;
	return;
    }
    model->entities = Hunk_AllocName(l->filelen, loadname);
    memcpy(model->entities, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVertexes
=================
*/
static void
Mod_LoadVertexes(model_t *model, const lump_t *l)
{
    dvertex_t *in;
    mvertex_t *out;
    int i, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->vertexes = out;
    model->numvertexes = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->position[0] = LittleFloat(in->point[0]);
	out->position[1] = LittleFloat(in->point[1]);
	out->position[2] = LittleFloat(in->point[2]);
    }
}

/*
=================
Mod_LoadSubmodels
=================
*/
static void
Mod_LoadSubmodels(model_t *model, const lump_t *l)
{
    dmodel_t *in;
    dmodel_t *out;
    int i, j, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->submodels = out;
    model->numsubmodels = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {	// spread the mins / maxs by a pixel
	    out->mins[j] = LittleFloat(in->mins[j]) - 1;
	    out->maxs[j] = LittleFloat(in->maxs[j]) + 1;
	    out->origin[j] = LittleFloat(in->origin[j]);
	}
	for (j = 0; j < MAX_MAP_HULLS; j++)
	    out->headnode[j] = LittleLong(in->headnode[j]);
	out->visleafs = LittleLong(in->visleafs);
	out->firstface = LittleLong(in->firstface);
	out->numfaces = LittleLong(in->numfaces);
    }
}

/*
=================
Mod_LoadEdges
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadEdges_BSP29(model_t *model, const lump_t *l)
{
    bsp29_dedge_t *in;
    medge_t *out;
    int i, count;

    in = (bsp29_dedge_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName((count + 1) * sizeof(*out), loadname);

    model->edges = out;
    model->numedges = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->v[0] = (uint16_t)LittleShort(in->v[0]);
	out->v[1] = (uint16_t)LittleShort(in->v[1]);
    }
}

static void
Mod_LoadEdges_BSP2(model_t *model, const lump_t *l)
{
    bsp2_dedge_t *in;
    medge_t *out;
    int i, count;

    in = (bsp2_dedge_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName((count + 1) * sizeof(*out), loadname);

    model->edges = out;
    model->numedges = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->v[0] = (uint32_t)LittleLong(in->v[0]);
	out->v[1] = (uint32_t)LittleLong(in->v[1]);
    }
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void
Mod_LoadTexinfo(model_t *model, const lump_t *l)
{
    texinfo_t *in;
    mtexinfo_t *out;
    int i, j, count;
    int miptex;
    float len1, len2;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->texinfo = out;
    model->numtexinfo = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 4; j++) {
	    out->vecs[0][j] = LittleFloat(in->vecs[0][j]);
	    out->vecs[1][j] = LittleFloat(in->vecs[1][j]);
	}
	len1 = Length(out->vecs[0]);
	len2 = Length(out->vecs[1]);
	len1 = (len1 + len2) / 2;
	if (len1 < 0.32)
	    out->mipadjust = 4;
	else if (len1 < 0.49)
	    out->mipadjust = 3;
	else if (len1 < 0.99)
	    out->mipadjust = 2;
	else
	    out->mipadjust = 1;

	miptex = LittleLong(in->miptex);
	out->flags = LittleLong(in->flags);

	if (!model->textures) {
#ifndef SERVERONLY
	    out->texture = r_notexture_mip;	// checkerboard texture
#else
	    out->texture = &r_notexture_mip_qwsv;	// checkerboard texture
#endif
	    out->flags = 0;
	} else {
	    if (miptex >= model->numtextures)
		SV_Error("miptex >= model->numtextures");
	    out->texture = model->textures[miptex];
	    if (!out->texture) {
#ifndef SERVERONLY
		out->texture = r_notexture_mip;	// texture not found
#else
		out->texture = &r_notexture_mip_qwsv;	// texture not found
#endif
		out->flags = 0;
	    }
	}
    }
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void
CalcSurfaceExtents(const model_t *model, msurface_t *s)
{
    float mins[2], maxs[2], val;
    int i, j, e;
    const mvertex_t *v;
    const mtexinfo_t *tex;
    int bmins[2], bmaxs[2];

    mins[0] = mins[1] = FLT_MAX;
    maxs[0] = maxs[1] = -FLT_MAX;

    tex = s->texinfo;

    for (i = 0; i < s->numedges; i++) {
	e = model->surfedges[s->firstedge + i];
	if (e >= 0)
	    v = &model->vertexes[model->edges[e].v[0]];
	else
	    v = &model->vertexes[model->edges[-e].v[1]];

	for (j = 0; j < 2; j++) {
	    val = v->position[0] * tex->vecs[j][0] +
		v->position[1] * tex->vecs[j][1] +
		v->position[2] * tex->vecs[j][2] + tex->vecs[j][3];
	    if (val < mins[j])
		mins[j] = val;
	    if (val > maxs[j])
		maxs[j] = val;
	}
    }

    for (i = 0; i < 2; i++) {
	bmins[i] = floor(mins[i] / 16);
	bmaxs[i] = ceil(maxs[i] / 16);

	s->texturemins[i] = bmins[i] * 16;
	s->extents[i] = (bmaxs[i] - bmins[i]) * 16;
	if (!(tex->flags & TEX_SPECIAL) && s->extents[i] > 256)
	    SV_Error("Bad surface extents");
    }
}

static void
CalcSurfaceBounds(const model_t *model, msurface_t *surf)
{
    int i, j, edgenum;
    const medge_t *edge;
    const mvertex_t *v;

    surf->mins[0] = surf->mins[1] = surf->mins[2] = FLT_MAX;
    surf->maxs[0] = surf->maxs[1] = surf->maxs[2] = -FLT_MAX;

    for (i = 0; i < surf->numedges; i++) {
	edgenum = model->surfedges[surf->firstedge + i];
	if (edgenum >= 0) {
	    edge = &model->edges[edgenum];
	    v = &model->vertexes[edge->v[0]];
	} else {
	    edge = &model->edges[-edgenum];
	    v = &model->vertexes[edge->v[1]];
	}

	for (j = 0; j < 3; j++) {
	    if (surf->mins[j] > v->position[j])
		surf->mins[j] = v->position[j];
	    if (surf->maxs[j] < v->position[j])
		surf->maxs[j] = v->position[j];
	}
    }
}

/*
=================
Mod_LoadFaces
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadFaces_BSP29(model_t *model, const lump_t *l)
{
    bsp29_dface_t *in;
    msurface_t *out;
    int i, count, surfnum;
    int planenum, side;

    in = (bsp29_dface_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->surfaces = out;
    model->numsurfaces = count;

    for (surfnum = 0; surfnum < count; surfnum++, in++, out++) {
	out->firstedge = LittleLong(in->firstedge);
	out->numedges = LittleShort(in->numedges);
	out->flags = 0;

	/* FIXME - Also check numedges doesn't overflow edges */
	if (out->numedges <= 0)
	    SV_Error("%s: bmodel %s has surface with no edges", __func__,
		     model->name);

	planenum = LittleShort(in->planenum);
	side = LittleShort(in->side);
	if (side)
	    out->flags |= SURF_PLANEBACK;

	out->plane = model->planes + planenum;
	out->texinfo = model->texinfo + LittleShort(in->texinfo);

	CalcSurfaceExtents(model, out);
	CalcSurfaceBounds(model, out);

	// lighting info

	for (i = 0; i < MAXLIGHTMAPS; i++)
	    out->styles[i] = in->styles[i];
	i = LittleLong(in->lightofs);
	if (i == -1)
	    out->samples = NULL;
	else
	    out->samples = model->lightdata + i;

	/* set the surface drawing flags */
	if (!strncmp(out->texinfo->texture->name, "sky", 3)) {
	    out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
#ifdef GLQUAKE
	    GL_SubdivideSurface(model, out, loadname);
#endif
	} else if (!strncmp(out->texinfo->texture->name, "*", 1)) {
	    out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
	    for (i = 0; i < 2; i++) {
		out->extents[i] = 16384;
		out->texturemins[i] = -8192;
	    }
#ifdef GLQUAKE
	    GL_SubdivideSurface(model, out, loadname);
#endif
	}
    }
}

static void
Mod_LoadFaces_BSP2(model_t *model, const lump_t *l)
{
    bsp2_dface_t *in;
    msurface_t *out;
    int i, count, surfnum;
    int planenum, side;

    in = (bsp2_dface_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->surfaces = out;
    model->numsurfaces = count;

    for (surfnum = 0; surfnum < count; surfnum++, in++, out++) {
	out->firstedge = LittleLong(in->firstedge);
	out->numedges = LittleLong(in->numedges);
	out->flags = 0;

	planenum = LittleLong(in->planenum);
	side = LittleLong(in->side);
	if (side)
	    out->flags |= SURF_PLANEBACK;

	out->plane = model->planes + planenum;
	out->texinfo = model->texinfo + LittleLong(in->texinfo);

	CalcSurfaceExtents(model, out);
	CalcSurfaceBounds(model, out);

	// lighting info

	for (i = 0; i < MAXLIGHTMAPS; i++)
	    out->styles[i] = in->styles[i];
	i = LittleLong(in->lightofs);
	if (i == -1)
	    out->samples = NULL;
	else
	    out->samples = model->lightdata + i;

	/* set the surface drawing flags */
	if (!strncmp(out->texinfo->texture->name, "sky", 3)) {
	    out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
#ifdef GLQUAKE
	    GL_SubdivideSurface(model, out, loadname);
#endif
	} else if (!strncmp(out->texinfo->texture->name, "*", 1)) {
	    out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
	    for (i = 0; i < 2; i++) {
		out->extents[i] = 16384;
		out->texturemins[i] = -8192;
	    }
#ifdef GLQUAKE
	    GL_SubdivideSurface(model, out, loadname);
#endif
	}
    }
}

/*
=================
Mod_SetParent
=================
*/
static void
Mod_SetParent(mnode_t *node, mnode_t *parent)
{
    node->parent = parent;
    if (node->contents < 0)
	return;
    Mod_SetParent(node->children[0], node);
    Mod_SetParent(node->children[1], node);
}

/*
=================
Mod_LoadNodes
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadNodes_BSP29(model_t *model, const lump_t *l)
{
    int i, j, count, p;
    bsp29_dnode_t *in;
    mnode_t *out;

    in = (bsp29_dnode_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->nodes = out;
    model->numnodes = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->mins[j] = LittleShort(in->mins[j]);
	    out->maxs[j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->planenum);
	out->plane = model->planes + p;

	out->firstsurface = (uint16_t)LittleShort(in->firstface);
	out->numsurfaces = (uint16_t)LittleShort(in->numfaces);

	for (j = 0; j < 2; j++) {
	    p = LittleShort(in->children[j]);
	    if (p >= 0)
		out->children[j] = model->nodes + p;
	    else
		out->children[j] = (mnode_t *)(model->leafs + (-1 - p));
	}
    }

    Mod_SetParent(model->nodes, NULL);	// sets nodes and leafs
}

static void
Mod_LoadNodes_BSP2(model_t *model, const lump_t *l)
{
    int i, j, count, p;
    bsp2_dnode_t *in;
    mnode_t *out;

    in = (bsp2_dnode_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->nodes = out;
    model->numnodes = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->mins[j] = LittleShort(in->mins[j]);
	    out->maxs[j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->planenum);
	out->plane = model->planes + p;

	out->firstsurface = (uint32_t)LittleLong(in->firstface);
	out->numsurfaces = (uint32_t)LittleLong(in->numfaces);

	for (j = 0; j < 2; j++) {
	    p = LittleLong(in->children[j]);
	    if (p >= 0)
		out->children[j] = model->nodes + p;
	    else
		out->children[j] = (mnode_t *)(model->leafs + (-1 - p));
	}
    }

    Mod_SetParent(model->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadLeafs_BSP29(model_t *model, const lump_t *l)
{
    bsp29_dleaf_t *in;
    mleaf_t *out;
    int i, j, count, p;

    in = (bsp29_dleaf_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->leafs = out;
    model->numleafs = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->mins[j] = LittleShort(in->mins[j]);
	    out->maxs[j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->contents);
	out->contents = p;

	out->firstmarksurface = model->marksurfaces +
	    (uint16_t)LittleShort(in->firstmarksurface);
	out->nummarksurfaces = (uint16_t)LittleShort(in->nummarksurfaces);

	p = LittleLong(in->visofs);
	if (p == -1)
	    out->compressed_vis = NULL;
	else
	    out->compressed_vis = model->visdata + p;
	out->efrags = NULL;

	for (j = 0; j < 4; j++)
	    out->ambient_sound_level[j] = in->ambient_level[j];

#ifdef GLQUAKE
	// FIXME - gl underwater warp
	// this warping is ugly, these ifdefs are ugly - get rid of it all?
	if (out->contents != CONTENTS_EMPTY) {
	    for (j = 0; j < out->nummarksurfaces; j++)
		out->firstmarksurface[j]->flags |= SURF_UNDERWATER;
	}

#ifdef QW_HACK
	{
	    char s[80];
	    snprintf(s, sizeof(s), "maps/%s.bsp",
		     Info_ValueForKey(cl.serverinfo, "map"));
	    s[sizeof(s) - 1] = 0;
	    if (strcmp(s, model->name)) {
#endif
		for (j = 0; j < out->nummarksurfaces; j++)
		    out->firstmarksurface[j]->flags |= SURF_DONTWARP;
#ifdef QW_HACK
	    }
	}
#endif
#endif
    }
}

static void
Mod_LoadLeafs_BSP2(model_t *model, const lump_t *l)
{
    bsp2_dleaf_t *in;
    mleaf_t *out;
    int i, j, count, p;

    in = (bsp2_dleaf_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->leafs = out;
    model->numleafs = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->mins[j] = LittleShort(in->mins[j]);
	    out->maxs[j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->contents);
	out->contents = p;

	out->firstmarksurface = model->marksurfaces +
	    (uint32_t)LittleLong(in->firstmarksurface);
	out->nummarksurfaces = (uint32_t)LittleLong(in->nummarksurfaces);

	p = LittleLong(in->visofs);
	if (p == -1)
	    out->compressed_vis = NULL;
	else
	    out->compressed_vis = model->visdata + p;
	out->efrags = NULL;

	for (j = 0; j < 4; j++)
	    out->ambient_sound_level[j] = in->ambient_level[j];

#ifdef GLQUAKE
	// FIXME - gl underwater warp
	// this warping is ugly, these ifdefs are ugly - get rid of it all?
	if (out->contents != CONTENTS_EMPTY) {
	    for (j = 0; j < out->nummarksurfaces; j++)
		out->firstmarksurface[j]->flags |= SURF_UNDERWATER;
	}

#ifdef QW_HACK
	{
	    char s[80];
	    snprintf(s, sizeof(s), "maps/%s.bsp",
		     Info_ValueForKey(cl.serverinfo, "map"));
	    s[sizeof(s) - 1] = 0;
	    if (strcmp(s, model->name)) {
#endif
		for (j = 0; j < out->nummarksurfaces; j++)
		    out->firstmarksurface[j]->flags |= SURF_DONTWARP;
#ifdef QW_HACK
	    }
	}
#endif
#endif
    }
}

/*
=================
Mod_LoadClipnodes
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadClipnodes_BSP29(model_t *model, const lump_t *l)
{
    bsp29_dclipnode_t *in;
    mclipnode_t *out;
    int i, j, count;
    hull_t *hull;

    in = (bsp29_dclipnode_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->clipnodes = out;
    model->numclipnodes = count;

    hull = &model->hulls[1];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = model->planes;
    hull->clip_mins[0] = -16;
    hull->clip_mins[1] = -16;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 16;
    hull->clip_maxs[1] = 16;
    hull->clip_maxs[2] = 32;

    hull = &model->hulls[2];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = model->planes;
    hull->clip_mins[0] = -32;
    hull->clip_mins[1] = -32;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 32;
    hull->clip_maxs[1] = 32;
    hull->clip_maxs[2] = 64;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = LittleLong(in->planenum);
	for (j = 0; j < 2; j++) {
	    out->children[j] = (uint16_t)LittleShort(in->children[j]);
	    if (out->children[j] > 0xfff0)
		out->children[j] -= 0x10000;
	    if (out->children[j] >= count)
		SV_Error("%s: bad clipnode child number", __func__);
	}
    }
}

static void
Mod_LoadClipnodes_BSP2(model_t *model, const lump_t *l)
{
    bsp2_dclipnode_t *in;
    mclipnode_t *out;
    int i, j, count;
    hull_t *hull;

    in = (bsp2_dclipnode_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->clipnodes = out;
    model->numclipnodes = count;

    hull = &model->hulls[1];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = model->planes;
    hull->clip_mins[0] = -16;
    hull->clip_mins[1] = -16;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 16;
    hull->clip_maxs[1] = 16;
    hull->clip_maxs[2] = 32;

    hull = &model->hulls[2];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = model->planes;
    hull->clip_mins[0] = -32;
    hull->clip_mins[1] = -32;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 32;
    hull->clip_maxs[1] = 32;
    hull->clip_maxs[2] = 64;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = LittleLong(in->planenum);
	for (j = 0; j < 2; j++) {
	    out->children[j] = LittleLong(in->children[j]);
	    if (out->children[j] >= count)
		SV_Error("%s: bad clipnode child number", __func__);
	}
    }
}

/*
=================
Mod_MakeHull0

Duplicate the drawing hull structure as a clipping hull
=================
*/
static void
Mod_MakeHull0(model_t *model)
{
    mnode_t *in, *child;
    mclipnode_t *out;
    int i, j, count;
    hull_t *hull;

    hull = &model->hulls[0];

    in = model->nodes;
    count = model->numnodes;
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = model->planes;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = in->plane - model->planes;
	for (j = 0; j < 2; j++) {
	    child = in->children[j];
	    if (child->contents < 0)
		out->children[j] = child->contents;
	    else
		out->children[j] = child - model->nodes;
	}
    }
}

/*
=================
Mod_LoadMarksurfaces
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadMarksurfaces_BSP29(model_t *model, const lump_t *l)
{
    int i, j, count;
    uint16_t *in;
    msurface_t **out;

    in = (uint16_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->marksurfaces = out;
    model->nummarksurfaces = count;

    for (i = 0; i < count; i++) {
	j = (uint16_t)LittleShort(in[i]);
	if (j >= model->numsurfaces)
	    SV_Error("%s: bad surface number", __func__);
	out[i] = model->surfaces + j;
    }
}

static void
Mod_LoadMarksurfaces_BSP2(model_t *model, const lump_t *l)
{
    int i, j, count;
    uint32_t *in;
    msurface_t **out;

    in = (uint32_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->marksurfaces = out;
    model->nummarksurfaces = count;

    for (i = 0; i < count; i++) {
	j = (uint32_t)LittleLong(in[i]);
	if (j >= model->numsurfaces)
	    SV_Error("%s: bad surface number", __func__);
	out[i] = model->surfaces + j;
    }
}

/*
=================
Mod_LoadSurfedges
=================
*/
static void
Mod_LoadSurfedges(model_t *model, const lump_t *l)
{
    int i, count;
    int *in, *out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    model->surfedges = out;
    model->numsurfedges = count;

    for (i = 0; i < count; i++)
	out[i] = LittleLong(in[i]);
}

/*
=================
Mod_LoadPlanes
=================
*/
static void
Mod_LoadPlanes(model_t *model, const lump_t *l)
{
    int i, j;
    mplane_t *out;
    dplane_t *in;
    int count;
    int bits;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, model->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * 2 * sizeof(*out), loadname);

    model->planes = out;
    model->numplanes = count;

    for (i = 0; i < count; i++, in++, out++) {
	bits = 0;
	for (j = 0; j < 3; j++) {
	    out->normal[j] = LittleFloat(in->normal[j]);
	    if (out->normal[j] < 0)
		bits |= 1 << j;
	}

	out->dist = LittleFloat(in->dist);
	out->type = LittleLong(in->type);
	out->signbits = bits;
    }
}

/*
=================
RadiusFromBounds
=================
*/
static float
RadiusFromBounds(vec3_t mins, vec3_t maxs)
{
    int i;
    vec3_t corner;

    for (i = 0; i < 3; i++)
	corner[i] = qmax(fabs(mins[i]), fabs(maxs[i]));

    return Length(corner);
}

static void
Mod_SetupSubmodels(model_t *world)
{
    const dmodel_t *dmodel;
    model_t *submodel;
    int i, j;

    /* Set up the extra submodel fields, starting with the world */
    submodel = world;
    for (i = 0; i < world->numsubmodels; i++) {
	if (i > 0) {
	    /* Find next submodel and copy base fields from world */
	    char name[10];
	    snprintf(name, sizeof(name), "*%d", i);
	    submodel = Mod_FindName(name);
	    *submodel = *world;
	    snprintf(submodel->name, sizeof(submodel->name), "%s", name);
	}

	dmodel = &world->submodels[i];
	submodel->hulls[0].firstclipnode = dmodel->headnode[0];
	for (j = 1; j < MAX_MAP_HULLS; j++) {
	    submodel->hulls[j].firstclipnode = dmodel->headnode[j];
	    submodel->hulls[j].lastclipnode = submodel->numclipnodes - 1;
	}

	submodel->firstmodelsurface = dmodel->firstface;
	submodel->nummodelsurfaces = dmodel->numfaces;

	VectorCopy(dmodel->maxs, submodel->maxs);
	VectorCopy(dmodel->mins, submodel->mins);

	submodel->radius = RadiusFromBounds(submodel->mins, submodel->maxs);
	submodel->numleafs = dmodel->visleafs;
    }
}

/*
=================
Mod_LoadBrushModel
=================
*/
static void
Mod_LoadBrushModel(model_t *model, void *buffer, size_t size)
{
    int i, j;
    dheader_t *header;

    model->type = mod_brush;
    header = (dheader_t *)buffer;

    /* swap all the header entries */
    header->version = LittleLong(header->version);
    for (i = 0; i < HEADER_LUMPS; i++) {
	header->lumps[i].fileofs = LittleLong(header->lumps[i].fileofs);
	header->lumps[i].filelen = LittleLong(header->lumps[i].filelen);
    }

    if (header->version != BSPVERSION && header->version != BSP2VERSION)
	SV_Error("%s: %s has wrong version number (%i should be %i or %i)",
		 __func__, model->name, header->version,
		 BSPVERSION, BSP2VERSION);

    mod_base = (byte *)header;

    /*
     * Check the lump extents
     * FIXME - do this more generally... cleanly...?
     */
    for (i = 0; i < HEADER_LUMPS; ++i) {
	int b1 = header->lumps[i].fileofs;
	int e1 = b1 + header->lumps[i].filelen;

	/*
	 * Sanity checks
	 * - begin and end >= 0 (end might overflow).
	 * - end > begin (again, overflow reqd.)
	 * - end < size of file.
	 */
	if (b1 > e1 || e1 > size || b1 < 0 || e1 < 0)
	    SV_Error("%s: bad lump extents in %s", __func__, model->name);

	/* Now, check that it doesn't overlap any other lumps */
	for (j = 0; j < HEADER_LUMPS; ++j) {
	    int b2 = header->lumps[j].fileofs;
	    int e2 = b2 + header->lumps[j].filelen;

	    if ((b1 < b2 && e1 > b2) || (b2 < b1 && e2 > b1))
		SV_Error("%s: overlapping lumps in %s", __func__, model->name);
	}
    }

#ifdef QW_HACK
    model->checksum = 0;
    model->checksum2 = 0;

// checksum all of the map, except for entities
    for (i = 0; i < HEADER_LUMPS; i++) {
	const lump_t *l = &header->lumps[i];
	unsigned int checksum;

	if (i == LUMP_ENTITIES)
	    continue;
	checksum = Com_BlockChecksum(mod_base + l->fileofs, l->filelen);
	model->checksum ^= checksum;
	if (i == LUMP_VISIBILITY || i == LUMP_LEAFS || i == LUMP_NODES)
	    continue;
	model->checksum2 ^= checksum;
    }
    model->checksum = LittleLong(model->checksum);
    model->checksum2 = LittleLong(model->checksum2);
#endif

    /* load into heap */
    Mod_LoadVertexes(model, &header->lumps[LUMP_VERTEXES]);
    if (header->version == BSPVERSION) {
	Mod_LoadEdges_BSP29(model, &header->lumps[LUMP_EDGES]);
    } else {
	Mod_LoadEdges_BSP2(model, &header->lumps[LUMP_EDGES]);
    }
    Mod_LoadSurfedges(model, &header->lumps[LUMP_SURFEDGES]);
    Mod_LoadTextures(model, &header->lumps[LUMP_TEXTURES]);
    Mod_LoadLighting(model, &header->lumps[LUMP_LIGHTING]);
    Mod_LoadPlanes(model, &header->lumps[LUMP_PLANES]);
    Mod_LoadTexinfo(model, &header->lumps[LUMP_TEXINFO]);
    if (header->version == BSPVERSION) {
	Mod_LoadFaces_BSP29(model, &header->lumps[LUMP_FACES]);
	Mod_LoadMarksurfaces_BSP29(model, &header->lumps[LUMP_MARKSURFACES]);
    } else {
	Mod_LoadFaces_BSP2(model, &header->lumps[LUMP_FACES]);
	Mod_LoadMarksurfaces_BSP2(model, &header->lumps[LUMP_MARKSURFACES]);
    }
    Mod_LoadVisibility(model, &header->lumps[LUMP_VISIBILITY]);
    if (header->version == BSPVERSION) {
	Mod_LoadLeafs_BSP29(model, &header->lumps[LUMP_LEAFS]);
	Mod_LoadNodes_BSP29(model, &header->lumps[LUMP_NODES]);
	Mod_LoadClipnodes_BSP29(model, &header->lumps[LUMP_CLIPNODES]);
    } else {
	Mod_LoadLeafs_BSP2(model, &header->lumps[LUMP_LEAFS]);
	Mod_LoadNodes_BSP2(model, &header->lumps[LUMP_NODES]);
	Mod_LoadClipnodes_BSP2(model, &header->lumps[LUMP_CLIPNODES]);
    }
    Mod_LoadEntities(model, &header->lumps[LUMP_ENTITIES]);
    Mod_LoadSubmodels(model, &header->lumps[LUMP_MODELS]);

    Mod_MakeHull0(model);

    model->numframes = 2;		// regular and alternate animation
    model->flags = 0;

    /*
     * Create space for the decompressed vis data
     * - We assume the main map is the first BSP file loaded (should be)
     * - If any other model has more leafs, then we may be in trouble...
     */
    if (model->numleafs > pvscache_numleafs) {
	if (pvscache[0].leafbits)
	    SV_Error("%s: %d allocated for visdata, but model %s has %d leafs",
		     __func__, pvscache_numleafs, model->name, model->numleafs);
	Mod_InitPVSCache(model->numleafs);
    }

    Mod_SetupSubmodels(model);
}

/*
 * =========================================================================
 *                          CLIENT ONLY FUNCTIONS
 * =========================================================================
 */
#ifndef SERVERONLY

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *
Mod_Extradata(model_t *model)
{
    void *data;

    data = Cache_Check(&model->cache);
    if (data)
	return data;

    Mod_LoadModel(model, true);
    if (!model->cache.data)
	Sys_Error("%s: caching failed", __func__);

    return model->cache.data;
}

/*
================
Mod_Print
================
*/
void
Mod_Print(void)
{
    int i;
    model_t *model;

    Con_Printf("Cached models:\n");
    for (i = 0, model = mod_known; i < mod_numknown; i++, model++)
	Con_Printf("%*p : %s\n", (int)sizeof(void *) * 2 + 2,
		   model->cache.data, model->name);
}

/*
==================
Mod_TouchModel

==================
*/
void
Mod_TouchModel(const char *name)
{
    model_t *model;

    model = Mod_FindName(name);

    if (!model->needload) {
	if (model->type == mod_alias)
	    Cache_Check(&model->cache);
    }
}

#endif /* !SERVERONLY */

/*
 * ===========================================================================
 * HULL BOXES
 *
 * To keep everything totally uniform, bounding boxes are turned into small
 * BSP trees instead of being traced directly.
 * ===========================================================================
 */

static const mclipnode_t box_clipnodes[6] = {
    { .planenum = 0, .children = { CONTENTS_EMPTY, 1 } },
    { .planenum = 1, .children = { 2, CONTENTS_EMPTY } },
    { .planenum = 2, .children = { CONTENTS_EMPTY, 3 } },
    { .planenum = 3, .children = { 4, CONTENTS_EMPTY } },
    { .planenum = 4, .children = { CONTENTS_EMPTY, 5 } },
    { .planenum = 5, .children = { CONTENTS_SOLID, CONTENTS_EMPTY } }
};

static const boxhull_t boxhull_template = {
    .hull = {
	.clipnodes = box_clipnodes,
	.firstclipnode = 0,
	.lastclipnode = 5
    },
    .planes = {
	{ .normal = { 1, 0, 0 }, .dist = 0, .type = 0 },
	{ .normal = { 1, 0, 0 }, .dist = 0, .type = 0 },
	{ .normal = { 0, 1, 0 }, .dist = 0, .type = 1 },
	{ .normal = { 0, 1, 0 }, .dist = 0, .type = 1 },
	{ .normal = { 0, 0, 1 }, .dist = 0, .type = 2 },
	{ .normal = { 0, 0, 1 }, .dist = 0, .type = 2 }
    }
};

/*
===================
SV_CreateBoxHull

Set up the planes and clipnodes using the template so that the six floats
of a bounding box can just be stored out and get a proper hull_t structure.
===================
*/
void
Mod_CreateBoxhull(const vec3_t mins, const vec3_t maxs, boxhull_t *boxhull)
{
    memcpy(boxhull, &boxhull_template, sizeof(boxhull_template));

    boxhull->hull.planes = boxhull->planes;
    boxhull->planes[0].dist = maxs[0];
    boxhull->planes[1].dist = mins[0];
    boxhull->planes[2].dist = maxs[1];
    boxhull->planes[3].dist = mins[1];
    boxhull->planes[4].dist = maxs[2];
    boxhull->planes[5].dist = mins[2];
}


/*
 * ===========================================================================
 * POINT / LINE TESTING IN HULLS
 * ===========================================================================
 */

/*
==================
Mod_HullPointContents
==================
*/
#ifndef USE_X86_ASM
int
Mod_HullPointContents(const hull_t *hull, int nodenum, const vec3_t point)
{
    float dist;
    const mclipnode_t *node;
    const mplane_t *plane;

    while (nodenum >= 0) {
	if (nodenum < hull->firstclipnode || nodenum > hull->lastclipnode)
	    SV_Error("%s: bad node number (%i)", __func__, nodenum);

	node = hull->clipnodes + nodenum;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	    dist = point[plane->type] - plane->dist;
	else
	    dist = DotProduct(plane->normal, point) - plane->dist;
	if (dist < 0)
	    nodenum = node->children[1];
	else
	    nodenum = node->children[0];
    }

    return nodenum;
}
#endif

/* 1/32 epsilon to keep floating point happy */
#define	DIST_EPSILON	(0.03125)

/*
==================
MOD_TraceHull
==================
*/
static qboolean
Mod_TraceHull_r(const hull_t *hull, int nodenum,
		const float p1f, const float p2f,
		const vec3_t p1, const vec3_t p2, trace_t *trace)
{
    const mclipnode_t *node;
    const mplane_t *plane;
    vec3_t mid;
    vec_t dist1, dist2, frac, midf;
    int i, child, side, contents;

    /* check for empty */
    if (nodenum < 0) {
	if (nodenum != CONTENTS_SOLID) {
	    trace->allsolid = false;
	    if (nodenum == CONTENTS_EMPTY)
		trace->inopen = true;
	    else
		trace->inwater = true;
	} else {
	    trace->startsolid = true;
	}
	return true;
    }

    if (nodenum < hull->firstclipnode || nodenum > hull->lastclipnode)
	SV_Error("%s: bad node number", __func__);

    /* Find the point distances */
    node = hull->clipnodes + nodenum;
    plane = hull->planes + node->planenum;
    if (plane->type < 3) {
	dist1 = p1[plane->type] - plane->dist;
	dist2 = p2[plane->type] - plane->dist;
    } else {
	dist1 = DotProduct(plane->normal, p1) - plane->dist;
	dist2 = DotProduct(plane->normal, p2) - plane->dist;
    }

#if 1
    if (dist1 >= 0 && dist2 >= 0) {
	child = node->children[0];
	return Mod_TraceHull_r(hull, child, p1f, p2f, p1, p2, trace);
    }
    if (dist1 < 0 && dist2 < 0) {
	child = node->children[1];
	return Mod_TraceHull_r(hull, child, p1f, p2f, p1, p2, trace);
    }
#else
    if ((dist1 >= DIST_EPSILON && dist2 >= DIST_EPSILON) || (dist2 > dist1 && dist1 >= 0)) {
	child = node->children[0];
	return Mod_TraceHull_r(hull, child, p1f, p2f, p1, p2, trace);
    }
    if ((dist1 <= -DIST_EPSILON && dist2 <= -DIST_EPSILON) || (dist2 < dist1 && dist1 <= 0)) {
	child = node->children[1];
	return Mod_TraceHull_r(hull, child, p1f, p2f, p1, p2, trace);
    }
#endif

    /* Put the crosspoint DIST_EPSILON pixels on the near side */
    if (dist1 < 0)
	frac = (dist1 + DIST_EPSILON) / (dist1 - dist2);
    else
	frac = (dist1 - DIST_EPSILON) / (dist1 - dist2);
    if (frac < 0)
	frac = 0;
    if (frac > 1)
	frac = 1;

    midf = p1f + (p2f - p1f) * frac;
    for (i = 0; i < 3; i++)
	mid[i] = p1[i] + frac * (p2[i] - p1[i]);

    side = (dist1 < 0);

    /* move up to the node */
    child = node->children[side];
    if (!Mod_TraceHull_r(hull, child, p1f, midf, p1, mid, trace))
	return false;

#ifdef PARANOID
    if (Mod_HullPointContents(sv_hullmodel, mid, child) == CONTENTS_SOLID) {
	Con_Printf("mid PointInHullSolid\n");
	return false;
    }
#endif

    child = node->children[side ^ 1];
    if (Mod_HullPointContents(hull, child, mid) != CONTENTS_SOLID)
	/* Go past the node */
	return Mod_TraceHull_r(hull, child, midf, p2f, mid, p2, trace);

    /* Never got out of the solid area */
    if (trace->allsolid)
	return false;

    /* The other side of the node is solid, this is the impact point */
    if (!side) {
	VectorCopy(plane->normal, trace->plane.normal);
	trace->plane.dist = plane->dist;
    } else {
	VectorSubtract(vec3_origin, plane->normal, trace->plane.normal);
	trace->plane.dist = -plane->dist;
    }

    /* shouldn't really happen, but does occasionally */
    contents = Mod_HullPointContents(hull, hull->firstclipnode, mid);
    while (contents == CONTENTS_SOLID) {
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

	contents = Mod_HullPointContents(hull, hull->firstclipnode, mid);
    }

    trace->fraction = midf;
    VectorCopy(mid, trace->endpos);

    return false;
}

qboolean
Mod_TraceHull(const hull_t *hull, int nodenum,
	      const vec3_t p1, const vec3_t p2, trace_t *trace)
{
    return Mod_TraceHull_r(hull, nodenum, 0, 1, p1, p2, trace);
}
