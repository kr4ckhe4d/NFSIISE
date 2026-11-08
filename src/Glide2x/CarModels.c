// SPDX-License-Identifier: MIT

/*
 * Replacement car models.
 *
 * The game transforms every car part on the CPU and draws pre-projected polygons. Two hooks in the game code
 * report to the renderer:
 *   - carModelLoaded(): a car's GEO model was loaded (file name and the in-memory model),
 *   - carPartHook(): a car part is about to be drawn, with its model -> camera matrix and translation.
 * When a converted model exists in "<settings>/models/<car>/model.cmdl", the original parts of that car are
 * skipped and the model is drawn instead, fitted to the size of the original body and wheels and lit with a
 * physically based shader. Its projection and depth match the game's exactly, so it sorts correctly against
 * the track and gets all post-processing effects.
 *
 * Model file (.cmdl, little endian): "CMDL", u32 version (1 or 2), u32 batch count, then per batch:
 *   f32 color[4], metallic, roughness, transmission, clearcoat, emission[3]; char texture[64];
 *   version 2: u32 wheel (0 - body, 1..4 - front left, front right, rear left, rear right), u32 spins, f32 pivot[3];
 *   u32 vertex count; vertices: f32 position[3], normal[3], uv[2] (Y up, front towards +Z, metres, right handed).
 *
 * Wheels spin with the distance the car travels and the front wheels steer by the turn rate of the car
 * (bicycle model), both measured from the car's motion.
 */

#include <SDL2/SDL_timer.h>

#define CAR_MAX_GEOS   64
#define CAR_MAX_MODELS 32
#define CAR_MAX_DRAWS  32

typedef struct
{
	GLuint vbo, tex;
	GLsizei count;
	float color[4], metallic, roughness, transmission, clearcoat, emission[3];
	int32_t wheel;   /* 0 - body, 1..4 - front left, front right, rear left, rear right */
	BOOL spins;
	float pivot[3];
} CarBatch;

typedef struct
{
	char name[32];
	CarBatch *batches;
	int32_t batchCount;
	float bmin[3], bmax[3];
	float wheelRadius, wheelBase; /* Model units */
	BOOL loaded, failed;
} CarModel;

typedef struct
{
	const uint8_t *geo;
	CarModel *model;
	float scale, offset[3]; /* Model space (mirrored X) -> car space */
	BOOL fitted;
	char name[32];
} CarGeo;

typedef struct
{
	const CarGeo *geo;
	float rot[9], origin[3]; /* Car space -> camera space (metres) */
	float spin, steer;       /* Wheels (radians) */
	BOOL opaqueDrawn;        /* Opaque parts were drawn early (before the scene's polygons) */
	float proj[4];           /* Game projection: scale X, scale Y, center X, center Y */
} CarDraw;

static CarGeo   g_carGeos[CAR_MAX_GEOS];
static int32_t  g_carGeoCount;
static CarModel g_carModels[CAR_MAX_MODELS];
static int32_t  g_carModelCount;
static CarDraw  g_carDraws[CAR_MAX_DRAWS];

/* Motion of each car (game object), for the wheels */
typedef struct
{
	const uint8_t *object;
	double pos[3], heading;
	uint32_t ticks, frame;
	float spin, steer;
} CarMotion;
static CarMotion g_carMotion[CAR_MAX_DRAWS];
static uint32_t  g_carFrame;
static int32_t  g_carDrawCount;
static GLuint   g_carProg;
static BOOL     g_carProgFailed;
int32_t carPartSkip; /* Read by the game code after carPartHook() */

extern float dword_4D5294, flt_4D5298, flt_4D529C, flt_4D52A0; /* Game projection: scale X, scale Y, center X, center Y */

static inline const uint8_t *carGeoPart(const uint8_t *geo, int32_t i)
{
	return *(const uint8_t *const *)(geo + 4 + i * 4);
}

static CarModel *carFindModel(const char *name)
{
	char path[1024];
	CarModel *m;
	int32_t i;
	for (i = 0; i < g_carModelCount; ++i)
		if (!strcmp(g_carModels[i].name, name))
			return g_carModels[i].failed ? NULL : &g_carModels[i];
	snprintf(path, sizeof path, "%s/models/%s/model.cmdl", getSettingsDir(), name);
	FILE *f = fopen(path, "rb");
	if (!f || g_carModelCount >= CAR_MAX_MODELS)
	{
		if (f)
			fclose(f);
		return NULL;
	}
	fclose(f);
	m = &g_carModels[g_carModelCount++];
	memset(m, 0, sizeof *m);
	snprintf(m->name, sizeof m->name, "%s", name);
	return m;
}

