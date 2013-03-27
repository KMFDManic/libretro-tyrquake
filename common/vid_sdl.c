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

#include <stdlib.h>

#include "SDL.h"

#include "cdaudio.h"
#include "cmd.h"
#include "common.h"
#include "console.h"
#include "cvar.h"
#include "d_iface.h"
#include "d_local.h"
#include "draw.h"
#include "input.h"
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "screen.h"
#include "sdl_common.h"
#include "sound.h"
#include "sys.h"
#include "vid.h"
#include "view.h"
#include "wad.h"

#ifdef _WIN32
#include "winquake.h"
#endif

#ifdef NQ_HACK
#include "host.h"
#endif
#ifdef QW_HACK
#include "client.h"
#endif

// FIXME: evil hack to get full DirectSound support with SDL
#ifdef _WIN32
#include <windows.h>
HWND mainwindow;
qboolean DDActive = false;
#endif

static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_PixelFormat *sdl_format = NULL;
static SDL_PixelFormat *sdl_desktop_format = NULL;

/* ------------------------------------------------------------------------- */

/*
 * Stuff adapted from vid_win.c below... refactor so SDL works for other
 * platforms...
 */

static byte *vid_surfcache;
static int vid_surfcachesize;
static int VID_highhunkmark;
static qboolean hide_window;
//static int window_x, window_y;
static int window_width, window_height;
static int firstupdate = 1;

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
viddef_t vid; /* global video state */

#ifdef _WIN32
static HICON hIcon;
RECT window_rect;
#endif

int window_center_x, window_center_y;
static int window_x, window_y;

/* Placeholders */
void VID_ForceLockState(int lk) { }
int VID_ForceUnlockedAndReturnState(void) { return 0; }
void VID_Shutdown(void)
{
    if (renderer)
	SDL_DestroyRenderer(renderer);
    if (sdl_window)
	SDL_DestroyWindow(sdl_window);
    if (sdl_format)
	SDL_FreeFormat(sdl_format);
    if (sdl_desktop_format)
	SDL_FreeFormat(sdl_desktop_format);
}

static qboolean palette_changed;

void
VID_ShiftPalette(unsigned char *palette)
{
    VID_SetPalette(palette);
}

void VID_SetDefaultMode(void) { }
static qboolean VID_SetWindowedMode(int modenum) { return true; }
static qboolean VID_FullScreenMode(int modenum) { return true; }

#define MAX_MODE_LIST	200
#define VID_ROW_SIZE	3

#define VID_MODE_NONE			(-1)
#define VID_MODE_WINDOWED		0
#define NUM_WINDOWED_MODES		5

/* so this can be "stringified" -> (VID_MODE_WINDOWED+NUM_WINDOWED_MODES) */
#define VID_MODE_FULLSCREEN_DEFAULT	5

static cvar_t vid_mode = {
    .name = "vid_mode",
    .string = stringify(VID_MODE_WINDOWED),
    .archive = false
};
static cvar_t _vid_default_mode = {
    .name = "_vid_default_mode",
    .string = stringify(VID_MODE_WINDOWED),
    .archive = true
};
static cvar_t _vid_default_mode_win = {
    .name = "_vid_default_mode_win",
    .string = stringify(VID_MODE_FULLSCREEN_DEFAULT),
    .archive = true
};
static cvar_t vid_fullscreen_mode = {
    .name = "vid_fullscreen_mode",
    .string = stringify(VID_MODE_FULLSCREEN_DEFAULT),
    .archive = true
};
static cvar_t vid_windowed_mode = {
    .name = "vid_windowed_mode",
    .string = stringify(VID_MODE_WINDOWED),
    .archive = true
};

static cvar_t vid_wait = { "vid_wait", "0" };
static cvar_t vid_nopageflip = { "vid_nopageflip", "0", true };
static cvar_t _vid_wait_override = { "_vid_wait_override", "0", true };
static cvar_t block_switch = { "block_switch", "0", true };
static cvar_t vid_window_x = { "vid_window_x", "0", true };
static cvar_t vid_window_y = { "vid_window_y", "0", true };

typedef struct {
    int width;
    int height;
} lmode_t;

static lmode_t lowresmodes[] = {
    {320, 200},
    {320, 240},
    {400, 300},
    {512, 384},
};

