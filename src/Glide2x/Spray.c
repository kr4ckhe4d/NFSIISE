// SPDX-License-Identifier: MIT

/*
 * Spray behind cars on wet roads.
 *
 * Every car body passing through carPartHook() reports its game object; its world position (object + 0x74) and
 * orientation (object + 0x50) give its velocity. While it rains (g_rainLevel, from the renderer's rain detector) and a
 * car is fast, its rear wheels emit mist particles in world space. They are simulated and drawn for the main view at
 * carModelsFlush() (after the scene, before the HUD) as soft camera facing sprites with the game's projection and
 * depth, tinted by the fog colour. Strength: RainSpray (0 - off).
 *
 * This file is included from OpenGL2.c (after CarModels.c).
 */

#define SPRAY_MAX_CARS      16
#define SPRAY_MAX_PARTICLES 1536

typedef struct
{
	const uint8_t *object;
	float pos[3], emit;
	uint32_t ticks, frame;
} SprayCar;

typedef struct
{
	float pos[3], vel[3], age, life, size, alpha;
} SprayParticle;

static SprayCar g_sprayCars[SPRAY_MAX_CARS];
static SprayParticle g_sprayParticles[SPRAY_MAX_PARTICLES];
static int32_t g_sprayNext;
static uint32_t g_sprayFrame, g_sprayTicks, g_sprayRandom = 12345u;
static GLuint g_sprayProg;
static BOOL g_sprayProgFailed;

static float sprayRandom()
{
	g_sprayRandom = g_sprayRandom * 1664525u + 1013904223u;
	return (g_sprayRandom >> 8) / 16777216.0f;
}

/* Called for each car body the game draws (any car, any view): emits spray behind fast cars on wet roads */
static void sprayCar(const uint8_t *object)
{
	SprayCar *c = NULL, *oldest = &g_sprayCars[0];
	const int32_t *p = (const int32_t *)(object + 0x74), *m = (const int32_t *)(object + 0x50);
	const uint32_t ticks = SDL_GetTicks();
	float pos[3], dt, v[3], speed;
	int32_t i, w;
	if (gfx.rainSpray <= 0.0f)
		return;
	for (i = 0; i < SPRAY_MAX_CARS; ++i)
	{
		if (g_sprayCars[i].object == object)
		{
			c = &g_sprayCars[i];
			break;
		}
		if (g_sprayCars[i].ticks < oldest->ticks)
			oldest = &g_sprayCars[i];
	}
	for (i = 0; i < 3; ++i)
		pos[i] = p[i] / 65536.0f;
	if (!c)
	{
		c = oldest;
		memset(c, 0, sizeof *c);
		c->object = object;
		c->frame = g_sprayFrame - 1;
	}
	else if (c->frame == g_sprayFrame)
	{
		return; /* Already seen in this frame (other parts, the mirror) */
	}
	dt = (ticks - c->ticks) / 1000.0f;
	c->frame = g_sprayFrame;
	if (c->ticks && dt > 0.0f && dt < 0.5f)
	{
		for (i = 0; i < 3; ++i)
			v[i] = (pos[i] - c->pos[i]) / dt;
		speed = SDL_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (speed > 8.0f && speed < 120.0f && g_rainLevel > 0.3f)
		{
			/* Particles per second for both rear wheels */
			const float rate = 90.0f * gfx.rainSpray * SDL_min(1.0f, (speed - 8.0f) / 30.0f) * g_rainLevel;
			float right[3], up[3], fwd[3];
			for (i = 0; i < 3; ++i)
			{
				right[i] = m[0 + i] / 65536.0f;
				up[i] = m[3 + i] / 65536.0f;
				fwd[i] = m[6 + i] / 65536.0f;
			}
			c->emit += rate * dt;
			while (c->emit >= 1.0f)
			{
				SprayParticle *s = &g_sprayParticles[g_sprayNext];
				const float side = (sprayRandom() < 0.5f) ? -0.8f : 0.8f;
				g_sprayNext = (g_sprayNext + 1) % SPRAY_MAX_PARTICLES;
				c->emit -= 1.0f;
				for (w = 0; w < 3; ++w)
				{
					/* Rear wheel contact patch, then thrown up and back, slowed by the air */
					s->pos[w] = pos[w] + right[w] * side + up[w] * 0.2f - fwd[w] * 1.6f;
					s->vel[w] = v[w] * 0.35f + right[w] * side * (0.5f + 1.5f * sprayRandom()) + up[w] * (1.0f + 2.0f * sprayRandom())
						+ (sprayRandom() - 0.5f) * 1.5f;
				}
				s->age = sprayRandom() * dt; /* Spread over the frame */
				s->life = 0.6f + 0.6f * sprayRandom();
				s->size = 0.7f + 0.5f * sprayRandom();
				s->alpha = (0.13f + 0.08f * sprayRandom()) * SDL_min(1.0f, speed / 35.0f);
			}
		}
		else
		{
			c->emit = 0.0f;
		}
	}
	memcpy(c->pos, pos, sizeof pos);
	c->ticks = ticks;
}

