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

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>

#include "cdaudio.h"
#include "cmd.h"
#include "console.h"
#include "draw.h"
#include "glquake.h"
#include "input.h"
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "resource.h"
#include "sbar.h"
#include "screen.h"
#include "sound.h"
#include "sys.h"
#include "vid.h"
#include "wad.h"
#include "winquake.h"

#ifdef NQ_HACK
#include "host.h"
#endif

#define WARP_WIDTH	320
#define WARP_HEIGHT	200
#define MAXWIDTH	10000
#define MAXHEIGHT	10000
#define BASEWIDTH	320
#define BASEHEIGHT	200

qboolean VID_CheckAdequateMem(int width, int height) { return true; }

static qboolean Minimized;

qboolean
window_visible(void)
{
    return !Minimized;
}

static void VID_MenuDraw_WGL(void);
static void VID_MenuKey_WGL(int key);

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

const char *gl_renderer;
static const char *gl_vendor;
static const char *gl_version;
static const char *gl_extensions;

static int gl_num_texture_units;

qboolean DDActive;
qboolean scr_skipupdate;

static DEVMODE gdevmode;
static qboolean vid_initialized = false;
static qboolean windowed, leavecurrentmode;
static qboolean vid_canalttab = false;
static qboolean vid_wassuspended = false;
static int windowed_mouse;

static HICON hIcon;
static int DIBWidth, DIBHeight;
static RECT WindowRect;
static DWORD WindowStyle, ExWindowStyle;

HWND mainwindow;
static HWND dibwindow;

int vid_modenum = VID_MODE_NONE;
static int vid_default = VID_MODE_WINDOWED;
static qboolean fullsbardraw = false;

static float vid_gamma = 1.0;

static HGLRC baseRC;
static HDC maindc;

cvar_t gl_ztrick = { "gl_ztrick", "1" };

viddef_t vid;			// global video state

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
byte d_15to8table[65536];

float gldepthmin, gldepthmax;

static modestate_t modestate = MS_UNINIT;

static void AppActivate(BOOL fActive, BOOL minimize);
static LONG WINAPI MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
			       LPARAM lParam);

static void ClearAllStates(void);
static void VID_UpdateWindowStatus(void);

static void GL_Init(void);

// FIXME - Shouldn't use the exact names from the library?
#ifndef GL_VERSION_1_2
static PROC glArrayElementEXT;
static PROC glColorPointerEXT;
static PROC glTexCoordPointerEXT;
static PROC glVertexPointerEXT;
#endif

qboolean gl_mtexable = false;

//====================================

static cvar_t vid_mode = { "vid_mode", "0", false };

// Note that 0 is VID_MODE_WINDOWED
static cvar_t _vid_default_mode = { "_vid_default_mode", "0", true };

// Note that 5 is VID_MODE_FULLSCREEN_DEFAULT
static cvar_t _vid_default_mode_win = { "_vid_default_mode_win", "5", true };
static cvar_t vid_wait = { "vid_wait", "0" };
static cvar_t vid_nopageflip = { "vid_nopageflip", "0", true };
static cvar_t _vid_wait_override = { "_vid_wait_override", "0", true };
static cvar_t vid_config_x = { "vid_config_x", "800", true };
static cvar_t vid_config_y = { "vid_config_y", "600", true };
static cvar_t vid_stretch_by_2 = { "vid_stretch_by_2", "1", true };

cvar_t _windowed_mouse = { "_windowed_mouse", "1", true };

int window_center_x, window_center_y;

static int window_x, window_y;
static int window_width, window_height;

RECT window_rect;

// direct draw software compatability stuff

void
VID_ForceLockState(int lk)
{
}

void
VID_LockBuffer(void)
{
}

void
VID_UnlockBuffer(void)
{
}

int
VID_ForceUnlockedAndReturnState(void)
{
    return 0;
}

void
D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
}

void
D_EndDirectRect(int x, int y, int width, int height)
{
}


static void
CenterWindow(HWND hWndCenter, int width, int height, BOOL lefttopjustify)
{
    int CenterX, CenterY;

    CenterX = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    CenterY = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    if (CenterX > CenterY * 2)
	CenterX >>= 1;		// dual screens
    CenterX = qmax(CenterX, 0);
    CenterY = qmax(CenterY, 0);
    SetWindowPos(hWndCenter, NULL, CenterX, CenterY, 0, 0,
		 SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW | SWP_DRAWFRAME);
}

