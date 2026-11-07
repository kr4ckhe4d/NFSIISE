// SPDX-License-Identifier: MIT

/*
 * Offline test scene for the Glide -> OpenGL 2 renderer.
 *
 * Drives the Glide API the same way the game does (pre-projected vertices with 1/w, table fog, Glide texture
 * formats, 2D overlays) with a synthetic street scene, then saves the presented frame as PNG.
 * No game data is needed. Build and run with tools/gfxtest/run.sh.
 *
 * Usage: gfxtest <output.png> [Key=Value ...]   (keys as in nfs2se.conf, plus DebugView=0..4)
 */

#include "Glide2x.c"

#include <SDL2/SDL.h>
#include <math.h>

#ifndef NEW_RENDERER
# define STB_IMAGE_WRITE_IMPLEMENTATION
# include "Glide2x/thirdparty/stb_image_write.h"
#endif

/* Globals normally provided by Wrapper.c */
BOOL keepAspectRatio = true, needRecreateGl = false, windowResized = false, linearFiltering = true;
BOOL fixedFramebufferSize = false, framebufferLinearFiltering = true;
int32_t vSync = 0, winWidth = 1280, winHeight = 960, initialWinWidth = 640, initialWinHeight = 480;
SDL_Window *sdlWin = NULL;
float dpr = 1.0f;

#ifdef NEW_RENDERER
GfxSettings gfx = {
	1.0f, 0, 16, true, true, true, false,
	true, 1.0f, 2.0f, 0.35f, 1.5f, true, 1.0f, 1.08f, 1.05f, 0.0f, true, 0.35f, 0.2f, 0.0f, 320.0f
};
volatile BOOL gfxPostToggleRequest = false;
volatile BOOL gfxScreenshotRequest = false;
volatile int32_t gfxDebugViewRequest = -1;
static char g_settingsDir[512] = "";
const char *getSettingsDir(void)
{
	return g_settingsDir[0] ? g_settingsDir : NULL;
}
#endif

/* ---- Tiny math ---- */