static const char g_sprayVertexSrc[] =
	"#version 120\n"
	"attribute vec3 aPos;\n"   /* Camera space */
	"attribute vec3 aUVA;\n"   /* Corner (-1..1), alpha */
	"uniform mat4 uProj;\n"
	"varying vec3 vUVA;\n"
	"void main()\n"
	"{\n"
	"	vUVA = aUVA;\n"
	"	gl_Position = uProj * vec4(aPos, 1.0);\n"
	"}\n"
;

static const char g_sprayFragmentSrc[] =
	"#version 120\n"
	"varying vec3 vUVA;\n"
	"uniform vec3 uColor;\n"
	"uniform float uShape;\n" /* 0 - soft round puff, 1 - soft rounded rectangle (car contact shadow) */
	"void main()\n"
	"{\n"
	"	vec2 q = abs(vUVA.xy);\n"
	"	float box = pow(pow(q.x, 4.0) + pow(q.y, 4.0), 0.25);\n"
	"	float a = vUVA.z * (uShape > 0.5 ? 1.0 - smoothstep(0.45, 1.0, box) : exp(-3.0 * dot(vUVA.xy, vUVA.xy)));\n"
	"	gl_FragData[0] = vec4(uColor, a);\n"
	"	gl_FragData[1] = vec4(1.0, 0.0, 0.0, a);\n"
	"}\n"
;

static BOOL sprayCreateProgram()
{
	const GLchar *vsrc = g_sprayVertexSrc, *fsrc = g_sprayFragmentSrc;
	GLuint vs, fs;
	if (g_sprayProg || g_sprayProgFailed)
		return g_sprayProg != 0;
	g_sprayProgFailed = true;
	vs = glCreateShader(GL_VERTEX_SHADER);
	fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(vs, 1, &vsrc, NULL);
	glShaderSource(fs, 1, &fsrc, NULL);
	glCompileShader(vs);
	glCompileShader(fs);
	if (!checkShaderCompilation(vs) || !checkShaderCompilation(fs))
	{
		fprintf(stderr, "Spray: cannot compile shaders\n");
		glDeleteShader(vs);
		glDeleteShader(fs);
		return false;
	}
	g_sprayProg = glCreateProgram();
	glAttachShader(g_sprayProg, vs);
	glAttachShader(g_sprayProg, fs);
	glBindAttribLocation(g_sprayProg, 0, "aPos");
	glBindAttribLocation(g_sprayProg, 1, "aUVA");
	glLinkProgram(g_sprayProg);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!checkShaderProgram(g_sprayProg))
	{
		glDeleteProgram(g_sprayProg);
		g_sprayProg = 0;
		return false;
	}
	g_sprayProgFailed = false;
	return true;
}

