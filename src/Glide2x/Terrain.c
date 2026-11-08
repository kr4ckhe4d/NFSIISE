// SPDX-License-Identifier: MIT

/*
 * Distant terrain from real elevation data.
 *
 * "<settings>/terrain/trNN/model.cmdl" (made by tools/terrain/make_terrain.py) is a mesh in the game's world
 * coordinates (metres) around track NN. It is drawn like a car model (CarModels.c: same shader, image based
 * lighting, the game's projection and depth), right after the sky and before the game's geometry, so nearer
 * scenery covers it. The game's own stand-ins for distant terrain (flat cards, e.g. its 100 m terrain ring) are
 * hidden by texture hash.
 *
 * "<settings>/terrain/trNN/terrain.txt" (key=value):
 *   hide=<hash>,<hash>,...   textures of the game's stand-ins (16 hex digits, as in texdump/ and textures/)
 *   fog_cap=<m>              the game's fog is applied up to this distance (it would hide everything farther)
 *   fog_max=<0..1>           ... and at most this much
 *   visibility=<m>           aerial perspective beyond: fog = 1 - exp(-distance / visibility)
 *   game_fog_scale=<0..1>    the game's fog on its own geometry is scaled by this (default 0.5)
 *   dissolve=<from> <to>     the game's geometry dissolves between these distances (its draw distance, ~500 m)
 *
 * This file is included from OpenGL2.c.
 */

static void terrainLoad(int32_t track)
{
	char path[1024], line[4096];
	FILE *f;
	g_terrain.track = track;
	g_terrain.active = false;
	g_terrain.hideCount = 0;
	g_terrain.fogCap = 350.0f;
	g_terrain.fogMax = 0.6f;
	g_terrain.visibility = 40000.0f;
	g_terrain.fogScale = 0.5f;
	g_terrain.dissolve[0] = 380.0f;
	g_terrain.dissolve[1] = 490.0f;
	memset(&g_terrain.geo, 0, sizeof g_terrain.geo);
	if (track < 0 || !getSettingsDir())
		return;
	snprintf(path, sizeof path, "%s/terrain/tr%02d/terrain.txt", getSettingsDir(), track);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f))
	{
		char *v = strchr(line, '=');
		if (!v)
			continue;
		*v++ = '\0';
		if (!strcmp(line, "hide"))
		{
			char *tok = strtok(v, ", \t\r\n");
			while (tok && g_terrain.hideCount < (int32_t)(sizeof g_terrain.hide / sizeof *g_terrain.hide))
			{
				g_terrain.hide[g_terrain.hideCount++] = strtoull(tok, NULL, 16);
				tok = strtok(NULL, ", \t\r\n");
			}
		}
		else if (!strcmp(line, "fog_cap"))
			g_terrain.fogCap = atof(v);
		else if (!strcmp(line, "fog_max"))
			g_terrain.fogMax = atof(v);
		else if (!strcmp(line, "visibility"))
			g_terrain.visibility = SDL_max((float)atof(v), 100.0f);
		else if (!strcmp(line, "game_fog_scale"))
			g_terrain.fogScale = atof(v);
		else if (!strcmp(line, "dissolve"))
			sscanf(v, "%f %f", &g_terrain.dissolve[0], &g_terrain.dissolve[1]);
	}
	fclose(f);
	{
		char dir[64];
		snprintf(dir, sizeof dir, "terrain/tr%02d", track);
		g_terrain.geo.model = carFindModel(dir);
	}
	if (g_terrain.geo.model && !g_terrain.geo.model->loaded)
		carLoadModel(g_terrain.geo.model);
	if (!g_terrain.geo.model || g_terrain.geo.model->failed)
	{
		g_terrain.geo.model = NULL;
		return;
	}
	g_terrain.geo.scale = 1.0f;
	g_terrain.geo.fitted = true;
	g_terrain.geo.terrain = true;
	snprintf(g_terrain.geo.name, sizeof g_terrain.geo.name, "terrain tr%02d", track);
	g_terrain.active = true;
	fprintf(stderr, "Terrain for track %d: %d hidden textures\n", track, g_terrain.hideCount);
}

/* True for textures of the game's stand-ins for distant terrain, hidden while the real terrain is drawn */
static inline BOOL terrainHides(uint64_t hash)
{
	int32_t i;
	if (!g_terrain.active)
		return false;
	for (i = 0; i < g_terrain.hideCount; ++i)
		if (g_terrain.hide[i] == hash)
			return true;
	return false;
}

/*
 * The game's stand-ins for distant terrain: its terrain ring (textured cards and an untextured base strip) stays at
 * 100 m around the camera, and textures listed in "hide".
 */
static inline BOOL terrainHidesTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
	if (!g_terrain.active)
		return false;
	if (a->oow > 1.0f / 101.0f && a->oow < 1.0f / 99.0f && b->oow > 1.0f / 101.0f && b->oow < 1.0f / 99.0f && c->oow > 1.0f / 101.0f && c->oow < 1.0f / 99.0f)
		return true;
	return g_textureEnabled > 0.5f && terrainHides(g_boundHash);
}

/* Queues the terrain for the current view (camera captured by skyHook); called before its first 3D triangle */
static void terrainQueue()
{
	CarDraw *d;
	int32_t r, c;
	if (!gfx.terrain)
	{
		g_terrain.active = false;
		return;
	}
	if (g_terrain.track != gfxCurrentTrack)
		terrainLoad(gfxCurrentTrack);
	{
		/* The game's geometry: lighter fog, dissolving near the draw distance (the terrain shows behind) */
		const BOOL on = g_terrain.active;
		glUseProgram(g_shaderProgram);
		glUniform3f(glGetUniformLocation(g_shaderProgram, "uTerrainView"), g_terrain.fogScale, g_terrain.dissolve[0], on ? g_terrain.dissolve[1] : 0.0f);
	}
	if (!g_terrain.active || g_carDrawCount >= CAR_MAX_DRAWS)
		return;
	d = &g_carDraws[g_carDrawCount++];
	memset(d, 0, sizeof *d);
	d->geo = &g_terrain.geo;
	/* World -> camera: rotation = transpose(camera -> world), then the camera position */
	for (r = 0; r < 3; ++r)
		for (c = 0; c < 3; ++c)
			d->rot[r * 3 + c] = g_skyView.camToWorld[r * 4 + c];
	for (r = 0; r < 3; ++r)
		d->origin[r] = -(d->rot[r * 3 + 0] * g_skyView.camPos[0] + d->rot[r * 3 + 1] * g_skyView.camPos[1] + d->rot[r * 3 + 2] * g_skyView.camPos[2]);
	memcpy(d->proj, g_skyView.proj, sizeof d->proj);
}
