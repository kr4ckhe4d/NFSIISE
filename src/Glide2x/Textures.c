// SPDX-License-Identifier: MIT

/*
 * Texture pipeline for the OpenGL 2 renderer:
 *  - converts every Glide texture format to RGBA8,
 *  - identifies textures by a content hash (stable between runs, used as the file name for HD texture packs),
 *  - caches GL textures by that hash, so re-downloaded textures are not processed again,
 *  - dumps textures as PNG ("TextureDump=1") and loads HD replacements ("TextureReplace=1"),
 *  - generates mipmaps on CPU with alpha-weighted colors and coverage-preserving alpha for cut-out textures,
 *  - enables trilinear and anisotropic filtering.
 *
 * This file is included from OpenGL2.c.
 */

#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wunused-function"
# pragma GCC diagnostic ignored "-Wunused-but-set-variable"
# pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_NO_GIF
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "thirdparty/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_SIMD
#include "thirdparty/stb_image_write.h"
#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif

#include <sys/stat.h>

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
# define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
# define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
# define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

enum
{
	AlphaOpaque = 0,      /* Every texel has alpha == 255 */
	AlphaCutout = 1,      /* Every texel has alpha 0 or 255 */
	AlphaTranslucent = 2, /* Anything else */
};

typedef struct
{
	uint64_t hash;
	GLuint id;
	int32_t refs;
	uint32_t lastUse;
	uint8_t alphaMode;
} TexCacheEntry;

static TexCacheEntry *g_texCache;
static uint32_t g_texCacheCount, g_texCacheCapacity;
static uint32_t g_texFrame;
static float g_maxAnisotropy;
static GLint g_maxTextureSize;
static BOOL g_texCapsQueried;

#define TexCacheMaxUnused 768

static uint8_t g_rgbaTmp[256 * 256 * 4];

/* Hashing */

static uint64_t texHash(const uint8_t *rgba, uint32_t w, uint32_t h)
{
	/* Two independent 32-bit lanes over 32-bit words - fast on x86-32 and good enough to identify textures */
	const uint32_t *words = (const uint32_t *)rgba;
	uint32_t n = w * h, i;
	uint32_t h1 = 0x811C9DC5u ^ w, h2 = 0x9E3779B9u ^ (h << 16);
	for (i = 0; i < n; ++i)
	{
		uint32_t v = words[i];
		h1 = (h1 ^ v) * 0x01000193u;
		h2 = ((h2 ^ v) * 0x85EBCA6Bu);
		h2 = (h2 << 13) | (h2 >> 19);
	}
	h1 ^= h1 >> 16; h1 *= 0x7FEB352Du; h1 ^= h1 >> 15;
	h2 ^= h2 >> 16; h2 *= 0x846CA68Bu; h2 ^= h2 >> 16;
	return ((uint64_t)h1 << 32) | h2;
}
static void texHashName(uint64_t hash, char name[17])
{
	sprintf(name, "%08X%08X", (uint32_t)(hash >> 32), (uint32_t)hash);
}

/* Format conversion (Glide data is little-endian ARGB) */