/* Simulates the particles and draws them for the main view (camera from the horizon hook); called at the scene's end */
static void sprayDraw()
{
	static float verts[SPRAY_MAX_PARTICLES * 6 * 3], uva[SPRAY_MAX_PARTICLES * 6 * 3];
	const uint32_t ticks = SDL_GetTicks();
	const float dt = g_sprayTicks ? SDL_min((ticks - g_sprayTicks) / 1000.0f, 0.1f) : 0.0f;
	const float *R = g_skyView.camToWorld, *cp = g_skyView.camPos;
	int32_t i, n = 0, k;
	if (!g_fullWidthView)
		return;
	++g_sprayFrame;
	g_sprayTicks = ticks;
	for (i = 0; i < SPRAY_MAX_PARTICLES; ++i)
	{
		SprayParticle *s = &g_sprayParticles[i];
		float rel[3], cam[3], t, size, alpha;
		if (s->age >= s->life)
			continue;
		s->age += dt;
		{
			const float drag = (float)SDL_exp(-2.5 * dt);
			for (k = 0; k < 3; ++k)
				s->vel[k] *= drag;
			s->vel[1] -= 1.5f * dt;
			for (k = 0; k < 3; ++k)
				s->pos[k] += s->vel[k] * dt;
		}
		if (s->age >= s->life)
			continue;
		for (k = 0; k < 3; ++k)
			rel[k] = s->pos[k] - cp[k];
		for (k = 0; k < 3; ++k)
			cam[k] = R[k * 4 + 0] * rel[0] + R[k * 4 + 1] * rel[1] + R[k * 4 + 2] * rel[2];
		if (cam[2] < 0.6f)
			continue;
		t = s->age / s->life;
		size = s->size * (1.0f + 3.0f * t);
		alpha = s->alpha * SDL_min(1.0f, t * 8.0f) * (1.0f - t) * (1.0f - t);
		{
			static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
			int32_t q;
			for (q = 0; q < 6; ++q)
			{
				float *vp = &verts[(n * 6 + q) * 3], *up = &uva[(n * 6 + q) * 3];
				vp[0] = cam[0] + corner[q][0] * size;
				vp[1] = cam[1] + corner[q][1] * size;
				vp[2] = cam[2];
				up[0] = corner[q][0];
				up[1] = corner[q][1];
				up[2] = alpha;
			}
		}
		++n;
	}
	if (!n || !sprayCreateProgram())
		return;
	drawTriangles();
	glUseProgram(g_sprayProg);
	glDisableVertexAttribArray(g_aPositionLoc);
	glDisableVertexAttribArray(g_aTexCoordLoc);
	glDisableVertexAttribArray(g_aColorLoc);
	glDisableVertexAttribArray(g_aFogLoc);
	if (g_aMaskLoc >= 0)
		glDisableVertexAttribArray(g_aMaskLoc);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, uva);
	{
		/* Same projection as the car models (CarModels.c) */
		const float x0 = g_clipWindow[0], y0 = g_clipWindow[1], w = g_clipWindow[2] - g_clipWindow[0], h = g_clipWindow[3] - g_clipWindow[1];
		const float sx = g_skyView.proj[0], sy = g_skyView.proj[1], cx = g_skyView.proj[2], cy = g_skyView.proj[3];
		const float proj[16] = {
			2.0f * sx / w, 0.0f, 0.0f, 0.0f,
			0.0f, -2.0f * sy / h, 0.0f, 0.0f,
			2.0f * (cx - x0) / w - 1.0f, 1.0f - 2.0f * (cy - y0) / h, 1.0f, 1.0f,
			0.0f, 0.0f, -2.0f, 0.0f,
		};
		/* Mist: lighter than the fog colour, darker at night */
		const float l = 0.2126f * g_fogColor[0] + 0.7152f * g_fogColor[1] + 0.0722f * g_fogColor[2];
		glUniformMatrix4fv(glGetUniformLocation(g_sprayProg, "uProj"), 1, GL_FALSE, proj);
		glUniform1f(glGetUniformLocation(g_sprayProg, "uShape"), 0.0f);
		glUniform3f(glGetUniformLocation(g_sprayProg, "uColor"), 0.5f * g_fogColor[0] + 0.5f * l + 0.15f, 0.5f * g_fogColor[1] + 0.5f * l + 0.15f, 0.5f * g_fogColor[2] + 0.5f * l + 0.15f);
	}
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, n * 6);
	glDisableVertexAttribArray(1);
	useGameProgram(true);
	glUniform1f(g_uTextureEnabledLoc, g_textureEnabled);
	glDepthMask(g_depthMask);
	glBlendFunc(GL_SRC_ALPHA, g_blendFuncDFactor);
}

