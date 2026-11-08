// SPDX-License-Identifier: MIT

/*
 * High resolution skies.
 *
 * The game draws its sky as a low resolution ring around the camera (horizon backdrop, "hrzsku.c"). A hook after
 * that (skyHook, wrapping sub_42DAF0) gets the view's camera; when "<settings>/skies/trNN.txt" exists for the
 * current track, a panorama (equirectangular, upper part only) is drawn over the backdrop before any 3D geometry,
 * so everything else is drawn over it as usual. The hook only captures the camera: the game queues the backdrop in
 * its sorted polygon list, so the panorama is drawn when the first 3D triangle of the view arrives (worldMask()). It fades in above the horizon, which keeps the game's distant
 * terrain (mountains, skyline) from the backdrop.
 *
 * The panorama is rotated so its sun matches the game's: the brightest vertex of the game's sky gradient (its sun
 * glow) gives the game's sun azimuth. "yaw" in the sky file is used when the game's sky has no clear glow.
 *
 * Sky file (key=value): image=<file in the same directory>, elevation_min=<degrees at the bottom row>,
 * yaw=<degrees>, sun_azimuth/sun_elevation=<sun in the image, degrees>, horizon_blend=<from> <to> (degrees),
 * auto_yaw=0/1. Made by tools/skies/make_sky.py.
 *
 * This file is included from OpenGL2.c.
 */

#include <SDL2/SDL_timer.h>

extern float dword_4D5294, flt_4D5298, flt_4D529C, flt_4D52A0; /* Game projection: scale X, scale Y, center X, center Y */

typedef struct
{
	int32_t track;          /* Track the sky belongs to, -2 = none loaded yet */
	GLuint tex;
	GLuint ibl[3];          /* Image based lighting: sharp (512 wide), blurred (128), diffuse (32), same encoding */
	float elevationMin;     /* Degrees */
	float yaw, sunAzimuth, sunElevation, blend[2];
	float gameFog;          /* Share of the game's fog colour in the sky fog (0 - panorama only) */
	float headlights;       /* Headlight strength of the player's car (0 - off, 1 - night) */
	float nightAmbient;     /* Night: the scenery is darkened to this (1 - off), bright lights stay */
	BOOL autoYaw;
	/* Sun alignment */
	uint32_t startTicks;    /* Views without a glow in the game's sky */
	float measuredSum, measuredCount;
	BOOL aligned;
} SkyState;

static SkyState g_sky = {-2};

/* Captured by skyHook() for the view being drawn */
static struct
{
	BOOL pending, captured;
	float camToWorld[16], proj[4], camPos[3];
} g_skyView;
static BOOL g_skyReplacing; /* The panorama was drawn in the current view: the game's sky gradient is skipped */
static void skySetFog(BOOL on);

/* Camera of the main (full width) view of the frame, for the post-processing */
static struct
{
	BOOL valid;
	float camToWorld[16];
} g_skyMain;
static GLuint g_skyProg;
static BOOL g_skyProgFailed;

static const char g_skyVertexSrc[] =
	"#version 120\n"
	"attribute vec2 aPos;\n"
	"varying vec2 vNdc;\n"
	"void main()\n"
	"{\n"
	"	vNdc = aPos;\n"
	"	gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"}\n"
;

