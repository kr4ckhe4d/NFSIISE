// SPDX-License-Identifier: MIT

#include "../Glide2x.h"

#include <SDL2/SDL_stdinc.h>
#include <SDL2/SDL_video.h>

#ifdef GLES2
# include <SDL2/SDL_opengles2.h>
#else
# include <SDL2/SDL_opengl.h>
static PFNGLGETSHADERIVPROC glGetShaderiv;
static PFNGLGETPROGRAMIVPROC glGetProgramiv;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
static PFNGLCREATESHADERPROC glCreateShader;
static PFNGLDELETESHADERPROC glDeleteShader;
static PFNGLSHADERSOURCEPROC glShaderSource;
static PFNGLCOMPILESHADERPROC glCompileShader;
static PFNGLCREATEPROGRAMPROC glCreateProgram;
static PFNGLDELETEPROGRAMPROC glDeleteProgram;
static PFNGLATTACHSHADERPROC glAttachShader;
static PFNGLLINKPROGRAMPROC glLinkProgram;
static PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
static PFNGLUSEPROGRAMPROC glUseProgram;
static PFNGLUNIFORM1IPROC glUniform1i;
static PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;
static PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
static PFNGLUNIFORM3FPROC glUniform3f;
static PFNGLUNIFORM1FPROC glUniform1f;
static PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
static PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray;
static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
static PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
static PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;

/* Graphics enhancements (desktop OpenGL only) */
# define GL2_ENHANCED
static PFNGLACTIVETEXTUREPROC p_glActiveTexture;
static PFNGLUNIFORM2FPROC p_glUniform2f;
static PFNGLUNIFORM4FPROC p_glUniform4f;
static PFNGLGENBUFFERSPROC p_glGenBuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;
static PFNGLBUFFERDATAPROC p_glBufferData;
static PFNGLBINDATTRIBLOCATIONPROC p_glBindAttribLocation;
static PFNGLDRAWBUFFERSPROC p_glDrawBuffers;
static PFNGLBLITFRAMEBUFFERPROC p_glBlitFramebuffer;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC p_glRenderbufferStorageMultisample;
# define glActiveTexture p_glActiveTexture
# define glUniform2f p_glUniform2f
# define glUniform4f p_glUniform4f
# define glGenBuffers p_glGenBuffers
# define glBindBuffer p_glBindBuffer
# define glBufferData p_glBufferData
# define glBindAttribLocation p_glBindAttribLocation
# define glDrawBuffers p_glDrawBuffers
# define glBlitFramebuffer p_glBlitFramebuffer
# define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#endif

#include <signal.h>
#include <string.h>
#include <time.h>

BOOL contextError = false;
BOOL shaderError = false;
BOOL framebufferError = false;

#ifdef WIN32
	#undef near
	#undef far
#endif

/* Matrix 4x4 */

static float g_matrix[16];

static inline void matrixLoadIdentity()
{
	g_matrix[ 0] = 1;
	g_matrix[ 5] = 1;
	g_matrix[10] = 1;
	g_matrix[12] = 0;
	g_matrix[13] = 0;
	g_matrix[14] = 0;
	g_matrix[15] = 1;
}
static inline void matrixScale2(float x, float y)
{
	g_matrix[ 0] *= x;
	g_matrix[ 5] *= y;
}
static inline void matrixOrtho(float left, float right, float bottom, float top, float near, float far)
{
	const float rl = (right - left);
	const float tb = (top - bottom);
	const float fn = (far - near);
	g_matrix[ 0] = 2 / rl;
	g_matrix[ 5] = 2 / tb;
	g_matrix[10] = -2 / fn;
	g_matrix[12] = -(left + right) / rl;
	g_matrix[13] = -(top + bottom) / tb;
	g_matrix[14] = -(far + near) / fn;
}

/* Shaders */

const char g_vShaderSrc[] =
#ifdef GLES2
	"precision mediump float;\n"
#else
	"#version 120\n"
	"#define ENHANCED 1\n"
#endif

	"attribute vec4 aPosition;\n"
	"attribute vec4 aTexCoord;\n"
	"attribute vec4 aColor;\n"
	"attribute float aFog;\n"

	"varying vec4 vTexCoord;\n"
	"varying vec4 vColor;\n"
	"varying float vFog;\n"

	"#ifdef ENHANCED\n"
	"attribute float aMask;\n"
	"varying float vMask;\n"
	"varying float vOow;\n"
	"#endif\n"

	"uniform mat4 uMatrix;\n"

	"void main()\n"
	"{\n"
		"vTexCoord = aTexCoord;\n"
		"vColor = aColor;\n"
		"vFog = aFog;\n"
	"#ifdef ENHANCED\n"
		"vMask = aMask;\n"
		"vOow = aPosition.z;\n"
	"#endif\n"
		"gl_Position = uMatrix * aPosition;\n"
	"}\n"
;
const char g_fShaderSrc[] =
#ifdef GLES2
	"precision mediump float;\n"
#else
	"#version 120\n"
	"#define ENHANCED 1\n"
#endif

	"varying vec4 vTexCoord;\n"
	"varying vec4 vColor;\n"
	"varying float vFog;\n"

	"uniform sampler2D uTextureSampler;\n"
	"uniform float uTextureEnabled;\n"
	"uniform float uFogEnabled;\n"
	"uniform vec3 uFogColor;\n"
	"uniform float uAlphaCutout;\n"

	"#ifdef ENHANCED\n"
	"varying float vMask;\n"
	"varying float vOow;\n"
	"uniform sampler2D uFogTable;\n"
	"uniform float uPerPixelFog;\n"
	/* Sky panorama (Sky.c): distant geometry fades into the sky behind it instead of the game's fog colour */
	"uniform float uSkyFog;\n"
	"uniform sampler2D uSkyFogTex;\n"
	"uniform mat4 uSkyFogCam;\n"      /* Camera -> world */
	"uniform vec4 uSkyFogProj;\n"     /* Game projection: scale X, scale Y, center X, center Y */
	"uniform vec4 uSkyFogViewport;\n" /* GL viewport of the view: x, y, width, height */
	"uniform vec4 uSkyFogClip;\n"     /* Its clip window in game units: x, y, width, height */
	"uniform vec2 uSkyFogParams;\n"   /* Panorama yaw (radians), elevation of its bottom row (degrees) */
	"uniform float uSkyFogMix;\n"     /* How much of the game's fog colour stays (its sky glow layers use it too) */
	/* Distant terrain (Terrain.c): lighter fog, geometry dissolves near the draw distance instead (terrain behind) */
	"uniform vec3 uTerrainView;\n"    /* fog scale, dissolve start, dissolve end (metres) */
	"#endif\n"

	"void main()\n"
	"{\n"
		"vec4 texture = texture2D(uTextureSampler, vTexCoord.st / vTexCoord.q);\n"
		/* Cut-out textures (binary alpha): alpha test at the middle of the filtered edge, then opaque */
		"if (uAlphaCutout > 0.5 && uTextureEnabled > 0.5)\n"
		"{\n"
			"if (texture.a < 0.35)\n"
				"discard;\n"
			"texture.a = 1.0;\n"
		"}\n"
		"vec4 ret = vColor * ((1.0 - uTextureEnabled) + uTextureEnabled * texture);\n"

		"if (ret.a <= 16.0 / 255.0)\n"
			"discard;\n"

		"float fog = 1.0 - vFog;\n"
	"#ifdef ENHANCED\n"
		/* Glide table fog evaluated per pixel: index = w = 1 / oow */
		/* vMask == 254/255 marks triangles reaching beyond the fog table (sky): keep per-vertex fog for them */
		"if (uPerPixelFog > 0.5 && vMask > 0.999)\n"
		"{\n"
			"float w = clamp(1.0 / max(vOow, 1e-6), 0.0, 65535.0);\n"
			"float hi = floor(w / 256.0);\n"
			"float lo = floor(w - hi * 256.0);\n"
			"fog = texture2D(uFogTable, (vec2(lo, hi) + 0.5) / 256.0).r;\n"
		"}\n"
	"#endif\n"
		"vec3 fogColor = uFogColor;\n"
	"#ifdef ENHANCED\n"
		"if (uSkyFog > 0.5)\n"
		"{\n"
			"vec2 g = uSkyFogClip.xy + vec2((gl_FragCoord.x - uSkyFogViewport.x) / uSkyFogViewport.z, 1.0 - (gl_FragCoord.y - uSkyFogViewport.y) / uSkyFogViewport.w) * uSkyFogClip.zw;\n"
			"vec3 d = normalize((uSkyFogCam * vec4((g.x - uSkyFogProj.z) / uSkyFogProj.x, (g.y - uSkyFogProj.w) / uSkyFogProj.y, 1.0, 0.0)).xyz);\n"
			"float el = clamp(degrees(asin(clamp(d.y, -1.0, 1.0))), 0.5, 6.0);\n"
			"vec2 uv = vec2(fract((atan(d.x, d.z) + uSkyFogParams.x) / 6.2831853 + 0.5), (90.0 - el) / (90.0 - uSkyFogParams.y));\n"
			"fogColor = mix(texture2D(uSkyFogTex, uv).rgb, uFogColor, uSkyFogMix);\n"
		"}\n"
	"#endif\n"
	"#ifdef ENHANCED\n"
		"float tw = 1.0 / max(vOow, 1e-6);\n"
		/* Only the track's geometry (the game's sky glow layers are farther and keep their fog) */
		"if (uTerrainView.z > 0.0 && vMask > 0.999 && tw < uTerrainView.z * 1.5)\n"
		"{\n"
			"float w = tw;\n"
			"float cell = mod(dot(floor(gl_FragCoord.xy), vec2(1.0, 4.0)), 16.0) / 16.0 + mod(floor(gl_FragCoord.y), 2.0) / 32.0;\n"
			"if (cell < smoothstep(uTerrainView.y, uTerrainView.z, w))\n"
				"discard;\n"
			"fog *= uTerrainView.x;\n"
		"}\n"
	"#endif\n"
		"ret.rgb = mix(ret.rgb, fogColor, fog * uFogEnabled);\n"

	"#ifdef ENHANCED\n"
		"gl_FragData[0] = ret;\n"
		"gl_FragData[1] = vec4(step(0.5, vMask), 0.0, 0.0, ret.a);\n"
	"#else\n"
		"gl_FragColor = ret;\n"
	"#endif\n"
	"}\n"