static void texConvertToRGBA(const void *data, GrTextureFormat_t fmt, uint32_t size, const uint32_t *palette, uint8_t *out)
{
	uint32_t n = size * size, i;
	switch (fmt)
	{
		case GR_TEXFMT_RGB_565:
		{
			const uint16_t *in = (const uint16_t *)data;
			for (i = 0; i < n; ++i)
			{
				uint32_t v = in[i];
				uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
				out[i * 4 + 0] = (r << 3) | (r >> 2);
				out[i * 4 + 1] = (g << 2) | (g >> 4);
				out[i * 4 + 2] = (b << 3) | (b >> 2);
				out[i * 4 + 3] = 255;
			}
			break;
		}
		case GR_TEXFMT_ARGB_1555:
		{
			const uint16_t *in = (const uint16_t *)data;
			for (i = 0; i < n; ++i)
			{
				uint32_t v = in[i];
				uint32_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
				out[i * 4 + 0] = (r << 3) | (r >> 2);
				out[i * 4 + 1] = (g << 3) | (g >> 2);
				out[i * 4 + 2] = (b << 3) | (b >> 2);
				out[i * 4 + 3] = (v & 0x8000) ? 255 : 0;
			}
			break;
		}
		case GR_TEXFMT_ARGB_4444:
		{
			const uint16_t *in = (const uint16_t *)data;
			for (i = 0; i < n; ++i)
			{
				uint32_t v = in[i];
				out[i * 4 + 0] = ((v >> 8) & 0xF) * 17;
				out[i * 4 + 1] = ((v >> 4) & 0xF) * 17;
				out[i * 4 + 2] = (v & 0xF) * 17;
				out[i * 4 + 3] = (v >> 12) * 17;
			}
			break;
		}
		case GR_TEXFMT_P_8:
		{
			const uint8_t *in = (const uint8_t *)data;
			for (i = 0; i < n; ++i)
			{
				uint32_t v = palette[in[i]];
				out[i * 4 + 0] = (v >> 16) & 0xFF;
				out[i * 4 + 1] = (v >>  8) & 0xFF;
				out[i * 4 + 2] = v & 0xFF;
				out[i * 4 + 3] = v >> 24;
			}
			break;
		}
		default:
			memset(out, 0xFF, n * 4);
			break;
	}
}

static uint8_t texAlphaMode(const uint8_t *rgba, uint32_t n)
{
	uint8_t mode = AlphaOpaque;
	uint32_t i;
	for (i = 0; i < n; ++i)
	{
		uint8_t a = rgba[i * 4 + 3];
		if (a == 255)
			continue;
		if (a != 0)
			return AlphaTranslucent;
		mode = AlphaCutout;
	}
	return mode;
}

/* Fills color of fully transparent texels from their neighbours, so filtering doesn't bleed black/garbage colors */
static void texDilate(uint8_t *rgba, uint32_t w, uint32_t h)
{
	uint8_t *filled = (uint8_t *)malloc(w * h);
	uint32_t pass, x, y, i, remaining = 0;
	if (!filled)
		return;
	for (i = 0; i < w * h; ++i)
	{
		filled[i] = rgba[i * 4 + 3] != 0;
		remaining += !filled[i];
	}
	for (pass = 0; pass < 16 && remaining > 0 && remaining < w * h; ++pass)
	{
		uint32_t changed = 0;
		for (y = 0; y < h; ++y)
		{
			for (x = 0; x < w; ++x)
			{
				uint32_t idx = y * w + x, cnt = 0, r = 0, g = 0, b = 0;
				int32_t dx, dy;
				if (filled[idx])
					continue;
				for (dy = -1; dy <= 1; ++dy)
				{
					for (dx = -1; dx <= 1; ++dx)
					{
						int32_t nx = (int32_t)x + dx, ny = (int32_t)y + dy;
						uint32_t nIdx;
						if (nx < 0 || ny < 0 || nx >= (int32_t)w || ny >= (int32_t)h)
							continue;
						nIdx = ny * w + nx;
						if (filled[nIdx] != 1)
							continue;
						r += rgba[nIdx * 4 + 0];
						g += rgba[nIdx * 4 + 1];
						b += rgba[nIdx * 4 + 2];
						++cnt;
					}
				}
				if (cnt)
				{
					rgba[idx * 4 + 0] = r / cnt;
					rgba[idx * 4 + 1] = g / cnt;
					rgba[idx * 4 + 2] = b / cnt;
					filled[idx] = 2; /* Filled in this pass, becomes a source in the next one */
					++changed;
				}
			}
		}
		for (i = 0; i < w * h; ++i)
			if (filled[i] == 2)
				filled[i] = 1;
		remaining -= changed;
		if (!changed)
			break;
	}
	free(filled);
}

