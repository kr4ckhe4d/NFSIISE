// SPDX-License-Identifier: MIT

#ifndef WRAPPER_H
#define WRAPPER_H

#if !defined(WIN32) && !defined(_GNU_SOURCE)
	#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef NOT_COMPILING
	#define STDCALL
	#define REGPARM

	#define MAYBE_THIS_SINGLE void
	#define MAYBE_THIS
#elif !defined(NFS_CPP) && (defined(__i386) || defined(__i386__))
	#define STDCALL __attribute__((stdcall))
	#define REGPARM __attribute__((regparm(2))) //First two arguments are compatible with Watcom fastcall

	#define MAYBE_THIS_SINGLE void
	#define MAYBE_THIS
#else
	#define STDCALL
	#define REGPARM

	#define MAYBE_THIS_SINGLE void *this
	#define MAYBE_THIS void *this,

	#ifndef NFS_CPP
		#define NFS_CPP
	#endif
#endif

#if !defined(NFS_CPP) && defined(STACK_REALIGN)
	#define REALIGN __attribute__((force_align_arg_pointer))
#else
	#define REALIGN
#endif

#define MAX_PATH 260

#define BOOL int32_t
#define false 0
#define true 1

typedef uint32_t (STDCALL *WindowProc)(MAYBE_THIS void *hWnd, uint32_t uMsg, uint32_t wParam, uint32_t lParam);

char *convertFilePath(const char *srcPth, BOOL convToLower);
const char *getSettingsDir(void);

/* Graphics enhancements (OpenGL 2 renderer) */
typedef struct
{
	float renderScale;        /* Internal resolution multiplier (supersampling) */
	int32_t msaa;             /* Multisample anti-aliasing samples (0 = off) */
	int32_t anisotropy;       /* Anisotropic filtering level (0/1 = off) */
	BOOL mipmaps;             /* Generate mipmaps for game textures */
	BOOL perPixelFog;         /* Evaluate the Glide fog table per pixel */
	BOOL textureReplace;      /* Load HD textures from "textures" directories */
	BOOL textureDump;         /* Dump game textures as PNG into ~/.nfs2se/texdump */

	BOOL postProcessing;      /* Master switch for the effects below */
	float aoStrength;         /* Screen-space ambient occlusion (0 = off) */
	float aoRadius;           /* SSAO radius in game units */
	float bloomStrength;      /* Bloom (0 = off) */
	float bloomThreshold;
	BOOL toneMapping;         /* Filmic tone mapping and grading */
	float exposure;
	float contrast;
	float saturation;
	float warmth;
	BOOL fxaa;                /* Fast approximate anti-aliasing */
	float sharpen;            /* Contrast adaptive sharpening (0 = off) */
	float vignette;           /* Vignette strength (0 = off) */
	float filmGrain;          /* Film grain strength (0 = off) */
	float reflections;        /* Screen-space reflections on the ground (0 = off) */
	float wetness;            /* Road wetness: 0 = dry (blurry sheen), 1 = soaked (mirror-like), < 0 = automatic (wet while it rains) */
	float sunShafts;          /* Light shafts from bright sky areas (0 = off) */
	float haze;               /* Distance haze tinted by the sky (0 = off) */
	float shadows;            /* Screen-space sun shadows strength (0 = off) */
	float motionBlur;         /* Camera motion blur, fraction of a 1/60 s frame (0 = off) */
	float rainSpray;          /* Spray behind fast cars while it rains (0 = off) */
	float rainDrops;          /* Raindrops on the camera while it rains (0 = off) */
	float focalLength;        /* Game projection focal length in 640x480 pixels (used by SSAO) */
	BOOL widescreen;          /* Hor+ widescreen: wider field of view on wide windows */
	BOOL hudCorners;          /* Widescreen: bottom HUD boxes (minimap, record) pinned to the screen corners in a race */
	BOOL carModels;           /* Replacement car models from "<settings>/models/<car>/model.cmdl" */
	BOOL skies;               /* High resolution skies from "<settings>/skies/trNN.txt" */
	BOOL terrain;             /* Distant terrain from "<settings>/terrain/trNN/" */
	BOOL trees;               /* 3D trees replacing the billboards listed in "<settings>/trees/trNN.txt" */
} GfxSettings;
extern GfxSettings gfx;
extern int32_t gfxCurrentTrack; /* Current track number, -1 if unknown */

/* Runtime toggles, set from the event thread and consumed by the renderer */
extern volatile BOOL gfxPostToggleRequest;
extern volatile BOOL gfxScreenshotRequest;
extern volatile BOOL gfxCaptureRequest;
extern volatile int32_t gfxDebugViewRequest;

#endif // WRAPPER_H