/* Reads the model file (GL resources are created on first use, from the rendering code) */
static BOOL carLoadModel(CarModel *m)
{
	char path[1024], magic[4];
	uint32_t version, count, i;
	FILE *f;
	snprintf(path, sizeof path, "%s/models/%s/model.cmdl", getSettingsDir(), m->name);
	f = fopen(path, "rb");
	m->failed = true;
	if (!f)
		return false;
	if (fread(magic, 4, 1, f) != 1 || memcmp(magic, "CMDL", 4) || fread(&version, 4, 1, f) != 1 || version < 1 || version > 2 || fread(&count, 4, 1, f) != 1 || !count || count > 256)
	{
		fclose(f);
		return false;
	}
	m->batches = calloc(count, sizeof(CarBatch));
	m->bmin[0] = m->bmin[1] = m->bmin[2] = 1e9f;
	m->bmax[0] = m->bmax[1] = m->bmax[2] = -1e9f;
	for (i = 0; i < count; ++i)
	{
		CarBatch *b = &m->batches[i];
		float params[11];
		char tex[64];
		uint32_t nv, k;
		float *data;
		if (fread(params, sizeof params, 1, f) != 1 || fread(tex, 64, 1, f) != 1)
			break;
		if (version >= 2)
		{
			uint32_t wheel[2];
			if (fread(wheel, sizeof wheel, 1, f) != 1 || fread(b->pivot, sizeof b->pivot, 1, f) != 1)
				break;
			b->wheel = wheel[0] <= 4 ? wheel[0] : 0;
			b->spins = wheel[1] != 0;
			b->pivot[0] = -b->pivot[0]; /* Right handed -> left handed */
		}
		if (fread(&nv, 4, 1, f) != 1 || nv > 16 * 1024 * 1024)
			break;
		data = malloc((size_t)nv * 32);
		if (!data || fread(data, 32, nv, f) != nv)
		{
			free(data);
			break;
		}
		memcpy(b->color, params, sizeof b->color);
		b->metallic = params[4];
		b->roughness = params[5];
		b->transmission = params[6];
		b->clearcoat = params[7];
		memcpy(b->emission, params + 8, sizeof b->emission);
		/* glTF import sets white emission on materials that don't emit */
		if (b->emission[0] == 1.0f && b->emission[1] == 1.0f && b->emission[2] == 1.0f)
			b->emission[0] = b->emission[1] = b->emission[2] = 0.0f;
		for (k = 0; k < nv; ++k)
		{
			float *v = data + k * 8;
			int32_t c;
			/* Right handed (glTF) -> the game's left handed space */
			v[0] = -v[0];
			v[3] = -v[3];
			for (c = 0; c < 3; ++c)
			{
				m->bmin[c] = SDL_min(m->bmin[c], v[c]);
				m->bmax[c] = SDL_max(m->bmax[c], v[c]);
			}
		}
		glGenBuffers(1, &b->vbo);
		glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * 32, data, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		free(data);
		b->count = nv;
		tex[63] = '\0';
		if (tex[0])
		{
			char texPath[1200];
			int32_t w, h, comp;
			uint8_t *pixels;
			snprintf(texPath, sizeof texPath, "%s/models/%s/%s", getSettingsDir(), m->name, tex);
			pixels = stbi_load(texPath, &w, &h, &comp, 4);
			if (pixels)
			{
				glGenTextures(1, &b->tex);
				glBindTexture(GL_TEXTURE_2D, b->tex);
				glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
				stbi_image_free(pixels);
				glBindTexture(GL_TEXTURE_2D, g_boundTexture);
			}
		}
		m->batchCount = i + 1;
	}
	fclose(f);
	m->loaded = true;
	m->failed = m->batchCount == 0;
	/* Wheel size and wheel base from the wheel pivots (the pivot height is the radius: wheels stand on y = 0) */
	{
		float front = 0.0f, rear = 0.0f;
		int32_t nf = 0, nr = 0;
		m->wheelRadius = 0.0f;
		for (i = 0; i < (uint32_t)m->batchCount; ++i)
		{
			const CarBatch *b = &m->batches[i];
			if (!b->wheel || !b->spins)
				continue;
			m->wheelRadius = SDL_max(m->wheelRadius, b->pivot[1] - m->bmin[1]);
			if (b->wheel <= 2)
				front += b->pivot[2], ++nf;
			else
				rear += b->pivot[2], ++nr;
		}
		if (m->wheelRadius < 0.05f)
			m->wheelRadius = 0.33f;
		m->wheelBase = (nf && nr) ? fabsf(front / nf - rear / nr) : 2.6f;
	}
	fprintf(stderr, "Car model \"%s\": %d batches%s\n", m->name, m->batchCount, m->failed ? " (failed)" : "");
	return !m->failed;
}