static uint32_t texCoverage(const uint8_t *rgba, uint32_t n, float scale, float threshold)
{
	uint32_t i, cnt = 0;
	for (i = 0; i < n; ++i)
		if (rgba[i * 4 + 3] * scale >= threshold * 255.0f)
			++cnt;
	return cnt;
}

/* Box-filter downsample with alpha-weighted colors */
static void texDownsample(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst, uint32_t dw, uint32_t dh)
{
	uint32_t x, y;
	for (y = 0; y < dh; ++y)
	{
		for (x = 0; x < dw; ++x)
		{
			uint32_t x0 = x * sw / dw, y0 = y * sh / dh;
			uint32_t x1 = SDL_min(x0 + (sw > dw ? 1 : 0), sw - 1), y1 = SDL_min(y0 + (sh > dh ? 1 : 0), sh - 1);
			const uint8_t *p[4];
			uint32_t r = 0, g = 0, b = 0, a = 0, rw = 0, gw = 0, bw = 0, i;
			uint8_t *o = dst + (y * dw + x) * 4;
			p[0] = src + (y0 * sw + x0) * 4;
			p[1] = src + (y0 * sw + x1) * 4;
			p[2] = src + (y1 * sw + x0) * 4;
			p[3] = src + (y1 * sw + x1) * 4;
			for (i = 0; i < 4; ++i)
			{
				r += p[i][0]; g += p[i][1]; b += p[i][2]; a += p[i][3];
				rw += p[i][0] * p[i][3]; gw += p[i][1] * p[i][3]; bw += p[i][2] * p[i][3];
			}
			if (a > 0)
			{
				o[0] = (rw + a / 2) / a;
				o[1] = (gw + a / 2) / a;
				o[2] = (bw + a / 2) / a;
			}
			else
			{
				o[0] = (r + 2) / 4;
				o[1] = (g + 2) / 4;
				o[2] = (b + 2) / 4;
			}
			o[3] = (a + 2) / 4;
		}
	}
}

#define CutoutThreshold 0.35f

static void texQueryCaps()
{
	const char *ext;
	if (g_texCapsQueried)
		return;
	g_texCapsQueried = true;
	g_maxAnisotropy = 0.0f;
	ext = (const char *)glGetString(GL_EXTENSIONS);
	if (ext && (strstr(ext, "GL_EXT_texture_filter_anisotropic") || strstr(ext, "GL_ARB_texture_filter_anisotropic")))
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_maxAnisotropy);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_maxTextureSize);
	if (g_maxTextureSize <= 0)
		g_maxTextureSize = 2048;
}

/* Uploads RGBA8 texture (with mip chain) into the currently bound texture. "rgba" may be modified. */
static void texUploadRGBA(uint8_t *rgba, uint32_t w, uint32_t h, uint8_t alphaMode)
{
	BOOL useMipmaps = gfx.mipmaps;
	uint32_t level = 0;

	texQueryCaps();

#ifdef GLES2
	if ((w & (w - 1)) || (h & (h - 1)))
		useMipmaps = false;
#endif

	if (alphaMode != AlphaOpaque)
		texDilate(rgba, w, h);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	if (useMipmaps && (w > 1 || h > 1))
	{
		const uint32_t baseCoverage = (alphaMode == AlphaCutout) ? texCoverage(rgba, w * h, 1.0f, CutoutThreshold) : 0;
		const float baseFraction = (float)baseCoverage / (float)(w * h);
		const uint32_t bufSize = SDL_max(w / 2, 1) * SDL_max(h / 2, 1) * 4;
		uint8_t *bufs[2];
		uint8_t *src = rgba;
		uint32_t sw = w, sh = h, which = 0;
		bufs[0] = (uint8_t *)malloc(bufSize);
		bufs[1] = (uint8_t *)malloc(bufSize);
		if (!bufs[0] || !bufs[1])
		{
			useMipmaps = false;
		}
		else
		{
			while (sw > 1 || sh > 1)
			{
				uint32_t dw = SDL_max(sw / 2, 1), dh = SDL_max(sh / 2, 1);
				uint8_t *dst = bufs[which];
				texDownsample(src, sw, sh, dst, dw, dh);
				++level;

				if (alphaMode == AlphaCutout && baseCoverage > 0)
				{
					/* Scale alpha so the fraction of texels passing the alpha test stays the same (avoids vanishing foliage) */
					const uint32_t n = dw * dh;
					const uint32_t target = (uint32_t)(baseFraction * n + 0.5f);
					float lo = 0.0f, hi = 8.0f;
					uint32_t i, iter;
					for (iter = 0; iter < 12; ++iter)
					{
						float mid = (lo + hi) * 0.5f;
						if (texCoverage(dst, n, mid, CutoutThreshold) < target)
							lo = mid;
						else
							hi = mid;
					}
					for (i = 0; i < n; ++i)
					{
						float a = dst[i * 4 + 3] * hi;
						dst[i * 4 + 3] = a > 255.0f ? 255 : (uint8_t)(a + 0.5f);
					}
				}

				glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, dst);

				src = dst;
				which ^= 1;
				sw = dw;
				sh = dh;
			}
		}
		free(bufs[0]);
		free(bufs[1]);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

#ifndef GLES2
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, useMipmaps ? (GLint)level : 0);
#endif
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, useMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linearFiltering ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (g_maxAnisotropy > 1.0f && useMipmaps)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, SDL_min((float)SDL_max(gfx.anisotropy, 1), g_maxAnisotropy));
}