static const char g_skyFragmentSrc[] =
	"#version 120\n"
	"varying vec2 vNdc;\n"
	"uniform sampler2D uSkyTex;\n"
	"uniform vec4 uClip;\n"       /* Clip window in game units: x, y, width, height */
	"uniform vec4 uProj;\n"       /* Game projection: scale X, scale Y, center X, center Y */
	"uniform mat4 uCam;\n"        /* Camera space -> world directions */
	"uniform vec4 uParams;\n"     /* yaw (radians), elevation_min (degrees), blend from, blend to (degrees) */
	"void main()\n"
	"{\n"
	"	vec2 g = vec2(uClip.x + (vNdc.x * 0.5 + 0.5) * uClip.z, uClip.y + (0.5 - vNdc.y * 0.5) * uClip.w);\n"
	"	vec3 d = normalize((uCam * vec4((g.x - uProj.z) / uProj.x, (g.y - uProj.w) / uProj.y, 1.0, 0.0)).xyz);\n"
	"	float az = atan(d.x, d.z) + uParams.x;\n"
	"	float el = degrees(asin(clamp(d.y, -1.0, 1.0)));\n"
	"	float u = fract(az / 6.2831853 + 0.5);\n"
	"	float v = (90.0 - el) / (90.0 - uParams.y);\n"
	"	vec3 c = texture2D(uSkyTex, vec2(u, min(v, 0.999))).rgb;\n"
	"	float a = smoothstep(uParams.z, uParams.w, el);\n"
	"	gl_FragData[0] = vec4(c, a);\n"
	"	gl_FragData[1] = vec4(1.0, 0.0, 0.0, a);\n"
	"}\n"
;

static BOOL skyCreateProgram()
{
	const GLchar *vsrc = g_skyVertexSrc, *fsrc = g_skyFragmentSrc;
	GLuint vs, fs;
	if (g_skyProg || g_skyProgFailed)
		return g_skyProg != 0;
	g_skyProgFailed = true;
	vs = glCreateShader(GL_VERTEX_SHADER);
	fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(vs, 1, &vsrc, NULL);
	glShaderSource(fs, 1, &fsrc, NULL);
	glCompileShader(vs);
	glCompileShader(fs);
	if (!checkShaderCompilation(vs) || !checkShaderCompilation(fs))
	{
		fprintf(stderr, "Skies: cannot compile shaders\n");
		glDeleteShader(vs);
		glDeleteShader(fs);
		return false;
	}
	g_skyProg = glCreateProgram();
	glAttachShader(g_skyProg, vs);
	glAttachShader(g_skyProg, fs);
	glBindAttribLocation(g_skyProg, 0, "aPos");
	glLinkProgram(g_skyProg);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!checkShaderProgram(g_skyProg))
	{
		glDeleteProgram(g_skyProg);
		g_skyProg = 0;
		return false;
	}
	g_skyProgFailed = false;
	return true;
}

static void skyUnload()
{
	if (g_sky.tex)
		glDeleteTextures(1, &g_sky.tex);
	if (g_sky.ibl[0])
		glDeleteTextures(3, g_sky.ibl);
	g_sky.tex = 0;
	memset(g_sky.ibl, 0, sizeof g_sky.ibl);
}

/*
 * Image based lighting. The sky image is encoded like the post-processing's HDR scene (c / (1 + 0.75 c), gamma 2.2):
 * smaller, blurred copies are made in linear HDR and stored with the same encoding.
 */
static inline float skyDecode(float v)
{
	const float lin = SDL_pow(v, 2.2f);
	return lin / SDL_max(1.0f - 0.75f * lin, 1.0f / 64.0f);
}
static inline uint8_t skyEncode(float hdr)
{
	const float c = hdr / (1.0f + 0.75f * hdr);
	return (uint8_t)(SDL_pow(SDL_min(SDL_max(c, 0.0f), 1.0f), 1.0f / 2.2f) * 255.0f + 0.5f);
}

/* Area average of a float RGB image (dst smaller than src) */
static void skyDownsample(const float *src, int32_t sw, int32_t sh, float *dst, int32_t dw, int32_t dh)
{
	int32_t x, y, sx, sy, c;
	for (y = 0; y < dh; ++y)
	{
		const int32_t y0 = y * sh / dh, y1 = SDL_max((y + 1) * sh / dh, y0 + 1);
		for (x = 0; x < dw; ++x)
		{
			const int32_t x0 = x * sw / dw, x1 = SDL_max((x + 1) * sw / dw, x0 + 1);
			float sum[3] = {0.0f, 0.0f, 0.0f};
			for (sy = y0; sy < y1; ++sy)
				for (sx = x0; sx < x1; ++sx)
					for (c = 0; c < 3; ++c)
						sum[c] += src[(sy * sw + sx) * 3 + c];
			for (c = 0; c < 3; ++c)
				dst[(y * dw + x) * 3 + c] = sum[c] / ((y1 - y0) * (x1 - x0));
		}
	}
}

