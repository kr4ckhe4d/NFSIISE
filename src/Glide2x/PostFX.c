// SPDX-License-Identifier: MIT

/*
 * Post-processing pipeline for the desktop OpenGL 2 renderer.
 *
 * The game renders into an off-screen framebuffer with two color attachments:
 *   0 - color,
 *   1 - "world mask": 1 for 3D world geometry, 0 for 2D overlays (HUD, menus, text),
 * plus a depth texture. Glide depth is 1/w, so view-space depth is w = 1 / (1 - depth).
 *
 * Per frame:
 *   [MSAA resolve] -> SSAO (half res) + bilateral blur -> bloom (mip chain)
 *   -> composite (AO, bloom, exposure, grading, filmic curve) -> FXAA
 *   -> final (downsample from supersampled resolution, adaptive sharpening, vignette, grain, gamma).
 * Overlays are kept untouched by blending with the mask.
 *
 * This file is included from OpenGL2.c.
 */

#ifndef GL_RGBA16F
# define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT
# define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_READ_FRAMEBUFFER
# define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
# define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_MAX_SAMPLES
# define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_COLOR_ATTACHMENT1
# define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_DEPTH_TEXTURE_MODE
# define GL_DEPTH_TEXTURE_MODE 0x884B
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
# define GL_TEXTURE_COMPARE_MODE 0x884C
#endif

#define BloomLevels 6

typedef struct
{
	GLuint tex, fbo;
	int32_t w, h;
} PostTarget;

static struct
{
	BOOL ok;              /* Pipeline is usable (shaders compiled, required GL features present) */
	BOOL active;          /* Post-processing enabled (toggled with Alt+P) */
	BOOL hasFloat;
	BOOL hasRed;
	BOOL hasMsaa;
	int32_t debugView;
	uint32_t frame;

	/* Programs */
	GLuint aoPrepProg, aoProg, blurProg, prefilterProg, downProg, upProg, compositeProg, fxaaProg, finalProg;
	GLuint skyProg, ssrProg, reflBlurProg, shaftsProg, envUpdateProg, envDownProg;

	/* Scene */
	int32_t w, h, samples;
	GLuint sceneFbo, sceneColor, sceneMask, sceneDepth;
	GLuint msFbo, msColor, msMask, msDepth;

	/* Effects */
	PostTarget aoLin, aoA, aoB;
	PostTarget sky, reflA, reflB, shafts;
	PostTarget env[2], env1, env2; /* Dynamic environment of the camera (equirect, world directions), blurred levels */
	int32_t envCur, envTrack;
	PostTarget bloom[BloomLevels];
	int32_t bloomLevels;
	PostTarget postA, postB;
} g_post;

/* Shaders */

#define POST_GLSL_HEADER \
	"#version 120\n"

static const char g_postVertexSrc[] =
	POST_GLSL_HEADER
	"attribute vec2 aPosition;\n"
	"varying vec2 vUV;\n"
	"void main()\n"
	"{\n"
	"	vUV = aPosition * 0.5 + 0.5;\n"
	"	gl_Position = vec4(aPosition, 0.0, 1.0);\n"
	"}\n"
;

#define POST_COMMON \
	POST_GLSL_HEADER \
	"varying vec2 vUV;\n" \
	"float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n" \
	"vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }\n" \
	"vec3 toGamma(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }\n" \
	/* Invertible highlight expansion: LDR 1.0 maps to uWhite (HDR), its inverse is used as the tone curve */ \
	"vec3 expandHDR(vec3 c, float s) { return c / max(vec3(1.0) - c * s, vec3(1.0 / 64.0)); }\n" \
	"vec3 compressHDR(vec3 c, float s) { return c / (vec3(1.0) + c * s); }\n" \
	"float linearDepth(float d) { return 1.0 / max(1.0 - d, 1e-7); }\n" \
	"float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"

/*
 * View-space mapping. A sub-view (rear-view mirror) is a separate camera centered on its own rectangle,
 * with the vertical field of view of the main view. setView() selects the camera of a screen position.
 */
#define POST_VIEW \
	"uniform vec2 uProj;\n"     /* (V / 2 / f, 240 / f) - view-space XY per unit depth at the screen edge */ \
	"uniform vec4 uSub;\n"      /* Sub-view rectangle in UV (min, max) */ \
	"uniform vec2 uProjSub;\n" \
	"uniform float uSubOn;\n" \
	"vec4 gR;\n" \
	"vec2 gP;\n" \
	"bool gSub;\n" \
	"bool inSub(vec2 uv) { return uSubOn > 0.5 && all(greaterThanEqual(uv, uSub.xy)) && all(lessThan(uv, uSub.zw)); }\n" \
	"void setView(vec2 uv)\n" \
	"{\n" \
	"	gSub = inSub(uv);\n" \
	"	gR = gSub ? uSub : vec4(0.0, 0.0, 1.0, 1.0);\n" \
	"	gP = gSub ? uProjSub : uProj;\n" \
	"}\n" \
	"vec3 viewPos(vec2 uv, float w) { return vec3(((uv - gR.xy) / (gR.zw - gR.xy) * 2.0 - 1.0) * gP * w, w); }\n" \
	"vec2 project(vec3 p) { return gR.xy + (p.xy / (p.z * gP) * 0.5 + 0.5) * (gR.zw - gR.xy); }\n"

/* AO input: half resolution linear depth, negative for overlay pixels, huge for the sky */
static const char g_postAOPrepSrc[] =
	POST_COMMON
	"uniform sampler2D uDepth;\n"
	"uniform sampler2D uMask;\n"
	"void main()\n"
	"{\n"
	"	float d = texture2D(uDepth, vUV).r;\n"
	"	float w = d >= 0.999999 ? 1e7 : linearDepth(d);\n"
	"	gl_FragColor = vec4(texture2D(uMask, vUV).r >= 0.99 ? w : -w);\n"
	"}\n"
;

/* Screen-space ambient occlusion (normal-oriented hemisphere, spiral sampling) on the half resolution depth */
static const char g_postAOSrc[] =
	POST_COMMON
	POST_VIEW
	"uniform sampler2D uLin;\n"
	"uniform vec2 uTexel;\n"     /* Half resolution texel size */
	"uniform float uRadius;\n"
	"uniform float uIntensity;\n"
	"vec3 viewPosAt(vec2 uv)\n"
	"{\n"
	"	return viewPos(uv, abs(texture2D(uLin, uv).r));\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	float w0 = texture2D(uLin, vUV).r;\n"
	"	if (w0 <= 0.0 || w0 >= 1e6)\n"
	"	{\n"
	"		gl_FragColor = vec4(1.0);\n"
	"		return;\n"
	"	}\n"
	"	setView(vUV);\n"
	"	vec3 P = viewPos(vUV, w0);\n"
	"	vec3 pr = viewPosAt(vUV + vec2(uTexel.x, 0.0));\n"
	"	vec3 pl = viewPosAt(vUV - vec2(uTexel.x, 0.0));\n"
	"	vec3 pu = viewPosAt(vUV + vec2(0.0, uTexel.y));\n"
	"	vec3 pd = viewPosAt(vUV - vec2(0.0, uTexel.y));\n"
	"	vec3 dx = (abs(pr.z - P.z) < abs(P.z - pl.z)) ? (pr - P) : (P - pl);\n"
	"	vec3 dy = (abs(pu.z - P.z) < abs(P.z - pd.z)) ? (pu - P) : (P - pd);\n"
	"	vec3 N = normalize(cross(dx, dy));\n"
	"	if (dot(N, P) > 0.0)\n"
	"		N = -N;\n"
	/* Projected radius in UV units, limited to keep texture fetches cache friendly */
	"	vec2 radiusUV = uRadius / (gP * 2.0 * P.z) * (gR.zw - gR.xy);\n"
	"	float maxR = max(radiusUV.x, radiusUV.y);\n"
	"	if (maxR < uTexel.x)\n"
	"	{\n"
	"		gl_FragColor = vec4(1.0);\n"
	"		return;\n"
	"	}\n"
	"	radiusUV *= min(1.0, 0.1 / maxR);\n"
	"	float r2 = uRadius * uRadius;\n"
	"	float noise = ign(gl_FragCoord.xy) * 6.2831853;\n"
	"	float jitter = ign(gl_FragCoord.yx + 17.0);\n"
	"	float occlusion = 0.0;\n"
	"	const int samples = 12;\n"
	"	for (int i = 0; i < samples; ++i)\n"
	"	{\n"
	"		float t = (float(i) + jitter) / float(samples);\n"
	"		float a = t * 5.0 * 6.2831853 + noise;\n"
	"		vec2 suv = vUV + vec2(cos(a), sin(a)) * radiusUV * t;\n"
	"		float sw = texture2D(uLin, suv).r;\n"
	"		vec3 v = viewPos(suv, abs(sw)) - P;\n"
	"		float vv = dot(v, v);\n"
	"		float vn = dot(v, N) * inversesqrt(vv + 1e-6 * r2);\n"
	"		float falloff = max(1.0 - vv / r2, 0.0);\n"
	"		occlusion += max(vn - 0.15, 0.0) * falloff * falloff * step(0.0, sw);\n"
	"	}\n"
	"	float ao = clamp(1.0 - 2.0 * uIntensity * occlusion / float(samples), 0.0, 1.0);\n"
	"	gl_FragColor = vec4(vec3(ao), 1.0);\n"
	"}\n"
;

/* Depth-aware separable blur for AO */
static const char g_postBlurSrc[] =
	POST_COMMON
	"uniform sampler2D uAO;\n"
	"uniform sampler2D uDepth;\n"
	"uniform vec2 uStep;\n"
	"void main()\n"
	"{\n"
	"	float w0 = abs(texture2D(uDepth, vUV).r);\n"
	"	float sum = texture2D(uAO, vUV).r;\n"
	"	float wsum = 1.0;\n"
	"	for (int i = -4; i <= 4; ++i)\n"
	"	{\n"
	"		if (i == 0)\n"
	"			continue;\n"
	"		vec2 uv = vUV + uStep * float(i);\n"
	"		float w = abs(texture2D(uDepth, uv).r);\n"
	"		float weight = exp(-float(i * i) / 18.0) * max(0.0, 1.0 - abs(w - w0) / (w0 * 0.03));\n"
	"		sum += texture2D(uAO, uv).r * weight;\n"
	"		wsum += weight;\n"
	"	}\n"
	"	gl_FragColor = vec4(vec3(sum / wsum), 1.0);\n"
	"}\n"
;

/*
 * Sky probe (4x1): x=0 - average sky color (HDR), x=1 - sun position (uv, coverage, spread), x=2 - sun color,
 * x=3 - average color of the visible scenery (not sky).
 * The "sun" is the centroid of very bright sky pixels; a large spread means an evenly bright (overcast) sky.
 */