static int windowed_default;
static int vid_default = VID_MODE_WINDOWED;
static int vid_modenum = VID_MODE_NONE;
static int vid_testingmode, vid_realmode;
static double vid_testendtime;

typedef struct {
    int width;
    int height;
    int modenum;
    int fullscreen;
    int bpp;
    int refresh;
    char modedesc[13];
    typeof(SDL_PIXELFORMAT_UNKNOWN) format;
} vmode_t;

static vmode_t modelist[MAX_MODE_LIST];
static int nummodes;

static vmode_t badmode;

static void VID_MenuDraw(void);
static void VID_MenuKey(int key);
static int VID_SetMode(int modenum, unsigned char *palette);
static char *VID_GetModeDescription(int mode);
static char *VID_GetModeDescription2(int mode);
static char *VID_GetModeDescriptionMemCheck(int mode);
static int VID_NumModes(void);
static vmode_t *VID_GetModePtr(int modenum);

static qboolean Minimized;
static qboolean force_mode_set;

qboolean
window_visible(void)
{
    return !Minimized;
}

static void
VID_GetWindowSize(int default_width, int default_height)
{

}

static void
InitMode(vmode_t *mode, int num, int fullscreen, int width, int height)
{
    mode->modenum = num;
    mode->fullscreen = fullscreen;
    mode->bpp = sdl_desktop_format->BitsPerPixel;
    mode->format = sdl_desktop_format->format;
    mode->width = width;
    mode->height = height;
    snprintf(mode->modedesc, sizeof(mode->modedesc), "%dx%d", width, height);
}

static void
VID_InitModeList(void)
{
    int i, err;
    int displays, sdlmodes;
    SDL_DisplayMode mode;

    displays = SDL_GetNumVideoDisplays();
    if (displays < 1)
	Sys_Error("%s: no displays found (%s)", __func__, SDL_GetError());

    /* FIXME - allow use of more than one display */
    sdlmodes = SDL_GetNumDisplayModes(0);
    if (sdlmodes < 0)
	Con_SafePrintf("%s: error enumerating SDL display modes (%s)\n",
		       __func__, SDL_GetError());

    nummodes = 0;

    InitMode(&modelist[0], 0, 0, 320, 240);
    InitMode(&modelist[1], 1, 0, 640, 480);
    InitMode(&modelist[2], 2, 0, 800, 600);
    InitMode(&modelist[3], 3, 0, 1024, 768);
    InitMode(&modelist[4], 4, 0, 1280, 960);
    nummodes = 5;

    /*
     * Check availability of fullscreen modes
     * (default to display 0 for now)
     */
    for (i = 0; i < sdlmodes && nummodes < MAX_MODE_LIST; i++) {
	err = SDL_GetDisplayMode(0, i, &mode);
	if (err)
	    Sys_Error("%s: couldn't get mode %d info (%s)",
		      __func__, i, SDL_GetError());

	printf("%s: checking mode %i: %dx%d, %s\n", __func__,
	       i, mode.w, mode.h, SDL_GetPixelFormatName(mode.format));

	if (mode.h > MAXHEIGHT)
	    continue;

	if (SDL_PIXELTYPE(mode.format) == SDL_PIXELTYPE_PACKED32)
	    modelist[nummodes].bpp = 32;
	else if (SDL_PIXELTYPE(mode.format) == SDL_PIXELTYPE_PACKED16)
	    modelist[nummodes].bpp = 16;
	else
	    continue;

	modelist[nummodes].modenum = nummodes;
	modelist[nummodes].fullscreen = 1;
	modelist[nummodes].width = mode.w;
	modelist[nummodes].height = mode.h;
	modelist[nummodes].refresh = mode.refresh_rate;
	modelist[nummodes].format = mode.format;
	sprintf(modelist[nummodes].modedesc, "%dx%d", mode.w, mode.h);
	nummodes++;
    }
}

static int vid_line;
static int vid_wmodes;

typedef struct {
    int modenum;
    char *desc;
    int iscur;
    int width;
    int height;
} modedesc_t;

#define MAX_COLUMN_SIZE		5
#define MODE_AREA_HEIGHT	(MAX_COLUMN_SIZE + 7)
#define MAX_MODEDESCS		(MAX_COLUMN_SIZE * 3 + NUM_WINDOWED_MODES)

static modedesc_t modedescs[MAX_MODEDESCS];