/*
 * Soft contact shadows under the replacement car models (instead of the game's flat shadow quad): a rounded rectangle
 * slightly larger than the car's footprint, just above the ground under the wheels. Drawn after the scene (so the road
 * is below it) and before the glass; the car body hides the part under it.
 */
static void sprayCarShadows()
{
	static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	float verts[6 * 3], uva[6 * 3];
	int32_t i, q, r;
	if (!g_carDrawCount || !sprayCreateProgram())
		return;
	drawTriangles();
	glUseProgram(g_sprayProg);
	glDisableVertexAttribArray(g_aPositionLoc);
	glDisableVertexAttribArray(g_aTexCoordLoc);
	glDisableVertexAttribArray(g_aColorLoc);
	glDisableVertexAttribArray(g_aFogLoc);
	if (g_aMaskLoc >= 0)
		glDisableVertexAttribArray(g_aMaskLoc);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, uva);
	glUniform1f(glGetUniformLocation(g_sprayProg, "uShape"), 1.0f);
	glUniform3f(glGetUniformLocation(g_sprayProg, "uColor"), 0.0f, 0.0f, 0.0f);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(-1.0f, -4.0f);
	for (i = 0; i < g_carDrawCount; ++i)
	{
		const CarDraw *d = &g_carDraws[i];
		const CarGeo *g = d->geo;
		if (g->terrain || !g->fitted || !g->model || g->model->failed)
			continue;
		for (q = 0; q < 6; ++q)
		{
			const float p[3] = {g->shadow[0] + corner[q][0] * g->shadow[2] * 1.25f, g->shadow[4] + 0.03f, g->shadow[1] + corner[q][1] * g->shadow[3] * 1.12f};
			for (r = 0; r < 3; ++r)
				verts[q * 3 + r] = d->rot[r * 3 + 0] * p[0] + d->rot[r * 3 + 1] * p[1] + d->rot[r * 3 + 2] * p[2] + d->origin[r];
			uva[q * 3 + 0] = corner[q][0];
			uva[q * 3 + 1] = corner[q][1];
			uva[q * 3 + 2] = 0.65f;
		}
		{
			const float x0 = g_clipWindow[0], y0 = g_clipWindow[1], w = g_clipWindow[2] - g_clipWindow[0], h = g_clipWindow[3] - g_clipWindow[1];
			const float sx = d->proj[0], sy = d->proj[1], cx = d->proj[2], cy = d->proj[3];
			const float proj[16] = {
				2.0f * sx / w, 0.0f, 0.0f, 0.0f,
				0.0f, -2.0f * sy / h, 0.0f, 0.0f,
				2.0f * (cx - x0) / w - 1.0f, 1.0f - 2.0f * (cy - y0) / h, 1.0f, 1.0f,
				0.0f, 0.0f, -2.0f, 0.0f,
			};
			glUniformMatrix4fv(glGetUniformLocation(g_sprayProg, "uProj"), 1, GL_FALSE, proj);
		}
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisableVertexAttribArray(1);
	useGameProgram(true);
	glUniform1f(g_uTextureEnabledLoc, g_textureEnabled);
	glDepthMask(g_depthMask);
	glBlendFunc(GL_SRC_ALPHA, g_blendFuncDFactor);
}