typedef struct { float x, y, z; } Vec3;
static Vec3 v3(float x, float y, float z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static Vec3 vsub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec3 vcross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 vnorm(Vec3 a) { float l = sqrtf(vdot(a, a)); return l > 0 ? v3(a.x / l, a.y / l, a.z / l) : a; }

#define FOCAL 320.0f
#define NEAR_Z 0.6f

static Vec3 g_camPos;
static float g_camPitch = 0.07f, g_camYaw = 0.0f;
static Vec3 g_sun;

typedef struct { float x, y, z, u, v, r, g, b, a; } VV; /* View-space vertex */

static Vec3 toView(Vec3 p)
{
	Vec3 d = vsub(p, g_camPos), r;
	float cy = cosf(g_camYaw), sy = sinf(g_camYaw), cp = cosf(g_camPitch), sp = sinf(g_camPitch);
	float x = d.x * cy - d.z * sy;
	float z = d.x * sy + d.z * cy;
	r.x = x;
	r.y = d.y * cp + z * sp;
	r.z = -d.y * sp + z * cp;
	return r;
}

static void project(const VV *in, GrVertex *out)
{
	float oow = 1.0f / in->z;
	memset(out, 0, sizeof *out);
	out->x = 320.0f + FOCAL * in->x * oow + VertexSnap;
	out->y = 240.0f - FOCAL * in->y * oow + VertexSnap;
	out->oow = oow;
	out->r = in->r;
	out->g = in->g;
	out->b = in->b;
	out->a = in->a;
	out->tmuvtx[0].sow = in->u * 256.0f * oow;
	out->tmuvtx[0].tow = in->v * 256.0f * oow;
	out->tmuvtx[0].oow = oow;
}

static VV lerpVV(const VV *a, const VV *b, float t)
{
	VV r;
	r.x = a->x + (b->x - a->x) * t; r.y = a->y + (b->y - a->y) * t; r.z = a->z + (b->z - a->z) * t;
	r.u = a->u + (b->u - a->u) * t; r.v = a->v + (b->v - a->v) * t;
	r.r = a->r + (b->r - a->r) * t; r.g = a->g + (b->g - a->g) * t; r.b = a->b + (b->b - a->b) * t; r.a = a->a + (b->a - a->a) * t;
	return r;
}

/* Clips a view-space triangle against the near plane and draws it */
static void drawTriVV(VV a, VV b, VV c)
{
	VV in[3], out[4];
	int n = 0, i;
	in[0] = a; in[1] = b; in[2] = c;
	for (i = 0; i < 3; ++i)
	{
		const VV *p = &in[i], *q = &in[(i + 1) % 3];
		BOOL pin = p->z >= NEAR_Z, qin = q->z >= NEAR_Z;
		if (pin)
			out[n++] = *p;
		if (pin != qin)
			out[n++] = lerpVV(p, q, (NEAR_Z - p->z) / (q->z - p->z));
	}
	for (i = 1; i + 1 < n; ++i)
	{
		GrVertex g0, g1, g2;
		project(&out[0], &g0);
		project(&out[i], &g1);
		project(&out[i + 1], &g2);
		grDrawTriangle(&g0, &g1, &g2);
	}
}

static float g_tint[3] = {1, 1, 1};

/* World-space quad with baked lambert lighting (like the game's pre-lit geometry) */
static void quad(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float u0, float v0, float u1, float v1, float ambient, float alpha)
{
	Vec3 n = vnorm(vcross(vsub(p1, p0), vsub(p3, p0)));
	float l = ambient + (1.0f - ambient) * SDL_max(0.0f, vdot(n, g_sun));
	float c = SDL_min(255.0f, 255.0f * l);
	VV v[4];
	Vec3 p[4];
	float uv[4][2];
	int i;
	p[0] = p0; p[1] = p1; p[2] = p2; p[3] = p3;
	uv[0][0] = u0; uv[0][1] = v0;
	uv[1][0] = u1; uv[1][1] = v0;
	uv[2][0] = u1; uv[2][1] = v1;
	uv[3][0] = u0; uv[3][1] = v1;
	for (i = 0; i < 4; ++i)
	{
		Vec3 vp = toView(p[i]);
		v[i].x = vp.x; v[i].y = vp.y; v[i].z = vp.z;
		v[i].u = uv[i][0]; v[i].v = uv[i][1];
		v[i].r = c * g_tint[0]; v[i].g = c * g_tint[1]; v[i].b = c * g_tint[2]; v[i].a = alpha;
	}
	drawTriVV(v[0], v[1], v[2]);
	drawTriVV(v[0], v[2], v[3]);
}

/* Axis-aligned box (5 faces, no bottom) with one texture */
static void box(float x0, float y0, float z0, float x1, float y1, float z1, float urep, float vrep, float ambient)
{
	quad(v3(x0, y1, z0), v3(x1, y1, z0), v3(x1, y0, z0), v3(x0, y0, z0), 0, 0, urep, vrep, ambient, 255); /* front (-z) */
	quad(v3(x1, y1, z1), v3(x0, y1, z1), v3(x0, y0, z1), v3(x1, y0, z1), 0, 0, urep, vrep, ambient, 255); /* back */
	quad(v3(x0, y1, z1), v3(x0, y1, z0), v3(x0, y0, z0), v3(x0, y0, z1), 0, 0, urep, vrep, ambient, 255); /* left */
	quad(v3(x1, y1, z0), v3(x1, y1, z1), v3(x1, y0, z1), v3(x1, y0, z0), 0, 0, urep, vrep, ambient, 255); /* right */
	quad(v3(x0, y1, z1), v3(x1, y1, z1), v3(x1, y1, z0), v3(x0, y1, z0), 0, 0, 1, 1, ambient, 255);       /* top */
}

/* Camera-facing billboard */
static void billboard(Vec3 base, float w, float h, float alpha, float bright)
{
	Vec3 c = toView(base);
	VV v[4];
	int i;
	for (i = 0; i < 4; ++i)
	{
		v[i].z = c.z;
		v[i].r = v[i].g = v[i].b = 255.0f * bright;
		v[i].a = alpha;
	}
	v[0].x = c.x - w / 2; v[0].y = c.y + h; v[0].u = 0; v[0].v = 0;
	v[1].x = c.x + w / 2; v[1].y = c.y + h; v[1].u = 1; v[1].v = 0;
	v[2].x = c.x + w / 2; v[2].y = c.y;     v[2].u = 1; v[2].v = 1;
	v[3].x = c.x - w / 2; v[3].y = c.y;     v[3].u = 0; v[3].v = 1;
	/* Slightly tilt so oow is not constant (the game's sprites are not exactly camera-parallel either) */
	v[0].z += 0.02f; v[1].z += 0.02f;
	drawTriVV(v[0], v[1], v[2]);
	drawTriVV(v[0], v[2], v[3]);
}

/* 2D overlay quad in 640x480 screen coordinates */
static void hudQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	GrVertex q[4];
	int i;
	float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1}, us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
	for (i = 0; i < 4; ++i)
	{
		memset(&q[i], 0, sizeof q[i]);
		q[i].x = xs[i] + VertexSnap;
		q[i].y = ys[i] + VertexSnap;
		q[i].oow = 1.0f;
		q[i].r = r; q[i].g = g; q[i].b = b; q[i].a = a;
		q[i].tmuvtx[0].sow = us[i] * 256.0f;
		q[i].tmuvtx[0].tow = vs[i] * 256.0f;
		q[i].tmuvtx[0].oow = 1.0f;
	}
	grDrawTriangle(&q[0], &q[1], &q[2]);
	grDrawTriangle(&q[0], &q[2], &q[3]);
}