/* Separable Gaussian blur, wrapping horizontally (azimuth), clamped vertically */
static void skyBlur(float *img, int32_t w, int32_t h, float sigma)
{
	const int32_t r = (int32_t)(sigma * 3.0f + 0.5f);
	float *tmp = (float *)malloc(sizeof(float) * 3 * w * h), *k = (float *)malloc(sizeof(float) * (2 * r + 1));
	float ksum = 0.0f;
	int32_t x, y, i, c;
	if (!tmp || !k)
	{
		free(tmp);
		free(k);
		return;
	}
	for (i = -r; i <= r; ++i)
		ksum += k[i + r] = SDL_exp(-(float)(i * i) / (2.0f * sigma * sigma));
	for (y = 0; y < h; ++y)
		for (x = 0; x < w; ++x)
			for (c = 0; c < 3; ++c)
			{
				float sum = 0.0f;
				for (i = -r; i <= r; ++i)
					sum += k[i + r] * img[(y * w + ((x + i) % w + w) % w) * 3 + c];
				tmp[(y * w + x) * 3 + c] = sum / ksum;
			}
	for (y = 0; y < h; ++y)
		for (x = 0; x < w; ++x)
			for (c = 0; c < 3; ++c)
			{
				float sum = 0.0f;
				for (i = -r; i <= r; ++i)
					sum += k[i + r] * tmp[(SDL_min(SDL_max(y + i, 0), h - 1) * w + x) * 3 + c];
				img[(y * w + x) * 3 + c] = sum / ksum;
			}
	free(tmp);
	free(k);
}

static GLuint skyUploadHdr(const float *img, int32_t w, int32_t h)
{
	uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
	GLuint tex = 0;
	int32_t i;
	if (!rgba)
		return 0;
	for (i = 0; i < w * h; ++i)
	{
		rgba[i * 4 + 0] = skyEncode(img[i * 3 + 0]);
		rgba[i * 4 + 1] = skyEncode(img[i * 3 + 1]);
		rgba[i * 4 + 2] = skyEncode(img[i * 3 + 2]);
		rgba[i * 4 + 3] = 255;
	}
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	free(rgba);
	return tex;
}

/* Builds the image based lighting textures from the sky image (RGBA8, encoded) */
static void skyBuildIbl(const uint8_t *pixels, int32_t w, int32_t h)
{
	const int32_t w0 = 512, h0 = SDL_max(h * w0 / w, 4), w1 = 128, h1 = SDL_max(h0 / 4, 2), w2 = 32, h2 = SDL_max(h1 / 4, 2);
	float decode[256], *full, *l0, *l1, *l2;
	int32_t i;
	for (i = 0; i < 256; ++i)
		decode[i] = skyDecode(i / 255.0f);
	full = (float *)malloc(sizeof(float) * 3 * w * h);
	l0 = (float *)malloc(sizeof(float) * 3 * w0 * h0);
	l1 = (float *)malloc(sizeof(float) * 3 * w1 * h1);
	l2 = (float *)malloc(sizeof(float) * 3 * w2 * h2);
	if (full && l0 && l1 && l2)
	{
		for (i = 0; i < w * h; ++i)
		{
			full[i * 3 + 0] = decode[pixels[i * 4 + 0]];
			full[i * 3 + 1] = decode[pixels[i * 4 + 1]];
			full[i * 3 + 2] = decode[pixels[i * 4 + 2]];
		}
		skyDownsample(full, w, h, l0, w0, h0);
		skyDownsample(l0, w0, h0, l1, w1, h1);
		skyBlur(l1, w1, h1, 2.0f);   /* ~6 degrees */
		skyDownsample(l1, w1, h1, l2, w2, h2);
		skyBlur(l2, w2, h2, 2.5f);   /* ~30 degrees: diffuse light */
		g_sky.ibl[0] = skyUploadHdr(l0, w0, h0);
		g_sky.ibl[1] = skyUploadHdr(l1, w1, h1);
		g_sky.ibl[2] = skyUploadHdr(l2, w2, h2);
	}
	free(full);
	free(l0);
	free(l1);
	free(l2);
}