/* Fits the model to the original car: body length and center, wheels on the ground */
static void carFitModel(CarGeo *g)
{
	float bmin[3] = {1e9f, 1e9f, 1e9f}, bmax[3] = {-1e9f, -1e9f, -1e9f}, ground = 1e9f;
	const CarModel *m = g->model;
	int32_t i, c;
	uint32_t k;
	for (i = 0; i < 20; ++i)
	{
		const uint8_t *part = carGeoPart(g->geo, i);
		const uint32_t nv = *(const uint32_t *)part;
		const int32_t *pos = (const int32_t *)(part + 8);
		const int16_t *v = *(int16_t *const *)(part + 0x14);
		const BOOL body = i <= 8, wheel = i >= 12 && i <= 19;
		if (!nv || !v || !(body || wheel))
			continue;
		for (k = 0; k < nv; ++k)
		{
			float p[3];
			for (c = 0; c < 3; ++c)
				p[c] = pos[c] / 65536.0f + v[k * 3 + c] / 256.0f;
			if (body)
			{
				for (c = 0; c < 3; ++c)
				{
					bmin[c] = SDL_min(bmin[c], p[c]);
					bmax[c] = SDL_max(bmax[c], p[c]);
				}
			}
			else
			{
				ground = SDL_min(ground, p[1]);
			}
		}
	}
	if (ground > 1e8f)
		ground = bmin[1];
	g->scale = (bmax[2] - bmin[2]) / SDL_max(m->bmax[2] - m->bmin[2], 0.01f);
	g->offset[0] = (bmin[0] + bmax[0]) * 0.5f - (m->bmin[0] + m->bmax[0]) * 0.5f * g->scale;
	g->offset[1] = ground - m->bmin[1] * g->scale;
	g->offset[2] = (bmin[2] + bmax[2]) * 0.5f - (m->bmin[2] + m->bmax[2]) * 0.5f * g->scale;
}

/* Game hook: a car GEO model was loaded */
REALIGN STDCALL void carModelLoaded(const char *fileName, const uint8_t *geo)
{
	char name[32];
	const char *base = fileName, *p;
	CarGeo *g = NULL;
	int32_t i;
	for (p = fileName; *p; ++p)
		if (*p == '/' || *p == '\\' || *p == ':')
			base = p + 1;
	for (i = 0; i < (int32_t)sizeof name - 1 && base[i] && base[i] != '.'; ++i)
		name[i] = tolower((unsigned char)base[i]);
	name[i] = '\0';
	for (i = 0; i < g_carGeoCount; ++i)
		if (g_carGeos[i].geo == geo)
			g = &g_carGeos[i];
	if (!g)
	{
		if (g_carGeoCount >= CAR_MAX_GEOS)
			memmove(g_carGeos, g_carGeos + 1, sizeof(CarGeo) * --g_carGeoCount);
		g = &g_carGeos[g_carGeoCount++];
	}
	g->geo = geo;
	g->model = carFindModel(name);
	g->fitted = false;
	snprintf(g->name, sizeof g->name, "%s", name);
}

/* Game hook: a car part is drawn. Sets carPartSkip when the part is replaced by a model. */
/* Updates the wheel spin and steering of a car from its motion since the previous frame */
static CarMotion *carUpdateMotion(const uint8_t *object, const CarGeo *g)
{
	CarMotion *cm = NULL, *oldest = &g_carMotion[0];
	const int32_t *pos = (const int32_t *)(object + 0x74), *mat = (const int32_t *)(object + 0x50);
	const uint32_t ticks = SDL_GetTicks();
	double p[3], heading;
	int32_t i;
	for (i = 0; i < CAR_MAX_DRAWS; ++i)
	{
		if (g_carMotion[i].object == object)
		{
			cm = &g_carMotion[i];
			break;
		}
		if (g_carMotion[i].frame < oldest->frame)
			oldest = &g_carMotion[i];
	}
	for (i = 0; i < 3; ++i)
		p[i] = pos[i] / 65536.0;
	heading = SDL_atan2(mat[6], mat[8]);
	if (!cm)
	{
		cm = oldest;
		memset(cm, 0, sizeof *cm);
		cm->object = object;
	}
	else if (cm->frame != g_carFrame)
	{
		const double dt = (ticks - cm->ticks) / 1000.0;
		const double dx = p[0] - cm->pos[0], dy = p[1] - cm->pos[1], dz = p[2] - cm->pos[2];
		const double forward = (dx * mat[6] + dy * mat[7] + dz * mat[8]) / 65536.0;
		const CarModel *m = g->model;
		if (dt > 0.0 && dt < 0.5 && fabs(forward) < 20.0)
		{
			const double radius = m->wheelRadius * g->scale, speed = forward / dt;
			double yaw = heading - cm->heading, target = cm->steer;
			while (yaw > M_PI)
				yaw -= 2.0 * M_PI;
			while (yaw < -M_PI)
				yaw += 2.0 * M_PI;
			cm->spin += forward / radius;
			while (cm->spin > M_PI)
				cm->spin -= 2.0 * M_PI;
			while (cm->spin < -M_PI)
				cm->spin += 2.0 * M_PI;
			if (fabs(speed) > 1.0)
				target = SDL_atan2(m->wheelBase * g->scale * (yaw / dt), speed);
			else if (fabs(speed) < 0.1)
				target = 0.0;
			/* Collisions and slides also turn the car: limit the angle like the steering of a real car at speed */
			{
				const double limit = SDL_min(0.6, 5.0 / SDL_max(fabs(speed), 1.0));
				target = SDL_max(-limit, SDL_min(limit, target));
			}
			cm->steer += (target - cm->steer) * SDL_min(1.0, dt / 0.15);
		}
	}
	else
	{
		return cm;
	}
	memcpy(cm->pos, p, sizeof p);
	cm->heading = heading;
	cm->ticks = ticks;
	cm->frame = g_carFrame;
	return cm;
}