static qboolean
VID_SetWindowedMode(const qvidmode_t *mode)
{
    HDC hdc;
    int width, height;
    RECT rect;

    WindowRect.top = WindowRect.left = 0;

    WindowRect.right = mode->width;
    WindowRect.bottom = mode->height;

    DIBWidth = mode->width;
    DIBHeight = mode->height;

    WindowStyle = WS_OVERLAPPED | WS_BORDER | WS_CAPTION | WS_SYSMENU |
	WS_MINIMIZEBOX;
    ExWindowStyle = 0;

    rect = WindowRect;
    AdjustWindowRectEx(&rect, WindowStyle, FALSE, 0);

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    // Create the DIB window
    dibwindow = CreateWindowEx(ExWindowStyle,
			       "TyrQuake",
			       "TyrQuake",
			       WindowStyle,
			       rect.left, rect.top,
			       width,
			       height, NULL, NULL, global_hInstance, NULL);

    if (!dibwindow)
	Sys_Error("Couldn't create DIB window");

    // Center and show the DIB window
    CenterWindow(dibwindow, WindowRect.right - WindowRect.left,
		 WindowRect.bottom - WindowRect.top, false);

    ShowWindow(dibwindow, SW_SHOWDEFAULT);
    UpdateWindow(dibwindow);

    modestate = MS_WINDOWED;

// because we have set the background brush for the window to NULL
// (to avoid flickering when re-sizing the window on the desktop),
// we clear the window to black when created, otherwise it will be
// empty while Quake starts up.
    hdc = GetDC(dibwindow);
    PatBlt(hdc, 0, 0, WindowRect.right, WindowRect.bottom, BLACKNESS);
    ReleaseDC(dibwindow, hdc);

    if (vid.conheight > mode->height)
	vid.conheight = mode->height;
    if (vid.conwidth > mode->width)
	vid.conwidth = mode->width;
    vid.width = vid.conwidth;
    vid.height = vid.conheight;

    vid.numpages = 2;

    mainwindow = dibwindow;

    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)hIcon);
    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)hIcon);

    return true;
}


static qboolean
VID_SetFullDIBMode(const qvidmode_t *mode)
{
    HDC hdc;
    int width, height;
    RECT rect;

    if (!leavecurrentmode) {
	gdevmode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
	gdevmode.dmBitsPerPel = mode->bpp;
	gdevmode.dmPelsWidth = mode->width;
	gdevmode.dmPelsHeight = mode->height;
	gdevmode.dmSize = sizeof(gdevmode);

	if (ChangeDisplaySettings(&gdevmode, CDS_FULLSCREEN) !=
	    DISP_CHANGE_SUCCESSFUL)
	    Sys_Error("Couldn't set fullscreen DIB mode");
    }

    modestate = MS_FULLDIB;

    WindowRect.top = WindowRect.left = 0;
    WindowRect.right = mode->width;
    WindowRect.bottom = mode->height;

    DIBWidth = mode->width;
    DIBHeight = mode->height;

    WindowStyle = WS_POPUP;
    ExWindowStyle = 0;

    rect = WindowRect;
    AdjustWindowRectEx(&rect, WindowStyle, FALSE, 0);

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    // Create the DIB window
    dibwindow = CreateWindowEx(ExWindowStyle,
			       "TyrQuake",
			       "TyrQuake",
			       WindowStyle,
			       rect.left, rect.top,
			       width,
			       height, NULL, NULL, global_hInstance, NULL);

    if (!dibwindow)
	Sys_Error("Couldn't create DIB window");

    ShowWindow(dibwindow, SW_SHOWDEFAULT);
    UpdateWindow(dibwindow);

    // Because we have set the background brush for the window to NULL
    // (to avoid flickering when re-sizing the window on the desktop), we
    // clear the window to black when created, otherwise it will be
    // empty while Quake starts up.
    hdc = GetDC(dibwindow);
    PatBlt(hdc, 0, 0, WindowRect.right, WindowRect.bottom, BLACKNESS);
    ReleaseDC(dibwindow, hdc);

    if (vid.conheight > mode->height)
	vid.conheight = mode->height;
    if (vid.conwidth > mode->width)
	vid.conwidth = mode->width;
    vid.width = vid.conwidth;
    vid.height = vid.conheight;

    vid.numpages = 2;

// needed because we're not getting WM_MOVE messages fullscreen on NT
    window_x = 0;
    window_y = 0;

    mainwindow = dibwindow;

    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)hIcon);
    SendMessage(mainwindow, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)hIcon);

    return true;
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    int temp, modenum;
    qboolean stat;
    MSG msg;

    modenum = mode - modelist;
    if (windowed) {
	if (modenum != 0)
	    Sys_Error("Bad video mode");
    } else {
	if (modenum < 1 ||  modenum >= nummodes)
	    Sys_Error("Bad video mode");
    }

    /* so Con_Printfs don't mess us up by forcing vid and snd updates */
    temp = scr_disabled_for_loading;
    scr_disabled_for_loading = true;

    CDAudio_Pause();

    // Set either the fullscreen or windowed mode
    stat = false;
    if (mode->fullscreen) {
	stat = VID_SetFullDIBMode(mode);
	IN_ActivateMouse();
	IN_HideMouse();
    } else {
	if (_windowed_mouse.value && key_dest == key_game) {
	    stat = VID_SetWindowedMode(mode);
	    IN_ActivateMouse();
	    IN_HideMouse();
	} else {
	    IN_DeactivateMouse();
	    IN_ShowMouse();
	    stat = VID_SetWindowedMode(mode);
	}
    }

    window_width = DIBWidth;
    window_height = DIBHeight;
    VID_UpdateWindowStatus();

    CDAudio_Resume();
    scr_disabled_for_loading = temp;

    if (!stat) {
	Sys_Error("Couldn't set video mode");
    }
// now we try to make sure we get the focus on the mode switch, because
// sometimes in some systems we don't.  We grab the foreground, then
// finish setting up, pump all our messages, and sleep for a little while
// to let messages finish bouncing around the system, then we put
// ourselves at the top of the z order, then grab the foreground again,
// Who knows if it helps, but it probably doesn't hurt
    SetForegroundWindow(mainwindow);
    VID_SetPalette(palette);
    vid_modenum = modenum;
    Cvar_SetValue("vid_mode", (float)vid_modenum);

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
	TranslateMessage(&msg);
	DispatchMessage(&msg);
    }

    Sleep(100);

    SetWindowPos(mainwindow, HWND_TOP, 0, 0, 0, 0,
		 SWP_DRAWFRAME | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW |
		 SWP_NOCOPYBITS);

    SetForegroundWindow(mainwindow);