/*
 * Binds the image based lighting textures to units "unit".."unit + 2" for a shader, with the camera of the current
 * view. Returns false when the track has no sky.
 */
static BOOL skyIblBind(int32_t unit, float camToWorld[16], float *yawRadians, float *elevationMin, float sunDirWorld[3], float *sunStrength)
{
	int32_t i;
	if (!gfx.skies || !g_sky.ibl[0] || g_sky.track != gfxCurrentTrack)
		return false;
	for (i = 0; i < 3; ++i)
	{
		glActiveTexture(GL_TEXTURE0 + unit + i);
		glBindTexture(GL_TEXTURE_2D, g_sky.ibl[i]);
	}
	glActiveTexture(GL_TEXTURE0);
	memcpy(camToWorld, g_skyView.camToWorld, sizeof(float) * 16);
	*yawRadians = g_sky.yaw * (float)M_PI / 180.0f;
	*elevationMin = g_sky.elevationMin;
	{
		/* Panorama azimuth = world azimuth + yaw */
		const float az = (g_sky.sunAzimuth - g_sky.yaw) * (float)M_PI / 180.0f, el = g_sky.sunElevation * (float)M_PI / 180.0f;
		sunDirWorld[0] = SDL_sinf(az) * SDL_cosf(el);
		sunDirWorld[1] = SDL_sinf(el);
		sunDirWorld[2] = SDL_cosf(az) * SDL_cosf(el);
	}
	/* Skies whose sun is matched to the game's have a real sun; overcast ones only diffuse light */
	*sunStrength = g_sky.autoYaw ? 1.0f : 0.0f;
	return true;
}

/* Loads "<settings>/skies/trNN.txt" and its image for the track */
static void skyLoad(int32_t track)
{
	char path[1024], line[512], image[256] = "";
	FILE *f;
	skyUnload();
	memset(&g_sky, 0, sizeof g_sky);
	g_sky.track = track;
	g_sky.elevationMin = -8.0f;
	g_sky.blend[0] = 1.0f;
	g_sky.blend[1] = 6.0f;
	g_sky.autoYaw = true;
	g_sky.gameFog = 0.5f;
	g_sky.nightAmbient = 1.0f;
	if (track < 0 || !getSettingsDir())
		return;
	snprintf(path, sizeof path, "%s/skies/tr%02d.txt", getSettingsDir(), track);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f))
	{
		char *v = strchr(line, '=');
		if (!v)
			continue;
		*v++ = '\0';
		v[strcspn(v, "\r\n")] = '\0';
		if (!strcmp(line, "image"))
			snprintf(image, sizeof image, "%s", v);
		else if (!strcmp(line, "elevation_min"))
			g_sky.elevationMin = atof(v);
		else if (!strcmp(line, "yaw"))
			g_sky.yaw = atof(v);
		else if (!strcmp(line, "sun_azimuth"))
			g_sky.sunAzimuth = atof(v);
		else if (!strcmp(line, "sun_elevation"))
			g_sky.sunElevation = atof(v);
		else if (!strcmp(line, "horizon_blend"))
			sscanf(v, "%f %f", &g_sky.blend[0], &g_sky.blend[1]);
		else if (!strcmp(line, "auto_yaw"))
			g_sky.autoYaw = atoi(v) != 0;
		else if (!strcmp(line, "night_ambient"))
			g_sky.nightAmbient = SDL_max(0.05f, SDL_min(1.0f, (float)atof(v)));
		else if (!strcmp(line, "headlights"))
			g_sky.headlights = SDL_max(0.0f, (float)atof(v));
		else if (!strcmp(line, "game_fog"))
			g_sky.gameFog = SDL_max(0.0f, SDL_min(1.0f, (float)atof(v)));
	}
	fclose(f);
	if (image[0])
	{
		int32_t w, h, comp;
		uint8_t *pixels;
		snprintf(path, sizeof path, "%s/skies/%s", getSettingsDir(), image);
		pixels = stbi_load(path, &w, &h, &comp, 4);
		if (pixels)
		{
			while (w > g_maxTextureSize || h > g_maxTextureSize)
			{
				int32_t nw = SDL_max(w / 2, 1), nh = SDL_max(h / 2, 1);
				uint8_t *smaller = (uint8_t *)malloc((size_t)nw * nh * 4);
				if (!smaller)
					break;
				texDownsample(pixels, w, h, smaller, nw, nh);
				stbi_image_free(pixels);
				pixels = smaller;
				w = nw;
				h = nh;
			}
			skyBuildIbl(pixels, w, h);
			glGenTextures(1, &g_sky.tex);
			glBindTexture(GL_TEXTURE_2D, g_sky.tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
			stbi_image_free(pixels);
			glBindTexture(GL_TEXTURE_2D, g_boundTexture);
			fprintf(stderr, "Sky for track %d: %s (%dx%d)\n", track, image, w, h);
		}
		else
		{
			fprintf(stderr, "Cannot load sky \"%s\"\n", path);
		}
	}
}