static const char g_postSkySrc[] =
	POST_COMMON
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uDepth;\n"
	"uniform sampler2D uMask;\n"
	"uniform float uWhite;\n"
	"void main()\n"
	"{\n"
	"	vec3 skySum = vec3(0.0), allSum = vec3(0.0), sunSum = vec3(0.0), worldSum = vec3(0.0);\n"
	"	vec2 sunUV = vec2(0.0), sunUV2 = vec2(0.0);\n"
	"	float skyN = 0.0, allN = 0.0, sunW = 0.0;\n"
	"	for (int y = 0; y < 24; ++y)\n"
	"	{\n"
	"		for (int x = 0; x < 48; ++x)\n"
	"		{\n"
	"			vec2 uv = (vec2(float(x), float(y)) + 0.5) / vec2(48.0, 24.0);\n"
	"			if (texture2D(uMask, uv).r < 0.5)\n"
	"				continue;\n"
	"			vec3 c = expandHDR(toLinear(texture2D(uColor, uv).rgb), uWhite);\n"
	"			allSum += c;\n"
	"			allN += 1.0;\n"
	"			if (texture2D(uDepth, uv).r >= 0.999999)\n"
	"			{\n"
	"				float b = max(luma(c) - 2.0, 0.0);\n"
	"				skySum += c;\n"
	"				skyN += 1.0;\n"
	"				sunSum += c * b;\n"
	"				sunUV += uv * b;\n"
	"				sunUV2 += uv * uv * b;\n"
	"				sunW += b;\n"
	"			}\n"
	"			else\n"
	"				worldSum += c;\n"
	"		}\n"
	"	}\n"
	"	float px = floor(gl_FragCoord.x);\n"
	"	if (px < 0.5)\n"
	"	{\n"
	"		gl_FragColor = vec4(skyN > 16.0 ? skySum / skyN : allSum / max(allN, 1.0), 1.0);\n"
	"	}\n"
	"	else if (px < 1.5)\n"
	"	{\n"
	"		vec2 m = sunUV / max(sunW, 1e-4);\n"
	"		vec2 v = max(sunUV2 / max(sunW, 1e-4) - m * m, vec2(0.0));\n"
	"		gl_FragColor = vec4(m, sunW / 1152.0, sqrt(v.x + v.y));\n"
	"	}\n"
	"	else if (px < 2.5)\n"
	"	{\n"
	"		gl_FragColor = vec4(sunSum / max(sunW, 1e-4), 1.0);\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		gl_FragColor = vec4(worldSum / max(allN - skyN, 1.0), 1.0);\n"
	"	}\n"
	"}\n"
;

/*
 * Dynamic environment (for car model reflections): an equirectangular map of world directions around the camera.
 * Directions visible on screen are updated with the scene (excluding overlays, the mirror and car models), others
 * keep what was seen before - so the scenery passed by stays in the reflections. Alpha = how much was seen.
 */
static const char g_postEnvUpdateSrc[] =
	POST_COMMON
	POST_VIEW
	"uniform sampler2D uPrev;\n"
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uLin;\n"
	"uniform sampler2D uMask;\n"
	"uniform mat4 uCam;\n"       /* Camera -> world */
	"uniform float uWhite;\n"
	"uniform float uRate;\n"
	"void main()\n"
	"{\n"
	"	vec4 prev = texture2D(uPrev, vUV);\n"
	"	float az = (vUV.x - 0.5) * 6.2831853, el = (vUV.y - 0.5) * 3.14159265;\n"
	"	vec3 w = vec3(sin(az) * cos(el), sin(el), cos(az) * cos(el));\n"
	"	vec3 c = (vec4(w, 0.0) * uCam).xyz;\n"   /* World -> camera (orthonormal: transpose) */
	"	gl_FragColor = prev;\n"
	"	if (c.z < 0.05)\n"
	"		return;\n"
	"	setView(vec2(0.5));\n"
	"	vec2 suv = project(c);\n"
	"	if (any(lessThan(suv, vec2(0.002))) || any(greaterThan(suv, vec2(0.998))) || inSub(suv))\n"
	"		return;\n"
	"	if (texture2D(uLin, suv).r <= 0.0 || texture2D(uMask, suv).g > 0.5)\n"
	"		return;\n"
	"	vec3 scene = expandHDR(toLinear(texture2D(uColor, suv).rgb), uWhite);\n"
	"	gl_FragColor = mix(prev, vec4(scene, 1.0), uRate);\n"
	"}\n"
;

/* Downsampling blur of the environment (4 bilinear taps, wraps horizontally) */
static const char g_postEnvDownSrc[] =
	POST_COMMON
	"uniform sampler2D uTex;\n"
	"uniform vec2 uTexel;\n"      /* Source texel size */
	"void main()\n"
	"{\n"
	"	vec2 o = uTexel * 1.5;\n"
	"	vec4 s = texture2D(uTex, vUV + vec2(-o.x, -o.y)) + texture2D(uTex, vUV + vec2(o.x, -o.y)) +\n"
	"	         texture2D(uTex, vUV + vec2(-o.x, o.y)) + texture2D(uTex, vUV + vec2(o.x, o.y)) + texture2D(uTex, vUV) * 2.0;\n"
	"	gl_FragColor = s / 6.0;\n"
	"}\n"
;

/*
 * Screen-space reflections on upward facing, asphalt-like (unsaturated) surfaces, half resolution.
 * Output: rgb - reflected radiance (HDR), a - specular weight (Fresnel * ground * strength).
 * Rays that leave the screen fall back to the sky in the reflected direction or the average sky color.
 */
static const char g_postSSRSrc[] =
	POST_COMMON
	"uniform sampler2D uLin;\n"
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uSky;\n"
	POST_VIEW
	"uniform vec2 uTexel;\n"
	"uniform float uWhite;\n"
	"uniform float uStrength;\n"
	"uniform float uWetness;\n"
	"uniform sampler2D uPano0;\n"
	"uniform sampler2D uPano1;\n"
	"uniform float uPanoOn;\n"
	"uniform mat4 uPanoCam;\n"
	"uniform vec2 uPanoParams;\n"
	"vec3 viewPosAt(vec2 uv) { return viewPos(uv, abs(texture2D(uLin, uv).r)); }\n"
	/* Inside the current view (a ray from the main view must not pick up the mirror) */
	"bool inView(vec2 uv) { return all(greaterThan(uv, gR.xy)) && all(lessThan(uv, gR.zw)) && (gSub || !inSub(uv)); }\n"
	"vec3 sceneHDR(vec2 uv) { return expandHDR(toLinear(texture2D(uColor, uv).rgb), uWhite); }\n"
	"void main()\n"
	"{\n"
	"	float w0 = texture2D(uLin, vUV).r;\n"
	"	if (w0 <= 0.0 || w0 >= 1e6)\n"
	"	{\n"
	"		gl_FragColor = vec4(0.0);\n"
	"		return;\n"
	"	}\n"
	"	setView(vUV);\n"
	"	vec3 P = viewPos(vUV, w0);\n"
	"	vec3 pr = viewPosAt(vUV + vec2(uTexel.x, 0.0));\n"
	"	vec3 pl = viewPosAt(vUV - vec2(uTexel.x, 0.0));\n"
	"	vec3 pu = viewPosAt(vUV + vec2(0.0, uTexel.y));\n"
	"	vec3 pd = viewPosAt(vUV - vec2(0.0, uTexel.y));\n"
	"	vec3 dx = (abs(pr.z - P.z) < abs(P.z - pl.z)) ? (pr - P) : (P - pl);\n"
	"	vec3 dy = (abs(pu.z - P.z) < abs(P.z - pd.z)) ? (pu - P) : (P - pd);\n"
	"	vec3 N = normalize(cross(dx, dy));\n"
	"	if (dot(N, P) > 0.0)\n"
	"		N = -N;\n"
	/* Ground: facing up, and grey like asphalt or concrete (grass, dirt and painted walls stay matte) */
	"	vec3 c0 = toLinear(texture2D(uColor, vUV).rgb);\n"
	"	float cMax = max(c0.r, max(c0.g, c0.b)), cMin = min(c0.r, min(c0.g, c0.b));\n"
	"	float sat = (cMax - cMin) / (cMax + 0.05);\n"
	"	float ground = smoothstep(0.8, 0.95, N.y) * (1.0 - smoothstep(0.15, 0.4, sat));\n"
	"	if (ground <= 0.002)\n"
	"	{\n"
	"		gl_FragColor = vec4(0.0);\n"
	"		return;\n"
	"	}\n"
	"	vec3 V = normalize(P);\n"
	"	vec3 R = reflect(V, N);\n"
	"	float NoV = clamp(dot(N, -V), 0.0, 1.0);\n"
	/*
	 * Schlick Fresnel with a roughness-limited grazing peak: rough dry asphalt only gets a faint sheen.
	 * The base reflectance of dry asphalt is already part of the game's textures, only a water film adds it.
	 */
	"	float rough = mix(0.88, 0.05, uWetness);\n"
	"	float F0 = 0.04 * smoothstep(0.0, 0.5, uWetness);\n"
	"	float F = F0 + (max(1.0 - rough, F0) - F0) * pow(1.0 - NoV, 5.0);\n"
	"	float spec = clamp(F * ground * uStrength, 0.0, 0.95);\n"
	/* Environment: the sky in the reflected direction, searched upwards past objects on the horizon */
	"	vec3 skyAvg = texture2D(uSky, vec2(0.5 / 4.0, 0.5)).rgb;\n"
	"	vec3 env = skyAvg;\n"
	/* Sky panorama of the track (Sky.c) in the reflected direction: sharp when wet, blurred when dry */
	"	if (uPanoOn > 0.5 && !gSub)\n"
	"	{\n"
	"		vec3 w = normalize((uPanoCam * vec4(R, 0.0)).xyz);\n"
	"		float el = degrees(asin(clamp(w.y, -1.0, 1.0)));\n"
	"		vec2 puv = vec2(fract((atan(w.x, w.z) + uPanoParams.x) / 6.2831853 + 0.5), clamp((90.0 - el) / (90.0 - uPanoParams.y), 0.0, 0.999));\n"
	"		vec3 p0 = expandHDR(toLinear(texture2D(uPano0, puv).rgb), uWhite);\n"
	"		vec3 p1 = expandHDR(toLinear(texture2D(uPano1, puv).rgb), uWhite);\n"
	"		env = mix(skyAvg, mix(p1, p0, smoothstep(0.2, 0.7, uWetness)), smoothstep(-2.0, 1.0, el));\n"
	"	}\n"
	"	else\n"
	"	if (R.z > 0.01)\n"
	"	{\n"
	"		vec2 uvInf = project(R);\n"
	"		vec3 sum = vec3(0.0);\n"
	"		float n = 0.0;\n"
	"		for (int k = 0; k < 6; ++k)\n"
	"		{\n"
	"			vec2 uv = vec2(uvInf.x, uvInf.y + float(k) * 0.03);\n"
	"			if (inView(uv) && texture2D(uLin, uv).r >= 1e6)\n"
	"			{\n"
	"				sum += sceneHDR(uv);\n"
	"				n += 1.0;\n"
	"			}\n"
	"		}\n"
	"		if (n > 0.0)\n"
	"			env = mix(skyAvg, sum / n, min(n / 3.0, 1.0));\n"
	"	}\n"
	"	vec3 result = env;\n"
	"	if (R.z > 0.02)\n"
	"	{\n"
	/* March in screen space between the projected ray ends, depth interpolated as 1/z */
	"		vec3 P1 = P + R * (P.z * 40.0 + 100.0);\n"
	"		vec2 uv0 = vUV, uv1 = project(P1);\n"
	"		vec2 d = uv1 - uv0;\n"
	"		float sMax = 1.0;\n"
	"		if (d.x > 0.0) sMax = min(sMax, (gR.z - uv0.x) / d.x); else if (d.x < 0.0) sMax = min(sMax, (gR.x - uv0.x) / d.x);\n"
	"		if (d.y > 0.0) sMax = min(sMax, (gR.w - uv0.y) / d.y); else if (d.y < 0.0) sMax = min(sMax, (gR.y - uv0.y) / d.y);\n"
	"		float iz0 = 1.0 / P.z, iz1 = 1.0 / P1.z;\n"
	"		const int steps = 40;\n"
	/* Ordered 4x4 jitter: averages out in the reflection blur (noise would leave per-row stripes) */
	"		vec2 q = mod(floor(gl_FragCoord.xy), 4.0);\n"
	"		float jitter = (mod(q.x + 2.0 * q.y, 4.0) * 4.0 + mod(q.y + 2.0 * floor(q.x / 2.0), 4.0) + 0.5) / 16.0;\n"
	/* Start a few texels away from the surface: point sampled depth would make the ray hit its own surface */
	"		float sMin = min(3.0 * length(uTexel) / max(length(d), 1e-6), sMax);\n"
	"		float sPrev = sMin, sHit = -1.0;\n"
	"		for (int i = 1; i <= steps; ++i)\n"
	"		{\n"
	"			float t = (float(i) - jitter) / float(steps);\n"
	/* Quadratic distribution: more samples close to the reflecting point */
	"			float s = sMin + (sMax - sMin) * t * t;\n"
	"			vec2 uv = uv0 + d * s;\n"
	"			float rayZ = 1.0 / mix(iz0, iz1, s);\n"
	"			if (!inView(uv))\n"
	"				break;\n"
	"			float sz = texture2D(uLin, uv).r;\n"
	"			float prevZ = 1.0 / mix(iz0, iz1, sPrev);\n"
	"			if (sz > 0.0 && rayZ > sz && rayZ - sz < abs(rayZ - prevZ) + 0.02 * rayZ + 0.25)\n"
	"			{\n"
	"				sHit = s;\n"
	"				break;\n"
	"			}\n"
	"			sPrev = s;\n"
	"		}\n"
	"		if (sHit > 0.0)\n"
	"		{\n"
	"			float a = sPrev, b = sHit;\n"
	"			for (int i = 0; i < 5; ++i)\n"
	"			{\n"
	"				float m = 0.5 * (a + b);\n"
	"				float sz = abs(texture2D(uLin, uv0 + d * m).r);\n"
	"				if (1.0 / mix(iz0, iz1, m) > sz) b = m; else a = m;\n"
	"			}\n"
	"			vec2 uvHit = uv0 + d * b;\n"
	"			vec2 lHit = (uvHit - gR.xy) / (gR.zw - gR.xy);\n"
	"			vec2 e = smoothstep(vec2(0.0), vec2(0.06), lHit) * smoothstep(vec2(1.0), vec2(0.94), lHit);\n"
	"			result = mix(env, sceneHDR(uvHit), e.x * e.y);\n"
	"		}\n"
	"	}\n"
	/* Dry roads only show a soft sheen of the sky, wet roads a clear mirror image */
	"	result = mix(mix(env, result, 0.25), result, smoothstep(0.2, 0.7, uWetness));\n"
	/* A rough surface spreads out highlights: soft limit of the reflected radiance when dry */
	"	float limit = mix(0.8, 8.0, uWetness * uWetness);\n"
	"	result = result / (1.0 + result / limit);\n"
	"	gl_FragColor = vec4(result, spec);\n"
	"}\n"
