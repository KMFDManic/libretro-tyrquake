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

#include <string.h>

#include "common.h"
#include "crc.h"
#include "model.h"
#include "sys.h"

#ifdef GLQUAKE
#include "glquake.h"
#else
#include "r_local.h"
#endif

static aliashdr_t *pheader;

/* FIXME - get rid of these static limits by doing two passes? */

static stvert_t stverts[MAXALIASVERTS];
static mtriangle_t triangles[MAXALIASTRIS];

// a pose is a single set of vertexes.  a frame may be
// an animating sequence of poses
static const trivertx_t *poseverts[MAXALIASFRAMES];
static float poseintervals[MAXALIASFRAMES];
static int posenum;

#define MAXALIASSKINS 256

// a skin may be an animating set 1 or more textures
static float skinintervals[MAXALIASSKINS];
static byte *skindata[MAXALIASSKINS];
static int skinnum;

/*
=================
Mod_LoadAliasFrame
=================
*/
static void
Mod_LoadAliasFrame(const daliasframe_t *in, maliasframedesc_t *frame)
{
    int i;

    strncpy(frame->name, in->name, sizeof(frame->name));
    frame->name[sizeof(frame->name) - 1] = 0;
    frame->firstpose = posenum;
    frame->numposes = 1;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about
	// endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    poseverts[posenum] = in->verts;
    poseintervals[posenum] = 999.0f; /* unused, but make problems obvious */
    posenum++;
}


/*
=================
Mod_LoadAliasGroup

returns a pointer to the memory location following this frame group
=================
*/
static daliasframetype_t *
Mod_LoadAliasGroup(const daliasgroup_t *in, maliasframedesc_t *frame,
		   const char *loadname)
{
    int i, numframes;
    daliasframe_t *dframe;

    numframes = LittleLong(in->numframes);
    frame->firstpose = posenum;
    frame->numposes = numframes;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    dframe = (daliasframe_t *)&in->intervals[numframes];
    strncpy(frame->name, dframe->name, sizeof(frame->name));
    frame->name[sizeof(frame->name) - 1] = 0;
    for (i = 0; i < numframes; i++) {
	poseverts[posenum] = dframe->verts;
	poseintervals[posenum] = LittleFloat(in->intervals[i].interval);
	if (poseintervals[posenum] <= 0)
	    Sys_Error("%s: interval <= 0", __func__);
	posenum++;
	dframe = (daliasframe_t *)&dframe->verts[pheader->numverts];
    }

    return (daliasframetype_t *)dframe;
}


/*
=================
Mod_LoadAliasSkinGroup
=================
*/
static void *
Mod_LoadAliasSkinGroup(void *pin, maliasskindesc_t *pskindesc, int skinsize)
{
    daliasskingroup_t *pinskingroup;
    daliasskininterval_t *pinskinintervals;
    byte *pdata;
    int i;

    pinskingroup = pin;
    pskindesc->firstframe = skinnum;
    pskindesc->numframes = LittleLong(pinskingroup->numskins);
    pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

    for (i = 0; i < pskindesc->numframes; i++) {
	skinintervals[skinnum] = LittleFloat(pinskinintervals->interval);
	if (skinintervals[skinnum] <= 0)
	    Sys_Error("%s: interval <= 0", __func__);
	skinnum++;
	pinskinintervals++;
    }

    pdata = (byte *)pinskinintervals;
    for (i = 0; i < pskindesc->numframes; i++) {
	skindata[pskindesc->firstframe + i] = pdata;
	pdata += skinsize;
    }

    return pdata;
}