REALIGN STDCALL void carPartHook(const uint8_t *part, const int32_t *m, const int32_t *t, const uint8_t *object)
{
	int32_t gi, i = -1;
	carPartSkip = 0;
	for (gi = g_carGeoCount - 1; gi >= 0 && i < 0; --gi)
	{
		int32_t k;
		if (!g_carGeos[gi].model || g_carGeos[gi].model->failed)
			continue;
		for (k = 0; k < 32; ++k)
		{
			if (carGeoPart(g_carGeos[gi].geo, k) == part)
			{
				i = k;
				break;
			}
		}
		if (i >= 0)
			break;
	}
	if (i < 0 || !gfx.carModels)
		return;
	/* Shadow (part 9) stays, everything else is replaced */
	if (i == 9 || i == 10)
		return;
	carPartSkip = 1;
	if ((i == 1 || i == 21) && g_carDrawCount < CAR_MAX_DRAWS)
	{
		const int32_t *pos = (const int32_t *)(part + 8);
		CarDraw *d = &g_carDraws[g_carDrawCount];
		int32_t r, c;
		for (r = 0; r < 3; ++r)
			for (c = 0; c < 3; ++c)
				d->rot[r * 3 + c] = m[c * 3 + r] / 65536.0f; /* Row vector convention in the game */
		for (r = 0; r < 3; ++r)
			d->origin[r] = (t[r] - (pos[0] / 65536.0f * m[0 * 3 + r] + pos[1] / 65536.0f * m[1 * 3 + r] + pos[2] / 65536.0f * m[2 * 3 + r])) / 65536.0f;
		for (r = 0; r < g_carDrawCount; ++r)
		{
			if (g_carDraws[r].geo == &g_carGeos[gi] && fabsf(g_carDraws[r].origin[0] - d->origin[0]) + fabsf(g_carDraws[r].origin[2] - d->origin[2]) < 0.05f)
				return; /* High and low detail body of the same car */
		}
		d->geo = &g_carGeos[gi];
		d->opaqueDrawn = false;
		d->spin = d->steer = 0.0f;
		if (object && g_carGeos[gi].model->loaded && g_carGeos[gi].fitted)
		{
			const CarMotion *cm = carUpdateMotion(object, &g_carGeos[gi]);
			d->spin = cm->spin;
			d->steer = cm->steer;
		}
		d->proj[0] = dword_4D5294;
		d->proj[1] = flt_4D5298;
		d->proj[2] = flt_4D529C;
		d->proj[3] = flt_4D52A0;
		++g_carDrawCount;
	}
}

static const char g_carVertexSrc[] =
	"#version 120\n"
	"attribute vec3 aPos;\n"
	"attribute vec3 aNrm;\n"
	"attribute vec2 aUV;\n"
	"uniform mat4 uModelView;\n"
	"uniform mat4 uProj;\n"
	"uniform vec4 uWheel;\n"    /* Wheel pivot, w - 1 for wheels */
	"uniform vec2 uWheelRot;\n" /* Spin (around the axle), steering (around the vertical) */
	"varying vec3 vPos;\n"
	"varying vec3 vNrm;\n"
	"varying vec2 vUV;\n"
	"void main()\n"
	"{\n"
	"	vec3 pos = aPos, nrm = aNrm;\n"
	"	if (uWheel.w > 0.5)\n"
	"	{\n"
	"		float cs = cos(uWheelRot.x), ss = sin(uWheelRot.x), ct = cos(uWheelRot.y), st = sin(uWheelRot.y);\n"
	"		mat3 spin = mat3(1.0, 0.0, 0.0, 0.0, cs, ss, 0.0, -ss, cs);\n"
	"		mat3 steer = mat3(ct, 0.0, -st, 0.0, 1.0, 0.0, st, 0.0, ct);\n"
	"		pos = steer * (spin * (pos - uWheel.xyz)) + uWheel.xyz;\n"
	"		nrm = steer * (spin * nrm);\n"
	"	}\n"
	"	vec4 p = uModelView * vec4(pos, 1.0);\n"
	"	vPos = p.xyz;\n"
	"	vNrm = mat3(uModelView) * nrm;\n"
	"	vUV = aUV;\n"
	"	gl_Position = uProj * p;\n"
	"}\n"
;