/*
================
VID_MenuDraw
================
*/
static void
VID_MenuDraw(void)
{
    const qpic_t *p;
    char *ptr;
    int lnummodes, i, j, k, column, row, dup, dupmode;
    char temp[100];
    vmode_t *pv;
    modedesc_t tmodedesc;

    p = Draw_CachePic("gfx/vidmodes.lmp");
    M_DrawPic((320 - p->width) / 2, 4, p);

    for (i = 0; i < NUM_WINDOWED_MODES; i++) {
	ptr = VID_GetModeDescriptionMemCheck(i);
	modedescs[i].modenum = modelist[i].modenum;
	modedescs[i].desc = ptr;
	modedescs[i].iscur = 0;

	if (vid_modenum == i)
	    modedescs[i].iscur = 1;
    }

    vid_wmodes = NUM_WINDOWED_MODES;
    lnummodes = VID_NumModes();

    dupmode = 0;	// FIXME - uninitialized -> guesssing 0

    for (i = VID_MODE_FULLSCREEN_DEFAULT; i < lnummodes; i++) {
	ptr = VID_GetModeDescriptionMemCheck(i);
	pv = VID_GetModePtr(i);

	// we only have room for 15 fullscreen modes, so don't allow
	// 360-wide modes, because if there are 5 320-wide modes and
	// 5 360-wide modes, we'll run out of space
	if (ptr && ((pv->width != 360) || COM_CheckParm("-allow360"))) {
	    dup = 0;
	    for (j = VID_MODE_FULLSCREEN_DEFAULT; j < vid_wmodes; j++) {
		if (!strcmp(modedescs[j].desc, ptr)) {
		    dup = 1;
		    dupmode = j;
		    break;
		}
	    }

	    if (dup || (vid_wmodes < MAX_MODEDESCS)) {
		if (!dup || COM_CheckParm("-noforcevga")) {
		    if (dup) {
			k = dupmode;
		    } else {
			k = vid_wmodes;
		    }

		    modedescs[k].modenum = i;
		    modedescs[k].desc = ptr;
		    modedescs[k].iscur = 0;
		    modedescs[k].width = pv->width;
		    modedescs[k].height = pv->height;

		    if (i == vid_modenum)
			modedescs[k].iscur = 1;

		    if (!dup)
			vid_wmodes++;
		}
	    }
	}
    }

    /*
     * Sort the modes on width & height
     * (to handle picking up oddball dibonly modes after all the others)
     */
    for (i = VID_MODE_FULLSCREEN_DEFAULT; i < (vid_wmodes - 1); i++) {
	for (j = (i + 1); j < vid_wmodes; j++) {
	    if (modedescs[i].width > modedescs[j].width	||
		(modedescs[i].width == modedescs[j].width &&
		 modedescs[i].height > modedescs[j].height)) {
		tmodedesc = modedescs[i];
		modedescs[i] = modedescs[j];
		modedescs[j] = tmodedesc;
	    }
	}
    }
    M_Print(13 * 8, 36, "Windowed Modes");

    column = 16;
    row = 36 + 2 * 8;

    for (i = 0; i < NUM_WINDOWED_MODES; i++) {
	if (modedescs[i].iscur)
	    M_PrintWhite(column, row, modedescs[i].desc);
	else
	    M_Print(column, row, modedescs[i].desc);

	column += 13 * 8;
	if (!((i + 1) % VID_ROW_SIZE)) {
	    column = 16;
	    row += 8;
	}
    }
    /* go to next row if previous row not filled */
    if (NUM_WINDOWED_MODES % VID_ROW_SIZE)
	row += 8;

    if (vid_wmodes > NUM_WINDOWED_MODES) {
	M_Print(12 * 8, row + 8, "Fullscreen Modes");

	column = 16;
	row += 3 * 8;

	for (i = VID_MODE_FULLSCREEN_DEFAULT; i < vid_wmodes; i++) {
	    if (modedescs[i].iscur)
		M_PrintWhite(column, row, modedescs[i].desc);
	    else
		M_Print(column, row, modedescs[i].desc);

	    column += 13 * 8;
	    if (!((i - NUM_WINDOWED_MODES + 1) % VID_ROW_SIZE)) {
		column = 16;
		row += 8;
	    }
	}
    }

    /* line cursor */
    if (vid_testingmode) {
	sprintf(temp, "TESTING %s", modedescs[vid_line].desc);
	M_Print(13 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 4, temp);
	M_Print(9 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 6,
		"Please wait 5 seconds...");
    } else {
	M_Print(9 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8,
		"Press Enter to set mode");
	M_Print(6 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 3,
		"T to test mode for 5 seconds");
	ptr = VID_GetModeDescription2(vid_modenum);

	if (ptr) {
	    sprintf(temp, "D to set default: %s", ptr);
	    M_Print(2 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 5, temp);
	}

	ptr = VID_GetModeDescription2((int)_vid_default_mode_win.value);
	if (ptr) {
	    sprintf(temp, "Current default: %s", ptr);
	    M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 6, temp);
	}

	M_Print(15 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 8, "Esc to exit");

	if (vid_line < NUM_WINDOWED_MODES) {
	    row = 36 + 2 * 8 + (vid_line / VID_ROW_SIZE) * 8;
	    column = 8 + (vid_line % VID_ROW_SIZE) * 13 * 8;
	} else {
	    row = 36 + (5 + (NUM_WINDOWED_MODES + 2) / VID_ROW_SIZE) * 8;
	    row += ((vid_line - NUM_WINDOWED_MODES) / VID_ROW_SIZE) * 8;
	    column = 8 + ((vid_line - NUM_WINDOWED_MODES) % VID_ROW_SIZE) *
		13 * 8;
	}
	M_DrawCharacter(column, row, 12 + ((int)(realtime * 4) & 1));
    }
}