;

/* Reflection blur (glossiness): depth-aware, weighted by the specular weight, stretched vertically like on wet roads */
static const char g_postReflBlurSrc[] =
	POST_COMMON
	"uniform sampler2D uTex;\n"
	"uniform sampler2D uLin;\n"
	"uniform vec2 uStep;\n"
	"void main()\n"
	"{\n"
	"	float w0 = abs(texture2D(uLin, vUV).r);\n"
	"	vec4 c0 = texture2D(uTex, vUV);\n"
	"	vec3 sum = c0.rgb * c0.a;\n"
	"	float wsum = 1.0, csum = c0.a;\n"
	"	for (int i = -5; i <= 5; ++i)\n"
	"	{\n"
	"		if (i == 0)\n"
	"			continue;\n"
	"		vec2 uv = vUV + uStep * float(i);\n"
	"		vec4 c = texture2D(uTex, uv);\n"
	"		float w = abs(texture2D(uLin, uv).r);\n"
	"		float weight = exp(-float(i * i) / 12.0) * max(0.0, 1.0 - abs(w - w0) / (w0 * 0.3));\n"
	"		sum += c.rgb * c.a * weight;\n"
	"		csum += c.a * weight;\n"
	"		wsum += weight;\n"
	"	}\n"
	"	gl_FragColor = vec4(sum / max(csum, 1e-4), c0.a > 0.0 ? csum / wsum : 0.0);\n"
	"}\n"
;

/* Sun shafts: radial blur of the bright sky towards the sun position from the sky probe, half resolution */
static const char g_postShaftsSrc[] =
	POST_COMMON
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uDepth;\n"
	"uniform sampler2D uMask;\n"
	"uniform sampler2D uSky;\n"
	"uniform float uWhite;\n"
	"uniform vec2 uAspect;\n"
	"void main()\n"
	"{\n"
	"	vec4 sun = texture2D(uSky, vec2(1.5 / 4.0, 0.5));\n"
	"	float strength = smoothstep(0.0005, 0.01, sun.z) * (1.0 - smoothstep(0.05, 0.1, sun.w));\n"
	"	if (strength <= 0.0)\n"
	"	{\n"
	"		gl_FragColor = vec4(0.0);\n"
	"		return;\n"
	"	}\n"
	"	const int samples = 48;\n"
	"	vec2 delta = (sun.xy - vUV) * (0.95 / float(samples));\n"
	"	vec2 uv = vUV + delta * ign(gl_FragCoord.xy);\n"
	"	float illum = 0.0, decay = 1.0;\n"
	"	for (int i = 0; i < samples; ++i)\n"
	"	{\n"
	"		if (texture2D(uDepth, uv).r >= 0.999999 && texture2D(uMask, uv).r > 0.5)\n"
	"			illum += max(luma(expandHDR(toLinear(texture2D(uColor, uv).rgb), uWhite)) - 1.5, 0.0) * decay;\n"
	"		decay *= 0.97;\n"
	"		uv += delta;\n"
	"	}\n"
	"	vec3 sunColor = texture2D(uSky, vec2(2.5 / 4.0, 0.5)).rgb;\n"
	"	sunColor /= max(luma(sunColor), 1e-3);\n"
	"	float dist = length((sun.xy - vUV) * uAspect);\n"
	"	gl_FragColor = vec4(sunColor * illum / float(samples) * strength * (1.0 - 0.6 * smoothstep(0.0, 1.5, dist)), 1.0);\n"
	"}\n"
;

/* Bloom: bright-pass with soft knee and Karis average, from scene (full res) to first bloom level */
static const char g_postPrefilterSrc[] =
	POST_COMMON
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uMask;\n"
	"uniform vec2 uTexel;\n"
	"uniform float uWhite;\n"
	"uniform float uThreshold;\n"
	"vec3 fetch(vec2 uv)\n"
	"{\n"
	"	vec3 c = expandHDR(toLinear(texture2D(uColor, uv).rgb), uWhite) * texture2D(uMask, uv).r;\n"
	"	float br = max(c.r, max(c.g, c.b));\n"
	"	float knee = uThreshold * 0.5;\n"
	"	float soft = clamp(br - uThreshold + knee, 0.0, 2.0 * knee);\n"
	"	soft = soft * soft / (4.0 * knee + 1e-5);\n"
	"	return c * max(soft, br - uThreshold) / max(br, 1e-5);\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	vec3 a = fetch(vUV + uTexel * vec2(-1.0, -1.0));\n"
	"	vec3 b = fetch(vUV + uTexel * vec2( 1.0, -1.0));\n"
	"	vec3 c = fetch(vUV + uTexel * vec2(-1.0,  1.0));\n"
	"	vec3 d = fetch(vUV + uTexel * vec2( 1.0,  1.0));\n"
	"	float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b)), wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));\n"
	"	gl_FragColor = vec4((a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd), 1.0);\n"
	"}\n"
;

/* Bloom: 13-tap downsample */
static const char g_postDownSrc[] =
	POST_COMMON
	"uniform sampler2D uTex;\n"
	"uniform vec2 uTexel;\n"
	"vec3 s(vec2 o) { return texture2D(uTex, vUV + o * uTexel).rgb; }\n"
	"void main()\n"
	"{\n"
	"	vec3 A = s(vec2(-2.0, -2.0)), B = s(vec2(0.0, -2.0)), C = s(vec2(2.0, -2.0));\n"
	"	vec3 D = s(vec2(-1.0, -1.0)), E = s(vec2(1.0, -1.0));\n"
	"	vec3 F = s(vec2(-2.0, 0.0)), G = s(vec2(0.0, 0.0)), H = s(vec2(2.0, 0.0));\n"
	"	vec3 I = s(vec2(-1.0, 1.0)), J = s(vec2(1.0, 1.0));\n"
	"	vec3 K = s(vec2(-2.0, 2.0)), L = s(vec2(0.0, 2.0)), M = s(vec2(2.0, 2.0));\n"
	"	vec3 c = (D + E + I + J) * 0.125;\n"
	"	c += (A + B + G + F) * 0.03125;\n"
	"	c += (B + C + H + G) * 0.03125;\n"
	"	c += (F + G + L + K) * 0.03125;\n"
	"	c += (G + H + M + L) * 0.03125;\n"
	"	gl_FragColor = vec4(c, 1.0);\n"
	"}\n"
;

/* Bloom: 9-tap tent upsample, additively blended into the larger level */
static const char g_postUpSrc[] =
	POST_COMMON
	"uniform sampler2D uTex;\n"
	"uniform vec2 uTexel;\n"
	"uniform float uScale;\n"
	"vec3 s(vec2 o) { return texture2D(uTex, vUV + o * uTexel).rgb; }\n"
	"void main()\n"
	"{\n"
	"	vec3 c = s(vec2(0.0)) * 4.0;\n"
	"	c += (s(vec2(-1.0, 0.0)) + s(vec2(1.0, 0.0)) + s(vec2(0.0, -1.0)) + s(vec2(0.0, 1.0))) * 2.0;\n"
	"	c += s(vec2(-1.0, -1.0)) + s(vec2(1.0, -1.0)) + s(vec2(-1.0, 1.0)) + s(vec2(1.0, 1.0));\n"
	"	gl_FragColor = vec4(c * (uScale / 16.0), 1.0);\n"
	"}\n"
;

