// SPDX-License-Identifier: MIT

/*
 * 3D trees replacing the game's tree billboards.
 *
 * The game draws trees as static crossed billboards (two or more vertical cards sharing the trunk line, textures with
 * transparent background). "<settings>/trees/trNN.txt" lists the billboard textures of track NN and the model that
 * replaces them:
 *   <texture hash> <model> [height scale]       e.g. "D9B073628F4E9030 palm 1.0"
 * Models are "<settings>/trees/<model>/model.cmdl" (tools/trees: base at the origin, height 1, leaves as alpha
 * cutout cards).
 *
 * In the main view, every triangle with a listed texture is projected back into the world (the camera from the
 * horizon hook and the game's projection) and merged into a per-track registry of trees: position = average of the
 * triangles' centroids (the cards are symmetric around the trunk), base and top = the union of their heights over all
 * frames (cards clipped at the screen edge only shrink a frame's extent). The billboard triangles are hidden; at the end
 * of the scene (carModelsFlush) every tree seen in this frame is drawn as a model with the car shader (image based
 * lighting, scenery fog, world mask), scaled to the billboard height and turned by a stable random angle. The rear-view
 * mirror keeps the billboards.
 *
 * This file is included from OpenGL2.c (after CarModels.c).
 */

#define TREE_MAX_KINDS 64
#define TREE_MAX       4096

typedef struct
{
	uint64_t hash;
	CarGeo geo;
	float heightScale;
} TreeKind;

typedef struct
{
	float sum[2], n, base, top;
	int32_t kind;
	uint32_t frame;
} Tree;

static struct
{
	int32_t track;
	TreeKind kinds[TREE_MAX_KINDS];
	int32_t kindCount;
	Tree trees[TREE_MAX];
	int32_t treeCount;
	uint32_t frame;
} g_trees = {-2};

static void treesLoad(int32_t track)
{
	char path[1024], line[512];
	FILE *f;
	g_trees.track = track;
	g_trees.kindCount = 0;
	g_trees.treeCount = 0;
	if (track < 0 || !getSettingsDir())
		return;
	snprintf(path, sizeof path, "%s/trees/tr%02d.txt", getSettingsDir(), track);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f) && g_trees.kindCount < TREE_MAX_KINDS)
	{
		char hash[32], model[64], dir[96];
		float scale = 1.0f;
		TreeKind *k = &g_trees.kinds[g_trees.kindCount];
		if (line[0] == '#' || sscanf(line, "%31s %63s %f", hash, model, &scale) < 2)
			continue;
		snprintf(dir, sizeof dir, "trees/%s", model);
		memset(k, 0, sizeof *k);
		k->hash = strtoull(hash, NULL, 16);
		k->heightScale = scale;
		k->geo.model = carFindModel(dir);
		if (k->geo.model && !k->geo.model->loaded)
			carLoadModel(k->geo.model);
		if (!k->geo.model || k->geo.model->failed)
		{
			fprintf(stderr, "Trees: no model \"%s\"\n", dir);
			continue;
		}
		k->geo.scale = 1.0f;
		k->geo.fitted = true;
		k->geo.terrain = true;
		k->geo.scenery = true;
		snprintf(k->geo.name, sizeof k->geo.name, "tree %s", model);
		++g_trees.kindCount;
	}
	fclose(f);
	fprintf(stderr, "Trees for track %d: %d billboard textures replaced\n", track, g_trees.kindCount);
}