/*
================
VID_MenuKey
================
*/
static void
VID_MenuKey(int key)
{
    if (vid_testingmode)
	return;

    switch (key) {
    case K_ESCAPE:
	S_LocalSound("misc/menu1.wav");
	M_Menu_Options_f();
	break;

    case K_LEFTARROW:
	S_LocalSound("misc/menu1.wav");
	if (vid_line < NUM_WINDOWED_MODES) {
	    if (!vid_line) {
		vid_line = VID_ROW_SIZE - 1;
	    } else if (vid_line % VID_ROW_SIZE) {
		vid_line -= 1;
	    } else {
		vid_line += VID_ROW_SIZE - 1;
		if (vid_line >= NUM_WINDOWED_MODES)
		    vid_line = NUM_WINDOWED_MODES - 1;
	    }
	} else if ((vid_line - NUM_WINDOWED_MODES) % VID_ROW_SIZE) {
	    vid_line -= 1;
	} else {
	    vid_line += VID_ROW_SIZE - 1;
	    if (vid_line >= vid_wmodes)
		vid_line = vid_wmodes - 1;
	}
	break;

    case K_RIGHTARROW:
	S_LocalSound("misc/menu1.wav");
	if (vid_line < NUM_WINDOWED_MODES) {
	    if ((vid_line + 1) % VID_ROW_SIZE) {
		vid_line += 1;
		if (vid_line >= NUM_WINDOWED_MODES)
		    vid_line = ((NUM_WINDOWED_MODES - 1) / VID_ROW_SIZE) *
			VID_ROW_SIZE;
	    } else {
		vid_line -= VID_ROW_SIZE - 1;
	    }
	} else if ((vid_line - NUM_WINDOWED_MODES + 1) % VID_ROW_SIZE) {
	    vid_line += 1;
	    if (vid_line >= vid_wmodes)
		vid_line = ((vid_line - NUM_WINDOWED_MODES) / VID_ROW_SIZE) *
		    VID_ROW_SIZE + NUM_WINDOWED_MODES;
	} else {
	    vid_line -= VID_ROW_SIZE - 1;
	}
	break;

    case K_UPARROW:
	S_LocalSound("misc/menu1.wav");
	if (vid_line < NUM_WINDOWED_MODES + VID_ROW_SIZE &&
	    vid_line >= NUM_WINDOWED_MODES) {
	    /* Going from fullscreen section to windowed section */
	    vid_line -= NUM_WINDOWED_MODES % VID_ROW_SIZE;
	    while (vid_line >= NUM_WINDOWED_MODES)
		vid_line -= VID_ROW_SIZE;
	} else if (vid_line < VID_ROW_SIZE) {
	    /* From top to bottom */
	    vid_line += (vid_wmodes / VID_ROW_SIZE + 1) * VID_ROW_SIZE;
	    vid_line += NUM_WINDOWED_MODES % VID_ROW_SIZE;
	    while (vid_line >= vid_wmodes)
		vid_line -= VID_ROW_SIZE;
	} else {
	    vid_line -= VID_ROW_SIZE;
	}
	break;

    case K_DOWNARROW:
	S_LocalSound("misc/menu1.wav");
	if (vid_line < NUM_WINDOWED_MODES &&
	    vid_line + VID_ROW_SIZE >= NUM_WINDOWED_MODES) {
	    /* windowed to fullscreen section */
	    vid_line = NUM_WINDOWED_MODES + (vid_line % VID_ROW_SIZE);
	} else if (vid_line + VID_ROW_SIZE >= vid_wmodes) {
	    /* bottom to top */
	    vid_line = (vid_line - NUM_WINDOWED_MODES) % VID_ROW_SIZE;
	} else {
	    vid_line += VID_ROW_SIZE;
	}
	break;

    case K_ENTER:
	S_LocalSound("misc/menu1.wav");
	VID_SetMode(modedescs[vid_line].modenum, host_basepal);
	break;

    case 'T':
    case 't':
	S_LocalSound("misc/menu1.wav");
	// have to set this before setting the mode because WM_PAINT
	// happens during the mode set and does a VID_Update, which
	// checks vid_testingmode
	vid_testingmode = 1;
	vid_testendtime = realtime + 5.0;

	if (!VID_SetMode(modedescs[vid_line].modenum, host_basepal)) {
	    vid_testingmode = 0;
	}
	break;

    case 'D':
    case 'd':
	S_LocalSound("misc/menu1.wav");
	firstupdate = 0;
	Cvar_SetValue("_vid_default_mode_win", vid_modenum);
	break;

    default:
	break;
    }
}