;

#ifndef GL2_ENHANCED
const char g_vShaderDispSrc[] =
#ifdef GLES2
	"precision mediump float;"
#else
	"#version 110\n"
#endif

	"attribute vec2 aPosition;"
	"attribute vec2 aTexCoord;"

	"varying vec2 vTexCoord;"

	"void main()"
	"{"
		"vTexCoord = aTexCoord;"
		"gl_Position = vec4(aPosition, 0.0, 1.0);"
	"}"
;
const char g_fShaderDispSrc[] =
#ifdef GLES2
	"precision mediump float;"
#else
	"#version 110\n"
#endif

	"varying vec2 vTexCoord;"

	"uniform sampler2D uTextureSampler;"
	"uniform float uGamma;"

	"void main()"
	"{"
		"vec4 texture = texture2D(uTextureSampler, vTexCoord);"
		"texture.rgb = pow(texture.rgb, vec3(1.0 / uGamma));"
		"gl_FragColor = texture;"
	"}"
;
#endif

#define MaxTriangles 0x000400
#define TextureMem   0x200000
#define VertexSnap   0x0C0000
#define Near        -1.0f
#define Far          0.0f

typedef struct
{
	struct
	{
		uint8_t r, g, b, a;
	} vertex[3];
} ColorValues;
typedef struct
{
	uint8_t vertex[3];
} FogCoord;
typedef struct
{
	uint8_t vertex[3];
} MaskValues;
typedef struct
{
	struct
	{
		float s, t, r, q;
	} vertex[3];
} TextureCoord;
typedef struct
{
	struct
	{
		float x, y, z;
	} vertex[3];
} Vertices;
static ColorValues g_colorValues[MaxTriangles];
static FogCoord g_fogCoord[MaxTriangles];
static MaskValues g_maskValues[MaxTriangles];
static TextureCoord g_textureCoord[MaxTriangles];
static Vertices g_vertices[MaxTriangles];

static float g_textureCoordDisp[4][2] = {
	{0.0f, 1.0f},
	{0.0f, 0.0f},
	{1.0f, 1.0f},
	{1.0f, 0.0f},
};
static float g_verticesDisp[4][2] = {
	{-1.0f, +1.0f},
	{-1.0f, -1.0f},
	{+1.0f, +1.0f},
	{+1.0f, -1.0f},
};

typedef struct
{
	void *data;
	void *palette;
	uint64_t hash;      /* Content hash of the GL texture currently used by this slot (0 = none) */
	GrTextureFormat_t fmt;
	uint32_t size;
	uint32_t id;
	uint8_t alphaMode;
	uint8_t used;
} TextureInfo;
static TextureInfo g_textures[TextureMem >> 2];

static uint8_t *g_lfb, g_textureMem[TextureMem], g_fogTable[0x10000];
static uint32_t *g_palette;
static uint32_t g_trianglesCount;

static SDL_GLContext g_glCtx;

extern BOOL keepAspectRatio, needRecreateGl, windowResized, linearFiltering, fixedFramebufferSize, framebufferLinearFiltering;
extern int32_t vSync, winWidth, winHeight, initialWinWidth, initialWinHeight;
extern SDL_Window *sdlWin;

/* GLSL game */
static GLuint g_vShader, g_fShader, g_shaderProgram;
static GLint g_aPositionLoc, g_aTexCoordLoc, g_aColorLoc, g_aFogLoc, g_aMaskLoc, g_uMatrixLoc, g_uTextureEnabledLoc, g_uFogEnabledLoc, g_uFogColorLoc, g_uAlphaCutoutLoc, g_uPerPixelFogLoc;

#ifndef GL2_ENHANCED
/* GLSL display */
static GLuint g_vShaderDisp, g_fShaderDisp, g_shaderProgramDisp;
static GLint g_aPositionLocDisp, g_aTexCoordLocDisp, g_uGammaLocDisp;
#endif

/* Framebuffer */
static uint32_t g_framebuffer;
#ifndef GL2_ENHANCED
static uint32_t g_framebufferTexture;
static uint32_t g_framebufferDepth;
#endif
static int32_t  g_framebufferHeight;
static int32_t  g_framebufferBaseEdge;

/*
 * Widescreen (Hor+): the patched game squeezes full-width 3D views horizontally by "widescreenScale",
 * so a wider field of view fits into its 640x480 screen space. The wrapper stretches those views over
 * a virtual screen of g_virtualWidth x 480 units, while 2D elements (HUD, menus) and sub-views
 * (rear-view mirror) keep 4:3 proportions in the middle of the screen.
 */
extern float widescreenScale;
static float g_virtualWidth = 640.0f;
static BOOL  g_fullWidthView = true;
static float g_clipWindow[4] = {0.0f, 0.0f, 640.0f, 480.0f}; /* Current clip window in 640x480 game units */
static BOOL  g_prevFrameHas3D;
static inline float hudShift(float minX, float maxX, float minY);

/* GLSL config */
static float g_textureEnabled;
static float g_fogEnabled;
static float g_fogColor[3];
static float g_gammaValue;
static float g_alphaCutout;

/* GL config */
static BOOL g_depthMask;
static GLenum g_blendFuncDFactor = GL_ONE_MINUS_SRC_ALPHA;
static GLuint g_boundTexture;
static uint8_t g_boundAlphaMode; /* AlphaOpaque, AlphaCutout or AlphaBlend (Textures.c) */
static uint64_t g_boundHash;     /* Content hash of the bound texture (Textures.c) */

/* Fog table texture (per-pixel fog) */
static GLuint g_fogTableTexture;
static BOOL g_fogTableDirty;

/* World/overlay classification state */
static BOOL g_seen3D;      /* 3D geometry was drawn since the last clip window change */
static BOOL g_frameHas3D;  /* 3D geometry was drawn in this frame */

/* Rain detection: the game draws rain streaks as flat, additive world triangles */
static uint32_t g_frameRainTriangles;

/* Sub-view (rear-view mirror): its own camera, the post-processing needs its rectangle (UV, min/max) */
static float g_clipRectUV[4];
static float g_subViewRect[4];
static BOOL  g_frameSubView;
static float g_rainLevel; /* 0 - dry, 1 - raining (smoothed) */


static BOOL checkShaderCompilation(GLuint shader)
{
	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		GLint maxLen = 0;
		GLsizei len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLen);
		if (maxLen > 1)
		{
			GLchar *log = (GLchar *)malloc(maxLen);
			glGetShaderInfoLog(shader, maxLen, &len, log);
			fprintf(stderr, "%s\n", log);
			free(log);
		}
		return false;
	}
	return true;
}
static BOOL checkShaderProgram(GLuint program)
{
	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		GLint maxLen = 0;
		GLsizei len = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLen);
		if (maxLen > 1)
		{
			GLchar *log = (GLchar *)malloc(maxLen);
			glGetProgramInfoLog(program, maxLen, &len, log);
			fprintf(stderr, "%s\n", log);
			free(log);
		}
		return false;
	}
	return true;
}

static void drawTriangles();

#include "Textures.c"
#ifdef GL2_ENHANCED
# include "PostFX.c"
static void useGameProgram(BOOL gameProgram);
# include "Sky.c"
# include "CarModels.c"
# include "Terrain.c"
#endif