static const char g_carFragmentSrc[] =
	"#version 120\n"
	"varying vec3 vPos;\n"
	"varying vec3 vNrm;\n"
	"varying vec2 vUV;\n"
	"uniform sampler2D uTex;\n"
	"uniform sampler2D uSky;\n"
	"uniform float uHasTex;\n"
	"uniform float uHasSky;\n"
	"uniform vec4 uColor;\n"
	"uniform vec4 uPbr;\n"       /* metallic, roughness, transmission, clearcoat */
	"uniform vec3 uEmission;\n"
	"uniform vec3 uUp;\n"        /* World up in camera space */
	"uniform vec3 uSun;\n"       /* Default direction towards the sun in camera space */
	"uniform vec4 uSunProj;\n"   /* Sun probe UV -> camera space direction (X/Z, Y/Z) */
	/* Image based lighting from the track's sky panorama (Sky.c): sharp, blurred, diffuse */
	"uniform sampler2D uIbl0;\n"
	"uniform sampler2D uIbl1;\n"
	"uniform sampler2D uIbl2;\n"
	"uniform float uHasIbl;\n"
	"uniform mat4 uCamToWorld;\n"
	"uniform vec2 uIblParams;\n" /* yaw (radians), elevation of the bottom row (degrees) */
	"uniform vec4 uSunIbl;\n"    /* Direction towards the sun (camera space), strength (0 - overcast) */
	"uniform vec3 uSunWorld;\n"
	"vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }\n"
	"vec3 iblDecode(vec3 c) { vec3 l = toLinear(c); return l / max(vec3(1.0) - 0.75 * l, vec3(1.0 / 64.0)); }\n"
	"vec2 iblUV(vec3 w, out float el)\n"
	"{\n"
	"	el = degrees(asin(clamp(w.y, -1.0, 1.0)));\n"
	"	return vec2(fract((atan(w.x, w.z) + uIblParams.x) / 6.2831853 + 0.5), clamp((90.0 - el) / (90.0 - uIblParams.y), 0.0, 0.999));\n"
	"}\n"
	/* Sky radiance in direction w, the ground (scenery, road) below the horizon */
	"vec3 iblSky(sampler2D s, vec3 w, vec3 ground, float soft)\n"
	"{\n"
	"	float el;\n"
	"	vec2 uv = iblUV(w, el);\n"
	"	return mix(ground, iblDecode(texture2D(s, uv).rgb), smoothstep(-1.0 - soft, 1.0 + soft, el));\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	vec3 N = normalize(vNrm);\n"
	"	vec3 V = normalize(-vPos);\n"
	"	if (dot(N, V) < 0.0)\n"
	"		N = -N;\n"
	"	vec4 base = uColor;\n"
	"	if (uHasTex > 0.5)\n"
	"		base *= vec4(toLinear(texture2D(uTex, vUV).rgb), texture2D(uTex, vUV).a);\n"
	/*
	 * Lighting in the game's exposure: its scenery shows textures at about full brightness whatever the sky,
	 * so the base illumination is fixed and only tinted by the sky's hue. A bright sky (e.g. a sunset) and a
	 * visible sun add to it.
	 */
	"	vec3 skyRaw = vec3(0.55, 0.6, 0.7), sunRaw = vec3(0.0);\n"
	"	vec3 L = normalize(uSun);\n"
	"	float sunVisible = 0.0;\n"
	"	if (uHasSky > 0.5)\n"
	"	{\n"
	"		skyRaw = texture2D(uSky, vec2(0.5 / 4.0, 0.5)).rgb;\n"
	"		vec4 sun = texture2D(uSky, vec2(1.5 / 4.0, 0.5));\n"
	"		sunVisible = smoothstep(0.0, 0.002, sun.z) * (1.0 - smoothstep(0.05, 0.1, sun.w));\n"
	"		if (sunVisible > 0.0)\n"
	"			L = normalize(mix(L, normalize(vec3(sun.x * uSunProj.x + uSunProj.z, (1.0 - sun.y) * uSunProj.y + uSunProj.w, 1.0)), sunVisible));\n"
	"		sunRaw = min(texture2D(uSky, vec2(2.5 / 4.0, 0.5)).rgb, vec3(3.0));\n"
	"	}\n"
	"	float skyLuma = dot(skyRaw, vec3(0.2126, 0.7152, 0.0722));\n"
	"	vec3 hue = mix(vec3(1.0), clamp(skyRaw / max(skyLuma, 1e-4), 0.0, 3.0), 0.3);\n"
	"	vec3 sky = max(skyRaw, hue * 0.7);\n"
	"	vec3 ground = hue * 0.06;\n"
	"	vec3 sunColor = mix(hue * 0.55, sunRaw, sunVisible);\n"
	/* Car paint (clear coated): the colored base coat also scatters light diffusely, it's not a bare metal */
	"	float metal = uPbr.w > 0.0 ? min(uPbr.x, 0.45) : uPbr.x, rough = max(uPbr.y, 0.04), a = rough * rough;\n"
	"	vec3 R = reflect(-V, N);\n"
	"	float ry = dot(R, uUp);\n"
	"	vec3 horizon = hue * 0.18;\n"
	"	vec3 env = ry < 0.0 ? mix(horizon, ground, smoothstep(0.0, 0.3 + rough, -ry)) : mix(horizon, sky, smoothstep(0.05, 0.5 + rough, ry));\n"
	"	vec3 F0 = mix(vec3(0.04), base.rgb, metal);\n"
	"	float NoV = clamp(dot(N, V), 0.0, 1.0);\n"
	"	vec3 F = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NoV, 5.0);\n"
	/* Sun: Lambert + GGX */
	"	vec3 H = normalize(L + V);\n"
	"	float NoL = clamp(dot(N, L), 0.0, 1.0), NoH = clamp(dot(N, H), 0.0, 1.0);\n"
	"	float d = NoH * NoH * (a * a - 1.0) + 1.0;\n"
	"	float D = a * a / (3.14159 * d * d);\n"
	"	float k = a * 0.5;\n"
	"	float G = NoL / (NoL * (1.0 - k) + k) * NoV / (NoV * (1.0 - k) + k);\n"
	"	vec3 spec = D * G * F / max(4.0 * NoL * NoV, 0.001);\n"
	"	float hemi = 0.5 + 0.5 * dot(N, uUp);\n"
	"	vec3 envSharpIbl = vec3(-1.0);\n"
	"	vec3 ambient = hue * mix(0.2, 0.55, hemi) + skyRaw * 0.12 * hemi;\n"
	/*
	 * Image based lighting: reflections and diffuse light from the sky panorama, the sun where the panorama has it.
	 * The game-exposure ambient above stays as a minimum, so cars don't go black under a night sky.
	 */
	"	if (uHasIbl > 0.5)\n"
	"	{\n"
	"		vec3 Nw = normalize((uCamToWorld * vec4(N, 0.0)).xyz);\n"
	"		vec3 Rw = normalize((uCamToWorld * vec4(R, 0.0)).xyz);\n"
	"		vec3 skyAvg = iblDecode(texture2D(uIbl2, vec2(0.5, 0.25)).rgb);\n"
	"		vec3 groundIbl = skyAvg * 0.15;\n"
	"		vec3 e0 = iblSky(uIbl0, Rw, groundIbl, 0.0);\n"
	"		vec3 e1 = iblSky(uIbl1, Rw, groundIbl, 4.0);\n"
	"		vec3 e2 = iblSky(uIbl2, Rw, groundIbl, 15.0);\n"
	"		env = rough < 0.3 ? mix(e0, e1, smoothstep(0.05, 0.3, rough)) : mix(e1, e2, smoothstep(0.3, 0.7, rough));\n"
	"		envSharpIbl = e0;\n"
	"		vec3 irradiance = iblSky(uIbl2, Nw, groundIbl, 30.0);\n"
	"		ambient = max(irradiance, ambient * 0.6);\n"
	"		if (uSunIbl.w > 0.0)\n"
	"		{\n"
	"			L = normalize(uSunIbl.xyz);\n"
	"			sunColor = min(iblSky(uIbl0, normalize(uSunWorld), groundIbl, 0.0), vec3(4.0)) * uSunIbl.w;\n"
	"			H = normalize(L + V);\n"
	"			NoL = clamp(dot(N, L), 0.0, 1.0);\n"
	"			NoH = clamp(dot(N, H), 0.0, 1.0);\n"
	"			d = NoH * NoH * (a * a - 1.0) + 1.0;\n"
	"			D = a * a / (3.14159 * d * d);\n"
	"			G = NoL / (NoL * (1.0 - k) + k) * NoV / (NoV * (1.0 - k) + k);\n"
	"			spec = D * G * F / max(4.0 * NoL * NoV, 0.001);\n"
	"		}\n"
	"		else\n"
	"		{\n"
	"			sunColor = vec3(0.0);\n"
	"		}\n"
	"	}\n"
	"	vec3 diffuse = base.rgb * (1.0 - metal);\n"
	"	vec3 color = diffuse * (ambient + sunColor * NoL * 0.9) + spec * sunColor * NoL + env * F;\n"
	/* Clear coat: an extra sharp reflection layer on car paint */
	"	float Fc = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);\n"
	"	vec3 envSharp = ry < 0.0 ? mix(horizon, ground, smoothstep(0.0, 0.08, -ry)) : mix(horizon, sky, smoothstep(0.05, 0.25, ry));\n"
	"	if (envSharpIbl.x >= 0.0)\n"
	"		envSharp = envSharpIbl;\n"
	"	color = mix(color, envSharp, uPbr.w * Fc);\n"
	"	color += uEmission;\n"
	"	float alpha = base.a;\n"
	/* Glass: mostly transparent, reflective at grazing angles */
	"	if (uPbr.z > 0.5)\n"
	"	{\n"
	"		float Fg = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);\n"
	"		color = env * Fg + vec3(0.01, 0.012, 0.015);\n"
	"		alpha = clamp(0.35 + Fg, 0.0, 1.0);\n"
	"	}\n"
	/* The scene is stored in the game's gamma space, highlights are compressed like the post-processing expects */
	"	color = color / (1.0 + color * 0.75);\n"
	"	gl_FragData[0] = vec4(pow(color, vec3(1.0 / 2.2)), alpha);\n"
	"	gl_FragData[1] = vec4(1.0, 0.0, 0.0, alpha);\n"
	"}\n"