/*
================
ClearAllStates
================
*/
static void
ClearAllStates(void)
{
    int i;

    // send an up event for each key, to make sure the server clears them all
    for (i = 0; i < 256; i++)
	Key_Event(i, false);

    Key_ClearStates();
    //IN_ClearStates();
}

/*
====================
VID_CheckAdequateMem
====================
*/
static qboolean
VID_CheckAdequateMem(int width, int height)
{
    int tbuffersize;

    tbuffersize = width * height * sizeof(*d_pzbuffer);
    tbuffersize += D_SurfaceCacheForRes(width, height);

    /*
     * see if there's enough memory, allowing for the normal mode 0x13 pixel,
     * z, and surface buffers
     */
    if ((host_parms.memsize - tbuffersize + SURFCACHE_SIZE_AT_320X200 +
	 0x10000 * 3) < minimum_memory)
	return false;

    return true;
}


/*
================
VID_AllocBuffers
================
*/
static qboolean
VID_AllocBuffers(int width, int height)
{
    int tsize, tbuffersize;

    tsize = D_SurfaceCacheForRes(width, height);
    tbuffersize = width * height * sizeof(*d_pzbuffer);
    tbuffersize += tsize;

    /*
     * see if there's enough memory, allowing for the normal mode 0x13 pixel,
     * z, and surface buffers
     */
    if ((host_parms.memsize - tbuffersize + SURFCACHE_SIZE_AT_320X200 +
	 0x10000 * 3) < minimum_memory) {
	Con_SafePrintf("Not enough memory for video mode\n");
	return false;
    }

    vid_surfcachesize = tsize;

    if (d_pzbuffer) {
	D_FlushCaches();
	Hunk_FreeToHighMark(VID_highhunkmark);
	d_pzbuffer = NULL;
    }

    VID_highhunkmark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(tbuffersize, "video");
    vid_surfcache = (byte *)d_pzbuffer + width * height * sizeof(*d_pzbuffer);

    return true;
}

/*
=================
VID_NumModes
=================
*/
static int
VID_NumModes(void)
{
    return nummodes;
}


/*
=================
VID_GetModePtr
=================
*/
static vmode_t *
VID_GetModePtr(int modenum)
{
    if ((modenum >= 0) && (modenum < nummodes))
	return &modelist[modenum];
    else
	return &badmode;
}

/*
=================
VID_GetModeDescription
=================
*/
static char *
VID_GetModeDescription(int mode)
{
    char *pinfo;
    vmode_t *pv;

    if ((mode < 0) || (mode >= nummodes))
	return NULL;

    pv = VID_GetModePtr(mode);
    pinfo = pv->modedesc;
    return pinfo;
}

