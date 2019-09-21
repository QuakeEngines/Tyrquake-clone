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

#ifdef APPLE_OPENGL
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "SDL.h"

#include "cmd.h"
#include "console.h"
#include "glquake.h"
#include "input.h"
#include "quakedef.h"
#include "sbar.h"
#include "screen.h"
#include "sdl_common.h"
#include "sys.h"
#include "vid.h"

#ifdef NQ_HACK
#include "host.h"
#endif

void *
GL_GetProcAddress(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}

viddef_t vid;

qboolean VID_CheckAdequateMem(int width, int height) { return true; }
void VID_LockBuffer(void) {}
void VID_UnlockBuffer(void) {}

void (*VID_SetGammaRamp)(unsigned short ramp[3][256]) = NULL;

float gldepthmin, gldepthmax;

void VID_Update(vrect_t *rects) {}
void D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height) {}
void D_EndDirectRect(int x, int y, int width, int height) {}

/*
 * FIXME!!
 *
 * Move stuff around or create abstractions so these hacks aren't needed
 */

#ifndef _WIN32
void Sys_SendKeyEvents(void)
{
    IN_ProcessEvents();
}
#endif

#ifdef _WIN32
#include <windows.h>

qboolean DDActive;
HWND mainwindow;
void VID_SetDefaultMode(void) {}
qboolean window_visible(void) { return true; }
#endif

void
VID_GetDesktopRect(vrect_t *rect)
{
    int display;
    SDL_DisplayMode mode;

    display = SDL_GetWindowDisplayIndex(sdl_window);
    SDL_GetDesktopDisplayMode(display, &mode);

    rect->x = 0;
    rect->y = 0;
    rect->width = mode.w;
    rect->height = mode.h;
}

static void
VID_InitCvars(void)
{
    Cvar_RegisterVariable(&gl_npot);
}

static SDL_GLContext gl_context = NULL;

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    Uint32 flags;
    int i, err;
    qboolean reload_textures = false;
    int depths[] = { 32, 24, 16 };

    S_BlockSound();
    S_ClearBuffer();

    if (gl_context) {
        GL_Shutdown();
	SDL_GL_DeleteContext(gl_context);
        reload_textures = true;
    }
    if (sdl_window)
	SDL_DestroyWindow(sdl_window);

    flags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
    if (mode != &vid_windowed_mode)
	flags |= SDL_WINDOW_FULLSCREEN;

    /*
     * Try to set the correct attributes for our desired GL context
     * - Ensure we request the compatibility context
     * - Set the requested color buffer BPP (although we may get more?)
     * - Try to get the best depth buffer we can, try 32, 24, then 16 bits.
     */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, mode->bpp);

    for (i = 0; i < ARRAY_SIZE(depths); i++) {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depths[i]);
        sdl_window = SDL_CreateWindow("TyrQuake",
                                      SDL_WINDOWPOS_UNDEFINED,
                                      SDL_WINDOWPOS_UNDEFINED,
                                      mode->width, mode->height, flags);
        if (sdl_window)
            break;
    }
    if (!sdl_window)
	Sys_Error("%s: Unable to create window: %s", __func__, SDL_GetError());

    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context)
	Sys_Error("%s: Unable to create OpenGL context: %s",
		  __func__, SDL_GetError());

    err = SDL_GL_MakeCurrent(sdl_window, gl_context);
    if (err)
	Sys_Error("%s: SDL_GL_MakeCurrent() failed: %s",
		  __func__, SDL_GetError());

    VID_SDL_SetIcon();

    GL_Init();
    if (reload_textures)
	GL_ReloadTextures();

    vid.numpages = 0; /* Contents of the back buffer are undefined after swap */
    vid.width = vid.conwidth = mode->width;
    vid.height = vid.conheight = mode->height;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    vid_currentmode = mode;

    vid.recalc_refdef = 1;

    SCR_CheckResize();
    Con_CheckResize();

    S_UnblockSound();

    return true;
}

void
VID_Init(const byte *palette)
{
    int err;
    const qvidmode_t *mode;

    VID_InitCvars();
    VID_InitModeCvars();
    VID_InitModeCommands();

    Q_SDL_InitOnce();
    err = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (err < 0)
	Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());

    VID_SDL_InitModeList();
    VID_LoadConfig();
    mode = VID_GetCmdlineMode();
    if (!mode)
        mode = VID_GetModeFromCvars();
    if (!mode)
	mode = &vid_windowed_mode;

    VID_SetMode(mode, palette);
    VID_SetPalette(palette);

    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;
}

void
VID_Shutdown(void)
{
    if (sdl_window)
	SDL_DestroyWindow(sdl_window);
    if (sdl_desktop_format)
	SDL_FreeFormat(sdl_desktop_format);
}

void
GL_BeginRendering(int *x, int *y, int *width, int *height)
{
    *x = *y = 0;
    *width = vid.width;
    *height = vid.height;
}

void
GL_EndRendering(void)
{
    glFlush();
    SDL_GL_SwapWindow(sdl_window);
}

void
VID_SetPalette(const byte *palette)
{
    QPic32_InitPalettes(palette);
}

void
VID_ShiftPalette(const byte *palette)
{
    /* Done via gl_polyblend instead */
    //VID_SetPalette(palette);
}