/* ---- Procedural textures (Glide formats) ---- */

static uint32_t g_seed = 1234567;
static float frand() { g_seed = g_seed * 1664525u + 1013904223u; return (g_seed >> 8) / 16777216.0f; }

static uint16_t rgb565(float r, float g, float b)
{
	int R = (int)(SDL_max(0, SDL_min(1, r)) * 31 + 0.5f), G = (int)(SDL_max(0, SDL_min(1, g)) * 63 + 0.5f), B = (int)(SDL_max(0, SDL_min(1, b)) * 31 + 0.5f);
	return (R << 11) | (G << 5) | B;
}
static uint16_t argb1555(int a, float r, float g, float b)
{
	int R = (int)(SDL_max(0, SDL_min(1, r)) * 31 + 0.5f), G = (int)(SDL_max(0, SDL_min(1, g)) * 31 + 0.5f), B = (int)(SDL_max(0, SDL_min(1, b)) * 31 + 0.5f);
	return (a ? 0x8000 : 0) | (R << 10) | (G << 5) | B;
}
static uint16_t argb4444(float a, float r, float g, float b)
{
	int A = (int)(SDL_max(0, SDL_min(1, a)) * 15 + 0.5f), R = (int)(SDL_max(0, SDL_min(1, r)) * 15 + 0.5f);
	int G = (int)(SDL_max(0, SDL_min(1, g)) * 15 + 0.5f), B = (int)(SDL_max(0, SDL_min(1, b)) * 15 + 0.5f);
	return (A << 12) | (R << 8) | (G << 4) | B;
}

enum { TEX_ROAD, TEX_GRASS, TEX_FACADE, TEX_FACADE2, TEX_TREE, TEX_CAR, TEX_SHADOW, TEX_GLOW, TEX_SKY, TEX_FONT, TEX_PANEL, TEX_COUNT };
static uint32_t g_texAddr[TEX_COUNT];
static GrTexInfo g_texInfo[TEX_COUNT];
static uint16_t g_texData[TEX_COUNT][128 * 128];

static void makeTexture(int id, int size, GrTextureFormat_t fmt, uint32_t addr)
{
	GrLOD_t lod = size == 128 ? GR_LOD_128 : (size == 64 ? GR_LOD_64 : GR_LOD_32);
	g_texInfo[id].smallLod = lod;
	g_texInfo[id].largeLod = lod;
	g_texInfo[id].aspectRatio = GR_ASPECT_1x1;
	g_texInfo[id].format = fmt;
	g_texInfo[id].data = g_texData[id];
	g_texAddr[id] = addr;
}