/*
===============
Mod_LoadAllSkins
===============
*/
static void *
Mod_LoadAllSkins(const model_loader_t *loader, const model_t *loadmodel,
		 int numskins, daliasskintype_t *pskintype,
		 const char *loadname)
{
    int i, skinsize;
    maliasskindesc_t *pskindesc;
    float *pskinintervals;
    byte *pskindata;

    if (numskins < 1
#if defined(GLQUAKE) && defined(NQ_HACK)
	|| numskins > MAX_SKINS
#endif
	)
	Sys_Error("%s: Invalid # of skins: %d", __func__, numskins);
    if (pheader->skinwidth & 0x03)
	Sys_Error("%s: skinwidth not multiple of 4", __func__);

    skinsize = pheader->skinwidth * pheader->skinheight;
    pskindesc = Hunk_AllocName(numskins * sizeof(maliasskindesc_t), loadname);
    pheader->skindesc = (byte *)pskindesc - (byte *)pheader;

    skinnum = 0;
    for (i = 0; i < numskins; i++) {
	aliasskintype_t skintype = LittleLong(pskintype->type);
	if (skintype == ALIAS_SKIN_SINGLE) {
	    pskindesc[i].firstframe = skinnum;
	    pskindesc[i].numframes = 1;
	    skindata[skinnum] = (byte *)(pskintype + 1);
	    skinintervals[skinnum] = 999.0f;
	    skinnum++;
	    pskintype = (daliasskintype_t *)((byte *)(pskintype + 1) + skinsize);
	} else {
	    pskintype = Mod_LoadAliasSkinGroup(pskintype + 1, pskindesc + i,
					       skinsize);
	}
    }

    pskinintervals = Hunk_Alloc(skinnum * sizeof(float));
    pheader->skinintervals = (byte *)pskinintervals - (byte *)pheader;
    memcpy(pskinintervals, skinintervals, skinnum * sizeof(float));

    /* Hand off saving the skin data to the loader */
    pskindata = loader->LoadSkinData(loadmodel->name, pheader, skinnum, skindata);
    pheader->skindata = (byte *)pskindata - (byte *)pheader;

    return pskintype;
}