/* Composite: AO, bloom, exposure, grading and filmic curve; output alpha = world mask */
static const char g_postCompositeSrc[] =
	POST_COMMON
	POST_VIEW
	"uniform sampler2D uPano;\n"
	"uniform float uPanoOn;\n"
	"uniform mat4 uPanoCam;\n"
	"uniform vec2 uPanoParams;\n"
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uMask;\n"
	"uniform sampler2D uDepth;\n"
	"uniform sampler2D uAO;\n"
	"uniform sampler2D uBloom;\n"
	"uniform sampler2D uAOLin;\n"
	"uniform sampler2D uRefl;\n"
	"uniform sampler2D uShafts;\n"
	"uniform sampler2D uSky;\n"
	"uniform float uReflections;\n"
	"uniform float uShaftStrength;\n"
	"uniform float uHaze;\n"
	"uniform float uShadow;\n" /* Screen-space sun shadows: strength, direction towards the sun (camera space) */
	"uniform vec3 uSunCam;\n"
	"uniform float uNightAmbient;\n" /* Night: scenery darkened to this, bright lights stay */
	"uniform float uHlStrength;\n" /* Headlights: strength, lamp positions and beam basis (camera space) */
	"uniform vec3 uHlPos[2];\n"
	"uniform mat3 uHlBasis;\n"
	"uniform vec2 uAOTexel;\n"
	"uniform vec2 uAOSize;\n"
	"uniform float uAOStrength;\n"
	"uniform float uBloomStrength;\n"
	"uniform float uWhite;\n"
	"uniform float uExposure;\n"
	"uniform float uContrast;\n"
	"uniform float uSaturation;\n"
	"uniform float uWarmth;\n"
	"uniform float uToneMapping;\n"
	"uniform int uDebug;\n"
	/* Joint bilateral upsample of the half resolution AO */
	"float sampleAO(float w0)\n"
	"{\n"
	"	vec2 pos = vUV * uAOSize - 0.5;\n"
	"	vec2 base = floor(pos);\n"
	"	vec2 f = pos - base;\n"
	"	float sum = 0.0, wsum = 0.0;\n"
	"	for (int y = 0; y < 2; ++y)\n"
	"	{\n"
	"		for (int x = 0; x < 2; ++x)\n"
	"		{\n"
	"			vec2 uv = (base + vec2(float(x), float(y)) + 0.5) * uAOTexel;\n"
	"			float bw = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);\n"
	"			float w = abs(texture2D(uAOLin, uv).r);\n"
	"			float dw = 1.0 / (1e-3 + abs(w - w0) / w0);\n"
	"			sum += texture2D(uAO, uv).r * bw * dw;\n"
	"			wsum += bw * dw;\n"
	"		}\n"
	"	}\n"
	"	return wsum > 0.0 ? sum / wsum : 1.0;\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	vec3 src = texture2D(uColor, vUV).rgb;\n"
	"	float mask = texture2D(uMask, vUV).r;\n"
	"	float depth = texture2D(uDepth, vUV).r;\n"
	"	float ao = 1.0;\n"
	"	if (uAOStrength > 0.0 && depth < 0.999999)\n"
	"		ao = sampleAO(linearDepth(depth));\n"
	"	vec3 bloom = uBloomStrength > 0.0 ? texture2D(uBloom, vUV).rgb * uBloomStrength : vec3(0.0);\n"
	"	if (uDebug == 1) { gl_FragColor = vec4(vec3(mask), 1.0); return; }\n"
	"	if (uDebug == 2) { gl_FragColor = vec4(vec3(ao), 1.0); return; }\n"
	"	if (uDebug == 3) { gl_FragColor = vec4(vec3(depth >= 0.999999 ? 1.0 : clamp(log2(linearDepth(depth)) / 16.0, 0.0, 1.0)), 1.0); return; }\n"
	"	if (uDebug == 4) { gl_FragColor = vec4(toGamma(compressHDR(bloom, 0.5)), 1.0); return; }\n"
	"	vec4 refl = uReflections > 0.0 ? texture2D(uRefl, vUV) : vec4(0.0);\n"
	"	vec3 shafts = uShaftStrength > 0.0 ? texture2D(uShafts, vUV).rgb * uShaftStrength : vec3(0.0);\n"
	"	if (uDebug == 5) { gl_FragColor = vec4(toGamma(compressHDR(refl.rgb * refl.a, 0.5)), 1.0); return; }\n"
	"	if (uDebug == 6) { gl_FragColor = vec4(toGamma(compressHDR(shafts, 0.5)), 1.0); return; }\n"
	"	vec3 hdr = expandHDR(toLinear(src), uWhite);\n"
	"	hdr *= mix(1.0, ao, mask);\n"
	/*
	 * Headlights (low beam): two spots with a sharp upper cut-off and a hot spot, inverse square fall-off, lighting
	 * the surface's own colour (the game's night lighting darkened it: the light brings the texture back)
	 */
	"	setView(vUV);\n"
	/*
	 * Sun shadows: march from the pixel towards the sun through the depth buffer; a sample behind nearby geometry
	 * (within a thickness) means an occluder on screen (cars, walls, trees). Faded out with distance.
	 */
	"	if (uShadow > 0.0 && depth < 0.999999 && mask > 0.5 && !gSub)\n"
	"	{\n"
	"		float w0 = linearDepth(depth);\n"
	"		vec3 P = viewPos(vUV, w0);\n"
	"		vec3 Ns = normalize(cross(dFdx(P), dFdy(P)));\n"
	"		if (dot(Ns, P) > 0.0) Ns = -Ns;\n"
	"		float occl = 0.0;\n"
	"		if (dot(Ns, uSunCam) > 0.05)\n"
	"		{\n"
	"			vec3 start = P + Ns * (0.05 + 0.004 * w0);\n"
	"			float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
	"			for (int i = 1; i <= 24; ++i)\n"
	"			{\n"
	"				float t = 0.05 + 8.0 * pow((float(i) - jitter) / 24.0, 1.6);\n"
	"				vec3 Q = start + uSunCam * t;\n"
	"				if (Q.z < 0.3) break;\n"
	"				vec2 uv = gR.xy + (Q.xy / (Q.z * gP) * 0.5 + 0.5) * (gR.zw - gR.xy);\n"
	"				if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || inSub(uv)) break;\n"
	"				float d = texture2D(uDepth, uv).r;\n"
	"				if (d >= 0.999999 || texture2D(uMask, uv).r < 0.5) continue;\n"
	"				float ws = linearDepth(d);\n"
	"				if (ws < 1.5) continue;\n"
	"				float behind = Q.z - ws;\n"
	"				if (behind > 0.12 + 0.01 * Q.z + 0.15 * t && behind < 1.5 + 0.03 * Q.z) { occl = 1.0; break; }\n"
	"			}\n"
	"		}\n"
	"		else occl = 0.4;\n" /* Facing away from the sun: the game's own shading mostly covers it, a little more */
	"		occl *= 1.0 - smoothstep(60.0, 120.0, w0);\n"
	"		if (uDebug == 8) { gl_FragColor = vec4(vec3(1.0 - occl), 1.0); return; }\n"
	"		hdr *= 1.0 - uShadow * 0.7 * occl;\n"
	"	}\n"
	"	vec3 albedo = hdr, headlight = vec3(0.0);\n"
	"	if (uHlStrength > 0.0 && depth < 0.999999 && !gSub)\n"
	"	{\n"
	"		vec3 P = viewPos(vUV, linearDepth(depth));\n"
	"		vec3 N = normalize(cross(dFdx(P), dFdy(P)));\n"
	"		if (dot(N, P) > 0.0) N = -N;\n"
	"		float light = 0.0;\n"
	"		for (int i = 0; i < 2; ++i)\n"
	"		{\n"
	"			vec3 L = uHlPos[i] - P;\n"
	"			float d2 = dot(L, L);\n"
	"			vec3 l = L * inversesqrt(d2);\n"
	"			vec3 v = -l * uHlBasis;\n" /* Direction from the lamp in beam space (right, up, forward) */
	"			if (v.z <= 0.05) continue;\n"
	"			float h = v.x / v.z, e = v.y / v.z;\n"
	"			float vert = e < 0.0 ? exp(-e * e / 0.02) : exp(-e * e / 0.0004);\n"
	"			float beam = exp(-h * h / 0.25) * vert + 2.0 * exp(-h * h / 0.012 - (e + 0.03) * (e + 0.03) / 0.002);\n"
	"			light += beam * 150.0 / (d2 + 9.0) * max(dot(N, l), 0.0);\n"
	"		}\n"
	"		if (uDebug == 7) { gl_FragColor = vec4(vec3(compressHDR(vec3(light * uHlStrength), 1.0)), 1.0); return; }\n"
	"		headlight = albedo * uHlStrength * min(light, 4.0) * vec3(1.0, 0.95, 0.85) * 1.5 * mask;\n"
	"	}\n"
	/* Aerial perspective: distant geometry fades towards the sky color */
	"	if (uHaze > 0.0 && depth < 0.999999)\n"
	"	{\n"
	"		vec3 skyColor = texture2D(uSky, vec2(0.5 / 4.0, 0.5)).rgb;\n"
	/* With a sky panorama: the sky near the horizon in this direction (the sky behind the geometry) */
	"		setView(vUV);\n"
	"		if (uPanoOn > 0.5 && !gSub)\n"
	"		{\n"
	"			vec3 d = normalize((uPanoCam * vec4(viewPos(vUV, 1.0), 0.0)).xyz);\n"
	"			float el = clamp(degrees(asin(clamp(d.y, -1.0, 1.0))), 0.5, 6.0);\n"
	"			vec2 puv = vec2(fract((atan(d.x, d.z) + uPanoParams.x) / 6.2831853 + 0.5), (90.0 - el) / (90.0 - uPanoParams.y));\n"
	"			skyColor = mix(skyColor, expandHDR(toLinear(texture2D(uPano, puv).rgb), uWhite), 0.5);\n"
	"		}\n"
	"		hdr = mix(hdr, skyColor, uHaze * (1.0 - exp(-linearDepth(depth) * 0.003)) * mask);\n"
	"	}\n"
	"	hdr = mix(hdr, refl.rgb, refl.a * mask);\n"
	/* Night: the scenery (with its haze and reflections) darkened, bright lights kept; then the headlights */
	"	if (uNightAmbient < 1.0 && depth < 0.999999)\n"
	"		hdr *= mix(1.0, mix(uNightAmbient, 1.0, smoothstep(0.65, 0.9, luma(src))), mask);\n"
	"	hdr += headlight;\n"
	"	hdr += (bloom + shafts) * mask;\n"
	"	if (uToneMapping > 0.5)\n"
	"	{\n"
	"		hdr *= uExposure;\n"
	"		hdr *= vec3(1.0 + 0.08 * uWarmth, 1.0 + 0.01 * uWarmth, 1.0 - 0.08 * uWarmth);\n"
	"		float l = luma(hdr);\n"
	"		hdr = max(mix(vec3(l), hdr, uSaturation), vec3(0.0));\n"
	/* Contrast around middle grey in log space */
	"		hdr = 0.18 * pow(hdr / 0.18 + 1e-6, vec3(uContrast));\n"
	"	}\n"
	"	vec3 result = toGamma(compressHDR(hdr, uWhite));\n"
	"	gl_FragColor = vec4(mix(src, result, mask), mask);\n"
	"}\n"
;