;

static BOOL carCreateProgram()
{
	const GLchar *vsrc = g_carVertexSrc, *fsrc = g_carFragmentSrc;
	GLuint vs, fs;
	if (g_carProg || g_carProgFailed)
		return g_carProg != 0;
	g_carProgFailed = true;
	vs = glCreateShader(GL_VERTEX_SHADER);
	fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(vs, 1, &vsrc, NULL);
	glShaderSource(fs, 1, &fsrc, NULL);
	glCompileShader(vs);
	glCompileShader(fs);
	if (!checkShaderCompilation(vs) || !checkShaderCompilation(fs))
	{
		fprintf(stderr, "Car models: cannot compile shaders\n");
		glDeleteShader(vs);
		glDeleteShader(fs);
		return false;
	}
	g_carProg = glCreateProgram();
	glAttachShader(g_carProg, vs);
	glAttachShader(g_carProg, fs);
	glBindAttribLocation(g_carProg, 0, "aPos");
	glBindAttribLocation(g_carProg, 1, "aNrm");
	glBindAttribLocation(g_carProg, 2, "aUV");
	glLinkProgram(g_carProg);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!checkShaderProgram(g_carProg))
	{
		glDeleteProgram(g_carProg);
		g_carProg = 0;
		return false;
	}
	g_carProgFailed = false;
	return true;
}