static inline BOOL loadShaders()
{
	{
		const GLint vShaderLen = sizeof(g_vShaderSrc) - 1;
		const GLint fShaderLen = sizeof(g_fShaderSrc) - 1;

		const GLchar *vShaderSrc = g_vShaderSrc;
		const GLchar *fShaderSrc = g_fShaderSrc;

		g_vShader = glCreateShader(GL_VERTEX_SHADER);
		g_fShader = glCreateShader(GL_FRAGMENT_SHADER);

		glShaderSource(g_vShader, 1, &vShaderSrc, &vShaderLen);
		glShaderSource(g_fShader, 1, &fShaderSrc, &fShaderLen);

		glCompileShader(g_vShader);
		if (!checkShaderCompilation(g_vShader))
			return false;
		glCompileShader(g_fShader);
		if (!checkShaderCompilation(g_fShader))
			return false;

		g_shaderProgram = glCreateProgram();

		glAttachShader(g_shaderProgram, g_vShader);
		glAttachShader(g_shaderProgram, g_fShader);

		glLinkProgram(g_shaderProgram);
		if (!checkShaderProgram(g_shaderProgram))
			return false;

		g_aPositionLoc = glGetAttribLocation(g_shaderProgram, "aPosition");
		g_aTexCoordLoc = glGetAttribLocation(g_shaderProgram, "aTexCoord");
		g_aColorLoc = glGetAttribLocation(g_shaderProgram, "aColor");
		g_aFogLoc = glGetAttribLocation(g_shaderProgram, "aFog");
		g_aMaskLoc = glGetAttribLocation(g_shaderProgram, "aMask");
		g_uMatrixLoc = glGetUniformLocation(g_shaderProgram, "uMatrix");

		g_uTextureEnabledLoc = glGetUniformLocation(g_shaderProgram, "uTextureEnabled");
		g_uFogEnabledLoc = glGetUniformLocation(g_shaderProgram, "uFogEnabled");
		g_uFogColorLoc = glGetUniformLocation(g_shaderProgram, "uFogColor");
		g_uAlphaCutoutLoc = glGetUniformLocation(g_shaderProgram, "uAlphaCutout");
		g_uPerPixelFogLoc = glGetUniformLocation(g_shaderProgram, "uPerPixelFog");

		glUseProgram(g_shaderProgram);
		glUniform1i(glGetUniformLocation(g_shaderProgram, "uTextureSampler"), 0);
#ifdef GL2_ENHANCED
		glUniform1i(glGetUniformLocation(g_shaderProgram, "uFogTable"), 1);
		glUniform1f(g_uPerPixelFogLoc, gfx.perPixelFog ? 1.0f : 0.0f);
#endif
		glUseProgram(0);
	}

#ifdef GL2_ENHANCED
	if (!postInit())
		return false;
#else
	{
		const GLint vShaderLen = sizeof(g_vShaderDispSrc) - 1;
		const GLint fShaderLen = sizeof(g_fShaderDispSrc) - 1;

		const GLchar *vShaderSrc = g_vShaderDispSrc;
		const GLchar *fShaderSrc = g_fShaderDispSrc;

		g_vShaderDisp = glCreateShader(GL_VERTEX_SHADER);
		g_fShaderDisp = glCreateShader(GL_FRAGMENT_SHADER);

		glShaderSource(g_vShaderDisp, 1, &vShaderSrc, &vShaderLen);
		glShaderSource(g_fShaderDisp, 1, &fShaderSrc, &fShaderLen);

		glCompileShader(g_vShaderDisp);
		if (!checkShaderCompilation(g_vShaderDisp))
			return false;
		glCompileShader(g_fShaderDisp);
		if (!checkShaderCompilation(g_fShaderDisp))
			return false;

		g_shaderProgramDisp = glCreateProgram();

		glAttachShader(g_shaderProgramDisp, g_vShaderDisp);
		glAttachShader(g_shaderProgramDisp, g_fShaderDisp);

		glLinkProgram(g_shaderProgramDisp);
		if (!checkShaderProgram(g_shaderProgramDisp))
			return false;

		g_aPositionLocDisp = glGetAttribLocation(g_shaderProgramDisp, "aPosition");
		g_aTexCoordLocDisp = glGetAttribLocation(g_shaderProgramDisp, "aTexCoord");

		g_uGammaLocDisp = glGetUniformLocation(g_shaderProgramDisp, "uGamma");

		glUseProgram(g_shaderProgramDisp);
		glUniform1i(glGetUniformLocation(g_shaderProgramDisp, "uTextureSampler"), 0);
		glUseProgram(0);
	}
#endif

	return true;
}

static void destroyFrameBuffer()
{
#ifdef GL2_ENHANCED
	postDestroyTargets();
	g_framebuffer = 0;
#else
	if (g_framebufferDepth != 0)
	{
		glDeleteRenderbuffers(1, &g_framebufferDepth);
		g_framebufferDepth = 0;
	}
	if (g_framebufferTexture != 0)
	{
		glDeleteTextures(1, &g_framebufferTexture);
		g_framebufferTexture = 0;
	}
	if (g_framebuffer != 0)
	{
		glDeleteFramebuffers(1, &g_framebuffer);
		g_framebuffer = 0;
	}
#endif
	g_framebufferHeight = 0;
	g_framebufferBaseEdge = 0;
}
static void createFrameBuffer()
{
	int32_t shorterEdge = fixedFramebufferSize ? SDL_min(initialWinWidth, initialWinHeight) : SDL_min(winWidth, winHeight);
	float virtualWidth = 640.0f;
#ifdef GL2_ENHANCED
	{
		const int32_t aw = fixedFramebufferSize ? initialWinWidth : winWidth;
		const int32_t ah = fixedFramebufferSize ? initialWinHeight : winHeight;
		const float aspect = ah > 0 ? (float)aw / (float)ah : 4.0f / 3.0f;
		if (gfx.widescreen && aspect > 4.0f / 3.0f + 0.01f)
			virtualWidth = 480.0f * SDL_min(aspect, 4.0f);
	}
#endif
	widescreenScale = 640.0f / virtualWidth;
	if (g_framebufferBaseEdge == shorterEdge && g_virtualWidth == virtualWidth)
		return;
	g_virtualWidth = virtualWidth;

#ifdef GL2_ENHANCED
	{
		GLint maxSize = 0, maxRbSize = 0;
		int32_t w, h;

		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
		glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRbSize);
		if (maxRbSize > 0 && maxRbSize < maxSize)
			maxSize = maxRbSize;
		if (maxSize <= 0)
			maxSize = 4096;

		h = (int32_t)(shorterEdge * gfx.renderScale + 0.5f);
		if (h < 120)
			h = 120;
		if (h * g_virtualWidth / 480.0f > maxSize)
			h = maxSize * 480.0f / g_virtualWidth;
		w = (int32_t)(h * g_virtualWidth / 480.0f + 0.5f);

		destroyFrameBuffer();
		g_framebuffer = postCreateTargets(w, h);
		g_framebufferHeight = h;
		g_framebufferBaseEdge = shorterEdge;
	}
#else
	{
		int32_t w = shorterEdge * 4 / 3;
		int32_t h = shorterEdge;

		destroyFrameBuffer();

		glGenFramebuffers(1, &g_framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);

		glGenTextures(1, &g_framebufferTexture);
		glBindTexture(GL_TEXTURE_2D, g_framebufferTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (!fixedFramebufferSize || !framebufferLinearFiltering) ? GL_NEAREST : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_framebufferTexture, 0);
		glBindTexture(GL_TEXTURE_2D, 0);

		glGenRenderbuffers(1, &g_framebufferDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, g_framebufferDepth);
#ifdef GLES2
		if (strstr((const char *)glGetString(GL_EXTENSIONS), "GL_OES_depth24") != NULL)
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24_OES, w, h);
		else
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
#else
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
#endif
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_framebufferDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			framebufferError = true;
			raise(SIGABRT);
		}

		g_framebufferHeight = shorterEdge;
		g_framebufferBaseEdge = shorterEdge;
	}
#endif
}

static void uploadFogTable()
{
#ifdef GL2_ENHANCED
	if (!g_fogTableTexture)
	{
		glGenTextures(1, &g_fogTableTexture);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, g_fogTableTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		g_fogTableDirty = true;
	}
	else
	{
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, g_fogTableTexture);
	}
	if (g_fogTableDirty)
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, 256, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, g_fogTable);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		g_fogTableDirty = false;
	}
	glActiveTexture(GL_TEXTURE0);
#endif
}

static void useGameProgram(BOOL gameProgram)
{
	if (gameProgram)
	{
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glEnable(GL_SCISSOR_TEST);
		glBlendFunc(GL_SRC_ALPHA, g_blendFuncDFactor);

		glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);

#ifdef GL2_ENHANCED
		glDisableVertexAttribArray(0);
#else
		glDisableVertexAttribArray(g_aPositionLocDisp);
		glDisableVertexAttribArray(g_aTexCoordLocDisp);
#endif

		glUseProgram(g_shaderProgram);

		glEnableVertexAttribArray(g_aPositionLoc);
		glEnableVertexAttribArray(g_aTexCoordLoc);
		glEnableVertexAttribArray(g_aColorLoc);
		glEnableVertexAttribArray(g_aFogLoc);
		if (g_aMaskLoc >= 0)
			glEnableVertexAttribArray(g_aMaskLoc);

		glVertexAttribPointer(g_aPositionLoc, 3, GL_FLOAT, GL_FALSE, 0, g_vertices);
		glVertexAttribPointer(g_aTexCoordLoc, 4, GL_FLOAT, GL_FALSE, 0, g_textureCoord);
		glVertexAttribPointer(g_aColorLoc, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, g_colorValues);
		glVertexAttribPointer(g_aFogLoc, 1, GL_UNSIGNED_BYTE, GL_TRUE, 0, g_fogCoord);
		if (g_aMaskLoc >= 0)
			glVertexAttribPointer(g_aMaskLoc, 1, GL_UNSIGNED_BYTE, GL_TRUE, 0, g_maskValues);

		uploadFogTable();
		glBindTexture(GL_TEXTURE_2D, g_boundTexture);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		glDisableVertexAttribArray(g_aPositionLoc);
		glDisableVertexAttribArray(g_aTexCoordLoc);
		glDisableVertexAttribArray(g_aColorLoc);
		glDisableVertexAttribArray(g_aFogLoc);
		if (g_aMaskLoc >= 0)
			glDisableVertexAttribArray(g_aMaskLoc);

#ifdef GL2_ENHANCED
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, g_verticesDisp);
		(void)g_textureCoordDisp;