/* FXAA (based on the FXAA 3.11 quality algorithm by Timothy Lottes) - only on world pixels */
static const char g_postFxaaSrc[] =
	POST_COMMON
	"uniform sampler2D uTex;\n"
	"uniform vec2 uTexel;\n"
	"float L(vec2 uv) { return dot(texture2D(uTex, uv).rgb, vec3(0.299, 0.587, 0.114)); }\n"
	"void main()\n"
	"{\n"
	"	vec4 cM = texture2D(uTex, vUV);\n"
	"	if (cM.a < 0.5)\n"
	"	{\n"
	"		gl_FragColor = cM;\n"
	"		return;\n"
	"	}\n"
	"	float lM = dot(cM.rgb, vec3(0.299, 0.587, 0.114));\n"
	"	float lN = L(vUV + vec2(0.0, uTexel.y));\n"
	"	float lS = L(vUV - vec2(0.0, uTexel.y));\n"
	"	float lE = L(vUV + vec2(uTexel.x, 0.0));\n"
	"	float lW = L(vUV - vec2(uTexel.x, 0.0));\n"
	"	float lMax = max(lM, max(max(lN, lS), max(lE, lW)));\n"
	"	float lMin = min(lM, min(min(lN, lS), min(lE, lW)));\n"
	"	float range = lMax - lMin;\n"
	"	if (range < max(0.0312, lMax * 0.125))\n"
	"	{\n"
	"		gl_FragColor = cM;\n"
	"		return;\n"
	"	}\n"
	"	float lNE = L(vUV + uTexel);\n"
	"	float lSW = L(vUV - uTexel);\n"
	"	float lNW = L(vUV + vec2(-uTexel.x, uTexel.y));\n"
	"	float lSE = L(vUV + vec2(uTexel.x, -uTexel.y));\n"
	"	float subpix = (2.0 * (lN + lS + lE + lW) + (lNE + lNW + lSE + lSW)) / 12.0;\n"
	"	subpix = clamp(abs(subpix - lM) / range, 0.0, 1.0);\n"
	"	subpix = smoothstep(0.0, 1.0, subpix);\n"
	"	subpix = subpix * subpix * 0.75;\n"
	"	float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);\n"
	"	float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);\n"
	"	bool horz = edgeH >= edgeV;\n"
	"	float stepLen = horz ? uTexel.y : uTexel.x;\n"
	"	float l1 = horz ? lS : lW;\n"
	"	float l2 = horz ? lN : lE;\n"
	"	float g1 = abs(l1 - lM);\n"
	"	float g2 = abs(l2 - lM);\n"
	"	float gradient = 0.25 * max(g1, g2);\n"
	"	float lAvg;\n"
	"	if (g1 >= g2)\n"
	"	{\n"
	"		stepLen = -stepLen;\n"
	"		lAvg = 0.5 * (l1 + lM);\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		lAvg = 0.5 * (l2 + lM);\n"
	"	}\n"
	"	vec2 uv = vUV;\n"
	"	if (horz) uv.y += stepLen * 0.5; else uv.x += stepLen * 0.5;\n"
	"	vec2 off = horz ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);\n"
	"	vec2 uv1 = uv - off, uv2 = uv + off;\n"
	"	float e1 = L(uv1) - lAvg, e2 = L(uv2) - lAvg;\n"
	"	bool r1 = abs(e1) >= gradient, r2 = abs(e2) >= gradient;\n"
	"	for (int i = 0; i < 10; ++i)\n"
	"	{\n"
	"		if (r1 && r2)\n"
	"			break;\n"
	"		float q = i < 4 ? 1.0 : (i < 5 ? 1.5 : (i < 8 ? 2.0 : (i < 9 ? 4.0 : 8.0)));\n"
	"		if (!r1) { uv1 -= off * q; e1 = L(uv1) - lAvg; r1 = abs(e1) >= gradient; }\n"
	"		if (!r2) { uv2 += off * q; e2 = L(uv2) - lAvg; r2 = abs(e2) >= gradient; }\n"
	"	}\n"
	"	float d1 = horz ? (vUV.x - uv1.x) : (vUV.y - uv1.y);\n"
	"	float d2 = horz ? (uv2.x - vUV.x) : (uv2.y - vUV.y);\n"
	"	bool dir1 = d1 < d2;\n"
	"	float pixOff = -min(d1, d2) / (d1 + d2) + 0.5;\n"
	"	bool correct = ((dir1 ? e1 : e2) < 0.0) != (lM < lAvg);\n"
	"	float finalOff = max(correct ? pixOff : 0.0, subpix);\n"
	"	vec2 fuv = vUV;\n"
	"	if (horz) fuv.y += finalOff * stepLen; else fuv.x += finalOff * stepLen;\n"
	"	gl_FragColor = vec4(texture2D(uTex, fuv).rgb, cM.a);\n"
	"}\n"
;

/* Final: supersampling resolve (box filter over the output pixel footprint), adaptive sharpening, vignette, grain, gamma */
static const char g_postFinalSrc[] =
	POST_COMMON
	"uniform sampler2D uTex;\n"
	"uniform vec2 uOutTexel;\n"  /* Output pixel size in source UV */
	"uniform int uTaps;\n"
	"uniform float uUseMask;\n"
	"uniform float uSharpen;\n"
	"uniform float uVignette;\n"
	"uniform float uGrain;\n"
	"uniform float uGamma;\n"
	"uniform float uTime;\n"
	"uniform float uAspect;\n"
	"vec4 px(vec2 uv)\n"
	"{\n"
	"	if (uTaps <= 1)\n"
	"		return texture2D(uTex, uv);\n"
	"	vec4 sum = vec4(0.0);\n"
	"	for (int y = 0; y < 4; ++y)\n"
	"	{\n"
	"		if (y >= uTaps) break;\n"
	"		for (int x = 0; x < 4; ++x)\n"
	"		{\n"
	"			if (x >= uTaps) break;\n"
	"			sum += texture2D(uTex, uv + ((vec2(float(x), float(y)) + 0.5) / float(uTaps) - 0.5) * uOutTexel);\n"
	"		}\n"
	"	}\n"
	"	return sum / float(uTaps * uTaps);\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	vec4 c4 = px(vUV);\n"
	"	vec3 c = c4.rgb;\n"
	"	float mask = uUseMask > 0.5 ? c4.a : 0.0;\n"
	"	if (uSharpen > 0.0 && mask > 0.0)\n"
	"	{\n"
	/* Contrast adaptive sharpening (after AMD FidelityFX CAS) */
	"		vec3 n = px(vUV + vec2(0.0, uOutTexel.y)).rgb;\n"
	"		vec3 s = px(vUV - vec2(0.0, uOutTexel.y)).rgb;\n"
	"		vec3 e = px(vUV + vec2(uOutTexel.x, 0.0)).rgb;\n"
	"		vec3 w = px(vUV - vec2(uOutTexel.x, 0.0)).rgb;\n"
	"		vec3 mn = min(c, min(min(n, s), min(e, w)));\n"
	"		vec3 mx = max(c, max(max(n, s), max(e, w)));\n"
	"		vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));\n"
	"		vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, uSharpen));\n"
	"		vec3 sharp = clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);\n"
	"		c = mix(c, sharp, mask);\n"
	"	}\n"
	"	if (uVignette > 0.0)\n"
	"	{\n"
	"		vec2 d = (vUV - 0.5) * vec2(uAspect, 1.0);\n"
	"		c *= mix(1.0, 1.0 - uVignette * smoothstep(0.25, 1.1, dot(d, d) * 2.2), mask);\n"
	"	}\n"
	"	if (uGrain > 0.0)\n"
	"	{\n"
	"		float g = ign(gl_FragCoord.xy + vec2(uTime * 5.588238, uTime * 2.218))\n"
	"			+ ign(gl_FragCoord.yx * 1.37 + uTime) - 1.0;\n"
	"		c += g * uGrain * 0.06 * mask * (1.0 - luma(c) * 0.6);\n"
	"	}\n"
	"	c = pow(max(c, vec3(0.0)), vec3(1.0 / uGamma));\n"
	"	gl_FragColor = vec4(c, 1.0);\n"
	"}\n"
;

/* Helpers */

static GLuint postCompile(const char *name, const char *fsrc)
{
	const GLchar *vsrc = g_postVertexSrc;
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	GLuint prog;
	glShaderSource(vs, 1, &vsrc, NULL);
	glShaderSource(fs, 1, &fsrc, NULL);
	glCompileShader(vs);
	glCompileShader(fs);
	if (!checkShaderCompilation(vs) || !checkShaderCompilation(fs))
	{
		fprintf(stderr, "PostFX: cannot compile \"%s\" shader\n", name);
		glDeleteShader(vs);
		glDeleteShader(fs);
		return 0;
	}
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "aPosition");
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!checkShaderProgram(prog))
	{
		fprintf(stderr, "PostFX: cannot link \"%s\" shader\n", name);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static void postUniform1i(GLuint prog, const char *name, GLint v)
{
	glUniform1i(glGetUniformLocation(prog, name), v);
}
static void postUniform1f(GLuint prog, const char *name, float v)
{
	glUniform1f(glGetUniformLocation(prog, name), v);
}
static void postUniform4f(GLuint prog, const char *name, float x, float y, float z, float w)
{
	glUniform4f(glGetUniformLocation(prog, name), x, y, z, w);
}
static void postUniform2f(GLuint prog, const char *name, float x, float y)
{
	glUniform2f(glGetUniformLocation(prog, name), x, y);
}

static void postBindTexture(GLint unit, GLuint tex)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, tex);
}

static GLuint postCreateTexture(GLint internalFormat, int32_t w, int32_t h, GLenum format, GLenum type, GLint filter)
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return tex;
}

static BOOL postCheckFramebuffer(const char *name)
{
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		fprintf(stderr, "PostFX: framebuffer \"%s\" incomplete (0x%X)\n", name, status);
		return false;
	}
	return true;
}