/*
=================
Mod_LoadAliasModel
=================
*/
void
Mod_LoadAliasModel(const model_loader_t *loader, model_t *model, void *buffer,
		   const char *loadname)
{
    byte *container;
    int i, j, pad;
    mdl_t *pinmodel;
    stvert_t *pinstverts;
    dtriangle_t *pintriangles;
    int version, numframes;
    int size;
    daliasframetype_t *pframetype;
    daliasframe_t *frame;
    daliasgroup_t *group;
    daliasskintype_t *pskintype;
    int start, end, total;
    float *intervals;

#ifdef QW_HACK
    unsigned short crc;
    const char *crcmodel = NULL;
    if (!strcmp(model->name, "progs/player.mdl"))
	crcmodel = "pmodel";
    if (!strcmp(model->name, "progs/eyes.mdl"))
	crcmodel = "emodel";

    if (crcmodel) {
	crc = CRC_Block(buffer, com_filesize);
	Info_SetValueForKey(cls.userinfo, crcmodel, va("%d", (int)crc),
			    MAX_INFO_STRING);
	if (cls.state >= ca_connected) {
	    MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	    MSG_WriteStringf(&cls.netchan.message, "setinfo %s %d", crcmodel,
			     (int)crc);
	}
    }
#endif

    start = Hunk_LowMark();

    pinmodel = (mdl_t *)buffer;

    version = LittleLong(pinmodel->version);
    if (version != ALIAS_VERSION)
	Sys_Error("%s has wrong version number (%i should be %i)",
		  model->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
    pad = loader->Aliashdr_Padding();
    size = pad + sizeof(aliashdr_t) +
	LittleLong(pinmodel->numframes) * sizeof(pheader->frames[0]);

    container = Hunk_AllocName(size, loadname);
    pheader = (aliashdr_t *)(container + pad);

    model->flags = LittleLong(pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
    pheader->numskins = LittleLong(pinmodel->numskins);
    pheader->skinwidth = LittleLong(pinmodel->skinwidth);
    pheader->skinheight = LittleLong(pinmodel->skinheight);

    if (pheader->skinheight > MAX_LBM_HEIGHT)
	Sys_Error("model %s has a skin taller than %d", model->name,
		  MAX_LBM_HEIGHT);

    pheader->numverts = LittleLong(pinmodel->numverts);

    if (pheader->numverts <= 0)
	Sys_Error("model %s has no vertices", model->name);

    if (pheader->numverts > MAXALIASVERTS)
	Sys_Error("model %s has too many vertices", model->name);

    pheader->numtris = LittleLong(pinmodel->numtris);

    if (pheader->numtris <= 0)
	Sys_Error("model %s has no triangles", model->name);

    pheader->numframes = LittleLong(pinmodel->numframes);
    pheader->size = LittleFloat(pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
    model->synctype = LittleLong(pinmodel->synctype);
    model->numframes = pheader->numframes;

    for (i = 0; i < 3; i++) {
	pheader->scale[i] = LittleFloat(pinmodel->scale[i]);
	pheader->scale_origin[i] = LittleFloat(pinmodel->scale_origin[i]);
    }

//
// load the skins
//
    pskintype = (daliasskintype_t *)&pinmodel[1];
    pskintype = Mod_LoadAllSkins(loader, model, pheader->numskins,
				 pskintype, loadname);

//
// set base s and t vertices
//
    pinstverts = (stvert_t *)pskintype;
    for (i = 0; i < pheader->numverts; i++) {
	stverts[i].onseam = LittleLong(pinstverts[i].onseam);
	stverts[i].s = LittleLong(pinstverts[i].s);
	stverts[i].t = LittleLong(pinstverts[i].t);
    }

//
// set up the triangles
//
    pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];
    for (i = 0; i < pheader->numtris; i++) {
	triangles[i].facesfront = LittleLong(pintriangles[i].facesfront);
	for (j = 0; j < 3; j++) {
	    triangles[i].vertindex[j] = LittleLong(pintriangles[i].vertindex[j]);
	    if (triangles[i].vertindex[j] < 0 ||
		triangles[i].vertindex[j] >= pheader->numverts)
		Sys_Error("%s: invalid vertex index (%d of %d) in %s\n",
			  __func__, triangles[i].vertindex[j],
			  pheader->numverts, model->name);
	}
    }

//
// load the frames
//
    numframes = pheader->numframes;
    if (numframes < 1)
	Sys_Error("%s: Invalid # of frames: %d", __func__, numframes);

    posenum = 0;
    pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];

    for (i = 0; i < numframes; i++) {
	if (LittleLong(pframetype->type) == ALIAS_SINGLE) {
	    frame = (daliasframe_t *)(pframetype + 1);
	    Mod_LoadAliasFrame(frame, &pheader->frames[i]);
	    pframetype = (daliasframetype_t *)&frame->verts[pheader->numverts];
	} else {
	    group = (daliasgroup_t *)(pframetype + 1);
	    pframetype = Mod_LoadAliasGroup(group, &pheader->frames[i],
					    loadname);
	}
    }
    pheader->numposes = posenum;
    model->type = mod_alias;

// FIXME: do this right
    model->mins[0] = model->mins[1] = model->mins[2] = -16;
    model->maxs[0] = model->maxs[1] = model->maxs[2] = 16;

    /*
     * Save the frame intervals
     */
    intervals = Hunk_Alloc(pheader->numposes * sizeof(float));
    pheader->poseintervals = (byte *)intervals - (byte *)pheader;
    for (i = 0; i < pheader->numposes; i++)
	intervals[i] = poseintervals[i];

    /*
     * Save the mesh data (verts, stverts, triangles)
     */
    loader->LoadMeshData(model, pheader, triangles, stverts, poseverts);

//
// move the complete, relocatable alias model to the cache
//
    end = Hunk_LowMark();
    total = end - start;

    Cache_AllocPadded(&model->cache, pad, total - pad, loadname);
    if (!model->cache.data)
	return;

    memcpy((byte *)model->cache.data - pad, container, total);

    Hunk_FreeToLowMark(start);
}