/*
=================
VID_GetModeDescription2

Tacks on "windowed" or "fullscreen"
=================
*/
static char *
VID_GetModeDescription2(int mode)
{
    static char pinfo[40];
    vmode_t *pv;

    if ((mode < 0) || (mode >= nummodes))
	return NULL;

    pv = VID_GetModePtr(mode);

    if (modelist[mode].fullscreen) {
	sprintf(pinfo, "%4d x %4d x %2d @ %3dHz", pv->width, pv->height,
		pv->bpp, pv->refresh);
    } else {
	sprintf(pinfo, "%4d x %4d windowed", pv->width, pv->height);
    }

    return pinfo;
}

/*
=================
VID_GetModeDescriptionMemCheck
=================
*/
static char *
VID_GetModeDescriptionMemCheck(int mode)
{
    char *pinfo;
    vmode_t *pv;

    if ((mode < 0) || (mode >= nummodes))
	return NULL;

    pv = VID_GetModePtr(mode);
    pinfo = pv->modedesc;

    if (VID_CheckAdequateMem(pv->width, pv->height)) {
	return pinfo;
    } else {
	return NULL;
    }
}

/*
=================
VID_DescribeModes_f
=================
*/
static void
VID_DescribeModes_f(void)
{
    int i, lnummodes;
    char *pinfo;
    qboolean na;
    vmode_t *pv;

    na = false;

    lnummodes = VID_NumModes();

    for (i = 0; i < lnummodes; i++) {
	pv = VID_GetModePtr(i);
	pinfo = VID_GetModeDescription2(i);

	if (VID_CheckAdequateMem(pv->width, pv->height)) {
	    Con_Printf("%2d: %s\n", i, pinfo);
	} else {
	    Con_Printf("**: %s\n", pinfo);
	    na = true;
	}
    }

    if (na) {
	Con_Printf("\n[**: not enough system RAM for mode]\n");
    }
}

qboolean
VID_IsFullScreen()
{
    return VID_GetModePtr(vid_modenum)->fullscreen;
}

/*
================
VID_UpdateWindowStatus
================
*/
static void
VID_UpdateWindowStatus(void)
{
#ifdef _WIN32
    window_rect.left = window_x;
    window_rect.top = window_y;
    window_rect.right = window_x + window_width;
    window_rect.bottom = window_y + window_height;
    window_center_x = (window_rect.left + window_rect.right) / 2;
    window_center_y = (window_rect.top + window_rect.bottom) / 2;

    IN_UpdateClipCursor();
#endif
}

static int
VID_SetMode(int modenum, unsigned char *palette)
{
    vmode_t *mode;
    int w, h;
    Uint32 flags;
    qboolean mouse_grab;

    /* FIXME - hack to reset mouse grabs */
    mouse_grab = _windowed_mouse.value;
    if (mouse_grab) {
	_windowed_mouse.value = 0;
	_windowed_mouse.callback(&_windowed_mouse);
    }

    mode = VID_GetModePtr(modenum);
    w = mode->width;
    h = mode->height;
    flags = SDL_WINDOW_SHOWN;
    if (mode->fullscreen)
	flags |= SDL_WINDOW_FULLSCREEN;

    if (renderer)
	SDL_DestroyRenderer(renderer);
    if (sdl_window)
	SDL_DestroyWindow(sdl_window);
    if (sdl_format)
	SDL_FreeFormat(sdl_format);

    sdl_format = SDL_AllocFormat(mode->format);

    sdl_window = SDL_CreateWindow("TyrQuake",
				  SDL_WINDOWPOS_UNDEFINED,
				  SDL_WINDOWPOS_UNDEFINED,
				  w, h, flags);
    if (!sdl_window)
	Sys_Error("%s: Unable to create window: %s", __func__, SDL_GetError());

    renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
	Sys_Error("%s: Unable to create renderer: %s", __func__, SDL_GetError());

    texture = SDL_CreateTexture(renderer,
				mode->format,
				SDL_TEXTUREACCESS_STREAMING,
				w, h);
    if (!texture)
	Sys_Error("%s: Unable to create texture: %s", __func__, SDL_GetError());

    //VID_InitGamma(palette);
    VID_SetPalette(palette);

    vid.numpages = 1;
    vid.width = vid.conwidth = w;
    vid.height = vid.conheight = h;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    VID_AllocBuffers(vid.width, vid.height);

    // In-memory buffer which we upload via SDL texture
    vid.buffer = vid.conbuffer = vid.direct = Hunk_HighAllocName(vid.width * vid.height, "vidbuf");
    vid.rowbytes = vid.conrowbytes = vid.width;

    D_InitCaches(vid_surfcache, vid_surfcachesize);

    window_width = vid.width;
    window_height = vid.height;
    VID_UpdateWindowStatus();

    vid_modenum = modenum;
    Cvar_SetValue("vid_mode", (float)vid_modenum);

    vid.recalc_refdef = 1;

#ifdef _WIN32
    mainwindow=GetActiveWindow();
    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)hIcon);
    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)hIcon);