static BOOL postCreateTarget(PostTarget *t, int32_t w, int32_t h, BOOL hdr)
{
	t->w = SDL_max(w, 1);
	t->h = SDL_max(h, 1);
	if (hdr && g_post.hasFloat)
		t->tex = postCreateTexture(GL_RGBA16F, t->w, t->h, GL_RGBA, GL_HALF_FLOAT, GL_LINEAR);
	else
		t->tex = postCreateTexture(GL_RGBA, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	glGenFramebuffers(1, &t->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
	return postCheckFramebuffer("effect");
}
static void postDestroyTarget(PostTarget *t)
{
	if (t->fbo)
		glDeleteFramebuffers(1, &t->fbo);
	if (t->tex)
		glDeleteTextures(1, &t->tex);
	memset(t, 0, sizeof *t);
}

static void postDrawQuad(const PostTarget *t)
{
	if (t)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
		glViewport(0, 0, t->w, t->h);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Public API */

static void postDestroyTargets()
{
	uint32_t i;
	if (g_post.sceneFbo)
		glDeleteFramebuffers(1, &g_post.sceneFbo);
	if (g_post.msFbo)
		glDeleteFramebuffers(1, &g_post.msFbo);
	if (g_post.sceneColor)
		glDeleteTextures(1, &g_post.sceneColor);
	if (g_post.sceneMask)
		glDeleteTextures(1, &g_post.sceneMask);
	if (g_post.sceneDepth)
		glDeleteTextures(1, &g_post.sceneDepth);
	if (g_post.msColor)
		glDeleteRenderbuffers(1, &g_post.msColor);
	if (g_post.msMask)
		glDeleteRenderbuffers(1, &g_post.msMask);
	if (g_post.msDepth)
		glDeleteRenderbuffers(1, &g_post.msDepth);
	g_post.sceneFbo = g_post.msFbo = 0;
	g_post.sceneColor = g_post.sceneMask = g_post.sceneDepth = 0;
	g_post.msColor = g_post.msMask = g_post.msDepth = 0;
	postDestroyTarget(&g_post.aoLin);
	postDestroyTarget(&g_post.aoA);
	postDestroyTarget(&g_post.aoB);
	postDestroyTarget(&g_post.sky);
	postDestroyTarget(&g_post.env[0]);
	postDestroyTarget(&g_post.env[1]);
	postDestroyTarget(&g_post.env1);
	postDestroyTarget(&g_post.env2);
	postDestroyTarget(&g_post.reflA);
	postDestroyTarget(&g_post.reflB);
	postDestroyTarget(&g_post.shafts);
	for (i = 0; i < BloomLevels; ++i)
		postDestroyTarget(&g_post.bloom[i]);
	g_post.bloomLevels = 0;
	postDestroyTarget(&g_post.postA);
	postDestroyTarget(&g_post.postB);
	g_post.w = g_post.h = 0;
}

static void postDestroy()
{
	postDestroyTargets();
	if (g_post.aoPrepProg) glDeleteProgram(g_post.aoPrepProg);
	if (g_post.aoProg) glDeleteProgram(g_post.aoProg);
	if (g_post.blurProg) glDeleteProgram(g_post.blurProg);
	if (g_post.prefilterProg) glDeleteProgram(g_post.prefilterProg);
	if (g_post.downProg) glDeleteProgram(g_post.downProg);
	if (g_post.upProg) glDeleteProgram(g_post.upProg);
	if (g_post.compositeProg) glDeleteProgram(g_post.compositeProg);
	if (g_post.fxaaProg) glDeleteProgram(g_post.fxaaProg);
	if (g_post.finalProg) glDeleteProgram(g_post.finalProg);
	if (g_post.skyProg) glDeleteProgram(g_post.skyProg);
	if (g_post.ssrProg) glDeleteProgram(g_post.ssrProg);
	if (g_post.reflBlurProg) glDeleteProgram(g_post.reflBlurProg);
	if (g_post.shaftsProg) glDeleteProgram(g_post.shaftsProg);
	g_post.skyProg = g_post.ssrProg = g_post.reflBlurProg = g_post.shaftsProg = 0;
	g_post.aoPrepProg = g_post.aoProg = g_post.blurProg = g_post.prefilterProg = g_post.downProg = 0;
	g_post.upProg = g_post.compositeProg = g_post.fxaaProg = g_post.finalProg = 0;
	g_post.ok = false;
}

/* Called after a GL context has been created. Returns false when the final pass cannot be used. */
static BOOL postInit()
{
	const char *version = (const char *)glGetString(GL_VERSION);
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	int major = version ? atoi(version) : 2;

	g_post.hasFloat = major >= 3 || (ext && strstr(ext, "GL_ARB_texture_float") && strstr(ext, "GL_ARB_half_float_pixel"));
	g_post.hasRed = major >= 3 || (ext && strstr(ext, "GL_ARB_texture_rg"));
	g_post.hasMsaa = glBlitFramebuffer && glRenderbufferStorageMultisample;

	g_post.finalProg = postCompile("final", g_postFinalSrc);
	if (!g_post.finalProg)
		return false;

	g_post.aoPrepProg = postCompile("ssao prepare", g_postAOPrepSrc);
	g_post.aoProg = postCompile("ssao", g_postAOSrc);
	g_post.blurProg = postCompile("blur", g_postBlurSrc);
	g_post.prefilterProg = postCompile("bloom prefilter", g_postPrefilterSrc);
	g_post.downProg = postCompile("bloom downsample", g_postDownSrc);
	g_post.upProg = postCompile("bloom upsample", g_postUpSrc);
	g_post.compositeProg = postCompile("composite", g_postCompositeSrc);
	g_post.fxaaProg = postCompile("fxaa", g_postFxaaSrc);
	/* Optional effects */
	g_post.skyProg = postCompile("sky probe", g_postSkySrc);
	g_post.ssrProg = postCompile("reflections", g_postSSRSrc);
	g_post.reflBlurProg = postCompile("reflection blur", g_postReflBlurSrc);
	g_post.shaftsProg = postCompile("sun shafts", g_postShaftsSrc);
	g_post.envUpdateProg = postCompile("environment update", g_postEnvUpdateSrc);
	g_post.envDownProg = postCompile("environment blur", g_postEnvDownSrc);

	g_post.ok = g_post.aoPrepProg && g_post.aoProg && g_post.blurProg && g_post.prefilterProg && g_post.downProg &&
	            g_post.upProg && g_post.compositeProg && g_post.fxaaProg && glDrawBuffers;
	if (!g_post.ok)
		fprintf(stderr, "PostFX: post-processing is not available\n");
	g_post.active = g_post.ok && gfx.postProcessing;
	return true;
}

/* (Re)creates render targets for internal resolution w x h. Returns framebuffer the game should render into. */
static GLuint postCreateTargets(int32_t w, int32_t h)
{
	GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	GLint maxSamples = 0;
	int32_t i, bw, bh;

	postDestroyTargets();

	g_post.w = w;
	g_post.h = h;

	/* Resolved scene: color, world mask and depth textures */
	glGenFramebuffers(1, &g_post.sceneFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_post.sceneFbo);
	g_post.sceneColor = postCreateTexture(GL_RGBA, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_post.sceneColor, 0);
	if (glDrawBuffers)
	{
		g_post.sceneMask = postCreateTexture(GL_RGBA, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_post.sceneMask, 0);
	}
	g_post.sceneDepth = postCreateTexture(GL_DEPTH_COMPONENT24, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_post.sceneDepth, 0);
	if (glDrawBuffers)
		glDrawBuffers(2, buffers);
	if (!postCheckFramebuffer("scene"))
	{
		framebufferError = true;
		raise(SIGABRT);
	}

	/* Multisampled render target */
	g_post.samples = 0;
	if (gfx.msaa >= 2 && g_post.hasMsaa)
	{
		glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
		g_post.samples = SDL_min(gfx.msaa, maxSamples);
	}
	if (g_post.samples >= 2)
	{
		glGenFramebuffers(1, &g_post.msFbo);
		glBindFramebuffer(GL_FRAMEBUFFER, g_post.msFbo);

		glGenRenderbuffers(1, &g_post.msColor);
		glBindRenderbuffer(GL_RENDERBUFFER, g_post.msColor);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_post.samples, GL_RGBA8, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_post.msColor);

		if (glDrawBuffers)
		{
			glGenRenderbuffers(1, &g_post.msMask);
			glBindRenderbuffer(GL_RENDERBUFFER, g_post.msMask);
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_post.samples, GL_RGBA8, w, h);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, g_post.msMask);
		}

		glGenRenderbuffers(1, &g_post.msDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, g_post.msDepth);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_post.samples, GL_DEPTH_COMPONENT24, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_post.msDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		if (glDrawBuffers)
			glDrawBuffers(2, buffers);
		if (!postCheckFramebuffer("msaa"))
		{
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &g_post.msFbo);
			glDeleteRenderbuffers(1, &g_post.msColor);
			glDeleteRenderbuffers(1, &g_post.msMask);
			glDeleteRenderbuffers(1, &g_post.msDepth);
			g_post.msFbo = g_post.msColor = g_post.msMask = g_post.msDepth = 0;
			g_post.samples = 0;
		}
	}

	/* Effect targets */
	if (g_post.ok)
	{
		BOOL ok = true;
		/* Linear depth for SSAO needs a 32-bit float target, without it SSAO is not available */
		if (g_post.hasFloat)
		{
			PostTarget *t = &g_post.aoLin;
			t->w = (w + 1) / 2;
			t->h = (h + 1) / 2;
			t->tex = postCreateTexture(g_post.hasRed ? 0x822E /* GL_R32F */ : 0x8814 /* GL_RGBA32F */, t->w, t->h, g_post.hasRed ? 0x1903 /* GL_RED */ : GL_RGBA, GL_FLOAT, GL_NEAREST);
			glGenFramebuffers(1, &t->fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
			if (!postCheckFramebuffer("ssao depth"))
				postDestroyTarget(t);
		}
		ok &= postCreateTarget(&g_post.aoA, (w + 1) / 2, (h + 1) / 2, false);
		ok &= postCreateTarget(&g_post.aoB, (w + 1) / 2, (h + 1) / 2, false);
		if (g_post.hasFloat && g_post.skyProg)
		{
			/* Optional HDR targets: sky probe, reflections and sun shafts */
			if (postCreateTarget(&g_post.sky, 4, 1, true))
			{
				glBindTexture(GL_TEXTURE_2D, g_post.sky.tex);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				if (!postCreateTarget(&g_post.reflA, (w + 1) / 2, (h + 1) / 2, true) || !postCreateTarget(&g_post.reflB, (w + 1) / 2, (h + 1) / 2, true))
				{
					postDestroyTarget(&g_post.reflA);
					postDestroyTarget(&g_post.reflB);
				}
				if (!postCreateTarget(&g_post.shafts, (w + 1) / 2, (h + 1) / 2, true))
					postDestroyTarget(&g_post.shafts);
				if (g_post.envUpdateProg && g_post.envDownProg)
				{
					int32_t e;
					BOOL envOk = postCreateTarget(&g_post.env[0], 256, 128, true) && postCreateTarget(&g_post.env[1], 256, 128, true) &&
					             postCreateTarget(&g_post.env1, 64, 32, true) && postCreateTarget(&g_post.env2, 16, 8, true);
					for (e = 0; envOk && e < 2; ++e)
					{
						/* Nothing seen yet: alpha 0 */
						glBindFramebuffer(GL_FRAMEBUFFER, g_post.env[e].fbo);
						glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
						glClear(GL_COLOR_BUFFER_BIT);
						glBindTexture(GL_TEXTURE_2D, g_post.env[e].tex);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					}
					if (envOk)
					{
						glBindTexture(GL_TEXTURE_2D, g_post.env1.tex);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
						glBindTexture(GL_TEXTURE_2D, g_post.env2.tex);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					}
					else
					{
						postDestroyTarget(&g_post.env[0]);
						postDestroyTarget(&g_post.env[1]);
						postDestroyTarget(&g_post.env1);
						postDestroyTarget(&g_post.env2);
					}
					g_post.envTrack = -2;
				}
			}
			else
			{
				postDestroyTarget(&g_post.sky);
			}
		}
		bw = (w + 1) / 2;
		bh = (h + 1) / 2;
		for (i = 0; i < BloomLevels && bw >= 4 && bh >= 4; ++i)
		{
			ok &= postCreateTarget(&g_post.bloom[i], bw, bh, true);
			bw = (bw + 1) / 2;
			bh = (bh + 1) / 2;
			++g_post.bloomLevels;
		}
		ok &= postCreateTarget(&g_post.postA, w, h, false);
		ok &= postCreateTarget(&g_post.postB, w, h, false);
		if (!ok)
		{
			fprintf(stderr, "PostFX: cannot create effect targets, post-processing disabled\n");
			g_post.ok = g_post.active = false;
		}
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	return g_post.msFbo ? g_post.msFbo : g_post.sceneFbo;
}

static void postResolve()
{
	if (!g_post.msFbo)
		return;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_post.msFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_post.sceneFbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(0, 0, g_post.w, g_post.h, 0, 0, g_post.w, g_post.h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	if (g_post.msMask)
	{
		GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
		glReadBuffer(GL_COLOR_ATTACHMENT1);
		glDrawBuffer(GL_COLOR_ATTACHMENT1);
		glBlitFramebuffer(0, 0, g_post.w, g_post.h, 0, 0, g_post.w, g_post.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glDrawBuffers(2, buffers);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Main view projection, and the sub-view (rear-view mirror) drawn in this frame */
/* Sky.c: binds the track's sky panorama (sharp, blurred) for the main view; false without one */
static BOOL skyMainPanorama(int32_t unit, float camToWorld[16], float *yawRadians, float *elevationMin);
/* Sky.c: camera -> world matrix of the main view of this frame; false if unknown */
static BOOL skyMainCamera(float camToWorld[16]);
/* CarModels.c: the player's headlights in camera space (two lamps, beam basis); returns the strength, 0 - off */
static float carHeadlights(float pos[6], float basis[9], float *nightAmbient);
/* Sky.c: direction towards the sun in camera space; returns the strength, 0 - no sun */
static float skySunCamera(float dir[3]);

static void postViewUniforms(GLuint prog, float projX, float projY)
{
	const float *r = g_subViewRect;
	const float aspect = (r[2] - r[0]) * g_virtualWidth / ((r[3] - r[1]) * 480.0f + 1e-6f);
	postUniform2f(prog, "uProj", projX, projY);
	postUniform4f(prog, "uSub", r[0], r[1], r[2], r[3]);
	postUniform2f(prog, "uProjSub", projY * aspect, projY);
	postUniform1f(prog, "uSubOn", g_frameSubView ? 1.0f : 0.0f);
}

static void postEffects()
{
	const float white = 4.0f;
	const float whiteS = 1.0f - 1.0f / white;
	const float projX = g_virtualWidth * 0.5f / gfx.focalLength, projY = 240.0f / gfx.focalLength;
	BOOL ao = gfx.aoStrength > 0.0f && g_post.aoLin.tex;
	BOOL bloom = gfx.bloomStrength > 0.0f && g_post.bloomLevels > 0;
	BOOL sky = g_post.sky.tex && (gfx.reflections > 0.0f || gfx.sunShafts > 0.0f || gfx.haze > 0.0f);
	BOOL refl = sky && gfx.reflections > 0.0f && g_post.aoLin.tex && g_post.reflA.tex && g_post.ssrProg && g_post.reflBlurProg;
	BOOL shafts = sky && gfx.sunShafts > 0.0f && g_post.shafts.tex && g_post.shaftsProg;
	int32_t i;

	/* Linear depth (SSAO and reflections) */
	if ((ao || refl) && g_post.aoLin.tex)
	{
		glUseProgram(g_post.aoPrepProg);
		postUniform1i(g_post.aoPrepProg, "uDepth", 0);
		postUniform1i(g_post.aoPrepProg, "uMask", 1);
		postBindTexture(0, g_post.sceneDepth);
		postBindTexture(1, g_post.sceneMask);
		postDrawQuad(&g_post.aoLin);
	}

	/* Sky probe: average sky color and the sun */
	if (sky)
	{
		glUseProgram(g_post.skyProg);
		postUniform1i(g_post.skyProg, "uColor", 0);
		postUniform1i(g_post.skyProg, "uDepth", 1);
		postUniform1i(g_post.skyProg, "uMask", 2);
		postUniform1f(g_post.skyProg, "uWhite", whiteS);
		postBindTexture(0, g_post.sceneColor);
		postBindTexture(1, g_post.sceneDepth);
		postBindTexture(2, g_post.sceneMask);
		postDrawQuad(&g_post.sky);
	}

	/* Dynamic environment for car model reflections */
	if (g_post.env2.tex && g_post.aoLin.tex && (ao || refl))
	{
		float cam[16];
		if (skyMainCamera(cam))
		{
			const int32_t next = g_post.envCur ^ 1;
			if (g_post.envTrack != gfxCurrentTrack)
			{
				/* Another track: forget what was seen */
				int32_t e;
				for (e = 0; e < 2; ++e)
				{
					glBindFramebuffer(GL_FRAMEBUFFER, g_post.env[e].fbo);
					glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
					glClear(GL_COLOR_BUFFER_BIT);
				}
				g_post.envTrack = gfxCurrentTrack;
			}
			glUseProgram(g_post.envUpdateProg);
			postUniform1i(g_post.envUpdateProg, "uPrev", 0);
			postUniform1i(g_post.envUpdateProg, "uColor", 1);
			postUniform1i(g_post.envUpdateProg, "uLin", 2);
			postUniform1i(g_post.envUpdateProg, "uMask", 3);
			postViewUniforms(g_post.envUpdateProg, projX, projY);
			glUniformMatrix4fv(glGetUniformLocation(g_post.envUpdateProg, "uCam"), 1, GL_FALSE, cam);
			postUniform1f(g_post.envUpdateProg, "uWhite", whiteS);
			postUniform1f(g_post.envUpdateProg, "uRate", 0.25f);
			postBindTexture(0, g_post.env[g_post.envCur].tex);
			postBindTexture(1, g_post.sceneColor);
			postBindTexture(2, g_post.aoLin.tex);
			postBindTexture(3, g_post.sceneMask);
			postDrawQuad(&g_post.env[next]);
			g_post.envCur = next;

			glUseProgram(g_post.envDownProg);
			postUniform1i(g_post.envDownProg, "uTex", 0);
			postBindTexture(0, g_post.env[next].tex);
			postUniform2f(g_post.envDownProg, "uTexel", 1.0f / g_post.env[next].w, 1.0f / g_post.env[next].h);
			postDrawQuad(&g_post.env1);
			postBindTexture(0, g_post.env1.tex);
			postUniform2f(g_post.envDownProg, "uTexel", 1.0f / g_post.env1.w, 1.0f / g_post.env1.h);
			postDrawQuad(&g_post.env2);
		}
	}

	/* Screen-space reflections, then glossy blur (sharper and more streaky when wet) */
	if (refl)
	{
		/* Automatic: wet while it rains */
		const float wet = gfx.wetness >= 0.0f ? gfx.wetness : 0.15f + 0.7f * g_rainLevel;
		glUseProgram(g_post.ssrProg);
		postUniform1i(g_post.ssrProg, "uLin", 0);
		postUniform1i(g_post.ssrProg, "uColor", 1);
		postUniform1i(g_post.ssrProg, "uSky", 2);
		postViewUniforms(g_post.ssrProg, projX, projY);
		postUniform2f(g_post.ssrProg, "uTexel", 1.0f / g_post.aoLin.w, 1.0f / g_post.aoLin.h);
		postUniform1f(g_post.ssrProg, "uWhite", whiteS);
		postUniform1f(g_post.ssrProg, "uStrength", gfx.reflections);
		postUniform1f(g_post.ssrProg, "uWetness", wet);
		{
			/* Sky panorama of the main view for rays that leave the screen */
			float cam[16], yaw = 0.0f, elevationMin = 0.0f;
			const BOOL pano = skyMainPanorama(3, cam, &yaw, &elevationMin);
			postUniform1f(g_post.ssrProg, "uPanoOn", pano ? 1.0f : 0.0f);
			postUniform1i(g_post.ssrProg, "uPano0", 3);
			postUniform1i(g_post.ssrProg, "uPano1", 4);
			if (pano)
			{
				glUniformMatrix4fv(glGetUniformLocation(g_post.ssrProg, "uPanoCam"), 1, GL_FALSE, cam);
				postUniform2f(g_post.ssrProg, "uPanoParams", yaw, elevationMin);
			}
		}
		postBindTexture(0, g_post.aoLin.tex);
		postBindTexture(1, g_post.sceneColor);
		postBindTexture(2, g_post.sky.tex);
		postDrawQuad(&g_post.reflA);

		glUseProgram(g_post.reflBlurProg);
		postUniform1i(g_post.reflBlurProg, "uTex", 0);
		postUniform1i(g_post.reflBlurProg, "uLin", 1);
		postBindTexture(1, g_post.aoLin.tex);
		postBindTexture(0, g_post.reflA.tex);
		postUniform2f(g_post.reflBlurProg, "uStep", (2.5f - 2.0f * wet) / g_post.reflA.w, 0.0f);
		postDrawQuad(&g_post.reflB);
		postBindTexture(0, g_post.reflB.tex);
		postUniform2f(g_post.reflBlurProg, "uStep", 0.0f, (2.5f - 1.0f * wet) / g_post.reflA.h);
		postDrawQuad(&g_post.reflA);
	}

	/* Sun shafts */
	if (shafts)
	{
		glUseProgram(g_post.shaftsProg);
		postUniform1i(g_post.shaftsProg, "uColor", 0);
		postUniform1i(g_post.shaftsProg, "uDepth", 1);
		postUniform1i(g_post.shaftsProg, "uMask", 2);
		postUniform1i(g_post.shaftsProg, "uSky", 3);
		postUniform1f(g_post.shaftsProg, "uWhite", whiteS);
		postUniform2f(g_post.shaftsProg, "uAspect", g_virtualWidth / 480.0f, 1.0f);
		postBindTexture(0, g_post.sceneColor);
		postBindTexture(1, g_post.sceneDepth);
		postBindTexture(2, g_post.sceneMask);
		postBindTexture(3, g_post.sky.tex);
		postDrawQuad(&g_post.shafts);
		postBindTexture(3, 0);
	}

	/* SSAO */
	if (ao)
	{
		glUseProgram(g_post.aoProg);
		postUniform1i(g_post.aoProg, "uLin", 0);
		postViewUniforms(g_post.aoProg, projX, projY);
		postUniform2f(g_post.aoProg, "uTexel", 1.0f / g_post.aoLin.w, 1.0f / g_post.aoLin.h);
		postUniform1f(g_post.aoProg, "uRadius", gfx.aoRadius);
		postUniform1f(g_post.aoProg, "uIntensity", 3.0f * gfx.aoStrength);
		postBindTexture(0, g_post.aoLin.tex);
		postDrawQuad(&g_post.aoA);

		glUseProgram(g_post.blurProg);
		postUniform1i(g_post.blurProg, "uAO", 0);
		postUniform1i(g_post.blurProg, "uDepth", 1);
		postBindTexture(1, g_post.aoLin.tex);
		postBindTexture(0, g_post.aoA.tex);
		postUniform2f(g_post.blurProg, "uStep", 1.0f / g_post.aoA.w, 0.0f);
		postDrawQuad(&g_post.aoB);
		postBindTexture(0, g_post.aoB.tex);
		postUniform2f(g_post.blurProg, "uStep", 0.0f, 1.0f / g_post.aoA.h);
		postDrawQuad(&g_post.aoA);
	}

	/* Bloom */
	if (bloom)
	{
		glUseProgram(g_post.prefilterProg);
		postUniform1i(g_post.prefilterProg, "uColor", 0);
		postUniform1i(g_post.prefilterProg, "uMask", 1);
		postUniform2f(g_post.prefilterProg, "uTexel", 1.0f / g_post.w, 1.0f / g_post.h);
		postUniform1f(g_post.prefilterProg, "uWhite", whiteS);
		postUniform1f(g_post.prefilterProg, "uThreshold", gfx.bloomThreshold);
		postBindTexture(0, g_post.sceneColor);
		postBindTexture(1, g_post.sceneMask);
		postDrawQuad(&g_post.bloom[0]);

		glUseProgram(g_post.downProg);
		postUniform1i(g_post.downProg, "uTex", 0);
		for (i = 1; i < g_post.bloomLevels; ++i)
		{
			postBindTexture(0, g_post.bloom[i - 1].tex);
			postUniform2f(g_post.downProg, "uTexel", 1.0f / g_post.bloom[i - 1].w, 1.0f / g_post.bloom[i - 1].h);
			postDrawQuad(&g_post.bloom[i]);
		}

		glUseProgram(g_post.upProg);
		postUniform1i(g_post.upProg, "uTex", 0);
		postUniform1f(g_post.upProg, "uScale", 1.0f);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		for (i = g_post.bloomLevels - 1; i > 0; --i)
		{
			postBindTexture(0, g_post.bloom[i].tex);
			postUniform2f(g_post.upProg, "uTexel", 1.0f / g_post.bloom[i].w, 1.0f / g_post.bloom[i].h);
			postDrawQuad(&g_post.bloom[i - 1]);
		}
		glDisable(GL_BLEND);
	}

	/* Composite */
	glUseProgram(g_post.compositeProg);
	postUniform1i(g_post.compositeProg, "uColor", 0);
	postUniform1i(g_post.compositeProg, "uMask", 1);
	postUniform1i(g_post.compositeProg, "uDepth", 2);
	postUniform1i(g_post.compositeProg, "uAO", 3);
	postUniform1i(g_post.compositeProg, "uBloom", 4);
	postUniform1i(g_post.compositeProg, "uAOLin", 5);
	postUniform1i(g_post.compositeProg, "uRefl", 6);
	postUniform1i(g_post.compositeProg, "uShafts", 7);
	postUniform1i(g_post.compositeProg, "uSky", 8);
	postUniform1f(g_post.compositeProg, "uReflections", refl ? 1.0f : 0.0f);
	postUniform1f(g_post.compositeProg, "uShaftStrength", shafts ? gfx.sunShafts : 0.0f);
	postUniform1f(g_post.compositeProg, "uHaze", sky ? gfx.haze : 0.0f);
	{
		/* Haze towards the sky panorama in the pixel's direction (units 9, 10) */
		float cam[16], yaw = 0.0f, elevationMin = 0.0f;
		const BOOL pano = skyMainPanorama(9, cam, &yaw, &elevationMin);
		postViewUniforms(g_post.compositeProg, (float)g_virtualWidth * 0.5f / gfx.focalLength, 240.0f / gfx.focalLength);
		postUniform1f(g_post.compositeProg, "uPanoOn", pano ? 1.0f : 0.0f);
		postUniform1i(g_post.compositeProg, "uPano", 10);
		if (pano)
		{
			glUniformMatrix4fv(glGetUniformLocation(g_post.compositeProg, "uPanoCam"), 1, GL_FALSE, cam);
			postUniform2f(g_post.compositeProg, "uPanoParams", yaw, elevationMin);
		}
	}
	{
		float hlPos[6], hlBasis[9], night = 1.0f;
		const float hl = carHeadlights(hlPos, hlBasis, &night);
		postUniform1f(g_post.compositeProg, "uHlStrength", hl);
		postUniform1f(g_post.compositeProg, "uNightAmbient", night);
		if (hl > 0.0f)
		{
			glUniform3fv(glGetUniformLocation(g_post.compositeProg, "uHlPos"), 2, hlPos);
			glUniformMatrix3fv(glGetUniformLocation(g_post.compositeProg, "uHlBasis"), 1, GL_FALSE, hlBasis);
		}
	}
	{
		float sun[3] = {0.0f, 1.0f, 0.0f};
		const float s = gfx.shadows > 0.0f ? skySunCamera(sun) * gfx.shadows : 0.0f;
		postUniform1f(g_post.compositeProg, "uShadow", s);
		glUniform3f(glGetUniformLocation(g_post.compositeProg, "uSunCam"), sun[0], sun[1], sun[2]);
	}
	postUniform2f(g_post.compositeProg, "uAOTexel", 1.0f / g_post.aoA.w, 1.0f / g_post.aoA.h);
	postUniform2f(g_post.compositeProg, "uAOSize", g_post.aoA.w, g_post.aoA.h);
	postUniform1f(g_post.compositeProg, "uAOStrength", ao ? gfx.aoStrength : 0.0f);
	/* Bloom is energy-normalized over the levels */
	postUniform1f(g_post.compositeProg, "uBloomStrength", bloom ? gfx.bloomStrength / g_post.bloomLevels : 0.0f);
	postUniform1f(g_post.compositeProg, "uWhite", whiteS);
	postUniform1f(g_post.compositeProg, "uExposure", gfx.exposure);
	postUniform1f(g_post.compositeProg, "uContrast", gfx.contrast);
	postUniform1f(g_post.compositeProg, "uSaturation", gfx.saturation);
	postUniform1f(g_post.compositeProg, "uWarmth", gfx.warmth);
	postUniform1f(g_post.compositeProg, "uToneMapping", gfx.toneMapping ? 1.0f : 0.0f);
	postUniform1i(g_post.compositeProg, "uDebug", g_post.debugView);
	postBindTexture(0, g_post.sceneColor);
	postBindTexture(1, g_post.sceneMask);
	postBindTexture(2, g_post.sceneDepth);
	postBindTexture(3, g_post.aoA.tex);
	postBindTexture(4, bloom ? g_post.bloom[0].tex : 0);
	postBindTexture(5, ao ? g_post.aoLin.tex : 0);
	postBindTexture(6, refl ? g_post.reflA.tex : 0);
	postBindTexture(7, shafts ? g_post.shafts.tex : 0);
	postBindTexture(8, sky ? g_post.sky.tex : 0);
	postDrawQuad(&g_post.postA);
	postBindTexture(8, 0);
	postBindTexture(7, 0);
	postBindTexture(6, 0);
	postBindTexture(5, 0);
	postBindTexture(4, 0);
	postBindTexture(3, 0);
	postBindTexture(2, 0);

	/* FXAA */
	if (gfx.fxaa && g_post.debugView == 0)
	{
		glUseProgram(g_post.fxaaProg);
		postUniform1i(g_post.fxaaProg, "uTex", 0);
		postUniform2f(g_post.fxaaProg, "uTexel", 1.0f / g_post.w, 1.0f / g_post.h);
		postBindTexture(0, g_post.postA.tex);
		postDrawQuad(&g_post.postB);
	}
}

static void postScreenshot(int32_t x, int32_t y, int32_t w, int32_t h, const char *forcedPath)
{
	uint8_t *pixels = (uint8_t *)malloc(w * h * 4);
	char *path = NULL;
	int32_t row;
	if (!pixels)
		return;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	for (row = 0; row < h / 2; ++row)
	{
		uint8_t *a = pixels + row * w * 4, *b = pixels + (h - 1 - row) * w * 4;
		int32_t i;
		for (i = 0; i < w * 4; ++i)
		{
			uint8_t t = a[i];
			a[i] = b[i];
			b[i] = t;
		}
	}
	for (row = 0; row < w * h; ++row)
		pixels[row * 4 + 3] = 255;
	if (forcedPath)
	{
		path = strdup(forcedPath);
	}
	else
	{
		const char *dir = getSettingsDir();
		time_t now = time(NULL);
		char stamp[32];
		strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
		path = (char *)malloc((dir ? strlen(dir) : 2) + 64);
		if (path)
			sprintf(path, "%sscreenshots/nfs2se-%s-%u.png", dir ? dir : "./", stamp, (unsigned)(g_post.frame % 1000));
	}
	if (path)
	{
		if (stbi_write_png(path, w, h, 4, pixels, w * 4))
			fprintf(stderr, "Screenshot saved: %s\n", path);
		else
			fprintf(stderr, "Cannot save screenshot: %s\n", path);
		free(path);
	}
	free(pixels);
}

/* Runs the effects and draws the image into the default framebuffer at the given viewport */
static void postPresent(int32_t vx, int32_t vy, int32_t vw, int32_t vh, int32_t xOffset, int32_t yOffset, int32_t visibleWidth, int32_t visibleHeight, float gamma, BOOL has3D)
{
	GLuint source;
	BOOL useMask = false;
	float scale;
	int32_t taps;

	if (gfxPostToggleRequest)
	{
		gfxPostToggleRequest = false;
		g_post.active = g_post.ok && !g_post.active;
		fprintf(stderr, "Post-processing: %s\n", g_post.active ? "on" : "off");
	}
	if (gfxDebugViewRequest >= 0)
	{
		static const char *names[] = {"off", "world mask", "ambient occlusion", "depth", "bloom", "reflections", "sun shafts", "headlights", "shadows"};
		gfxDebugViewRequest = -1;
		g_post.debugView = (g_post.debugView + 1) % 9;
		fprintf(stderr, "Debug view: %s\n", names[g_post.debugView]);
	}

	++g_post.frame;
	g_texFrame = g_post.frame;

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	postResolve();

	source = g_post.sceneColor;
	/* Frames without 3D geometry (menus, movies, loading screens) are presented untouched */
	if (g_post.active && (has3D || g_post.debugView != 0))
	{
		postEffects();
		source = (gfx.fxaa && g_post.debugView == 0) ? g_post.postB.tex : g_post.postA.tex;
		useMask = g_post.debugView == 0;
	}

	/* Final pass into the window */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glEnable(GL_SCISSOR_TEST);
	clearUnusedArea(xOffset, yOffset, visibleWidth, visibleHeight);
	glViewport(vx, vy, vw, vh);
	glScissor(vx, vy, vw, vh);

	scale = (float)g_post.h / (float)SDL_max(vh, 1);
	taps = (scale > 1.01f) ? (int32_t)SDL_ceilf(scale - 0.01f) : 1;
	if (taps > 4)
		taps = 4;

	glUseProgram(g_post.finalProg);
	postUniform1i(g_post.finalProg, "uTex", 0);
	postUniform2f(g_post.finalProg, "uOutTexel", 1.0f / vw, 1.0f / vh);
	postUniform1i(g_post.finalProg, "uTaps", taps);
	postUniform1f(g_post.finalProg, "uUseMask", useMask ? 1.0f : 0.0f);
	postUniform1f(g_post.finalProg, "uSharpen", g_post.active && useMask ? gfx.sharpen : 0.0f);
	postUniform1f(g_post.finalProg, "uVignette", g_post.active && useMask ? gfx.vignette : 0.0f);
	postUniform1f(g_post.finalProg, "uGrain", g_post.active && useMask ? gfx.filmGrain : 0.0f);
	postUniform1f(g_post.finalProg, "uGamma", gamma > 0.0f ? gamma : 1.0f);
	postUniform1f(g_post.finalProg, "uTime", (float)(g_post.frame % 1024));
	postUniform1f(g_post.finalProg, "uAspect", g_virtualWidth / 480.0f);
	postBindTexture(1, 0);
	postBindTexture(0, source);
	if (taps == 1 && (!framebufferLinearFiltering || scale > 0.99f))
	{
		/* No scaling (or nearest scaling requested) */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}
	postDrawQuad(NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	if (gfxScreenshotRequest)
	{
		gfxScreenshotRequest = false;
		postScreenshot(vx, vy, vw, vh, NULL);
	}
}