/* Camera space -> world direction matrix (column major mat4) from the game's world -> camera matrix (row vectors) */
static void skyCameraToWorld(const int32_t *c, float out[16])
{
	int32_t i, j;
	memset(out, 0, sizeof(float) * 16);
	for (i = 0; i < 3; ++i)
		for (j = 0; j < 3; ++j)
			out[j * 4 + i] = c[i * 3 + j] / 65536.0f; /* world_i = sum_j cam_j * C[i][j] */
	out[15] = 1.0f;
}

/*
 * Sun alignment. The game's sky gradient (skipped while a panorama is drawn) has per-vertex colours with its sun
 * glow brightest: the brightest gradient vertex of the main view is turned into a world azimuth, and the panorama is
 * rotated so its own sun is there. The yaw is a running average, locked after 30 views; the panorama is drawn from
 * the start (only the first frame uses the configured yaw).
 */
static struct
{
	float maxLum, minLum, x, y;
	int32_t count;
} g_skyGradient;

static void skyObserveGradient(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
	const GrVertex *v[3] = {a, b, c};
	int32_t i;
	for (i = 0; i < 3; ++i)
	{
		const float lum = v[i]->r + v[i]->g + v[i]->b;
		if (!g_skyGradient.count || lum > g_skyGradient.maxLum)
		{
			g_skyGradient.maxLum = lum;
			g_skyGradient.x = v[i]->x - VertexSnap;
			g_skyGradient.y = v[i]->y - VertexSnap;
		}
		if (!g_skyGradient.count || lum < g_skyGradient.minLum)
			g_skyGradient.minLum = lum;
		++g_skyGradient.count;
	}
}

/* Called before the first 3D triangle of a view: uses the gradient seen in it */
static void skyProcessGradient()
{
	const int32_t count = g_skyGradient.count;
	g_skyGradient.count = 0;
	if (g_sky.aligned || !g_sky.autoYaw || !g_sky.tex || !g_fullWidthView || count < 6)
		return;
	/* A clear glow: the brightest vertex is distinctly brighter than the darkest */
	if (g_skyGradient.maxLum > g_skyGradient.minLum * 1.15f + 6.0f)
	{
		const float *m = g_skyView.camToWorld;
		const float d[3] = {(g_skyGradient.x - g_skyView.proj[2]) / g_skyView.proj[0], (g_skyGradient.y - g_skyView.proj[3]) / g_skyView.proj[1], 1.0f};
		float w[3], az, offset, mean;
		int32_t i;
		for (i = 0; i < 3; ++i)
			w[i] = m[0 * 4 + i] * d[0] + m[1 * 4 + i] * d[1] + m[2 * 4 + i] * d[2];
		az = SDL_atan2(w[0], w[2]) * (180.0f / (float)M_PI);
		/* Panorama azimuth = world azimuth + yaw */
		offset = g_sky.sunAzimuth - az;
		mean = g_sky.measuredCount > 0.0f ? g_sky.measuredSum / g_sky.measuredCount : offset;
		while (offset - mean > 180.0f)
			offset -= 360.0f;
		while (offset - mean < -180.0f)
			offset += 360.0f;
		g_sky.measuredSum += offset;
		g_sky.measuredCount += 1.0f;
		g_sky.yaw = g_sky.measuredSum / g_sky.measuredCount;
		if (g_sky.measuredCount >= 30.0f)
		{
			g_sky.aligned = true;
			fprintf(stderr, "Sky yaw: %.1f (measured from the game's sky)\n", g_sky.yaw);
		}
	}
	else if (++g_sky.startTicks > 300)
	{
		/* No glow in the game's sky (overcast): keep the configured or measured yaw */
		g_sky.aligned = true;
		fprintf(stderr, "Sky yaw: %.1f (%s)\n", g_sky.yaw, g_sky.measuredCount > 0.0f ? "measured" : "configured");
	}
}