// fix the leftover Alt from any Alt-Tab or the like that switched us away
    ClearAllStates();

    VID_SetPalette(palette);

    vid.recalc_refdef = 1;

    return true;
}


/*
================
VID_UpdateWindowStatus
================
*/
static void
VID_UpdateWindowStatus(void)
{

    window_rect.left = window_x;
    window_rect.top = window_y;
    window_rect.right = window_x + window_width;
    window_rect.bottom = window_y + window_height;
    window_center_x = (window_rect.left + window_rect.right) / 2;
    window_center_y = (window_rect.top + window_rect.bottom) / 2;

    IN_UpdateClipCursor();
}

//int           texture_mode = GL_NEAREST;
//int           texture_mode = GL_NEAREST_MIPMAP_NEAREST;
//int           texture_mode = GL_NEAREST_MIPMAP_LINEAR;
int texture_mode = GL_LINEAR;

//int           texture_mode = GL_LINEAR_MIPMAP_NEAREST;
//int           texture_mode = GL_LINEAR_MIPMAP_LINEAR;

static void
CheckMultiTextureExtensions(void)
{
    // FIXME - Do proper substring testing (could be last extension, no space)
    // FIXME - Check for wglGetProcAddress errors...
    gl_mtexable = false;
    if (!COM_CheckParm("-nomtex")
	&& strstr(gl_extensions, "GL_ARB_multitexture ")) {
	Con_Printf("ARB multitexture extensions found.\n");

	qglMultiTexCoord2fARB =
	    (void *)wglGetProcAddress("glMultiTexCoord2fARB");
	qglActiveTextureARB =
	    (void *)wglGetProcAddress("glActiveTextureARB");

	/* Check how many texture units there actually are */
	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &gl_num_texture_units);

	if (gl_num_texture_units < 2) {
	    Con_Printf("Only %i texture units, multitexture disabled.\n",
		       gl_num_texture_units);
	} else if (!qglMultiTexCoord2fARB || !qglActiveTextureARB) {
	    Con_Printf("ARB Multitexture symbols not found, disabled.\n");
	} else {
	    Con_Printf("ARB multitexture extension enabled\n"
		       "-> %i texture units available\n",
		       gl_num_texture_units);
	    gl_mtexable = true;
	}
    }
}


/*
===============
GL_Init
===============
*/
static void
GL_Init(void)
{
    gl_vendor = (char *)glGetString(GL_VENDOR);
    Con_Printf("GL_VENDOR: %s\n", gl_vendor);
    gl_renderer = (char *)glGetString(GL_RENDERER);
    Con_Printf("GL_RENDERER: %s\n", gl_renderer);

    gl_version = (char *)glGetString(GL_VERSION);
    Con_Printf("GL_VERSION: %s\n", gl_version);
    gl_extensions = (char *)glGetString(GL_EXTENSIONS);
    Con_DPrintf("GL_EXTENSIONS: %s\n", gl_extensions);

//      Con_Printf ("%s %s\n", gl_renderer, gl_version);

    CheckMultiTextureExtensions();

    //glClearColor(1, 0, 0, 0);
    glClearColor(0.5, 0.5, 0.5, 0);
    glCullFace(GL_FRONT);
    glEnable(GL_TEXTURE_2D);

    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.666);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_FLAT);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

//      glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
}

/*
=================
GL_BeginRendering
=================
*/
void
GL_BeginRendering(int *x, int *y, int *width, int *height)
{
    *x = *y = 0;
    *width = WindowRect.right - WindowRect.left;
    *height = WindowRect.bottom - WindowRect.top;

//    if (!wglMakeCurrent( maindc, baseRC ))
//              Sys_Error ("wglMakeCurrent failed");

//      glViewport (*x, *y, *width, *height);
}


void
GL_EndRendering(void)
{
    if (!scr_skipupdate || scr_block_drawing)
	SwapBuffers(maindc);

// handle the mouse state when windowed if that's changed
    if (modestate == MS_WINDOWED) {
	if (!_windowed_mouse.value) {
	    if (windowed_mouse) {
		IN_DeactivateMouse();
		IN_ShowMouse();
		windowed_mouse = false;
	    }
	} else {
	    windowed_mouse = true;
	    if (key_dest == key_game && !mouseactive && ActiveApp) {
		IN_ActivateMouse();
		IN_HideMouse();
	    } else if (mouseactive && key_dest != key_game) {
		IN_DeactivateMouse();
		IN_ShowMouse();
	    }
	}
    }
    if (fullsbardraw)
	Sbar_Changed();
}