static void carDrawBatches(const CarModel *m, const CarDraw *d, BOOL transparent)
{
	int32_t i;
	for (i = 0; i < m->batchCount; ++i)
	{
		const CarBatch *b = &m->batches[i];
		if ((b->transmission > 0.5f || b->color[3] < 0.99f) != transparent || !b->count)
			continue;
		glUniform4f(glGetUniformLocation(g_carProg, "uColor"), b->color[0], b->color[1], b->color[2], b->color[3]);
		glUniform4f(glGetUniformLocation(g_carProg, "uPbr"), b->metallic, b->roughness, b->transmission, b->clearcoat);
		glUniform3f(glGetUniformLocation(g_carProg, "uEmission"), b->emission[0], b->emission[1], b->emission[2]);
		glUniform1f(glGetUniformLocation(g_carProg, "uHasTex"), b->tex ? 1.0f : 0.0f);
		glUniform4f(glGetUniformLocation(g_carProg, "uWheel"), b->pivot[0], b->pivot[1], b->pivot[2], b->wheel ? 1.0f : 0.0f);
		glUniform2f(glGetUniformLocation(g_carProg, "uWheelRot"), b->spins ? d->spin : 0.0f, b->wheel == 1 || b->wheel == 2 ? d->steer : 0.0f);
		glBindTexture(GL_TEXTURE_2D, b->tex);
		glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (const void *)0);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (const void *)12);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (const void *)24);
		glDrawArrays(GL_TRIANGLES, 0, b->count);
	}
}

/* Draws the queued car models into the current (main) view */
/*
 * Draws the queued car models. Opaque parts are drawn early, right after the sky (before the scene's polygons), so
 * everything the game draws later - including translucent rain, smoke and lights - is depth tested against them.
 * Transparent parts (glass) are drawn at the end, over the scene behind them.
 */