/* Texture dumps and replacements */

static BOOL texFileExists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}
static char *texPath(const char *dir, const char *subdir, const char *name, const char *ext)
{
	size_t len = (dir ? strlen(dir) : 0) + strlen(subdir) + strlen(name) + strlen(ext) + 4;
	char *path = (char *)malloc(len);
	if (path)
		sprintf(path, "%s%s/%s%s", dir ? dir : "", subdir, name, ext);
	return path;
}

static void texDump(uint64_t hash, const uint8_t *rgba, uint32_t w, uint32_t h)
{
	const char *settings = getSettingsDir();
	char name[17], *path;
	if (!settings)
		return;
	texHashName(hash, name);
	path = texPath(settings, "texdump", name, ".png");
	if (path && !texFileExists(path))
		stbi_write_png(path, w, h, 4, rgba, w * 4);
	free(path);
}

static uint8_t *texLoadReplacement(uint64_t hash, uint32_t *w, uint32_t *h)
{
	static const char *exts[] = {".png", ".jpg", ".tga"};
	const char *dirs[2];
	char name[17];
	uint32_t d, e;

	dirs[0] = getSettingsDir();
	dirs[1] = "";
	texHashName(hash, name);

	for (d = 0; d < 2; ++d)
	{
		if (d == 0 && !dirs[0])
			continue;
		for (e = 0; e < sizeof exts / sizeof *exts; ++e)
		{
			char *path = texPath(dirs[d], d == 0 ? "textures" : "./textures", name, exts[e]);
			uint8_t *pixels = NULL;
			int iw = 0, ih = 0, comp = 0;
			if (!path)
				continue;
			if (texFileExists(path))
			{
				pixels = stbi_load(path, &iw, &ih, &comp, 4);
				if (!pixels)
					fprintf(stderr, "Cannot load texture \"%s\": %s\n", path, stbi_failure_reason());
			}
			free(path);
			if (pixels)
			{
				/* Downscale if too large for the GPU */
				while (iw > g_maxTextureSize || ih > g_maxTextureSize)
				{
					int nw = SDL_max(iw / 2, 1), nh = SDL_max(ih / 2, 1);
					uint8_t *smaller = (uint8_t *)malloc(nw * nh * 4);
					if (!smaller)
						break;
					texDownsample(pixels, iw, ih, smaller, nw, nh);
					stbi_image_free(pixels);
					pixels = smaller;
					iw = nw;
					ih = nh;
				}
				*w = iw;
				*h = ih;
				return pixels;
			}
		}
	}
	return NULL;
}