#else
		glUseProgram(g_shaderProgramDisp);

		glEnableVertexAttribArray(g_aPositionLocDisp);
		glEnableVertexAttribArray(g_aTexCoordLocDisp);

		glVertexAttribPointer(g_aPositionLocDisp, 2, GL_FLOAT, GL_FALSE, 0, g_verticesDisp);
		glVertexAttribPointer(g_aTexCoordLocDisp, 2, GL_FLOAT, GL_FALSE, 0, g_textureCoordDisp);
#endif
	}
}

static void createContext()
{
	g_glCtx = SDL_GL_CreateContext(sdlWin);
	if (!g_glCtx)
	{
		contextError = true;
		raise(SIGABRT);
	}

#ifndef GLES2
	glGetShaderiv = SDL_GL_GetProcAddress("glGetShaderiv");
	glGetProgramiv = SDL_GL_GetProcAddress("glGetProgramiv");
	glGetShaderInfoLog = SDL_GL_GetProcAddress("glGetShaderInfoLog");
	glGetProgramInfoLog = SDL_GL_GetProcAddress("glGetProgramInfoLog");
	glCreateShader = SDL_GL_GetProcAddress("glCreateShader");
	glDeleteShader = SDL_GL_GetProcAddress("glDeleteShader");
	glShaderSource = SDL_GL_GetProcAddress("glShaderSource");
	glCompileShader = SDL_GL_GetProcAddress("glCompileShader");
	glCreateProgram = SDL_GL_GetProcAddress("glCreateProgram");
	glDeleteProgram = SDL_GL_GetProcAddress("glDeleteProgram");
	glAttachShader = SDL_GL_GetProcAddress("glAttachShader");
	glLinkProgram = SDL_GL_GetProcAddress("glLinkProgram");
	glGetAttribLocation = SDL_GL_GetProcAddress("glGetAttribLocation");
	glGetUniformLocation = SDL_GL_GetProcAddress("glGetUniformLocation");
	glUseProgram = SDL_GL_GetProcAddress("glUseProgram");
	glUniform1i = SDL_GL_GetProcAddress("glUniform1i");
	glUniformMatrix4fv = SDL_GL_GetProcAddress("glUniformMatrix4fv");
	glVertexAttribPointer = SDL_GL_GetProcAddress("glVertexAttribPointer");
	glUniform3f = SDL_GL_GetProcAddress("glUniform3f");
	glUniform1f = SDL_GL_GetProcAddress("glUniform1f");
	glEnableVertexAttribArray = SDL_GL_GetProcAddress("glEnableVertexAttribArray");
	glDisableVertexAttribArray = SDL_GL_GetProcAddress("glDisableVertexAttribArray");
	glGenFramebuffers = SDL_GL_GetProcAddress("glGenFramebuffers");
	glBindFramebuffer = SDL_GL_GetProcAddress("glBindFramebuffer");
	glFramebufferTexture2D = SDL_GL_GetProcAddress("glFramebufferTexture2D");
	glGenRenderbuffers = SDL_GL_GetProcAddress("glGenRenderbuffers");
	glBindRenderbuffer = SDL_GL_GetProcAddress("glBindRenderbuffer");
	glRenderbufferStorage = SDL_GL_GetProcAddress("glRenderbufferStorage");
	glFramebufferRenderbuffer = SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
	glCheckFramebufferStatus = SDL_GL_GetProcAddress("glCheckFramebufferStatus");
	glDeleteFramebuffers = SDL_GL_GetProcAddress("glDeleteFramebuffers");
	glDeleteRenderbuffers = SDL_GL_GetProcAddress("glDeleteRenderbuffers");

	glActiveTexture = SDL_GL_GetProcAddress("glActiveTexture");
	glUniform2f = SDL_GL_GetProcAddress("glUniform2f");
	glUniform4f = SDL_GL_GetProcAddress("glUniform4f");
	glGenBuffers = SDL_GL_GetProcAddress("glGenBuffers");
	glBindBuffer = SDL_GL_GetProcAddress("glBindBuffer");
	glBufferData = SDL_GL_GetProcAddress("glBufferData");
	glBindAttribLocation = SDL_GL_GetProcAddress("glBindAttribLocation");
	glDrawBuffers = SDL_GL_GetProcAddress("glDrawBuffers");
	glBlitFramebuffer = SDL_GL_GetProcAddress("glBlitFramebuffer");
	glRenderbufferStorageMultisample = SDL_GL_GetProcAddress("glRenderbufferStorageMultisample");

	BOOL ok = true;
	ok &= !!glGetShaderiv;
	ok &= !!glGetProgramiv;
	ok &= !!glGetShaderInfoLog;
	ok &= !!glGetProgramInfoLog;
	ok &= !!glCreateShader;
	ok &= !!glDeleteShader;
	ok &= !!glShaderSource;
	ok &= !!glCompileShader;
	ok &= !!glCreateProgram;
	ok &= !!glDeleteProgram;
	ok &= !!glAttachShader;
	ok &= !!glLinkProgram;
	ok &= !!glGetAttribLocation;
	ok &= !!glGetUniformLocation;
	ok &= !!glUseProgram;
	ok &= !!glUniform1i;
	ok &= !!glUniformMatrix4fv;
	ok &= !!glVertexAttribPointer;
	ok &= !!glUniform3f;
	ok &= !!glUniform1f;
	ok &= !!glEnableVertexAttribArray;
	ok &= !!glDisableVertexAttribArray;
	ok &= !!glGenFramebuffers;
	ok &= !!glBindFramebuffer;
	ok &= !!glFramebufferTexture2D;
	ok &= !!glGenRenderbuffers;
	ok &= !!glBindRenderbuffer;
	ok &= !!glRenderbufferStorage;
	ok &= !!glFramebufferRenderbuffer;
	ok &= !!glCheckFramebufferStatus;
	ok &= !!glDeleteRenderbuffers;
	ok &= !!glDeleteFramebuffers;
	ok &= !!glActiveTexture;
	ok &= !!glUniform2f;
	ok &= !!glUniform4f;
	ok &= !!glBindAttribLocation;
	if (!ok)
	{
		contextError = true;
		raise(SIGABRT);
	}
#endif

	if (vSync >= 0)
		SDL_GL_SetSwapInterval(vSync);

	glEnable(GL_SCISSOR_TEST);
	glDisable(GL_DITHER);

	glDepthFunc(GL_LEQUAL);

	if (!loadShaders())
	{
		shaderError = true;
		raise(SIGABRT);
	}
}
static void destroyContext()
{
	uint32_t i;

	glUseProgram(0);

	destroyFrameBuffer();

	texCacheClear();
	for (i = 0; i < (TextureMem >> 2); ++i)
	{
		TextureInfo *ti = &g_textures[i];
		ti->id = 0;
		ti->hash = 0;
	}
	g_boundTexture = 0;

	if (g_fogTableTexture)
	{
		glDeleteTextures(1, &g_fogTableTexture);
		g_fogTableTexture = 0;
	}

	glDeleteProgram(g_shaderProgram);
	glDeleteShader(g_fShader);
	glDeleteShader(g_vShader);

#ifdef GL2_ENHANCED
	postDestroy();
#else
	glDeleteProgram(g_shaderProgramDisp);
	glDeleteShader(g_fShaderDisp);
	glDeleteShader(g_vShaderDisp);
#endif

	SDL_GL_DeleteContext(g_glCtx);
	g_glCtx = NULL;

	g_trianglesCount = 0;
}

/* (Re)creates the GL texture of a texture slot. Paletted textures are created later in grTexSource (palette is needed). */
static void uploadTexture(TextureInfo *ti)
{
	ti->used = true;
	if (ti->fmt == GR_TEXFMT_P_8)
	{
		ti->palette = NULL;
		return;
	}
	texRelease(ti->hash);
	ti->hash = 0;
	ti->id = texAcquire(ti->data, ti->fmt, ti->size, NULL, &ti->hash, &ti->alphaMode);
	g_boundTexture = ti->id;
}