/* Called for each world triangle of the game: true if it is a replaced tree billboard (then it's not drawn) */
static BOOL treesTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
	const GrVertex *v[3] = {a, b, c};
	const float *R = g_skyView.camToWorld, *cp = g_skyView.camPos, *P = g_skyView.proj;
	float lo = 1e9f, hi = -1e9f, cx = 0.0f, cz = 0.0f;
	int32_t k, i, j, best = -1;
	if (!gfx.trees || g_textureEnabled < 0.5f)
		return false;
	if (g_trees.track != gfxCurrentTrack)
		treesLoad(gfxCurrentTrack);
	for (k = 0; k < g_trees.kindCount; ++k)
		if (g_trees.kinds[k].hash == g_boundHash)
			break;
	if (k == g_trees.kindCount)
		return false;
	if (!g_fullWidthView)
		return false; /* The mirror keeps the billboards */
	for (i = 0; i < 3; ++i)
	{
		/* Game projection back to camera space, then world */
		const float Z = 1.0f / SDL_max(v[i]->oow, 1e-6f);
		const float X = (v[i]->x - VertexSnap - P[2]) / P[0] * Z, Y = (v[i]->y - VertexSnap - P[3]) / P[1] * Z;
		float w[3];
		for (j = 0; j < 3; ++j)
			w[j] = cp[j] + X * R[0 * 4 + j] + Y * R[1 * 4 + j] + Z * R[2 * 4 + j];
		lo = SDL_min(lo, w[1]);
		hi = SDL_max(hi, w[1]);
		cx += w[0] / 3.0f;
		cz += w[2] / 3.0f;
	}
	{
		float bestD = 4.0f * 4.0f;
		for (i = 0; i < g_trees.treeCount; ++i)
		{
			const Tree *t = &g_trees.trees[i];
			const float dx = t->sum[0] / t->n - cx, dz = t->sum[1] / t->n - cz;
			const float d = dx * dx + dz * dz;
			if (t->kind == k && d < bestD)
			{
				bestD = d;
				best = i;
			}
		}
	}
	if (best < 0)
	{
		Tree *t;
		if (g_trees.treeCount >= TREE_MAX)
			return true;
		t = &g_trees.trees[g_trees.treeCount];
		best = g_trees.treeCount++;
		memset(t, 0, sizeof *t);
		t->kind = k;
		t->base = lo;
		t->top = hi;
	}
	{
		Tree *t = &g_trees.trees[best];
		/* A running average that settles (a tree's triangles come back every frame) */
		if (t->n < 64.0f)
		{
			t->sum[0] += cx;
			t->sum[1] += cz;
			t->n += 1.0f;
		}
		t->base = SDL_min(t->base, lo);
		t->top = SDL_max(t->top, hi);
		t->frame = g_trees.frame;
	}
	return true;
}

/* Queues the trees seen in this frame as model draws (end of the main view's scene) */
static void treesQueue()
{
	const float *R = g_skyView.camToWorld, *cp = g_skyView.camPos;
	int32_t i, r, c;
	if (!g_fullWidthView)
		return;
	for (i = 0; i < g_trees.treeCount && g_carDrawCount < CAR_MAX_DRAWS; ++i)
	{
		const Tree *t = &g_trees.trees[i];
		const TreeKind *k = &g_trees.kinds[t->kind];
		CarDraw *d;
		float pos[3], h, yaw, cy, sy, m[9];
		if (t->frame != g_trees.frame)
			continue;
		pos[0] = t->sum[0] / t->n;
		pos[1] = t->base;
		pos[2] = t->sum[1] / t->n;
		h = (t->top - t->base) * k->heightScale;
		if (h < 0.5f)
			continue;
		/* Stable random turn from the position */
		yaw = (float)(((uint32_t)(pos[0] * 7.0f) * 2654435761u ^ (uint32_t)(pos[2] * 13.0f) * 40503u) % 6283u) / 1000.0f;
		cy = SDL_cosf(yaw) * h;
		sy = SDL_sinf(yaw) * h;
		/* Model (Y up, scaled to the height, turned) -> world: columns */
		m[0] = cy; m[1] = 0.0f; m[2] = sy;
		m[3] = 0.0f; m[4] = h; m[5] = 0.0f;
		m[6] = -sy; m[7] = 0.0f; m[8] = cy;
		d = &g_carDraws[g_carDrawCount++];
		memset(d, 0, sizeof *d);
		d->geo = &k->geo;
		/* Car space -> camera space: world -> camera rows times the model -> world matrix */
		for (r = 0; r < 3; ++r)
		{
			for (c = 0; c < 3; ++c)
				d->rot[r * 3 + c] = R[r * 4 + 0] * m[0 * 3 + c] + R[r * 4 + 1] * m[1 * 3 + c] + R[r * 4 + 2] * m[2 * 3 + c];
			d->origin[r] = R[r * 4 + 0] * (pos[0] - cp[0]) + R[r * 4 + 1] * (pos[1] - cp[1]) + R[r * 4 + 2] * (pos[2] - cp[2]);
		}
		memcpy(d->proj, g_skyView.proj, sizeof d->proj);
	}
	++g_trees.frame;
}