static void carModelsDraw(BOOL opaqueOnly)
{
	int32_t i;
	if (!g_carDrawCount)
		return;
	drawTriangles();
	if (!carCreateProgram())
	{
		g_carDrawCount = 0;
		return;
	}

	glUseProgram(g_carProg);
	glDisableVertexAttribArray(g_aPositionLoc);
	glDisableVertexAttribArray(g_aTexCoordLoc);
	glDisableVertexAttribArray(g_aColorLoc);
	glDisableVertexAttribArray(g_aFogLoc);
	if (g_aMaskLoc >= 0)
		glDisableVertexAttribArray(g_aMaskLoc);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glUniform1i(glGetUniformLocation(g_carProg, "uTex"), 0);
	glUniform1i(glGetUniformLocation(g_carProg, "uSky"), 1);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, g_post.sky.tex);
	glActiveTexture(GL_TEXTURE0);
	{
		float camToWorld[16], yaw = 0.0f, elevationMin = 0.0f, sunWorld[3] = {0.0f, 1.0f, 0.0f}, sunStrength = 0.0f;
		const BOOL ibl = skyIblBind(2, camToWorld, &yaw, &elevationMin, sunWorld, &sunStrength);
		glUniform1f(glGetUniformLocation(g_carProg, "uHasIbl"), ibl ? 1.0f : 0.0f);
		if (ibl)
		{
			/* Sun in camera space: camera = transpose(camToWorld) * world */
			float sunCam[3];
			int32_t c;
			for (c = 0; c < 3; ++c)
				sunCam[c] = camToWorld[c * 4 + 0] * sunWorld[0] + camToWorld[c * 4 + 1] * sunWorld[1] + camToWorld[c * 4 + 2] * sunWorld[2];
			glUniform1i(glGetUniformLocation(g_carProg, "uIbl0"), 2);
			glUniform1i(glGetUniformLocation(g_carProg, "uIbl1"), 3);
			glUniform1i(glGetUniformLocation(g_carProg, "uIbl2"), 4);
			glUniformMatrix4fv(glGetUniformLocation(g_carProg, "uCamToWorld"), 1, GL_FALSE, camToWorld);
			glUniform2f(glGetUniformLocation(g_carProg, "uIblParams"), yaw, elevationMin);
			glUniform4f(glGetUniformLocation(g_carProg, "uSunIbl"), sunCam[0], sunCam[1], sunCam[2], sunStrength);
			glUniform3f(glGetUniformLocation(g_carProg, "uSunWorld"), sunWorld[0], sunWorld[1], sunWorld[2]);
		}
	}
	glUniform1f(glGetUniformLocation(g_carProg, "uHasSky"), (g_post.ok && g_post.sky.tex && g_prevFrameHas3D) ? 1.0f : 0.0f);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	for (i = 0; i < g_carDrawCount; ++i)
	{
		const CarDraw *d = &g_carDraws[i];
		CarGeo *g = (CarGeo *)d->geo;
		if (!g->model->loaded)
			carLoadModel(g->model);
		if (g->model->failed)
			continue;
		if (!g->fitted)
		{
			carFitModel(g);
			g->fitted = true;
			fprintf(stderr, "Car \"%s\": replacement model, scale %.3f\n", g->name, g->scale);
		}
		const float s = g->scale;
		/* uModelView = [R * s | R * offset + origin], column major */
		float mv[16];
		int32_t r;
		for (r = 0; r < 3; ++r)
		{
			mv[0 + r] = d->rot[r * 3 + 0] * s;
			mv[4 + r] = d->rot[r * 3 + 1] * s;
			mv[8 + r] = d->rot[r * 3 + 2] * s;
			mv[12 + r] = d->rot[r * 3 + 0] * g->offset[0] + d->rot[r * 3 + 1] * g->offset[1] + d->rot[r * 3 + 2] * g->offset[2] + d->origin[r];
		}
		mv[3] = mv[7] = mv[11] = 0.0f;
		mv[15] = 1.0f;
		glUniformMatrix4fv(glGetUniformLocation(g_carProg, "uModelView"), 1, GL_FALSE, mv);
		{
			/*
			 * Camera space (metres, X right, Y up, Z forward) -> clip space, matching the game: the screen
			 * position is center + scale * XY / Z in 640x480 units (mapped to the current clip window),
			 * the depth is 1 - 1/Z.
			 */
			const float x0 = g_clipWindow[0], y0 = g_clipWindow[1], w = g_clipWindow[2] - g_clipWindow[0], h = g_clipWindow[3] - g_clipWindow[1];
			const float sx = d->proj[0], sy = d->proj[1], cx = d->proj[2], cy = d->proj[3];
			const float proj[16] = {
				2.0f * sx / w, 0.0f, 0.0f, 0.0f,
				0.0f, -2.0f * sy / h, 0.0f, 0.0f,
				2.0f * (cx - x0) / w - 1.0f, 1.0f - 2.0f * (cy - y0) / h, 1.0f, 1.0f,
				0.0f, 0.0f, -2.0f, 0.0f,
			};
			glUniformMatrix4fv(glGetUniformLocation(g_carProg, "uProj"), 1, GL_FALSE, proj);
			/* Sun direction from the sky probe (screen position of the sun in the previous frame) */
			glUniform4f(glGetUniformLocation(g_carProg, "uSunProj"), 640.0f / sx, 480.0f / sy, -cx / sx, -cy / sy);
		}
		/* Up and sun: car space up, sun high and slightly behind the camera */
		glUniform3f(glGetUniformLocation(g_carProg, "uUp"), d->rot[1], d->rot[4], d->rot[7]);
		glUniform3f(glGetUniformLocation(g_carProg, "uSun"), 0.3f + d->rot[1], 0.9f * d->rot[4] + 0.3f, -0.4f + d->rot[7]);
		if (!d->opaqueDrawn)
		{
			glDisable(GL_BLEND);
			carDrawBatches(g->model, d, false);
			((CarDraw *)d)->opaqueDrawn = true;
		}
		if (!opaqueOnly)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			carDrawBatches(g->model, d, true);
			glDepthMask(GL_TRUE);
		}
	}
	if (!opaqueOnly)
	{
		g_carDrawCount = 0;
		++g_carFrame;
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	useGameProgram(true);
	glUniform1f(g_uTextureEnabledLoc, g_textureEnabled);
	glDepthMask(g_depthMask);
}

/* Game hook: the scene's polygons were drawn */
REALIGN STDCALL void carModelsFlush()
{
	carModelsDraw(false);
}