static void recreateContext()
{
	uint32_t i;

	destroyContext();

	createContext();

	// Restore textures
	for (i = 0; i < (TextureMem >> 2); ++i)
	{
		TextureInfo *ti = &g_textures[i];
		if (!ti->used)
			continue;
		uploadTexture(ti);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	g_boundTexture = 0;

	// Restore config
	{
		glDepthMask(g_depthMask);
		glBlendFunc(GL_SRC_ALPHA, g_blendFuncDFactor);

		glUseProgram(g_shaderProgram);
		glUniform1f(g_uTextureEnabledLoc, g_textureEnabled);
		glUniform1f(g_uFogEnabledLoc, g_fogEnabled);
		glUniform3f(g_uFogColorLoc, g_fogColor[0], g_fogColor[1], g_fogColor[2]);
		glUniform1f(g_uAlphaCutoutLoc, g_alphaCutout);
		glUseProgram(0);

#ifndef GL2_ENHANCED
		glUseProgram(g_shaderProgramDisp);
		glUniform1f(g_uGammaLocDisp, g_gammaValue);
		glUseProgram(0);
#endif
	}
}

static void drawTriangles()
{
	if (g_trianglesCount == 0)
		return;

#ifdef GL2_ENHANCED
	/* The game may change the fog table between draws (e.g. sky vs. world) */
	if (g_fogTableDirty && gfx.perPixelFog)
		uploadFogTable();
#endif

	glDrawArrays(GL_TRIANGLES, 0, g_trianglesCount * 3);
	g_trianglesCount = 0;
}

static inline void fogTableChanged()
{
	drawTriangles();
	g_fogTableDirty = true;
}

/* Optional statistics of submitted geometry (NFS2SE_GFX_STATS=1), used to tune the classification */
/*
 * Frame capture (Alt+D, for bug reports): the next frame's triangles as the game sends them (texture hash, render
 * state, clip window, vertices) go to "<settings>/screenshots/capture-<time>.txt", with every texture used, as the
 * renderer sees it (HD replacement if any), in "capture-<time>/<hash>.png".
 */
static FILE *g_capFile;
static int32_t g_capState; /* 0 - idle, 1 - starts with the next frame, 2 - capturing */
static char g_capDir[1024];
static struct { uint64_t hash; GLuint id; } g_capTex[1024];
static int32_t g_capTexCount;
static uint32_t g_capTriangles;

static void captureFrameStart()
{
	extern float dword_4D5294, flt_4D5298, flt_4D529C, flt_4D52A0;
	const char *dir = getSettingsDir();
	char stamp[32], path[1100];
	time_t now = time(NULL);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
	snprintf(g_capDir, sizeof g_capDir, "%sscreenshots/capture-%s", dir ? dir : "./", stamp);
	snprintf(path, sizeof path, "%s.txt", g_capDir);
	g_capFile = fopen(path, "w");
	g_capTexCount = 0;
	g_capTriangles = 0;
	g_capState = g_capFile ? 2 : 0;
	if (g_capFile)
		fprintf(g_capFile, "# virtualWidth=%g widescreenScale=%g track=%d proj=%g,%g,%g,%g\n# outcome tex texOn alphaMode blendDst depthMask fog clip(x0,y0,x1,y1) full seen3D | x y oow alpha (x3)\n",
			g_virtualWidth, widescreenScale, gfxCurrentTrack, dword_4D5294, flt_4D5298, flt_4D529C, flt_4D52A0);
#ifdef GL2_ENHANCED
	if (g_capFile)
	{
		const float *m = g_skyView.camToWorld;
		fprintf(g_capFile, "# camera pos=%g,%g,%g camToWorld rows=%g,%g,%g;%g,%g,%g;%g,%g,%g\n", g_skyView.camPos[0], g_skyView.camPos[1], g_skyView.camPos[2],
			m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
	}
#endif
}
static void captureTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c, const char *outcome)
{
	const GrVertex *v[3] = {a, b, c};
	int32_t i;
	const BOOL textured = g_textureEnabled > 0.5f;
	fprintf(g_capFile, "%s %016llX %d %d %s %d %d %g,%g,%g,%g %d %d |", outcome, textured ? (unsigned long long)g_boundHash : 0ULL,
		textured, g_boundAlphaMode, g_blendFuncDFactor == GL_ONE ? "add" : g_blendFuncDFactor == GL_ZERO ? "none" : "alpha",
		!!g_depthMask, g_fogEnabled > 0.5f, g_clipWindow[0], g_clipWindow[1], g_clipWindow[2], g_clipWindow[3], g_fullWidthView, g_seen3D);
	for (i = 0; i < 3; ++i)
		fprintf(g_capFile, " %.1f %.1f %.6g %.0f", v[i]->x - VertexSnap, v[i]->y - VertexSnap, v[i]->oow, v[i]->a);
	fputc('\n', g_capFile);
	++g_capTriangles;
	if (textured && g_boundTexture)
	{
		for (i = 0; i < g_capTexCount; ++i)
			if (g_capTex[i].hash == g_boundHash)
				return;
		if (g_capTexCount < (int32_t)(sizeof g_capTex / sizeof *g_capTex))
		{
			g_capTex[g_capTexCount].hash = g_boundHash;
			g_capTex[g_capTexCount].id = g_boundTexture;
			++g_capTexCount;
		}
	}
}
static void captureFrameEnd()
{
	int32_t i;
	fclose(g_capFile);
	g_capFile = NULL;
	g_capState = 0;
#ifndef GLES2
#ifdef _WIN32
	mkdir(g_capDir);
#else
	mkdir(g_capDir, 0755);
#endif
	for (i = 0; i < g_capTexCount; ++i)
	{
		GLint w = 0, h = 0;
		uint8_t *pixels;
		char path[1200];
		glBindTexture(GL_TEXTURE_2D, g_capTex[i].id);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		if (w <= 0 || h <= 0 || !(pixels = (uint8_t *)malloc(w * h * 4)))
			continue;
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		snprintf(path, sizeof path, "%s/%016llX.png", g_capDir, (unsigned long long)g_capTex[i].hash);
		stbi_write_png(path, w, h, 4, pixels, w * 4);
		free(pixels);
	}
	glBindTexture(GL_TEXTURE_2D, g_boundTexture);
#endif
	fprintf(stderr, "Frame capture saved: %s.txt (%u triangles, %d textures)\n", g_capDir, g_capTriangles, g_capTexCount);
}

static int32_t g_statsEnabled = -1;
static struct
{
	uint32_t count;
	float oowMin, oowMax;
	uint32_t depthMask, textured, additive;
} g_stats[2][2];
static float g_statsSkyMinX = 1e9f, g_statsSkyMaxX = -1e9f;
static uint32_t g_statsSkyCount;
static void statsTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
	const BOOL fog = g_fogEnabled > 0.5f;
	const BOOL flat = a->oow == b->oow && b->oow == c->oow;
	float lo = SDL_min(a->oow, SDL_min(b->oow, c->oow)), hi = SDL_max(a->oow, SDL_max(b->oow, c->oow));
	if (g_stats[fog][flat].count == 0)
	{
		g_stats[fog][flat].oowMin = lo;
		g_stats[fog][flat].oowMax = hi;
	}
	g_stats[fog][flat].oowMin = SDL_min(g_stats[fog][flat].oowMin, lo);
	g_stats[fog][flat].oowMax = SDL_max(g_stats[fog][flat].oowMax, hi);
	g_stats[fog][flat].count++;
	g_stats[fog][flat].depthMask += !!g_depthMask;
	g_stats[fog][flat].textured += g_textureEnabled > 0.5f;
	g_stats[fog][flat].additive += g_blendFuncDFactor == GL_ONE;
	if (flat && !g_seen3D && g_prevFrameHas3D)
	{
		/* Sky backdrop: X extent in game coordinates */
		const float ax = a->x - VertexSnap, bx = b->x - VertexSnap, cx = c->x - VertexSnap;
		g_statsSkyMinX = SDL_min(g_statsSkyMinX, SDL_min(ax, SDL_min(bx, cx)));
		g_statsSkyMaxX = SDL_max(g_statsSkyMaxX, SDL_max(ax, SDL_max(bx, cx)));
		g_statsSkyCount++;
	}
}
static void statsFrame()
{
	static uint32_t frame;
	uint32_t fog, flat;
	if (++frame % 120 != 0)
	{
		return;
	}
	for (fog = 0; fog < 2; ++fog)
		for (flat = 0; flat < 2; ++flat)
			if (g_stats[fog][flat].count)
				fprintf(stderr, "stats fog=%u flat=%u: %u tris/120f oow=[%g, %g] depthWrite=%u textured=%u additive=%u\n",
					fog, flat, g_stats[fog][flat].count, g_stats[fog][flat].oowMin, g_stats[fog][flat].oowMax,
					g_stats[fog][flat].depthMask, g_stats[fog][flat].textured, g_stats[fog][flat].additive);
	if (g_statsSkyCount)
		fprintf(stderr, "stats sky: %u tris/120f x=[%g, %g] widescreenScale=%g virtualWidth=%g\n", g_statsSkyCount, g_statsSkyMinX, g_statsSkyMaxX, widescreenScale, g_virtualWidth);
	g_statsSkyMinX = 1e9f;
	g_statsSkyMaxX = -1e9f;
	g_statsSkyCount = 0;
	memset(g_stats, 0, sizeof g_stats);
}

/**/