static void genTextures()
{
	int x, y, i;
	uint32_t addr = 0;
	uint16_t *t;

	/* Asphalt with center dashes and edge line */
	t = g_texData[TEX_ROAD];
	for (y = 0; y < 128; ++y)
		for (x = 0; x < 128; ++x)
		{
			float n = 0.27f + 0.06f * frand() + 0.03f * sinf(x * 0.7f) * sinf(y * 0.5f);
			float r = n, g = n, b = n * 1.03f;
			if (abs(x - 64) <= 1 && (y % 64) < 36)
				r = g = b = 0.85f;
			if (x <= 3 || x >= 124)
				r = g = 0.82f, b = 0.7f;
			t[y * 128 + x] = rgb565(r, g, b);
		}
	makeTexture(TEX_ROAD, 128, GR_TEXFMT_RGB_565, addr); addr += 128 * 128 * 2;

	t = g_texData[TEX_GRASS];
	for (y = 0; y < 64; ++y)
		for (x = 0; x < 64; ++x)
		{
			float n = frand();
			t[y * 64 + x] = rgb565(0.18f + 0.1f * n, 0.36f + 0.16f * n, 0.12f + 0.05f * n);
		}
	makeTexture(TEX_GRASS, 64, GR_TEXFMT_RGB_565, addr); addr += 64 * 64 * 2;

	for (i = 0; i < 2; ++i)
	{
		float wr = i ? 0.62f : 0.72f, wg = i ? 0.38f : 0.66f, wb = i ? 0.30f : 0.56f;
		t = g_texData[TEX_FACADE + i];
		for (y = 0; y < 128; ++y)
			for (x = 0; x < 128; ++x)
			{
				int wx = x % 32, wy = y % 32;
				float n = 0.92f + 0.08f * frand();
				float r = wr * n, g = wg * n, b = wb * n;
				if (wx >= 8 && wx < 24 && wy >= 6 && wy < 26)
				{
					float refl = 0.25f + 0.25f * (wy - 6) / 20.0f;
					r = 0.12f + refl * 0.5f; g = 0.16f + refl * 0.6f; b = 0.24f + refl;
					if (wx == 16 || wy == 16)
						r = g = b = 0.85f;
				}
				else if (wx >= 6 && wx < 26 && wy >= 4 && wy < 28)
					r = g = b = 0.86f;
				t[y * 128 + x] = rgb565(r, g, b);
			}
		makeTexture(TEX_FACADE + i, 128, GR_TEXFMT_RGB_565, addr); addr += 128 * 128 * 2;
	}

	/* Tree: trunk + noisy canopy, binary alpha */
	t = g_texData[TEX_TREE];
	for (y = 0; y < 128; ++y)
		for (x = 0; x < 128; ++x)
		{
			float dx = (x - 64) / 52.0f, dy = (y - 50) / 48.0f;
			float d = dx * dx + dy * dy + 0.35f * (frand() - 0.5f);
			int a = 0;
			float r = 0, g = 0, b = 0;
			if (d < 1.0f && y < 100)
			{
				float n = frand();
				a = 1; r = 0.12f + 0.1f * n; g = 0.30f + 0.2f * n - 0.1f * dy; b = 0.08f + 0.05f * n;
			}
			else if (abs(x - 64) < 5 && y >= 70)
			{
				a = 1; r = 0.30f; g = 0.2f; b = 0.12f;
			}
			t[y * 128 + x] = argb1555(a, r, g, b);
		}
	makeTexture(TEX_TREE, 128, GR_TEXFMT_ARGB_1555, addr); addr += 128 * 128 * 2;

	/* Car paint with windows */
	t = g_texData[TEX_CAR];
	for (y = 0; y < 64; ++y)
		for (x = 0; x < 64; ++x)
		{
			float s = 0.85f + 0.15f * (1.0f - y / 64.0f);
			float r = 0.75f * s, g = 0.06f * s, b = 0.06f * s;
			if (y < 22 && x > 6 && x < 58)
				r = 0.08f, g = 0.1f, b = 0.14f + 0.1f * (y / 22.0f);
			t[y * 64 + x] = rgb565(r, g, b);
		}
	makeTexture(TEX_CAR, 64, GR_TEXFMT_RGB_565, addr); addr += 64 * 64 * 2;

	/* Shadow blob and light glow (translucent 4444) */
	t = g_texData[TEX_SHADOW];
	for (y = 0; y < 64; ++y)
		for (x = 0; x < 64; ++x)
		{
			float dx = (x - 31.5f) / 32.0f, dy = (y - 31.5f) / 32.0f;
			float d = SDL_max(0.0f, 1.0f - (dx * dx + dy * dy));
			t[y * 64 + x] = argb4444(d * 0.8f, 0, 0, 0);
		}
	makeTexture(TEX_SHADOW, 64, GR_TEXFMT_ARGB_4444, addr); addr += 64 * 64 * 2;
	t = g_texData[TEX_GLOW];
	for (y = 0; y < 64; ++y)
		for (x = 0; x < 64; ++x)
		{
			float dx = (x - 31.5f) / 32.0f, dy = (y - 31.5f) / 32.0f;
			float d = SDL_max(0.0f, 1.0f - sqrtf(dx * dx + dy * dy));
			t[y * 64 + x] = argb4444(d * d, 1.0f, 0.9f, 0.7f);
		}
	makeTexture(TEX_GLOW, 64, GR_TEXFMT_ARGB_4444, addr); addr += 64 * 64 * 2;

	/* Sky gradient with sun */
	t = g_texData[TEX_SKY];
	for (y = 0; y < 128; ++y)
		for (x = 0; x < 128; ++x)
		{
			float h = y / 127.0f; /* 0 = top */
			float r = 0.30f + 0.55f * h, g = 0.50f + 0.40f * h, b = 0.85f + 0.12f * h;
			float dx = (x - 40) / 128.0f, dy = (y - 70) / 128.0f;
			float sun = expf(-(dx * dx + dy * dy) * 900.0f);
			r += sun; g += sun; b += sun * 0.8f;
			t[y * 128 + x] = rgb565(r, g, b);
		}
	makeTexture(TEX_SKY, 128, GR_TEXFMT_RGB_565, addr); addr += 128 * 128 * 2;

	/* "Font": 8x8 grid of block glyphs */
	t = g_texData[TEX_FONT];
	for (y = 0; y < 128; ++y)
		for (x = 0; x < 128; ++x)
		{
			int gx = x % 16, gy = y % 16, glyph = (y / 16) * 8 + x / 16;
			int on = (gx >= 3 && gx <= 12 && gy >= 2 && gy <= 13) &&
			         ((gx <= 4 || gx >= 11) ? ((glyph >> (gy / 4)) & 1) || gy < 4 : (gy <= 3 || gy >= 12 || (gy >= 7 && gy <= 8 && (glyph & 4))));
			t[y * 128 + x] = argb1555(on, 1, 1, 1);
		}
	makeTexture(TEX_FONT, 128, GR_TEXFMT_ARGB_1555, addr); addr += 128 * 128 * 2;

	t = g_texData[TEX_PANEL];
	for (y = 0; y < 32; ++y)
		for (x = 0; x < 32; ++x)
			t[y * 32 + x] = argb4444((x == 0 || y == 0 || x == 31 || y == 31) ? 0.9f : 0.55f, 0.05f, 0.08f, 0.12f);
	makeTexture(TEX_PANEL, 32, GR_TEXFMT_ARGB_4444, addr); addr += 32 * 32 * 2;

	for (i = 0; i < TEX_COUNT; ++i)
		grTexDownloadMipMap(GR_TMU0, g_texAddr[i], 3, &g_texInfo[i]);
}