/* Game hook: the horizon (sky backdrop) of a view was queued, the camera is in "camera" */
REALIGN STDCALL void skyHook(const uint8_t *camera)
{
	g_skyView.pending = false;
	if (g_skyReplacing)
		skySetFog(false); /* A new view: its own sky sets the fog again */
	g_skyReplacing = false;
	if (!camera)
		return;
	/* The camera is also used without a sky (car model environment reflections) */
	skyCameraToWorld((const int32_t *)(camera + 0x44), g_skyView.camToWorld);
	g_skyView.proj[0] = dword_4D5294;
	g_skyView.proj[1] = flt_4D5298;
	g_skyView.proj[2] = flt_4D529C;
	g_skyView.proj[3] = flt_4D52A0;
	{
		const int32_t *pos = (const int32_t *)(camera + 8);
		g_skyView.camPos[0] = pos[0] / 65536.0f;
		g_skyView.camPos[1] = pos[1] / 65536.0f;
		g_skyView.camPos[2] = pos[2] / 65536.0f;
	}
	g_skyView.captured = true;
	if (!gfx.skies)
		return;
	if (g_sky.track != gfxCurrentTrack)
		skyLoad(gfxCurrentTrack);
	if (!g_sky.tex)
		return;
	g_skyView.pending = true;
}

/*
 * Sky fog: the game's fog colour is replaced per pixel by the (blurred) panorama near the horizon in the pixel's
 * direction, so distant geometry fades into the sky behind it. Set for the view whose sky was drawn.
 */
static void skySetFog(BOOL on)
{
	GLint viewport[4];
	glUseProgram(g_shaderProgram);
	glUniform1f(glGetUniformLocation(g_shaderProgram, "uSkyFog"), on ? 1.0f : 0.0f);
	if (!on)
		return;
	glGetIntegerv(GL_VIEWPORT, viewport);
	glActiveTexture(GL_TEXTURE8);
	glBindTexture(GL_TEXTURE_2D, g_sky.ibl[1]);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(g_shaderProgram, "uSkyFogTex"), 8);
	glUniformMatrix4fv(glGetUniformLocation(g_shaderProgram, "uSkyFogCam"), 1, GL_FALSE, g_skyView.camToWorld);
	glUniform4f(glGetUniformLocation(g_shaderProgram, "uSkyFogProj"), g_skyView.proj[0], g_skyView.proj[1], g_skyView.proj[2], g_skyView.proj[3]);
	glUniform4f(glGetUniformLocation(g_shaderProgram, "uSkyFogViewport"), viewport[0], viewport[1], viewport[2], viewport[3]);
	glUniform4f(glGetUniformLocation(g_shaderProgram, "uSkyFogClip"), g_clipWindow[0], g_clipWindow[1], g_clipWindow[2] - g_clipWindow[0], g_clipWindow[3] - g_clipWindow[1]);
	glUniform2f(glGetUniformLocation(g_shaderProgram, "uSkyFogParams"), g_sky.yaw * (float)M_PI / 180.0f, g_sky.elevationMin);
	glUniform1f(glGetUniformLocation(g_shaderProgram, "uSkyFogMix"), g_sky.gameFog);
}

