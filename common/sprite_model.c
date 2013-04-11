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
#include "console.h"
#include "model.h"
#include "sys.h"
#include "zone.h"

#ifdef GLQUAKE
#include "glquake.h"
#else
#include "d_iface.h"
#include "render.h"
#endif

/*
=================
Mod_LoadSpriteFrame
=================
*/
static void *
Mod_LoadSpriteFrame(void *pin, mspriteframe_t **ppframe, const char *loadname,
		    int framenum)
{
    dspriteframe_t *pinframe;
    mspriteframe_t *pspriteframe;
    int width, height, numpixels, size, origin[2];

    pinframe = (dspriteframe_t *)pin;

    width = LittleLong(pinframe->width);
    height = LittleLong(pinframe->height);
    numpixels = width * height;
    size = sizeof(mspriteframe_t) + R_SpriteDataSize(numpixels);

    pspriteframe = Hunk_AllocName(size, loadname);
    memset(pspriteframe, 0, size);
    *ppframe = pspriteframe;

    pspriteframe->width = width;
    pspriteframe->height = height;
    origin[0] = LittleLong(pinframe->origin[0]);
    origin[1] = LittleLong(pinframe->origin[1]);

    pspriteframe->up = origin[1];
    pspriteframe->down = origin[1] - height;
    pspriteframe->left = origin[0];
    pspriteframe->right = width + origin[0];

    /* Let the renderer process the pixel data as needed */
    R_SpriteDataStore(pspriteframe, loadname, framenum, (byte *)(pinframe + 1));

    return (byte *)pinframe + sizeof(dspriteframe_t) + numpixels;
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
static void *
Mod_LoadSpriteGroup(void *pin, mspriteframe_t **ppframe, const char *loadname,
		    int framenum)
{
    dspritegroup_t *pingroup;
    mspritegroup_t *pspritegroup;
    int i, numframes;
    dspriteinterval_t *pin_intervals;
    float *poutintervals;
    void *ptemp;

    pingroup = (dspritegroup_t *)pin;
    numframes = LittleLong(pingroup->numframes);

    pspritegroup = Hunk_AllocName(sizeof(*pspritegroup) +
				  numframes * sizeof(pspritegroup->frames[0]),
				  loadname);

    pspritegroup->numframes = numframes;
    *ppframe = (mspriteframe_t *)pspritegroup;
    pin_intervals = (dspriteinterval_t *)(pingroup + 1);
    poutintervals = Hunk_AllocName(numframes * sizeof(float), loadname);
    pspritegroup->intervals = poutintervals;

    for (i = 0; i < numframes; i++) {
	*poutintervals = LittleFloat(pin_intervals->interval);
	if (*poutintervals <= 0.0)
	    Sys_Error("%s: interval <= 0", __func__);

	poutintervals++;
	pin_intervals++;
    }

    ptemp = (void *)pin_intervals;

    for (i = 0; i < numframes; i++) {
	ptemp = Mod_LoadSpriteFrame(ptemp, &pspritegroup->frames[i], loadname,
				    framenum * 100 + i);
    }

    return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
void
Mod_LoadSpriteModel(model_t *model, void *buffer)
{
    char hunkname[HUNK_NAMELEN];
    int i;
    int version;
    dsprite_t *pin;
    msprite_t *psprite;
    int numframes;
    int size;
    dspriteframetype_t *pframetype;

    pin = (dsprite_t *)buffer;

    version = LittleLong(pin->version);
    if (version != SPRITE_VERSION)
	Sys_Error("%s: %s has wrong version number (%i should be %i)",
		  __func__, model->name, version, SPRITE_VERSION);

    numframes = LittleLong(pin->numframes);
    size = sizeof(*psprite) + numframes * sizeof(psprite->frames[0]);
    COM_FileBase(model->name, hunkname, sizeof(hunkname));
    psprite = Hunk_AllocName(size, hunkname);
    model->cache.data = psprite;

    psprite->type = LittleLong(pin->type);
    psprite->maxwidth = LittleLong(pin->width);
    psprite->maxheight = LittleLong(pin->height);
    psprite->beamlength = LittleFloat(pin->beamlength);
    model->synctype = LittleLong(pin->synctype);
    psprite->numframes = numframes;

    model->mins[0] = model->mins[1] = -psprite->maxwidth / 2;
    model->maxs[0] = model->maxs[1] = psprite->maxwidth / 2;
    model->mins[2] = -psprite->maxheight / 2;
    model->maxs[2] = psprite->maxheight / 2;

//
// load the frames
//
    if (numframes < 1)
	Sys_Error("%s: Invalid # of frames: %d", __func__, numframes);

    model->numframes = numframes;
    model->flags = 0;

    pframetype = (dspriteframetype_t *)(pin + 1);

    for (i = 0; i < numframes; i++) {
	spriteframetype_t frametype;

	frametype = LittleLong(pframetype->type);
	psprite->frames[i].type = frametype;

	if (frametype == SPR_SINGLE) {
	    pframetype = (dspriteframetype_t *)
		Mod_LoadSpriteFrame(pframetype + 1,
				    &psprite->frames[i].frameptr, hunkname, i);
	} else {
	    pframetype = (dspriteframetype_t *)
		Mod_LoadSpriteGroup(pframetype + 1,
				    &psprite->frames[i].frameptr, hunkname, i);
	}
    }

    model->type = mod_sprite;
}

/*
==================
Mod_GetSpriteFrame
==================
*/
mspriteframe_t *
Mod_GetSpriteFrame(const entity_t *e, msprite_t *psprite, float time)
{
    mspritegroup_t *pspritegroup;
    mspriteframe_t *pspriteframe;
    int i, numframes, frame;
    float *pintervals, fullinterval, targettime;

    frame = e->frame;
    if ((frame >= psprite->numframes) || (frame < 0)) {
	Con_Printf("R_DrawSprite: no such frame %d\n", frame);
	frame = 0;
    }

    if (psprite->frames[frame].type == SPR_SINGLE) {
	pspriteframe = psprite->frames[frame].frameptr;
    } else {
	pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
	pintervals = pspritegroup->intervals;
	numframes = pspritegroup->numframes;
	fullinterval = pintervals[numframes - 1];

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval
	// values are positive, so we don't have to worry about division by 0
	targettime = time - ((int)(time / fullinterval)) * fullinterval;

	for (i = 0; i < (numframes - 1); i++) {
	    if (pintervals[i] > targettime)
		break;
	}
	pspriteframe = pspritegroup->frames[i];
    }

    return pspriteframe;
}