static void useTexture(int id)
{
	grTexSource(GR_TMU0, g_texAddr[id], 3, &g_texInfo[id]);
	grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, false);
}

/* ---- Scene ---- */

static void drawScene()
{
	int i;
	static GrFog_t fogTable[GR_FOG_TABLE_SIZE];
	float carZ = 9.0f;

	g_sun = vnorm(v3(-0.55f, 0.75f, -0.35f));
	g_camPos = v3(0.6f, 1.55f, 0.0f);

	grClipWindow(0, 0, 640, 480);
	grDepthMask(true);
	grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
	grBufferClear(0x00A8C4E0, 0, 0xFFFF);

	/* Sky: cylinder segments, no fog */
	grFogMode(GR_FOG_DISABLE);
	useTexture(TEX_SKY);
	for (i = -4; i < 4; ++i)
	{
		float a0 = i * 0.22f, a1 = (i + 1) * 0.22f, R = 900.0f;
		quad(v3(sinf(a0) * R + g_camPos.x, 420, cosf(a0) * R), v3(sinf(a1) * R + g_camPos.x, 420, cosf(a1) * R),
		     v3(sinf(a1) * R + g_camPos.x, -30, cosf(a1) * R), v3(sinf(a0) * R + g_camPos.x, -30, cosf(a0) * R),
		     (i + 4) / 8.0f, 0, (i + 5) / 8.0f, 1, 1.0f, 255);
	}

	/* World with table fog */
	guFogGenerateExp(fogTable, 0.0065f);
	grFogTable(fogTable);
	grFogColorValue(0x00B4C8DC);
	grFogMode(GR_FOG_WITH_TABLE);

	/* Ground */
	useTexture(TEX_GRASS);
	for (i = 0; i < 40; ++i)
	{
		float z0 = i * 10.0f, z1 = z0 + 10.0f;
		quad(v3(-60, 0, z1), v3(-5, 0, z1), v3(-5, 0, z0), v3(-60, 0, z0), 0, 0, 1, 1, 0.55f, 255);
		quad(v3(5, 0, z1), v3(60, 0, z1), v3(60, 0, z0), v3(5, 0, z0), 0, 0, 1, 1, 0.55f, 255);
	}
	useTexture(TEX_ROAD);
	for (i = 0; i < 40; ++i)
	{
		float z0 = i * 10.0f, z1 = z0 + 10.0f;
		quad(v3(-5, 0, z1), v3(5, 0, z1), v3(5, 0, z0), v3(-5, 0, z0), 0, 0, 1, 1, 0.55f, 255);
	}
	/* Sidewalk curbs */
	g_tint[0] = 0.8f; g_tint[1] = 0.8f; g_tint[2] = 0.82f;
	useTexture(TEX_GRASS);
	grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_ITERATED, false);
	for (i = 0; i < 40; ++i)
	{
		box(-7.5f, 0, i * 10.0f, -5.0f, 0.18f, i * 10.0f + 10.0f, 1, 1, 0.5f);
		box(5.0f, 0, i * 10.0f, 7.5f, 0.18f, i * 10.0f + 10.0f, 1, 1, 0.5f);
	}
	g_tint[0] = g_tint[1] = g_tint[2] = 1.0f;

	/* Buildings */
	for (i = 0; i < 16; ++i)
	{
		float z0 = 6.0f + i * 22.0f, h = 9.0f + 7.0f * ((i * 37) % 5);
		useTexture(i & 1 ? TEX_FACADE : TEX_FACADE2);
		box(-24.0f, 0, z0, -9.0f, h, z0 + 18.0f, 3, h / 6.0f, 0.42f);
		useTexture(i & 1 ? TEX_FACADE2 : TEX_FACADE);
		box(9.5f, 0, z0 + 8.0f, 24.0f, h + 4.0f, z0 + 24.0f, 3, (h + 4.0f) / 6.0f, 0.42f);
	}

	/* Car (body, cabin, wheels) */
	useTexture(TEX_SHADOW);
	quad(v3(-0.1f, 0.02f, carZ + 4.8f), v3(2.5f, 0.02f, carZ + 4.8f), v3(2.5f, 0.02f, carZ - 0.4f), v3(-0.1f, 0.02f, carZ - 0.4f), 0, 0, 1, 1, 1.0f, 255);
	useTexture(TEX_CAR);
	box(0.15f, 0.35f, carZ, 2.25f, 1.0f, carZ + 4.4f, 1, 1, 0.5f);
	box(0.35f, 1.0f, carZ + 1.2f, 2.05f, 1.45f, carZ + 3.3f, 1, 1, 0.5f);
	grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_ITERATED, false);
	g_tint[0] = g_tint[1] = g_tint[2] = 0.12f;
	box(0.05f, 0.0f, carZ + 0.5f, 0.35f, 0.62f, carZ + 1.3f, 1, 1, 0.5f);
	box(2.05f, 0.0f, carZ + 0.5f, 2.35f, 0.62f, carZ + 1.3f, 1, 1, 0.5f);
	g_tint[0] = g_tint[1] = g_tint[2] = 1.0f;

	/* Trees (cut-out sprites) */
	useTexture(TEX_TREE);
	for (i = 0; i < 18; ++i)
	{
		billboard(v3(-6.8f, 0, 14.0f + i * 19.0f), 4.5f, 6.0f, 255, 0.9f);
		billboard(v3(7.0f, 0, 22.0f + i * 19.0f), 4.5f, 6.0f, 255, 0.9f);
	}

	/* Emissive: tail lights and street lamps (additive) */
	grDepthMask(false);
	grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ZERO);
	useTexture(TEX_GLOW);
	g_tint[0] = 1.0f; g_tint[1] = 0.15f; g_tint[2] = 0.1f;
	quad(v3(0.15f, 0.95f, carZ - 0.02f), v3(0.65f, 0.95f, carZ - 0.02f), v3(0.65f, 0.65f, carZ - 0.02f), v3(0.15f, 0.65f, carZ - 0.02f), 0, 0, 1, 1, 1.0f, 255);
	quad(v3(1.75f, 0.95f, carZ - 0.02f), v3(2.25f, 0.95f, carZ - 0.02f), v3(2.25f, 0.65f, carZ - 0.02f), v3(1.75f, 0.65f, carZ - 0.02f), 0, 0, 1, 1, 1.0f, 255);
	g_tint[0] = g_tint[1] = g_tint[2] = 1.0f;
	for (i = 0; i < 10; ++i)
		billboard(v3(-5.6f, 5.0f, 30.0f + i * 36.0f), 1.6f, 1.6f, 255, 1.0f);
	grDepthMask(true);
	grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);

	/* HUD overlay */
	grFogMode(GR_FOG_DISABLE);
	useTexture(TEX_PANEL);
	hudQuad(452, 380, 628, 468, 0, 0, 1, 1, 255, 255, 255, 255);
	useTexture(TEX_FONT);
	for (i = 0; i < 3; ++i)
		hudQuad(470 + i * 34, 392, 502 + i * 34, 432, (i * 3 % 8) / 8.0f, 0, (i * 3 % 8 + 1) / 8.0f, 1 / 8.0f, 255, 220, 120, 255);
	for (i = 0; i < 6; ++i)
		hudQuad(20 + i * 18, 16, 36 + i * 18, 36, (i % 8) / 8.0f, 2 / 8.0f, (i % 8 + 1) / 8.0f, 3 / 8.0f, 255, 255, 255, 255);
	grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_ITERATED, false);
	hudQuad(470, 446, 470 + 140 * 0.7f, 456, 0, 0, 1, 1, 90, 230, 90, 230);
}