/* Draws the panorama of the current view: called before its first 3D triangle */
static void skyDrawPending()
{
	static const float quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
	if (g_skyView.captured)
	{
		g_skyView.captured = false;
		if (g_fullWidthView)
		{
			memcpy(g_skyMain.camToWorld, g_skyView.camToWorld, sizeof g_skyMain.camToWorld);
			g_skyMain.valid = true;
		}
	}
	if (!g_skyView.pending)
		return;
	g_skyView.pending = false;
	if (!skyCreateProgram())
		return;

	drawTriangles();
	glUseProgram(g_skyProg);
	glDisableVertexAttribArray(g_aPositionLoc);
	glDisableVertexAttribArray(g_aTexCoordLoc);
	glDisableVertexAttribArray(g_aColorLoc);
	glDisableVertexAttribArray(g_aFogLoc);
	if (g_aMaskLoc >= 0)
		glDisableVertexAttribArray(g_aMaskLoc);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glUniform1i(glGetUniformLocation(g_skyProg, "uSkyTex"), 0);
	glUniform4f(glGetUniformLocation(g_skyProg, "uClip"), g_clipWindow[0], g_clipWindow[1], g_clipWindow[2] - g_clipWindow[0], g_clipWindow[3] - g_clipWindow[1]);
	glUniform4f(glGetUniformLocation(g_skyProg, "uProj"), g_skyView.proj[0], g_skyView.proj[1], g_skyView.proj[2], g_skyView.proj[3]);
	glUniformMatrix4fv(glGetUniformLocation(g_skyProg, "uCam"), 1, GL_FALSE, g_skyView.camToWorld);
	/* Opaque: the game's sky gradient is skipped, its terrain is drawn over the panorama afterwards */
	glUniform4f(glGetUniformLocation(g_skyProg, "uParams"), g_sky.yaw * (float)M_PI / 180.0f, g_sky.elevationMin, -1000.0f, -999.0f);
	glBindTexture(GL_TEXTURE_2D, g_sky.tex);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	g_skyReplacing = true;

	useGameProgram(true);
	skySetFog(true);
	glUniform1f(g_uTextureEnabledLoc, g_textureEnabled);
	glDepthMask(g_depthMask);
}

static BOOL skyMainPanorama(int32_t unit, float camToWorld[16], float *yawRadians, float *elevationMin)
{
	if (!gfx.skies || !g_sky.ibl[0] || g_sky.track != gfxCurrentTrack || !g_skyMain.valid || !g_sky.aligned)
		return false;
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, g_sky.ibl[0]);
	glActiveTexture(GL_TEXTURE0 + unit + 1);
	glBindTexture(GL_TEXTURE_2D, g_sky.ibl[1]);
	glActiveTexture(GL_TEXTURE0);
	memcpy(camToWorld, g_skyMain.camToWorld, sizeof(float) * 16);
	*yawRadians = g_sky.yaw * (float)M_PI / 180.0f;
	*elevationMin = g_sky.elevationMin;
	return true;
}

static BOOL skyMainCamera(float camToWorld[16])
{
	if (!g_skyMain.valid)
		return false;
	memcpy(camToWorld, g_skyMain.camToWorld, sizeof(float) * 16);
	return true;
}

/* Binds the dynamic environment (sharp, blurred, diffuse) to units "unit".."unit + 2"; false if not available */
static BOOL skyEnvBind(int32_t unit)
{
	if (!g_post.ok || !g_post.active || !g_post.env2.tex || !g_skyMain.valid)
		return false;
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, g_post.env[g_post.envCur].tex);
	glActiveTexture(GL_TEXTURE0 + unit + 1);
	glBindTexture(GL_TEXTURE_2D, g_post.env1.tex);
	glActiveTexture(GL_TEXTURE0 + unit + 2);
	glBindTexture(GL_TEXTURE_2D, g_post.env2.tex);
	glActiveTexture(GL_TEXTURE0);
	return true;
}