REALIGN STDCALL void grAlphaBlendFunction(GrAlphaBlendFnc_t rgb_sf, GrAlphaBlendFnc_t rgb_df, GrAlphaBlendFnc_t alpha_sf, GrAlphaBlendFnc_t alpha_df)
{
	drawTriangles();
	switch (rgb_df)
	{
		case GR_BLEND_ONE:
			g_blendFuncDFactor = GL_ONE;
			break;
		case GR_BLEND_ONE_MINUS_SRC_ALPHA:
			g_blendFuncDFactor = GL_ONE_MINUS_SRC_ALPHA;
			break;
	}
	glBlendFunc(GL_SRC_ALPHA, g_blendFuncDFactor);
// 	fprintf(stderr, "grAlphaBlendFunction: %d %d %d %d\n", rgb_sf, rgb_df, alpha_sf, alpha_df);
}
REALIGN STDCALL void grAlphaCombine(GrCombineFunction_t function, GrCombineFactor_t factor, GrCombineLocal_t local, GrCombineOther_t other, BOOL invert)
{
	drawTriangles();
	if (other == GR_COMBINE_OTHER_TEXTURE)
		g_textureEnabled = 1.0f;
	else
		g_textureEnabled = 0.0f;
	glUniform1f(g_uTextureEnabledLoc, g_textureEnabled);
// 	fprintf(stderr, "grAlphaCombine: %d\n", (other == GR_COMBINE_OTHER_TEXTURE));
}
REALIGN STDCALL void grAlphaTestFunction(GrCmpFnc_t function)
{
// 	fprintf(stderr, "grAlphaTestFunction: %X\n", function);
}
REALIGN STDCALL void grAlphaTestReferenceValue(GrAlpha_t value)
{
// 	fprintf(stderr, "grAlphaTestReferenceValue: %f\n", value / 255.0f);
}
REALIGN STDCALL void grClipWindow(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
	float ratio = g_framebufferHeight / 480.0f;

	/* Full-width views are stretched over the whole virtual width, other windows stay 4:3 in the middle */
	float offsetX = (g_virtualWidth - 640.0f) * 0.5f;
	float virtualMinX = minX + offsetX, virtualMaxX = maxX + offsetX;
	g_fullWidthView = (minX == 0 && maxX >= 640);
	g_clipWindow[0] = minX;
	g_clipWindow[1] = minY;
	g_clipWindow[2] = SDL_min(maxX, 640);
	g_clipWindow[3] = maxY;
	if (g_fullWidthView)
	{
		virtualMinX = 0.0f;
		virtualMaxX = g_virtualWidth;
	}
	else
	{
		/* A clip window of a corner HUD element (minimap) moves with it */
		const float shift = hudShift(minX, SDL_min(maxX, 640), minY);
		virtualMinX += shift;
		virtualMaxX += shift;
	}

	g_clipRectUV[0] = virtualMinX / g_virtualWidth;
	g_clipRectUV[1] = 1.0f - maxY / 480.0f;
	g_clipRectUV[2] = virtualMaxX / g_virtualWidth;
	g_clipRectUV[3] = 1.0f - minY / 480.0f;

	int32_t scaledMinX = virtualMinX * ratio;
	int32_t scaledMinY = minY * ratio;
	int32_t scaledMaxX = virtualMaxX * ratio + 0.5f;
	int32_t scaledMaxY = maxY * ratio + 0.5f;

// 	fprintf(stderr, "grClipWindow: %d %d %d %d [%d]\n", minX, minY, maxX, maxY, g_trianglesCount);

	drawTriangles();

	glViewport(scaledMinX, g_framebufferHeight - scaledMaxY, scaledMaxX - scaledMinX, scaledMaxY - scaledMinY);
	glScissor (scaledMinX, g_framebufferHeight - scaledMaxY, scaledMaxX - scaledMinX, scaledMaxY - scaledMinY);

	glLineWidth(2.0f * ratio);

	matrixLoadIdentity();
	matrixOrtho(scaledMinX, scaledMaxX, scaledMaxY, scaledMinY, Near, Far);
	matrixScale2(ratio, ratio);
	glUniformMatrix4fv(g_uMatrixLoc, 1, GL_FALSE, g_matrix);
}
REALIGN STDCALL void grBufferClear(GrColor_t color, GrAlpha_t alpha, uint16_t depth)
{
	float r, g , b, a;
	convertColor(color, &alpha, &r, &g, &b, &a);

// 	fprintf(stderr, "grBufferClear: %X %X %X [%d]\n", color, alpha, depth, g_trianglesCount);

	drawTriangles();

	glClearColor(r, g, b, a);
#ifdef GLES2
	glClearDepthf(depth / 65535.0f);
#else
	glClearDepth(depth / 65535.0f);
#endif

	glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

	/* A new view starts (e.g. rear-view mirror): its backdrop belongs to the world again */
	g_seen3D = false;

#ifdef GL2_ENHANCED
	if (glDrawBuffers)
	{
		/* Cleared background belongs to the world (sky); green marks car models */
		GLenum maskBuffer = GL_COLOR_ATTACHMENT1;
		GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
		glDrawBuffers(1, &maskBuffer);
		glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawBuffers(2, buffers);
	}
#endif
}
REALIGN STDCALL void grChromakeyMode(GrChromakeyMode_t mode)
{
// 	fprintf(stderr, "grChromakeyMode: %X\n", mode);
// 	chromaKeyEnabled = mode;
}
REALIGN STDCALL void grChromakeyValue(GrColor_t value)
{
// 	fprintf(stderr, "grChromakeyValue: %X\n", value);
// 	chromaKeyValue = value;//(value & 0x0000FF00) | ((value & 0x00FF0000) >> 16) | ((value & 0x000000FF) << 16);
}
REALIGN STDCALL void grBufferSwap(int swap_interval)
{
	carModelsDraw(false);
#ifdef GL2_ENHANCED
	carHeadlightFrame();
	if (g_skyReplacing)
		skySetFog(false);
	g_skyReplacing = false;
#endif
// 	fprintf(stderr, "grBufferSwap: [%d]\n", g_trianglesCount);

	drawTriangles();

	if (g_statsEnabled > 0)
		statsFrame();
	if (g_capState == 2)
		captureFrameEnd();
	else if (g_capState == 1)
		captureFrameStart();
	if (gfxCaptureRequest)
	{
		gfxCaptureRequest = false;
		g_capState = 1;
	}

	GLint viewportSize[4] = {};
	glGetIntegerv(GL_VIEWPORT, viewportSize);

	GLint scissorBox[4] = {};
	glGetIntegerv(GL_SCISSOR_BOX, scissorBox);

	useGameProgram(false);

	int32_t xOffset = 0, yOffset = 0;
	int32_t visibleWidth = 0, visibleHeight = 0;
	float widthRatio  = winWidth  / g_virtualWidth;
	float heightRatio = winHeight / 480.0f;

	if (keepAspectRatio)
	{
		if (widthRatio > heightRatio)
			widthRatio = heightRatio;
		else if (heightRatio > widthRatio)
			heightRatio = widthRatio;

		xOffset = winWidth  / 2 - widthRatio  * g_virtualWidth * 0.5f;
		yOffset = winHeight / 2 - heightRatio * 240;

		visibleWidth  = g_virtualWidth * widthRatio  + 0.5f;
		visibleHeight = 480 * heightRatio + 0.5f;
	}

	int32_t scaledMaxX = g_virtualWidth * widthRatio  + 0.5f;
	int32_t scaledMaxY = 480.0f * heightRatio + 0.5f;

#ifdef GL2_ENHANCED
	if (g_frameHas3D)
	{
		/* Dozens of streaks per frame mean rain, ramp the road wetness over a couple of seconds */
		g_rainLevel += ((g_frameRainTriangles >= 30 ? 1.0f : 0.0f) - g_rainLevel) * 0.015f;
	}
	g_frameRainTriangles = 0;
	postPresent(xOffset, winHeight - scaledMaxY - yOffset, scaledMaxX, scaledMaxY, xOffset, yOffset, visibleWidth, visibleHeight, g_gammaValue, g_frameHas3D);
	g_prevFrameHas3D = g_frameHas3D;
	g_frameHas3D = false;
	g_frameSubView = false;
	g_seen3D = false;
	glActiveTexture(GL_TEXTURE0);
#else
	clearUnusedArea(xOffset, yOffset, visibleWidth, visibleHeight);

	glViewport(xOffset, winHeight - scaledMaxY - yOffset, scaledMaxX, scaledMaxY);
	glScissor (xOffset, winHeight - scaledMaxY - yOffset, scaledMaxX, scaledMaxY);

	glBindTexture(GL_TEXTURE_2D, g_framebufferTexture);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
#endif

	SDL_GL_SwapWindow(sdlWin);

	if (needRecreateGl)
	{
		recreateContext();
		needRecreateGl = false;
		windowResized = true;
	}

	if (windowResized)
	{
		createFrameBuffer();
		useGameProgram(true);
		grClipWindow(0, 0, 640, 480);
		windowResized = false;
	}
	else
	{
		useGameProgram(true);
		glViewport(viewportSize[0], viewportSize[1], viewportSize[2], viewportSize[3]);
		glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
	}
}
REALIGN STDCALL void grColorCombine(GrCombineFunction_t function, GrCombineFactor_t factor, GrCombineLocal_t local, GrCombineOther_t other, BOOL invert)
{
// 	fprintf(stderr, "grColorCombine\n");
}
REALIGN STDCALL void grCullMode(GrCullMode_t mode)
{
// 	fprintf(stderr, "grCullMode: %d\n", mode);
}
REALIGN STDCALL void grDepthBiasLevel(int16_t level)
{
// 	fprintf(stderr, "grDepthBiasLevel: %d\n", level);
}
REALIGN STDCALL void grDepthBufferFunction(GrCmpFnc_t function)
{
// 	drawTriangles();
// 	glDepthFunc(GL_NEVER + function);
// 	fprintf(stderr, "grDepthBufferFunction: %X\n", function);
}
REALIGN STDCALL void grDepthBufferMode(GrDepthBufferMode_t mode)
{
// 	fprintf(stderr, "grDepthBufferMode: %X\n", mode);
}
REALIGN STDCALL void grDepthMask(BOOL mask)
{
	drawTriangles();

	g_depthMask = mask;
	glDepthMask(g_depthMask);
// 	fprintf(stderr, "grDepthMask: %d [%d]\n", mask, g_trianglesCount);
}
REALIGN STDCALL void grDitherMode(GrDitherMode_t mode)
{
// 	fprintf(stderr, "grDitherMode: %d\n", mode);
}