void
VID_SetPalette(const byte *palette)
{
    const byte *pal;
    unsigned r, g, b;
    unsigned v;
    int r1, g1, b1;
    int j, k, l;
    unsigned short i;
    unsigned *table;

//
// 8 8 8 encoding
//
    pal = palette;
    table = d_8to24table;
    for (i = 0; i < 256; i++) {
	r = pal[0];
	g = pal[1];
	b = pal[2];
	pal += 3;

//              v = (255<<24) + (r<<16) + (g<<8) + (b<<0);
//              v = (255<<0) + (r<<8) + (g<<16) + (b<<24);
	v = (255 << 24) + (r << 0) + (g << 8) + (b << 16);
	*table++ = v;
    }
    d_8to24table[255] &= 0xffffff;	// 255 is transparent

    // JACK: 3D distance calcs - k is last closest, l is the distance.
    // FIXME: Precalculate this and cache to disk.
    for (i = 0; i < (1 << 15); i++) {
	/* Maps
	   000000000000000
	   000000000011111 = Red  = 0x1F
	   000001111100000 = Blue = 0x03E0
	   111110000000000 = Grn  = 0x7C00
	 */
	r = ((i & 0x1F) << 3) + 4;
	g = ((i & 0x03E0) >> 2) + 4;
	b = ((i & 0x7C00) >> 7) + 4;
	pal = (const byte *)d_8to24table;
	for (v = 0, k = 0, l = 10000 * 10000; v < 256; v++, pal += 4) {
	    r1 = r - pal[0];
	    g1 = g - pal[1];
	    b1 = b - pal[2];
	    j = (r1 * r1) + (g1 * g1) + (b1 * b1);
	    if (j < l) {
		k = v;
		l = j;
	    }
	}
	d_15to8table[i] = k;
    }
}

void (*VID_SetGammaRamp)(unsigned short ramp[3][256]);
static unsigned short saved_gamma_ramp[3][256];

static void
VID_SetWinGammaRamp(unsigned short ramp[3][256])
{
    BOOL result;

    result = SetDeviceGammaRamp(maindc, ramp);
}

void
Gamma_Init(void)
{
    BOOL result = GetDeviceGammaRamp(maindc, saved_gamma_ramp);

    if (result)
	result = SetDeviceGammaRamp(maindc, saved_gamma_ramp);
    if (result)
	VID_SetGammaRamp = VID_SetWinGammaRamp;
    else
	VID_SetGammaRamp = NULL;
}

void
VID_ShiftPalette(const byte *palette)
{
    //VID_SetPalette (palette);
    //gammaworks = SetDeviceGammaRamp (maindc, ramps);
}


void
VID_SetDefaultMode(void)
{
    IN_DeactivateMouse();
}


void
VID_Shutdown(void)
{
    HGLRC hRC;
    HDC hDC;

    if (vid_initialized) {
	if (VID_SetGammaRamp)
	    VID_SetGammaRamp(saved_gamma_ramp);

	vid_canalttab = false;
	hRC = wglGetCurrentContext();
	hDC = wglGetCurrentDC();

	wglMakeCurrent(NULL, NULL);

	if (hRC)
	    wglDeleteContext(hRC);

	if (hDC && dibwindow)
	    ReleaseDC(dibwindow, hDC);

	if (modestate == MS_FULLDIB)
	    ChangeDisplaySettings(NULL, 0);

	if (maindc && dibwindow)
	    ReleaseDC(dibwindow, maindc);

	AppActivate(false, false);
    }
}


//==========================================================================


static BOOL
bSetupPixelFormat(HDC hDC)
{
    static PIXELFORMATDESCRIPTOR pfd = {
	sizeof(PIXELFORMATDESCRIPTOR),	// size of this pfd
	1,			// version number
	PFD_DRAW_TO_WINDOW	// support window
	    | PFD_SUPPORT_OPENGL	// support OpenGL
	    | PFD_DOUBLEBUFFER,	// double buffered
	PFD_TYPE_RGBA,		// RGBA type
	24,			// 24-bit color depth
	0, 0, 0, 0, 0, 0,	// color bits ignored
	0,			// no alpha buffer
	0,			// shift bit ignored
	0,			// no accumulation buffer
	0, 0, 0, 0,		// accum bits ignored
	32,			// 32-bit z-buffer
	0,			// no stencil buffer
	0,			// no auxiliary buffer
	PFD_MAIN_PLANE,		// main layer
	0,			// reserved
	0, 0, 0			// layer masks ignored
    };
    int pixelformat;

    if ((pixelformat = ChoosePixelFormat(hDC, &pfd)) == 0) {
	MessageBox(NULL, "ChoosePixelFormat failed", "Error", MB_OK);
	return FALSE;
    }

    if (SetPixelFormat(hDC, pixelformat, &pfd) == FALSE) {
	MessageBox(NULL, "SetPixelFormat failed", "Error", MB_OK);
	return FALSE;
    }

    return TRUE;
}


static knum_t scantokey[128] = {
//  0       1       2       3       4       5       6       7
//  8       9       A       B       C       D       E       F
    0,      K_ESCAPE, '1',  '2',    '3',    '4',    '5',    '6',
    '7',    '8',    '9',    '0',    '-',    '=',    K_BACKSPACE, K_TAB,	// 0
    'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
    'o',    'p',    '[',    ']',    13,     K_LCTRL,'a',    's',	// 1
    'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
    '\'',   '`',    K_LSHIFT, '\\', 'z',    'x',    'c',    'v',	// 2
    'b',    'n',    'm',    ',',    '.',    '/',    K_RSHIFT, '*',
    K_LALT, ' ',    0,      K_F1,   K_F2,   K_F3,   K_F4,   K_F5,	// 3
    K_F6,   K_F7,   K_F8,   K_F9,   K_F10,  K_PAUSE, 0,     K_HOME,
    K_UPARROW, K_PGUP, '-', K_LEFTARROW, '5', K_RIGHTARROW, '+', K_END,	// 4
    K_DOWNARROW, K_PGDN, K_INS, K_DEL, 0,   0,      0,      K_F11,
    K_F12,  0,      0,      0,      0,      0,      0,      0,		// 5
    0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,		// 6
    0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0		// 7
};