/* Cache */

static TexCacheEntry *texCacheFind(uint64_t hash)
{
	uint32_t i;
	for (i = 0; i < g_texCacheCount; ++i)
		if (g_texCache[i].hash == hash)
			return &g_texCache[i];
	return NULL;
}

static void texCacheEvict()
{
	uint32_t unused = 0, i;
	for (i = 0; i < g_texCacheCount; ++i)
		if (g_texCache[i].refs <= 0)
			++unused;
	while (unused > TexCacheMaxUnused)
	{
		uint32_t oldest = UINT32_MAX, oldestIdx = 0;
		for (i = 0; i < g_texCacheCount; ++i)
		{
			if (g_texCache[i].refs <= 0 && g_texCache[i].lastUse < oldest)
			{
				oldest = g_texCache[i].lastUse;
				oldestIdx = i;
			}
		}
		glDeleteTextures(1, &g_texCache[oldestIdx].id);
		g_texCache[oldestIdx] = g_texCache[--g_texCacheCount];
		--unused;
	}
}

static void texRelease(uint64_t hash)
{
	TexCacheEntry *e;
	if (!hash)
		return;
	e = texCacheFind(hash);
	if (e && e->refs > 0)
		--e->refs;
}

/* Returns GL texture for given texture data, creating it when necessary. Leaves the texture bound. */
static GLuint texAcquire(const void *data, GrTextureFormat_t fmt, uint32_t size, const uint32_t *palette, uint64_t *hashOut, uint8_t *alphaModeOut)
{
	TexCacheEntry *e;
	uint64_t hash;
	uint32_t n = size * size;

	if (size == 0 || size > 256)
		return 0;

	texConvertToRGBA(data, fmt, size, palette, g_rgbaTmp);
	hash = texHash(g_rgbaTmp, size, size);
	if (!hash)
		hash = 1;

	e = texCacheFind(hash);
	if (!e)
	{
		uint8_t alphaMode = texAlphaMode(g_rgbaTmp, n);
		uint8_t *replacement = NULL;
		uint32_t rw = 0, rh = 0;

		texQueryCaps();

		if (gfx.textureDump)
			texDump(hash, g_rgbaTmp, size, size);
		if (gfx.textureReplace)
			replacement = texLoadReplacement(hash, &rw, &rh);

		if (g_texCacheCount == g_texCacheCapacity)
		{
			uint32_t newCapacity = g_texCacheCapacity ? g_texCacheCapacity * 2 : 1024;
			TexCacheEntry *newCache = (TexCacheEntry *)realloc(g_texCache, newCapacity * sizeof(TexCacheEntry));
			if (!newCache)
			{
				if (replacement)
					stbi_image_free(replacement);
				return 0;
			}
			g_texCache = newCache;
			g_texCacheCapacity = newCapacity;
		}
		e = &g_texCache[g_texCacheCount++];
		memset(e, 0, sizeof *e);
		e->hash = hash;
		e->alphaMode = alphaMode;
		glGenTextures(1, &e->id);
		glBindTexture(GL_TEXTURE_2D, e->id);
		if (replacement)
		{
			texUploadRGBA(replacement, rw, rh, alphaMode);
			stbi_image_free(replacement);
		}
		else
		{
			texUploadRGBA(g_rgbaTmp, size, size, alphaMode);
		}
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, e->id);
	}

	++e->refs;
	e->lastUse = g_texFrame;
	*hashOut = hash;
	*alphaModeOut = e->alphaMode;

	{
		/* Eviction may move entries, so read everything we need before it */
		GLuint id = e->id;
		texCacheEvict();
		return id;
	}
}

static void texCacheClear()
{
	uint32_t i;
	for (i = 0; i < g_texCacheCount; ++i)
		glDeleteTextures(1, &g_texCache[i].id);
	free(g_texCache);
	g_texCache = NULL;
	g_texCacheCount = g_texCacheCapacity = 0;
	g_texCapsQueried = false;
}