#endif

    /* FIXME - hack to reset mouse grabs */
    if (mouse_grab) {
	_windowed_mouse.value = 1;
	_windowed_mouse.callback(&_windowed_mouse);
    }

    return true;
}

/* ------------------------------------------------------------------------- */

// The original defaults
#define BASEWIDTH 320
#define BASEHEIGHT 200

byte *VGA_pagebase;
int VGA_width, VGA_height, VGA_rowbytes, VGA_bufferrowbytes = 0;

void
VID_SetPalette(unsigned char *palette)
{
    unsigned i, r, g, b;

    switch (SDL_PIXELTYPE(sdl_format->format)) {
    case SDL_PIXELTYPE_PACKED32:
	for (i = 0; i < 256; i++) {
	    r = palette[0];
	    g = palette[1];
	    b = palette[2];
	    palette += 3;
	    d_8to24table[i] = SDL_MapRGB(sdl_format, r, g, b);
	}
	break;
    case SDL_PIXELTYPE_PACKED16:
	for (i = 0; i < 256; i++) {
	    r = palette[0];
	    g = palette[1];
	    b = palette[2];
	    palette += 3;
	    d_8to16table[i] = SDL_MapRGB(sdl_format, r, g, b);
	}
	break;
    default:
	Sys_Error("%s: unsupported pixel format (%s)", __func__,
		  SDL_GetPixelFormatName(sdl_format->format));
    }

    palette_changed = true;
}

static void
do_screen_buffer(void)
{
}

void
VID_Init(unsigned char *palette) /* (byte *palette, byte *colormap) */
{
    int err;
    SDL_DisplayMode desktop_mode;

    Cvar_RegisterVariable(&vid_mode);
    Cvar_RegisterVariable(&vid_wait);
    Cvar_RegisterVariable(&vid_nopageflip);
    Cvar_RegisterVariable(&_vid_wait_override);
    Cvar_RegisterVariable(&_vid_default_mode);
    Cvar_RegisterVariable(&_vid_default_mode_win);
    Cvar_RegisterVariable(&vid_fullscreen_mode);
    Cvar_RegisterVariable(&vid_windowed_mode);
    Cvar_RegisterVariable(&block_switch);
    Cvar_RegisterVariable(&vid_window_x);
    Cvar_RegisterVariable(&vid_window_y);

    Cmd_AddCommand("vid_describemodes", VID_DescribeModes_f);

    /*
     * Init SDL and the video subsystem
     */
    Q_SDL_InitOnce();
    err = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (err < 0)
	Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());

    err = SDL_GetDesktopDisplayMode(0, &desktop_mode);
    if (err)
	Sys_Error("%s: Unable to query desktop display mode (%s)",
		  __func__, SDL_GetError());
    sdl_desktop_format = SDL_AllocFormat(desktop_mode.format);
    if (!sdl_desktop_format)
	Sys_Error("%s: Unable to allocate desktop pixel format (%s)",
		  __func__, SDL_GetError());

    VID_InitModeList();

    VID_SetMode(0, palette);

#ifdef _WIN32
    mainwindow=GetActiveWindow();
    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)hIcon);
    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)hIcon);
#endif

    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;
}