/*
=======
MapKey

Map from windows to quake keynums
=======
*/
static knum_t
MapKey(int key)
{
    key = (key >> 16) & 255;
    if (key > 127)
	return 0;
    if (scantokey[key] == 0)
	Con_DPrintf("key 0x%02x has no translation\n", key);
    return scantokey[key];
}

/*
===================================================================

MAIN WINDOW

===================================================================
*/

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
    for (i = 0; i < 256; i++) {
	Key_Event(i, false);
    }

    Key_ClearStates();
    IN_ClearStates();
}

static void
AppActivate(BOOL fActive, BOOL minimize)
/****************************************************************************
*
* Function:     AppActivate
* Parameters:   fActive - True if app is activating
*
* Description:  If the application is activating, then swap the system
*               into SYSPAL_NOSTATIC mode so that our palettes will display
*               correctly.
*
****************************************************************************/
{
    static BOOL sound_active;

    ActiveApp = fActive;
    Minimized = minimize;

// enable/disable sound on focus gain/loss
    if (!ActiveApp && sound_active) {
	S_BlockSound();
	sound_active = false;
    } else if (ActiveApp && !sound_active) {
	S_UnblockSound();
	sound_active = true;
    }

    if (fActive) {
	if (modestate == MS_FULLDIB) {
	    IN_ActivateMouse();
	    IN_HideMouse();
	    if (vid_canalttab && vid_wassuspended) {
		vid_wassuspended = false;
		ChangeDisplaySettings(&gdevmode, CDS_FULLSCREEN);
		ShowWindow(mainwindow, SW_SHOWNORMAL);

		/*
		 * Work-around for a bug in some video drivers that don't
		 * correctly update the offsets into the GL front buffer after
		 * alt-tab to the desktop and back.
		 */
		MoveWindow(mainwindow, 0, 0, gdevmode.dmPelsWidth,
			   gdevmode.dmPelsHeight, false);
	    }
	} else if ((modestate == MS_WINDOWED) && _windowed_mouse.value
		   && key_dest == key_game) {
	    IN_ActivateMouse();
	    IN_HideMouse();
	}
	/* Restore game gamma */
	if (VID_SetGammaRamp)
	    VID_SetGammaRamp(ramps);
    }

    if (!fActive) {
	/* Restore desktop gamma */
	if (VID_SetGammaRamp)
	    VID_SetGammaRamp(saved_gamma_ramp);
	if (modestate == MS_FULLDIB) {
	    IN_DeactivateMouse();
	    IN_ShowMouse();
	    if (vid_canalttab) {
		ChangeDisplaySettings(NULL, 0);
		vid_wassuspended = true;
	    }
	} else if ((modestate == MS_WINDOWED) && _windowed_mouse.value) {
	    IN_DeactivateMouse();
	    IN_ShowMouse();
	}
    }
}


/* main window procedure */
static LONG WINAPI
MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LONG lRet = 1;
    int fActive, fMinimized, temp;

    if (uMsg == uiWheelMessage)
	uMsg = WM_MOUSEWHEEL;

    switch (uMsg) {
    case WM_KILLFOCUS:
	if (modestate == MS_FULLDIB)
	    ShowWindow(mainwindow, SW_SHOWMINNOACTIVE);
	break;

    case WM_CREATE:
	break;

    case WM_MOVE:
	window_x = (int)LOWORD(lParam);
	window_y = (int)HIWORD(lParam);
	VID_UpdateWindowStatus();
	break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
	Key_Event(MapKey(lParam), true);
	break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
	Key_Event(MapKey(lParam), false);
	break;

    case WM_SYSCHAR:
	// keep Alt-Space from happening
	break;

	// this is complicated because Win32 seems to pack multiple mouse events into
	// one update sometimes, so we always check all states and look for events
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:
	temp = 0;

	if (wParam & MK_LBUTTON)
	    temp |= 1;

	if (wParam & MK_RBUTTON)
	    temp |= 2;

	if (wParam & MK_MBUTTON)
	    temp |= 4;

	IN_MouseEvent(temp);

	break;

	// JACK: This is the mouse wheel with the Intellimouse
	// Its delta is either positive or neg, and we generate the proper
	// Event.
    case WM_MOUSEWHEEL:
	if ((short)HIWORD(wParam) > 0) {
	    Key_Event(K_MWHEELUP, true);
	    Key_Event(K_MWHEELUP, false);
	} else {
	    Key_Event(K_MWHEELDOWN, true);
	    Key_Event(K_MWHEELDOWN, false);
	}
	break;

    case WM_SIZE:
	break;

    case WM_CLOSE:
	if (MessageBox
	    (mainwindow, "Are you sure you want to quit?", "Confirm Exit",
	     MB_YESNO | MB_SETFOREGROUND | MB_ICONQUESTION) == IDYES) {
	    Sys_Quit();
	}

	break;

    case WM_ACTIVATE:
	fActive = LOWORD(wParam);
	fMinimized = (BOOL)HIWORD(wParam);
	AppActivate(!(fActive == WA_INACTIVE), fMinimized);

	// fix the leftover Alt from any Alt-Tab or the like that switched us away
	ClearAllStates();

	break;

    case WM_DESTROY:
	{
	    if (dibwindow)
		DestroyWindow(dibwindow);

	    PostQuitMessage(0);
	}
	break;

    case MM_MCINOTIFY:
	lRet = CDDrv_MessageHandler(hWnd, uMsg, wParam, lParam);
	break;

    default:
	/* pass all unhandled messages to DefWindowProc */
	lRet = DefWindowProc(hWnd, uMsg, wParam, lParam);
	break;
    }

    /* return 1 if handled message, 0 if not */
    return lRet;
}

