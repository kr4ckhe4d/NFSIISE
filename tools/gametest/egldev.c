// SPDX-License-Identifier: MIT
/*
 * Selects the GPU for headless (SDL offscreen) benchmarks: SDL picks the first EGL device, this shim hides all
 * others. Build: gcc -m32 -shared -fPIC -O2 egldev.c -o egldev.so -ldl -lEGL
 * Use:   SDL_EGL_LIBRARY=$PWD/egldev.so EGL_ONLY_NODE=/dev/dri/renderD128 SDL_VIDEODRIVER=offscreen <gfxtest binary> ...
 * (loaded as SDL_EGL_LIBRARY, not LD_PRELOAD: SDL resolves EGL functions with dlsym on its library handle)
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
static PFNEGLQUERYDEVICESEXTPROC realQuery;
static PFNEGLQUERYDEVICESTRINGEXTPROC queryStr;
static EGLBoolean myQuery(EGLint max, EGLDeviceEXT *devs, EGLint *num)
{
	EGLDeviceEXT all[16]; EGLint n = 0, i, k = 0;
	const char *want = getenv("EGL_ONLY_NODE");
	if (!realQuery(16, all, &n)) return EGL_FALSE;
	for (i = 0; i < n; ++i) {
		const char *node = queryStr(all[i], EGL_DRM_RENDER_NODE_FILE_EXT);
		if (node && want && !strcmp(node, want)) { if (devs && k < max) devs[k] = all[i]; ++k; }
	}
	*num = devs ? (k < max ? k : max) : k;
	return EGL_TRUE;
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
	static __eglMustCastToProperFunctionPointerType (*real)(const char *);
	if (!real) real = dlsym(RTLD_NEXT, "eglGetProcAddress");
	if (!strcmp(name, "eglQueryDevicesEXT")) {
		realQuery = (PFNEGLQUERYDEVICESEXTPROC)real(name);
		queryStr = (PFNEGLQUERYDEVICESTRINGEXTPROC)real("eglQueryDeviceStringEXT");
		return (__eglMustCastToProperFunctionPointerType)myQuery;
	}
	return real(name);
}