static void savePNG(const char *path)
{
	uint8_t *px = (uint8_t *)malloc(winWidth * winHeight * 4);
	int y;
	glFinish();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, winWidth, winHeight, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (y = 0; y < winHeight / 2; ++y)
	{
		uint8_t tmp[16384];
		memcpy(tmp, px + y * winWidth * 4, winWidth * 4);
		memcpy(px + y * winWidth * 4, px + (winHeight - 1 - y) * winWidth * 4, winWidth * 4);
		memcpy(px + (winHeight - 1 - y) * winWidth * 4, tmp, winWidth * 4);
	}
	for (y = 0; y < winWidth * winHeight; ++y)
		px[y * 4 + 3] = 255;
	stbi_write_png(path, winWidth, winHeight, 4, px, winWidth * 4);
	free(px);
	glReadBuffer(GL_BACK);
}

#ifdef NEW_RENDERER
static void applySetting(const char *kv)
{
	char key[64];
	const char *eq = strchr(kv, '=');
	float v;
	if (!eq || eq - kv >= (int)sizeof key)
		return;
	memcpy(key, kv, eq - kv);
	key[eq - kv] = '\0';
	v = (float)atof(eq + 1);
	if (!strcasecmp(key, "RenderScale")) gfx.renderScale = v;
	else if (!strcasecmp(key, "MSAA")) gfx.msaa = (int)v;
	else if (!strcasecmp(key, "AnisotropicFiltering")) gfx.anisotropy = (int)v;
	else if (!strcasecmp(key, "Mipmaps")) gfx.mipmaps = v != 0;
	else if (!strcasecmp(key, "PerPixelFog")) gfx.perPixelFog = v != 0;
	else if (!strcasecmp(key, "PostProcessing")) gfx.postProcessing = v != 0;
	else if (!strcasecmp(key, "AmbientOcclusion")) gfx.aoStrength = v;
	else if (!strcasecmp(key, "AmbientOcclusionRadius")) gfx.aoRadius = v;
	else if (!strcasecmp(key, "Bloom")) gfx.bloomStrength = v;
	else if (!strcasecmp(key, "BloomThreshold")) gfx.bloomThreshold = v;
	else if (!strcasecmp(key, "ToneMapping")) gfx.toneMapping = v != 0;
	else if (!strcasecmp(key, "Exposure")) gfx.exposure = v;
	else if (!strcasecmp(key, "Contrast")) gfx.contrast = v;
	else if (!strcasecmp(key, "Saturation")) gfx.saturation = v;
	else if (!strcasecmp(key, "Warmth")) gfx.warmth = v;
	else if (!strcasecmp(key, "FXAA")) gfx.fxaa = v != 0;
	else if (!strcasecmp(key, "Sharpen")) gfx.sharpen = v;
	else if (!strcasecmp(key, "Vignette")) gfx.vignette = v;
	else if (!strcasecmp(key, "FilmGrain")) gfx.filmGrain = v;
	else if (!strcasecmp(key, "TextureDump")) gfx.textureDump = v != 0;
	else if (!strcasecmp(key, "SettingsDir")) snprintf(g_settingsDir, sizeof g_settingsDir, "%s", eq + 1);
	else if (!strcasecmp(key, "DebugView")) g_post.debugView = (int)v;
	else fprintf(stderr, "Unknown setting: %s\n", key);
}
#endif