static void
VID_InitDIB(HINSTANCE hInstance)
{
    qvidmode_t *mode = &modelist[0];
    WNDCLASS wc;

    /* Register the frame class */
    wc.style = 0;
    wc.lpfnWndProc = (WNDPROC) MainWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = 0;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = 0;
    wc.lpszClassName = "TyrQuake";

    if (!RegisterClass(&wc))
	Sys_Error("Couldn't register window class");

    /* Fill in a vidmode from the command line */
    mode->fullscreen = false;
    if (COM_CheckParm("-width"))
	mode->width = Q_atoi(com_argv[COM_CheckParm("-width") + 1]);
    else
	mode->width = 640;

    if (mode->width < 320)
	mode->width = 320;

    if (COM_CheckParm("-height"))
	mode->height = Q_atoi(com_argv[COM_CheckParm("-height") + 1]);
    else
	mode->height = mode->width * 240 / 320;

    if (mode->height < 240)
	mode->height = 240;

    snprintf(mode->modedesc, sizeof(mode->modedesc), "%dx%d",
	     mode->width, mode->height);

    mode->modenum = 0;
    mode->bpp = 0;

    nummodes = 1;
}


/*
=================
VID_InitFullDIB
=================
*/
static void
VID_InitFullDIB(HINSTANCE hInstance)
{
    DEVMODE devmode;
    int i, modenum, originalnummodes, existingmode, numlowresmodes;
    int j, bpp, done;
    BOOL stat;
    qvidmode_t *mode;

// enumerate >8 bpp modes
    originalnummodes = nummodes;
    modenum = 0;

    do {
	stat = EnumDisplaySettings(NULL, modenum, &devmode);

	if ((devmode.dmBitsPerPel >= 15) &&
	    (devmode.dmPelsWidth <= MAXWIDTH) &&
	    (devmode.dmPelsHeight <= MAXHEIGHT) &&
	    (nummodes < MAX_MODE_LIST)) {
	    devmode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

	    if (ChangeDisplaySettings(&devmode, CDS_TEST | CDS_FULLSCREEN) ==
		DISP_CHANGE_SUCCESSFUL) {
		mode = &modelist[nummodes];

		mode->width = devmode.dmPelsWidth;
		mode->height = devmode.dmPelsHeight;
		mode->modenum = 0;
		mode->fullscreen = 1;
		mode->bpp = devmode.dmBitsPerPel;
		snprintf(mode->modedesc, sizeof(mode->modedesc), "%dx%dx%d",
			 mode->width, mode->height, mode->bpp);

		/*
		 * if the width is more than twice the height, reduce
		 * it by half because this is probably a dual-screen
		 * monitor
		 */
		if (!COM_CheckParm("-noadjustaspect")) {
		    if (mode->width > mode->height << 1) {
			mode->width >>= 1;
			snprintf(mode->modedesc, sizeof(mode->modedesc),
				 "%dx%dx%d", mode->width, mode->height,
				 mode->bpp);
		    }
		}

		for (i = originalnummodes, existingmode = 0; i < nummodes; i++) {
		    if (mode->width == modelist[i].width
			&& mode->height == modelist[i].height
			&& mode->bpp == modelist[i].bpp) {
			existingmode = 1;
			break;
		    }
		}
		if (!existingmode)
		    nummodes++;
	    }
	}
	modenum++;
    } while (stat);

// see if there are any low-res modes that aren't being reported
    numlowresmodes = sizeof(lowresmodes) / sizeof(lowresmodes[0]);
    bpp = 16;
    done = 0;

    do {
	for (j = 0; (j < numlowresmodes) && (nummodes < MAX_MODE_LIST); j++) {
	    devmode.dmBitsPerPel = bpp;
	    devmode.dmPelsWidth = lowresmodes[j].width;
	    devmode.dmPelsHeight = lowresmodes[j].height;
	    devmode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

	    if (ChangeDisplaySettings(&devmode, CDS_TEST | CDS_FULLSCREEN) ==
		DISP_CHANGE_SUCCESSFUL) {
		mode = &modelist[nummodes];

		mode->width = devmode.dmPelsWidth;
		mode->height = devmode.dmPelsHeight;
		mode->modenum = 0;
		mode->fullscreen = 1;
		mode->bpp = devmode.dmBitsPerPel;
		snprintf(mode->modedesc, sizeof(mode->modedesc), "%dx%dx%d",
			 mode->width, mode->height, mode->bpp);

		for (i = originalnummodes, existingmode = 0; i < nummodes; i++) {
		    if (mode->width == modelist[i].width
			&& mode->height == modelist[i].height
			&& mode->bpp == modelist[i].bpp) {
			existingmode = 1;
			break;
		    }
		}
		if (!existingmode)
		    nummodes++;
	    }
	}
	switch (bpp) {
	case 16:
	    bpp = 32;
	    break;

	case 32:
	    bpp = 24;
	    break;

	case 24:
	    done = 1;
	    break;
	}
    } while (!done);

    if (nummodes == originalnummodes)
	Con_SafePrintf("No fullscreen DIB modes found\n");
}