void
VID_Update(vrect_t *rects)
{
    SDL_Rect subrect;
    int i;
    vrect_t *rect;
    vrect_t fullrect;
    byte *src;
    void *dst;
    Uint32 *dst32;
    Uint16 *dst16;
    int pitch;
    int height;
    int err;

    /*
     * Check for vid_mode changes
     */
    if ((int)vid_mode.value != vid_modenum) {
	VID_SetMode((int)vid_mode.value, host_basepal);
	/* FIXME - not the right place! redraw the scene to buffer first */
	return;
    }

    /*
     * If the palette changed, refresh the whole screen
     */
    if (palette_changed) {
	palette_changed = false;
	fullrect.x = 0;
	fullrect.y = 0;
	fullrect.width = vid.width;
	fullrect.height = vid.height;
	fullrect.pnext = NULL;
	rects = &fullrect;
    }

    for (rect = rects; rect; rect = rect->pnext) {
	subrect.x = rect->x;
	subrect.y = rect->y;
	subrect.w = rect->width;
	subrect.h = rect->height;

	err = SDL_LockTexture(texture, &subrect, (void **)&dst, &pitch);
	if (err)
	    Sys_Error("%s: unable to lock texture (%s)",
		      __func__, SDL_GetError());
	src = vid.buffer + rect->y * vid.width + rect->x;
	height = subrect.h;
	switch (SDL_PIXELTYPE(sdl_format->format)) {
	case SDL_PIXELTYPE_PACKED32:
	    dst32 = dst;
	    while (height--) {
		for (i = 0; i < rect->width; i++)
		    dst32[i] = d_8to24table[src[i]];
		dst32 += pitch / sizeof(*dst32);
		src += vid.width;
	    }
	    break;
	case SDL_PIXELTYPE_PACKED16:
	    dst16 = dst;
	    while (height--) {
		for (i = 0; i < rect->width; i++)
		    dst16[i] = d_8to16table[src[i]];
		dst16 += pitch / sizeof(*dst16);
		src += vid.width;
	    }
	    break;
	default:
	    Sys_Error("%s: unsupported pixel format (%s)", __func__,
		      SDL_GetPixelFormatName(sdl_format->format));
	}
	SDL_UnlockTexture(texture);
    }
    err = SDL_RenderCopy(renderer, texture, NULL, NULL);
    if (err)
	Sys_Error("%s: unable to render texture (%s)", __func__, SDL_GetError());
    SDL_RenderPresent(renderer);
}

void
D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
    int err, i;
    const byte *src;
    unsigned *dst;
    int pitch;
    SDL_Rect subrect;

    if (!texture || !renderer)
	return;

    subrect.x = (x < 0) ? vid.width + x - 1 : x;
    subrect.y = y;
    subrect.w = width;
    subrect.h = height;

    err = SDL_LockTexture(texture, &subrect, (void **)&dst, &pitch);
    if (err)
	Sys_Error("%s: unable to lock texture (%s)", __func__, SDL_GetError());
    src = pbitmap;
    while (height--) {
	for (i = 0; i < width; i++)
	    dst[i] = d_8to24table[src[i]];
	dst += pitch / sizeof(*dst);
	src += width;
    }
    SDL_UnlockTexture(texture);

    err = SDL_RenderCopy(renderer, texture, NULL, NULL);
    if (err)
	Sys_Error("%s: unable to render texture (%s)", __func__, SDL_GetError());
    SDL_RenderPresent(renderer);
}

void
D_EndDirectRect(int x, int y, int width, int height)
{
    int err, i;
    byte *src;
    unsigned *dst;
    int pitch;
    SDL_Rect subrect;

    if (!texture || !renderer)
	return;

    subrect.x = (x < 0) ? vid.width + x - 1 : x;
    subrect.y = y;
    subrect.w = width;
    subrect.h = height;

    err = SDL_LockTexture(texture, &subrect, (void **)&dst, &pitch);
    if (err)
	Sys_Error("%s: unable to lock texture (%s)", __func__, SDL_GetError());
    src = vid.buffer + y * vid.width + subrect.x;
    while (height--) {
	for (i = 0; i < width; i++)
	    dst[i] = d_8to24table[src[i]];
	dst += pitch / sizeof(*dst);
	src += vid.width;
    }
    SDL_UnlockTexture(texture);

    err = SDL_RenderCopy(renderer, texture, NULL, NULL);
    if (err)
	Sys_Error("%s: unable to render texture (%s)", __func__, SDL_GetError());
    SDL_RenderPresent(renderer);
}

void
VID_LockBuffer(void)
{
}

void
VID_UnlockBuffer(void)
{
}

#ifndef _WIN32
void
Sys_SendKeyEvents(void)
{
    IN_ProcessEvents();
}
#endif