/*
 * Classifies geometry for post-processing: 3D world (255) or 2D overlay such as HUD, menus and text (0).
 * NFS2SE keeps table fog enabled for everything and draws all 2D elements flat at 1/w == 1.0,
 * while 3D geometry never comes that close (1/w <= ~0.62 in races). Flat 1/w == 1 geometry drawn
 * before any 3D geometry of a view (since frame start or buffer clear) is the sky backdrop (world),
 * after it - HUD and other overlays.
 */
enum
{
	MapOverlay,  /* 4:3 in the middle of the screen */
	MapWorld,    /* Stretched over the whole virtual width (the game squeezed it) */
	MapBackdrop, /* Sky backdrop: stretched, and widened to cover the screen edges */
};

static inline uint8_t worldMask(const GrVertex *a, const GrVertex *b, const GrVertex *c, int32_t *mapping)
{
	if (a->oow >= 0.999f && b->oow >= 0.999f && c->oow >= 0.999f)
	{
		/* Sky backdrop of a race frame is stretched with the 3D view, overlays and menus are not */
		*mapping = (!g_seen3D && g_prevFrameHas3D && g_fullWidthView) ? MapBackdrop : MapOverlay;
#ifdef GL2_ENHANCED
		/* 2D after the 3D view (HUD, pause menu): car models still pending belong under it */
		if (g_seen3D && g_carDrawCount)
			carModelsDraw(false);
#endif
		return g_seen3D ? 0 : 255;
	}
#ifdef GL2_ENHANCED
	if (!g_seen3D)
	{
		/* After the sky backdrop, before the first 3D triangle of the view: the sky, then opaque car models */
		skyDrawPending();
		skyProcessGradient();
		terrainQueue();
		carModelsDraw(true);
	}
#endif
	g_seen3D = g_frameHas3D = true;
	*mapping = g_fullWidthView ? MapWorld : MapOverlay;
	return 255;
}

/*
 * HUD in the corners (HudCorners): in a race on a wide screen, 2D elements entirely in the bottom left or bottom right
 * corner of the 4:3 layout (record box; minimap with its clip window, name box) move to the screen edges. Returns the
 * shift in virtual units for an element with this bounding box (game units).
 */
static inline float hudShift(float minX, float maxX, float minY)
{
	const float offset = (g_virtualWidth - 640.0f) * 0.5f;
	if (!gfx.hudCorners || offset <= 0.0f || !g_prevFrameHas3D || minY < 300.0f)
		return 0.0f;
	if (maxX <= 140.0f)
		return -offset;
	if (minX >= 480.0f)
		return offset;
	return 0.0f;
}

/* Maps game X (640 units) to the virtual screen */
static inline float mapX(float x, int32_t mapping)
{
	const float center = g_virtualWidth * 0.5f;
	switch (mapping)
	{
		case MapWorld:
			return x * (g_virtualWidth / 640.0f);
		case MapBackdrop:
			/*
			 * The game projects the sky as a flat strip spanning +-1.5 focal lengths (+-480 units at 4:3),
			 * which doesn't reach the edges of wider screens - widen it just enough.
			 */
			return center + (x * (g_virtualWidth / 640.0f) - center) * SDL_max(1.0f, center / 470.0f);
	}
	return x + (g_virtualWidth - 640.0f) * 0.5f;
}

REALIGN STDCALL void grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
	if (g_statsEnabled < 0)
		g_statsEnabled = getenv("NFS2SE_GFX_STATS") != NULL;
	if (g_statsEnabled)
		statsTriangle(a, b, c);

// 	fprintf(stderr, "grDrawTriangle\n");
	const GrVertex *grVertices[3] = {a, b, c};
	/*
	 * Per-vertex fog index is (uint16_t)w, which wraps around for w >= 65536 (e.g. the sky dome gets no fog).
	 * The game's look depends on it, so such triangles keep per-vertex fog instead of the per-pixel table lookup.
	 */
	const BOOL beyondFogTable = a->oow < 1.0f / 65535.0f || b->oow < 1.0f / 65535.0f || c->oow < 1.0f / 65535.0f;
	int32_t mapping;
#ifdef GL2_ENHANCED
	/*
	 * Sky backdrop (flat, before the 3D geometry of a race view): with a sky panorama, it is drawn at the first
	 * backdrop triangle and only the backdrop's terrain (textures with transparent sky) is kept over it.
	 */
	if (!g_seen3D && g_prevFrameHas3D && a->oow >= 0.999f && b->oow >= 0.999f && c->oow >= 0.999f)
	{
		skyDrawPending();
		if (g_textureEnabled < 0.5f)
			skyObserveGradient(a, b, c);
		if (g_skyReplacing && (g_textureEnabled < 0.5f || g_boundAlphaMode == AlphaOpaque))
		{
			if (g_capFile)
				captureTriangle(a, b, c, "backdrop-replaced");
			return;
		}
	}
	/* Game sky layers replaced by the panorama */
	if (g_textureEnabled > 0.5f && skyHides(g_boundHash))
	{
		if (g_capFile)
			captureTriangle(a, b, c, "sky-hidden");
		return;
	}
	/* The game's stand-ins for distant terrain are hidden while the real terrain is drawn */
	if (terrainHidesTriangle(a, b, c))
	{
		if (g_capFile)
			captureTriangle(a, b, c, "terrain-hidden");
		return;
	}
#endif
	const uint8_t world = worldMask(a, b, c, &mapping);
	if (g_capFile)
		captureTriangle(a, b, c, mapping == MapWorld ? (world ? "world" : "world-nomask") : mapping == MapBackdrop ? "backdrop" : "overlay");
#ifdef GL2_ENHANCED
	if (world && g_blendFuncDFactor == GL_ONE && a->oow == b->oow && b->oow == c->oow && a->oow < 0.999f)
		++g_frameRainTriangles;
	if (world && !g_fullWidthView && !g_frameSubView)
	{
		memcpy(g_subViewRect, g_clipRectUV, sizeof g_subViewRect);
		g_frameSubView = true;
	}