int main(int argc, char **argv)
{
	int i, frames = 3;
	const char *out = argc > 1 ? argv[1] : "gfxtest.png";
	int32_t debugView = 0;
	Uint64 t0, t1;

#ifdef NEW_RENDERER
	for (i = 2; i < argc; ++i)
	{
		if (!strncasecmp(argv[i], "DebugView=", 10))
			debugView = atoi(argv[i] + 10);
		else
			applySetting(argv[i]);
	}
#endif
	if (getenv("GFXTEST_SIZE"))
		sscanf(getenv("GFXTEST_SIZE"), "%dx%d", &winWidth, &winHeight);

	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	sdlWin = SDL_CreateWindow("gfxtest", 0, 0, winWidth, winHeight, SDL_WINDOW_OPENGL);
	if (!sdlWin)
	{
		fprintf(stderr, "Cannot create window: %s\n", SDL_GetError());
		return 1;
	}

	grGlideInit();
	grSstWinOpen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz, 0, 0, 2, 1);
	grGammaCorrectionValue(1.0f);
	printf("GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	genTextures();

#ifdef NEW_RENDERER
	g_post.debugView = debugView;
#endif
	if (getenv("GFXTEST_FRAMES"))
		frames = atoi(getenv("GFXTEST_FRAMES"));

	t0 = SDL_GetPerformanceCounter();
	for (i = 0; i < frames; ++i)
	{
		drawScene();
		grBufferSwap(0);
	}
	glFinish();
	t1 = SDL_GetPerformanceCounter();
	printf("%d frames, %.2f ms/frame\n", frames, (t1 - t0) * 1000.0 / SDL_GetPerformanceFrequency() / frames);

	savePNG(out);
	printf("Saved %s\n", out);

	grGlideShutdown();
	SDL_DestroyWindow(sdlWin);
	SDL_Quit();
	return 0;
}