static void
Check_Gamma(const byte *palette, byte *newpalette)
{
    float f, inf;
    int i;

    i = COM_CheckParm("-gamma");
    vid_gamma = i ? Q_atof(com_argv[i + 1]) : 1.0;

    for (i = 0; i < 768; i++) {
	f = pow((palette[i] + 1) / 256.0, vid_gamma);
	inf = f * 255 + 0.5;
	if (inf < 0)
	    inf = 0;
	if (inf > 255)
	    inf = 255;
	newpalette[i] = inf;
    }
}

/*
===================
VID_Init
===================
*/
void
VID_Init(const byte *palette)
{
    int i, existingmode;
    int width, height, bpp, findbpp, done;
    byte gamma_palette[256 * 3];
    char gldir[MAX_OSPATH];
    HDC hdc;
    DEVMODE devmode;
    qvidmode_t *mode;

    memset(&devmode, 0, sizeof(devmode));

    Cvar_RegisterVariable(&vid_mode);
    Cvar_RegisterVariable(&vid_wait);
    Cvar_RegisterVariable(&vid_nopageflip);
    Cvar_RegisterVariable(&_vid_wait_override);
    Cvar_RegisterVariable(&_vid_default_mode);
    Cvar_RegisterVariable(&_vid_default_mode_win);
    Cvar_RegisterVariable(&vid_config_x);
    Cvar_RegisterVariable(&vid_config_y);
    Cvar_RegisterVariable(&vid_stretch_by_2);
    Cvar_RegisterVariable(&_windowed_mouse);
    Cvar_RegisterVariable(&gl_ztrick);

    Cmd_AddCommand("vid_nummodes", VID_NumModes_f);
    Cmd_AddCommand("vid_describecurrentmode", VID_DescribeCurrentMode_f);
    Cmd_AddCommand("vid_describemode", VID_DescribeMode_f);
    Cmd_AddCommand("vid_describemodes", VID_DescribeModes_f);

    hIcon = LoadIcon(global_hInstance, MAKEINTRESOURCE(IDI_ICON2));

    InitCommonControls();

    VID_InitDIB(global_hInstance);
    nummodes = 1;

    VID_InitFullDIB(global_hInstance);

    height = 0;			// FIXME - Uninitialized? Zero probably not desirable...

    if (COM_CheckParm("-window") || COM_CheckParm("-w")) {
	hdc = GetDC(NULL);

	if (GetDeviceCaps(hdc, RASTERCAPS) & RC_PALETTE) {
	    Sys_Error("Can't run in non-RGB mode");
	}

	ReleaseDC(NULL, hdc);

	windowed = true;

	vid_default = VID_MODE_WINDOWED;
    } else {
	if (nummodes == 1)
	    Sys_Error("No RGB fullscreen modes available");

	windowed = false;

	if (COM_CheckParm("-mode")) {
	    vid_default = Q_atoi(com_argv[COM_CheckParm("-mode") + 1]);
	} else {
	    if (COM_CheckParm("-current")) {
		mode = &modelist[VID_MODE_FULLSCREEN_DEFAULT];
		mode->width = GetSystemMetrics(SM_CXSCREEN);
		mode->height = GetSystemMetrics(SM_CYSCREEN);
		vid_default = VID_MODE_FULLSCREEN_DEFAULT;
		leavecurrentmode = 1;
	    } else {
		if (COM_CheckParm("-width")) {
		    width = Q_atoi(com_argv[COM_CheckParm("-width") + 1]);
		} else {
		    width = 640;
		}

		if (COM_CheckParm("-bpp")) {
		    bpp = Q_atoi(com_argv[COM_CheckParm("-bpp") + 1]);
		    findbpp = 0;
		} else {
		    bpp = 15;
		    findbpp = 1;
		}

		if (COM_CheckParm("-height")) {
		    height = Q_atoi(com_argv[COM_CheckParm("-height") + 1]);
		} else {
		    // FIXME - what default to use? calculate for 4:3 aspect?
		    height = 0;
		}

		// if they want to force it, add the specified mode to the list
		if (COM_CheckParm("-force") && (nummodes < MAX_MODE_LIST)) {
		    mode = &modelist[nummodes];

		    mode->width = width;
		    mode->height = height;
		    mode->modenum = 0;
		    mode->fullscreen = 1;
		    mode->bpp = bpp;
		    sprintf(mode->modedesc, "%ldx%ldx%ld",
			    devmode.dmPelsWidth, devmode.dmPelsHeight,
			    devmode.dmBitsPerPel);

		    for (i = nummodes, existingmode = 0; i < nummodes; i++) {
			if ((mode->width == modelist[i].width) &&
			    (mode->height == modelist[i].height)
			    && (mode->bpp == modelist[i].bpp)) {
			    existingmode = 1;
			    break;
			}
		    }
		    if (!existingmode)
			nummodes++;
		}

		done = 0;

		do {
		    if (COM_CheckParm("-height")) {
			height =
			    Q_atoi(com_argv[COM_CheckParm("-height") + 1]);

			for (i = 1, vid_default = 0; i < nummodes; i++) {
			    if ((modelist[i].width == width) &&
				(modelist[i].height == height) &&
				(modelist[i].bpp == bpp)) {
				vid_default = i;
				done = 1;
				break;
			    }
			}
		    } else {
			for (i = 1, vid_default = 0; i < nummodes; i++) {
			    if ((modelist[i].width == width)
				&& (modelist[i].bpp == bpp)) {
				vid_default = i;
				done = 1;
				break;
			    }
			}
		    }

		    if (!done) {
			if (findbpp) {
			    switch (bpp) {
			    case 15:
				bpp = 16;
				break;
			    case 16:
				bpp = 32;
				break;
			    case 32:
				bpp = 24;
				break;
			    case 24:
				done = 1;
				break;
			    }
			} else {
			    done = 1;
			}
		    }
		} while (!done);

		if (!vid_default) {
		    Sys_Error("Specified video mode not available");
		}
	    }
	}
    }

    vid_initialized = true;

    if ((i = COM_CheckParm("-conwidth")) != 0)
	vid.conwidth = Q_atoi(com_argv[i + 1]);
    else
	vid.conwidth = 640;

    vid.conwidth &= 0xfff8;	// make it a multiple of eight

    if (vid.conwidth < 320)
	vid.conwidth = 320;

    // pick a conheight that matches with correct aspect
    vid.conheight = vid.conwidth * 3 / 4;

    if ((i = COM_CheckParm("-conheight")) != 0)
	vid.conheight = Q_atoi(com_argv[i + 1]);
    if (vid.conheight < 200)
	vid.conheight = 200;

    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    DestroyWindow(hwnd_dialog);

    Check_Gamma(palette, gamma_palette);
    VID_SetPalette(gamma_palette);

    VID_SetMode(&modelist[vid_default], gamma_palette);
    Gamma_Init();

    maindc = GetDC(mainwindow);
    bSetupPixelFormat(maindc);

    baseRC = wglCreateContext(maindc);
    if (!baseRC)
	Sys_Error("Could not initialize GL (wglCreateContext failed).\n\n"
		  "Make sure you in are 65535 color mode, "
		  "and try running -window.");
    if (!wglMakeCurrent(maindc, baseRC))
	Sys_Error("wglMakeCurrent failed");

    GL_Init();

    sprintf(gldir, "%s/glquake", com_gamedir);
    Sys_mkdir(gldir);

    vid_realmode = vid_modenum;
    vid_menudrawfn = VID_MenuDraw_WGL;
    vid_menukeyfn = VID_MenuKey_WGL;
    vid_canalttab = true;

    if (COM_CheckParm("-fullsbar"))
	fullsbardraw = true;
}