#endif
	const uint8_t mask = (world && beyondFogTable) ? world - 1 : world;
	float shift = 0.0f;
	uint32_t i;
	if (mapping == MapOverlay)
	{
		const float ax = a->x - VertexSnap, bx = b->x - VertexSnap, cx = c->x - VertexSnap;
		shift = hudShift(SDL_min(ax, SDL_min(bx, cx)), SDL_max(ax, SDL_max(bx, cx)), SDL_min(a->y, SDL_min(b->y, c->y)) - VertexSnap);
	}
	for (i = 0; i < 3; ++i)
	{
		const GrVertex *grVertex = grVertices[i];

		g_colorValues[g_trianglesCount].vertex[i].r = grVertex->r;
		g_colorValues[g_trianglesCount].vertex[i].g = grVertex->g;
		g_colorValues[g_trianglesCount].vertex[i].b = grVertex->b;
		g_colorValues[g_trianglesCount].vertex[i].a = grVertex->a;

		g_fogCoord[g_trianglesCount].vertex[i] = 255 - g_fogTable[(uint16_t)(1.0f / grVertex->oow)];
		g_maskValues[g_trianglesCount].vertex[i] = mask;

		g_textureCoord[g_trianglesCount].vertex[i].s = grVertex->tmuvtx[0].sow / 256.0f;
		g_textureCoord[g_trianglesCount].vertex[i].t = grVertex->tmuvtx[0].tow / 256.0f;
		g_textureCoord[g_trianglesCount].vertex[i].q = grVertex->oow;

		g_vertices[g_trianglesCount].vertex[i].x = mapX(grVertex->x - VertexSnap, mapping) + shift;
		g_vertices[g_trianglesCount].vertex[i].y = grVertex->y - VertexSnap;
		g_vertices[g_trianglesCount].vertex[i].z = grVertex->oow;
	}
	if (++g_trianglesCount >= MaxTriangles)
	{
		drawTriangles();
// 		fprintf(stderr, "Too many triangles!\n");
	}
}
REALIGN STDCALL void grDrawLine(const GrVertex *a, const GrVertex *b)
{
//	fprintf(stderr, "grDrawLine: [%d]\n", g_trianglesCount);
	drawTriangles();

	const GrVertex *grVertices[2] = {a, b};
	const BOOL flat = a->oow >= 0.999f && b->oow >= 0.999f;
	const int32_t mapping = (g_fullWidthView && !(flat && g_seen3D)) ? MapWorld : MapOverlay;
	const float shift = mapping == MapOverlay ? hudShift(SDL_min(a->x, b->x) - VertexSnap, SDL_max(a->x, b->x) - VertexSnap, SDL_min(a->y, b->y) - VertexSnap) : 0.0f;
	uint32_t i;
	for (i = 0; i < 2; ++i)
	{
		g_colorValues->vertex[i].r = grVertices[i]->r;
		g_colorValues->vertex[i].g = grVertices[i]->g;
		g_colorValues->vertex[i].b = grVertices[i]->b;
		g_colorValues->vertex[i].a = grVertices[i]->a;

		g_maskValues->vertex[i] = (grVertices[i]->oow >= 0.999f && g_seen3D) ? 0 : 255;

		g_vertices->vertex[i].x = mapX(grVertices[i]->x - VertexSnap, mapping) + shift;
		g_vertices->vertex[i].y = grVertices[i]->y - VertexSnap;
		g_vertices->vertex[i].z = grVertices[i]->oow;
	}
	glDrawArrays(GL_LINES, 0, 2);
}
REALIGN STDCALL void grFogColorValue(GrColor_t fogcolor)
{
	drawTriangles();

	convertColor(fogcolor, NULL, &g_fogColor[0], &g_fogColor[1], &g_fogColor[2], NULL);
	glUniform3f(g_uFogColorLoc, g_fogColor[0], g_fogColor[1], g_fogColor[2]);

// 	fprintf(stderr, "grFogColorValue: 0x%.8X [%d]\n", fogcolor, g_trianglesCount);
}
REALIGN STDCALL void grFogMode(GrFogMode_t mode)
{
	drawTriangles();
	switch (mode)
	{
		case GR_FOG_DISABLE:
			g_fogEnabled = 0.0f;
			break;
		case GR_FOG_WITH_TABLE:
			g_fogEnabled = 1.0f;
			break;
	}
	glUniform1f(g_uFogEnabledLoc, g_fogEnabled);
#ifdef GL2_ENHANCED
	if (g_fogEnabled > 0.5f && g_fogTableDirty)
		uploadFogTable();
#endif
// 	fprintf(stderr, "grFogMode: %X\n", mode);
}
REALIGN STDCALL void grGammaCorrectionValue(float value)
{
	if (g_gammaValue == value)
		return;

	g_gammaValue = value;

#ifndef GL2_ENHANCED
	useGameProgram(false);
	glUniform1f(g_uGammaLocDisp, g_gammaValue);
	useGameProgram(true);
#endif
}
REALIGN STDCALL void grGlideInit(void)
{
// 	fprintf(stderr, "grGlideInit\n");
}
REALIGN STDCALL void grGlideShutdown(void)
{
	destroyContext();
	g_palette = NULL;

// 	fprintf(stderr, "grGlideShutdown\n");
}
REALIGN STDCALL BOOL grLfbLock(GrLock_t type, GrBuffer_t buffer, GrLfbWriteMode_t writeMode, GrOriginLocation_t origin, BOOL pixelPipeline, GrLfbInfo_t *info)
{
	//TODO Remove this
	memset(info, 0, sizeof(GrLfbInfo_t));
	if (type == GR_LFB_WRITE_ONLY)
	{
		info->lfbPtr = g_lfb = (uint8_t *)malloc(640*480*2);
		info->strideInBytes = 2;
		return true;
	}
	return false;
}
REALIGN STDCALL BOOL grLfbUnlock(GrLock_t type, GrBuffer_t buffer)
{
	//TODO Remove this
	free(g_lfb);
	g_lfb = NULL;
	return true;
}
REALIGN STDCALL void grRenderBuffer(GrBuffer_t buffer)
{
// 	fprintf(stderr, "grRenderBuffer\n");
}
REALIGN STDCALL void grSstIdle(void)
{
// 	fprintf(stderr, "grSstIdle\n");
}
REALIGN STDCALL BOOL grSstIsBusy(void)
{
// 	fprintf(stderr, "grSstIsBusy\n");
	return false;
}
REALIGN STDCALL BOOL grSstQueryHardware(GrHwConfiguration *hwconfig)
{
	return true;
}
REALIGN STDCALL void grSstSelect(int which_sst)
{
// 	fprintf(stderr, "grSstSelect: %d\n", which_sst);
}
REALIGN STDCALL uint32_t grSstStatus(void)
{
// 	fprintf(stderr, "grSstStatus\n");
	return 0x0FFFF03F;
}
REALIGN STDCALL void grSstWinClose(void)
{
// 	fprintf(stderr, "grSstWinClose\n");
}
REALIGN STDCALL BOOL grSstWinOpen(uint32_t hWnd, GrScreenResolution_t screen_resolution, GrScreenRefresh_t refresh_rate, GrColorFormat_t color_format, GrOriginLocation_t origin_location, int nColBuffers, int nAuxBuffers)
{
	createContext();
	handleDpr();
	createFrameBuffer();
	useGameProgram(true);

	grClipWindow(0, 0, 640, 480);

	return true;
}
REALIGN STDCALL uint32_t grTexCalcMemRequired(GrLOD_t lodmin, GrLOD_t lodmax, GrAspectRatio_t aspect, GrTextureFormat_t fmt)
{
	uint32_t size = 256 >> lodmax;
	size *= size;
	switch (fmt)
	{
		case GR_TEXFMT_P_8:
			break;
		case GR_TEXFMT_RGB_565:
		case GR_TEXFMT_ARGB_1555:
		case GR_TEXFMT_ARGB_4444:
			size <<= 1;
			break;
	}
	return size;
}
REALIGN STDCALL void grTexClampMode(GrChipID_t tmu, GrTextureClampMode_t s_clampmode, GrTextureClampMode_t t_clampmode)
{
}
REALIGN STDCALL void grTexCombine(GrChipID_t tmu, GrCombineFunction_t rgb_function, GrCombineFactor_t rgb_factor, GrCombineFunction_t alpha_function, GrCombineFactor_t alpha_factor, BOOL rgb_invert, BOOL alpha_invert)
{
// 	fprintf(stderr, "grTexCombine\n");
}
REALIGN STDCALL void grTexCombineFunction(GrChipID_t tmu, GrTextureCombineFnc_t fnc)
{
// 	fprintf(stderr, "grTexCombineFunction\n");
}
REALIGN STDCALL void grTexDownloadMipMap(GrChipID_t tmu, uint32_t startAddress, uint32_t evenOdd, GrTexInfo *info)
{
	TextureInfo *ti = &g_textures[startAddress >> 2];
	ti->data = &g_textureMem[startAddress];
	ti->palette = NULL;
	ti->fmt = info->format;
	ti->size = 256 >> info->largeLod;

	drawTriangles();

	/* Keep the original data, conversion to RGBA is done by the texture pipeline */
	memcpy(ti->data, info->data, ti->size * ti->size * (ti->fmt == GR_TEXFMT_P_8 ? 1 : 2));

	uploadTexture(ti);

//	fprintf(stderr, "grTexDownloadMipMap: 0x%.8X %d %u %u\n", startAddress, ti->fmt, ti->size, ti->id);
}
REALIGN STDCALL void grTexDownloadTable(GrChipID_t tmu, GrTexTable_t type, void *data)
{
	if (type == GR_TEXTABLE_PALETTE)
		g_palette = (uint32_t *)data;
}
REALIGN STDCALL void grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t minfilter_mode, GrTextureFilterMode_t magfilter_mode)
{
	/* Always linear */
// 	fprintf(stderr, "grTexFilterMode: %d\n", minfilter_mode);
}
REALIGN STDCALL uint32_t grTexMaxAddress(GrChipID_t tmu)
{
	return TextureMem;
}
REALIGN STDCALL uint32_t grTexMinAddress(GrChipID_t tmu)
{
	return 0;
}
REALIGN STDCALL void grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, BOOL lodBlend)
{
	/* mode = 0 */
}
REALIGN STDCALL void grTexSource(GrChipID_t tmu, uint32_t startAddress, uint32_t evenOdd, GrTexInfo *info)
{
	TextureInfo *ti = &g_textures[startAddress >> 2];
	float alphaCutout;
	drawTriangles();
	if (info->format == GR_TEXFMT_P_8 && g_palette && ti->palette != g_palette && ti->data)
	{
		// Update only when palette or texture changes (let's assume every palette has different pointer)
		// When texture changes, palette is NULL
		texRelease(ti->hash);
		ti->hash = 0;
		ti->id = texAcquire(ti->data, GR_TEXFMT_P_8, 256 >> info->largeLod, g_palette, &ti->hash, &ti->alphaMode);
		ti->palette = g_palette;
	}
	g_boundTexture = ti->id;
	g_boundAlphaMode = ti->alphaMode;
	g_boundHash = ti->hash;
	glBindTexture(GL_TEXTURE_2D, g_boundTexture);

	alphaCutout = (ti->alphaMode == AlphaCutout) ? 1.0f : 0.0f;
	if (alphaCutout != g_alphaCutout)
	{
		g_alphaCutout = alphaCutout;
		glUniform1f(g_uAlphaCutoutLoc, g_alphaCutout);
	}
}