//========================================================
// Video menu stuff
//========================================================

static int vid_wmodes;

typedef struct {
    const qvidmode_t *mode;
    const char *desc;
    int iscur;
} modedesc_t;

#define VID_ROW_SIZE     3
#define MAX_COLUMN_SIZE  9
#define MODE_AREA_HEIGHT (MAX_COLUMN_SIZE + 2)
#define MAX_MODEDESCS    (MAX_COLUMN_SIZE * VID_ROW_SIZE)

static modedesc_t modedescs[MAX_MODEDESCS];

static const char *
VID_GetModeDescription(const qvidmode_t *mode)
{
    static char desc[40];
    char temp[40];

    snprintf(temp, sizeof(temp), "%dx%dx%d", mode->width, mode->height, mode->bpp);
    snprintf(desc, sizeof(desc), "%14s", temp);

    return desc;
}

/*
================
VID_MenuDraw_WGL
================
*/
static void
VID_MenuDraw_WGL(void)
{
    const qpic_t *pic;
    int i, column, row;

    pic = Draw_CachePic("gfx/vidmodes.lmp");
    M_DrawPic((320 - pic->width) / 2, 4, pic);

    vid_wmodes = 0;

    for (i = 1; (i < nummodes) && (vid_wmodes < MAX_MODEDESCS); i++) {
	modedesc_t *mode = &modedescs[vid_wmodes];
	mode->mode = &modelist[i];
	mode->desc = NULL;
	mode->iscur = (i == vid_modenum);
	vid_wmodes++;
    }

    if (vid_wmodes > 0) {
	M_Print(2 * 8, 36 + 0 * 8, "Fullscreen Modes (WIDTHxHEIGHTxBPP)");

	column = 8;
	row = 36 + 2 * 8;

	for (i = 0; i < vid_wmodes; i++) {
	    const modedesc_t *mode = &modedescs[i];
	    const char *desc = VID_GetModeDescription(mode->mode);

	    if (mode->iscur)
		M_PrintWhite(column, row, desc);
	    else
		M_Print(column, row, desc);

	    column += 15 * 8;
	    if ((i % VID_ROW_SIZE) == (VID_ROW_SIZE - 1)) {
		column = 8;
		row += 8;
	    }
	}
    }

    M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 2,
	    "Video modes must be set from the");
    M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 3,
	    "command line with -width <width>");
    M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 4,
	    "and -bpp <bits-per-pixel>");
    M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 6,
	    "Select windowed mode with -window");
}


/*
================
VID_MenuKey_WGL
================
*/
static void
VID_MenuKey_WGL(int key)
{
    switch (key) {
    case K_ESCAPE:
	S_LocalSound("misc/menu1.wav");
	M_Menu_Options_f();
	break;

    default:
	break;
    }
}

qboolean
VID_IsFullScreen()
{
    return modelist[vid_modenum].fullscreen;
}
