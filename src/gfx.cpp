// Renderer. Each frame the game's world pass is redirected into our framebuffer with three colour
// targets: 0 = scene (exactly what the game draws, but RGBA16F so additive glows can exceed 1),
// 1 = emissive rgb (light-emitting sprites add their colour, everything else darkens it by its alpha)
// + playfield coverage in alpha, 2 = solid (occluder) coverage in alpha. Target 0's blend state is
// never touched, so the game's own state queries and rendering are unaffected.
// At the first draw under the HUD projection we post-process the world into the back buffer:
// ray-traced 2D lighting (radiance cascades over a jump-flood distance field), lit height fog,
// bloom, tonemap, grade, FXAA, sharpen, vignette, grain. The HUD then draws on top untouched.
// Settings live in <game>\FortsGFX.ini.
#include "common.h"
#include "shaders.h"
#include <mmsystem.h>
#include <wincodec.h>
#include <algorithm>
#include <map>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

enum : GLenum {
    GL_ZERO = 0, GL_ONE = 1, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_COLOR = 0x1800, GL_COLOR_BUFFER_BIT = 0x4000, GL_FLOAT = 0x1406, GL_HALF_FLOAT = 0x140B,
    GL_RGBA8 = 0x8058, GL_RGBA16F = 0x881A, GL_RGBA32F = 0x8814, GL_R16F = 0x822D, GL_RED_FMT = 0x1903, GL_LINEAR = 0x2601, GL_CLAMP_TO_EDGE = 0x812F,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_COLOR_ATTACHMENT1 = 0x8CE1, GL_COLOR_ATTACHMENT2 = 0x8CE2, GL_COLOR_ATTACHMENT3 = 0x8CE3,
    GL_ALPHA_TEST_FUNC = 0x0BC1, GL_ALPHA_TEST_REF = 0x0BC2, GL_SHADE_MODEL = 0x0B54, GL_FLAT = 0x1D00,
    GL_TEXTURE_ENV = 0x2300, GL_TEXTURE_ENV_MODE = 0x2200, GL_MODULATE = 0x2100, GL_REPLACE = 0x1E01, GL_ADD = 0x0104,
    GL_DECAL = 0x2101, GL_ALPHA_FMT = 0x1906, GL_ALPHA8 = 0x803C, GL_NEVER = 0x0200,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_FUNC_ADD = 0x8006, GL_BLEND_SRC_ALPHA = 0x80CB, GL_BLEND_DST_ALPHA = 0x80CA,
    GL_BLEND_EQUATION_RGB = 0x8009, GL_BLEND_EQUATION_ALPHA = 0x883D, GL_VERTEX_ARRAY_BINDING = 0x85B5,
    GL_COLOR_WRITEMASK = 0x0C23, GL_POLYGON_MODE = 0x0B40, GL_FRONT = 0x0404, GL_FRONT_AND_BACK = 0x0408, GL_FILL = 0x1B02,
    GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_CULL_FACE = 0x0B44, GL_SCISSOR_TEST = 0x0C11,
    GL_FRAMEBUFFER_SRGB = 0x8DB9, GL_ACTIVE_TEXTURE = 0x84E0, GL_TEXTURE0 = 0x84C0,
};

// ---------------------------------------------------------------- entry points

static struct {
    void (WINAPI* Enable)(GLenum);
    void (WINAPI* Disable)(GLenum);
    void (WINAPI* Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (WINAPI* BindTexture)(GLenum, GLuint);
    void (WINAPI* GenTextures)(GLsizei, GLuint*);
    void (WINAPI* DeleteTextures)(GLsizei, const GLuint*);
    void (WINAPI* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void (WINAPI* TexParameteri)(GLenum, GLenum, GLint);
    void (WINAPI* DrawArrays)(GLenum, GLint, GLsizei);
    void (WINAPI* ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (WINAPI* PolygonMode)(GLenum, GLenum);
    GLuint (WINAPI* CreateShader)(GLenum);
    void (WINAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (WINAPI* CompileShader)(GLuint);
    void (WINAPI* GetShaderiv)(GLuint, GLenum, GLint*);
    void (WINAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (WINAPI* DeleteShader)(GLuint);
    GLuint (WINAPI* CreateProgram)();
    void (WINAPI* AttachShader)(GLuint, GLuint);
    void (WINAPI* LinkProgram)(GLuint);
    void (WINAPI* GetProgramiv)(GLuint, GLenum, GLint*);
    void (WINAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (WINAPI* UseProgram)(GLuint);
    GLint (WINAPI* GetUniformLocation)(GLuint, const GLchar*);
    void (WINAPI* Uniform1i)(GLint, GLint);
    void (WINAPI* Uniform1f)(GLint, GLfloat);
    void (WINAPI* Uniform2f)(GLint, GLfloat, GLfloat);
    void (WINAPI* Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void (WINAPI* Uniform4i)(GLint, GLint, GLint, GLint, GLint);
    void (WINAPI* GetTexEnviv)(GLenum, GLenum, GLint*);
    void (WINAPI* Uniform4fv)(GLint, GLsizei, const GLfloat*);
    void (WINAPI* TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    void (WINAPI* DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
    void (WINAPI* ActiveTexture)(GLenum);
    void (WINAPI* GenFramebuffers)(GLsizei, GLuint*);
    void (WINAPI* DeleteFramebuffers)(GLsizei, const GLuint*);
    void (WINAPI* BindFramebuffer)(GLenum, GLuint);
    void (WINAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (WINAPI* CheckFramebufferStatus)(GLenum);
    void (WINAPI* DrawBuffers)(GLsizei, const GLenum*);
    void (WINAPI* GenVertexArrays)(GLsizei, GLuint*);
    void (WINAPI* BindVertexArray)(GLuint);
    void (WINAPI* ClearBufferfv)(GLenum, GLint, const GLfloat*);
    void (WINAPI* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (WINAPI* BlendEquationSeparate)(GLenum, GLenum);
    void (WINAPI* Enablei)(GLenum, GLuint);
    void (WINAPI* Disablei)(GLenum, GLuint);
    void (WINAPI* BlendFuncSeparatei)(GLuint, GLenum, GLenum, GLenum, GLenum);
    void (WINAPI* BlendEquationi)(GLuint, GLenum);
    void (WINAPI* ColorMaski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
    void (WINAPI* GenQueries)(GLsizei, GLuint*);
    void (WINAPI* QueryCounter)(GLuint, GLenum);
    void (WINAPI* GetQueryObjectui64v)(GLuint, GLenum, unsigned long long*);
    void (WINAPI* GetQueryObjectiv)(GLuint, GLenum, GLint*);
    void (WINAPI* BeginQuery)(GLenum, GLuint);
    void (WINAPI* EndQuery)(GLenum);
} G;

static bool LoadProcs() {
    bool ok = true;
#define P(field, name) do { G.field = (decltype(G.field))RealProc(name); if (!G.field) { SessionLog("gfx: missing %s", name); ok = false; } } while (0)
    P(Enable, "glEnable"); P(Disable, "glDisable"); P(Viewport, "glViewport"); P(BindTexture, "glBindTexture");
    P(GenTextures, "glGenTextures"); P(DeleteTextures, "glDeleteTextures"); P(TexImage2D, "glTexImage2D");
    P(TexParameteri, "glTexParameteri"); P(DrawArrays, "glDrawArrays"); P(ColorMask, "glColorMask");
    P(PolygonMode, "glPolygonMode");
    P(CreateShader, "glCreateShader"); P(ShaderSource, "glShaderSource"); P(CompileShader, "glCompileShader");
    P(GetShaderiv, "glGetShaderiv"); P(GetShaderInfoLog, "glGetShaderInfoLog"); P(DeleteShader, "glDeleteShader");
    P(CreateProgram, "glCreateProgram"); P(AttachShader, "glAttachShader"); P(LinkProgram, "glLinkProgram");
    P(GetProgramiv, "glGetProgramiv"); P(GetProgramInfoLog, "glGetProgramInfoLog"); P(UseProgram, "glUseProgram");
    P(GetUniformLocation, "glGetUniformLocation"); P(Uniform1i, "glUniform1i"); P(Uniform1f, "glUniform1f");
    P(Uniform2f, "glUniform2f"); P(Uniform4f, "glUniform4f"); P(Uniform4i, "glUniform4i"); P(GetTexEnviv, "glGetTexEnviv"); P(Uniform4fv, "glUniform4fv"); P(TexSubImage2D, "glTexSubImage2D"); P(DrawArraysInstanced, "glDrawArraysInstanced"); P(ActiveTexture, "glActiveTexture");
    P(GenFramebuffers, "glGenFramebuffers"); P(DeleteFramebuffers, "glDeleteFramebuffers");
    P(BindFramebuffer, "glBindFramebuffer"); P(FramebufferTexture2D, "glFramebufferTexture2D");
    P(CheckFramebufferStatus, "glCheckFramebufferStatus"); P(DrawBuffers, "glDrawBuffers");
    P(GenVertexArrays, "glGenVertexArrays"); P(BindVertexArray, "glBindVertexArray"); P(ClearBufferfv, "glClearBufferfv");
    P(BlendFuncSeparate, "glBlendFuncSeparate"); P(BlendEquationSeparate, "glBlendEquationSeparate");
    P(Enablei, "glEnablei"); P(Disablei, "glDisablei"); P(BlendFuncSeparatei, "glBlendFuncSeparatei");
    P(BlendEquationi, "glBlendEquationi"); P(ColorMaski, "glColorMaski");
#undef P
    G.GenQueries = (decltype(G.GenQueries))RealProc("glGenQueries");
    G.QueryCounter = (decltype(G.QueryCounter))RealProc("glQueryCounter");
    G.GetQueryObjectui64v = (decltype(G.GetQueryObjectui64v))RealProc("glGetQueryObjectui64v");
    G.GetQueryObjectiv = (decltype(G.GetQueryObjectiv))RealProc("glGetQueryObjectiv");
    G.BeginQuery = (decltype(G.BeginQuery))RealProc("glBeginQuery");
    G.EndQuery = (decltype(G.EndQuery))RealProc("glEndQuery");
    return ok;
}

// ---------------------------------------------------------------- settings

struct Config {
    bool enabled = true, fxaa = true, additiveEmits = true;
    int toggleKey = VK_F9, overlayKey = VK_INSERT;
    float hudHeight = 600;
    float exposure = 1.0f, knee = 0.8f;
    float bloomEmissive = 1.0f, bloomScene = 0.1f, bloomThreshold = 1.0f, bloomKnee = 0.5f, bloomRadius = 1.0f;
    float bloomStrength = 1.0f, bloomSpread = 0.85f, bloomLimit = 2.5f, bloomAutoDim = 0.5f;   // [Bloom]: kept across looks
    float contrast = 1.05f, saturation = 1.06f, vibrance = 0.15f, temperature = 0.0f, tint = 0.0f;
    float lift = 0.0f, gamma = 1.0f, gain = 1.0f;
    float vignette = 0.25f, grain = 0.02f, sharpen = 0.3f, chroma = 0.0f;
    float splitTone = 0.0f, shadowTint[3] = { 0.5f, 0.5f, 0.5f }, highTint[3] = { 0.5f, 0.5f, 0.5f };
    float ink = 0.0f, posterize = 0.0f;
    int lookPrevKey = VK_F7, lookNextKey = VK_F8, labKey = VK_HOME, photoKey = VK_END;
    std::vector<std::string> emitNames, emitExclude, softGlowNames;
    float softGlow = 0.25f;   // light weight of soft halo sprites (bullet-impact glows etc.)
    // lighting
    bool lighting = true;
    int lightingKey = VK_F10;
    float giScale = 0.2f, giTemporal = 0.8f, giEmission = 6.0f, giIntensity = 1.0f, ao = 0.5f, skyDown = 0.4f, bgLight = 0.3f, lightOpacity = 0.15f;
    std::vector<std::string> occluderPaths;
    // fog
    bool fog = false;   // off by default for now (pale haze pooling around fort bases)
    float fogDensity = 1.0f, fogHaze = 0.07f, fogBanks = 0.10f, fogPool = 0.25f, fogFront = 0.35f, bgBlur = 0.45f;
    float fogAmbient = 0.85f, fogLight = 1.0f, fogNoiseScale = 1400, fogWind = 40;
    float fogStartY = 0, fogEndY = 2000, fogHeight = 0;
    float fogColor[3] = { 0.6f, 0.57f, 0.53f };
    int fogBlurLevel = 1;
    // materials (world shader takeover) + sun
    bool materials = true;
    float bevel = 2.0f, bevelPx = 14.0f, detail = 1.2f, specMask = 0.8f;
    float lightHeight = 0.4f, toon = 0.35f, hemi = 0.15f, ambient = 0.68f, specular = 1.0f, shininess = 24.0f;
    float edgeBevel = 1.2f, edgeRadius = 12.0f;
    float sunStrength = 0.35f, sunDir[2] = { -0.64f, 0.77f }, sunWidth = 14.0f, sunColor[3] = { 1.0f, 0.92f, 0.78f };
    // impact effects
    bool impactFx = true, fireballs = true, impactFrames = true;
    float shockStrength = 0.24f, shockSize = 2.2f, shake = 0.6f, pulse = 0.6f, flashLight = 1.6f, heatHaze = 0.6f;
    float muzzleLight = 0.4f, gunHeat = 1.0f;
    bool simSmoke = true;                         // replace the game's steam and smoke with simulated puffs
    float smokeAmount = 1.0f, plumeLight = 1.2f, beamGlow = 1.0f, tracerGlow = 1.0f;
    float fireballSize = 1.5f, fireballBright = 1.6f, fxSaturation = 1.25f;
    float effectGlow = 1.0f, effectLight = 1.0f;   // effects preset: how effects glow and light the world, whatever the look
    float fireGlow = 0.7f, fireHeat = 0.45f;        // [Bloom] / [Impact]: burning structures (dozens of flame sprites each)
    bool moreSimFx = true;                          // the newer replacements: blast clouds, dust, smoke clouds, hot gas
    // [GameFx] effects from the game's own events (engine.cpp): what was hit, by what, where
    bool gameFx = true, gfImpacts = true, gfMuzzles = true, gfLasers = true, gfHeavy = true, gfScorch = true, gfDevices = true;
    bool gfCollapse = true, gfWeather = true, gfLights = true, gfInFlight = true;
    float gfAmount = 1.0f, scorchLife = 180.0f, scorchStrength = 1.0f;
    float gfBeamLook = 1.0f;                                  // [GameFx] BeamLook: the layered glow over lasers and firebeams
    float gfBlastWind = 1.0f;                                 // [GameFx] BlastWind: blasts shove the smoke, dust, rain and snow around them
    bool gfShakeOff = true, gfHitColour = true, gfDustHaze = true, gfGroundBlast = true;   // [GameFx] what a hit leaves behind
    bool funMiko = false, mikoVoice = true, mikoFuji = true;  // [Fun] the water cannon is an anime shrine maiden spraying milk (opt in)
    bool waterStream = true;                                  // [Impact] the water cannon's shots drawn as one continuous stream
    float debris = 1.0f, sparks = 1.0f, embers = 1.0f, emberBright = 2.5f, sparkBright = 3.0f, gravity = 1000.0f;
    float impactMinSize = 0.06f, impactCooldown = 2.5f, impactDurationMs = 55.0f;
    // weather
    int weatherMode = 0;                 // 0 auto (follow the map), 1 off, 2 rain, 3 storm
    bool replaceRain = true, lightning = true, autoStorms = true, thunder = true;
    float rainAmount = 1.0f, rainLean = 0.35f, nearRain = 0.25f, wetness = 1.0f, puddles = 1.0f;
    float snowAmount = 1.0f, snowCover = 1.0f;
    float snowDepth = 8.0f, snowBuild = 1.0f, snowSlide = 0.35f;
    // day / night cycle
    bool dayNight = true;
    float dayMinutes = 20.0f, dayStart = 13.0f, nightLights = 1.6f, nightDark = 0.8f, duskGlow = 0.45f;
    // water (High Seas maps: the map's dlc2_WaterLevel)
    bool water = true;
    float waterRefl = 1.0f, waterWaves = 1.0f, waterFoam = 1.0f;   // snow physics: max depth (cells of 4 px at 4K), build-up, sliding
    float lightningRate = 6.0f, lightningBright = 1.6f, thunderVolume = 0.7f;
    // hybrid 3D backdrop
    bool backdrop3D = true, bdColourFromMap = true;
    int bgReplace = -1, bdRanges = 3;
    bool bdHideOriginal = false;   // hide every painted background layer, only the 3D backdrop
    float bgAutoFraction = 0.5f, bdHorizon = 0.0f, bdMountainHeight = 0.75f, bdClouds = 0.6f, bdCloudSpeed = 1.0f;
    float bdStars = 1.0f, bdSunGlow = 1.0f, bdParallax = 1.0f, bdRefWidth = 6000.0f;
    float bdSkyTop[3] = { 0.36f, 0.52f, 0.78f }, bdSkyHorizon[3] = { 0.85f, 0.78f, 0.66f }, bdMountain[3] = { 0.45f, 0.40f, 0.42f };
    // presets (remembered so the panel shows the last one picked)
    int stylePreset = 0, qualityPreset = 1, look = 0, fxPreset = 0;
};
static Config cfg;
static std::atomic<float> g_waterLevel{ 0.0f };   // water surface (world y) of the current map, if it has water
static std::atomic<bool> g_hasWater{ false };
static int g_debugView;   // runtime only: buffer shown full screen (0 = none)

// One table drives ini loading/saving and the overlay's controls. Section/key names are the ini's.
static std::vector<GfxSetting> g_settings;

static void BuildSettingsTable() {
    if (!g_settings.empty()) return;
    using S = GfxSetting;
    auto B = [](const char* sec, const char* key, const char* label, bool* p, const char* tip = "") {
        g_settings.push_back({ S::Bool, sec, key, label, p, 0, 1, tip });
    };
    auto F = [](const char* sec, const char* key, const char* label, float* p, float lo, float hi, const char* tip = "") {
        g_settings.push_back({ S::Float, sec, key, label, p, lo, hi, tip });
    };
    auto I = [](const char* sec, const char* key, const char* label, int* p, float lo, float hi, const char* tip = "") {
        g_settings.push_back({ S::Int, sec, key, label, p, lo, hi, tip });
    };
    auto V = [](const char* sec, const char* key, const char* label, float* p, float lo, float hi, const char* tip = "") {
        g_settings.push_back({ S::Vec2, sec, key, label, p, lo, hi, tip });
    };
    auto C = [](const char* sec, const char* key, const char* label, float* p, const char* tip = "") {
        g_settings.push_back({ S::Color, sec, key, label, p, 0, 1, tip });
    };
    B("Post", "Enabled", "Post-processing", &cfg.enabled, "Master switch (F9)");
    I("Post", "ToggleKeyVK", "Post toggle key (VK code)", &cfg.toggleKey, 1, 254);
    I("Post", "OverlayKeyVK", "Control panel key (VK code)", &cfg.overlayKey, 1, 254);
    F("Post", "Exposure", "Exposure", &cfg.exposure, 0.3f, 3.0f);
    F("Post", "HighlightKnee", "Highlight roll-off start", &cfg.knee, 0.3f, 1.0f);
    F("Post", "BloomEmissive", "Bloom from light sources", &cfg.bloomEmissive, 0, 4);
    F("Post", "BloomScene", "Bloom from bright pixels", &cfg.bloomScene, 0, 1);
    F("Post", "BloomSceneThreshold", "Bright-pixel threshold", &cfg.bloomThreshold, 0.2f, 3.0f);
    F("Post", "BloomRadius", "Bloom radius", &cfg.bloomRadius, 0.25f, 3.0f);
    F("Bloom", "Strength", "Bloom strength", &cfg.bloomStrength, 0, 2, "How much bloom is laid over the image (its opacity)");
    F("Bloom", "Spread", "Bloom spread", &cfg.bloomSpread, 0.3f, 1, "Lower: glows stay tight around each light instead of a haze over the whole base");
    F("Bloom", "Limit", "Bloom limit", &cfg.bloomLimit, 0, 8, "The brightest bloom can get anywhere (0 = no limit): stacked explosions can't wash the base out");
    F("Bloom", "AutoDim", "Auto-dim in big fights", &cfg.bloomAutoDim, 0, 2, "Turns bloom down while much of the screen glows");
    F("Bloom", "FireGlow", "Fire glow", &cfg.fireGlow, 0, 2, "How much burning structures (and their sparks) glow and light their surroundings");
    F("Post", "Contrast", "Contrast", &cfg.contrast, 0.5f, 2.0f);
    F("Post", "Saturation", "Saturation", &cfg.saturation, 0, 2.5f);
    F("Post", "Vibrance", "Vibrance", &cfg.vibrance, -1, 1.5f);
    F("Post", "Temperature", "Temperature", &cfg.temperature, -1, 1, "Negative = cooler, positive = warmer");
    F("Post", "Tint", "Tint", &cfg.tint, -1, 1);
    F("Post", "Lift", "Lift (shadows)", &cfg.lift, -0.2f, 0.3f);
    F("Post", "Gamma", "Gamma (midtones)", &cfg.gamma, 0.5f, 2.0f);
    F("Post", "Gain", "Gain (highlights)", &cfg.gain, 0.5f, 2.0f);
    B("Post", "FXAA", "FXAA anti-aliasing", &cfg.fxaa);
    F("Post", "Sharpen", "Sharpen", &cfg.sharpen, 0, 1);
    F("Post", "Vignette", "Vignette", &cfg.vignette, 0, 1);
    F("Post", "Grain", "Film grain", &cfg.grain, 0, 0.15f);
    F("Post", "ChromaticAberration", "Chromatic aberration", &cfg.chroma, 0, 2);
    F("Post", "SplitTone", "Split toning", &cfg.splitTone, 0, 1, "Tints shadows and highlights with the two colours below");
    C("Post", "ShadowTint", "Shadow tint (grey = none)", cfg.shadowTint);
    C("Post", "HighlightTint", "Highlight tint (grey = none)", cfg.highTint);
    F("Post", "InkOutlines", "Ink outlines (comic)", &cfg.ink, 0, 2);
    F("Post", "Posterize", "Posterize levels (0 = off)", &cfg.posterize, 0, 12);
    I("Post", "LookPrevKeyVK", "Previous look key (VK code)", &cfg.lookPrevKey, 1, 254);
    I("Post", "LookNextKeyVK", "Next look key (VK code)", &cfg.lookNextKey, 1, 254);
    I("Post", "LabKeyVK", "Lab bar key (VK code)", &cfg.labKey, 1, 254);
    I("Post", "PhotoKeyVK", "Photo mode key (VK code)", &cfg.photoKey, 1, 254);
    F("Post", "HudVirtualHeight", "HUD virtual height (detection)", &cfg.hudHeight, 300, 1200, "Height of the game's HUD projection; used to find where the HUD starts");
    B("Emitters", "AdditiveBlendEmits", "Additive sprites emit light", &cfg.additiveEmits);
    F("Emitters", "SoftGlowLight", "Light from soft glow sprites", &cfg.softGlow, 0, 1,
      "Halo sprites (names in SoftGlowNames, e.g. bullet-impact glows) are already drawn as light; this is how much they also light the world and bloom");
    B("Lighting", "Enabled", "Ray-traced lighting + fog", &cfg.lighting, "F10");
    I("Lighting", "ToggleKeyVK", "Lighting toggle key (VK code)", &cfg.lightingKey, 1, 254);
    F("Lighting", "Resolution", "Lighting resolution", &cfg.giScale, 0.125f, 0.5f, "Fraction of screen resolution the lighting is traced at");
    F("Lighting", "TemporalSmoothing", "Temporal smoothing", &cfg.giTemporal, 0, 0.95f);
    F("Lighting", "EmitterBrightness", "Light source brightness", &cfg.giEmission, 0, 20);
    F("Lighting", "Intensity", "Light intensity on surfaces", &cfg.giIntensity, 0, 4);
    F("Lighting", "LightSourceOpacity", "Light source opacity (per cell)", &cfg.lightOpacity, 0, 2, "How much lasers/fire block light passing through them; 0 = fully transparent");
    F("Lighting", "AmbientOcclusion", "Ambient occlusion", &cfg.ao, 0, 1);
    F("Lighting", "SkyLightFromBelow", "Sky light from below", &cfg.skyDown, 0, 1);
    F("Lighting", "BackgroundLight", "Light on the backdrop", &cfg.bgLight, 0, 1);
    F("Materials", "AmbientLevel", "Ambient level", &cfg.ambient, 0.2f, 2.0f, "Brightness of surfaces lit only by the sky");
    B("Materials", "Enabled", "Material shader (normals)", &cfg.materials, "Draw the world with our shader instead of fixed-function");
    F("Materials", "SpriteBevel", "Sprite bevel strength", &cfg.bevel, 0, 5);
    F("Materials", "SpriteBevelPixels", "Sprite bevel width (px at 4K)", &cfg.bevelPx, 1, 40);
    F("Materials", "SurfaceDetail", "Painted detail relief", &cfg.detail, 0, 4);
    F("Materials", "MetalShine", "Metal shine mask", &cfg.specMask, 0, 2);
    F("Materials", "Specular", "Specular highlights", &cfg.specular, 0, 4);
    F("Materials", "Shininess", "Shininess", &cfg.shininess, 2, 128);
    F("Materials", "LightHeight", "Light height (lower = more grazing)", &cfg.lightHeight, 0.1f, 1.0f);
    F("Materials", "ToonBanding", "Toon banding", &cfg.toon, 0, 1);
    F("Materials", "SkyGroundTint", "Sky above / ground below tint", &cfg.hemi, 0, 0.5f);
    F("Materials", "EdgeBevel", "Terrain edge bevel", &cfg.edgeBevel, 0, 4);
    F("Materials", "EdgeBevelPixels", "Terrain edge bevel width (px)", &cfg.edgeRadius, 2, 40);
    F("Sun", "OpenAreaBrightening", "Sun strength", &cfg.sunStrength, 0, 1.5f, "How much the sun brightens open surfaces");
    F("Sun", "WidthDegrees", "Sun size (shadow softness)", &cfg.sunWidth, 2, 60);
    V("Sun", "Direction", "Sun direction (x, y up)", cfg.sunDir, -1, 1);
    C("Sun", "Color", "Sun colour", cfg.sunColor);
    B("Fog", "Enabled", "Fog", &cfg.fog, "Volumetric fog: backdrop haze, drifting banks, pooling, light shafts");
    F("Fog", "Density", "Fog density", &cfg.fogDensity, 0, 4);
    F("Fog", "BackdropHaze", "Backdrop haze", &cfg.fogHaze, 0, 1);
    F("Fog", "DriftingBanks", "Drifting fog banks", &cfg.fogBanks, 0, 1);
    F("Fog", "PoolInValleys", "Fog pooling in valleys", &cfg.fogPool, 0, 2);
    F("Fog", "BackdropBlur", "Backdrop depth blur", &cfg.bgBlur, 0, 1);
    F("Fog", "InFrontOfPlayfield", "Fog in front of forts", &cfg.fogFront, 0, 1);
    F("Fog", "Ambient", "Fog ambient brightness", &cfg.fogAmbient, 0, 2);
    F("Fog", "LightScatter", "Light scattering (shafts)", &cfg.fogLight, 0, 4);
    F("Fog", "NoiseScale", "Fog noise scale (world units)", &cfg.fogNoiseScale, 200, 6000);
    F("Fog", "Wind", "Wind speed", &cfg.fogWind, -400, 400);
    I("Fog", "LightBlurLevel", "Fog light blur level", &cfg.fogBlurLevel, 0, 4);
    C("Fog", "Color", "Fog colour", cfg.fogColor);
    F("Fog", "HeightFogAmount", "Height fog amount", &cfg.fogHeight, 0, 2);
    F("Fog", "HeightFogStartWorldY", "Height fog start (world y)", &cfg.fogStartY, -5000, 10000);
    F("Fog", "HeightFogFullWorldY", "Height fog full (world y)", &cfg.fogEndY, -5000, 10000);
    B("Impact", "Enabled", "Impact effects", &cfg.impactFx, "Everything triggered by explosions, flashes and sparks");
    F("Impact", "Shockwave", "Shockwave distortion", &cfg.shockStrength, 0, 3);
    F("Impact", "ShockwaveSize", "Shockwave size (x explosion)", &cfg.shockSize, 1, 20);
    F("Impact", "ScreenShake", "Screen shake", &cfg.shake, 0, 2);
    F("Impact", "ColourPulse", "Colour pulse", &cfg.pulse, 0, 2);
    F("Impact", "FlashLight", "Explosion light flash", &cfg.flashLight, 0, 20, "Short burst of ray-traced light from each explosion");
    F("Impact", "MuzzleFlashLight", "Gun muzzle flash light", &cfg.muzzleLight, 0, 4, "Light from muzzle flashes, relative to the explosion flash");
    B("Impact", "SimulatedSmoke", "Simulated steam & smoke", &cfg.simSmoke, "Replace the game's steam and smoke sprites (missile and mortar trails, overheating guns, damaged devices) with simulated puffs that rise, billow and are lit by the scene");
    F("Impact", "SmokeAmount", "Steam & smoke amount", &cfg.smokeAmount, 0, 3);
    F("Impact", "RocketPlumeLight", "Rocket plume light", &cfg.plumeLight, 0, 6, "Light, heat shimmer and smoke trail from rocket and missile engines");
    F("Impact", "LaserGlow", "Laser glow", &cfg.beamGlow, 0, 3, "White-hot core and flowing energy on lasers and beams");
    F("Impact", "TracerGlow", "Tracer glow", &cfg.tracerGlow, 0, 3, "Glowing streaks and light behind bullets, cannon and mortar shells (0 = off)");
    F("Impact", "HeatHaze", "Heat haze", &cfg.heatHaze, 0, 3, "Shimmer above fire, explosions and guns that are firing");
    F("Impact", "GunHeat", "Heat from firing guns", &cfg.gunHeat, 0, 3, "How fast sustained fire heats a gun (haze builds up, cools over a few seconds)");
    B("Impact", "Fireballs", "Volumetric 3D fireballs", &cfg.fireballs);
    F("Impact", "FireballSize", "Fireball size (x explosion)", &cfg.fireballSize, 0.5f, 4);
    F("Impact", "FireballBrightness", "Fireball brightness", &cfg.fireballBright, 0, 6);
    F("Impact", "EffectSaturation", "Effect saturation", &cfg.fxSaturation, 0, 2.5f);
    F("Impact", "EffectGlow", "Effect glow", &cfg.effectGlow, 0, 4, "How much explosions, fire, flashes and lasers bloom - the same on every look");
    F("Impact", "EffectLight", "Effect light", &cfg.effectLight, 0, 4, "How much light explosions, flashes and rocket flames throw - the same on every look");
    F("Impact", "FireHeat", "Heat haze over fires", &cfg.fireHeat, 0, 1.5f, "How strongly burning structures shimmer");
    B("Impact", "SimulateMoreEffects", "Simulate more effects", &cfg.moreSimFx,
      "The newer replacements: the deck gun's blast cloud, dust from impacts, smoke clouds and hot gas become simulated puffs");
    F("Impact", "Debris", "Debris amount", &cfg.debris, 0, 4);
    F("Impact", "Sparks", "Spark amount", &cfg.sparks, 0, 4);
    F("Impact", "Embers", "Ember amount", &cfg.embers, 0, 4);
    F("Impact", "EmberBrightness", "Ember brightness", &cfg.emberBright, 0, 8);
    F("Impact", "SparkBrightness", "Spark brightness", &cfg.sparkBright, 0, 8);
    F("Impact", "Gravity", "Particle gravity (world units/s^2)", &cfg.gravity, 0, 4000);
    B("Impact", "ImpactFrames", "Impact frames (big hits)", &cfg.impactFrames, "Anime-style 1-3 frame flash of silhouettes and speed lines");
    F("Impact", "ImpactMinSize", "Impact frame: min explosion size", &cfg.impactMinSize, 0.005f, 0.4f, "Explosion radius as a fraction of the view height");
    F("Impact", "ImpactCooldown", "Impact frame cooldown (s)", &cfg.impactCooldown, 0, 15);
    F("Impact", "ImpactDurationMs", "Impact frame duration (ms)", &cfg.impactDurationMs, 10, 250);
    B("GameFx", "Enabled", "Effects from the game's events", &cfg.gameFx, "Effects driven by what the game itself reports (read from its memory): what was hit and by what, which weapon fired, which device died");
    B("GameFx", "MaterialImpacts", "Impacts by material", &cfg.gfImpacts, "Sparks and molten slag off armour, splinters off bracing, ripples on shields, splashes on water, earth off the ground");
    B("GameFx", "Muzzles", "Weapon muzzle effects", &cfg.gfMuzzles, "Cannon pressure ring and hanging smoke, mortar puff, sniper shimmer, hot smoking machine-gun barrels, launch exhaust");
    B("GameFx", "Lasers", "Laser effects", &cfg.gfLasers, "The emitter glows while firing, light along the beam, a molten glow and dripping slag where it cuts");
    F("GameFx", "BeamLook", "Laser beam glow", &cfg.gfBeamLook, 0, 3, "The layered glow over lasers and firebeams, like the orbital laser's: halo, glow, white-hot core, energy pulses (0 = off)");
    F("GameFx", "BlastWind", "Blast wind", &cfg.gfBlastWind, 0, 3, "Blasts shove the smoke, dust, rain, snow, embers and petals around them outward, then the air rushing back draws them in a little (0 = off)");
    B("GameFx", "ShakeOff", "Dust shaken off", &cfg.gfShakeOff, "Hits shake dust and grit off the struts and surfaces above and around them; bigger hits loosen more, over a wider area");
    B("GameFx", "HitColour", "Debris coloured like the hit", &cfg.gfHitColour, "Chunks and splinters take the colour of what they broke off (not the explosion's glow)");
    B("GameFx", "DustHaze", "Dust haze", &cfg.gfDustHaze, "Heavy hits leave a thin haze of dust that settles over a few seconds");
    B("GameFx", "GroundBlast", "Ground blast", &cfg.gfGroundBlast, "Ground hits blow a ring of dark smoke out along the ground, and blasts kick dust off the surfaces around them");
    B("GameFx", "HeavyWeapons", "Heavy weapon effects", &cfg.gfHeavy, "Warheads, nukes, reactors: a delayed shockwave ring, the camera adjusting to the flash, a dust wave along the ground");
    B("GameFx", "ScorchMarks", "Scorch marks & craters", &cfg.gfScorch, "Marks left where things really hit, fading over a few minutes");
    F("GameFx", "ScorchSeconds", "Scorch marks last (s)", &cfg.scorchLife, 10, 900);
    F("GameFx", "ScorchDarkness", "Scorch darkness", &cfg.scorchStrength, 0, 2);
    B("GameFx", "DeviceDeaths", "Device destruction", &cfg.gfDevices, "Reactor flash and glow, battery arcs, mine dust, smoking wrecks");
    B("GameFx", "CollapseDust", "Collapse dust", &cfg.gfCollapse, "Dust and rubble falling where struts break");
    B("GameFx", "Weather", "Weather meets the battle", &cfg.gfWeather, "Rain hissing to steam on fires, snow melting around them, storm wind bending the smoke");
    B("GameFx", "EventLights", "Light from shots and hits", &cfg.gfLights, "Muzzle flashes and impacts briefly light the structures around them");
    B("GameFx", "InFlight", "Projectiles in flight", &cfg.gfInFlight, "Shells shimmer behind them, fast rounds leave a faint ring, missiles cast shadows");
    F("GameFx", "Amount", "Game effect amount", &cfg.gfAmount, 0, 3, "How many particles and puffs these effects make");
    B("Impact", "WaterStream", "Water cannon: continuous stream", &cfg.waterStream, "The water cannon's shots are drawn as one flowing stream of water instead of separate drops (visual only)");
    B("Fun", "MilkMiko", "Milk miko (water cannon)", &cfg.funMiko, "A joke: the water cannon is an over-the-top anime shrine maiden spraying simulated milk, with sakura, manga sound words and her voice");
    B("Fun", "Voice", "Milk miko voice", &cfg.mikoVoice, "Her voice lines (ElevenLabs)");
    B("Fun", "MountFuji", "Mount Fuji in the sky", &cfg.mikoFuji, "Mount Fuji in the 3D backdrop while she is on the map");
    I("Weather", "Mode", "Weather (0 auto, 1 off, 2 rain, 3 storm, 4 snow, 5 thick snow, 6 sandstorm, 7 fog banks, 8 ash)", &cfg.weatherMode, 0, 8);
    B("Weather", "ReplaceGameRain", "Replace the game's rain", &cfg.replaceRain);
    F("Weather", "RainAmount", "Rain amount", &cfg.rainAmount, 0, 4);
    F("Weather", "WindLean", "Rain wind lean", &cfg.rainLean, -1.5f, 1.5f);
    F("Weather", "NearRain", "Near rain layer", &cfg.nearRain, 0, 3);
    F("Weather", "Wetness", "Wet surfaces", &cfg.wetness, 0, 2);
    F("Weather", "Puddles", "Puddle reflections", &cfg.puddles, 0, 2, "Flat wet tops mirror the sky, fires and explosions, with rain ripples");
    F("Weather", "SnowAmount", "Snowfall amount", &cfg.snowAmount, 0, 12, "How many flakes fall (also sand and ash)");
    F("Weather", "SnowDepth", "Snow max depth", &cfg.snowDepth, 1, 24, "How deep snow can pile up (in steps of 4 px at 4K)");
    F("Weather", "SnowBuildUp", "Snow build-up speed", &cfg.snowBuild, 0, 40, "How much each landed flake adds to the snow (1 = gentle, 10 = fast, 40 = instant drifts)");
    F("Weather", "SnowSliding", "Snow sliding", &cfg.snowSlide, 0, 1, "How easily snow slides off edges and slopes (0 = sticks everywhere)");
    B("Weather", "Lightning", "Lightning", &cfg.lightning);
    B("Weather", "StormsWithMapRain", "Storms when the map rains (auto)", &cfg.autoStorms);
    F("Weather", "StrikesPerMinute", "Lightning strikes per minute", &cfg.lightningRate, 0, 60);
    F("Weather", "LightningBrightness", "Lightning brightness", &cfg.lightningBright, 0, 5);
    B("Weather", "Thunder", "Thunder sound", &cfg.thunder);
    F("Weather", "ThunderVolume", "Thunder volume", &cfg.thunderVolume, 0, 1);
    B("Water", "Enabled", "Water effects", &cfg.water, "On maps with water (High Seas): reflections, waves, foam, depth, splashes");
    F("Water", "Reflections", "Water reflections", &cfg.waterRefl, 0, 1.5f);
    F("Water", "Waves", "Waves and ripples", &cfg.waterWaves, 0, 3);
    F("Water", "Foam", "Foam at the waterline", &cfg.waterFoam, 0, 2);
    B("DayNight", "Enabled", "Day / night cycle", &cfg.dayNight, "The sun crosses the sky: dawn, day, golden dusk, moonlit night");
    F("DayNight", "CycleMinutes", "Minutes for a full day", &cfg.dayMinutes, 2, 120);
    F("DayNight", "StartHour", "Time of day at start (hours)", &cfg.dayStart, 0, 24);
    F("DayNight", "NightLights", "Light sources at night", &cfg.nightLights, 1, 4, "How much brighter fires, lasers and explosions are at night");
    F("DayNight", "NightDarkness", "Night darkness", &cfg.nightDark, 0, 1);
    F("DayNight", "DuskGlow", "Dawn / dusk glow", &cfg.duskGlow, 0, 1, "How orange sunrise and sunset get");
    B("Backdrop", "Enabled", "3D backdrop (replaces far layers)", &cfg.backdrop3D);
    I("Backdrop", "ReplaceLayers", "Layers replaced (-1 = auto)", &cfg.bgReplace, -1, 16, "How many of the farthest painted layers our 3D backdrop replaces");
    F("Backdrop", "AutoReplaceFraction", "Auto: fraction of layers replaced", &cfg.bgAutoFraction, 0.1f, 1.0f);
    B("Backdrop", "HideOriginal", "Hide the original background", &cfg.bdHideOriginal,
      "Every painted background layer is hidden, near ones too: only the 3D backdrop behind the battle");
    B("Backdrop", "ColoursFromMap", "Colours from the map", &cfg.bdColourFromMap);
    F("Backdrop", "Horizon", "Horizon height", &cfg.bdHorizon, -0.6f, 0.6f);
    F("Backdrop", "MountainHeight", "Mountain height", &cfg.bdMountainHeight, 0, 2.5f);
    I("Backdrop", "MountainRanges", "Mountain ranges", &cfg.bdRanges, 0, 3);
    F("Backdrop", "Clouds", "Clouds", &cfg.bdClouds, 0, 1);
    F("Backdrop", "CloudSpeed", "Cloud speed", &cfg.bdCloudSpeed, -5, 5);
    F("Backdrop", "Stars", "Stars (dark skies)", &cfg.bdStars, 0, 2);
    F("Backdrop", "SunGlow", "Sun glow", &cfg.bdSunGlow, 0, 3);
    F("Backdrop", "Parallax", "Parallax", &cfg.bdParallax, 0, 3);
    F("Backdrop", "ReferenceWidth", "Reference view width (world)", &cfg.bdRefWidth, 1000, 30000, "World width at which the backdrop is sized; larger = smaller mountains");
    C("Backdrop", "SkyTop", "Sky colour (top) - manual", cfg.bdSkyTop);
    C("Backdrop", "SkyHorizon", "Sky colour (horizon) - manual", cfg.bdSkyHorizon);
    C("Backdrop", "Mountain", "Mountain colour - manual", cfg.bdMountain);
    I("Presets", "Style", "Style preset", &cfg.stylePreset, 0, 2);
    I("Presets", "Quality", "Quality preset", &cfg.qualityPreset, 0, 2);
    I("Presets", "Look", "Look", &cfg.look, 0, 99);
    I("Presets", "Effects", "Effects preset", &cfg.fxPreset, 0, 2);
}

const std::vector<GfxSetting>& GfxSettings() {
    BuildSettingsTable();
    return g_settings;
}

static std::wstring IniPath() { return g_gameDir + L"FortsGFX.ini"; }

static std::wstring SettingToString(const GfxSetting& s) {
    wchar_t b[96];
    switch (s.type) {
    case GfxSetting::Bool: return *(bool*)s.ptr ? L"1" : L"0";
    case GfxSetting::Int: return std::to_wstring(*(int*)s.ptr);
    case GfxSetting::Float: swprintf(b, 96, L"%g", *(float*)s.ptr); return b;
    case GfxSetting::Vec2: swprintf(b, 96, L"%g,%g", ((float*)s.ptr)[0], ((float*)s.ptr)[1]); return b;
    case GfxSetting::Color: swprintf(b, 96, L"%g,%g,%g", ((float*)s.ptr)[0], ((float*)s.ptr)[1], ((float*)s.ptr)[2]); return b;
    }
    return L"";
}

static void SettingFromString(const GfxSetting& s, const std::wstring& v) {
    switch (s.type) {
    case GfxSetting::Bool: *(bool*)s.ptr = _wtoi(v.c_str()) != 0; break;
    case GfxSetting::Int: *(int*)s.ptr = _wtoi(v.c_str()); break;
    case GfxSetting::Float: *(float*)s.ptr = (float)_wtof(v.c_str()); break;
    case GfxSetting::Vec2:
    case GfxSetting::Color: {
        int n = s.type == GfxSetting::Vec2 ? 2 : 3;
        const wchar_t* p = v.c_str();
        for (int i = 0; i < n && *p; i++) {
            ((float*)s.ptr)[i] = (float)wcstod(p, (wchar_t**)&p);
            while (*p == L',' || *p == L' ') p++;
        }
        break;
    }
    }
}

// Reads a key; if it's missing, writes the default so every tunable shows up in the ini.
static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const std::wstring& def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec, key, L"\x01", buf, 2048, IniPath().c_str());
    if (buf[0] == 1 && buf[1] == 0) {
        WritePrivateProfileStringW(sec, key, def.c_str(), IniPath().c_str());
        return def;
    }
    return buf;
}

static std::vector<std::string> IniList(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    std::string s = Narrow(IniStr(sec, key, def).c_str());
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        std::string item = s.substr(p, q - p);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        std::transform(item.begin(), item.end(), item.begin(), ::tolower);
        if (!item.empty()) out.push_back(item);
        p = q + 1;
    }
    return out;
}

// Settings written by older builds carried much gentler values; when the layout version changes,
// keep a backup and start from the current defaults. From v3 on, upgrades are small in-place steps
// that only retune values still holding the old default, so the user's own changes are kept.
static const int kConfigVersion = 13;
static bool g_fxReset;   // v6 -> v7: looks had turned explosion effects down; put the effects preset back
static bool g_fxGlowSet; // v8 -> v9: effect glow / light joined the effects preset: take the current preset's values
static bool g_lookRefresh; // v9 -> v10 / v11: the Showcase look got brighter, then neutral: re-apply the built-in look in use
static void UpgradeIni() {
    std::wstring ini = IniPath();
    int v = GetPrivateProfileIntW(L"FortsGFX", L"ConfigVersion", 0, ini.c_str());
    if (v == kConfigVersion) return;
    if (v >= 3 && v < kConfigVersion) {
        int changed = 0;
        if (v == 3) {
            struct { const wchar_t *sec, *key; float was, now; } kRetune[] = {
                { L"Post", L"ChromaticAberration", 0.35f, 0.12f },   // Showcase aberration was too strong
                { L"Impact", L"HeatHaze", 0.9f, 0.8f },              // haze now only over real heat sources
                { L"Impact", L"Sparks", 1.8f, 1.4f },
            };
            for (auto& r : kRetune) {
                wchar_t buf[64];
                GetPrivateProfileStringW(r.sec, r.key, L"", buf, 64, ini.c_str());
                if (buf[0] && fabsf((float)_wtof(buf) - r.was) < 1e-4f) {
                    wchar_t nb[32]; swprintf(nb, 32, L"%g", r.now);
                    WritePrivateProfileStringW(r.sec, r.key, nb, ini.c_str());
                    changed++;
                }
            }
        }
        if (v <= 4) {   // weapon reload animations (beam-reloadanim, flakreload...) are not light sources
            wchar_t buf[2048];
            GetPrivateProfileStringW(L"Emitters", L"NameExcludes", L"", buf, 2048, ini.c_str());
            std::wstring ex = buf;
            if (!ex.empty() && ex.find(L"reload") == std::wstring::npos) {
                WritePrivateProfileStringW(L"Emitters", L"NameExcludes", (ex + L",reload").c_str(), ini.c_str());
                changed++;
            }
        }
        if (v <= 6) g_fxReset = true;
        if (v <= 8) g_fxGlowSet = true;
        if (v <= 11) g_lookRefresh = true;   // v12: looks re-balanced so structures aren't darker than the game (measured per look)
        if (v <= 12) { WritePrivateProfileStringW(L"Fun", L"MilkMiko", L"0", ini.c_str()); changed++; }   // v13: the milk miko is opt in (Lab bar)
        if (v <= 7) {   // the screen-space near-rain layer looked like the game's overlay rain: subtle now
            wchar_t buf[64];
            GetPrivateProfileStringW(L"Weather", L"NearRain", L"", buf, 64, ini.c_str());
            if (buf[0] && fabsf((float)_wtof(buf) - 0.7f) < 1e-4f) { WritePrivateProfileStringW(L"Weather", L"NearRain", L"0.25", ini.c_str()); changed++; }
        }
        if (v <= 5) {   // fireball.tga flame puffs from burning structures pulse less (soft glow)
            wchar_t buf[2048];
            GetPrivateProfileStringW(L"Emitters", L"SoftGlowNames", L"", buf, 2048, ini.c_str());
            std::wstring sg = buf;
            if (!sg.empty() && sg.find(L"fireball") == std::wstring::npos) {
                WritePrivateProfileStringW(L"Emitters", L"SoftGlowNames", (sg + L",fireball").c_str(), ini.c_str());
                changed++;
            }
        }
        WritePrivateProfileStringW(L"FortsGFX", L"ConfigVersion", std::to_wstring(kConfigVersion).c_str(), ini.c_str());
        SessionLog("gfx: settings upgraded v%d -> v%d (%d values retuned, your other settings kept)", v, kConfigVersion, changed);
        return;
    }
    CopyFileW(ini.c_str(), (ini + L".bak").c_str(), FALSE);
    for (const wchar_t* s : { L"Post", L"Emitters", L"Lighting", L"Fog", L"Materials", L"Sun" })
        WritePrivateProfileStringW(s, nullptr, nullptr, ini.c_str());
    WritePrivateProfileStringW(L"FortsGFX", L"ConfigVersion", std::to_wstring(kConfigVersion).c_str(), ini.c_str());
    SessionLog("gfx: settings reset to v%d defaults (previous file kept as FortsGFX.ini.bak)", kConfigVersion);
}

void GfxApplyStyle(int preset);
void GfxApplyFx(int preset);
void GfxApplyQuality(int preset);
static void BuildLooks();

static void LoadConfig() {
    UpgradeIni();
    BuildSettingsTable();
    // no preset saved yet (first run of this build): start from the presets, not the raw defaults
    bool firstRun = GetPrivateProfileIntW(L"Presets", L"Style", -1, IniPath().c_str()) == -1;
    cfg = Config{};   // defaults; the table points into cfg
    for (auto& s : g_settings) {
        std::wstring sec = Widen(s.section), key = Widen(s.key);
        SettingFromString(s, IniStr(sec.c_str(), key.c_str(), SettingToString(s)));
    }
    cfg.giScale = std::min(std::max(cfg.giScale, 0.125f), 1.0f);
    cfg.giTemporal = std::min(std::max(cfg.giTemporal, 0.0f), 0.97f);
    cfg.emitNames = IniList(L"Emitters", L"NameContains",
        L"fire,flame,explosion,flash,glow,flare,spark,beam,laser,muzzle,plasma,lightning,emp,reactor_core,"
        L"ember,incendiary,burn,blast,shockwave,tracer,thruster,flak,fireball,additive");
    cfg.emitExclude = IniList(L"Emitters", L"NameExcludes", L"smoke,debris,casing,dust,shadow,barrel,base,head,reload");
    cfg.softGlowNames = IniList(L"Emitters", L"SoftGlowNames", L"glow,fireball");
    cfg.occluderPaths = IniList(L"Lighting", L"SolidPaths", L"materials/,devices/,weapons/");
    BuildLooks();
    if (firstRun) {
        GfxApplyStyle(cfg.stylePreset);
        GfxApplyFx(cfg.fxPreset);
        GfxApplyQuality(cfg.qualityPreset);
        for (auto& s : g_settings) {   // write the preset values so the ini matches what is on screen
            std::wstring sec = Widen(s.section), key = Widen(s.key);
            WritePrivateProfileStringW(sec.c_str(), key.c_str(), SettingToString(s).c_str(), IniPath().c_str());
        }
        SessionLog("gfx: first run - applied the %s style and %s quality presets", cfg.stylePreset == 0 ? "Showcase" : cfg.stylePreset == 1 ? "Cinematic" : "Clean",
                   cfg.qualityPreset == 0 ? "Low" : cfg.qualityPreset == 1 ? "High" : "Ultra");
    }
    if (g_fxReset) {
        g_fxReset = false;
        if (!firstRun) {
            GfxApplyFx(0);
            for (auto& s : g_settings)
                if (!strcmp(s.section, "Impact") || (!strcmp(s.section, "Presets") && !strcmp(s.key, "Effects"))) {
                    std::wstring sec = Widen(s.section), key = Widen(s.key);
                    WritePrivateProfileStringW(sec.c_str(), key.c_str(), SettingToString(s).c_str(), IniPath().c_str());
                }
            SessionLog("gfx: explosion effects back to the Showcase preset (looks no longer change them)");
        }
    }
    if (g_fxGlowSet) {
        g_fxGlowSet = false;
        Config keep = cfg;
        GfxApplyFx(cfg.fxPreset);                     // only the two new values are taken from the preset
        float glow = cfg.effectGlow, light = cfg.effectLight;
        cfg = keep;
        cfg.effectGlow = glow; cfg.effectLight = light;
        wchar_t b[32];
        swprintf(b, 32, L"%g", glow); WritePrivateProfileStringW(L"Impact", L"EffectGlow", b, IniPath().c_str());
        swprintf(b, 32, L"%g", light); WritePrivateProfileStringW(L"Impact", L"EffectLight", b, IniPath().c_str());
        SessionLog("gfx: effect glow %.2f / light %.2f from the effects preset (the same on every look now)", glow, light);
    }
    if (g_lookRefresh) {
        g_lookRefresh = false;
        if (!firstRun && !GfxLookIsCustom(cfg.look)) {
            GfxApplyLook(cfg.look, false);
            for (auto& s : g_settings) {
                std::wstring sec = Widen(s.section), key = Widen(s.key);
                WritePrivateProfileStringW(sec.c_str(), key.c_str(), SettingToString(s).c_str(), IniPath().c_str());
            }
            SessionLog("gfx: your look was re-applied with its updated values");
        }
    }
}

// Effects glow and light as the effects preset says, whatever the look's bloom and light strength.
static float EffectGlowScale() { return std::min(std::max(cfg.effectGlow / std::max(cfg.bloomEmissive, 0.05f), 0.25f), 4.0f); }
static float EffectLightScale() { return std::min(std::max(cfg.effectLight / std::max(cfg.giIntensity, 0.05f), 0.25f), 4.0f); }

void GfxSaveSettings() {
    for (auto& s : g_settings) {
        std::wstring sec = Widen(s.section), key = Widen(s.key);
        WritePrivateProfileStringW(sec.c_str(), key.c_str(), SettingToString(s).c_str(), IniPath().c_str());
    }
    SessionLog("gfx: settings saved to FortsGFX.ini");
}

static void ReclassifyAll();
void GfxReloadSettings() {
    LoadConfig();
    ReclassifyAll();
    SessionLog("gfx: settings reloaded from FortsGFX.ini");
}

// Style presets only touch look-related values; effects presets only how hard impacts hit;
// quality presets only cost-related values. Looks never change the effects preset.
void GfxApplyStyle(int preset) {
    Config d;   // the defaults are the Cinematic look
    auto look = [&](float sat, float vib, float con, float bloom, float vig, float grain, float chroma,
                    float light, float ao, float sun, float spec, float toon, float haze, float banks, float fxSat) {
        cfg.saturation = sat; cfg.vibrance = vib; cfg.contrast = con; cfg.bloomEmissive = bloom;
        cfg.vignette = vig; cfg.grain = grain; cfg.chroma = chroma; cfg.giIntensity = light; cfg.ao = ao;
        cfg.sunStrength = sun; cfg.specular = spec; cfg.toon = toon; cfg.fogHaze = haze; cfg.fogBanks = banks;
        cfg.fxSaturation = fxSat;
    };
    if (preset == 0) {
        // Showcase: the game's own colours, as bright as the game, with FortsGFX's light, glow and
        // effects on top. (Strong saturation amplified each map's dominant colour: warm maps went red.)
        look(1.05f, 0.2f, 1.0f, 1.3f, 0.12f, 0.02f, 0.08f, 1.4f, 0.45f, 0.45f, 1.8f, 0.55f, 0.08f, 0.12f, 1.1f);
        cfg.ambient = 1.0f; cfg.gamma = 1.1f; cfg.temperature = -0.06f;
    }
    else if (preset == 1) look(d.saturation, d.vibrance, d.contrast, d.bloomEmissive, d.vignette, d.grain, d.chroma,
                               d.giIntensity, d.ao, d.sunStrength, d.specular, d.toon, d.fogHaze, d.fogBanks, d.fxSaturation);  // Cinematic
    else look(1.0f, 0.05f, 1.02f, 0.7f, 0.0f, 0.0f, 0.0f, 0.8f, 0.35f, 0.25f, 0.6f, 0.2f, 0.04f, 0.05f, 1.0f);          // Clean
    cfg.stylePreset = preset;
}

// 0 Showcase (over the top), 1 Cinematic, 2 Subtle
void GfxApplyFx(int preset) {
    preset = std::min(std::max(preset, 0), 2);
    Config d;
    auto fx = [&](float shock, float shake, float pulse, float flash, float fbBright, float debris, float sparks,
                  float embers, bool frames, float heat, float glow, float light) {
        cfg.shockStrength = shock; cfg.shake = shake; cfg.pulse = pulse; cfg.flashLight = flash; cfg.fireballBright = fbBright;
        cfg.debris = debris; cfg.sparks = sparks; cfg.embers = embers; cfg.impactFrames = frames; cfg.heatHaze = heat;
        cfg.effectGlow = glow; cfg.effectLight = light;
    };
    // Showcase glow / light = the Showcase look's bloom and light when its explosions were tuned
    if (preset == 0) fx(0.24f, 1.0f, 1.0f, 2.2f, 2.2f, 1.6f, 1.4f, 1.6f, true, 0.8f, 2.0f, 1.6f);
    else if (preset == 1) fx(d.shockStrength, d.shake, d.pulse, d.flashLight, d.fireballBright, d.debris, d.sparks, d.embers,
                             d.impactFrames, d.heatHaze, d.effectGlow, d.effectLight);
    else fx(0.12f, 0.15f, 0.0f, 1.0f, 1.2f, 0.6f, 0.6f, 0.6f, false, 0.3f, 0.7f, 0.8f);
    cfg.fxPreset = preset;
}
void GfxApplyQuality(int preset) {
    preset = std::min(std::max(preset, 0), 2);
    static const float res[3] = { 0.125f, 0.2f, 0.25f };
    cfg.giScale = res[preset];
    cfg.fxaa = preset > 0;
    cfg.fireballs = preset > 0;
    cfg.bdRanges = preset == 0 ? 2 : 3;
    cfg.bdClouds = preset == 0 ? 0.35f : 0.6f;
    cfg.qualityPreset = preset;
}

// ---------------------------------------------------------------- looks
// A look sets every look-related setting: the defaults, optionally a style preset, then a few
// overrides ("Section.Key=value;..."). Custom looks live in the ini: [CustomLooks] LookN=Name|overrides.
struct Look { std::string name, desc, overrides; int style; std::wstring iniKey; };
static std::vector<Look> g_looks;

static bool IsLookSetting(const GfxSetting& s) {
    static const char* kSections[] = { "Post", "Lighting", "Materials", "Sun", "Fog", "Weather", "Backdrop" };
    bool sec = false;
    for (auto* k : kSections) if (!strcmp(s.section, k)) sec = true;
    if (!strcmp(s.section, "Impact")) return !strcmp(s.key, "EffectSaturation");   // explosion strength is the effects preset's
    if (!sec) return false;
    static const char* kNot[] = {
        "Post.Enabled", "Post.ToggleKeyVK", "Post.OverlayKeyVK", "Post.LookPrevKeyVK", "Post.LookNextKeyVK", "Post.LabKeyVK", "Post.PhotoKeyVK",
        "Post.HudVirtualHeight", "Post.FXAA", "Lighting.Enabled", "Lighting.ToggleKeyVK", "Lighting.Resolution",
        "Lighting.TemporalSmoothing", "Lighting.LightSourceOpacity", "Materials.Enabled", "Fog.LightBlurLevel",
        "Impact.Enabled", "Impact.Fireballs", "Weather.ReplaceGameRain", "Weather.Thunder", "Weather.ThunderVolume",
        "Backdrop.Enabled", "Backdrop.ReplaceLayers", "Backdrop.AutoReplaceFraction", "Backdrop.MountainRanges",
        "Backdrop.ReferenceWidth", "Emitters.SoftGlowLight", "DayNight.Enabled", "DayNight.CycleMinutes", "DayNight.StartHour" };
    std::string id = std::string(s.section) + "." + s.key;
    for (auto* k : kNot) if (id == k) return false;
    return true;
}

static size_t SettingSize(const GfxSetting& s) {
    switch (s.type) {
    case GfxSetting::Bool: return sizeof(bool);
    case GfxSetting::Int: return sizeof(int);
    case GfxSetting::Float: return sizeof(float);
    case GfxSetting::Vec2: return 2 * sizeof(float);
    default: return 3 * sizeof(float);
    }
}

static void ResetLookSettings() {
    static const Config d;
    for (auto& s : g_settings)
        if (IsLookSetting(s)) {
            size_t off = (const char*)s.ptr - (const char*)&cfg;
            memcpy(s.ptr, (const char*)&d + off, SettingSize(s));
        }
}

static std::string TrimS(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static void ApplyOverrides(const std::string& ov) {
    size_t p = 0;
    while (p < ov.size()) {
        size_t q = ov.find(';', p);
        if (q == std::string::npos) q = ov.size();
        std::string item = ov.substr(p, q - p);
        p = q + 1;
        size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string id = TrimS(item.substr(0, eq)), val = TrimS(item.substr(eq + 1));
        for (auto& s : g_settings)
            if (id == std::string(s.section) + "." + s.key) { SettingFromString(s, Widen(val)); break; }
    }
}

static void BuildLooks() {
    g_looks.clear();
    auto Add = [](const char* name, const char* desc, int style, const char* ov) { g_looks.push_back({ name, desc, ov, style, std::wstring() }); };
    Add("Showcase", "The game's own colours with FortsGFX's light, glow and effects on top", 0, "");
    Add("Cinematic", "Balanced film look, the FortsGFX defaults", 1, ";Materials.AmbientLevel=1.46");
    Add("Clean", "Restrained: the game's colours with light and depth", 2, ";Materials.AmbientLevel=1.14");
    Add("Vivid Cartoon", "Saturated toon shading with soft ink lines", 0,
      "Post.Saturation=1.45;Post.Vibrance=0.6;Post.Contrast=1.12;Post.BloomEmissive=1.6;Post.Vignette=0.12;Post.Grain=0;"
      "Post.Sharpen=0.5;Post.ChromaticAberration=0;Materials.ToonBanding=0.85;Materials.EdgeBevel=2;Materials.Specular=1.4;"
      "Lighting.Intensity=1.3;Post.InkOutlines=0.6;Materials.AmbientLevel=1.61");
    Add("Comic Book", "Bold ink outlines and posterized colour", 0,
      "Post.InkOutlines=1.4;Post.Posterize=6;Post.Saturation=1.35;Post.Contrast=1.2;Materials.ToonBanding=1;Post.Grain=0;"
      "Post.Vignette=0.1;Post.Sharpen=0.6;Post.ChromaticAberration=0;Materials.AmbientLevel=1.99");
    Add("Golden Hour", "Low warm sun, long glow, warm haze", 1,
      "Post.Temperature=0.55;Post.Saturation=1.2;Post.Contrast=1.08;Post.BloomEmissive=1.5;Post.BloomScene=0.3;"
      "Sun.Direction=-0.95,0.3;Sun.Color=1,0.72,0.42;Sun.OpenAreaBrightening=0.7;Backdrop.SunGlow=2.2;Fog.Enabled=1;"
      "Fog.Density=0.6;Fog.Color=0.95,0.7,0.45;Post.SplitTone=0.35;Post.ShadowTint=0.42,0.45,0.6;Post.HighlightTint=0.65,0.55,0.4;"
      "Post.Vignette=0.3;Materials.AmbientLevel=1.63");
    Add("Moonlit Night", "Dark blue night: every fire and laser becomes a light source", 1,
      "Post.Exposure=0.7;Post.Temperature=-0.6;Post.Saturation=0.8;Materials.AmbientLevel=1.09;Lighting.EmitterBrightness=10;"
      "Lighting.Intensity=1.8;Sun.OpenAreaBrightening=0.15;Sun.Color=0.6,0.7,1;Sun.Direction=0.4,0.9;Backdrop.ColoursFromMap=0;"
      "Backdrop.SkyTop=0.02,0.03,0.09;Backdrop.SkyHorizon=0.1,0.12,0.25;Backdrop.Mountain=0.06,0.07,0.13;Backdrop.Stars=2;"
      "Backdrop.SunGlow=0.4;Post.BloomEmissive=2;Post.SplitTone=0.4;Post.ShadowTint=0.35,0.42,0.7;Post.HighlightTint=0.55,0.55,0.6;"
      "Post.Vignette=0.45");
    Add("Thunderstorm", "Heavy rain, frequent lightning, cold light", 1,
      "Weather.Mode=3;Weather.StrikesPerMinute=14;Weather.RainAmount=1.8;Weather.NearRain=0.5;Post.Saturation=0.75;"
      "Post.Temperature=-0.35;Post.Contrast=1.1;Post.Exposure=0.85;Materials.AmbientLevel=1.38;Fog.Enabled=1;Fog.Density=1;"
      "Fog.Color=0.45,0.5,0.58;Backdrop.Clouds=1;Backdrop.CloudSpeed=3;Sun.OpenAreaBrightening=0.1;Post.Vignette=0.35");
    Add("Gritty War", "Bleach bypass: desaturated, harsh, dusty, heavy shake", 1,
      "Post.Saturation=0.55;Post.Contrast=1.3;Post.Grain=0.06;Post.Vignette=0.4;Post.Temperature=0.1;Post.Tint=-0.1;"
      "Post.Sharpen=0.6;Post.SplitTone=0.25;Post.ShadowTint=0.42,0.47,0.45;Post.HighlightTint=0.58,0.55,0.48;"
      "Fog.Enabled=1;Fog.Density=0.5;Fog.Color=0.5,0.48,0.42;Post.BloomEmissive=0.8;Materials.AmbientLevel=1.81");
    Add("Film Noir", "Black and white, deep shadows, hard light", 1,
      "Post.Saturation=0;Post.Contrast=1.45;Post.Vignette=0.6;Post.Grain=0.07;Post.Exposure=0.9;Materials.AmbientLevel=1.99;"
      "Lighting.Intensity=2;Post.BloomEmissive=1.2;Impact.EffectSaturation=0");
    Add("Old Film", "Sepia, grain, vignette and a little fringing", 1,
      "Post.Saturation=0.15;Post.SplitTone=0.8;Post.ShadowTint=0.42,0.33,0.25;Post.HighlightTint=0.68,0.6,0.45;Post.Grain=0.1;"
      "Post.Vignette=0.55;Post.Contrast=1.15;Post.ChromaticAberration=0.3;Post.Sharpen=0;Post.Lift=0.05;Materials.AmbientLevel=1.88");
    Add("Neon Synthwave", "Magenta and cyan, glowing everything, retro sky", 0,
      "Post.SplitTone=0.7;Post.ShadowTint=0.35,0.3,0.65;Post.HighlightTint=0.7,0.4,0.62;Post.BloomEmissive=3;Post.Saturation=1.5;"
      "Post.Contrast=1.18;Lighting.EmitterBrightness=12;Materials.AmbientLevel=1.73;Post.Exposure=0.85;Backdrop.ColoursFromMap=0;"
      "Backdrop.SkyTop=0.05,0.02,0.15;Backdrop.SkyHorizon=0.8,0.2,0.5;Backdrop.Mountain=0.12,0.05,0.25;Backdrop.Stars=1.5;"
      "Sun.Color=1,0.4,0.8;Post.ChromaticAberration=0.4;Impact.EffectSaturation=2");
    Add("Pastel Dream", "Soft, low contrast, glowing haze", 1,
      "Post.Contrast=0.85;Post.Lift=0.08;Post.Saturation=0.9;Post.BloomScene=0.45;Post.BloomEmissive=2;Post.BloomRadius=2;"
      "Post.Vignette=0.1;Post.Grain=0;Fog.Enabled=1;Fog.Density=0.6;Fog.Color=0.9,0.82,0.9;Post.SplitTone=0.3;"
      "Post.ShadowTint=0.5,0.45,0.62;Post.HighlightTint=0.62,0.55,0.5;Materials.ToonBanding=0.6");
    Add("Arcade Punch", "Maximum contrast, colour and impact", 0,
      "Post.Contrast=1.35;Post.Saturation=1.7;Post.Vibrance=0.8;Post.Sharpen=0.9;Post.Grain=0;Post.Vignette=0;"
      "Post.BloomEmissive=2.5;"
      "Impact.EffectSaturation=2;Materials.Specular=2;Materials.AmbientLevel=1.6");
    Add("Misty Morning", "Thick cool fog with light shafts", 1,
      "Fog.Enabled=1;Fog.Density=1.25;Fog.BackdropHaze=0.16;Fog.DriftingBanks=0.28;Fog.Color=0.72,0.77,0.82;Post.Contrast=0.95;"
      "Post.Saturation=0.85;Post.Temperature=-0.2;Sun.OpenAreaBrightening=0.5;Sun.Direction=-0.7,0.45;Fog.LightScatter=1.4;"
      "Backdrop.Clouds=0.8;Materials.AmbientLevel=1.15");
    Add("Hellfire", "Burning red sky, embers and heat everywhere", 0,
      "Post.Temperature=0.6;Post.Tint=0.2;Post.Saturation=1.3;Post.Contrast=1.2;Post.Exposure=0.85;Post.SplitTone=0.6;"
      "Post.ShadowTint=0.55,0.3,0.25;Post.HighlightTint=0.72,0.5,0.3;"
      "Lighting.EmitterBrightness=10;Backdrop.ColoursFromMap=0;Backdrop.SkyTop=0.25,0.04,0.02;Backdrop.SkyHorizon=0.9,0.35,0.08;"
      "Backdrop.Mountain=0.2,0.06,0.04;Materials.AmbientLevel=1.5;Fog.Enabled=1;Fog.Color=0.6,0.25,0.1;Fog.Density=0.7");
    Add("Realistic Preview", "A taste of a realistic grade: no toon bands, softer effects", 1,
      "Materials.ToonBanding=0;Post.Saturation=0.95;Post.Contrast=1.05;Lighting.Intensity=1.2;Materials.Specular=0.8;"
      "Sun.OpenAreaBrightening=0.45;Fog.Enabled=1;Fog.Density=0.5;Post.Grain=0.015;Post.Vignette=0.2;"
      "Materials.EdgeBevel=0.6;Materials.AmbientLevel=1.4");
    Add("Vanilla+", "Close to the original game, just a little light and polish", 2,
      "Post.Saturation=1;Post.Contrast=1;Post.Vibrance=0;Post.Vignette=0;Post.Grain=0;Post.Sharpen=0.2;Post.BloomEmissive=0.6;"
      "Lighting.Intensity=0.6;Materials.ToonBanding=0;Materials.Specular=0.3;"
      ";Materials.AmbientLevel=1.07");
    Add("Snowfall", "Gentle snow settling on everything, cool soft light", 1,
      "Weather.Mode=4;Post.Temperature=-0.3;Post.Saturation=0.9;Materials.AmbientLevel=1.61;Backdrop.ColoursFromMap=0;"
      "Backdrop.SkyTop=0.52,0.6,0.72;Backdrop.SkyHorizon=0.82,0.85,0.9;Backdrop.Mountain=0.5,0.54,0.62;Sun.OpenAreaBrightening=0.2;"
      "Post.Vignette=0.2;Post.BloomScene=0.2");
    Add("Blizzard", "Thick snow, gusts and a white-out", 1,
      "Weather.Mode=5;Post.Temperature=-0.45;Post.Saturation=0.75;Post.Contrast=0.95;Materials.AmbientLevel=1.32;"
      "Backdrop.ColoursFromMap=0;Backdrop.SkyTop=0.58,0.63,0.7;Backdrop.SkyHorizon=0.85,0.87,0.9;Backdrop.Mountain=0.6,0.63,0.68;"
      "Backdrop.Clouds=1;Backdrop.CloudSpeed=4;Sun.OpenAreaBrightening=0.05;Post.Vignette=0.3");
    Add("Sandstorm", "A desert storm: driven sand, dust sheets, drifts piling against the forts", 1,
      "Weather.Mode=6;Weather.WindLean=0.8;Post.Temperature=0.3;Post.Saturation=0.85;Post.Contrast=1.05;Materials.AmbientLevel=1.42;"
      "Backdrop.Clouds=1;Backdrop.CloudSpeed=5;Post.Vignette=0.35");
    Add("Fog Banks", "Thick low fog rolling through the valleys; lights glow through it", 1,
      "Weather.Mode=7;Post.Saturation=0.85;Post.Contrast=0.95;Post.Temperature=-0.1;Sun.OpenAreaBrightening=0.3;Post.Vignette=0.25;Materials.AmbientLevel=1.16");
    Add("Ashfall", "A burning world: falling ash, embers on the wind, a dark red sky", 0,
      "Weather.Mode=8;Post.Temperature=0.3;Post.Saturation=0.85;Post.Contrast=1.15;Materials.AmbientLevel=1.26;"
      "Lighting.EmitterBrightness=9;Post.Vignette=0.45;Post.SplitTone=0.4;Post.ShadowTint=0.45,0.38,0.38;Post.HighlightTint=0.65,0.48,0.36");
    // low ambient light so the ray-traced light and shadows carry the scene: fires and blasts light up the forts
    Add("Dramatic Light", "Low ambient light: every fire, laser and blast visibly lights and shadows the forts", 0,
      "Materials.AmbientLevel=0.67;Lighting.Intensity=2.2;Lighting.AmbientOcclusion=0.85;Sun.OpenAreaBrightening=0.3;"
      "Post.BloomEmissive=1.2;Post.Gamma=1.05");
    for (int i = 1; i <= 64; i++) {   // custom looks
        std::wstring key = L"Look" + std::to_wstring(i);
        wchar_t buf[8192];
        GetPrivateProfileStringW(L"CustomLooks", key.c_str(), L"", buf, 8192, IniPath().c_str());
        if (!buf[0]) continue;
        std::string v = Narrow(buf);
        size_t bar = v.find('|');
        if (bar == std::string::npos) continue;
        g_looks.push_back({ v.substr(0, bar), "Your saved look", v.substr(bar + 1), -1, key });
    }
}

int GfxLookCount() { return (int)g_looks.size(); }
const char* GfxLookName(int i) { return i >= 0 && i < (int)g_looks.size() ? g_looks[i].name.c_str() : ""; }
const char* GfxLookDesc(int i) { return i >= 0 && i < (int)g_looks.size() ? g_looks[i].desc.c_str() : ""; }
bool GfxLookIsCustom(int i) { return i >= 0 && i < (int)g_looks.size() && !g_looks[i].iniKey.empty(); }
int* GfxCurrentLook() { return &cfg.look; }

// Writes all settings to the ini on a worker thread (a few hundred WritePrivateProfileString calls
// would hitch the frame); the newest request wins.
static std::mutex g_saveMx;
static void SaveSettingsAsync() {
    auto items = std::make_shared<std::vector<std::wstring>>();
    for (auto& s : g_settings) { items->push_back(Widen(s.section)); items->push_back(Widen(s.key)); items->push_back(SettingToString(s)); }
    std::wstring ini = IniPath();
    std::thread([items, ini] {
        std::lock_guard<std::mutex> lk(g_saveMx);
        for (size_t i = 0; i + 2 < items->size(); i += 3)
            WritePrivateProfileStringW((*items)[i].c_str(), (*items)[i + 1].c_str(), (*items)[i + 2].c_str(), ini.c_str());
    }).detach();
}

void GfxApplyLook(int i, bool keep) {
    if (g_looks.empty()) BuildLooks();
    int n = (int)g_looks.size();
    if (n == 0) return;
    i = ((i % n) + n) % n;
    const Look& l = g_looks[i];
    ResetLookSettings();
    if (l.style >= 0) GfxApplyStyle(l.style);
    ApplyOverrides(l.overrides);
    cfg.look = i;
    if (keep) SaveSettingsAsync();   // the look you land on is kept (the tour passes false)
    SessionLog("gfx: look %d/%d \"%s\"", i + 1, n, l.name.c_str());
}

// Saves the current look-related settings (those that differ from the defaults) as a new look.
int GfxSaveCustomLook(const char* name) {
    static const Config d;
    std::string ov;
    for (auto& s : g_settings) {
        if (!IsLookSetting(s)) continue;
        size_t off = (const char*)s.ptr - (const char*)&cfg;
        if (memcmp(s.ptr, (const char*)&d + off, SettingSize(s)) == 0) continue;
        ov += std::string(s.section) + "." + s.key + "=" + Narrow(SettingToString(s).c_str()) + ";";
    }
    std::wstring key;
    for (int i = 1; i <= 64; i++) {
        std::wstring k = L"Look" + std::to_wstring(i);
        wchar_t buf[8];
        GetPrivateProfileStringW(L"CustomLooks", k.c_str(), L"", buf, 8, IniPath().c_str());
        if (!buf[0]) { key = k; break; }
    }
    if (key.empty()) return -1;
    std::string nm = name && *name ? name : "My look";
    std::replace(nm.begin(), nm.end(), '|', '/');
    WritePrivateProfileStringW(L"CustomLooks", key.c_str(), Widen(nm + "|" + ov).c_str(), IniPath().c_str());
    g_looks.push_back({ nm, "Your saved look", ov, -1, key });
    cfg.look = (int)g_looks.size() - 1;
    SaveSettingsAsync();
    SessionLog("gfx: saved custom look \"%s\" (%zu chars)", nm.c_str(), ov.size());
    return cfg.look;
}

void GfxDeleteCustomLook(int i) {
    if (!GfxLookIsCustom(i)) return;
    WritePrivateProfileStringW(L"CustomLooks", g_looks[i].iniKey.c_str(), nullptr, IniPath().c_str());
    SessionLog("gfx: deleted custom look \"%s\"", g_looks[i].name.c_str());
    g_looks.erase(g_looks.begin() + i);
    if (cfg.look >= (int)g_looks.size()) cfg.look = 0;
}

// Snapshot / restore of every setting (the tour changes many and puts them back afterwards).
std::vector<uint8_t> GfxSnapshot() {
    std::vector<uint8_t> out;
    for (auto& s : g_settings) { size_t n = SettingSize(s); out.insert(out.end(), (uint8_t*)s.ptr, (uint8_t*)s.ptr + n); }
    out.push_back((uint8_t)g_debugView);
    return out;
}
void GfxRestore(const std::vector<uint8_t>& snap) {
    size_t p = 0;
    for (auto& s : g_settings) {
        size_t n = SettingSize(s);
        if (p + n > snap.size()) return;
        memcpy(s.ptr, &snap[p], n);
        p += n;
    }
    if (p < snap.size()) g_debugView = snap[p];
}

// ---------------------------------------------------------------- day / night
// A clock (real time, not slowed by the lab) drives the sun along an arc: its direction, colour and
// strength, ambient light, exposure, white balance and light-source brightness. Dawn and dusk turn
// golden, the night is lit by a cool moon and stars, and everything that burns or glows stands out.
// It only scales the user's settings at the moment uniforms are set, never changes them.
struct DayNight {
    float day = 1, golden = 0, sunDir[2] = { -0.64f, 0.77f }, sunCol[3] = { 1, 1, 1 };
    float sunMul = 1, ambMul = 1, expoMul = 1, tempAdd = 0, emitMul = 1, stars = 0, bgTint[3] = { 1, 1, 1 };
};
static DayNight g_dn;
static float g_tod = -1;          // time of day, hours
static bool g_todPaused;
float* GfxTimeOfDay() { if (g_tod < 0) g_tod = cfg.dayStart; return &g_tod; }
bool* GfxTimePaused() { return &g_todPaused; }

static void UpdateDayNight() {
    static LARGE_INTEGER f, last;
    LARGE_INTEGER nq;
    QueryPerformanceCounter(&nq);
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); last = nq; }
    float dt = std::min((float)((nq.QuadPart - last.QuadPart) / (double)f.QuadPart), 0.25f);
    last = nq;
    if (g_tod < 0) g_tod = cfg.dayStart;
    if (cfg.dayNight && !g_todPaused && cfg.dayMinutes > 0) g_tod = fmodf(g_tod + dt * 24.0f / (cfg.dayMinutes * 60.0f), 24.0f);
    DayNight d;
    d.sunDir[0] = cfg.sunDir[0]; d.sunDir[1] = cfg.sunDir[1];
    for (int k = 0; k < 3; k++) d.sunCol[k] = cfg.sunColor[k];
    if (!cfg.dayNight) { g_dn = d; return; }
    auto ss = [](float a, float b, float x) { float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f); return t * t * (3 - 2 * t); };
    float e = (g_tod - 6.0f) / 12.0f * 3.14159265f, sE = sinf(e), cE = cosf(e);
    d.day = ss(-0.10f, 0.25f, sE);
    d.golden = ss(-0.25f, 0.03f, sE) * (1.0f - ss(0.03f, 0.3f, sE)) * cfg.duskGlow;   // the glow around sunrise and sunset
    if (sE > -0.05f) { d.sunDir[0] = cE; d.sunDir[1] = std::max(sE, 0.06f); }   // the sun, east to west
    else { d.sunDir[0] = -cE; d.sunDir[1] = std::max(-sE, 0.06f); }       // the moon on the other side
    const float moon[3] = { 0.45f, 0.55f, 1.0f }, gold[3] = { 1.0f, 0.62f, 0.38f };
    for (int k = 0; k < 3; k++) {
        float dayc = cfg.sunColor[k] + (gold[k] - cfg.sunColor[k]) * d.golden;
        d.sunCol[k] = moon[k] + (dayc - moon[k]) * d.day;
    }
    d.sunMul = (0.25f + 0.75f * d.day) * (1.0f - 0.3f * d.golden);
    d.ambMul = (1.0f - 0.75f * cfg.nightDark * (1.0f - d.day)) * (1.0f - 0.15f * d.golden);
    d.expoMul = 0.82f + 0.18f * d.day;
    d.tempAdd = 0.45f * d.golden - 0.7f * (1.0f - d.day) * cfg.nightDark;
    d.emitMul = cfg.nightLights + (1.0f - cfg.nightLights) * d.day;
    d.stars = (1.0f - d.day) * 3.0f;
    const float night[3] = { 0.10f, 0.14f, 0.28f }, dusk[3] = { 1.15f, 0.72f, 0.45f };
    for (int k = 0; k < 3; k++) {
        float tnt = 1.0f + (night[k] - 1.0f) * (1.0f - d.day) * std::min(cfg.nightDark * 1.25f, 1.0f);
        d.bgTint[k] = tnt * (1.0f + (dusk[k] - 1.0f) * d.golden);
    }
    g_dn = d;
}

int* GfxDebugView() { return &g_debugView; }

// ---------------------------------------------------------------- emitter classification

static std::mutex g_emitMx;
static std::unordered_set<GLuint> g_emitTex, g_excludedTex, g_solidSet, g_effectSet, g_alphaFmtSet, g_softGlowSet;
static std::unordered_set<GLuint> g_beamSet, g_bgSet;   // lasers / beams; the fort's back wall (bracing-bg, *_background)
static std::unordered_set<GLuint> g_projSet;            // missile and rocket bodies in flight (cast shadows with GameFx.InFlight)

static void SetEventClass(GLuint tex, const std::string& base, const std::string& path);   // effects section; caller holds g_emitMx
static void PuffOf(GLuint tex, uint8_t& puff, bool& extra);

static void Classify(GLuint tex, const std::string& src) {
    std::string path = src;
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    std::string base = path.substr(path.find_last_of('/') + 1);
    bool excluded = false, named = false, solid = false, effect = path.find("effects/") != std::string::npos;
    // HUD art (bushfireicon, firealarm, tip-mortarexplosion...) never lights the world or fires effects
    bool ui = path.compare(0, 3, "ui/") == 0 || path.find("/ui/") != std::string::npos;
    if (ui) excluded = true;
    if (base.find("blastdoor") != std::string::npos) excluded = true;   // a fort material, not a blast
    for (auto& x : cfg.emitExclude) if (base.find(x) != std::string::npos) { excluded = true; break; }
    if (!excluded)
        for (auto& n : cfg.emitNames) if (base.find(n) != std::string::npos) { named = true; break; }
    if (!named)
        for (auto& p : cfg.occluderPaths) if (path.find(p) != std::string::npos) { solid = true; break; }
    bool soft = false;
    for (auto& n : cfg.softGlowNames) if (base.find(n) != std::string::npos) { soft = true; break; }
    auto bh = [&](const char* s) { return base.find(s) != std::string::npos; };
    // the beam itself (effects/media/laserfx_*, weapons/media/beam, firebeam/beam.dds), never the weapon sprites or the aim line
    bool beam = !ui && !bh("sight") && ((effect && (bh("laser") || bh("beam"))) || base.compare(0, 5, "beam.") == 0);
    bool bg = bh("-bg") || bh("_bg") || bh("background") || bh("backbracing");
    bool proj = !ui && path.find("projectiles/") != std::string::npos && (bh("missile") || bh("rocket") || bh("warhead")) &&
                !bh("tail") && !bh("trail") && !bh("flare") && !bh("glow") && !bh("smoke");
    std::lock_guard<std::mutex> lk(g_emitMx);
    if (named) g_emitTex.insert(tex); else g_emitTex.erase(tex);
    if (soft) g_softGlowSet.insert(tex); else g_softGlowSet.erase(tex);
    if (beam) g_beamSet.insert(tex); else g_beamSet.erase(tex);
    if (bg) g_bgSet.insert(tex); else g_bgSet.erase(tex);
    if (proj) g_projSet.insert(tex); else g_projSet.erase(tex);
    if (excluded) g_excludedTex.insert(tex); else g_excludedTex.erase(tex);
    if (solid) g_solidSet.insert(tex); else g_solidSet.erase(tex);
    if (effect) g_effectSet.insert(tex); else g_effectSet.erase(tex);
    SetEventClass(tex, ui ? std::string() : base, path);
}

static void ReclassifyAll() {
    TexForEach([](GLuint id, const std::string& src) { Classify(id, src); });
}

void GfxClassifyTexture(GLuint tex, const std::string& src) {
    static bool loaded;
    if (!loaded) { loaded = true; LoadConfig(); }
    Classify(tex, src);
}

void GfxForgetTexture(GLuint tex) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    g_emitTex.erase(tex);
    g_excludedTex.erase(tex);
    g_solidSet.erase(tex);
    g_effectSet.erase(tex);
    g_alphaFmtSet.erase(tex);
    g_softGlowSet.erase(tex);
    g_beamSet.erase(tex);
    g_bgSet.erase(tex);
    g_projSet.erase(tex);
    SetEventClass(tex, "", "");
}

// Alpha-only textures sample as (0,0,0,a) in GLSL but as (1,1,1,a) in fixed-function MODULATE.
void GfxNoteTextureFormat(GLuint tex, GLint ifmt) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    if (ifmt == GL_ALPHA_FMT || ifmt == GL_ALPHA8) g_alphaFmtSet.insert(tex); else g_alphaFmtSet.erase(tex);
}

// ---------------------------------------------------------------- GL state save / restore

static const int kUnits = 15;   // texture units our passes may bind

struct StateGuard {
    GLint prog = 0, vao = 0, active = 0, tex[kUnits] = {}, drawFbo = 0, readFbo = 0, vp[4] = {};
    GLint srcRGB = 1, dstRGB = 0, srcA = 1, dstA = 0, eqRGB = GL_FUNC_ADD, eqA = GL_FUNC_ADD, poly[2] = { GL_FILL, GL_FILL };
    GLboolean blend = 0, alpha = 0, scissor = 0, depth = 0, stencil = 0, cull = 0, srgb = 0, mask[4] = { 1, 1, 1, 1 };

    void Save() {
        R.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
        R.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        R.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (int i = 0; i < kUnits; i++) { G.ActiveTexture(GL_TEXTURE0 + i); R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]); }
        G.ActiveTexture(active);
        R.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        R.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        R.GetIntegerv(GL_VIEWPORT, vp);
        R.GetIntegerv(GL_BLEND_SRC_RGB, &srcRGB); R.GetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        R.GetIntegerv(GL_BLEND_SRC_ALPHA, &srcA); R.GetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        R.GetIntegerv(GL_BLEND_EQUATION_RGB, &eqRGB); R.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
        R.GetIntegerv(GL_POLYGON_MODE, poly);
        blend = R.IsEnabled(GL_BLEND); alpha = R.IsEnabled(GL_ALPHA_TEST); scissor = R.IsEnabled(GL_SCISSOR_TEST);
        depth = R.IsEnabled(GL_DEPTH_TEST); stencil = R.IsEnabled(GL_STENCIL_TEST); cull = R.IsEnabled(GL_CULL_FACE);
        srgb = R.IsEnabled(GL_FRAMEBUFFER_SRGB);
        GLint m[4] = { 1, 1, 1, 1 };
        R.GetIntegerv(GL_COLOR_WRITEMASK, m);
        for (int i = 0; i < 4; i++) mask[i] = m[i] != 0;
    }
    // Neutral state for our full-screen passes.
    void Neutral() {
        G.Disable(GL_BLEND); G.Disable(GL_ALPHA_TEST); G.Disable(GL_SCISSOR_TEST); G.Disable(GL_DEPTH_TEST);
        G.Disable(GL_STENCIL_TEST); G.Disable(GL_CULL_FACE); G.Disable(GL_FRAMEBUFFER_SRGB);
        G.ColorMask(1, 1, 1, 1);
        G.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    void Restore(bool leaveBackBuffer) {
        auto en = [](GLenum cap, GLboolean on) { on ? G.Enable(cap) : G.Disable(cap); };
        en(GL_BLEND, blend); en(GL_ALPHA_TEST, alpha); en(GL_SCISSOR_TEST, scissor); en(GL_DEPTH_TEST, depth);
        en(GL_STENCIL_TEST, stencil); en(GL_CULL_FACE, cull); en(GL_FRAMEBUFFER_SRGB, srgb);
        G.BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
        G.BlendEquationSeparate(eqRGB, eqA);
        G.ColorMask(mask[0], mask[1], mask[2], mask[3]);
        G.PolygonMode(GL_FRONT, poly[0]);
        G.PolygonMode(GL_BACK, poly[1]);
        for (int i = 0; i < kUnits; i++) { G.ActiveTexture(GL_TEXTURE0 + i); G.BindTexture(GL_TEXTURE_2D, tex[i]); }
        G.ActiveTexture(active);
        G.BindVertexArray(vao);
        G.UseProgram(prog);
        if (leaveBackBuffer) G.BindFramebuffer(GL_FRAMEBUFFER, 0);
        else { G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo); G.BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo); }
        G.Viewport(vp[0], vp[1], vp[2], vp[3]);
    }
};

// ---------------------------------------------------------------- resources

struct Target { GLuint tex = 0, tex2 = 0, fbo = 0; int w = 0, h = 0; };
static const int kMaxBloom = 8, kMaxPyramid = 8, kDebugViews = 5;
static GLuint g_worldFbo, g_sceneTex, g_emisTex, g_solidTex, g_normTex, g_vao;
// world material shader (replaces fixed-function during the world pass)
static GLuint g_pWorld;
static GLint g_locMode, g_locClass, g_locAlpha, g_locMat, g_locTex, g_locFx;
static bool g_worldBound;
static Target g_bloom[kMaxBloom], g_ldr, g_ldr2, g_bgBlur[2], g_bloomAvg[2];   // g_bloomAvg: 1x1, ping-pong
static int g_bloomAvgCur;
// effects: GPU particle state (tex = pos x, y, depth, life; tex2 = vel x, y, z, type) and an FBO
// over the world's scene + light-source targets for things drawn into the world before lighting
static const int kPW = 256, kPH = 64, kMaxParticles = kPW * kPH;
static Target g_parts[2];
static GLuint g_fxFbo;
static Target g_decalEmis;   // half-res copy of the light-source target: scorch marks stay under glowing effects
static GLuint g_pFireball, g_pPartUpdate, g_pPartDraw, g_pBolt, g_pBackdrop, g_pDecal, g_pMilk;
static GLuint g_pSprite, g_pStream, g_pBeamFx;
static Target g_milkT;   // the milk's density (half res)
static Target g_bdHalf;        // 3D backdrop, rendered at half resolution
static GLuint g_bdSceneFbo;    // just the scene target, to place the backdrop behind the kept layers
static int g_bloomLevels, g_w, g_h;
// lighting, at working resolution
static Target g_giScene, g_jfa[2], g_sdf, g_cascade[2], g_hist[2], g_pull[kMaxPyramid], g_push[kMaxPyramid], g_debug[kDebugViews];
static int g_gw, g_gh, g_cascades, g_pyramid, g_rcOut, g_gridP = 4, g_histCur;
static GLuint g_lightTex, g_lightTex2;    // temporally smoothed cascade result (light+sky, direction)
struct GridMap { float ox = 0, oy = 0, p = 4, l = 0, b = 0, wx = 1, wy = 1; };
static GridMap g_map, g_prevMap;
static bool g_histValid;
static float g_giScaleBuilt;
static GLuint g_pPrefilter, g_pDown, g_pUp, g_pComposite, g_pFxaa, g_pFinal, g_pBloomAvg;
static GLuint g_pSnowStep, g_pSnowDep, g_pSnowCopy;   // snow physics
static GLuint g_pBlur;                                // photo mode depth of field
static GLuint g_pTerrain;                             // rasterises the terrain mask
static GLuint g_pCopy, g_pBgDown, g_pTemporal, g_pGiPrep, g_pJfaSeed, g_pJfaStep, g_pJfaDist, g_pCascade, g_pPull0, g_pPull, g_pPush, g_pDebug;
static bool g_procsLoaded, g_progsBuilt, g_ready, g_failed;

enum Phase { kOff, kWorld, kHud };
static Phase g_phase = kOff;
static bool g_projHud, g_postRan, g_playfield, g_debugRan;
static bool g_hudMasked;   // lab "hide HUD": colour writes are off from the end of the post chain to the swap
bool GfxHudMasked() { return g_hudMasked; }
static unsigned g_frameNo;
static GLuint g_outFbo;   // final pass target: 0 = back buffer (tools/replay.cpp redirects it)
static bool g_bridge;     // a Vulkan frame (GfxBridgeFrame): only the image stages, nothing that needs the world
// GPU stage timers, only used by tools/replay.cpp
static bool g_profile;
static GLuint g_timers[16];
static int g_timerCount;
static const char* g_timerNames[16];
static void Mark(const char* name) {
    if (!g_profile || !G.QueryCounter || g_timerCount >= 16) return;
    if (!g_timers[0]) G.GenQueries(16, g_timers);
    g_timerNames[g_timerCount] = name;
    G.QueryCounter(g_timers[g_timerCount++], 0x8E28);   // GL_TIMESTAMP
}

// In-game performance log: frame time, GPU time of our post chain (read back a few frames late so
// it never stalls), and CPU time spent in the per-draw hooks. Written to session.log every 5 s.
static struct {
    GLuint q[4] = {};
    bool queued[4] = {};
    int idx = 0;
    double gpuMs = 0, frameMs = 0, drawMs = 0, drawMsFrame = 0, lastFrame = 0, lastGpu = 0, lastDraw = 0, worst = 0;
    int gpuN = 0, frames = 0;
    LARGE_INTEGER last = {}, freq = {};
    ULONGLONG logAt = 0;
} P;

static double QpcMs(LARGE_INTEGER a, LARGE_INTEGER b) { return (b.QuadPart - a.QuadPart) * 1000.0 / P.freq.QuadPart; }

static void PerfBeginPost() {
    if (!G.BeginQuery || g_profile) return;
    if (!P.q[0]) G.GenQueries(4, P.q);
    int i = P.idx;
    if (P.queued[i]) {
        GLint avail = 0;
        G.GetQueryObjectiv(P.q[i], 0x8867, &avail);   // GL_QUERY_RESULT_AVAILABLE
        if (avail) {
            unsigned long long ns = 0;
            G.GetQueryObjectui64v(P.q[i], 0x8866, &ns);
            P.gpuMs += ns / 1e6;
            P.lastGpu = ns / 1e6;
            P.gpuN++;
        }
        P.queued[i] = false;
    }
    G.BeginQuery(0x88BF, P.q[i]);   // GL_TIME_ELAPSED
}

static void PerfEndPost() {
    if (!G.BeginQuery || g_profile) return;
    G.EndQuery(0x88BF);
    P.queued[P.idx] = true;
    P.idx = (P.idx + 1) % 4;
}

static void PerfFrame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!P.freq.QuadPart) { QueryPerformanceFrequency(&P.freq); P.last = now; P.logAt = GetTickCount64() + 60000; return; }
    P.lastFrame = QpcMs(P.last, now);
    P.worst = std::max(P.worst, P.lastFrame);
    P.lastDraw = P.drawMsFrame;
    P.frameMs += P.lastFrame;
    P.drawMs += P.drawMsFrame;
    P.drawMsFrame = 0;
    P.frames++;
    P.last = now;
    if (GetTickCount64() < P.logAt || !P.frames) return;
    double ft = P.frameMs / P.frames;
    SessionLog("perf, last minute: %.0f fps (frame %.2f ms, worst %.0f ms) | FortsGFX GPU %.2f ms | hooks CPU %.2f ms/frame | post %s, lighting %s",
               1000.0 / ft, ft, P.worst, P.gpuN ? P.gpuMs / P.gpuN : 0.0, P.drawMs / P.frames, cfg.enabled ? "on" : "off", cfg.lighting ? "on" : "off");
    P.frameMs = P.gpuMs = P.drawMs = 0;
    P.worst = 0;
    P.frames = P.gpuN = 0;
    P.logAt = GetTickCount64() + 60000;
}
static float g_world[4] = { -1000, 1000, 1000, -1000 };   // last world glOrtho: left, right, bottom, top

static GLuint Compile(GLenum type, const char* a, const char* b, const char* label) {
    GLuint s = G.CreateShader(type);
    const GLchar* parts[2] = { a, b };
    G.ShaderSource(s, b ? 2 : 1, parts, nullptr);
    G.CompileShader(s);
    GLint ok = 0;
    G.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        G.GetShaderInfoLog(s, sizeof(log), nullptr, log);
        SessionLog("gfx: shader '%s' failed to compile:\n%s", label, log);
        G.DeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint Program(const char* fs, const char* label) {
    GLuint v = Compile(GL_VERTEX_SHADER, kVsFullscreen, nullptr, "fullscreen.vs");
    GLuint f = Compile(GL_FRAGMENT_SHADER, kFsCommon, fs, label);
    if (!v || !f) return 0;
    GLuint p = G.CreateProgram();
    G.AttachShader(p, v);
    G.AttachShader(p, f);
    G.LinkProgram(p);
    G.DeleteShader(v);
    G.DeleteShader(f);
    GLint ok = 0;
    G.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        G.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        SessionLog("gfx: program '%s' failed to link:\n%s", label, log);
        return 0;
    }
    return p;
}

// A program with its own vertex shader (instanced effects); the fragment shader gets kFsCommon.
static GLuint ProgramVS(const char* vs, const char* fs, const char* label) {
    GLuint v = Compile(GL_VERTEX_SHADER, vs, nullptr, label);
    GLuint f = Compile(GL_FRAGMENT_SHADER, kFsCommon, fs, label);
    if (!v || !f) return 0;
    GLuint p = G.CreateProgram();
    G.AttachShader(p, v);
    G.AttachShader(p, f);
    G.LinkProgram(p);
    G.DeleteShader(v);
    G.DeleteShader(f);
    GLint ok = 0;
    G.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        G.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        SessionLog("gfx: program '%s' failed to link:\n%s", label, log);
        return 0;
    }
    return p;
}

static GLuint WorldProgram() {
    GLuint v = Compile(GL_VERTEX_SHADER, kVsWorld, nullptr, "world.vs");
    GLuint f = Compile(GL_FRAGMENT_SHADER, kFsWorld, nullptr, "world.fs");
    if (!v || !f) return 0;
    GLuint p = G.CreateProgram();
    G.AttachShader(p, v);
    G.AttachShader(p, f);
    G.LinkProgram(p);
    G.DeleteShader(v);
    G.DeleteShader(f);
    GLint ok = 0;
    G.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        G.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        SessionLog("gfx: world material shader failed to link (falling back to fixed-function):\n%s", log);
        return 0;
    }
    g_locMode = G.GetUniformLocation(p, "uMode");
    g_locClass = G.GetUniformLocation(p, "uClass");
    g_locAlpha = G.GetUniformLocation(p, "uAlpha");
    g_locMat = G.GetUniformLocation(p, "uMat");
    g_locFx = G.GetUniformLocation(p, "uFx");
    g_locTex = G.GetUniformLocation(p, "uTex");
    return p;
}

static GLuint MakeTex(int w, int h, GLenum ifmt) {
    GLuint t = 0;
    GLenum type = (ifmt == GL_RGBA16F || ifmt == GL_R16F) ? GL_HALF_FLOAT : ifmt == GL_RGBA32F ? GL_FLOAT : GL_UNSIGNED_BYTE;
    GLenum fmt = ifmt == GL_R16F ? GL_RED_FMT : GL_RGBA;
    G.GenTextures(1, &t);
    G.BindTexture(GL_TEXTURE_2D, t);
    G.TexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

// One colour target, or two (MRT) when `second` is set.
static bool MakeTarget(Target& t, int w, int h, GLenum ifmt, bool second = false) {
    t.w = w; t.h = h;
    t.tex = MakeTex(w, h, ifmt);
    if (second) t.tex2 = MakeTex(w, h, ifmt);
    G.GenFramebuffers(1, &t.fbo);
    G.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    if (second) {
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t.tex2, 0);
        const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        G.DrawBuffers(2, bufs);
    }
    return G.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void FreeTarget(Target& t) {
    if (t.fbo) G.DeleteFramebuffers(1, &t.fbo);
    if (t.tex) G.DeleteTextures(1, &t.tex);
    if (t.tex2) G.DeleteTextures(1, &t.tex2);
    t = Target{};
}

static void FreeResources() {
    if (g_worldFbo) G.DeleteFramebuffers(1, &g_worldFbo);
    GLuint world[4] = { g_sceneTex, g_emisTex, g_solidTex, g_normTex };
    for (GLuint t : world) if (t) G.DeleteTextures(1, &t);
    g_worldFbo = g_sceneTex = g_emisTex = g_solidTex = g_normTex = 0;
    for (auto& b : g_bloom) FreeTarget(b);
    FreeTarget(g_ldr);
    FreeTarget(g_ldr2);
    FreeTarget(g_bgBlur[0]);
    FreeTarget(g_bgBlur[1]);
    FreeTarget(g_bloomAvg[0]);
    FreeTarget(g_bloomAvg[1]);
    for (Target* t : { &g_giScene, &g_jfa[0], &g_jfa[1], &g_sdf, &g_cascade[0], &g_cascade[1], &g_hist[0], &g_hist[1] }) FreeTarget(*t);
    for (auto& t : g_pull) FreeTarget(t);
    for (auto& t : g_push) FreeTarget(t);
    for (auto& t : g_debug) FreeTarget(t);
    FreeTarget(g_parts[0]);
    FreeTarget(g_parts[1]);
    FreeTarget(g_bdHalf);
    if (g_bdSceneFbo) G.DeleteFramebuffers(1, &g_bdSceneFbo);
    g_bdSceneFbo = 0;
    if (g_fxFbo) G.DeleteFramebuffers(1, &g_fxFbo);
    g_fxFbo = 0;
    g_bloomLevels = g_pyramid = g_cascades = 0;
    g_histValid = false;
    g_ready = false;
}

// Working resolution is padded so every cascade's probe grid and every pyramid level divide evenly.
static bool CreateLighting(int w, int h) {
    // Cells are a whole number of screen pixels; one spare cell absorbs the world-anchored offset.
    int p = std::max(1, (int)lroundf(1.0f / cfg.giScale));
    int gw = std::max(64, (w + p - 1) / p + 1), gh = std::max(64, (h + p - 1) / p + 1);
    float diag = sqrtf((float)gw * gw + (float)gh * gh);
    int n = 1;
    while (n < 9 && (powf(4.0f, (float)n) - 1.0f) / 3.0f < diag) n++;   // ray reach with a 1 px first interval
    int align = std::max(32, 1 << (n - 1));
    gw = (gw + align - 1) / align * align;
    gh = (gh + align - 1) / align * align;
    bool ok = MakeTarget(g_giScene, gw, gh, GL_RGBA16F) && MakeTarget(g_jfa[0], gw, gh, GL_RGBA32F) &&
              MakeTarget(g_jfa[1], gw, gh, GL_RGBA32F) && MakeTarget(g_sdf, gw, gh, GL_R16F) &&
              MakeTarget(g_cascade[0], gw, gh, GL_RGBA16F, true) && MakeTarget(g_cascade[1], gw, gh, GL_RGBA16F, true) && MakeTarget(g_hist[0], gw, gh, GL_RGBA16F, true) && MakeTarget(g_hist[1], gw, gh, GL_RGBA16F, true);
    int pw = gw, ph = gh;
    while (ok && g_pyramid < kMaxPyramid && pw >= 8 && ph >= 8) {
        ok = MakeTarget(g_pull[g_pyramid], pw, ph, GL_RGBA16F, true) && MakeTarget(g_push[g_pyramid], pw, ph, GL_RGBA16F, true);
        g_pyramid++;
        if (pw % 2 || ph % 2) break;
        pw /= 2;
        ph /= 2;
    }
    for (int i = 0; ok && i < kDebugViews; i++) ok = MakeTarget(g_debug[i], w / 2, h / 2, GL_RGBA8);
    g_gridP = p;
    g_gw = gw;
    g_gh = gh;
    g_cascades = n;
    return ok;
}

static bool CreateResources(int w, int h) {
    StateGuard sg;
    sg.Save();
    FreeResources();
    bool ok = true;
    if (!g_progsBuilt) {
        g_pPrefilter = Program(kFsPrefilter, "prefilter");
        g_pDown = Program(kFsDown, "down");
        g_pUp = Program(kFsUp, "up");
        g_pBloomAvg = Program(kFsBloomAvg, "bloom_avg");
        g_pComposite = Program(kFsComposite, "composite");
        g_pFxaa = Program(kFsFxaa, "fxaa");
        g_pFinal = Program(kFsFinal, "final");
        g_pTemporal = Program(kFsTemporal, "temporal");
        g_pBgDown = Program(kFsBgDown, "bg_down");
        g_pCopy = Program(kFsCopy, "copy");
        g_pGiPrep = Program(kFsGiPrep, "gi_prep");
        g_pJfaSeed = Program(kFsJfaSeed, "jfa_seed");
        g_pJfaStep = Program(kFsJfaStep, "jfa_step");
        g_pJfaDist = Program(kFsJfaDist, "jfa_dist");
        g_pCascade = Program(kFsCascade, "cascade");
        g_pPull0 = Program(kFsPull0, "pull0");
        g_pPull = Program(kFsPull, "pull");
        g_pPush = Program(kFsPush, "push");
        g_pDebug = Program(kFsDebug, "debug");
        g_pWorld = WorldProgram();
        g_pFireball = ProgramVS(kVsFireball, kFsFireball, "fireball");
        g_pDecal = ProgramVS(kVsDecal, kFsDecal, "decal");
        g_pMilk = Program(kFsMilk, "milk");
        g_pSprite = ProgramVS(kVsSprite, kFsSprite, "sprite");
        g_pStream = ProgramVS(kVsStream, kFsStream, "stream");
        g_pBeamFx = ProgramVS(kVsStream, kFsBeam, "beam_fx");
        g_pPartUpdate = Program(kFsParticleUpdate, "particle_update");
        g_pPartDraw = ProgramVS(kVsParticle, kFsParticle, "particle_draw");
        g_pSnowStep = Program(kFsSnowStep, "snow_step");
        g_pSnowDep = ProgramVS(kVsSnowDeposit, kFsSnowDeposit, "snow_deposit");
        g_pSnowCopy = Program(kFsSnowCopy, "snow_copy");
        g_pBlur = Program(kFsBlur, "dof_blur");
        g_pTerrain = ProgramVS(kVsTerrain, kFsTerrain, "terrain_mask");
        g_pBolt = ProgramVS(kVsBolt, kFsBolt, "bolt");
        g_pBackdrop = Program(kFsBackdrop, "backdrop");
        G.GenVertexArrays(1, &g_vao);
        g_progsBuilt = true;
    }
    ok = g_pPrefilter && g_pDown && g_pUp && g_pComposite && g_pFxaa && g_pFinal && g_pCopy && g_pBgDown && g_pTemporal && g_pGiPrep && g_pJfaSeed &&
         g_pJfaStep && g_pJfaDist && g_pCascade && g_pPull0 && g_pPull && g_pPush && g_pDebug && g_vao;

    if (ok) {
        g_sceneTex = MakeTex(w, h, GL_RGBA16F);
        g_emisTex = MakeTex(w, h, GL_RGBA16F);
        g_solidTex = MakeTex(w, h, GL_RGBA8);
        g_normTex = MakeTex(w, h, GL_RGBA8);
        G.GenFramebuffers(1, &g_worldFbo);
        G.BindFramebuffer(GL_FRAMEBUFFER, g_worldFbo);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sceneTex, 0);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_emisTex, 0);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, g_solidTex, 0);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, g_normTex, 0);
        const GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
        G.DrawBuffers(4, bufs);
        ok = G.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    int bw = w / 2, bh = h / 2;
    while (ok && g_bloomLevels < kMaxBloom && bw >= 8 && bh >= 8) {
        ok = MakeTarget(g_bloom[g_bloomLevels++], bw, bh, GL_RGBA16F);
        bw /= 2;
        bh /= 2;
    }
    if (ok) ok = MakeTarget(g_ldr, w, h, GL_RGBA8) && MakeTarget(g_ldr2, w, h, GL_RGBA8);
    if (ok) ok = MakeTarget(g_bgBlur[0], w / 2, h / 2, GL_RGBA16F) && MakeTarget(g_bgBlur[1], w / 4, h / 4, GL_RGBA16F);
    if (ok) ok = MakeTarget(g_bloomAvg[0], 1, 1, GL_RGBA16F) && MakeTarget(g_bloomAvg[1], 1, 1, GL_RGBA16F);
    if (ok) {   // start at "nothing glows" (an uncleared target could hold NaN and dim the bloom to black)
        const GLfloat zero[4] = { 0, 0, 0, 0 };
        for (auto& t : g_bloomAvg) { G.BindFramebuffer(GL_FRAMEBUFFER, t.fbo); G.ClearBufferfv(GL_COLOR, 0, zero); }
    }
    if (ok) ok = CreateLighting(w, h);
    if (ok) {   // 3D backdrop targets
        MakeTarget(g_bdHalf, w / 2, h / 2, GL_RGBA16F);
        G.GenFramebuffers(1, &g_bdSceneFbo);
        G.BindFramebuffer(GL_FRAMEBUFFER, g_bdSceneFbo);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sceneTex, 0);
    }
    if (ok) {   // effects: particle state + an FBO over the world's scene and light-source targets
        bool pok = MakeTarget(g_parts[0], kPW, kPH, GL_RGBA32F, true) && MakeTarget(g_parts[1], kPW, kPH, GL_RGBA32F, true);
        G.GenFramebuffers(1, &g_fxFbo);
        G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sceneTex, 0);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_emisTex, 0);
        G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, g_solidTex, 0);   // g: smoke density
        const GLenum fb2[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
        G.DrawBuffers(3, fb2);
        if (!pok || G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || !G.DrawArraysInstanced || !G.TexSubImage2D) {
            SessionLog("gfx: effects targets unavailable - impact effects disabled");
            if (g_fxFbo) G.DeleteFramebuffers(1, &g_fxFbo);
            g_fxFbo = 0;
        } else {
            static const GLfloat zero[4] = { 0, 0, 0, 0 };
            for (auto& pt : g_parts) {   // all particles start dead
                G.BindFramebuffer(GL_FRAMEBUFFER, pt.fbo);
                G.ClearBufferfv(GL_COLOR, 0, zero);
                G.ClearBufferfv(GL_COLOR, 1, zero);
            }
        }
    }
    sg.Restore(false);
    if (!ok) {
        SessionLog("gfx: could not create render targets (%dx%d) - post-processing disabled", w, h);
        FreeResources();
        return false;
    }
    g_w = w;
    g_h = h;
    g_giScaleBuilt = cfg.giScale;
    g_ready = true;
    SessionLog("gfx: world targets %dx%d, %d bloom levels; lighting %dx%d, %d cascades, %d pyramid levels",
               w, h, g_bloomLevels, g_gw, g_gh, g_cascades, g_pyramid);
    return true;
}

// ---------------------------------------------------------------- post chain

static void Bind(int unit, GLuint tex) {
    G.ActiveTexture(GL_TEXTURE0 + unit);
    G.BindTexture(GL_TEXTURE_2D, tex);
}
static void Use(GLuint prog) { G.UseProgram(prog); }
static void U1i(GLuint p, const char* n, int v) { G.Uniform1i(G.GetUniformLocation(p, n), v); }
static void U1f(GLuint p, const char* n, float v) { G.Uniform1f(G.GetUniformLocation(p, n), v); }
static void U2f(GLuint p, const char* n, float a, float b) { G.Uniform2f(G.GetUniformLocation(p, n), a, b); }
static void U4f(GLuint p, const char* n, float a, float b, float c, float d) { G.Uniform4f(G.GetUniformLocation(p, n), a, b, c, d); }
static void Draw(GLuint fbo, int w, int h) {
    G.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    G.Viewport(0, 0, w, h);
    G.DrawArrays(GL_TRIANGLES, 0, 3);
}

// ---------------------------------------------------------------- effects (Phase 4/5)
// World draws whose texture is an explosion / flash / spark / fire are measured as they are drawn
// (GfxTrackVertex) and become events in world coordinates. Explosions are tracked across frames so
// each one triggers its effects once: shockwave, screen shake, a flash light fed into the
// ray-traced lighting, a volumetric fireball, debris/sparks that collide with the real terrain,
// embers, colour pulse and (for big hits) an impact frame.

enum EvClass : uint8_t { kEvNone, kEvExplosion, kEvFlash, kEvSpark, kEvFire, kEvPlume, kEvSteam, kEvSmoke, kEvTracer,
                         kEvMiko, kEvMikoGun, kEvMilk,   // the water cannon's body, its barrel, its water ([Fun] MilkMiko)
                         kEvBridgeLightning, kEvBridgeImpact, kEvBridgeShock, kEvBridgeFlash, kEvWeatherProbe, kEvWaterProbe,
                         kEvBridgeCapture, kEvBridgeTour };

struct FrameEvent { uint8_t cls; float x, y, r, a; bool part = false, fireFrame = false; uint8_t puff = 0; GLuint tex = 0; unsigned seq = 0; };
// world units; a = sprite opacity; part = one particle of a batch; puff = which simulated puff; seq = the draw in the frame
struct Instance { uint8_t cls; float x, y, r, maxR, maxA, emberAcc; unsigned lastSeen, firstSeen; bool fired; uint8_t puff; GLuint tex; bool quiet = false; };
struct Shock { float x, y, r, t0, life, strength; };
struct FlashLight { float x, y, r, t0, life, intensity, col[3]; };
struct Fireball { float x, y, r, t0, life, seed; };

static std::vector<FrameEvent> g_frameEvents;
static std::vector<Instance> g_inst;
static std::vector<Shock> g_shocks;
static std::vector<FlashLight> g_flashes;
static std::vector<Fireball> g_fireballs;
static float g_trauma, g_impactT0 = -100, g_impactX, g_impactY, g_lastImpact = -100;
static float g_pulseT0 = -100, g_pulseAmt;
static float g_fxNow, g_fxDt;
static unsigned g_fxFrame;

static std::unordered_map<GLuint, uint8_t> g_evClassTex;   // guarded by g_emitMx
static std::unordered_map<GLuint, uint8_t> g_puffTex;     // smoke / steam images: which simulated puff replaces them (g_emitMx)
static std::unordered_set<GLuint> g_extraTex;             // ...one of the newer replacements (SimulateMoreEffects)

static const char* EvName(int c) {
    static const char* n[] = { "none", "explosion", "flash", "spark", "fire", "plume", "steam", "smoke", "tracer", "miko", "miko-cannon", "milk",
                               "bridge-lightning", "bridge-impact", "bridge-shock", "bridge-flash", "weather-probe", "water-probe",
                               "bench-capture", "showcase-chapter" };
    return c >= 0 && c < (int)(sizeof(n) / sizeof(n[0])) ? n[c] : "?";
}

static uint8_t EventClassForName(const std::string& base) {
    auto has = [&](const char* s) { return base.find(s) != std::string::npos; };
    if (has("fortsgfx_capture")) return kEvBridgeCapture;   // test bench marker: capture this moment
    if (has("fortsgfx_tour")) return kEvBridgeTour;         // showcase map marker: show tour chapter NN
    if (has("fortsgfx_lightning")) return kEvBridgeLightning;
    if (has("fortsgfx_impact")) return kEvBridgeImpact;
    if (has("fortsgfx_shock")) return kEvBridgeShock;
    if (has("fortsgfx_flash")) return kEvBridgeFlash;
    // rocket and missile engine flames (missiletail01-08), boosters, thrusters: a plume every frame
    if (has("missiletail") || has("booster") || has("thruster")) return kEvPlume;
    if (has("steam")) return kEvSteam;     // also the missile smoke trail (missile_trail.lua uses Steam)
    if (has("flareuranium")) return kEvSteam;   // Buster's depleted-uranium trails (uranium_fly*.lua): smoke, not fire
    // projectiles: bullets, cannon shells, mortar shells get a glowing tracer streak
    if (has("bullet") || has("cannonshell") || base == "mortar.tga" || base == "mortar.dds" || has("mortar-incendiary")) return kEvTracer;
    if (has("smoke")) return kEvSmoke;     // mortar trails, damaged devices
    // the newer replacements (behind [Impact] SimulateMoreEffects; effect images only, see SetEventClass)
    if (has("fire_deckgun")) return kEvSmoke;                        // High Seas deck gun: the blast cloud
    if (has("sandcloud") || has("dust")) return kEvSmoke;             // dust kicked up by impacts
    if (has("cloud") && !has("mushroom")) return kEvSmoke;           // smoke clouds (burning structures, weapon pack)
    if (has("debris") || has("casing")) return kEvNone;
    // "fireball" is not an explosion: burning structures puff fireball.tga about once a second
    if (has("explosion") || has("blast") || has("nuke") || has("detonat") || has("device_destruction")) return kEvExplosion;
    if (has("flash") || has("muzzle")) return kEvFlash;
    if (has("fire") && !has("beam")) return kEvFire;   // incl. fireball, fire_sparks, firestart
    if (has("spark")) return kEvSpark;
    return kEvNone;
}

// ---- the session log's effects summary: every 20 s, what the game drew and what FortsGFX did with it
static std::map<std::string, int> g_fxStats;
static std::map<std::string, int> g_gfStats;   // what the game reported and what effects it made (GameFx)
static int g_fxBursts, g_fxPuffs;
static double g_fxStatT0;
static std::string ImageName(GLuint tex) {   // "[64x64 effects/media/fire02.tga]" -> "fire02.tga"
    std::string t = TexShort(tex);
    size_t sp = t.find(' '), sl = t.find_last_of("/\\");
    size_t a = sl != std::string::npos ? sl + 1 : sp != std::string::npos ? sp + 1 : 0;
    std::string n = t.substr(a);
    if (!n.empty() && n.back() == ']') n.pop_back();
    return n.empty() ? "?" : n;
}
static void FxStat(const char* what, GLuint tex, const char* did) {
    if (g_fxStats.size() < 400) g_fxStats[std::string(what) + " " + ImageName(tex) + " -> " + did]++;
}
static void FxStatFlush(bool force) {
    double now = GetTickCount64() / 1000.0;
    if (g_fxStatT0 == 0) g_fxStatT0 = now;
    if (!force && now - g_fxStatT0 < 20.0) return;
    if (!g_fxStats.empty() || g_fxBursts || g_fxPuffs) {
        std::vector<std::pair<int, std::string>> rows;
        for (auto& kv : g_fxStats) rows.push_back({ kv.second, kv.first });
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        SessionLog("effects, last %.0f s: FortsGFX made %d explosion bursts and %d simulated puffs; the game's sprites:", now - g_fxStatT0, g_fxBursts, g_fxPuffs);
        int shown = 0;
        for (auto& r : rows) {
            if (shown++ == 14) { SessionLog("    ... %zu more kinds", rows.size() - 14); break; }
            SessionLog("    %5d x %s", r.first, r.second.c_str());
        }
    }
    if (!g_gfStats.empty()) {
        std::vector<std::pair<int, std::string>> rows;
        for (auto& kv : g_gfStats) rows.push_back({ kv.second, kv.first });
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        SessionLog("game events -> effects, last %.0f s:", now - g_fxStatT0);
        int shown = 0;
        for (auto& r : rows) {
            if (shown++ == 16) { SessionLog("    ... %zu more kinds", rows.size() - 16); break; }
            SessionLog("    %5d x %s", r.first, r.second.c_str());
        }
    }
    g_gfStats.clear();
    g_fxStats.clear();
    g_fxBursts = g_fxPuffs = 0;
    g_fxStatT0 = now;
}

// A line in the session log when a battle view starts (with the settings in use) and when it ends.
static void BattleMarks() {
    static double lastPf;
    static bool inBattle;
    double now = GetTickCount64() / 1000.0;
    if (g_playfield) {
        if (!inBattle) {
            inBattle = true;
            static const char* fx[] = { "Showcase", "Cinematic", "Subtle" };
            static const char* wx[] = { "auto", "off", "rain", "storm", "snow", "thick snow", "sandstorm", "fog banks", "ash" };
            SessionLog("--- battle view: look \"%s\", effects %s, weather %s, day/night %s, simulated smoke %s, more sim fx %s, lighting %s",
                       GfxLookName(cfg.look), fx[std::min(std::max(cfg.fxPreset, 0), 2)], wx[std::min(std::max(cfg.weatherMode, 0), 8)],
                       cfg.dayNight ? "on" : "off", cfg.simSmoke ? "on" : "off", cfg.moreSimFx ? "on" : "off", cfg.lighting ? "on" : "off");
        }
        lastPf = now;
    } else if (inBattle && now - lastPf > 3.0) {
        inBattle = false;
        FxStatFlush(true);
        SessionLog("--- battle view ended");
    }
}

static float Rand01() {
    static uint32_t s = 0x9E3779B9u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xFFFFFF) / 16777216.0f;
}
static float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

static double g_timeOverride = -1;   // replay tool: simulated clock
static GfxLab g_lab;
GfxLab& GfxLabState() { return g_lab; }

// The effects clock: real time scaled by the lab's slow motion.
static double FxSeconds() {
    if (g_timeOverride >= 0) return g_timeOverride;
    static LARGE_INTEGER f, last;
    static double acc;
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); last = n; }
    double dt = (double)(n.QuadPart - last.QuadPart) / f.QuadPart;
    last = n;
    acc += g_lab.freeze ? 0.0 : std::min(dt, 0.25) * std::min(std::max(g_lab.slowMo, 0.02f), 4.0f);
    return acc;
}

// ---- draw tracking (called from the vertex hooks while g_trackDraw is set)
bool g_trackDraw;
GLenum g_beginMode = 0x0005;   // primitive of the current glBegin (hooks)
static uint8_t g_trackClass;
static float g_trackM[16], g_trackVp[4];
static float g_trackX0, g_trackY0, g_trackX1, g_trackY1;
static int g_trackN;
static float g_trackA;          // opacity of the tracked draw (current colour, or colours inside the block)
static bool g_trackColSeen;

static GLuint g_trackTex;
static std::vector<GLuint> g_probeQueue;
// The game's particle system draws a whole effect's particles in one GL_QUADS batch (a trail of 25 puffs
// is one draw). For smoke and steam every quad is its own event, so each puff is replaced where it is.
// The draws of this frame, counted, and which ones were fire sprites: the game's fire system draws a
// burning structure's sprites one after another, and an explosion-named sprite among them is one of the
// fire's own frames (the game shares the image), not a blast.
static unsigned g_drawSeq;
static std::vector<unsigned> g_fireDraws;
static bool g_trackSplit;
static uint8_t g_trackPuff;     // the simulated puff for this draw (0 = by class)
static float g_qX0, g_qY0, g_qX1, g_qY1, g_qA, g_qR, g_qG, g_qB;
static int g_qN, g_trackParts;
static void BeginTrack(uint8_t cls) {
    float mv[16], p[16];
    R.GetFloatv(GL_MODELVIEW_MATRIX, mv);
    R.GetFloatv(GL_PROJECTION_MATRIX, p);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += p[k * 4 + r] * mv[c * 4 + k];
            g_trackM[c * 4 + r] = v;
        }
    GLint vp[4];
    R.GetIntegerv(GL_VIEWPORT, vp);
    for (int i = 0; i < 4; i++) g_trackVp[i] = (float)vp[i];
    g_trackX0 = g_trackY0 = 1e30f;
    g_trackX1 = g_trackY1 = -1e30f;
    g_trackN = 0;
    float col[4] = { 1, 1, 1, 1 };
    R.GetFloatv(GL_CURRENT_COLOR, col);
    g_trackA = col[3];
    g_trackColSeen = false;
    g_trackClass = cls;
    g_trackDraw = true;
    g_trackSplit = (cls == kEvSteam || cls == kEvSmoke || cls == kEvFire) && g_beginMode == 0x0007 /* GL_QUADS */;
    g_qX0 = g_qY0 = 1e30f; g_qX1 = g_qY1 = -1e30f; g_qN = 0; g_qA = -1.0f; g_trackParts = 0;
    g_qR = g_qG = g_qB = -1.0f;
}

// glColor inside the tracked glBegin..glEnd (per-vertex colours): the block's opacity is their max.
void GfxTrackColor(float r, float g, float b, float a) {
    g_trackA = g_trackColSeen ? std::max(g_trackA, a) : a;
    g_trackColSeen = true;
    g_qA = std::max(g_qA, a);
    g_qR = std::max(g_qR, r); g_qG = std::max(g_qG, g); g_qB = std::max(g_qB, b);
}

static void TrackQuadDone() {   // one particle of a split batch
    float a = g_qA >= 0.0f ? g_qA : g_trackA;
    float ext = std::max(g_qX1 - g_qX0, g_qY1 - g_qY0);
    if (a >= 0.06f && ext >= g_h * 0.006f && g_frameEvents.size() < 512 && g_w > 0) {
        float wx = (g_world[1] - g_world[0]) / g_w, wy = (g_world[3] - g_world[2]) / g_h;
        FrameEvent e{ g_trackClass, g_world[0] + (g_qX0 + g_qX1) * 0.5f * wx, g_world[2] + (g_qY0 + g_qY1) * 0.5f * wy, ext * 0.5f * fabsf(wx), std::min(a, 1.0f) };
        e.part = true;
        e.puff = g_trackPuff;
        e.tex = g_trackTex;
        // steam the game tints hot (the deck gun's blast gas: yellow-orange) becomes glowing hot gas
        if (g_trackClass == kEvSteam && cfg.moreSimFx && g_qR > 0.5f && g_qR > g_qB * 1.8f && g_qG > g_qB * 1.3f) e.puff = 12;
        g_frameEvents.push_back(e);
        g_trackParts++;
    }
    g_qX0 = g_qY0 = 1e30f; g_qX1 = g_qY1 = -1e30f; g_qN = 0; g_qA = -1.0f;
    g_qR = g_qG = g_qB = -1.0f;
}

// ---- the sea, read from the game's own wave mesh (High Seas maps). Packed DLC maps never pass
// through the file hooks, so the water line comes from the drawing: an immediate triangle mesh across
// the view whose top edge is the surface. Each screen column keeps the highest vertex seen in it.
static const int kWaterCols = 64;
static float g_probeTop[kWaterCols];             // GL y (up) of the highest vertex per column, this draw
static float g_waterProf[kWaterCols];            // world y of the surface per screen column (y down)
static unsigned g_waterGeoFrame;                 // g_fxFrame the sea mesh was last seen (0 = never)
static bool g_waterForced;                       // replay: the level is given, no geometry
static std::unordered_set<GLuint> g_seaTex;      // textures confirmed as the sea

void GfxTrackVertex(float x, float y, float z) {
    const float* M = g_trackM;
    float cw = M[3] * x + M[7] * y + M[11] * z + M[15];
    if (cw == 0) return;
    float sx = g_trackVp[0] + ((M[0] * x + M[4] * y + M[8] * z + M[12]) / cw + 1) * 0.5f * g_trackVp[2];
    float sy = g_trackVp[1] + ((M[1] * x + M[5] * y + M[9] * z + M[13]) / cw + 1) * 0.5f * g_trackVp[3];
    g_trackX0 = std::min(g_trackX0, sx); g_trackX1 = std::max(g_trackX1, sx);
    g_trackY0 = std::min(g_trackY0, sy); g_trackY1 = std::max(g_trackY1, sy);
    g_trackN++;
    if (g_trackSplit) {
        g_qX0 = std::min(g_qX0, sx); g_qX1 = std::max(g_qX1, sx);
        g_qY0 = std::min(g_qY0, sy); g_qY1 = std::max(g_qY1, sy);
        if (++g_qN == 4) TrackQuadDone();
    }
    if (g_trackClass == kEvWaterProbe && sx >= 0 && sx < g_w) {
        int b = std::min((int)(sx * kWaterCols / g_w), kWaterCols - 1);
        g_probeTop[b] = std::max(g_probeTop[b], sy);
    }
}

static void WaterProbeEnd() {
    bool known = g_seaTex.count(g_trackTex) != 0;
    if (g_trackN < (known ? 30 : 60) || g_trackX1 - g_trackX0 < g_w * (known ? 0.5f : 0.8f)) return;   // a mesh across the view
    float top[kWaterCols];
    int have = 0;
    for (int b = 0; b < kWaterCols; b++) if (g_probeTop[b] > -1e29f) have++;
    if (have < kWaterCols / 3) return;
    for (int b = 0; b < kWaterCols; b++) {            // columns without a vertex: the nearest one that has
        top[b] = g_probeTop[b];
        for (int k = 1; top[b] < -1e29f && k < kWaterCols; k++) {
            if (b - k >= 0 && g_probeTop[b - k] > -1e29f) top[b] = g_probeTop[b - k];
            else if (b + k < kWaterCols && g_probeTop[b + k] > -1e29f) top[b] = g_probeTop[b + k];
        }
    }
    float wy = (g_world[3] - g_world[2]) / g_h;      // world y per GL px
    float viewH = fabsf(g_world[3] - g_world[2]);
    float lo = 1e30f, hi = -1e30f, topGl = -1e30f, prof[kWaterCols];
    for (int b = 0; b < kWaterCols; b++) {
        prof[b] = g_world[2] + top[b] * wy;
        lo = std::min(lo, prof[b]); hi = std::max(hi, prof[b]);
        topGl = std::max(topGl, top[b]);
    }
    float depth = fabsf((topGl - g_trackY0) * wy);   // world units from the surface down to the mesh bottom
    bool flat = hi - lo < std::max(viewH * 0.06f, 150.0f);
    bool deep = known ? depth > viewH * 0.1f : depth > std::max(900.0f, viewH * 0.25f);
    if (!flat || !deep) return;                      // terrain edge decals are bumpy and thin
    if (!known) g_seaTex.insert(g_trackTex);
    memcpy(g_waterProf, prof, sizeof(prof));
    float sorted[kWaterCols];
    memcpy(sorted, prof, sizeof(prof));
    std::nth_element(sorted, sorted + kWaterCols / 2, sorted + kWaterCols);
    g_waterLevel = sorted[kWaterCols / 2];
    bool fresh = g_waterGeoFrame == 0 || g_fxFrame - g_waterGeoFrame > 600;   // first sight on this map
    g_waterGeoFrame = std::max(g_fxFrame, 1u);
    if (fresh) {
        SessionLog("gfx: sea found in the drawing (tex %u, %d verts): water line %.0f, waves %.0f", g_trackTex, g_trackN, (float)g_waterLevel, hi - lo);
    }
}

static bool WaterOn();
bool GfxWaterVisible() { return WaterOn(); }
static bool WaterOn() {
    if (!cfg.water) return false;
    if (g_waterForced) return g_hasWater;
    return g_waterGeoFrame != 0 && g_fxFrame - g_waterGeoFrame < 30;
}

void GfxEndTrack() {
    g_trackDraw = false;
    if (g_trackClass == kEvWaterProbe) { WaterProbeEnd(); return; }
    if (g_trackClass == kEvBridgeCapture) {   // test bench marker (fortsgfx_capture_NN_label.png): one capture per beat
        static std::unordered_map<GLuint, float> last;
        auto it = last.find(g_trackTex);
        if (it == last.end() || g_fxNow - it->second > 3.0f || g_fxNow < it->second) {
            last[g_trackTex] = g_fxNow;
            std::string name = TexShort(g_trackTex), label = "bench";
            size_t a = name.find("fortsgfx_capture_");
            if (a != std::string::npos) {
                a += 17;
                size_t b = name.find_first_of(".]", a);
                label = name.substr(a, b == std::string::npos ? std::string::npos : b - a);
            }
            SessionLog("bench: beat %s", label.c_str());
            CapRequest(label.c_str());
        }
        return;
    }
    if (g_trackClass == kEvBridgeTour) {   // showcase map marker (fortsgfx_tour_NN_name.png): tour chapter NN
        static std::unordered_map<GLuint, float> last;
        auto it = last.find(g_trackTex);
        if (it == last.end() || g_fxNow - it->second > 1.5f || g_fxNow < it->second) {
            last[g_trackTex] = g_fxNow;
            std::string name = TexShort(g_trackTex);
            size_t a = name.find("fortsgfx_tour_");
            int n = a == std::string::npos ? -1 : atoi(name.c_str() + a + 14);
            SessionLog("showcase: chapter %d (%s)", n, name.c_str());
            GfxTourChapter(n >= 99 ? -1 : n);
        }
        return;
    }
    if (g_trackSplit) {
        CapHistNote(Fmt("sprite %-10s %-48s %d particles -> %d events", EvName(g_trackClass), TexShort(g_trackTex).c_str(), g_trackN / 4, g_trackParts));
        return;
    }
    if (g_trackN < 2 || g_w <= 0) return;
    float wx = (g_world[1] - g_world[0]) / g_w, wy = (g_world[3] - g_world[2]) / g_h;   // world per GL px
    float cx = (g_trackX0 + g_trackX1) * 0.5f, cy = (g_trackY0 + g_trackY1) * 0.5f;
    float r = std::max(g_trackX1 - g_trackX0, g_trackY1 - g_trackY0) * 0.5f * fabsf(wx);
    if (g_trackClass == kEvWeatherProbe) {         // a huge overlay tile: maybe the game's rain
        if (g_trackN <= 6 && std::max(g_trackX1 - g_trackX0, g_trackY1 - g_trackY0) > g_h * 0.5f &&   // a tile, not a mesh (the sea's whitecaps)
            std::find(g_probeQueue.begin(), g_probeQueue.end(), g_trackTex) == g_probeQueue.end())
            g_probeQueue.push_back(g_trackTex);
        return;
    }
    // Invisible (faded-out) or pixel-sized sprites trigger nothing: the game draws plenty of both,
    // e.g. bullet-impact flashes at alpha 0 or 1-4 px across.
    float extentPx = std::max(g_trackX1 - g_trackX0, g_trackY1 - g_trackY0);
    bool bridge = g_trackClass >= kEvBridgeLightning;
    bool faint = !bridge && (g_trackA < 0.06f || extentPx < g_h * 0.006f);
    CapHistNote(Fmt("sprite %-10s %-48s box=(%.0f,%.0f)-(%.0f,%.0f) a=%.2f %s%s", EvName(g_trackClass), TexShort(g_trackTex).c_str(),
                    g_trackX0, g_h - g_trackY1, g_trackX1, g_h - g_trackY0, g_trackA, g_beginMode == 0x0007 ? "quads" : "strip",
                    faint ? "  -> ignored (faint or tiny)" : ""));
    if (faint) return;
    if (g_frameEvents.size() < 512) {
        FrameEvent e{ g_trackClass, g_world[0] + cx * wx, g_world[2] + cy * wy, r, std::min(g_trackA, 1.0f) };
        e.puff = g_trackPuff;
        e.tex = g_trackTex;
        e.seq = g_drawSeq;
        g_frameEvents.push_back(e);
    }
}

// ---- GPU particles
static int g_partCur, g_partNext;
struct Spawn { int slot; float pos[4], vel[4]; };
static std::vector<Spawn> g_spawns;

static void SpawnParticle(int type, float x, float y, float vx, float vy, float life) {
    if (!g_parts[0].fbo || g_spawns.size() > 4096) return;
    Spawn s;
    s.slot = g_partNext;
    g_partNext = (g_partNext + 1) % kMaxParticles;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = RandRange(-1, 1); s.pos[3] = life;
    s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = (type == 1 || type == 13) ? -1.0f : 0.0f; s.vel[3] = (float)type;   // -1: debris takes its colour on its first update
    g_spawns.push_back(s);
}

// Snowflake / sand / ash (type 6): vel.z keeps the life at spawn so it can fade in where it appears.
static void SpawnFlake(float x, float y, float vx, float vy, float life) {
    if (!g_parts[0].fbo || g_spawns.size() > 4096) return;
    Spawn s;
    s.slot = g_partNext;
    g_partNext = (g_partNext + 1) % kMaxParticles;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = RandRange(-1, 1); s.pos[3] = life;
    s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = life; s.vel[3] = 6.0f;
    g_spawns.push_back(s);
}

// Steam / smoke puff (types 8 and 9): pos.z keeps the life at spawn and vel.z the size, so it can grow.
static void SpawnPuff(int type, float x, float y, float vx, float vy, float life, float size) {
    if (!g_parts[0].fbo || g_spawns.size() > 4096) return;
    g_fxPuffs++;
    Spawn s;
    s.slot = g_partNext;
    g_partNext = (g_partNext + 1) % kMaxParticles;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = life; s.pos[3] = life;
    s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = size; s.vel[3] = (float)type;
    g_spawns.push_back(s);
}

// A candidate (type 19) for dust or grit shaken off a surface. The particle update keeps it only where there is one -
// grit or a dust puff off the underside above it, or dust off the top below it - and drops it otherwise.
// code: 0 grit, 1..999 an underside dust puff (its size), 1000 + size dust off a top.
static void SpawnShaken(float x, float y, float vx, float vy, float life, float code) {
    if (!g_parts[0].fbo || g_spawns.size() > 4096) return;
    Spawn s;
    s.slot = g_partNext;
    g_partNext = (g_partNext + 1) % kMaxParticles;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = 0; s.pos[3] = life;
    s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = code; s.vel[3] = 19.0f;
    g_spawns.push_back(s);
}

// Lights that live for one frame (rocket plumes follow their rocket).
struct FrameLight { float x, y, r, inten, col[3]; };
static std::vector<FrameLight> g_frameLights;
static float g_puffBudget = 60;

// Tracers: a projectile is matched to the nearest one of the last frame to get its velocity, then a
// short-lived glowing streak (particle type 10) is drawn behind it along that velocity.
struct TracerPos { float x, y; bool rung; };
struct TrailHeat { float x, y; };
static std::vector<TrailHeat> g_gfTrail;   // this frame: hot wakes behind shells (heat haze)
static float g_gfRings = 3;                // budget of sound-barrier rings (~3 a second)
static std::vector<TracerPos> g_tracersPrev, g_tracersNow;
static float g_tracerDt = 1.0f / 60.0f;
static void TracerFrame(const struct FrameEvent& e);

static void UploadSpawns() {
    if (g_spawns.empty() || !G.TexSubImage2D) return;
    Target& t = g_parts[g_partCur];
    size_t i = 0;
    while (i < g_spawns.size()) {   // contiguous slots within one texture row go up together
        size_t j = i + 1;
        while (j < g_spawns.size() && g_spawns[j].slot == g_spawns[j - 1].slot + 1 && g_spawns[j].slot % kPW != 0) j++;
        int x = g_spawns[i].slot % kPW, y = g_spawns[i].slot / kPW, n = (int)(j - i);
        std::vector<float> pos(n * 4), vel(n * 4);
        for (int k = 0; k < n; k++) {
            memcpy(&pos[k * 4], g_spawns[i + k].pos, 16);
            memcpy(&vel[k * 4], g_spawns[i + k].vel, 16);
        }
        G.BindTexture(GL_TEXTURE_2D, t.tex);
        G.TexSubImage2D(GL_TEXTURE_2D, 0, x, y, n, 1, GL_RGBA, GL_FLOAT, pos.data());
        G.BindTexture(GL_TEXTURE_2D, t.tex2);
        G.TexSubImage2D(GL_TEXTURE_2D, 0, x, y, n, 1, GL_RGBA, GL_FLOAT, vel.data());
        i = j;
    }
    g_spawns.clear();
}

// ---- spawning effects for one new event
static float WorldHeight() { return fabsf(g_world[2] - g_world[3]); }

// Heat for the haze: spots that warm up when something hot happens there (explosions, every
// muzzle flash, sustained fire) and cool down over a few seconds, like the game's overheating.
struct HeatSpot { float x, y, r, heat; };
static std::vector<HeatSpot> g_heat;
static void AddHeat(float x, float y, float r, float amount) {
    for (auto& s : g_heat)
        if (hypotf(s.x - x, s.y - y) < std::max(s.r, r) * 1.2f) {
            s.heat = std::min(1.5f, s.heat + amount);
            s.x += (x - s.x) * 0.2f; s.y += (y - s.y) * 0.2f;
            s.r = std::max(s.r * 0.98f, r);
            return;
        }
    if (g_heat.size() < 48) g_heat.push_back({ x, y, r, std::min(1.5f, amount) });
}

// Rapid-fire weapons produce dozens of flash/spark events a second; cap how many spawn effects.
static float g_spawnBudget = 30;
static float g_nextBlastMag = -1;   // the size of the impact the game spawned for the next blast (engine watch), -1 unknown
static float g_fireEmbers = 8;   // shared ember budget of everything that burns
static float g_flashLights = 2;  // shared light budget of flash sprites (~3 per second)
static bool GfShakeFromGame();   // game events section: shake comes from the game's impacts instead
static bool TakeSpawn() {
    if (g_spawnBudget < 1.0f) return false;
    g_spawnBudget -= 1.0f;
    return true;
}

// Blast wind: each blast shoves the light particles around it outward for a moment, then the air rushing back draws
// them in a little (particle update uBlast). From the game's impacts (by their size) and from blast sprites.
struct GfBlast { float x, y, r, push, t0, t1; };   // t1: how long the push lasts
static std::vector<GfBlast> g_gfBlasts;
static void AddBlast(float x, float y, float r, float push) {
    if (cfg.gfBlastWind <= 0 || r < 1.0f) return;
    push *= cfg.gfBlastWind;
    for (auto& b : g_gfBlasts)   // the same blast seen twice (the game's impact and its sprite): the bigger of the two
        if (g_fxNow - b.t0 < 0.15f && hypotf(b.x - x, b.y - y) < 0.5f * std::max(b.r, r)) {
            b.r = std::max(b.r, r); b.push = std::max(b.push, push);
            return;
        }
    if (g_gfBlasts.size() >= 16) g_gfBlasts.erase(g_gfBlasts.begin());
    g_gfBlasts.push_back({ x, y, r, push, g_fxNow, 0.2f + 0.05f * std::min(r / 300.0f, 3.0f) });
}

// Burning chunks thrown out of a blast, flown on the CPU so they can trail smoke as they arc away.
struct SmokeChunk { float x, y, vx, vy, t0, life, size, acc; };
static std::vector<SmokeChunk> g_smokeChunks;
static void SmokeChunksFrame() {
    for (auto& c : g_smokeChunks) {
        c.vy += cfg.gravity * g_fxDt;
        c.x += c.vx * g_fxDt; c.y += c.vy * g_fxDt;
        c.acc += g_fxDt / 0.05f;
        float fade = 1.0f - (g_fxNow - c.t0) / c.life;
        while (c.acc >= 1.0f) {
            c.acc -= 1.0f;
            if (g_puffBudget < 1.0f) { c.acc = 0; break; }
            g_puffBudget -= 1.0f;
            SpawnPuff(9, c.x, c.y, RandRange(-0.2f, 0.2f) * c.size, -c.size * RandRange(0.2f, 0.5f), RandRange(1.2f, 2.0f), c.size * RandRange(0.8f, 1.3f) * (0.5f + 0.5f * fade));
        }
        if (Rand01() < g_fxDt * 20.0f) SpawnParticle(0, c.x, c.y, c.vx * 0.1f, c.vy * 0.1f, 0.3f);   // the glowing chunk itself
    }
    g_smokeChunks.erase(std::remove_if(g_smokeChunks.begin(), g_smokeChunks.end(), [](const SmokeChunk& c) { return g_fxNow - c.t0 > c.life; }), g_smokeChunks.end());
}

static void ExplosionBurst(float x, float y, float r, float scale) {
    CapHistNote(Fmt("SPAWN  explosion burst at (%.0f,%.0f) r=%.0f scale=%.1f: shockwave, flash light, fireball, debris, sparks", x, y, r, scale));
    g_fxBursts++;
    float rel = r / std::max(WorldHeight(), 1.0f);       // size relative to the view
    // The shockwave follows the real impact when the game said what it was (a 20 mm shell's impact_medium
    // ripples gently, a missile's impact_heavy hard); otherwise the sprite's size, as before.
    float magK = g_nextBlastMag < 0 ? 1.0f : std::min(std::max(g_nextBlastMag / 0.8f, 0.25f), 1.5f);
    g_nextBlastMag = -1;
    if (cfg.shockStrength > 0)
        g_shocks.push_back({ x, y, r * cfg.shockSize * sqrtf(magK), g_fxNow, 0.55f + rel * 1.5f,
                             cfg.shockStrength * std::min(1.0f, 0.4f + rel * 6.0f) * scale * magK });
    AddBlast(x, y, r * 3.0f * sqrtf(magK), 36.0f * r * std::min(1.0f, 0.3f + 0.6f * magK) * scale);
    if (cfg.flashLight > 0) {
        FlashLight f{ x, y, r * 1.6f, g_fxNow, 0.22f + rel, cfg.flashLight * scale, { 1.0f, 0.62f, 0.28f } };
        g_flashes.push_back(f);
    }
    if (cfg.fireballs)
        g_fireballs.push_back({ x, y, r * cfg.fireballSize, g_fxNow, 0.85f + rel * 1.5f, Rand01() * 100.0f });
    AddHeat(x, y, r * 1.2f, 0.9f * scale);
    if (WaterOn() && fabsf(y - g_waterLevel) < r * 1.5f) {   // an explosion at the water: a plume of spray and mist
        float wl = g_waterLevel;
        for (int i = 0; i < 60; i++) SpawnParticle(4, x + RandRange(-r, r) * 0.6f, wl, RandRange(-0.35f, 0.35f) * r * 6.0f, -r * RandRange(5.0f, 14.0f), RandRange(0.6f, 1.5f));
        for (int i = 0; i < 6; i++) SpawnPuff(8, x + RandRange(-r, r) * 0.6f, wl - r * 0.3f, RandRange(-0.2f, 0.2f) * r, -r * RandRange(0.5f, 1.2f), RandRange(1.5f, 2.8f), r * RandRange(0.8f, 1.4f));
    }
    if (!GfShakeFromGame()) g_trauma = std::min(1.0f, g_trauma + cfg.shake * std::min(1.0f, rel * 10.0f) * scale);
    if (cfg.pulse > 0 && rel > 0.02f) { g_pulseT0 = g_fxNow; g_pulseAmt = std::min(1.0f, rel * 8.0f) * cfg.pulse * scale; }
    int debris = (int)(cfg.debris * (8 + rel * 300) * scale), sparks = (int)(cfg.sparks * (10 + rel * 300) * scale);
    int embers = (int)(cfg.embers * (6 + rel * 160) * scale);
    for (int i = 0; i < debris; i++) {
        float a = RandRange(-3.0f, 0.15f), s = r * RandRange(3.0f, 9.0f);
        SpawnParticle(1, x + RandRange(-r, r) * 0.3f, y + RandRange(-r, r) * 0.3f, cosf(a) * s, sinf(a) * s, RandRange(1.5f, 3.5f));
    }
    for (int i = 0; i < sparks; i++) {
        float a = RandRange(0, 6.2832f), s = r * RandRange(6.0f, 16.0f);
        SpawnParticle(2, x, y, cosf(a) * s, sinf(a) * s - r * 3.0f, RandRange(0.25f, 0.8f));
    }
    for (int i = 0; i < embers; i++) {
        float a = RandRange(0, 6.2832f), s = r * RandRange(0.5f, 3.0f);
        SpawnParticle(0, x + cosf(a) * r * 0.5f, y + sinf(a) * r * 0.5f, cosf(a) * s, sinf(a) * s - r, RandRange(1.0f, 2.8f));
    }
    if (cfg.simSmoke && cfg.moreSimFx) {   // its smoke: hot gas cooling into a dark cloud, and burning chunks trailing smoke
        int n = std::min(10, (int)(3 + rel * 40));
        for (int i = 0; i < n && g_puffBudget >= 1.0f; i++) {
            g_puffBudget -= 1.0f;
            float a = RandRange(0, 6.2832f), sp = r * RandRange(0.4f, 1.4f);
            SpawnPuff(i < n / 3 ? 12 : 9, x + cosf(a) * r * 0.4f, y + sinf(a) * r * 0.3f, cosf(a) * sp, sinf(a) * sp * 0.5f - r * RandRange(0.3f, 0.8f),
                      RandRange(3.0f, 5.0f), r * RandRange(0.6f, 1.1f));
        }
        int trails = std::min(5, (int)(1 + rel * 30 * cfg.debris));
        for (int i = 0; i < trails && g_smokeChunks.size() < 32; i++) {
            float a = RandRange(-2.6f, -0.5f), s = r * RandRange(5.0f, 10.0f);
            g_smokeChunks.push_back({ x, y, cosf(a) * s, sinf(a) * s, g_fxNow, RandRange(1.0f, 2.0f), r * 0.25f, 0.0f });
        }
    }
    if (cfg.impactFrames && rel > cfg.impactMinSize && g_fxNow - g_lastImpact > cfg.impactCooldown) {
        g_impactT0 = g_lastImpact = g_fxNow;
        g_impactX = x; g_impactY = y;
    }
}

static void OnSpawn(uint8_t cls, float x, float y, float r, float a = 1.0f, uint8_t puff = 0) {
    CapHistNote(Fmt("SPAWN  %-10s at (%.0f,%.0f) r=%.0f a=%.2f", EvName(cls), x, y, r, a));
    switch (cls) {
    case kEvExplosion: ExplosionBurst(x, y, r, 1.0f); break;
    case kEvBridgeImpact: ExplosionBurst(x, y, r, 2.0f); g_impactT0 = g_lastImpact = g_fxNow; g_impactX = x; g_impactY = y; break;
    case kEvBridgeShock: g_shocks.push_back({ x, y, r * cfg.shockSize, g_fxNow, 0.9f, cfg.shockStrength }); break;
    case kEvBridgeFlash:
        if (cfg.flashLight > 0) g_flashes.push_back({ x, y, r * 2.5f, g_fxNow, 0.09f, cfg.flashLight * 0.6f, { 1.0f, 0.85f, 0.55f } });
        break;
    case kEvFlash: {   // muzzle flashes: a small, short light; repeated shots refresh it instead of stacking
        AddHeat(x, y, std::max(r, WorldHeight() * 0.015f) * 1.5f, 0.06f * cfg.gunHeat);
        if (!TakeSpawn()) break;
        float rel = r / std::max(WorldHeight(), 1.0f);
        float inten = cfg.flashLight * cfg.muzzleLight * a * std::min(1.0f, 0.3f + rel * 20.0f);
        bool merged = false;
        for (auto& f : g_flashes)
            if (g_fxNow - f.t0 < 0.15f && hypotf(f.x - x, f.y - y) < std::max(f.r, r) && f.life < 0.1f) {
                f.t0 = g_fxNow; merged = true; break;
            }
        // fires and bullet impacts in many mods draw dozens of flash sprites: few, small lights only
        if (!merged && inten > 0 && g_flashLights >= 1.0f) {
            g_flashLights -= 1.0f;
            g_flashes.push_back({ x, y, r * 0.9f, g_fxNow, 0.07f, std::min(inten, 0.8f), { 1.0f, 0.8f, 0.5f } });
        }
        int sparks = (int)(cfg.sparks * 2);
        for (int i = 0; i < sparks; i++) {
            float a = RandRange(0, 6.2832f), s = r * RandRange(4.0f, 10.0f);
            SpawnParticle(2, x, y, cosf(a) * s, sinf(a) * s, RandRange(0.12f, 0.3f));
        }
        break;
    }
    case kEvSteam:
    case kEvSmoke: {   // a game puff appeared: simulated puffs take its place
        if (!cfg.simSmoke) break;
        int type = puff ? puff : cls == kEvSteam ? 8 : 9;
        if (type == 12) {   // a gun's blast cloud: hot gas bursting out, cooling into a dark cloud
            for (int i = 0; i < 7 && g_puffBudget >= 1.0f; i++) {
                g_puffBudget -= 1.0f;
                float ang = RandRange(0.0f, 6.2832f), sp = r * RandRange(0.6f, 1.6f);
                SpawnPuff(i < 4 ? 12 : 9, x + cosf(ang) * r * 0.25f, y + sinf(ang) * r * 0.25f, cosf(ang) * sp, sinf(ang) * sp - r * 0.3f,
                          RandRange(2.0f, 3.4f), r * RandRange(0.55f, 0.9f));
            }
            break;
        }
        int n = 1 + (int)std::min(2.0f, r / std::max(WorldHeight() * 0.03f, 1.0f));
        for (int i = 0; i < n && g_puffBudget >= 1.0f; i++) {
            g_puffBudget -= 1.0f;
            SpawnPuff(type, x + RandRange(-r, r) * 0.3f, y + RandRange(-r, r) * 0.3f,
                      RandRange(-0.15f, 0.15f) * r, -r * RandRange(0.2f, 0.6f) * (type == 11 ? 0.3f : 1.0f), RandRange(2.2f, 3.6f), r * RandRange(0.8f, 1.2f));
        }
        break;
    }
    case kEvSpark:
        if (!TakeSpawn()) break;
        for (int i = 0; i < (int)(cfg.sparks * 2); i++) {
            float a = RandRange(0, 6.2832f), s = r * RandRange(3.0f, 8.0f);
            SpawnParticle(2, x, y, cosf(a) * s, sinf(a) * s, RandRange(0.15f, 0.35f));
        }
        break;
    default: break;
    }
}

void LightningStrike(float worldX, bool forced);   // weather section

// ---- lab: effects on demand at a screen position (the overlay's click tools and tour)
struct LabLight { bool on; float x, y, r, inten, col[3]; };
static LabLight g_labLights[8];

bool GfxWorldVisible() { return g_postRan && g_w > 0 && g_h > 0; }

static void ScreenToWorld(float px, float py, float& x, float& y) {
    x = g_world[0] + px / std::max(g_w, 1) * (g_world[1] - g_world[0]);
    y = g_world[2] + (g_h - py) / std::max(g_h, 1) * (g_world[3] - g_world[2]);
}

// Spawns are queued and run at the start of the next post chain, inside its saved GL state
// (lightning reads the solid mask back from the GPU).
struct LabSpawn { int kind; float x, y, r; };
static std::vector<LabSpawn> g_labQueue;
static double g_snowBrushT = -1000;   // the snow brush keeps the snow layer running

void GfxLabSpawn(int kind, float px, float py, float size) {
    if (!GfxWorldVisible() || g_labQueue.size() > 64) return;
    float x, y;
    ScreenToWorld(px, py, x, y);
    g_labQueue.push_back({ kind, x, y, std::max(size, 0.004f) * WorldHeight() });
}

static void RunLabQueue() {
    for (auto& q : g_labQueue) {
        float x = q.x, y = q.y, r = q.r;
        switch (q.kind) {
        case kLabExplosion: ExplosionBurst(x, y, r, 1.0f); break;
        case kLabMega: OnSpawn(kEvBridgeImpact, x, y, r * 1.5f); break;
        case kLabLightning: LightningStrike(x, true); break;
        case kLabShock: OnSpawn(kEvBridgeShock, x, y, r); break;
        case kLabFlash: OnSpawn(kEvBridgeFlash, x, y, r); break;
        case kLabHeat:
            AddHeat(x, y, r, 1.5f);
            for (int i = 0; i < 40; i++)
                SpawnParticle(0, x + RandRange(-r, r) * 0.6f, y + RandRange(-r, r) * 0.3f, RandRange(-30, 30), RandRange(-120, -40), RandRange(1.0f, 2.5f));
            break;
        case kLabSnow: {   // snow brush: flakes (or sand / ash in those weathers) sprayed around the mouse
            g_snowBrushT = g_fxNow;
            float vh = WorldHeight();
            int count = (int)std::min(60.0f, 6.0f + r / std::max(vh * 0.005f, 1.0f) * 2.0f);
            for (int i = 0; i < count; i++) {
                float a = RandRange(0, 6.2832f), d = sqrtf(Rand01()) * r;
                SpawnFlake(x + cosf(a) * d, y + sinf(a) * d * 0.6f, RandRange(-0.02f, 0.02f) * vh, vh * RandRange(0.06f, 0.12f), 14.0f);
            }
            break;
        }
        case kLabSparks:
            for (int i = 0; i < 90; i++) {
                float a = RandRange(0, 6.2832f), s = r * RandRange(4.0f, 14.0f);
                SpawnParticle(2, x, y, cosf(a) * s, sinf(a) * s - r * 2.0f, RandRange(0.3f, 0.9f));
            }
            break;
        }
    }
    g_labQueue.clear();
}

void GfxLabSetLight(int slot, bool on, float px, float py, float size, float intensity, const float col[3]) {
    if (slot < 0 || slot >= 8) return;
    LabLight& l = g_labLights[slot];
    l.on = on && GfxWorldVisible();
    if (!l.on) return;
    ScreenToWorld(px, py, l.x, l.y);
    l.r = std::max(size, 0.004f) * WorldHeight();
    l.inten = intensity;
    for (int k = 0; k < 3; k++) l.col[k] = col[k];
}
void GfxLabClearLights() { for (auto& l : g_labLights) l.on = false; }
int GfxLabLightCount() { int n = 0; for (auto& l : g_labLights) n += l.on; return n; }

static float g_realDt = 1.0f / 60.0f;   // real (not slowed) frame time, for projectile velocities
static void TracerFrame(const FrameEvent& e) {
    if (cfg.tracerGlow <= 0) return;
    float best = WorldHeight() * 0.25f, bx = 0, by = 0;
    bool found = false, rung = false;
    for (auto& p : g_tracersPrev) {
        float d = hypotf(p.x - e.x, p.y - e.y);
        if (d < best && d > 1e-3f) { best = d; bx = p.x; by = p.y; found = true; rung = p.rung; }
    }
    g_tracersNow.push_back({ e.x, e.y, rung });
    if (!found || !g_parts[0].fbo || g_spawns.size() > 4096) return;
    float vx = (e.x - bx) / g_realDt, vy = (e.y - by) / g_realDt;
    float kind = e.r < WorldHeight() * 0.008f ? 0.0f : 1.0f;   // small = bullet, larger = shell
    if (kind > 0.5f && cfg.gameFx && cfg.gfInFlight && cfg.impactFx && !EngineTracking()) {   // from the sprites until the game's list is tracked
        float vh = WorldHeight();
        if (g_gfTrail.size() < 8) g_gfTrail.push_back({ e.x - vx * 0.03f, e.y - vy * 0.03f });   // the shell's hot wake shimmers
        if (!rung && hypotf(vx, vy) > vh * 1.6f && g_gfRings >= 1.0f) {   // a fast round: a faint sound-barrier ring, once
            g_gfRings -= 1.0f;
            g_shocks.push_back({ e.x, e.y, vh * 0.035f, g_fxNow, 0.3f, 0.3f * cfg.shockStrength });
            g_tracersNow.back().rung = true;
        }
    }
    Spawn s;
    s.slot = g_partNext;
    g_partNext = (g_partNext + 1) % kMaxParticles;
    s.pos[0] = e.x; s.pos[1] = e.y; s.pos[2] = 0; s.pos[3] = 0.05f;
    s.vel[0] = vx * 0.25f; s.vel[1] = vy * 0.25f; s.vel[2] = kind; s.vel[3] = 10.0f;   // drawn length follows speed
    g_spawns.push_back(s);
    if (g_frameLights.size() < 12)
        g_frameLights.push_back({ e.x, e.y, WorldHeight() * 0.025f, 0.5f * cfg.tracerGlow, { 1.0f, 0.75f, 0.4f } });
}

// ================================================================ effects from the game's own events
// engine.cpp reports what the game did each frame: every effect it spawned (file and position), where a
// projectile hit and what it hit, which weapon fired, which device died. That drives effects the sprites
// alone can't tell apart: the material that was hit, the weapon, how big the impact really was. The names
// are the game's own (effects/armor_bullet_hit.lua, "bracing", "reactor"), not guesses from images.
enum GfMat : uint8_t { kMatNone, kMatMetal, kMatWood, kMatShield, kMatGround, kMatWater, kMatDust };
static const char* const kMatNames[] = { "structure", "metal", "wood", "shield", "ground", "water", "sandbags" };
static bool GfHas(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
static std::string GfLower(const char* s) {
    std::string r = s;
    for (auto& ch : r) ch = (char)tolower((unsigned char)ch);
    return r;
}
static GfMat MaterialOf(const std::string& n) {   // a hit save name ("armour", "bracing") or an effect file's name
    if (GfHas(n, "shield")) return kMatShield;
    if (GfHas(n, "sandbag")) return kMatDust;
    if (GfHas(n, "armor") || GfHas(n, "armour") || GfHas(n, "door") || GfHas(n, "metal") || GfHas(n, "plate")) return kMatMetal;
    if (GfHas(n, "bracing") || GfHas(n, "wood") || GfHas(n, "rope")) return kMatWood;
    if (GfHas(n, "ground") || GfHas(n, "terrain") || GfHas(n, "rock")) return kMatGround;
    return kMatNone;
}
// How big an impact is, from the effect's (or projectile's) name: 0.18 a bullet .. 1 a missile .. 3 a nuke.
static float MagnitudeOf(const std::string& f) {
    if (GfHas(f, "nuke") || GfHas(f, "mushroom")) return 3.0f;
    if (GfHas(f, "warhead") || GfHas(f, "howitzer") || GfHas(f, "reactor") || GfHas(f, "large")) return 1.6f;
    if (GfHas(f, "heavy") || GfHas(f, "missile") || GfHas(f, "rocket")) return 1.0f;
    if (GfHas(f, "bullet") || GfHas(f, "light") || GfHas(f, "sniper") || GfHas(f, "machinegun") || GfHas(f, "minigun") || GfHas(f, "flak") || GfHas(f, "ricochet"))
        return 0.18f;
    if (GfHas(f, "medium") || GfHas(f, "mortar") || GfHas(f, "cannon") || GfHas(f, "explode") || GfHas(f, "deckgun")) return 0.55f;
    return 0.4f;
}
enum GfKind : uint8_t { kGfNone, kGfImpact, kGfMuzzle, kGfLaunch, kGfBeamHit, kGfCollapse, kGfCharge };
static GfKind KindOf(const std::string& b) {   // b: the effect file's name, lower case, without folder and .lua
    if (GfHas(b, "beam_hit") || GfHas(b, "beam_impact")) return kGfBeamHit;
    // beamlaser.lua: ReloadEffect laser_reload 3.4 s before it is ready; missilelauncher.lua: FireEffect
    // warhead_warmup, launched FireDelay = 5 s later (missile_swarm_warmup likewise)
    if ((GfHas(b, "laser") && GfHas(b, "reload")) || GfHas(b, "warmup")) return kGfCharge;
    if (GfHas(b, "launch")) return kGfLaunch;   // missile_launch, warhead_launch, shuttle_launch
    if ((b.compare(0, 5, "fire_") == 0 && !GfHas(b, "extinguish") && !GfHas(b, "alarm")) || GfHas(b, "muzzle") ||
        (GfHas(b, "_flash") && !GfHas(b, "node")))
        return kGfMuzzle;                       // fire_cannon, fire_mortar, fire_minigun, fire_laser, sniper_flash
    if (GfHas(b, "snap") || GfHas(b, "structure_break") || GfHas(b, "bracing_destroy") || GfHas(b, "armor_destroy") || GfHas(b, "node_explode"))
        return kGfCollapse;
    if (GfHas(b, "destroy_fire")) return kGfNone;
    for (const char* k : { "explo", "impact", "hit", "destroy", "burst", "blast", "detonat", "nuke", "mushroom", "ricochet" })
        if (GfHas(b, k)) return kGfImpact;
    return kGfNone;
}

struct GfHit { float x, y, nx, ny, t; GfMat mat; std::string what; bool used; int type; };   // type: the game's hit type (2 = ground)
static std::vector<GfHit> g_gfHits;           // projectile hits: the exact surface, its normal and what it is
struct GfBoom { float x, y, t; std::string file; bool used; };
static std::vector<GfBoom> g_gfBooms;         // recent explosions, to place the devices the game destroyed
struct GfDeath { std::string dev; float t; };
static std::vector<GfDeath> g_gfDeaths;
struct GfGun { float x, y, heat, last, acc; };
static std::vector<GfGun> g_gfGuns;           // rapid-fire guns heating up
struct GfLaser { float x, y, t0, lastHit, hx, hy; int team; bool hit; float farD, farT; bool firebeam; };   // far: the beam's reach
static std::vector<GfLaser> g_gfLasers;
struct GfGlow { float x, y, r, t0, life, inten, col[3]; bool flicker; };
static std::vector<GfGlow> g_gfGlows;         // a dead reactor's glow, a battery's electrical flicker
struct GfSmoker { float x, y, t0, life, acc; };
static std::vector<GfSmoker> g_gfSmokers;     // wrecks smoking
struct GfCharge { float x, y, t0, dur; bool laser; };
static std::vector<GfCharge> g_gfCharges;     // a laser charging, a missile silo warming up
static std::unordered_map<uint32_t, bool> g_gfRung;   // projectiles in flight (by effect id) that already made their ring
struct GfLater { float t, x, y, mag; };
static std::vector<GfLater> g_gfLater;        // heavy weapons: the shockwave ring arrives a moment later
struct Decal { float x, y, r, t0, life, strength, seed, hotT; uint8_t kind; };
static std::vector<Decal> g_decals;           // scorch marks (0), craters (1), molten marks (2)
static float g_gfLightBudget = 6, g_exposureT0 = -100, g_exposureAmt;
static float g_gfMelt[8 * 4];                 // heat that melts snow (world x, y, r, melt per step)
static int g_gfMeltN, g_gfLogged;

static bool GfOn() { return cfg.gameFx && cfg.impactFx && EngineWatching(); }
static bool GfShakeFromGame() { return GfOn(); }
static void GfStat(const std::string& what, const char* did) {
    if (g_gfStats.size() < 300) g_gfStats[what + " -> " + did]++;
}
static void GfLight(float x, float y, float r, float life, float inten, float cr, float cg, float cb, bool always = false) {
    if (!cfg.gfLights || inten <= 0.0f) return;
    if (!always) {
        if (g_gfLightBudget < 1.0f) return;
        g_gfLightBudget -= 1.0f;
    }
    g_flashes.push_back({ x, y, r, g_fxNow, life, inten, { cr, cg, cb } });
}
static void GfPuff(int type, float x, float y, float vx, float vy, float life, float size) {
    if (g_puffBudget < 1.0f) return;
    g_puffBudget -= 1.0f;
    SpawnPuff(type, x, y, vx, vy, life, size);
}
static int GfCount(float n) { n *= cfg.gfAmount; int k = (int)n; return k + (Rand01() < n - k ? 1 : 0); }
// A spray of particles in a cone around (nx, ny).
static void GfSpray(int type, int n, float x, float y, float nx, float ny, float spread, float speed, float life0, float life1) {
    float base = atan2f(ny, nx);
    for (int i = 0; i < n; i++) {
        float a = base + RandRange(-spread, spread), v = speed * RandRange(0.35f, 1.0f);
        SpawnParticle(type, x, y, cosf(a) * v, sinf(a) * v, RandRange(life0, life1));
    }
}
// Shake by how big the impact really is and how near the middle of the view it is.
static void GfShake(float x, float y, float mag) {
    float vh = std::max(WorldHeight(), 1.0f);
    float cx = (g_world[0] + g_world[1]) * 0.5f, cy = (g_world[2] + g_world[3]) * 0.5f;
    float d = hypotf(x - cx, y - cy) / vh;
    float prox = EngineReady() ? 1.0f / (1.0f + d * d * 4.0f) : 0.7f;   // until the positions are confirmed: by size only
    float k = mag <= 0.2f ? 0.012f : std::min(0.9f, mag * mag * 0.12f + mag * 0.05f);
    g_trauma = std::min(1.0f, g_trauma + cfg.shake * k * prox);
}
static void AddDecal(uint8_t kind, float x, float y, float r, float strength) {
    if (!cfg.gfScorch) return;
    for (auto& d : g_decals)   // repeated hits on one spot darken it instead of stacking dozens
        if (d.kind == kind && hypotf(d.x - x, d.y - y) < std::max(d.r, r) * 0.5f) {
            d.strength = std::min(1.5f, d.strength + strength * 0.25f);
            d.r = std::max(d.r, r);
            d.t0 = d.hotT = g_fxNow;
            return;
        }
    if (g_decals.size() >= 96) g_decals.erase(g_decals.begin());
    g_decals.push_back({ x, y, r, g_fxNow, std::max(cfg.scorchLife, 1.0f), strength, Rand01() * 100.0f, g_fxNow, kind });
}

static void HeavyFx(float x, float y, float mag) {
    if (!cfg.gfHeavy) return;
    float m = std::min(mag, 3.0f);
    GfLight(x, y, 380.0f * m, 0.35f + 0.1f * m, cfg.flashLight * 0.35f * std::min(m, 2.0f), 1.0f, 0.86f, 0.68f, true);
    g_gfLater.push_back({ g_fxNow + 0.12f + 0.05f * m, x, y, m });
    g_exposureT0 = g_fxNow; g_exposureAmt = std::min(1.0f, 0.35f * m);   // the camera adjusts to the flash
    if (cfg.pulse > 0) { g_pulseT0 = g_fxNow; g_pulseAmt = std::min(1.0f, 0.4f * m) * cfg.pulse; }
    int n = GfCount(6.0f + 4.0f * m);   // a dust wave rolling out along the ground
    for (int i = 0; i < n; i++) {
        float side = (i & 1) ? 1.0f : -1.0f;
        GfPuff(11, x + side * RandRange(0.0f, 60.0f), y - RandRange(0.0f, 30.0f), side * RandRange(300.0f, 800.0f) * std::min(m, 1.5f),
               -RandRange(20.0f, 100.0f), RandRange(2.0f, 3.5f), RandRange(50.0f, 100.0f) * std::min(m, 1.5f));
    }
}

// What a hit leaves behind over the next moments: dust and grit shaken off the structure above and around it (bigger
// hits loosen more, over a wider area), and dust kicked off the tops of the surfaces around it as the blast spreads.
// Spawned as candidates that the particle update keeps only where the solid mask has a surface.
struct GfShakeOff { float x, y, r, mag, t0; bool shake, kick; };
static std::vector<GfShakeOff> g_gfShakeOffs;
static float g_shakeBudget = 500;
static void ShakeOffFrame() {
    g_shakeBudget = std::min(500.0f, g_shakeBudget + g_fxDt * 500.0f);
    float now = g_fxNow;
    for (auto& s : g_gfShakeOffs) {
        float a0 = std::max(0.0f, now - g_fxDt - s.t0), a1 = now - s.t0, m = std::min(s.mag, 2.0f);
        if (s.shake) {   // most comes loose at once, then a trickle for a second
            const float tau = 0.35f;
            float share = expf(-a0 / tau) - expf(-a1 / tau);
            int grit = GfCount(share * 220.0f * powf(m, 1.2f)), dust = GfCount(share * (2.0f + 10.0f * m));
            for (int i = 0; i < grit + dust && g_shakeBudget >= 1.0f; i++) {
                g_shakeBudget -= 1.0f;
                float a = RandRange(0.0f, 6.2832f), d = s.r * powf(Rand01(), 0.8f), ox = cosf(a) * d, oy = sinf(a) * d;
                oy = oy < 0 ? oy * 1.3f : oy * 0.35f;   // mostly above it (world y grows down)
                if (i < grit) SpawnShaken(s.x + ox, s.y + oy, RandRange(-40.0f, 40.0f) + ox * 0.3f, RandRange(0.0f, 60.0f), RandRange(1.2f, 2.4f), 0.0f);
                else SpawnShaken(s.x + ox, s.y + oy, RandRange(-25.0f, 25.0f), RandRange(10.0f, 50.0f), RandRange(1.6f, 2.6f), std::min(s.r * RandRange(0.06f, 0.12f), 400.0f));
            }
        }
        if (s.kick && a0 < 0.4f) {   // the blast spreading out over 0.4 s kicks dust off the tops around it
            float rg = s.r * 1.6f;
            int n = GfCount((std::min(a1, 0.4f) - a0) / 0.4f * (10.0f + 24.0f * m));
            for (int i = 0; i < n && g_shakeBudget >= 1.0f; i++) {
                g_shakeBudget -= 1.0f;
                float side = Rand01() < 0.5f ? -1.0f : 1.0f, ring = rg * (RandRange(a0, std::min(a1, 0.4f)) / 0.4f + RandRange(0.0f, 0.1f));
                SpawnShaken(s.x + side * ring, s.y + RandRange(-0.15f, 0.35f) * rg, side * RandRange(80.0f, 260.0f) * (0.6f + 0.4f * m), -RandRange(30.0f, 120.0f),
                            RandRange(1.6f, 2.8f), 1000.0f + RandRange(25.0f, 55.0f) * (0.6f + 0.4f * m));
            }
        }
    }
    g_gfShakeOffs.erase(std::remove_if(g_gfShakeOffs.begin(), g_gfShakeOffs.end(), [&](const GfShakeOff& s) { return now - s.t0 > 1.6f; }), g_gfShakeOffs.end());
}
// The blast wind, the shaking and the haze of one hit, by its size and what it hit.
static void AftermathFx(GfMat mat, float mag, float x, float y) {
    float br = 160.0f + 360.0f * powf(std::min(mag, 3.0f), 0.75f);   // ~260 a bullet .. 520 a missile .. 980 a nuke
    AddBlast(x, y, br, 12.0f * br * std::min(1.0f, 0.25f + mag));
    if (mat == kMatShield || mat == kMatWater) return;   // nothing there to shake loose
    bool minor = mag < 0.3f;
    bool shake = cfg.gfShakeOff, kick = cfg.gfGroundBlast && !minor;
    if ((shake || kick) && g_gfShakeOffs.size() < 24 && (!minor || TakeSpawn()))
        g_gfShakeOffs.push_back({ x, y, 100.0f + 220.0f * std::min(mag, 2.0f), mag, g_fxNow, shake, kick });
    float s = 40.0f + 90.0f * std::min(mag, 2.0f);
    if (cfg.gfDustHaze && mag >= 0.5f)   // a heavy hit: a thin haze of dust hangs about, then settles (size < 0: haze)
        for (int i = 0, n = GfCount(2.0f + 2.5f * std::min(mag, 2.0f)); i < n; i++)
            GfPuff(11, x + RandRange(-s, s) * 0.6f, y - s * RandRange(0.0f, 0.6f), RandRange(-30.0f, 30.0f), -RandRange(0.0f, 40.0f), RandRange(5.0f, 8.0f),
                   -s * RandRange(1.0f, 1.6f));
    if (cfg.gfGroundBlast && mat == kMatGround && !minor)   // a ring of dark smoke blown out along the ground
        for (int i = 0, n = GfCount(8.0f + 6.0f * std::min(mag, 2.0f)); i < n; i++) {
            float side = (i & 1) ? 1.0f : -1.0f;
            GfPuff(i % 3 == 2 ? 11 : 9, x + side * RandRange(0.0f, s * 0.2f), y - RandRange(0.0f, s * 0.15f), side * s * RandRange(3.0f, 8.0f),
                   -s * RandRange(0.05f, 0.4f), RandRange(2.2f, 3.6f), s * RandRange(0.3f, 0.55f));
        }
}

static void ImpactFx(GfMat mat, float mag, float x, float y, float nx, float ny) {
    float s = 40.0f + 90.0f * std::min(mag, 2.0f);   // how far things fly / how big (world units)
    bool minor = mag < 0.3f;
    if (cfg.gfImpacts && (!minor || TakeSpawn())) {
        switch (mat) {
        case kMatMetal:    // sparks off the armour along its surface, molten slag from heavier hits
            GfSpray(2, GfCount(4 + mag * 24), x, y, nx, ny, 1.1f, s * 11.0f, 0.15f, 0.5f);
            if (!minor) GfSpray(14, GfCount(2 + mag * 6), x, y, nx, ny, 0.8f, s * 5.0f, 0.8f, 1.6f);
            GfLight(x, y, s * 2.2f, 0.07f, cfg.flashLight * 0.3f * (0.5f + mag), 1.0f, 0.85f, 0.6f);
            break;
        case kMatWood:     // splinters and dust off the bracing
            GfSpray(13, GfCount(3 + mag * 14), x, y, nx, ny, 1.2f, s * 7.0f, 1.2f, 2.6f);
            for (int i = 0, n = GfCount(0.5f + mag * 2); i < n; i++)
                GfPuff(11, x + nx * s * 0.3f, y + ny * s * 0.3f, nx * s * RandRange(0.5f, 1.5f), ny * s * RandRange(0.5f, 1.5f), RandRange(1.5f, 2.5f), s * RandRange(0.4f, 0.7f));
            break;
        case kMatShield:   // a ripple across the energy shield and crackling arcs
            g_shocks.push_back({ x, y, s * 3.0f, g_fxNow, 0.35f, 0.35f * cfg.shockStrength });
            GfSpray(15, GfCount(4 + mag * 12), x, y, nx, ny, 1.6f, s * 8.0f, 0.15f, 0.4f);
            GfLight(x, y, s * 3.0f, 0.12f, cfg.flashLight * 0.35f * (0.5f + mag), 0.5f, 0.8f, 1.0f);
            break;
        case kMatGround:   // a burst of earth and dust thrown up off the ground
            for (int i = 0, n = GfCount(1 + mag * 3); i < n; i++)
                GfPuff(11, x + RandRange(-s, s) * 0.3f, y - s * 0.2f, nx * s * RandRange(0.5f, 2.0f) + RandRange(-s, s) * 0.5f, ny * s * RandRange(1.0f, 3.0f),
                       RandRange(2.0f, 3.2f), s * RandRange(0.5f, 0.9f));
            if (!minor) GfSpray(1, GfCount(mag * 8), x, y, nx, ny, 0.9f, s * 7.0f, 1.5f, 3.0f);
            break;
        case kMatWater: {  // a splash: a column of spray and mist
            float wl = g_waterLevel;
            GfSpray(4, GfCount(10 + mag * 40), x, wl, 0.0f, -1.0f, 0.35f, s * 12.0f, 0.6f, 1.5f);
            for (int i = 0, n = GfCount(1 + mag * 4); i < n; i++)
                GfPuff(8, x + RandRange(-s, s) * 0.5f, wl - s * 0.3f, RandRange(-0.2f, 0.2f) * s, -s * RandRange(0.4f, 1.0f), RandRange(1.5f, 2.6f), s * RandRange(0.8f, 1.3f));
            g_shocks.push_back({ x, wl, s * 2.0f, g_fxNow, 0.4f, 0.25f * cfg.shockStrength });
            break;
        }
        case kMatDust:     // sandbags: a puff of sand
            for (int i = 0, n = GfCount(2 + mag * 3); i < n; i++)
                GfPuff(11, x, y, nx * s * RandRange(0.5f, 1.5f) + RandRange(-s, s) * 0.4f, ny * s * RandRange(0.5f, 1.5f), RandRange(1.8f, 2.8f), s * RandRange(0.5f, 0.8f));
            break;
        default:           // something else: a few sparks and a little dust
            GfSpray(2, GfCount(3 + mag * 10), x, y, nx, ny, 1.3f, s * 9.0f, 0.15f, 0.45f);
            if (!minor) GfPuff(11, x, y, nx * s, ny * s, RandRange(1.5f, 2.5f), s * 0.6f);
            break;
        }
    }
    if (mag >= 0.3f && mat != kMatShield && mat != kMatWater)
        AddDecal(mat == kMatGround ? 1 : 0, x, y, 35.0f + 110.0f * std::min(mag, 2.0f), 0.55f + 0.3f * std::min(mag, 1.5f));
    if (mat != kMatMetal && mat != kMatShield && !minor)   // the hit lights the structure around it for a moment
        GfLight(x, y, s * 3.0f, 0.1f + 0.05f * mag, cfg.flashLight * 0.25f * (0.4f + mag), 1.0f, 0.7f, 0.4f);
    GfShake(x, y, mag);
    AftermathFx(mat, mag, x, y);
}

static void ChargeFx(const std::string& b, float x, float y) {
    bool laser = GfHas(b, "laser");
    if (laser ? !cfg.gfLasers : !cfg.gfMuzzles) return;
    float dur = laser ? 3.4f : GfHas(b, "warhead") ? 5.0f : 2.0f;
    for (auto& c : g_gfCharges)
        if (c.laser == laser && hypotf(c.x - x, c.y - y) < 80.0f) { c.t0 = g_fxNow; c.dur = dur; return; }
    if (g_gfCharges.size() < 24) g_gfCharges.push_back({ x, y, g_fxNow, dur, laser });
}
// The charge that a weapon firing at (x, y) ends: its glow discharges.
static bool EndCharge(bool laser, float x, float y, float within) {
    for (size_t i = 0; i < g_gfCharges.size(); i++)
        if (g_gfCharges[i].laser == laser && hypotf(g_gfCharges[i].x - x, g_gfCharges[i].y - y) < within) {
            g_gfCharges.erase(g_gfCharges.begin() + i);
            return true;
        }
    return false;
}

static void MuzzleFx(const std::string& b, float x, float y, float dx, float dy, bool hasDir, int team) {
    if (!cfg.gfMuzzles && !(cfg.gfLasers && GfHas(b, "laser"))) return;
    if (GfHas(b, "water")) return;   // fire_watercannon.lua: a water cannon has no smoke or blast (the milk miko has her own)
    if (!hasDir) { dx = 0.0f; dy = -1.0f; }
    if (GfHas(b, "laser") || GfHas(b, "beam")) {   // a laser fires (the beam follows 0.1 s later): the charge discharges
        if (!cfg.gfLasers) return;
        bool charged = EndCharge(true, x, y, 250.0f);
        GfLight(x, y, charged ? 420.0f : 300.0f, 0.12f, cfg.flashLight * (charged ? 0.8f : 0.5f), 0.75f, 0.88f, 1.0f, true);
        if (charged) GfSpray(15, GfCount(10), x, y, dx, dy, 1.2f, 900.0f, 0.1f, 0.3f);
        if (g_gfLasers.size() < 16) g_gfLasers.push_back({ x, y, g_fxNow, -100.0f, x, y, team, false, 0.0f, -100.0f, GfHas(b, "firebeam") });
        return;
    }
    if (!cfg.gfMuzzles) return;
    if (GfHas(b, "cannon") || GfHas(b, "deckgun") || GfHas(b, "howitzer") || GfHas(b, "flak") || GfHas(b, "20mm")) {
        // a pressure ring at the muzzle and a cloud of smoke that hangs in the air
        g_shocks.push_back({ x + dx * 60.0f, y + dy * 60.0f, 320.0f, g_fxNow, 0.28f, 0.45f * cfg.shockStrength });
        for (int i = 0, n = GfCount(7); i < n; i++) {
            float sp = RandRange(150.0f, 500.0f);
            GfPuff(9, x + dx * RandRange(30.0f, 140.0f), y + dy * RandRange(30.0f, 140.0f), dx * sp + RandRange(-40.0f, 40.0f), dy * sp - RandRange(10.0f, 40.0f),
                   RandRange(3.5f, 6.0f), RandRange(28.0f, 50.0f));
        }
        GfLight(x + dx * 80.0f, y + dy * 80.0f, 450.0f, 0.07f, cfg.flashLight * 0.75f, 1.0f, 0.75f, 0.45f);
        GfShake(x, y, 0.3f);
    } else if (GfHas(b, "mortar")) {   // a short puff thrown up out of the tube
        for (int i = 0, n = GfCount(3); i < n; i++) {
            float sp = RandRange(300.0f, 650.0f);
            GfPuff(9, x + dx * 30.0f, y + dy * 30.0f, dx * sp + RandRange(-30.0f, 30.0f), dy * sp, RandRange(1.4f, 2.2f), RandRange(25.0f, 40.0f));
        }
        GfLight(x, y, 300.0f, 0.06f, cfg.flashLight * 0.4f, 1.0f, 0.78f, 0.5f);
    } else if (GfHas(b, "sniper")) {   // a thin line of shimmer along the shot, a wisp of smoke
        for (int k = 1; k <= 4; k++) AddHeat(x + dx * k * 55.0f, y + dy * k * 55.0f, 35.0f, 0.22f);
        GfPuff(9, x + dx * 20.0f, y + dy * 20.0f, dx * 60.0f, dy * 60.0f - 20.0f, RandRange(1.0f, 1.6f), 12.0f);
    } else if (GfHas(b, "minigun") || GfHas(b, "machinegun") || GfHas(b, "_mg")) {   // the barrel heats up as it keeps firing
        GfGun* g = nullptr;
        for (auto& o : g_gfGuns) if (hypotf(o.x - x, o.y - y) < 60.0f) { g = &o; break; }
        if (!g && g_gfGuns.size() < 32) { g_gfGuns.push_back({ x, y, 0.0f, g_fxNow, 0.0f }); g = &g_gfGuns.back(); }
        if (g) { g->heat = std::min(1.5f, g->heat + (GfHas(b, "minigun") ? 0.035f : 0.06f)); g->last = g_fxNow; g->x = x; g->y = y; }
    }
}

static void LaunchFx(const std::string& b, float x, float y, float dx, float dy, bool hasDir) {
    EndCharge(false, x, y, 400.0f);   // the silo's warm-up ends with the launch
    if (!cfg.gfMuzzles) return;
    if (!hasDir) { dx = 0.0f; dy = -1.0f; }
    float m = GfHas(b, "warhead") || GfHas(b, "shuttle") ? 2.0f : 1.0f;
    for (int i = 0, n = GfCount(3 * m); i < n; i++)   // hot exhaust bursting out behind it
        GfPuff(12, x - dx * 40.0f, y - dy * 40.0f, -dx * RandRange(200.0f, 500.0f) + RandRange(-80.0f, 80.0f), -dy * RandRange(200.0f, 500.0f), RandRange(1.5f, 2.5f), RandRange(30.0f, 55.0f) * m);
    for (int i = 0, n = GfCount(2 * m); i < n; i++)   // and dust kicked up around the launcher
        GfPuff(11, x + RandRange(-60.0f, 60.0f), y + 20.0f, RandRange(-300.0f, 300.0f), -RandRange(20.0f, 80.0f), RandRange(2.0f, 3.0f), RandRange(40.0f, 70.0f) * m);
    GfLight(x, y, 500.0f * m, 0.15f, cfg.flashLight * 0.6f * m, 1.0f, 0.7f, 0.4f);
}

static void BeamHitFx(float x, float y, int team) {
    if (!cfg.gfLasers) return;
    GfLaser* best = nullptr;
    float bestD = 1e30f;
    for (auto& L : g_gfLasers) {
        if (g_fxNow - L.t0 > 12.0f || (team >= 0 && L.team >= 0 && L.team != team)) continue;
        float d = hypotf(L.x - x, L.y - y);
        if (d < bestD) { bestD = d; best = &L; }
    }
    if (best) {   // the beam reaches the farthest of its recent hits (it passes through struts)
        float d = hypotf(x - best->x, y - best->y);
        if (g_fxNow - best->farT > 0.3f || d >= best->farD) { best->hx = x; best->hy = y; best->farD = d; best->farT = g_fxNow; }
        best->lastHit = g_fxNow; best->hit = true;
    }
    AddDecal(2, x, y, 18.0f, 0.6f);   // a molten mark where it cuts (glows while the beam is on it)
    if (Rand01() < 0.3f) SpawnParticle(2, x, y, RandRange(-400.0f, 400.0f), RandRange(-600.0f, 100.0f), RandRange(0.15f, 0.4f));
}

static void CollapseFx(const std::string& b, float x, float y) {
    if (!cfg.gfCollapse) return;
    for (int i = 0, n = GfCount(3); i < n; i++)   // dust falling out of the break
        GfPuff(11, x + RandRange(-40.0f, 40.0f), y + RandRange(-20.0f, 20.0f), RandRange(-30.0f, 30.0f), RandRange(20.0f, 90.0f), RandRange(1.8f, 3.0f), RandRange(30.0f, 60.0f));
    for (int i = 0, n = GfCount(5); i < n; i++)
        SpawnParticle(1, x + RandRange(-30.0f, 30.0f), y, RandRange(-120.0f, 120.0f), RandRange(-60.0f, 40.0f), RandRange(1.5f, 3.0f));
    if (GfHas(b, "bracing")) GfSpray(13, GfCount(6), x, y, 0.0f, 1.0f, 1.5f, 300.0f, 1.2f, 2.5f);
}

static void DeviceDeathFx(const std::string& dev, float x, float y) {
    if (!cfg.gfDevices) return;
    if (GfHas(dev, "reactor")) {           // a huge flash, then the core glows on for a while
        GfLight(x, y, 900.0f, 0.6f, cfg.flashLight * 0.6f, 0.85f, 0.92f, 1.0f, true);
        g_gfGlows.push_back({ x, y, 350.0f, g_fxNow, 8.0f, 0.6f, { 1.0f, 0.72f, 0.42f }, false });
        HeavyFx(x, y, 1.6f);
    } else if (GfHas(dev, "battery") || GfHas(dev, "capacitor") || GfHas(dev, "shield")) {   // electrical arcs and flicker
        GfSpray(15, GfCount(40), x, y, 0.0f, -1.0f, 3.14f, 1600.0f, 0.2f, 0.6f);
        g_gfGlows.push_back({ x, y, 300.0f, g_fxNow, 1.8f, 0.7f, { 0.55f, 0.75f, 1.0f }, true });
    } else if (GfHas(dev, "mine") || GfHas(dev, "derrick") || GfHas(dev, "drill")) {   // dust and rubble
        for (int i = 0, n = GfCount(6); i < n; i++)
            GfPuff(11, x + RandRange(-80.0f, 80.0f), y + RandRange(-30.0f, 30.0f), RandRange(-250.0f, 250.0f), -RandRange(50.0f, 250.0f), RandRange(2.0f, 3.2f), RandRange(50.0f, 90.0f));
        GfSpray(1, GfCount(14), x, y, 0.0f, -1.0f, 1.2f, 900.0f, 1.5f, 3.0f);
    }
    if (g_gfSmokers.size() >= 24) g_gfSmokers.erase(g_gfSmokers.begin());
    g_gfSmokers.push_back({ x, y, g_fxNow, 25.0f, 0.0f });   // the wreck smokes
}

static float GfWind();   // weather section
static float RainLevel();

// Once a frame, after the sprites' events: what the game did, and everything that carries on over time.
static void FunHurt(float x, float y);   // the milk miko
static void FunDeviceDied();
static void GameFxFrame() {
    std::vector<EngineFx> evs;
    EngineTakeFx(evs);   // always taken, so they never pile up
    float now = g_fxNow;
    g_gfLightBudget = std::min(6.0f, g_gfLightBudget + g_fxDt * 8.0f);
    if (GfOn()) {
        for (auto& e : evs)   // projectile hits first: they give the exact surface, its normal and what it is
            if (e.kind == EngineFx::Impact) {
                float l = hypotf(e.dx, e.dy);
                std::string what = GfLower(e.hit);
                g_gfHits.push_back({ e.x, e.y, l > 0.5f && l < 1.5f ? e.dx / l : 0.0f, l > 0.5f && l < 1.5f ? e.dy / l : -1.0f, now, MaterialOf(what), what, false, e.a });
            }
        for (auto& e : evs) {
            if (e.kind == EngineFx::Device) {
                g_gfDeaths.push_back({ GfLower(e.name), now });
                if (GfHas(GfLower(e.name), "water")) FunDeviceDied();
                continue;
            }
            if (e.kind != EngineFx::Effect) continue;
            std::string f = GfLower(e.name);
            size_t sl = f.find_last_of('/');
            std::string b = f.substr(sl == std::string::npos ? 0 : sl + 1);
            if (b.size() > 4 && b.compare(b.size() - 4, 4, ".lua") == 0) b.resize(b.size() - 4);
            if (GfHas(b, "hurt")) FunHurt(e.x, e.y);
            GfKind k = KindOf(b);
            if (k == kGfNone) continue;
            float dl = hypotf(e.dx, e.dy);
            bool dir = e.hasDir && dl > 0.5f && dl < 1.5f;
            float dx = dir ? e.dx / dl : 0.0f, dy = dir ? e.dy / dl : -1.0f;
            switch (k) {
            case kGfImpact: {
                GfMat mat = MaterialOf(b);   // the game names many impact effects after what was hit
                float nx = dx, ny = dy;
                const char* from = mat != kMatNone ? "the effect's name" : "unknown";
                GfHit* h = nullptr;   // the projectile hit this effect is for: at the same spot, just now
                float hd = 120.0f;
                for (auto& o : g_gfHits)
                    if (!o.used && now - o.t < 0.3f && hypotf(o.x - e.x, o.y - e.y) < hd) { hd = hypotf(o.x - e.x, o.y - e.y); h = &o; }
                bool fromHit = false;
                if (h) {
                    h->used = true;
                    if (mat == kMatNone && h->mat != kMatNone) { mat = h->mat; from = "what the projectile hit"; fromHit = true; }   // an effect named after a material wins
                    else if (mat == kMatNone && h->type == 2) { mat = kMatGround; from = "the game's hit type (ground)"; }   // hit type 2: always the ground in battle logs
                    nx = h->nx; ny = h->ny;
                }
                if (WaterOn() && e.y >= g_waterLevel - 25.0f) { mat = kMatWater; from = "the sea"; }
                float mag = MagnitudeOf(b);
                if (g_gfLogged < 15) {
                    g_gfLogged++;
                    SessionLog("game fx: %s at (%.0f, %.0f): %s (from %s%s), normal (%.2f, %.2f), size %.2f", b.c_str(), e.x, e.y, kMatNames[mat], from,
                               fromHit && !h->what.empty() ? (" \"" + h->what + "\"").c_str() : "", nx, ny, mag);
                }
                ImpactFx(mat, mag, e.x, e.y, nx, ny);
                if (mag >= 1.5f) HeavyFx(e.x, e.y, mag);
                if (GfHas(b, "explo") || GfHas(b, "destroy")) {
                    if (g_gfBooms.size() >= 64) g_gfBooms.erase(g_gfBooms.begin());
                    g_gfBooms.push_back({ e.x, e.y, now, b, false });
                }
                GfStat(b, Fmt("%s hit, size %.2f%s", kMatNames[mat], mag, mag >= 1.5f ? ", heavy" : "").c_str());
                break;
            }
            case kGfMuzzle: MuzzleFx(b, e.x, e.y, dx, dy, dir, e.team); GfStat(b, "muzzle"); break;
            case kGfLaunch: LaunchFx(b, e.x, e.y, dx, dy, dir); GfStat(b, "launch"); break;
            case kGfBeamHit: BeamHitFx(e.x, e.y, e.team); GfStat(b, "beam cutting"); break;
            case kGfCollapse: CollapseFx(b, e.x, e.y); GfStat(b, "collapse dust"); break;
            case kGfCharge: ChargeFx(b, e.x, e.y); GfStat(b, "charging up"); break;
            default: break;
            }
        }
        // devices the game destroyed meet their explosion (whichever the game reports first)
        for (auto& d : g_gfDeaths) {
            GfBoom* best = nullptr;
            float bestT = 0.6f;
            for (auto& bm : g_gfBooms) {
                if (bm.used || fabsf(bm.t - d.t) > bestT) continue;
                bool named = GfHas(bm.file, d.dev.c_str());
                float dt = fabsf(bm.t - d.t) - (named ? 1.0f : 0.0f);   // prefer the device's own explosion
                if (dt < bestT) { bestT = dt; best = &bm; }
            }
            if (best) {
                best->used = true;
                DeviceDeathFx(d.dev, best->x, best->y);
                GfStat("device " + d.dev, Fmt("destroyed at %s", best->file.c_str()).c_str());
                d.t = -1000.0f;
            }
        }
        g_gfDeaths.erase(std::remove_if(g_gfDeaths.begin(), g_gfDeaths.end(), [&](const GfDeath& d) {
            if (d.t > -999.0f && now - d.t > 0.6f) { GfStat("device " + d.dev, "destroyed, no explosion seen"); return true; }
            return d.t <= -999.0f;
        }), g_gfDeaths.end());
    }
    g_gfHits.erase(std::remove_if(g_gfHits.begin(), g_gfHits.end(), [&](const GfHit& h) { return now - h.t > 0.5f; }), g_gfHits.end());
    g_gfBooms.erase(std::remove_if(g_gfBooms.begin(), g_gfBooms.end(), [&](const GfBoom& b) { return now - b.t > 1.0f; }), g_gfBooms.end());

    // heavy weapons: the shockwave ring arriving across the terrain
    for (auto& l : g_gfLater)
        if (now >= l.t) {
            g_shocks.push_back({ l.x, l.y, 1400.0f * std::min(l.mag, 2.5f), now, 1.1f + 0.3f * l.mag, 0.9f * cfg.shockStrength });
            GfShake(l.x, l.y, l.mag * 0.6f);
            l.t = 1e30f;
        }
    g_gfLater.erase(std::remove_if(g_gfLater.begin(), g_gfLater.end(), [](const GfLater& l) { return l.t > 1e29f; }), g_gfLater.end());
    // rapid-fire guns: thin smoke curling off a hot barrel
    for (auto& g : g_gfGuns) {
        g.heat = std::max(0.0f, g.heat - g_fxDt * 0.12f);
        if (g.heat > 0.3f) {
            g.acc += g_fxDt * (g.heat - 0.3f) * 6.0f * cfg.gfAmount;
            AddHeat(g.x, g.y, 40.0f, g_fxDt * g.heat * 0.6f);
        }
        while (g.acc >= 1.0f) {
            g.acc -= 1.0f;
            GfPuff(9, g.x + RandRange(-8.0f, 8.0f), g.y - 6.0f, RandRange(-12.0f, 12.0f), -RandRange(40.0f, 90.0f), RandRange(1.6f, 2.6f), RandRange(14.0f, 26.0f) * (0.8f + g.heat));
        }
    }
    g_gfGuns.erase(std::remove_if(g_gfGuns.begin(), g_gfGuns.end(), [&](const GfGun& g) { return g.heat <= 0.0f && now - g.last > 5.0f; }), g_gfGuns.end());
    // lasers: the emitter's glow builds while it fires, light along the beam, a molten glow where it cuts
    for (auto& L : g_gfLasers) {
        bool beam = L.hit && now - L.lastHit < 0.35f;
        if (!(now - L.t0 < 0.5f || beam) || !cfg.gfLasers) continue;
        float on = std::min(1.0f, (now - L.t0) / 0.3f);
        g_frameLights.push_back({ L.x, L.y, 260.0f, 0.9f * on, { 0.8f, 0.88f, 1.0f } });
        if (!beam) continue;
        for (int i = 1; i <= 3; i++) {
            float t = i / 4.0f;
            g_frameLights.push_back({ L.x + (L.hx - L.x) * t, L.y + (L.hy - L.y) * t, 320.0f, 0.45f, { 0.75f, 0.85f, 1.0f } });
        }
        g_frameLights.push_back({ L.hx, L.hy, 180.0f, 0.8f, { 1.0f, 0.62f, 0.28f } });
        AddHeat(L.hx, L.hy, 70.0f, g_fxDt * 1.2f);
        if (Rand01() < g_fxDt * 8.0f * cfg.gfAmount) SpawnParticle(14, L.hx, L.hy, RandRange(-90.0f, 90.0f), RandRange(-160.0f, 10.0f), RandRange(0.6f, 1.4f));   // dripping slag
        for (int k = 0, n = GfCount(g_fxDt * 30.0f); k < n; k++)   // sparks spraying off where it cuts
            SpawnParticle(2, L.hx, L.hy, RandRange(-700.0f, 700.0f), RandRange(-900.0f, 200.0f), RandRange(0.12f, 0.35f));
    }
    g_gfLasers.erase(std::remove_if(g_gfLasers.begin(), g_gfLasers.end(), [&](const GfLaser& L) {
        return now - std::max(L.t0, L.lastHit) > 1.5f; }), g_gfLasers.end());
    // charging: a laser's emitter glows up over its 3.4 s charge, then idles ready; a silo vents and glows
    // through its warm-up until the launch
    for (auto& c : g_gfCharges) {
        float p = std::min(1.0f, (now - c.t0) / c.dur);
        if (c.laser) {
            if (!cfg.gfLasers) continue;
            float inten = p < 1.0f ? 0.1f + 0.8f * p * p : 0.3f + 0.08f * sinf(now * 5.0f);
            g_frameLights.push_back({ c.x, c.y, 120.0f + 160.0f * p, inten, { 0.7f, 0.85f, 1.0f } });
            for (int k = 0, n = p < 1.0f ? 2 + (int)(p * 5.0f) : 2; k < n; k++)   // a visible core: blue-white, flickering
                SpawnParticle(15, c.x + RandRange(-10.0f, 10.0f), c.y + RandRange(-10.0f, 10.0f), 0.0f, 0.0f, 0.06f);
            if (p < 1.0f && Rand01() < g_fxDt * (1.0f + 8.0f * p) * cfg.gfAmount)
                GfSpray(15, 1, c.x + RandRange(-30.0f, 30.0f), c.y + RandRange(-30.0f, 30.0f), 0.0f, -1.0f, 3.14f, 500.0f, 0.08f, 0.2f);
        } else {
            if (!cfg.gfMuzzles) continue;
            g_frameLights.push_back({ c.x, c.y, 150.0f + 150.0f * p, 0.15f + 0.6f * p, { 1.0f, 0.72f, 0.42f } });
            AddHeat(c.x, c.y, 60.0f + 60.0f * p, g_fxDt * (0.3f + p));
            if (Rand01() < g_fxDt * (1.0f + 5.0f * p) * cfg.gfAmount)
                GfPuff(8, c.x + RandRange(-40.0f, 40.0f), c.y - 10.0f, RandRange(-40.0f, 40.0f), -RandRange(80.0f, 200.0f), RandRange(1.2f, 2.0f), RandRange(25.0f, 45.0f));
        }
    }
    g_gfCharges.erase(std::remove_if(g_gfCharges.begin(), g_gfCharges.end(), [&](const GfCharge& c) {
        return now - c.t0 > c.dur + (c.laser ? 30.0f : 3.0f); }), g_gfCharges.end());
    // projectiles in flight, from the game's own effect list (their trails): a shell's hot wake shimmers,
    // a fast round (cannon 6000, sniper 10000 units/s in the game's weapons) leaves a faint ring once
    if (GfOn() && cfg.gfInFlight && EngineTracking()) {
        std::vector<EngineTrail> trails;
        EngineTrails(trails);
        float vh = WorldHeight();
        for (auto& kv : g_gfRung) kv.second = false;   // false: not seen this frame
        for (auto& t : trails) {
            std::string n = GfLower(t.name);
            float sp = hypotf(t.vx, t.vy);
            bool shell = GfHas(n, "cannon") || GfHas(n, "mortar") || GfHas(n, "sniper") || GfHas(n, "deckgun") || GfHas(n, "howitzer");
            if (shell && sp > 1.0f && g_gfTrail.size() < 8) g_gfTrail.push_back({ t.x - t.vx * 0.03f, t.y - t.vy * 0.03f });
            auto it = g_gfRung.find(t.key);
            if (it == g_gfRung.end()) {
                bool ring = sp > 4500.0f && g_gfRings >= 1.0f;
                if (ring) {
                    g_gfRings -= 1.0f;
                    g_shocks.push_back({ t.x, t.y, std::max(vh * 0.035f, 120.0f), now, 0.3f, 0.3f * cfg.shockStrength });
                }
                if (ring || sp > 1.0f) g_gfRung[t.key] = true;   // decided once it is moving
            } else it->second = true;
        }
        for (auto it = g_gfRung.begin(); it != g_gfRung.end();) it = it->second ? std::next(it) : g_gfRung.erase(it);
    }
    // dead devices: lingering glows, flickering electrics, smoking wrecks
    for (auto& g : g_gfGlows) {
        float a = 1.0f - (now - g.t0) / g.life;
        if (a <= 0.0f) continue;
        float k = g.flicker ? (Rand01() < 0.45f ? 1.0f : 0.15f) : 1.0f;
        g_frameLights.push_back({ g.x, g.y, g.r, g.inten * a * a * k, { g.col[0], g.col[1], g.col[2] } });
        if (g.flicker && Rand01() < g_fxDt * 14.0f) GfSpray(15, 2, g.x + RandRange(-60.0f, 60.0f), g.y + RandRange(-60.0f, 60.0f), 0.0f, -1.0f, 3.14f, 900.0f, 0.1f, 0.3f);
    }
    g_gfGlows.erase(std::remove_if(g_gfGlows.begin(), g_gfGlows.end(), [&](const GfGlow& g) { return now - g.t0 > g.life; }), g_gfGlows.end());
    for (auto& s : g_gfSmokers) {
        float age = now - s.t0, a = std::max(0.0f, 1.0f - age / s.life);
        s.acc += g_fxDt * 2.5f * powf(a, 1.5f) * cfg.gfAmount;
        while (s.acc >= 1.0f) {
            s.acc -= 1.0f;
            GfPuff(9, s.x + RandRange(-25.0f, 25.0f), s.y - 20.0f, RandRange(-20.0f, 20.0f), -RandRange(60.0f, 140.0f), RandRange(3.0f, 5.0f), RandRange(26.0f, 45.0f));
        }
        if (age < 6.0f && Rand01() < g_fxDt * 3.0f) SpawnParticle(0, s.x + RandRange(-30.0f, 30.0f), s.y - 10.0f, RandRange(-30.0f, 30.0f), -RandRange(40.0f, 120.0f), RandRange(1.0f, 2.2f));
    }
    g_gfSmokers.erase(std::remove_if(g_gfSmokers.begin(), g_gfSmokers.end(), [&](const GfSmoker& s) { return now - s.t0 > s.life; }), g_gfSmokers.end());
    g_decals.erase(std::remove_if(g_decals.begin(), g_decals.end(), [&](const Decal& d) { return now - d.t0 > d.life; }), g_decals.end());

    // weather meets the battle: rain hissing to steam on fires, snow melting around them
    g_gfMeltN = 0;
    if (cfg.gameFx && cfg.gfWeather && cfg.impactFx) {
        std::vector<const Instance*> fires;
        for (auto& in : g_inst)
            if (in.cls == kEvFire && in.lastSeen == g_fxFrame) fires.push_back(&in);
        float rain = RainLevel();
        static float steamAcc;
        if (rain > 0.05f && !fires.empty()) {
            steamAcc += g_fxDt * std::min((float)fires.size(), 16.0f) * 0.8f * std::min(rain, 1.5f) * cfg.gfAmount;
            while (steamAcc >= 1.0f) {
                steamAcc -= 1.0f;
                const Instance& f = *fires[(size_t)(Rand01() * fires.size()) % fires.size()];
                GfPuff(8, f.x + RandRange(-f.r, f.r) * 0.5f, f.y - f.r * 0.6f, RandRange(-10.0f, 10.0f), -RandRange(40.0f, 90.0f), RandRange(1.2f, 2.2f), f.r * RandRange(0.5f, 0.9f));
            }
        } else steamAcc = 0;
        for (auto* f : fires) {   // the biggest separate fires melt the snow around them
            if (g_gfMeltN == 6) break;
            bool close = false;
            for (int i = 0; i < g_gfMeltN && !close; i++) close = hypotf(g_gfMelt[i * 4] - f->x, g_gfMelt[i * 4 + 1] - f->y) < g_gfMelt[i * 4 + 2];
            if (close) continue;
            float* m = &g_gfMelt[g_gfMeltN++ * 4];
            m[0] = f->x; m[1] = f->y; m[2] = std::max(f->r * 2.5f, 80.0f); m[3] = 0.03f;
        }
        for (auto& d : g_decals) {   // and so does a fresh blast
            if (g_gfMeltN == 8) break;
            float heat = 1.0f - (now - d.hotT) / 5.0f;
            if (heat <= 0.0f || d.kind == 2) continue;
            float* m = &g_gfMelt[g_gfMeltN++ * 4];
            m[0] = d.x; m[1] = d.y; m[2] = d.r * 1.5f; m[3] = 0.05f * heat;
        }
    }
}

// ================================================================ the milk miko ([Fun] MilkMiko - a joke)
// The DLC water cannon becomes an over-the-top chibi shrine maiden with a giant milk-bottle cannon (art
// drawn in code: fun/make_art.py), and its water becomes simulated milk: a liquid of particles that runs
// along what it lands on, drawn as one glossy surface (kFsMilk). Sakura petals, manga sound words, speed
// lines and sparkles, faces that change when her milk lands or she is hit, ElevenLabs voice lines
// (fun/make_voice.py), and Mount Fuji in the 3D sky while she is on the map. Assets: <game>\FortsGFX_fun\.
// Purely visual and client-side, like everything else here.
static void WorldToScreen(float x, float y, float& sx, float& sy);
struct FunImage { int w = 0, h = 0; std::vector<uint8_t> px; };   // RGBA, rows top-down
static std::map<std::string, FunImage> g_funImg;                  // decoded on the loader thread
static std::map<std::string, std::vector<uint8_t>> g_funWav;      // whole WAV files (PlaySound plays them from memory)
static std::atomic<int> g_funState{ 0 };                           // 0 not started, 1 loading, 2 ready, 3 nothing found
static std::map<std::string, GLuint> g_funTex;                    // the overlay sprites, uploaded
static std::unordered_map<GLuint, GLuint> g_mikoSwap[4];          // game texture -> replacement: girl normal / happy / hurt, cannon
static std::unordered_set<GLuint> g_mikoPending;                   // game textures seen, replacement not made yet
GLuint g_texRestore;                                               // the game's texture, rebound after a swapped draw
static std::vector<int> g_mikoMoods;                               // per girl draw (in draw order), from the last frame
static int g_mikoDrawIdx;

static DWORD WINAPI FunLoader(void*) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    std::wstring dir = g_gameDir + L"FortsGFX_fun\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    int images = 0, sounds = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            std::string key = Narrow(name.c_str());
            for (auto& ch : key) ch = (char)tolower((unsigned char)ch);
            std::wstring path = dir + name;
            if (key.size() > 4 && key.compare(key.size() - 4, 4, ".png") == 0 && wic) {
                IWICBitmapDecoder* dec = nullptr;
                IWICBitmapFrameDecode* fr = nullptr;
                IWICFormatConverter* cv = nullptr;
                if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
                    SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
                    SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                    UINT w = 0, hh = 0;
                    cv->GetSize(&w, &hh);
                    FunImage im;
                    im.w = (int)w; im.h = (int)hh;
                    im.px.resize((size_t)w * hh * 4);
                    if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)im.px.size(), im.px.data()))) {
                        g_funImg[key.substr(0, key.size() - 4)] = std::move(im);
                        images++;
                    }
                }
                if (cv) cv->Release();
                if (fr) fr->Release();
                if (dec) dec->Release();
            } else if (key.size() > 4 && key.compare(key.size() - 4, 4, ".wav") == 0) {
                FILE* f = _wfopen(path.c_str(), L"rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long n = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    std::vector<uint8_t> b(n > 0 ? (size_t)n : 0);
                    if (n > 0 && fread(b.data(), 1, b.size(), f) == b.size()) { g_funWav[key.substr(0, key.size() - 4)] = std::move(b); sounds++; }
                    fclose(f);
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (wic) wic->Release();
    if (SUCCEEDED(hr)) CoUninitialize();
    bool ok = g_funImg.count("miko_normal") && g_funImg.count("milk_cannon");
    SessionLog("fun: milk miko assets from %s: %d images, %d voice lines%s", Narrow(dir.c_str()).c_str(), images, sounds,
               ok ? "" : " - the girl or her cannon is missing: off");
    g_funState = ok ? 2 : 3;
    return 0;
}

static GLuint FunUpload(const uint8_t* px, int w, int h) {
    static auto genMip = (void (WINAPI*)(GLenum))RealProc("glGenerateMipmap");
    GLuint t = 0;
    G.GenTextures(1, &t);
    G.BindTexture(GL_TEXTURE_2D, t);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (genMip) genMip(GL_TEXTURE_2D);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, genMip ? 0x2703 /* LINEAR_MIPMAP_LINEAR */ : GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* CLAMP_TO_EDGE */);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    return t;
}
// The art letterboxed into the game texture's shape, so the quad the game draws doesn't stretch it: the girl
// stands on the bottom edge, the cannon sits in the middle.
static GLuint FunReplacement(GLuint orig, const FunImage& im, bool bottom) {
    int ow = 0, oh = 0;
    if (!TexSize(orig, ow, oh) || ow <= 0 || oh <= 0) { ow = im.w; oh = im.h; }
    float oa = (float)ow / oh, ia = (float)im.w / im.h;
    int cw = im.w, ch = im.h;
    if (oa > ia) cw = (int)lroundf(im.h * oa); else ch = (int)lroundf(im.w / oa);
    std::vector<uint8_t> c((size_t)cw * ch * 4, 0);
    int x0 = (cw - im.w) / 2, y0 = bottom ? ch - im.h : (ch - im.h) / 2;
    for (int y = 0; y < im.h; y++) memcpy(&c[((size_t)(y0 + y) * cw + x0) * 4], &im.px[(size_t)y * im.w * 4], (size_t)im.w * 4);
    return FunUpload(c.data(), cw, ch);
}
// Draw time (world shader): the replacement for this water-cannon draw, 0 while it isn't made yet.
static GLuint MikoSwapFor(GLuint tex, uint8_t ev) {
    int variant = 3;
    if (ev == kEvMiko) {
        int i = g_mikoDrawIdx++;
        variant = i < (int)g_mikoMoods.size() ? g_mikoMoods[i] : 0;
    }
    auto it = g_mikoSwap[variant].find(tex);
    if (it != g_mikoSwap[variant].end()) return it->second;
    g_mikoPending.insert(tex);
    return 0;
}
void GfxRestoreTexture() {
    if (!g_texRestore) return;
    G.BindTexture(GL_TEXTURE_2D, g_texRestore);
    g_texRestore = 0;
}

struct Miko { float x, y, r, lastSeen, born; int mood; float moodUntil, lastFire, lastVoice, nextSparkle; };
static std::vector<Miko> g_mikos;
static std::vector<FrameEvent> g_funMikoEv, g_funMilkEv;            // this frame's sprites
static std::unordered_map<uint32_t, EngineTrail> g_waterShots;   // the water cannon's shots in flight last frame
static std::vector<std::vector<float>> g_streams;               // this frame's jets: x, y, half width, distance (world)
struct FunPop { std::string tex; float x, y, t0, life, size, rot; };   // a manga word / speed lines / sparkle at a world spot
static std::vector<FunPop> g_funPops;
static float g_milkLastSpawn = -100, g_funDeathT = -100, g_lastArrive = -100, g_lastVoiceT = -100, g_lastSplashWord = -100;
static float g_milkAcc, g_fujiWorldX;
static bool g_fujiOn;

static void FunEvent(const FrameEvent& e) {
    if (e.cls == kEvMiko) { if (g_funMikoEv.size() < 32) g_funMikoEv.push_back(e); }
    else if (e.cls == kEvMilk) { if (g_funMilkEv.size() < 256) g_funMilkEv.push_back(e); }
}
static void FunVoice(const char* key, float minGap = 1.2f) {
    if (!cfg.mikoVoice || g_fxNow - g_lastVoiceT < minGap) return;
    auto it = g_funWav.find(std::string("voice_") + key);
    if (it == g_funWav.end()) return;
    g_lastVoiceT = g_fxNow;
    PlaySoundW((LPCWSTR)it->second.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
static void FunPopAt(const char* tex, float x, float y, float life, float size) {
    if (g_funPops.size() < 64) g_funPops.push_back({ tex, x, y, g_fxNow, life, size, RandRange(-0.18f, 0.18f) });
}
static void SpawnMilk(float x, float y, float vx, float vy, float life, float size) {
    if (!g_parts[0].fbo || g_spawns.size() > 4096) return;
    Spawn s;
    s.slot = g_partNext;
    g_partNext = (g_partNext + 1) % kMaxParticles;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = RandRange(-1, 1); s.pos[3] = life;
    s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = size; s.vel[3] = 16.0f;
    g_spawns.push_back(s);
    g_milkLastSpawn = g_fxNow;
}
static void Petals(float x, float y, float r, int n) {
    for (int i = 0; i < n; i++)
        SpawnParticle(17, x + RandRange(-r, r), y + RandRange(-r, r) * 0.5f, RandRange(-1.0f, 1.0f) * r * 3.0f, -RandRange(0.5f, 2.0f) * r, RandRange(3.5f, 6.0f));
}
static Miko* NearestMiko(float x, float y, float within) {
    Miko* best = nullptr;
    float bd = within;
    for (auto& m : g_mikos) { float d = hypotf(m.x - x, m.y - y); if (d < bd) { bd = d; best = &m; } }
    return best;
}
// From the game's events (GameFxFrame): she was hit, or a water cannon was destroyed.
static void FunHurt(float x, float y) {
    if (!cfg.funMiko) return;
    Miko* m = NearestMiko(x, y, 400.0f);
    if (!m) return;
    m->mood = 2; m->moodUntil = g_fxNow + 1.5f;
    FunPopAt("word_ita", m->x, m->y - m->r * 1.2f, 1.0f, 0.8f);
    FunVoice(Rand01() < 0.5f ? "hurt1" : "hurt2", 3.0f);
}

static void FunFrame() {
    bool miko = cfg.funMiko;
    if (!miko) { g_mikos.clear(); g_funMikoEv.clear(); g_funPops.clear(); g_mikoMoods.clear(); g_fujiOn = false; g_mikoDrawIdx = 0; }
    if (!miko && !cfg.waterStream) { g_funMilkEv.clear(); g_waterShots.clear(); g_streams.clear(); return; }
    if (miko && g_funState == 0) { g_funState = 1; CloseHandle(CreateThread(nullptr, 0, FunLoader, nullptr, 0, nullptr)); }
    if (miko && g_funState == 2) {   // upload on the render thread, inside the post chain (its state guard restores the bindings)
        if (g_funTex.empty())
            for (auto& kv : g_funImg)
                if (kv.first.compare(0, 5, "word_") == 0 || kv.first == "speedlines" || kv.first == "sparkle")
                    g_funTex[kv.first] = FunUpload(kv.second.px.data(), kv.second.w, kv.second.h);
        for (GLuint t : g_mikoPending) {
            static const char* girl[3] = { "miko_normal", "miko_happy", "miko_hurt" };
            for (int v = 0; v < 3; v++) {
                auto it = g_funImg.find(girl[v]);
                if (it == g_funImg.end()) it = g_funImg.find("miko_normal");
                if (!g_mikoSwap[v].count(t)) g_mikoSwap[v][t] = FunReplacement(t, it->second, true);
            }
            if (!g_mikoSwap[3].count(t)) g_mikoSwap[3][t] = FunReplacement(t, g_funImg["milk_cannon"], false);
            int w = 0, h = 0;
            TexSize(t, w, h);
            SessionLog("fun: replacement made for %s (%dx%d)", TexShort(t).c_str(), w, h);
        }
        g_mikoPending.clear();
    }
    float now = g_fxNow, vh = std::max(WorldHeight(), 1.0f);
    // the girls, in this frame's draw order (their faces next frame follow the same order)
    std::vector<bool> taken(g_mikos.size(), false);
    g_mikoMoods.clear();
    for (auto& e : g_funMikoEv) {
        int bi = -1;
        float bd = std::max(e.r * 1.5f, 60.0f);
        for (size_t i = 0; i < g_mikos.size(); i++) {
            if (taken[i]) continue;
            float d = hypotf(g_mikos[i].x - e.x, g_mikos[i].y - e.y);
            if (d < bd) { bd = d; bi = (int)i; }
        }
        if (bi < 0) {   // she arrives
            g_mikos.push_back({ e.x, e.y, e.r, now, now, 0, 0, -100, -100, now + 0.5f });
            taken.push_back(true);
            bi = (int)g_mikos.size() - 1;
            if (now - g_lastArrive > 20.0f) {
                g_lastArrive = now;
                FunVoice("arrive", 0.0f);
                FunPopAt("word_kira", e.x, e.y - e.r * 1.4f, 1.3f, 0.9f);
            }
            if (!g_fujiOn) { g_fujiOn = true; g_fujiWorldX = (g_world[0] + g_world[1]) * 0.5f; }
        }
        taken[bi] = true;
        Miko& m = g_mikos[bi];
        m.x = e.x; m.y = e.y; m.r = e.r; m.lastSeen = now;
        if (m.mood && now > m.moodUntil) m.mood = 0;
        g_mikoMoods.push_back(m.mood);
        if (now > m.nextSparkle) {   // kira-kira
            m.nextSparkle = now + RandRange(0.5f, 1.1f);
            FunPopAt("sparkle", m.x + RandRange(-1.0f, 1.0f) * m.r, m.y - m.r * RandRange(0.4f, 1.3f), 0.7f, RandRange(0.6f, 1.2f));
        }
    }
    for (size_t i = 0; i < g_mikos.size();) {   // gone: destroyed just now -> goodbye
        Miko& m = g_mikos[i];
        if (now - m.lastSeen > 1.0f) {
            if (now - g_funDeathT < 2.5f) {
                FunVoice("bye", 0.0f);
                FunPopAt("word_sayonara", m.x, m.y - m.r, 1.8f, 1.0f);
                Petals(m.x, m.y - m.r * 0.5f, m.r, 40);
                g_funDeathT = -100;
            }
            g_mikos.erase(g_mikos.begin() + i);
        } else i++;
    }
    // the water cannon's jet: its shots in flight are the game's own (each carries water_trail.lua, so the effect
    // list has them with their speed), joined nozzle -> front into one continuous stream. A new shot is her firing,
    // a shot gone (in view) has splashed.
    g_streams.clear();
    std::vector<EngineTrail> trails, shots;
    if (EngineTracking()) EngineTrails(trails);
    for (auto& t : trails) if (GfHas(GfLower(t.name), "water_trail")) shots.push_back(t);
    std::unordered_map<uint32_t, EngineTrail> live;
    for (auto& s : shots) {
        live[s.key] = s;
        if (!g_waterShots.count(s.key) && miko)
            if (Miko* m = NearestMiko(s.x, s.y, vh * 0.8f)) {   // she fires
                if (now - m->lastFire > 1.2f) {
                    Petals(m->x, m->y - m->r * 0.6f, m->r, 18);
                    FunPopAt(Rand01() < 0.5f ? "word_pyu" : "word_don", m->x + RandRange(-0.3f, 0.3f) * m->r, m->y - m->r * 1.3f, 1.0f, 1.0f);
                    FunPopAt("speedlines", m->x, m->y - m->r * 0.5f, 0.4f, 1.0f);
                    static const char* lines[3] = { "fire1", "fire2", "fire3" };
                    if (now - m->lastVoice > 4.0f) { m->lastVoice = now; FunVoice(lines[(int)(Rand01() * 2.999f)]); }
                }
                m->lastFire = now;
            }
    }
    for (auto& kv : g_waterShots) {   // splashed: a burst of droplets (the liquid particles), "basha!", a happy miko
        if (live.count(kv.first)) continue;
        const EngineTrail& j = kv.second;
        bool inView = j.x > std::min(g_world[0], g_world[1]) && j.x < std::max(g_world[0], g_world[1]) &&
                      j.y > std::min(g_world[2], g_world[3]) && j.y < std::max(g_world[2], g_world[3]);
        if (!inView) continue;
        for (int k = 0; k < 12; k++) {
            float a = RandRange(-3.0f, -0.14f), sp = RandRange(150.0f, 600.0f);
            SpawnMilk(j.x, j.y, cosf(a) * sp + j.vx * 0.1f, sinf(a) * sp, RandRange(1.0f, 2.2f), RandRange(8.0f, 18.0f));
        }
        if (miko && now - g_lastSplashWord > 0.6f) { g_lastSplashWord = now; FunPopAt("word_basha", j.x, j.y - vh * 0.03f, 0.9f, 0.8f); }
        if (miko)
            if (Miko* m = NearestMiko(j.x, j.y, vh)) {
                m->mood = 1; m->moodUntil = now + 1.5f;
                if (Rand01() < 0.3f && now - m->lastVoice > 6.0f) { m->lastVoice = now; FunVoice(Rand01() < 0.5f ? "hit1" : "hit2"); }
                if (Rand01() < 0.2f) FunPopAt("word_yatta", m->x, m->y - m->r * 1.5f, 1.0f, 0.7f);
            }
    }
    g_waterShots.swap(live);
    // chains: from each newest shot, the next older one just ahead of it along its flight, and so on
    std::sort(shots.begin(), shots.end(), [](const EngineTrail& a, const EngineTrail& b) { return a.key > b.key; });
    std::vector<bool> usedShot(shots.size(), false);
    float link = std::max(vh * 0.25f, 700.0f);
    for (size_t i = 0; i < shots.size(); i++) {
        if (usedShot[i]) continue;
        std::vector<size_t> chain{ i };
        usedShot[i] = true;
        for (size_t cur = i;;) {
            const EngineTrail& c = shots[cur];
            float sp = hypotf(c.vx, c.vy);
            size_t best = SIZE_MAX;
            float bd = link;
            for (size_t j = 0; j < shots.size(); j++) {
                if (usedShot[j] || shots[j].key >= c.key) continue;
                float dx = shots[j].x - c.x, dy = shots[j].y - c.y, d = hypotf(dx, dy);
                if (sp > 1.0f && dx * c.vx + dy * c.vy < -0.3f * d * sp) continue;   // behind it: another jet
                if (d < bd) { bd = d; best = j; }
            }
            if (best == SIZE_MAX || chain.size() >= 63) break;
            usedShot[best] = true;
            chain.push_back(best);
            cur = best;
        }
        if (chain.size() < 2) continue;
        std::vector<float> pts;   // smoothed (Catmull-Rom, 4 steps a segment): x, y, half width, distance from the nozzle
        float s = 0, lx = shots[chain[0]].x, ly = shots[chain[0]].y;
        auto P = [&](int k) { k = std::max(0, std::min((int)chain.size() - 1, k)); return std::pair<float, float>(shots[chain[k]].x, shots[chain[k]].y); };
        for (int k = 0; k + 1 < (int)chain.size(); k++)
            for (int st = 0; st < 4; st++) {
                float t = st / 4.0f, t2 = t * t, t3 = t2 * t;
                auto p0 = P(k - 1), p1 = P(k), p2 = P(k + 1), p3 = P(k + 2);
                float x = 0.5f * (2 * p1.first + (-p0.first + p2.first) * t + (2 * p0.first - 5 * p1.first + 4 * p2.first - p3.first) * t2 + (-p0.first + 3 * p1.first - 3 * p2.first + p3.first) * t3);
                float y = 0.5f * (2 * p1.second + (-p0.second + p2.second) * t + (2 * p0.second - 5 * p1.second + 4 * p2.second - p3.second) * t2 + (-p0.second + 3 * p1.second - 3 * p2.second + p3.second) * t3);
                s += hypotf(x - lx, y - ly); lx = x; ly = y;
                pts.insert(pts.end(), { x, y, std::min(9.0f + s * 0.05f, 45.0f), s });
            }
        auto last = P((int)chain.size() - 1);
        s += hypotf(last.first - lx, last.second - ly);
        pts.insert(pts.end(), { last.first, last.second, std::min(9.0f + s * 0.05f, 45.0f), s });
        g_streams.push_back(std::move(pts));
        g_milkLastSpawn = now;   // keeps the liquid passes on
    }
    g_funMikoEv.clear();
    g_funMilkEv.clear();
    g_funPops.erase(std::remove_if(g_funPops.begin(), g_funPops.end(), [&](const FunPop& p) { return now - p.t0 > p.life; }), g_funPops.end());
    g_mikoDrawIdx = 0;
    if (g_mikos.empty() && now - g_lastArrive > 120.0f) g_fujiOn = false;   // left the map for a while
}

// After the final pass, before the HUD: the manga words, speed lines and sparkles, crisp and unlit.
static void DrawFunOverlay() {
    if (!cfg.funMiko || g_funPops.empty() || !g_pSprite) return;
    G.BindFramebuffer(GL_FRAMEBUFFER, g_outFbo);
    G.Viewport(0, 0, g_w, g_h);
    G.Enable(GL_BLEND);
    G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    Use(g_pSprite);
    U2f(g_pSprite, "uFull", (float)g_w, (float)g_h);
    U1i(g_pSprite, "uTexS", 0);
    for (auto& p : g_funPops) {
        auto it = g_funTex.find(p.tex);
        auto im = g_funImg.find(p.tex);
        if (it == g_funTex.end() || im == g_funImg.end()) continue;
        float t = std::min(1.0f, (g_fxNow - p.t0) / p.life), sx, sy;
        WorldToScreen(p.x, p.y, sx, sy);
        float aspect = (float)im->second.w / im->second.h, hpx, alpha = 1.0f - std::max(0.0f, (t - 0.75f) / 0.25f), rot = p.rot;
        if (p.tex == "speedlines") { hpx = g_h * 0.32f * (0.8f + 0.4f * t); alpha = (1.0f - t) * 0.55f; rot = t * 0.3f; }
        else if (p.tex == "sparkle") { hpx = g_h * 0.022f * p.size * sinf(3.14159f * t); alpha = 1.0f; rot = t * 1.5f; }
        else {   // a word pops in, overshoots, settles, rises a little and fades
            float s = t < 0.12f ? 0.6f + t / 0.12f * 0.55f : t < 0.2f ? 1.15f - (t - 0.12f) / 0.08f * 0.15f : 1.0f;
            hpx = g_h * 0.06f * p.size * s;
            sy += t * g_h * 0.03f;
        }
        U4f(g_pSprite, "uRect", sx, sy, hpx * aspect * 0.5f, hpx * 0.5f);
        U1f(g_pSprite, "uRot", rot);
        U4f(g_pSprite, "uTint", 1, p.tex == "sparkle" ? 0.92f : 1.0f, 1, std::max(alpha, 0.0f));
        Bind(0, it->second);
        G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    G.Disable(GL_BLEND);
}

static void FunDeviceDied() { g_funDeathT = g_fxNow; }
void GfxReplayFuji(float worldX) { g_fujiOn = true; g_fujiWorldX = worldX; g_lastArrive = 1e30f; }   // replay tool: show Mount Fuji

// Turns this frame's measured draws into effects. Runs once per frame at the start of the post chain.
// Is this explosion-named sprite a real blast? With the engine watch: when the game has just spawned an
// impact or explosion effect where it is. Until the watch has confirmed that the game's positions line
// up with its sprites (and without it), yes - the draw-order rule above has already taken out fire frames.
static bool BlastConfirmed(const Instance& in) {
    float radius = std::max(in.maxR * 2.5f, 100.0f);
    EngineMatch m = EngineEffectAt(in.x, in.y, radius);
    static int detailed;
    if (detailed < 25 && (m.state >= 0 || m.dist >= 0)) {
        detailed++;
        std::string seen = m.dist < 0 ? "no game impact or explosion in the last 3 s"
                                      : Fmt("nearest game effect %s, %.0f away (within %.0f counts), %.2f s ago", m.file, m.dist, radius, m.age);
        SessionLog("blast check: %s at (%.0f, %.0f), size %.0f: %s -> %s", ImageName(in.tex).c_str(), in.x, in.y, in.maxR, seen.c_str(),
                   m.state == 1 ? "blast" : m.state == 0 ? "no blast (a fire's own frame)" : "blast (positions not confirmed yet)");
    }
    if (m.state == 1) {
        FxStat("blast", in.tex, Fmt("where the game spawned %s", m.file).c_str());
        std::string f = GfLower(m.file);
        size_t sl = f.find_last_of('/');
        g_nextBlastMag = MagnitudeOf(f.substr(sl == std::string::npos ? 0 : sl + 1));   // sizes its shockwave
    }
    if (m.state == 0) FxStat("explosion-named", in.tex, "no game impact or explosion there: a fire's own frame, no blast");
    return m.state != 0;
}

static void ProcessEvents() {
    double now = FxSeconds();
    g_fxDt = std::min(std::max((float)(now - g_fxNow), 0.0f), 0.05f);
    g_fxNow = (float)now;
    g_fxFrame++;
    g_spawnBudget = std::min(30.0f, g_spawnBudget + g_fxDt * 25.0f);
    g_puffBudget = std::min(80.0f, g_puffBudget + g_fxDt * 320.0f * cfg.smokeAmount);
    g_frameLights.clear();
    {
        static LARGE_INTEGER f, last;
        LARGE_INTEGER nq;
        QueryPerformanceCounter(&nq);
        if (!f.QuadPart) { QueryPerformanceFrequency(&f); last = nq; }
        g_realDt = std::min(std::max((float)((nq.QuadPart - last.QuadPart) / (double)f.QuadPart), 1e-3f), 0.1f);
        last = nq;
    }
    g_tracersPrev.swap(g_tracersNow);
    g_tracersNow.clear();
    g_gfTrail.clear();
    g_gfRings = std::min(3.0f, g_gfRings + g_fxDt * 3.0f);
    g_fireEmbers = std::min(8.0f, g_fireEmbers + g_fxDt * 10.0f * cfg.embers);
    g_flashLights = std::min(2.0f, g_flashLights + g_fxDt * 3.0f);
    if (!cfg.impactFx) { g_frameEvents.clear(); g_inst.clear(); g_heat.clear(); }
    // Real blast or a fire's own explosion-like frame? With the engine watch (engine.cpp) the game says:
    // a blast sprite appears where it has just spawned an impact or explosion effect - decided per sprite
    // when it first shows (below). Without it: an explosion-named sprite drawn in the middle of a fire's
    // sprites (a fire sprite within two draws before and after it) is taken as one of the fire's frames.
    // That guess fails in big battles, where nearly every blast is drawn next to some fire.
    bool engineDecides = EngineReady();
    for (auto& e : g_frameEvents) {
        if (e.cls != kEvExplosion || !e.seq || engineDecides) continue;
        auto fireAt = [](unsigned seq) { return std::binary_search(g_fireDraws.begin(), g_fireDraws.end(), seq); };
        bool inFire = fireAt(e.seq - 1) || fireAt(e.seq + 1) || (e.seq > 2 && fireAt(e.seq - 2)) || fireAt(e.seq + 2);
        if (inFire) { e.cls = kEvFire; e.fireFrame = true; }
    }
    g_fireDraws.clear();
    for (auto& e : g_frameEvents) {
        if (e.fireFrame) FxStat("explosion-named", e.tex, "the fire's own frame (drawn among its flames): burns, no blast");
        else switch (e.cls) {
        case kEvExplosion:
            if (!engineDecides) FxStat("explosion", e.tex, "blast: shockwave, flash light, fireball, shake");
            break;
        case kEvFire: FxStat("fire", e.tex, "flames: embers, light, shimmer"); break;
        case kEvSteam: case kEvSmoke:
            FxStat(e.cls == kEvSteam ? "steam" : "smoke", e.tex, !cfg.simSmoke ? "kept (simulated smoke off)" : e.puff == 12 ? "hidden: simulated hot gas"
                   : e.puff == 11 ? "hidden: simulated dust" : "hidden: simulated puffs");
            break;
        case kEvFlash: FxStat("flash", e.tex, "muzzle flash light"); break;
        case kEvTracer: FxStat("projectile", e.tex, "glowing tracer"); break;
        case kEvPlume: FxStat("engine", e.tex, "plume light, heat, smoke trail"); break;
        case kEvSpark: FxStat("spark", e.tex, "spark burst"); break;
        default: break;
        }
        if (e.cls == kEvMiko || e.cls == kEvMikoGun || e.cls == kEvMilk) { FunEvent(e); continue; }
        if (e.cls == kEvTracer) { TracerFrame(e); continue; }
        if (e.cls == kEvPlume) {   // rocket / missile engine: light, heat and a smoke trail, every frame
            float r = std::max(e.r, WorldHeight() * 0.006f);
            if (cfg.plumeLight > 0 && g_frameLights.size() < 8)
                g_frameLights.push_back({ e.x, e.y, r * 3.0f, cfg.plumeLight * e.a, { 1.0f, 0.62f, 0.3f } });
            AddHeat(e.x, e.y, r * 1.5f, g_fxDt * 2.0f);
            bool gameTrail = false;   // the game's own trail (Steam, Buster's uranium) is already being replaced
            for (auto& o : g_frameEvents)
                if (o.part && hypotf(o.x - e.x, o.y - e.y) < r * 8.0f) { gameTrail = true; break; }
            if (cfg.simSmoke && !gameTrail && g_puffBudget >= 1.0f && Rand01() < g_fxDt * 45.0f * cfg.smokeAmount) {
                g_puffBudget -= 1.0f;
                SpawnPuff(8, e.x + RandRange(-r, r) * 0.4f, e.y + RandRange(-r, r) * 0.4f, RandRange(-0.2f, 0.2f) * r, RandRange(-0.2f, 0.2f) * r,
                          RandRange(1.8f, 3.0f), r * RandRange(1.0f, 1.6f));
            }
            if (Rand01() < g_fxDt * 10.0f)
                SpawnParticle(2, e.x, e.y, RandRange(-1, 1) * r * 6.0f, RandRange(-1, 1) * r * 6.0f, RandRange(0.15f, 0.35f));
            continue;
        }
        if (e.part && e.cls != kEvFire) {   // one game smoke / steam particle: on average one simulated puff over its life, where it is
            if (cfg.simSmoke && g_puffBudget >= 1.0f && Rand01() < g_fxDt / 1.2f * cfg.smokeAmount) {
                g_puffBudget -= 1.0f;
                SpawnPuff(e.puff ? e.puff : e.cls == kEvSteam ? 8 : 9, e.x, e.y, RandRange(-0.15f, 0.15f) * e.r, -e.r * RandRange(0.1f, 0.4f),
                          RandRange(2.0f, 3.2f), e.r * RandRange(0.9f, 1.2f));
            }
            continue;
        }
        Instance* best = nullptr;
        for (auto& in : g_inst) {
            if (in.cls != e.cls) continue;
            float d = hypotf(in.x - e.x, in.y - e.y);
            if (d < std::max(in.r, e.r) * 0.8f + 4.0f) { best = &in; break; }
        }
        if (best) {
            best->x = (best->x + e.x) * 0.5f; best->y = (best->y + e.y) * 0.5f;
            best->r = e.r; best->maxR = std::max(best->maxR, e.r); best->maxA = std::max(best->maxA, e.a);
            best->lastSeen = g_fxFrame;
        } else if (g_inst.size() < 256) {
            g_inst.push_back({ e.cls, e.x, e.y, e.r, e.r, e.a, 0.0f, g_fxFrame, g_fxFrame, false, e.puff, e.tex });
        }
    }
    g_frameEvents.clear();
    for (auto& in : g_inst) {
        // fire after a couple of frames so the size reflects the sprite's real extent
        if (!in.fired && (g_fxFrame - in.firstSeen >= 2 || in.lastSeen != g_fxFrame)) {
            in.fired = true;
            if (in.cls == kEvExplosion && !BlastConfirmed(in)) in.quiet = true;
            else if (in.cls == kEvBridgeLightning) LightningStrike(in.x, true);
            else OnSpawn(in.cls, in.x, in.y, in.maxR, in.maxA, in.puff);
        }
        if ((in.cls == kEvSteam || in.cls == kEvSmoke) && in.lastSeen == g_fxFrame && cfg.simSmoke) {   // long-lived emitters keep puffing
            in.emberAcc += g_fxDt * 1.2f * cfg.smokeAmount;
            while (in.emberAcc >= 1.0f && g_puffBudget >= 1.0f) {
                in.emberAcc -= 1.0f;
                g_puffBudget -= 1.0f;
                SpawnPuff(in.puff == 12 ? 9 : in.puff ? in.puff : in.cls == kEvSteam ? 8 : 9, in.x + RandRange(-in.r, in.r) * 0.3f, in.y, RandRange(-0.1f, 0.1f) * in.r,
                          -in.r * RandRange(0.3f, 0.7f), RandRange(2.2f, 3.4f), in.r * RandRange(0.8f, 1.1f));
            }
        }
        if (in.cls == kEvFlash && in.lastSeen == g_fxFrame)   // a gun that keeps firing keeps heating up
            AddHeat(in.x, in.y, std::max(in.r, WorldHeight() * 0.015f) * 1.5f, g_fxDt * 0.55f * cfg.gunHeat);
        // Burning things shed a few small embers. A burning strut is dozens of small flame sprites,
        // so all fires share one budget (~10 x Embers per second) instead of each shedding its own.
        if (in.cls == kEvFire && in.lastSeen == g_fxFrame && cfg.embers > 0) {
            in.emberAcc += g_fxDt * cfg.embers * (0.6f + in.r * 0.01f);
            while (in.emberAcc >= 1.0f) {
                in.emberAcc -= 1.0f;
                if (g_fireEmbers < 1.0f) { in.emberAcc = 0; break; }
                g_fireEmbers -= 1.0f;
                SpawnParticle(5, in.x + RandRange(-in.r, in.r) * 0.5f, in.y - in.r * 0.3f, RandRange(-20, 20), RandRange(-70, -25), RandRange(0.7f, 1.8f));
            }
        }
    }
    g_inst.erase(std::remove_if(g_inst.begin(), g_inst.end(), [](const Instance& i) { return g_fxFrame - i.lastSeen > 8; }), g_inst.end());
    SmokeChunksFrame();
    GameFxFrame();
    ShakeOffFrame();
    FunFrame();
    RunLabQueue();
    for (auto& l : g_labLights)   // lab lights show as a glowing core
        if (l.on) SpawnParticle(0, l.x, l.y, 0, 0, 0.05f);
    for (auto& s : g_heat) s.heat -= g_fxDt * 0.2f;   // ~5 s to cool from full heat
    g_heat.erase(std::remove_if(g_heat.begin(), g_heat.end(), [](const HeatSpot& s) { return s.heat <= 0; }), g_heat.end());
    auto alive = [](float t0, float life) { return g_fxNow - t0 < life; };
    g_shocks.erase(std::remove_if(g_shocks.begin(), g_shocks.end(), [&](const Shock& s) { return !alive(s.t0, s.life); }), g_shocks.end());
    g_flashes.erase(std::remove_if(g_flashes.begin(), g_flashes.end(), [&](const FlashLight& f) { return !alive(f.t0, f.life); }), g_flashes.end());
    g_fireballs.erase(std::remove_if(g_fireballs.begin(), g_fireballs.end(), [&](const Fireball& f) { return !alive(f.t0, f.life); }), g_fireballs.end());
    if (g_shocks.size() > 16) g_shocks.erase(g_shocks.begin(), g_shocks.end() - 16);
    if (g_flashes.size() > 14) g_flashes.erase(g_flashes.begin(), g_flashes.end() - 14);
    if (g_fireballs.size() > 24) g_fireballs.erase(g_fireballs.begin(), g_fireballs.end() - 24);
    g_trauma = std::max(0.0f, g_trauma - g_fxDt * 1.4f);
    FxStatFlush(false);
}

// world -> GL screen pixels (y up)
static void WorldToScreen(float x, float y, float& sx, float& sy) {
    sx = (x - g_world[0]) / (g_world[1] - g_world[0]) * g_w;
    sy = (y - g_world[2]) / (g_world[3] - g_world[2]) * g_h;
}
static float WorldToPx() { return g_w / std::max(fabsf(g_world[1] - g_world[0]), 1e-3f); }

// ---- terrain geometry, read from the drawing itself
// The terrain is drawn from display lists; the hooks keep the vertices compiled into each list, and when a
// list is drawn in the playfield its triangles are taken to world space with the modelview it is drawn
// with. That gives the real terrain shape for the whole map (also off screen) - snow, sand, ash and rain
// use it as a world-space mask, so they never sit inside terrain whatever its texture or overlays do.
struct TerrainList { uint64_t hash = 0; std::vector<float> tris; };   // world xy, 3 vertices per triangle
static std::unordered_map<GLuint, TerrainList> g_terrain;
static unsigned g_terrainVersion, g_terrainEpoch;   // epoch: a new map
static std::atomic<bool> g_terrainReset{ false };
static bool TerrainBounds(float b[4]) {   // min x, max x, min y, max y
    b[0] = b[2] = 1e30f; b[1] = b[3] = -1e30f;
    for (auto& kv : g_terrain)
        for (size_t i = 0; i + 1 < kv.second.tris.size(); i += 2) {
            b[0] = std::min(b[0], kv.second.tris[i]); b[1] = std::max(b[1], kv.second.tris[i]);
            b[2] = std::min(b[2], kv.second.tris[i + 1]); b[3] = std::max(b[3], kv.second.tris[i + 1]);
        }
    return b[0] < b[1] && b[2] < b[3];
}
static void NoteTerrainList(GLuint id) {
    if (g_terrainReset.exchange(false)) { g_terrain.clear(); g_terrainVersion++; g_terrainEpoch++; }
    const std::vector<GeoBatch>* geo = ListGeometry(id);
    if (!geo) return;
    float mv[16];
    R.GetFloatv(GL_MODELVIEW_MATRIX, mv);
    uint64_t h = 1469598103934665603ull ^ id;
    for (float f : { mv[0], mv[1], mv[4], mv[5], mv[12], mv[13] }) { uint32_t u; memcpy(&u, &f, 4); h = (h ^ u) * 1099511628211ull; }
    h ^= geo->size() * 0x9E3779B97F4A7C15ull;
    TerrainList& tl = g_terrain[id];
    if (tl.hash == h) return;
    tl.hash = h;
    tl.tris.clear();
    auto put = [&](const std::vector<float>& xy, size_t i) {
        float x = xy[i * 2], y = xy[i * 2 + 1];
        tl.tris.push_back(mv[0] * x + mv[4] * y + mv[12]);
        tl.tris.push_back(mv[1] * x + mv[5] * y + mv[13]);
    };
    for (auto& b : *geo) {
        size_t nv = b.xy.size() / 2;
        auto tri = [&](size_t a, size_t c, size_t d) { put(b.xy, a); put(b.xy, c); put(b.xy, d); };
        switch (b.mode) {
        case 0x0004: for (size_t i = 0; i + 2 < nv; i += 3) tri(i, i + 1, i + 2); break;                          // TRIANGLES
        case 0x0005: for (size_t i = 0; i + 2 < nv; i++) tri(i, i + 1, i + 2); break;                             // TRIANGLE_STRIP
        case 0x0006: case 0x0009: for (size_t i = 1; i + 1 < nv; i++) tri(0, i, i + 1); break;                    // FAN, POLYGON
        case 0x0007: for (size_t i = 0; i + 3 < nv; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); } break; // QUADS
        case 0x0008: for (size_t i = 0; i + 3 < nv; i += 2) { tri(i, i + 1, i + 3); tri(i, i + 3, i + 2); } break; // QUAD_STRIP
        default: break;
        }
    }
    g_terrainVersion++;
}

// ---- snow physics layer (shaders: kFsSnowStep / kVsSnowDeposit / kFsSnowCopy)
// A world-anchored layer around the battle (anchored where it first snows, ~7 views wide); every frame the
// visible part is simulated into a scratch target and copied back, the rest keeps its snow.
static Target g_snowM, g_snowS;          // the layer, the scratch
static Target g_terrainM;                // world terrain mask aligned with the layer
static unsigned g_terrainRastered = ~0u;
static GLuint g_terrainVerts;            // vertices for rasterising it (RGBA32F, 4096 wide)
static float g_snowRegion[4];            // world rect of the layer (same orientation as g_world)
static bool g_snowLive, g_snowClear;
static double g_snowLastT = -1000;
void GfxLabClearSnow() { g_snowClear = true; }
static float SnowLevel();
static int PrecipKind();

static void ClearSnowLayer() {
    static const GLfloat zero[4] = { 0, 0, 0, 0 };
    G.BindFramebuffer(GL_FRAMEBUFFER, g_snowM.fbo);
    G.ClearBufferfv(GL_COLOR, 0, zero);
}

static void RunSnow() {
    float snow = SnowLevel();
    if (snow > 0) g_snowLastT = g_fxNow;
    g_snowLive = cfg.impactFx && (snow > 0 || g_fxNow - g_snowLastT < 150.0 || g_fxNow - g_snowBrushT < 150.0);
    if (!g_snowLive || !g_pSnowStep || !g_pSnowDep || !g_pSnowCopy || !g_parts[0].fbo) return;
    float vw = g_world[1] - g_world[0], vh = g_world[3] - g_world[2];
    // anchor (or re-anchor when the view has left the layer entirely)
    bool inside = g_snowM.fbo &&
        std::max(g_world[0], g_world[1]) > std::min(g_snowRegion[0], g_snowRegion[1]) && std::min(g_world[0], g_world[1]) < std::max(g_snowRegion[0], g_snowRegion[1]) &&
        std::max(g_world[2], g_world[3]) > std::min(g_snowRegion[2], g_snowRegion[3]) && std::min(g_world[2], g_world[3]) < std::max(g_snowRegion[2], g_snowRegion[3]);
    float tb[4];
    bool haveTerrain = TerrainBounds(tb);
    static unsigned anchoredEpoch = ~0u;
    static bool anchoredOnTerrain;
    bool terrainRegion = haveTerrain && (!anchoredOnTerrain || anchoredEpoch != g_terrainEpoch);   // terrain known / a new map
    if (!inside || terrainRegion) {
        float cellW = fabsf(vw) / std::max(g_w / 4, 64), cellH = fabsf(vh) / std::max(g_h / 4, 64);   // ~4 px cells now
        float L, Rr, B, T;   // in g_world orientation (left, right, bottom, top)
        if (haveTerrain) {   // the whole map: the terrain plus room above it for forts and flakes
            float padX = (tb[1] - tb[0]) * 0.08f + fabsf(vw) * 0.5f, up = fabsf(vh) * 1.5f + (tb[3] - tb[2]) * 0.3f;
            L = tb[0] - padX; Rr = tb[1] + padX;
            bool yDown = g_world[2] > g_world[3];   // Forts: y grows downward
            B = yDown ? tb[3] + fabsf(vh) * 0.1f : tb[2] - fabsf(vh) * 0.1f;
            T = yDown ? tb[2] - up : tb[3] + up;
            if (g_world[0] > g_world[1]) std::swap(L, Rr);
        } else {
            float cx = (g_world[0] + g_world[1]) * 0.5f, cy = (g_world[2] + g_world[3]) * 0.5f;
            L = cx - vw * 3.5f; Rr = cx + vw * 3.5f; B = cy - vh * 3.5f; T = cy + vh * 3.5f;
        }
        int mw = (int)std::min(8192.0f, std::max(64.0f, fabsf(Rr - L) / std::max(cellW, 1e-3f)));
        int mh = (int)std::min(8192.0f, std::max(64.0f, fabsf(T - B) / std::max(cellH, 1e-3f)));
        if (g_snowM.w != mw || g_snowM.h != mh) {
            FreeTarget(g_snowM); MakeTarget(g_snowM, mw, mh, GL_R16F);
            FreeTarget(g_terrainM); MakeTarget(g_terrainM, mw, mh, GL_RGBA8);
        }
        g_snowRegion[0] = L; g_snowRegion[1] = Rr; g_snowRegion[2] = B; g_snowRegion[3] = T;
        ClearSnowLayer();
        g_terrainRastered = ~0u;
        anchoredEpoch = g_terrainEpoch;
        anchoredOnTerrain = haveTerrain;
        SessionLog("gfx: snow layer %dx%d anchored %s", mw, mh, haveTerrain ? "over the whole terrain" : "around the view");
    }
    if (g_terrainRastered != g_terrainVersion && g_pTerrain) {   // draw the terrain mask from the triangles
        std::vector<float> v;
        for (auto& kv : g_terrain) for (size_t i = 0; i + 1 < kv.second.tris.size(); i += 2) { v.push_back(kv.second.tris[i]); v.push_back(kv.second.tris[i + 1]); v.push_back(0); v.push_back(0); }
        size_t nv = v.size() / 4;
        static const GLfloat zero[4] = { 0, 0, 0, 0 };
        G.BindFramebuffer(GL_FRAMEBUFFER, g_terrainM.fbo);
        G.ClearBufferfv(GL_COLOR, 0, zero);
        if (nv >= 3) {
            int rows = (int)((nv + 4095) / 4096);
            v.resize((size_t)rows * 4096 * 4, 0.0f);
            if (!g_terrainVerts) G.GenTextures(1, &g_terrainVerts);
            G.BindTexture(GL_TEXTURE_2D, g_terrainVerts);
            G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4096, rows, 0, GL_RGBA, GL_FLOAT, v.data());
            G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2600 /* GL_NEAREST */);
            G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, 0x2600 /* GL_NEAREST */);
            Use(g_pTerrain);
            U1i(g_pTerrain, "uVerts", 0);
            Bind(0, g_terrainVerts);
            G.Uniform1i(G.GetUniformLocation(g_pTerrain, "uWidth"), 4096);
            U4f(g_pTerrain, "uRegion", g_snowRegion[0], g_snowRegion[1], g_snowRegion[2], g_snowRegion[3]);
            G.Viewport(0, 0, g_terrainM.w, g_terrainM.h);
            G.DrawArrays(GL_TRIANGLES, 0, (GLsizei)(nv / 3 * 3));
        }
        g_terrainRastered = g_terrainVersion;
        SessionLog("gfx: terrain mask from %zu triangles (%zu display lists)", nv / 3, g_terrain.size());
    }
    if (g_snowClear) { ClearSnowLayer(); g_snowClear = false; }
    // the visible rect in layer cells (+2 cells margin)
    float MW = (float)g_snowM.w, MH = (float)g_snowM.h;
    auto cellX = [&](float wx) { return (wx - g_snowRegion[0]) / (g_snowRegion[1] - g_snowRegion[0]) * MW; };
    auto cellY = [&](float wy) { return (wy - g_snowRegion[2]) / (g_snowRegion[3] - g_snowRegion[2]) * MH; };
    int x0 = (int)floorf(std::min(cellX(g_world[0]), cellX(g_world[1]))) - 2, x1 = (int)ceilf(std::max(cellX(g_world[0]), cellX(g_world[1]))) + 2;
    int y0 = (int)floorf(std::min(cellY(g_world[2]), cellY(g_world[3]))) - 2, y1 = (int)ceilf(std::max(cellY(g_world[2]), cellY(g_world[3]))) + 2;
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, g_snowM.w); y1 = std::min(y1, g_snowM.h);
    int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 4 || rh <= 4) return;
    if (g_snowS.w < rw || g_snowS.h < rh) {
        FreeTarget(g_snowS);
        MakeTarget(g_snowS, (rw + 255) / 256 * 256, (rh + 255) / 256 * 256, GL_R16F);
    }
    // 1. simulate the visible rect: layer -> scratch
    Use(g_pSnowStep);
    U1i(g_pSnowStep, "uM", 0); U1i(g_pSnowStep, "uSolid", 1); U1i(g_pSnowStep, "uTerrain", 2);
    Bind(0, g_snowM.tex); Bind(1, g_solidTex); Bind(2, g_terrainM.tex);
    U4f(g_pSnowStep, "uRegion", g_snowRegion[0], g_snowRegion[1], g_snowRegion[2], g_snowRegion[3]);
    U2f(g_pSnowStep, "uMSize", MW, MH);
    U2f(g_pSnowStep, "uOrg", (float)x0, (float)y0);
    U4f(g_pSnowStep, "uWorld", g_world[0], g_world[1], g_world[2], g_world[3]);
    float maxDepth = PrecipKind() == 3 ? std::min(cfg.snowDepth, 3.0f) : cfg.snowDepth;   // ash: a thin layer
    U4f(g_pSnowStep, "uP", cfg.snowSlide * 0.5f, snow > 0 ? 0.0f : g_fxDt / 90.0f, maxDepth, (g_fxFrame & 1) ? 1.0f : -1.0f);
    U4f(g_pSnowStep, "uWaterS", WaterOn() ? 1.0f : 0.0f, g_waterLevel + 15.0f, 0, 0);   // a little below the line: the waves
    G.Uniform4fv(G.GetUniformLocation(g_pSnowStep, "uHeatW"), 8, g_gfMelt);
    G.Uniform1i(G.GetUniformLocation(g_pSnowStep, "uHeatN"), g_gfMeltN);
    G.BindFramebuffer(GL_FRAMEBUFFER, g_snowS.fbo);
    G.Viewport(0, 0, rw, rh);
    G.DrawArrays(GL_TRIANGLES, 0, 3);
    // 2. flakes that settled add their snow (into the scratch)
    G.Enable(GL_BLEND);
    G.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    Use(g_pSnowDep);
    U1i(g_pSnowDep, "uPos", 0); U1i(g_pSnowDep, "uVel", 1);
    Bind(0, g_parts[g_partCur].tex); Bind(1, g_parts[g_partCur].tex2);
    G.Uniform1i(G.GetUniformLocation(g_pSnowDep, "uWidth"), kPW);
    U4f(g_pSnowDep, "uRegion", g_snowRegion[0], g_snowRegion[1], g_snowRegion[2], g_snowRegion[3]);
    U2f(g_pSnowDep, "uMSize", MW, MH);
    U2f(g_pSnowDep, "uOrg", (float)x0, (float)y0);
    U2f(g_pSnowDep, "uSSize", (float)rw, (float)rh);
    U1f(g_pSnowDep, "uRate", 0.12f * cfg.snowBuild * g_fxDt);   // each settled flake adds ~0.3 of a cell
    G.DrawArrays(0x0000 /* GL_POINTS */, 0, kMaxParticles);
    G.Disable(GL_BLEND);
    // 3. copy the rect back into the layer
    Use(g_pSnowCopy);
    U1i(g_pSnowCopy, "uS", 0);
    U2f(g_pSnowCopy, "uOrg", (float)x0, (float)y0);
    Bind(0, g_snowS.tex);
    G.BindFramebuffer(GL_FRAMEBUFFER, g_snowM.fbo);
    G.Viewport(x0, y0, rw, rh);
    G.DrawArrays(GL_TRIANGLES, 0, 3);
    G.Viewport(0, 0, g_w, g_h);
}
// Particles + fireballs, drawn into the world's scene and light-source targets before lighting.
static void RunWorldFx() {
    if (!g_fxFbo || !g_pPartUpdate) return;
    UploadSpawns();
    // simulate
    Target& cur = g_parts[g_partCur];
    Target& nxt = g_parts[g_partCur ^ 1];
    Use(g_pPartUpdate);
    U1i(g_pPartUpdate, "uPos", 0); U1i(g_pPartUpdate, "uVel", 1); U1i(g_pPartUpdate, "uSolid", 2); U1i(g_pPartUpdate, "uSnowL", 3);
    U1f(g_pPartUpdate, "uSnowOn", g_snowLive && g_snowM.fbo ? 1.0f : 0.0f);
    U4f(g_pPartUpdate, "uSnowR", g_snowRegion[0], g_snowRegion[1], g_snowRegion[2], g_snowRegion[3]);
    U4f(g_pPartUpdate, "uPrecip", (float)PrecipKind(), 0, 0, 0);
    U4f(g_pPartUpdate, "uWaterP", WaterOn() ? 1.0f : 0.0f, g_waterLevel, 0, 0);
    U1i(g_pPartUpdate, "uTerrain", 4);
    Bind(4, g_terrainM.fbo ? g_terrainM.tex : g_solidTex);
    U1f(g_pPartUpdate, "uTerrainOn", g_terrainM.fbo && g_terrainRastered != ~0u ? 1.0f : 0.0f);
    Bind(3, g_snowM.fbo ? g_snowM.tex : g_solidTex);
    U4f(g_pPartUpdate, "uWorld", g_world[0], g_world[1], g_world[2], g_world[3]);
    U4f(g_pPartUpdate, "uSim", g_fxDt, g_fxNow, GfWind(), cfg.gravity);
    {   // blast wind: the newest blasts - a front sweeping out from 0.3 of the radius to all of it, weakening as it
        // spreads, then the air rushing back in for 0.7 s
        float bl[8 * 4] = {}, bw[8 * 4] = {};
        int bn = 0;
        g_gfBlasts.erase(std::remove_if(g_gfBlasts.begin(), g_gfBlasts.end(), [](const GfBlast& b) { return g_fxNow - b.t0 > b.t1 + 0.7f; }), g_gfBlasts.end());
        for (auto it = g_gfBlasts.rbegin(); it != g_gfBlasts.rend() && bn < 8; ++it, bn++) {
            float a = g_fxNow - it->t0, k = a / it->t1;
            bl[bn * 4] = it->x; bl[bn * 4 + 1] = it->y;
            bl[bn * 4 + 2] = k < 1.0f ? it->r * (0.3f + 0.7f * k) : -1.2f * it->r;
            bl[bn * 4 + 3] = k < 1.0f ? it->push * (1.0f - 0.6f * k) : -0.08f * it->push * sinf(3.14159f * (a - it->t1) / 0.7f);
            bw[bn * 4] = 0.18f * it->r;
        }
        G.Uniform4fv(G.GetUniformLocation(g_pPartUpdate, "uBlast"), 8, bl);
        G.Uniform4fv(G.GetUniformLocation(g_pPartUpdate, "uBlastW"), 8, bw);
        G.Uniform1i(G.GetUniformLocation(g_pPartUpdate, "uBlastN"), bn);
    }
    U1i(g_pPartUpdate, "uScene", 5); U1i(g_pPartUpdate, "uEmis", 6);
    U1f(g_pPartUpdate, "uHitCol", cfg.gameFx && cfg.gfHitColour ? 1.0f : 0.0f);
    Bind(5, g_sceneTex); Bind(6, g_emisTex);
    Bind(0, cur.tex); Bind(1, cur.tex2); Bind(2, g_solidTex);
    Draw(nxt.fbo, kPW, kPH);
    Bind(5, 0); Bind(6, 0);   // (both are drawn into next)
    G.ActiveTexture(GL_TEXTURE0);
    g_partCur ^= 1;

    G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
    G.Viewport(0, 0, g_w, g_h);
    G.Enable(GL_BLEND);
    G.ColorMaski(2, 0, 0, 0, 0);   // only the particles write smoke density
    // scorch marks and craters: into the scene where there is a surface; still-hot ones glow
    if (!g_decals.empty() && g_pDecal) {
        float dc[96 * 4] = {}, dk[96 * 4] = {};
        int n = 0;
        float px = WorldToPx();
        for (auto& d : g_decals) {
            float sx, sy, rp = d.r * px;
            WorldToScreen(d.x, d.y, sx, sy);
            if (rp < 1.0f || sx + rp < 0 || sy + rp < 0 || sx - rp > g_w || sy - rp > g_h) continue;
            float heat = std::max(0.0f, 1.0f - (g_fxNow - d.hotT) / (d.kind == 2 ? 1.2f : 1.8f));   // cools within a couple of seconds
            dc[n * 4] = sx; dc[n * 4 + 1] = sy; dc[n * 4 + 2] = rp; dc[n * 4 + 3] = (g_fxNow - d.t0) / d.life;
            dk[n * 4] = d.kind; dk[n * 4 + 1] = d.strength * cfg.scorchStrength; dk[n * 4 + 2] = d.seed; dk[n * 4 + 3] = heat;
            if (++n == 96) break;
        }
        if (n) {
            if (g_decalEmis.w != g_w / 2 || g_decalEmis.h != g_h / 2) MakeTarget(g_decalEmis, g_w / 2, g_h / 2, GL_RGBA16F);
            G.Disable(GL_BLEND);   // what glows here (the game's explosion and fire sprites): marks go under it
            Use(g_pCopy);
            U1i(g_pCopy, "uSrc", 0); U1i(g_pCopy, "uAlpha", 0);
            Bind(0, g_emisTex);
            Draw(g_decalEmis.fbo, g_decalEmis.w, g_decalEmis.h);
            G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
            G.Viewport(0, 0, g_w, g_h);
            G.Enable(GL_BLEND);
            Use(g_pDecal);
            U2f(g_pDecal, "uFull", (float)g_w, (float)g_h);
            U1i(g_pDecal, "uSolid", 0); U1i(g_pDecal, "uEmis", 1);
            Bind(0, g_solidTex); Bind(1, g_decalEmis.tex);
            G.Uniform4fv(G.GetUniformLocation(g_pDecal, "uDc"), n, dc);
            G.Uniform4fv(G.GetUniformLocation(g_pDecal, "uDk"), n, dk);
            G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            G.BlendFuncSeparatei(1, GL_ONE, GL_ONE, GL_ZERO, GL_ONE);   // the glow of hot ones adds light
            G.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
        }
    }
    // fireballs: premultiplied into scene, light into the light-source target
    if (!g_fireballs.empty() && g_pFireball) {
        Use(g_pFireball);
        U2f(g_pFireball, "uFull", (float)g_w, (float)g_h);
        U1f(g_pFireball, "uTime", g_fxNow);
        U4f(g_pFireball, "uFbLook", cfg.fireballBright, cfg.fxSaturation, 0.35f, 0);
        float fb[24 * 4] = {}, seed[24 * 4] = {};
        int n = 0;
        for (auto& f : g_fireballs) {
            float sx, sy;
            WorldToScreen(f.x, f.y, sx, sy);
            fb[n * 4] = sx; fb[n * 4 + 1] = sy; fb[n * 4 + 2] = f.r * WorldToPx(); fb[n * 4 + 3] = (g_fxNow - f.t0) / f.life;
            seed[n * 4] = f.seed;
            if (++n == 24) break;
        }
        G.Uniform4fv(G.GetUniformLocation(g_pFireball, "uFb"), n, fb);
        G.Uniform4fv(G.GetUniformLocation(g_pFireball, "uFbSeed"), n, seed);
        G.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        G.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    }
    // particles: glowing ones add light, solid ones occlude
    Use(g_pPartDraw);
    U1i(g_pPartDraw, "uPos", 0); U1i(g_pPartDraw, "uVel", 1);
    U4f(g_pPartDraw, "uWorld", g_world[0], g_world[1], g_world[2], g_world[3]);
    U2f(g_pPartDraw, "uFull", (float)g_w, (float)g_h);
    G.Uniform1i(G.GetUniformLocation(g_pPartDraw, "uWidth"), kPW);
    U1f(g_pPartDraw, "uScale", WorldToPx());
    U1f(g_pPartDraw, "uTime", g_fxNow);
    U4f(g_pPartDraw, "uLook", cfg.emberBright, cfg.sparkBright, cfg.fxSaturation, 0);
    { int k = PrecipKind();
      if (k == 2) U4f(g_pPartDraw, "uPrecipCol", 0.80f, 0.64f, 0.43f, 1); else if (k == 3) U4f(g_pPartDraw, "uPrecipCol", 0.36f, 0.34f, 0.33f, 1); else U4f(g_pPartDraw, "uPrecipCol", 0.94f, 0.96f, 1.0f, 1); }
    Bind(0, g_parts[g_partCur].tex); Bind(1, g_parts[g_partCur].tex2);
    G.Uniform1i(G.GetUniformLocation(g_pPartDraw, "uPass"), 1);
    G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    // smoke, steam, dust and debris are near things: their coverage marks the playfield (emissive alpha), so
    // over the sky they are not softened and hazed like the backdrop, and they hide the glow behind them
    G.BlendFuncSeparatei(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    G.ColorMaski(2, 0, 1, 1, 0);   // g: lit-smoke density, b: puff coverage
    G.BlendFuncSeparatei(2, GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    G.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, kMaxParticles);
    G.ColorMaski(2, 0, 0, 0, 0);
    G.Uniform1i(G.GetUniformLocation(g_pPartDraw, "uPass"), 0);
    G.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    G.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, kMaxParticles);
    if ((cfg.funMiko || cfg.waterStream) && g_pMilk && g_fxNow - g_milkLastSpawn < 6.0f) {   // the water cannon's stream (the miko's milk): one liquid surface
        if (g_milkT.w != g_w / 2 || g_milkT.h != g_h / 2) MakeTarget(g_milkT, g_w / 2, g_h / 2, GL_RGBA16F);
        G.BindFramebuffer(GL_FRAMEBUFFER, g_milkT.fbo);
        G.Viewport(0, 0, g_milkT.w, g_milkT.h);
        const float zero[4] = { 0, 0, 0, 0 };
        G.ClearBufferfv(GL_COLOR, 0, zero);
        U2f(g_pPartDraw, "uFull", (float)g_milkT.w, (float)g_milkT.h);
        U1f(g_pPartDraw, "uScale", WorldToPx() * 0.5f);
        G.Uniform1i(G.GetUniformLocation(g_pPartDraw, "uPass"), 2);
        G.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        G.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, kMaxParticles);
        G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
        G.Viewport(0, 0, g_w, g_h);
        Use(g_pMilk);
        U1i(g_pMilk, "uD", 0);
        U2f(g_pMilk, "uTexel", 1.0f / g_milkT.w, 1.0f / g_milkT.h);
        U1f(g_pMilk, "uWater", cfg.funMiko ? 0.0f : 1.0f);
        Bind(0, g_milkT.tex);
        G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        G.BlendFuncSeparatei(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // milk hides the glow behind it
        G.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (g_pBeamFx && cfg.gameFx && cfg.gfLasers && cfg.impactFx && cfg.gfBeamLook > 0 && !g_gfLasers.empty()) {   // lasers and firebeams: the layered beam
        G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
        G.Viewport(0, 0, g_w, g_h);
        Use(g_pBeamFx);
        U2f(g_pBeamFx, "uFull", (float)g_w, (float)g_h);
        U1f(g_pBeamFx, "uTime", g_fxNow);
        G.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        float px = WorldToPx();
        for (auto& L : g_gfLasers) {
            if (!L.hit) continue;
            float on = std::min(1.0f, (g_fxNow - L.t0) / 0.1f) * std::max(0.0f, 1.0f - std::max(0.0f, g_fxNow - L.lastHit - 0.15f) / 0.3f);
            if (on <= 0.0f) continue;
            float len = hypotf(L.hx - L.x, L.hy - L.y);
            if (len < 1.0f) continue;
            float pts[16];
            for (int i = 0; i < 4; i++) {
                float t = i / 3.0f, sx, sy;
                WorldToScreen(L.x + (L.hx - L.x) * t, L.y + (L.hy - L.y) * t, sx, sy);
                pts[i * 4] = sx; pts[i * 4 + 1] = sy; pts[i * 4 + 2] = std::max(60.0f * px, 6.0f); pts[i * 4 + 3] = len * t;
            }
            G.Uniform4fv(G.GetUniformLocation(g_pBeamFx, "uPts"), 4, pts);
            G.Uniform1i(G.GetUniformLocation(g_pBeamFx, "uCount"), 4);
            U1f(g_pBeamFx, "uLen", len);
            U1f(g_pBeamFx, "uI", on * cfg.gfBeamLook);
            if (L.firebeam) U4f(g_pBeamFx, "uCol", 1.0f, 0.5f, 0.15f, 1.0f);
            else U4f(g_pBeamFx, "uCol", 0.45f, 0.75f, 1.0f, 1.0f);
            G.DrawArrays(GL_TRIANGLE_STRIP, 0, 8);
        }
    }
    if (!g_streams.empty() && g_pStream) {   // the water cannon's jets: each one continuous stream
        G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
        G.Viewport(0, 0, g_w, g_h);
        Use(g_pStream);
        U2f(g_pStream, "uFull", (float)g_w, (float)g_h);
        U1f(g_pStream, "uTime", g_fxNow);
        U1f(g_pStream, "uWater", cfg.funMiko ? 0.0f : 1.0f);
        G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        G.BlendFuncSeparatei(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        float px = WorldToPx();
        for (auto& st : g_streams) {
            int n = std::min((int)st.size() / 4, 256);
            if (n < 2) continue;
            std::vector<float> u((size_t)n * 4);
            for (int i = 0; i < n; i++) {
                float sx, sy;
                WorldToScreen(st[i * 4], st[i * 4 + 1], sx, sy);
                u[i * 4] = sx; u[i * 4 + 1] = sy; u[i * 4 + 2] = std::max(st[i * 4 + 2] * px, 1.5f); u[i * 4 + 3] = st[i * 4 + 3];
            }
            G.Uniform4fv(G.GetUniformLocation(g_pStream, "uPts"), n, u.data());
            G.Uniform1i(G.GetUniformLocation(g_pStream, "uCount"), n);
            U1f(g_pStream, "uLen", st[(size_t)(n - 1) * 4 + 3]);
            G.DrawArrays(GL_TRIANGLE_STRIP, 0, n * 2);
        }
    }
    G.Disable(GL_BLEND);
}

// Flash lights for the lighting input, in working-grid cells.
static int CollectLights(float* pos, float* col) {
    int n = 0;
    auto add = [&](float x, float y, float r, float inten, const float* c) {
        if (n >= 16) return;
        float sx, sy;
        WorldToScreen(x, y, sx, sy);
        pos[n * 4] = (sx + g_map.ox) / g_map.p;
        pos[n * 4 + 1] = (sy + g_map.oy) / g_map.p;
        pos[n * 4 + 2] = std::max(r * WorldToPx() / g_map.p, 1.5f);
        pos[n * 4 + 3] = inten;
        col[n * 4] = c[0]; col[n * 4 + 1] = c[1]; col[n * 4 + 2] = c[2]; col[n * 4 + 3] = 0;
        n++;
    };
    for (auto& l : g_labLights)
        if (l.on) add(l.x, l.y, l.r, l.inten, l.col);
    float fx = EffectLightScale();   // explosion, flash, plume and tracer lights: the effects preset's strength
    for (auto& l : g_frameLights) add(l.x, l.y, l.r, l.inten * fx, l.col);
    for (auto& f : g_flashes) {
        float a = 1.0f - (g_fxNow - f.t0) / f.life;
        add(f.x, f.y, f.r, f.intensity * a * a * fx, f.col);
    }
    return n;
}

// Final-pass uniforms for shockwaves, shake, heat haze, colour pulse and impact frames.
static void SetFinalFxUniforms(GLuint p) {
    float sh[16 * 4] = {};
    int n = 0;
    float wh = std::max(WorldHeight(), 1.0f);
    for (auto& s : g_shocks) {
        float age = (g_fxNow - s.t0) / s.life;
        float sx, sy;
        WorldToScreen(s.x, s.y, sx, sy);
        sh[n * 4] = sx / g_w; sh[n * 4 + 1] = sy / g_h;
        sh[n * 4 + 2] = (1.0f - powf(1.0f - age, 2.5f)) * s.r / wh;
        sh[n * 4 + 3] = s.strength * (1.0f - age) * (1.0f - age);
        if (++n == 16) break;
    }
    G.Uniform1i(G.GetUniformLocation(p, "uShockCount"), cfg.impactFx ? n : 0);
    G.Uniform4fv(G.GetUniformLocation(p, "uShock"), 16, sh);
    float t2 = g_trauma * g_trauma * (cfg.impactFx ? 1.0f : 0.0f);
    float jx = sinf(g_fxNow * 47.0f) * 0.6f + sinf(g_fxNow * 83.0f) * 0.4f, jy = cosf(g_fxNow * 53.0f) * 0.6f + sinf(g_fxNow * 71.0f) * 0.4f;
    U4f(p, "uShake", jx * t2 * 0.012f, jy * t2 * 0.012f, t2 * 0.025f, cfg.impactFx ? cfg.heatHaze : 0.0f);
    float ia = 0;
    if (cfg.impactFx && g_fxNow - g_impactT0 < cfg.impactDurationMs / 1000.0f) ia = 1.0f;
    float ix, iy;
    WorldToScreen(g_impactX, g_impactY, ix, iy);
    U4f(p, "uImpact", ia, ix / g_w, iy / g_h, (g_frameNo & 1) ? 1.0f : 0.0f);
    float pa = cfg.impactFx ? std::max(0.0f, 1.0f - (g_fxNow - g_pulseT0) / 0.35f) * g_pulseAmt : 0.0f;
    float et = g_fxNow - g_exposureT0;   // after a heavy flash the camera stops down, then recovers
    float dip = cfg.impactFx && et > 0.0f && et < 5.0f ? g_exposureAmt * (et < 0.25f ? et / 0.25f : expf(-(et - 0.25f) * 1.6f)) : 0.0f;
    U4f(p, "uPulse", pa * pa * 0.35f, pa * 0.3f, pa * 0.5f, dip * 0.45f);

    // heat haze sources: hot spots (explosions, firing guns), burning things, live fireballs
    struct Src { float x, y, r, s; };
    std::vector<Src> src;
    for (auto& s : g_heat) src.push_back({ s.x, s.y, s.r, std::min(1.0f, s.heat) });
    for (auto& in : g_inst)
        if (in.cls == kEvFire && g_fxFrame - in.lastSeen <= 1 && cfg.fireHeat > 0)   // one flame: a small, gentle shimmer
            src.push_back({ in.x, in.y, std::min(in.r, wh * 0.025f), 0.9f * cfg.fireHeat });
    for (auto& f : g_fireballs) src.push_back({ f.x, f.y, f.r, 1.0f - (g_fxNow - f.t0) / f.life });
    for (auto& t : g_gfTrail) src.push_back({ t.x, t.y, wh * 0.01f, 0.35f });
    std::sort(src.begin(), src.end(), [](const Src& a, const Src& b) { return a.s > b.s; });
    float hs[16 * 4] = {};
    int hn = 0;
    for (auto& s : src) {
        if (hn == 16 || s.s <= 0.01f) break;
        float sx, sy;
        WorldToScreen(s.x, s.y, sx, sy);
        hs[hn * 4] = sx / g_w; hs[hn * 4 + 1] = sy / g_h; hs[hn * 4 + 2] = std::max(s.r / wh, 0.004f); hs[hn * 4 + 3] = s.s;
        hn++;
    }
    G.Uniform1i(G.GetUniformLocation(p, "uHeatCount"), cfg.impactFx ? hn : 0);
    G.Uniform4fv(G.GetUniformLocation(p, "uHeatSrc"), 16, hs);
}


static std::unordered_set<GLuint> g_waterPartTex, g_waterPartDrawn;   // the water cannon's textures; those seen in a draw (g_emitMx)
static std::atomic<bool> g_anyWaterPart{ false };
static void SetEventClass(GLuint tex, const std::string& base, const std::string& path) {
    uint8_t c = base.empty() ? (uint8_t)kEvNone : EventClassForName(base);
    // Names only count where effects live. The DLC images now have names too, and their fort materials
    // and weapon sprites use the same words: blast doors are no explosions, the smoke bomb's shell is no
    // smoke, a weapon's firing animation is no fire, a flashbang in flight is no muzzle flash.
    bool fx = path.find("effects/") != std::string::npos;
    auto in = [&](const char* k) { return path.find(k) != std::string::npos; };
    switch (c) {
    case kEvExplosion: case kEvSteam: case kEvSmoke: case kEvSpark: if (!fx) c = kEvNone; break;
    case kEvFire: if (!fx && !in("materials/")) c = kEvNone; break;          // flames, and burning fuses
    case kEvFlash: if ((!fx && !in("weapons/")) || in("flashbang_")) c = kEvNone; break;   // muzzle flashes
    case kEvTracer: if (in("bullethole")) c = kEvNone; break;                // a decal, not a bullet in flight
    default: break;
    }
    // the DLC water cannon (the milk miko joke): its water shots, its barrel, the rest of it
    if (!base.empty() && (in("water_cannon") || in("watercannon") || in("water cannon") || in("water_gun") || in("watergun")) &&
        !in("ui/") && (in("weapons/") || in("devices/"))) {
        auto hb = [&](const char* k) { return base.find(k) != std::string::npos; };
        c = (hb("projectile") || hb("shot") || hb("stream") || hb("jet")) ? kEvMilk
            : (hb("base") || hb("body") || hb("stand") || hb("mount")) ? kEvMiko
            : (hb("head") || hb("barrel") || hb("nozzle") || hb("turret") || hb("hose") || hb("gun")) ? kEvMikoGun : kEvMiko;
        if (c != kEvMilk || base.find("_30") != std::string::npos || base.find("projectile.") != std::string::npos)
            SessionLog("fun: water cannon part %s (texture %u) -> %s", path.c_str(), tex, c == kEvMilk ? "a water shot" : c == kEvMiko ? "the miko" : "her milk cannon");
        g_waterPartTex.insert(tex);
        g_anyWaterPart = true;
    }
    uint8_t puff = 0;
    bool extra = false;
    if (c == kEvSmoke || c == kEvSteam) {
        auto has = [&](const char* k) { return base.find(k) != std::string::npos; };
        if (has("fire_deckgun")) { puff = 12; extra = true; }                       // hot gas cooling into smoke
        else if (has("sandcloud") || has("dust")) { puff = 11; extra = true; }      // dust
        else if (has("cloud") && !has("steam")) { puff = 9; extra = true; }         // smoke
        if (extra && path.find("effects/") == std::string::npos) c = kEvNone;        // map props named "clouds" stay as they are
    }
    if (c && puff) g_puffTex[tex] = puff; else g_puffTex.erase(tex);
    if (c && extra) g_extraTex.insert(tex); else g_extraTex.erase(tex);
    if (c) g_evClassTex[tex] = c; else g_evClassTex.erase(tex);
}
static void PuffOf(GLuint tex, uint8_t& puff, bool& extra) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    auto it = g_puffTex.find(tex);
    puff = it == g_puffTex.end() ? 0 : it->second;
    extra = g_extraTex.count(tex) != 0;
}

static uint8_t EventClassOf(GLuint tex) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    auto it = g_evClassTex.find(tex);
    return it == g_evClassTex.end() ? (uint8_t)kEvNone : it->second;
}

// ---------------------------------------------------------------- weather
// Rain: the game's rain is a few huge tiled overlays of a streak texture. Large textures drawn
// that big over the playfield are probed once (streak coherence tells rain from snow); rain tiles
// are hidden and replaced by particle drops that collide with the real terrain and splash, a near
// rain layer in the composite and wet surfaces. Lightning: procedural branching bolts that find
// the ground from the solid mask, flicker with restrikes, light the scene through the ray tracer
// and the sky-visibility term, and are followed by synthesized thunder.

static std::unordered_set<GLuint> g_largeTex;             // >= 1024 px textures (guarded by g_emitMx)
static std::unordered_map<GLuint, int> g_weatherTex;      // probed overlay textures: 1 rain, 2 other
static std::vector<GLuint> g_weatherToProbe;
static unsigned g_rainSeenFrame;                          // last frame a game rain layer was drawn
static float g_rainSpawnAcc, g_nextStrike = 5.0f;

struct Bolt { std::vector<float> pts; std::vector<int> branchStart; float t0, life, x, groundY; bool thunder; float thunderAt; };
static std::vector<Bolt> g_bolts;
static float g_flashAmt;                                   // sky flash this frame (0..)


// ---- water: a map's main .lua (maps\<name>\<name>.lua) may set dlc2_WaterLevel (world y, y down)
void GfxNoteLuaFile(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("/maps/") == std::string::npos && lower.compare(0, 5, "maps/") != 0) return;
    size_t slash = lower.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string base = lower.substr(slash + 1, lower.size() - slash - 5);
    size_t prev = lower.find_last_of('/', slash - 1);
    std::string folder = lower.substr(prev == std::string::npos ? 0 : prev + 1, slash - (prev == std::string::npos ? 0 : prev + 1));
    if (base != folder) return;                     // fort layouts and scripts live next to it
    FILE* f = _wfopen(Widen(p).c_str(), L"rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1u << 20)) text.append(buf, got);
    fclose(f);
    bool found = false;
    float level = 0;
    for (size_t at = text.find("WaterLevel"); at != std::string::npos; at = text.find("WaterLevel", at + 10)) {
        size_t lineStart = text.rfind('\n', at);
        std::string before = text.substr(lineStart == std::string::npos ? 0 : lineStart + 1, at - (lineStart == std::string::npos ? 0 : lineStart + 1));
        if (before.find("--") != std::string::npos) continue;   // commented out
        size_t eq = text.find('=', at);
        if (eq == std::string::npos || eq - at > 24) continue;
        level = (float)atof(text.c_str() + eq + 1);
        found = true;
        break;
    }
    // (the water itself is read from the drawing: the menus read every map's script, and packed DLC
    // maps are never seen here)
    g_terrainReset = true;   // a new map: its terrain lists replace the old ones
    LoadLog("map script %s: %s", folder.c_str(), found ? Fmt("water level %.0f", level).c_str() : "no water");
}

void GfxNoteTextureSize(GLuint tex, int w, int h) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    if (w >= 1024 && h >= 1024) g_largeTex.insert(tex); else g_largeTex.erase(tex);
    g_weatherTex.erase(tex);
}

static bool IsLargeTex(GLuint tex) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    return g_largeTex.count(tex) != 0;
}

static int WeatherKind(GLuint tex) {
    std::lock_guard<std::mutex> lk(g_emitMx);
    auto it = g_weatherTex.find(tex);
    return it == g_weatherTex.end() ? 0 : it->second;
}

// 0 = no rain, 1 = rain, 1.7 = storm
static float RainLevel() {
    switch (cfg.weatherMode) {
    case 1: return 0.0f;
    case 2: return 1.0f;
    case 3: return 1.7f;
    default: return (g_fxFrame - g_rainSeenFrame < 30 && g_rainSeenFrame) ? 1.0f : 0.0f;   // auto: follow the map
    }
}
static bool Storming() { return cfg.weatherMode == 3 || (cfg.weatherMode == 0 && RainLevel() > 0 && cfg.autoStorms); }
// What falls as flakes and settles in the snow layer: 1 snow (thick 2.6), 2 sand (sandstorm), 3 ash.
static int PrecipKind() { return cfg.weatherMode == 6 ? 2 : cfg.weatherMode == 8 ? 3 : 1; }
static float SnowLevel() {
    switch (cfg.weatherMode) { case 4: return 1.0f; case 5: return 2.6f; case 6: return 2.2f; case 8: return 1.0f; default: return 0.0f; }
}
// Smoothed presence of the other weathers (0..1), so they fade in and out.
static float g_sandW, g_fogW, g_ashW;
// The wind that carries smoke, embers and debris: the set wind, and with GameFx.Weather the storm's too
// (rain, sandstorm, blizzard), gusting, from the side the rain leans from.
static float GfWind() {
    float w = cfg.fogWind;
    if (!cfg.gameFx || !cfg.gfWeather) return w;
    float storm = std::min(RainLevel(), 2.0f) * 0.5f + g_sandW * 1.5f + (cfg.weatherMode == 5 ? 0.8f : 0.0f);
    float dir = cfg.rainLean >= 0 ? 1.0f : -1.0f;
    return w + dir * WorldHeight() * 0.02f * storm * (0.75f + 0.25f * sinf(g_fxNow * 0.37f) + 0.1f * sinf(g_fxNow * 1.3f));
}
// Surfaces get wet within seconds and dry over a minute; snow builds up over ~half a minute and melts slowly.
static float g_wetAmt, g_snowCover, g_snowMood, g_snowSpawnAcc;
void GfxReplaySnowCover(float c) { g_snowCover = c; g_snowMood = 1; }
static float g_skyCol[3] = { 0.8f, 0.78f, 0.72f };   // horizon colour for wet reflections (per frame, from the backdrop)
static void BackdropColours(float skyTop[3], float skyHor[3], float mtn[3][3]);   // backdrop section
void GfxReplayWet(float w) { g_wetAmt = w; }

// Reads the texture back small and measures how line-like its alpha is (structure tensor coherence).
static void ProbeWeatherTexture(GLuint tex) {
    static Target probe;
    if (!probe.fbo) MakeTarget(probe, 128, 128, GL_RGBA8);
    Use(g_pCopy);
    U1i(g_pCopy, "uSrc", 0);
    U1i(g_pCopy, "uAlpha", 1);
    Bind(0, tex);
    Draw(probe.fbo, 128, 128);
    U1i(g_pCopy, "uAlpha", 0);
    std::vector<uint8_t> px(128 * 128 * 4);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, probe.fbo);
    R.ReadBuffer(GL_COLOR_ATTACHMENT0);
    R.ReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    double jxx = 0, jyy = 0, jxy = 0;
    auto lum = [&](int x, int y) { x = (x + 128) % 128; y = (y + 128) % 128; const uint8_t* p = &px[(y * 128 + x) * 4]; return (p[0] + p[1] + p[2]) / 765.0; };
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            double gx = lum(x + 1, y) - lum(x - 1, y), gy = lum(x, y + 1) - lum(x, y - 1);
            jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
        }
    double tr = jxx + jyy, coh = tr > 1e-9 ? sqrt((jxx - jyy) * (jxx - jyy) + 4 * jxy * jxy) / tr : 0;
    int kind = coh > 0.35 ? 1 : 2;
    {
        std::lock_guard<std::mutex> lk(g_emitMx);
        g_weatherTex[tex] = kind;
    }
    SessionLog("gfx: weather overlay tex %u: streak coherence %.2f -> %s", tex, coh, kind == 1 ? "rain (replaced)" : "not rain (kept)");
}

// --- thunder: brown-noise rumble with crackle, rendered once per variant into an in-memory WAV
static std::vector<uint8_t> g_thunderWav[3];
static void BuildThunder() {
    if (!g_thunderWav[0].empty()) return;
    const int rate = 22050;
    for (int v = 0; v < 3; v++) {
        int n = rate * (3 + v);
        std::vector<int16_t> s(n);
        float brown = 0, lp = 0;
        uint32_t seed = 1234567 + v * 7919;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 8388608.0f - 1.0f; };
        float crackEnd = 0.25f + 0.15f * v;
        for (int i = 0; i < n; i++) {
            float t = (float)i / rate;
            brown = brown * 0.995f + rnd() * 0.06f;
            lp += (brown - lp) * 0.08f;
            float env = (1.0f - expf(-t * 25.0f)) * expf(-t * (0.9f + 0.25f * v));
            float roll = 0.7f + 0.3f * sinf(t * 5.3f + v) * sinf(t * 2.1f);
            float crack = t < crackEnd ? rnd() * expf(-t * 18.0f) * 0.5f : 0.0f;
            float x = (lp * 5.0f * roll + crack) * env;
            s[i] = (int16_t)(std::max(-1.0f, std::min(1.0f, x)) * 30000);
        }
        std::vector<uint8_t>& w = g_thunderWav[v];
        uint32_t data = n * 2;
        w.resize(44 + data);
        auto u32 = [&](size_t o, uint32_t x) { memcpy(&w[o], &x, 4); };
        auto u16 = [&](size_t o, uint16_t x) { memcpy(&w[o], &x, 2); };
        memcpy(&w[0], "RIFF", 4); u32(4, 36 + data); memcpy(&w[8], "WAVEfmt ", 8);
        u32(16, 16); u16(20, 1); u16(22, 1); u32(24, rate); u32(28, rate * 2); u16(32, 2); u16(34, 16);
        memcpy(&w[36], "data", 4); u32(40, data);
        memcpy(&w[44], s.data(), data);
    }
}

static void PlayThunder(float volume) {
    BuildThunder();
    int v = (int)(Rand01() * 3) % 3;
    std::vector<uint8_t> scaled = g_thunderWav[v];
    int16_t* s = (int16_t*)&scaled[44];
    size_t n = (scaled.size() - 44) / 2;
    for (size_t i = 0; i < n; i++) s[i] = (int16_t)(s[i] * std::min(std::max(volume, 0.0f), 1.0f));
    static std::vector<uint8_t> playing;    // PlaySound reads the buffer asynchronously
    playing.swap(scaled);
    PlaySoundW((LPCWSTR)playing.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// First solid pixel from the top of the screen in one column of the solid mask (world y).
static float GroundBelow(float worldX) {
    float sx, sy;
    WorldToScreen(worldX, g_world[3], sx, sy);
    int x = std::min(std::max((int)sx, 0), g_w - 1);
    std::vector<uint8_t> col((size_t)g_h * 4);
    static GLuint readFbo;
    if (!readFbo) G.GenFramebuffers(1, &readFbo);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    G.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_solidTex, 0);
    R.ReadBuffer(GL_COLOR_ATTACHMENT0);
    R.ReadPixels(x, 0, 1, g_h, GL_RGBA, GL_UNSIGNED_BYTE, col.data());
    for (int y = g_h - 1; y >= 0; y--)            // GL rows: top of the screen is the last row
        if (col[(size_t)y * 4 + 3] > 128) return g_world[2] + (float)y / g_h * (g_world[3] - g_world[2]);
    return g_world[2];                             // nothing solid: strike the bottom of the view
}

static void BoltPath(std::vector<float>& pts, float x0, float y0, float x1, float y1, float rough, int depth) {
    if (depth == 0) { pts.push_back(x1); pts.push_back(y1); return; }
    float mx = (x0 + x1) * 0.5f + RandRange(-1, 1) * rough, my = (y0 + y1) * 0.5f + RandRange(-0.3f, 0.3f) * rough;
    BoltPath(pts, x0, y0, mx, my, rough * 0.55f, depth - 1);
    BoltPath(pts, mx, my, x1, y1, rough * 0.55f, depth - 1);
}

void LightningStrike(float worldX, bool forced) {
    if (!forced && !cfg.lightning) return;
    Bolt b;
    float top = g_world[3] - WorldHeight() * 0.15f;
    b.x = worldX;
    b.groundY = GroundBelow(worldX);
    float len = fabsf(b.groundY - top);
    b.pts = { worldX + RandRange(-1, 1) * len * 0.2f, top };
    BoltPath(b.pts, b.pts[0], top, worldX, b.groundY, len * 0.18f, 6);
    for (int k = 0; k < 3; k++) {                  // branches split off the upper part
        size_t i = (size_t)(RandRange(0.1f, 0.6f) * (b.pts.size() / 2 - 1)) * 2;
        float bx = b.pts[i], by = b.pts[i + 1];
        float ex = bx + RandRange(-1, 1) * len * 0.35f, ey = by + len * RandRange(0.15f, 0.4f);
        b.branchStart.push_back((int)b.pts.size() / 2);
        b.pts.push_back(bx); b.pts.push_back(by);
        BoltPath(b.pts, bx, by, ex, ey, len * 0.08f, 4);
    }
    b.t0 = g_fxNow;
    b.life = 0.55f;
    b.thunder = cfg.thunder;
    b.thunderAt = g_fxNow + RandRange(0.25f, 1.6f);
    g_bolts.push_back(b);
    g_trauma = std::min(1.0f, g_trauma + cfg.shake * 0.25f);
}

// flicker of a strike over its life: main flash, restrikes, fade
static float BoltIntensity(float age) {
    if (age < 0.06f) return 1.0f;
    if (age < 0.11f) return 0.15f;
    if (age < 0.17f) return 0.85f;
    if (age < 0.24f) return 0.2f;
    if (age < 0.30f) return 0.6f;
    return std::max(0.0f, 0.5f * (1.0f - (age - 0.3f) / 0.25f));
}

static void UpdateWeather() {
    float rain = RainLevel(), snow = SnowLevel();
    if (g_wetAmt > 0.001f) { float st[3], mt[3][3]; BackdropColours(st, g_skyCol, mt); }   // may sample textures: before any pass is set up
    float wetTarget = std::min(rain, 1.4f);
    g_wetAmt += (wetTarget - g_wetAmt) * std::min(1.0f, g_fxDt * (wetTarget > g_wetAmt ? 0.3f : 0.025f));
    float coverTarget = snow > 0 ? std::min(1.0f, 0.55f + 0.18f * snow) : 0.0f;
    if (g_snowCover < coverTarget) g_snowCover = std::min(coverTarget, g_snowCover + g_fxDt * snow / 35.0f);
    else g_snowCover = std::max(coverTarget, g_snowCover - g_fxDt / 90.0f);
    int kind = PrecipKind();
    float snowOnly = kind == 1 ? snow : 0.0f;
    g_snowMood += ((snowOnly > 0 ? std::min(1.0f, snowOnly / 2.6f + 0.4f) : 0.0f) - g_snowMood) * std::min(1.0f, g_fxDt * 0.4f);
    auto fade = [&](float& w, bool on) { w += ((on ? 1.0f : 0.0f) - w) * std::min(1.0f, g_fxDt * 0.35f); };
    fade(g_sandW, cfg.weatherMode == 6); fade(g_fogW, cfg.weatherMode == 7); fade(g_ashW, cfg.weatherMode == 8);
    if (g_ashW > 0.05f && cfg.embers > 0) {   // ash & embers: glowing embers carried on the wind
        static float acc;
        acc += g_fxDt * 28.0f * g_ashW * cfg.embers;
        float vw = fabsf(g_world[1] - g_world[0]), vh = WorldHeight();
        while (acc >= 1.0f) {
            acc -= 1.0f;
            float x = std::min(g_world[0], g_world[1]) + RandRange(-0.1f, 1.1f) * vw, y = g_world[2] + RandRange(0.1f, 0.95f) * (g_world[3] - g_world[2]);
            SpawnParticle(0, x, y, vh * RandRange(0.02f, 0.08f), -vh * RandRange(0.0f, 0.05f), RandRange(1.5f, 3.5f));
        }
    }
    // snowflakes: spawned above the view, drifting down with the wind (gusts in thick snow)
    if (snow > 0 && cfg.snowAmount > 0) {
        float vw = fabsf(g_world[1] - g_world[0]), vh = WorldHeight();
        g_snowSpawnAcc += g_fxDt * cfg.snowAmount * 300.0f * snow;
        int n = std::min((int)g_snowSpawnAcc, 400);
        g_snowSpawnAcc -= n;
        float gust = snow > 2.0f ? sinf(g_fxNow * 0.31f) * 0.6f + sinf(g_fxNow * 0.83f) * 0.3f : 0.0f;
        for (int i = 0; i < n; i++) {
            if (kind == 2) {   // sand: driven sideways by the storm, entering from the upwind side
                float dirw = cfg.rainLean >= 0 ? 1.0f : -1.0f;
                float sx = vw * RandRange(0.22f, 0.45f) * (1.0f + gust * 0.5f) * dirw;
                float x = dirw > 0 ? std::min(g_world[0], g_world[1]) - RandRange(0.0f, 0.3f) * vw : std::max(g_world[0], g_world[1]) + RandRange(0.0f, 0.3f) * vw;
                float y = g_world[3] + RandRange(-0.1f, 0.95f) * (g_world[2] - g_world[3]);
                if (Rand01() < 0.35f) x = std::min(g_world[0], g_world[1]) + RandRange(0.0f, 1.0f) * vw;   // some appear anywhere in view
                SpawnFlake(x, y, sx, vh * RandRange(0.02f, 0.08f), 10.0f);
                continue;
            }
            float fall = vh * (kind == 3 ? RandRange(0.04f, 0.08f) : RandRange(0.07f, 0.15f) * (snow > 2.0f ? 1.5f : 1.0f));
            float x = std::min(g_world[0], g_world[1]) + RandRange(-0.3f, 1.3f) * vw;
            float y = g_world[3] - RandRange(0.0f, 0.1f) * vh;
            // a share of the flakes appears anywhere in the view (fading in), so zooming out or panning to a
            // new place is filled within a second or two instead of only where the camera has been
            if (Rand01() < 0.35f) y = g_world[3] + RandRange(0.0f, 1.0f) * (g_world[2] - g_world[3]);
            SpawnFlake(x, y, fall * (cfg.rainLean * 0.8f + gust), fall, kind == 3 ? 22.0f : 16.0f);
        }
    }
    // rain drops, spawned above the view and falling with the wind
    if (rain > 0 && cfg.rainAmount > 0) {
        float vw = fabsf(g_world[1] - g_world[0]), vh = WorldHeight();
        float speed = vh * 1.15f;
        g_rainSpawnAcc += g_fxDt * cfg.rainAmount * 1100.0f * rain;
        int n = std::min((int)g_rainSpawnAcc, 200);
        g_rainSpawnAcc -= n;
        for (int i = 0; i < n; i++) {
            float x = std::min(g_world[0], g_world[1]) + RandRange(-0.2f, 1.2f) * vw - cfg.rainLean * vh * 0.5f;
            float y = g_world[3] - RandRange(0.0f, 0.15f) * vh;
            float s = speed * RandRange(0.85f, 1.15f);
            SpawnParticle(3, x, y, s * cfg.rainLean, s, 1.6f);
        }
    }
    // lightning: the first strike comes a few seconds after a storm starts
    static bool wasStorming;
    bool storming = Storming() && cfg.lightning && cfg.lightningRate > 0;
    if (storming && !wasStorming) g_nextStrike = g_fxNow + RandRange(2.0f, 5.0f);
    wasStorming = storming;
    if (storming && g_fxNow >= g_nextStrike) {
        LightningStrike(std::min(g_world[0], g_world[1]) + RandRange(0.1f, 0.9f) * fabsf(g_world[1] - g_world[0]), false);
        g_nextStrike = g_fxNow + RandRange(0.4f, 1.6f) * 60.0f / cfg.lightningRate;
    }
    g_flashAmt = 0;
    for (auto& b : g_bolts) {
        float age = g_fxNow - b.t0, k = BoltIntensity(age);
        g_flashAmt = std::max(g_flashAmt, k * cfg.lightningBright);
        if (k > 0.05f && cfg.flashLight > 0) {   // light along the bolt, strongest where it hits
            size_t main = b.branchStart.empty() ? b.pts.size() / 2 : (size_t)b.branchStart[0];
            for (int s = 1; s <= 3; s++) {
                size_t i = std::min(main - 1, main * s / 3) * 2;
                g_flashes.push_back({ b.pts[i], b.pts[i + 1], WorldHeight() * 0.07f, g_fxNow, 0.02f, cfg.lightningBright * 2.0f * k, { 0.75f, 0.85f, 1.0f } });
            }
        }
        if (b.thunder && g_fxNow >= b.thunderAt) { b.thunder = false; PlayThunder(cfg.thunderVolume); }
    }
    g_bolts.erase(std::remove_if(g_bolts.begin(), g_bolts.end(), [](const Bolt& b) { return g_fxNow - b.t0 > b.life && !b.thunder; }), g_bolts.end());
}

// Bolts as glowing quads (instanced segments) into scene + light-source targets.
static void DrawBolts() {
    if (g_bolts.empty() || !g_pBolt) return;
    float seg[128 * 4], wid[128 * 4];
    int n = 0;
    float px = WorldToPx();
    for (auto& b : g_bolts) {
        float k = BoltIntensity(g_fxNow - b.t0);
        if (k <= 0.01f) continue;
        size_t main = b.branchStart.empty() ? b.pts.size() / 2 : (size_t)b.branchStart[0];
        for (size_t i = 0; i + 1 < b.pts.size() / 2 && n < 128; i++) {
            bool branchJump = std::find(b.branchStart.begin(), b.branchStart.end(), (int)(i + 1)) != b.branchStart.end();
            if (branchJump) continue;
            float x0, y0, x1, y1;
            WorldToScreen(b.pts[i * 2], b.pts[i * 2 + 1], x0, y0);
            WorldToScreen(b.pts[i * 2 + 2], b.pts[i * 2 + 3], x1, y1);
            bool isMain = i + 1 < main;
            seg[n * 4] = x0; seg[n * 4 + 1] = y0; seg[n * 4 + 2] = x1; seg[n * 4 + 3] = y1;
            wid[n * 4] = (isMain ? 7.0f : 3.5f) * g_h / 2160.0f; wid[n * 4 + 1] = k * (isMain ? 1.0f : 0.6f) * cfg.lightningBright;
            wid[n * 4 + 2] = 0; wid[n * 4 + 3] = 0;
            n++;
        }
    }
    (void)px;
    if (!n) return;
    G.BindFramebuffer(GL_FRAMEBUFFER, g_fxFbo);
    G.Viewport(0, 0, g_w, g_h);
    G.Enable(GL_BLEND);
    G.ColorMaski(2, 0, 0, 0, 0);
    G.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    Use(g_pBolt);
    U2f(g_pBolt, "uFull", (float)g_w, (float)g_h);
    G.Uniform4fv(G.GetUniformLocation(g_pBolt, "uSeg"), n, seg);
    G.Uniform4fv(G.GetUniformLocation(g_pBolt, "uWid"), n, wid);
    G.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    G.Disable(GL_BLEND);
}

// Composite uniforms: rain amount for the near layer and wet surfaces, sky flash.
static void SetWeatherUniforms(GLuint p) {
    float rain = RainLevel();
    float snow = SnowLevel();
    U4f(p, "uRain", rain * cfg.nearRain, cfg.rainLean, g_wetAmt * cfg.wetness, g_fxNow);
    U4f(p, "uFlash", g_flashAmt, 0.75f, 0.85f, 1.0f);
    float gust = snow > 2.0f ? sinf(g_fxNow * 0.31f) * 0.6f + sinf(g_fxNow * 0.83f) * 0.3f : 0.0f;
    int kind = PrecipKind();
    float veilAmt = kind == 2 ? 0.45f * g_sandW : kind == 3 ? 0.25f * g_ashW : snow > 2.0f ? 0.3f * std::min(cfg.snowAmount, 2.0f) : snow > 0 ? 0.05f : 0.0f;
    U4f(p, "uSnow", snow * 0.6f * cfg.snowAmount, g_snowCover * cfg.snowCover * 24.0f, veilAmt, (cfg.rainLean + gust) * (kind == 2 ? 6.0f : 2.0f));
    if (kind == 2) { U4f(p, "uVeil", 0.74f, 0.47f, 0.22f, 0.6f * g_sandW); U4f(p, "uSnowCol", 0.80f, 0.62f, 0.38f, 2); }
    else if (kind == 3) { U4f(p, "uVeil", 0.30f, 0.25f, 0.24f, 0.0f); U4f(p, "uSnowCol", 0.26f, 0.25f, 0.25f, 3); }
    else { U4f(p, "uVeil", 0.62f, 0.66f, 0.72f, 0.0f); U4f(p, "uSnowCol", 0.80f, 0.84f, 0.90f, 1); }
    U4f(p, "uWet", cfg.puddles, 0, 0, 0);
    U4f(p, "uSkyC", powf(g_skyCol[0], 2.2f) * g_dn.bgTint[0], powf(g_skyCol[1], 2.2f) * g_dn.bgTint[1], powf(g_skyCol[2], 2.2f) * g_dn.bgTint[2], 0);
    float rm = std::min(1.0f, g_wetAmt), sm = std::min(1.0f, g_snowMood);
    // weather mood: overcast rain is darker and cooler, snow is brighter (bounce) and cold
    float amb = (1.0f - 0.15f * rm) * (1.0f + 0.1f * sm) * (1.0f - 0.15f * g_sandW) * (1.0f - 0.05f * g_fogW) * (1.0f - 0.25f * g_ashW);
    float tmp = -0.3f * rm - 0.25f * sm + 0.35f * g_sandW - 0.1f * g_fogW + 0.3f * g_ashW;
    float sat = (1.0f - 0.12f * rm) * (1.0f - 0.15f * sm) * (1.0f - 0.15f * g_sandW) * (1.0f - 0.2f * g_fogW) * (1.0f - 0.2f * g_ashW);
    U4f(p, "uMood", amb, tmp, sat, 0);
}


// (weather section above)
// ---------------------------------------------------------------- hybrid 3D backdrop
// The game draws its backdrop first, farthest layer first, each layer with its own texture. Layers
// are counted as they arrive; the farthest N are hidden and our 3D backdrop is rendered into the
// scene just before the first kept layer (or at the start of the playfield if all are replaced),
// so the near painted hills still draw on top. Colours are sampled from the hidden layers.

struct TexPalette { float top[3], mid[3], avg[3]; float cover; };
static std::unordered_map<GLuint, TexPalette> g_palettes;
static int g_bgLayer = -1, g_bgLayersPrev;
static GLuint g_bgCurTex, g_bgLayerTex[32];
static bool g_bdDone;

static TexPalette SamplePalette(GLuint tex) {
    auto it = g_palettes.find(tex);
    if (it != g_palettes.end()) return it->second;
    static Target probe;
    if (!probe.fbo) MakeTarget(probe, 64, 64, GL_RGBA8);
    std::vector<uint8_t> rgb(64 * 64 * 4), alpha(64 * 64 * 4);
    Use(g_pCopy);
    U1i(g_pCopy, "uSrc", 0);
    Bind(0, tex);
    for (int pass = 0; pass < 2; pass++) {
        U1i(g_pCopy, "uAlpha", pass);
        Draw(probe.fbo, 64, 64);
        G.BindFramebuffer(GL_READ_FRAMEBUFFER, probe.fbo);
        R.ReadBuffer(GL_COLOR_ATTACHMENT0);
        R.ReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pass ? alpha.data() : rgb.data());
    }
    U1i(g_pCopy, "uAlpha", 0);
    TexPalette p{};
    double sum[3][4] = {};   // top rows, middle rows, everything (alpha-weighted)
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const uint8_t* c = &rgb[(y * 64 + x) * 4];
            double a = alpha[(y * 64 + x) * 4] / 255.0;
            int band = y >= 52 ? 0 : (y >= 20 && y < 44) ? 1 : -1;   // GL rows: 63 = top of the texture
            for (int b = 0; b < 3; b++) {
                if (b < 2 && b != band) continue;
                for (int k = 0; k < 3; k++) sum[b][k] += c[k] / 255.0 * a;
                sum[b][3] += a;
            }
        }
    float* outs[3] = { p.top, p.mid, p.avg };
    for (int b = 0; b < 3; b++)
        for (int k = 0; k < 3; k++) outs[b][k] = sum[b][3] > 1e-3 ? (float)(sum[b][k] / sum[b][3]) : 0.5f;
    p.cover = (float)(sum[2][3] / (64.0 * 64.0));
    g_palettes[tex] = p;
    return p;
}

static int BackdropReplaceCount() {
    if (cfg.bdHideOriginal) return 1000;   // all of them
    if (cfg.bgReplace >= 0) return cfg.bgReplace;
    return std::max(1, (int)lroundf(g_bgLayersPrev * cfg.bgAutoFraction));
}

static void BackdropColours(float skyTop[3], float skyHor[3], float mtn[3][3]) {
    auto set = [](float* d, const float* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; };
    set(skyTop, cfg.bdSkyTop); set(skyHor, cfg.bdSkyHorizon);
    for (int i = 0; i < 3; i++) set(mtn[i], cfg.bdMountain);
    int hidden = std::min(BackdropReplaceCount(), std::min(g_bgLayer + 1, 32));
    if (!cfg.bdColourFromMap || hidden <= 0) return;
    TexPalette sky = SamplePalette(g_bgLayerTex[0]);
    if (sky.cover > 0.3f) { set(skyTop, sky.top); set(skyHor, sky.mid); }
    // mountain colours: the hidden layers after the sky, far to near; reuse the last one if fewer
    float last[3] = { skyHor[0] * 0.8f, skyHor[1] * 0.75f, skyHor[2] * 0.75f };
    int src = 1;
    for (int i = 0; i < 3; i++) {
        if (src < hidden) {
            TexPalette m = SamplePalette(g_bgLayerTex[src++]);
            if (m.cover > 0.05f) set(last, m.avg);
        }
        set(mtn[i], last);
    }
}

static void RenderBackdropNow() {
    g_bdDone = true;
    if (!g_pBackdrop || !g_bdHalf.fbo) return;
    StateGuard sg;
    sg.Save();
    sg.Neutral();
    G.BindVertexArray(g_vao);
    float skyTop[3], skyHor[3], mtn[3][3];
    BackdropColours(skyTop, skyHor, mtn);
    {   // day / night: dusk colours, a deep blue night sky (the composite darkens all of the backdrop)
        const float duskTop[3] = { 0.34f, 0.25f, 0.45f }, duskHor[3] = { 1.0f, 0.52f, 0.28f }, nTop[3] = { 0.05f, 0.08f, 0.2f }, nHor[3] = { 0.12f, 0.16f, 0.32f };
        for (int k = 0; k < 3; k++) {
            skyTop[k] += (duskTop[k] - skyTop[k]) * g_dn.golden * 0.7f; skyHor[k] += (duskHor[k] - skyHor[k]) * g_dn.golden * 0.8f;
            skyTop[k] += (nTop[k] - skyTop[k]) * (1.0f - g_dn.day) * 0.7f; skyHor[k] += (nHor[k] - skyHor[k]) * (1.0f - g_dn.day) * 0.7f;
        }
        // weather skies: a dusty orange sandstorm, grey fog, a dark red ash sky
        const float sand[3] = { 0.78f, 0.55f, 0.32f }, fogc[3] = { 0.68f, 0.70f, 0.73f }, ash[3] = { 0.35f, 0.18f, 0.15f };
        for (int k = 0; k < 3; k++) {
            float* cols[2] = { &skyTop[k], &skyHor[k] };
            for (float* cc : cols) { *cc += (sand[k] - *cc) * 0.65f * g_sandW; *cc += (fogc[k] - *cc) * 0.55f * g_fogW; *cc += (ash[k] - *cc) * 0.7f * g_ashW; }
            for (int i = 0; i < 3; i++) { mtn[i][k] += (sand[k] * 0.8f - mtn[i][k]) * 0.5f * g_sandW; mtn[i][k] += (fogc[k] * 0.9f - mtn[i][k]) * 0.45f * g_fogW; mtn[i][k] += (ash[k] * 0.7f - mtn[i][k]) * 0.5f * g_ashW; }
        }
    }
    float camX = (g_world[0] + g_world[1]) * 0.5f, camY = -(g_world[2] + g_world[3]) * 0.5f;
    float vw = fabsf(g_world[1] - g_world[0]), vh = WorldHeight();
    Use(g_pBackdrop);
    U2f(g_pBackdrop, "uFull", (float)g_bdHalf.w, (float)g_bdHalf.h);
    U4f(g_pBackdrop, "uCam", camX, camY, vw, vh);
    U4f(g_pBackdrop, "uRef", cfg.bdRefWidth, cfg.bdRefWidth * g_h / std::max(g_w, 1), cfg.bdHorizon, cfg.bdParallax);
    U4f(g_pBackdrop, "uSkyTop", skyTop[0], skyTop[1], skyTop[2], 1);
    U4f(g_pBackdrop, "uSkyHor", skyHor[0], skyHor[1], skyHor[2], 1);
    float m4[12] = { mtn[0][0], mtn[0][1], mtn[0][2], 1, mtn[1][0], mtn[1][1], mtn[1][2], 1, mtn[2][0], mtn[2][1], mtn[2][2], 1 };
    G.Uniform4fv(G.GetUniformLocation(g_pBackdrop, "uMtn"), 3, m4);
    float sl = std::max(sqrtf(cfg.sunDir[0] * cfg.sunDir[0] + cfg.sunDir[1] * cfg.sunDir[1]), 1e-4f);
    float dsl = std::max(sqrtf(g_dn.sunDir[0] * g_dn.sunDir[0] + g_dn.sunDir[1] * g_dn.sunDir[1]), 1e-4f);
    U4f(g_pBackdrop, "uSunB", g_dn.sunDir[0] / dsl, g_dn.sunDir[1] / dsl, cfg.bdSunGlow * (1.0f + g_dn.golden), cfg.bdClouds);
    U4f(g_pBackdrop, "uSunCol", g_dn.sunCol[0], g_dn.sunCol[1], g_dn.sunCol[2], 1);
    U4f(g_pBackdrop, "uMisc", g_fxNow, cfg.bdStars + g_dn.stars, cfg.bdMountainHeight, (float)cfg.bdRanges);
    float fx = 0.5f;
    if (!g_bolts.empty()) { float sx, sy; WorldToScreen(g_bolts.back().x, g_world[3], sx, sy); fx = sx / g_w; }
    U4f(g_pBackdrop, "uFlashB", g_flashAmt, fx, cfg.bdCloudSpeed, std::min(1.0f, g_snowCover * 1.2f));
    {   // Mount Fuji while the milk miko is on the map: centred where the camera was when she arrived
        float refH = cfg.bdRefWidth * g_h / std::max(g_w, 1);
        U4f(g_pBackdrop, "uFuji", cfg.funMiko && cfg.mikoFuji && g_fujiOn ? 1.0f : 0.0f, g_fujiWorldX * 0.02f / std::max(refH, 1.0f), 0.75f, 0.42f);
    }
    Draw(g_bdHalf.fbo, g_bdHalf.w, g_bdHalf.h);
    Use(g_pCopy);
    U1i(g_pCopy, "uSrc", 0);
    U1i(g_pCopy, "uAlpha", 0);
    Bind(0, g_bdHalf.tex);
    Draw(g_bdSceneFbo, g_w, g_h);
    sg.Restore(false);
}

// Called for every world draw (takeover path): counts backdrop layers and hides the replaced ones.
// Returns true when this draw must be hidden.
static bool BackdropDraw(GLuint tex, bool texOn, bool displayList) {
    if (!cfg.backdrop3D || g_playfield || displayList || !texOn || !tex) return false;
    if (tex != g_bgCurTex) {
        g_bgCurTex = tex;
        g_bgLayer++;
        if (g_bgLayer < 32) g_bgLayerTex[g_bgLayer] = tex;
    }
    if (g_bgLayer < BackdropReplaceCount()) return true;
    if (!g_bdDone) RenderBackdropNow();   // first kept layer: our backdrop goes in just behind it
    return false;
}

static void BackdropFrameBegin() {
    g_bgLayersPrev = g_bgLayer + 1;
    g_bgLayer = -1;
    g_bgCurTex = 0;
    g_bdDone = false;
}

static void BackdropPlayfieldStart() {
    if (cfg.backdrop3D && !g_bdDone && g_phase == kWorld) RenderBackdropNow();
}


static const float kEmitThreshold = 0.02f;
// The sky-visibility rays run over a grid of screen cells, so zoomed out each cell covers more of the fort
// and the same structure comes out more occluded - forts got darker the further out you zoomed. Measured on
// three captures shrunk 2x and 3x (replay.exe ... zoom): the occlusion grows ~ (view size)^0.45. This scales
// it back by the same law, so a fort shades alike at any zoom (unchanged at the reference zoom and closer).
static float AoZoomScale() {
    float cellWorld = g_map.p * std::max(g_map.wy, 1e-4f);    // world units per lighting cell
    const float kRefCell = 4.0f * 600.0f / 2160.0f;           // 4K, 1/4 resolution lighting, a close 600-unit-high view
    return std::max(0.3f, std::min(1.0f, powf(kRefCell / cellWorld, 0.45f)));
}
static float OpenSky() { return (1.0f + cfg.skyDown) * 0.5f; }

// Fraction of all directions covered by the sun lobe used in kFsCascade, so [Sun] OpenAreaBrightening
// means the same thing whatever the sun's width.
static float SunLobeFraction(float widthDeg) {
    float c0 = cosf(widthDeg * 3.14159265f / 180.0f), c1 = c0 + (1.0f - c0) * 0.6f;
    double sum = 0;
    for (int i = 0; i < 3600; i++) {
        float x = cosf((i + 0.5f) * 6.2831853f / 3600.0f);
        float s = std::min(std::max((x - c0) / (c1 - c0), 0.0f), 1.0f);
        sum += s * s * (3.0f - 2.0f * s);
    }
    return (float)(sum / 3600.0);
}

// Ray-traced 2D lighting at working resolution:
// emission/solid mask -> jump-flood distance field -> radiance cascades -> pull/push inpainting.
static float Frac(float x) { return x - floorf(x); }

// Grid cell j covers screen px [j*p - o, (j+1)*p - o); o keeps cell edges on world multiples of p px.
static GridMap ComputeGridMap() {
    GridMap m;
    m.p = (float)g_gridP;
    m.l = g_world[0];
    m.b = g_world[2];
    m.wx = (g_world[1] - g_world[0]) / g_w;   // world units per screen px (x grows right)
    m.wy = (g_world[2] - g_world[3]) / g_h;   // world y grows down the screen
    if (m.wx > 0 && m.wy > 0) {
        m.ox = m.p * Frac(m.l / m.wx / m.p);
        m.oy = m.p * Frac(-m.b / m.wy / m.p);
    }
    return m;
}

static void RunLighting() {
    g_map = ComputeGridMap();
    bool mapOk = g_map.wx > 0 && g_map.wy > 0;

    Use(g_pGiPrep);
    U1i(g_pGiPrep, "uEmis", 0); U1i(g_pGiPrep, "uOcc", 1);
    U2f(g_pGiPrep, "uFull", (float)g_w, (float)g_h);
    U4f(g_pGiPrep, "uGrid", g_map.p, g_map.ox, g_map.oy, 0.0f);
    U2f(g_pGiPrep, "uE", cfg.giEmission * g_dn.emitMul, kEmitThreshold);
    float lpos[16 * 4] = {}, lcol[16 * 4] = {};
    int lights = CollectLights(lpos, lcol);
    G.Uniform1i(G.GetUniformLocation(g_pGiPrep, "uLightCount"), lights);
    G.Uniform4fv(G.GetUniformLocation(g_pGiPrep, "uLights"), 16, lpos);
    G.Uniform4fv(G.GetUniformLocation(g_pGiPrep, "uLightCol"), 16, lcol);
    Bind(0, g_emisTex); Bind(1, g_solidTex);
    Draw(g_giScene.fbo, g_gw, g_gh);

    Mark("gi prep");
    Use(g_pJfaSeed);
    U1i(g_pJfaSeed, "uSrc", 0);
    Bind(0, g_giScene.tex);
    Draw(g_jfa[0].fbo, g_gw, g_gh);
    Use(g_pJfaStep);
    U1i(g_pJfaStep, "uSrc", 0);
    U2f(g_pJfaStep, "uRes", (float)g_gw, (float)g_gh);
    int cur = 0, step = 1;
    while (step * 2 < std::max(g_gw, g_gh)) step *= 2;
    for (; step >= 1; step /= 2) {
        U1f(g_pJfaStep, "uStep", (float)step);
        Bind(0, g_jfa[cur].tex);
        Draw(g_jfa[cur ^ 1].fbo, g_gw, g_gh);
        cur ^= 1;
    }
    Use(g_pJfaDist);
    U1i(g_pJfaDist, "uSrc", 0);
    Bind(0, g_jfa[cur].tex);
    Draw(g_sdf.fbo, g_gw, g_gh);

    Mark("jump flood");
    // cascades from the widest down; each merges the one above it
    Use(g_pCascade);
    U1i(g_pCascade, "uGi", 0); U1i(g_pCascade, "uSdf", 1); U1i(g_pCascade, "uUpper", 2);
    U2f(g_pCascade, "uRes", (float)g_gw, (float)g_gh);
    U2f(g_pCascade, "uSky", 1.0f, cfg.skyDown);
    U1f(g_pCascade, "uOpacity", cfg.lightOpacity);
    float sl = sqrtf(g_dn.sunDir[0] * g_dn.sunDir[0] + g_dn.sunDir[1] * g_dn.sunDir[1]);
    float sx = sl > 1e-4f ? g_dn.sunDir[0] / sl : 0.0f, sy = sl > 1e-4f ? g_dn.sunDir[1] / sl : 1.0f;
    float sunLuma = 0.2126f * cfg.sunColor[0] + 0.7152f * cfg.sunColor[1] + 0.0722f * cfg.sunColor[2];
    float sunI = cfg.sunStrength * g_dn.sunMul / std::max(SunLobeFraction(cfg.sunWidth) * std::max(sunLuma, 0.05f), 1e-4f);
    sunI *= (1.0f - 0.6f * std::min(1.0f, g_wetAmt)) * (1.0f - 0.45f * std::min(1.0f, g_snowMood));   // clouds hide the sun
    sunI *= (1.0f - 0.55f * g_sandW) * (1.0f - 0.5f * g_fogW) * (1.0f - 0.5f * g_ashW);
    U4f(g_pCascade, "uSun", sx, sy, cosf(cfg.sunWidth * 3.14159265f / 180.0f), sunI);
    G.Uniform4f(G.GetUniformLocation(g_pCascade, "uSunColor"), g_dn.sunCol[0], g_dn.sunCol[1], g_dn.sunCol[2], 0.0f);
    Bind(0, g_giScene.tex); Bind(1, g_sdf.tex);
    int w = 0;
    for (int i = g_cascades - 1; i >= 0; i--) {
        U4f(g_pCascade, "uC", (float)i, (float)g_cascades, 1.0f, 0.0f);
        Bind(2, g_cascade[w ^ 1].tex);
        Draw(g_cascade[w].fbo, g_gw, g_gh);
        g_rcOut = w;
        w ^= 1;
    }

    Mark("cascades");
    // temporal: reproject last frame's light through the camera change and blend
    Use(g_pTemporal);
    U1i(g_pTemporal, "uCur", 0); U1i(g_pTemporal, "uHist", 1); U1i(g_pTemporal, "uCur2", 2); U1i(g_pTemporal, "uHist2", 3);
    U2f(g_pTemporal, "uRes", (float)g_gw, (float)g_gh);
    bool hist = g_histValid && mapOk && cfg.giTemporal > 0 && g_prevMap.p == g_map.p;
    U4f(g_pTemporal, "uNow", g_map.ox, g_map.oy, g_map.p, hist ? 1.0f : 0.0f);
    U4f(g_pTemporal, "uNowW", g_map.l, g_map.b, g_map.wx, g_map.wy);
    U4f(g_pTemporal, "uPrev", g_prevMap.ox, g_prevMap.oy, g_prevMap.p, cfg.giTemporal);
    U4f(g_pTemporal, "uPrevW", g_prevMap.l, g_prevMap.b, g_prevMap.wx, g_prevMap.wy);
    Bind(0, g_cascade[g_rcOut].tex); Bind(1, g_hist[g_histCur ^ 1].tex); Bind(2, g_cascade[g_rcOut].tex2); Bind(3, g_hist[g_histCur ^ 1].tex2);
    Draw(g_hist[g_histCur].fbo, g_gw, g_gh);
    g_lightTex = g_hist[g_histCur].tex;
    g_lightTex2 = g_hist[g_histCur].tex2;
    g_histCur ^= 1;
    g_prevMap = g_map;
    g_histValid = mapOk;

    Use(g_pPull0);
    U1i(g_pPull0, "uRC", 0); U1i(g_pPull0, "uGi", 1); U1i(g_pPull0, "uRC2", 2);
    U1f(g_pPull0, "uEmisThreshold", kEmitThreshold);
    Bind(0, g_lightTex); Bind(1, g_giScene.tex); Bind(2, g_lightTex2);
    Draw(g_pull[0].fbo, g_pull[0].w, g_pull[0].h);
    Use(g_pPull);
    U1i(g_pPull, "uA", 0); U1i(g_pPull, "uB", 1);
    for (int l = 1; l < g_pyramid; l++) {
        Bind(0, g_pull[l - 1].tex); Bind(1, g_pull[l - 1].tex2);
        Draw(g_pull[l].fbo, g_pull[l].w, g_pull[l].h);
    }
    Use(g_pPush);
    U1i(g_pPush, "uA", 0); U1i(g_pPush, "uB", 1); U1i(g_pPush, "uCA", 2); U1i(g_pPush, "uCB", 3);
    for (int l = g_pyramid - 1; l >= 0; l--) {
        U1f(g_pPush, "uCoarsest", l == g_pyramid - 1 ? 1.0f : 0.0f);
        Bind(0, g_pull[l].tex); Bind(1, g_pull[l].tex2);
        if (l < g_pyramid - 1) { Bind(2, g_push[l + 1].tex); Bind(3, g_push[l + 1].tex2); }
        Draw(g_push[l].fbo, g_push[l].w, g_push[l].h);
    }
}

static void RunDebugViews() {
    Use(g_pDebug);
    U1i(g_pDebug, "uEmis", 0); U1i(g_pDebug, "uOcc", 1); U1i(g_pDebug, "uRC", 2);
    U1i(g_pDebug, "uFillE", 3); U1i(g_pDebug, "uSdf", 4); U1i(g_pDebug, "uNorm", 5);
    U1f(g_pDebug, "uOpenSky", OpenSky());
    U4f(g_pDebug, "uGiMap", g_map.ox, g_map.oy, 1.0f / (g_map.p * g_gw), 1.0f / (g_map.p * g_gh));
    Bind(0, g_emisTex); Bind(1, g_solidTex); Bind(2, g_lightTex); Bind(3, g_push[0].tex); Bind(4, g_sdf.tex); Bind(5, g_normTex);
    for (int m = 0; m < kDebugViews; m++) {
        U1i(g_pDebug, "uMode", m);
        Draw(g_debug[m].fbo, g_debug[m].w, g_debug[m].h);
    }
    g_debugRan = true;
}

static void RunPost() {
    g_timerCount = 0;
    Mark("start");
    PerfBeginPost();
    if (CapOn()) CapLine("POST-PROCESS world -> back buffer (%dx%d), HUD follows", g_w, g_h);
    StateGuard sg;
    sg.Save();
    sg.Neutral();
    G.BindVertexArray(g_vao);
    float tx = 1.0f / g_w, ty = 1.0f / g_h;

    if (g_bridge) g_dn = DayNight();   // a night with no lights in it would only be dark
    else {
        UpdateDayNight();
        ProcessEvents();
        BattleMarks();
        for (GLuint tex : g_probeQueue) if (WeatherKind(tex) == 0) ProbeWeatherTexture(tex);
        g_probeQueue.clear();
        UpdateWeather();
    }
    static Target raw;   // lab wipe: the game's own colours, before any of our effects are drawn in
    if (g_lab.wipe >= 0) {
        if (raw.w != g_w || raw.h != g_h) MakeTarget(raw, g_w, g_h, GL_RGBA8);
        Use(g_pCopy);
        U1i(g_pCopy, "uSrc", 0);
        U1i(g_pCopy, "uAlpha", 0);
        Bind(0, g_sceneTex);
        Draw(raw.fbo, g_w, g_h);
    }
    if (!g_bridge) {
        RunWorldFx();
        RunSnow();
        DrawBolts();
    }
    Mark("world fx");
    bool lit = !g_bridge && cfg.lighting && g_pyramid > 0;
    if (lit) { RunLighting(); Mark("lighting"); }
    else g_histValid = false;
    if (lit && CapOn()) RunDebugViews();

    // bloom: prefilter -> downsample chain -> additive upsample back to level 0
    Use(g_pPrefilter);
    U1i(g_pPrefilter, "uEmis", 0); U1i(g_pPrefilter, "uScene", 1);
    U2f(g_pPrefilter, "uTexel", tx, ty);
    U4f(g_pPrefilter, "uP", cfg.bloomEmissive, cfg.bloomScene, cfg.bloomThreshold, cfg.bloomKnee);
    Bind(0, g_emisTex); Bind(1, g_sceneTex);
    Draw(g_bloom[0].fbo, g_bloom[0].w, g_bloom[0].h);

    Use(g_pDown);
    U1i(g_pDown, "uSrc", 0);
    for (int i = 1; i < g_bloomLevels; i++) {
        U2f(g_pDown, "uTexel", 1.0f / g_bloom[i - 1].w, 1.0f / g_bloom[i - 1].h);
        Bind(0, g_bloom[i - 1].tex);
        Draw(g_bloom[i].fbo, g_bloom[i].w, g_bloom[i].h);
    }

    if (g_pBloomAvg && g_bloomLevels > 0) {   // how much of the screen glows, for auto-dim
        int nx = g_bloomAvgCur ^ 1;
        Use(g_pBloomAvg);
        U1i(g_pBloomAvg, "uSrc", 0); U1i(g_pBloomAvg, "uPrev", 1);
        float dt = std::max(g_fxDt, 1e-3f);
        U2f(g_pBloomAvg, "uK", 1.0f - expf(-dt / 0.15f), 1.0f - expf(-dt / 0.8f));
        Bind(0, g_bloom[g_bloomLevels - 1].tex); Bind(1, g_bloomAvg[g_bloomAvgCur].tex);
        Draw(g_bloomAvg[nx].fbo, 1, 1);
        g_bloomAvgCur = nx;
    }

    Use(g_pUp);
    U1i(g_pUp, "uSrc", 0);
    U1f(g_pUp, "uGain", std::min(std::max(cfg.bloomSpread, 0.05f), 1.0f));
    G.Enable(GL_BLEND);
    G.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    G.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    for (int i = g_bloomLevels - 1; i > 0; i--) {
        U2f(g_pUp, "uTexel", cfg.bloomRadius / g_bloom[i].w, cfg.bloomRadius / g_bloom[i].h);
        Bind(0, g_bloom[i].tex);
        Draw(g_bloom[i - 1].fbo, g_bloom[i - 1].w, g_bloom[i - 1].h);
    }
    G.Disable(GL_BLEND);
    Mark("bloom");

    // backdrop-only blur pyramid (depth softening + local backdrop colour for the fog)
    Use(g_pBgDown);
    U1i(g_pBgDown, "uSrc", 0); U1i(g_pBgDown, "uMask", 1);
    U1i(g_pBgDown, "uFirst", 1);
    U2f(g_pBgDown, "uTexel", tx, ty);
    Bind(0, g_sceneTex); Bind(1, g_emisTex);
    Draw(g_bgBlur[0].fbo, g_bgBlur[0].w, g_bgBlur[0].h);
    U1i(g_pBgDown, "uFirst", 0);
    U2f(g_pBgDown, "uTexel", 1.0f / g_bgBlur[0].w, 1.0f / g_bgBlur[0].h);
    Bind(0, g_bgBlur[0].tex);
    Draw(g_bgBlur[1].fbo, g_bgBlur[1].w, g_bgBlur[1].h);
    Mark("backdrop blur");

    // lighting + fog + composite + tonemap + grade -> LDR
    int fogLvl = std::min(std::max(cfg.fogBlurLevel, 0), std::max(g_pyramid - 1, 0));
    float secs = (float)(GetTickCount64() % 3600000ull) / 1000.0f;
    Use(g_pComposite);
    const char* samplers[] = { "uScene", "uBloom", "uEmis", "uOcc", "uRC", "uFillE", "uFillV", "uFogE", "uFogV", "uRC2", "uNorm", "uBgBlur" };
    for (int i = 0; i < 12; i++) U1i(g_pComposite, samplers[i], i);
    bool mats = cfg.materials && g_pWorld;
    U4f(g_pComposite, "uM1", cfg.lightHeight, mats ? cfg.toon : 0.0f, mats ? cfg.hemi : 0.0f, cfg.ambient * g_dn.ambMul);
    U4f(g_pComposite, "uDN", g_dn.bgTint[0], g_dn.bgTint[1], g_dn.bgTint[2], g_dn.day);
    U4f(g_pComposite, "uM2", mats ? cfg.specular : 0.0f, cfg.shininess, cfg.edgeBevel, cfg.edgeRadius);
    U2f(g_pComposite, "uFull", (float)g_w, (float)g_h);
    float bloomNorm = 0, wgt = 1;   // level k adds spread^k: normalise so a lone light keeps its brightness
    for (int i = 0; i < std::max(g_bloomLevels, 1); i++) { bloomNorm += wgt; wgt *= std::min(std::max(cfg.bloomSpread, 0.05f), 1.0f); }
    U4f(g_pComposite, "uA", cfg.exposure * g_dn.expoMul, cfg.knee, 1.0f / bloomNorm, lit ? 1.0f : 0.0f);
    U4f(g_pComposite, "uBl", cfg.bloomStrength, cfg.bloomLimit, cfg.bloomAutoDim, 0);
    U4f(g_pComposite, "uG1", cfg.contrast, cfg.saturation, cfg.vibrance, cfg.temperature + g_dn.tempAdd);
    U4f(g_pComposite, "uG2", cfg.tint, cfg.lift, cfg.gamma, cfg.gain);
    U4f(g_pComposite, "uL", cfg.giIntensity, cfg.ao * AoZoomScale(), OpenSky(), cfg.bgLight);
    U4f(g_pComposite, "uWorld", g_world[0], g_world[1], g_world[2], g_world[3]);
    // fog banks weather: thick drifting low fog, faded in over the user's fog settings
    float fw = g_fogW;
    auto fmix = [&](float a, float b) { return a + (b - a) * fw; };
    U4f(g_pComposite, "uF1", fmix(cfg.fog ? cfg.fogDensity : 0.0f, 1.6f), fmix(cfg.fogHaze, 0.18f), fmix(cfg.fogBanks, 0.45f), fmix(cfg.fogPool, 0.7f));
    U4f(g_pComposite, "uF4", cfg.fogStartY, cfg.fogEndY, cfg.fogHeight, lit ? cfg.bgBlur : 0.0f);
    U4f(g_pComposite, "uF2", cfg.fogFront, cfg.fogAmbient, fmix(cfg.fogLight, 1.6f), cfg.fogNoiseScale);
    U4f(g_pComposite, "uF3", fmix(cfg.fogColor[0], 0.64f), fmix(cfg.fogColor[1], 0.67f), fmix(cfg.fogColor[2], 0.72f), fmix(cfg.fogWind, 70.0f));
    U4f(g_pComposite, "uGiMap", g_map.ox, g_map.oy, 1.0f / (g_map.p * g_gw), 1.0f / (g_map.p * g_gh));
    SetWeatherUniforms(g_pComposite);
    U1f(g_pComposite, "uTime", secs);
    Bind(0, g_sceneTex); Bind(1, g_bloom[0].tex); Bind(2, g_emisTex); Bind(3, g_solidTex);
    Bind(4, g_lightTex ? g_lightTex : g_cascade[0].tex); Bind(5, g_push[0].tex); Bind(6, g_push[0].tex2);
    Bind(7, g_push[fogLvl].tex); Bind(8, g_push[fogLvl].tex2); Bind(9, g_lightTex2 ? g_lightTex2 : g_cascade[0].tex2); Bind(10, g_normTex); Bind(11, g_bgBlur[1].tex);
    bool snowOn = g_snowLive && g_snowM.fbo;
    Bind(12, snowOn ? g_snowM.tex : g_sceneTex);
    U1i(g_pComposite, "uSnowMap", 12);
    U4f(g_pComposite, "uSnowL", snowOn ? 1.0f : 0.0f, 0, 0, 0);
    U4f(g_pComposite, "uSnowR", g_snowRegion[0], g_snowRegion[1], g_snowRegion[2], g_snowRegion[3]);
    Bind(13, g_terrainM.fbo ? g_terrainM.tex : g_sceneTex);
    Bind(14, g_bloomAvg[g_bloomAvgCur].fbo ? g_bloomAvg[g_bloomAvgCur].tex : g_sceneTex);
    U1i(g_pComposite, "uBloomAvg", 14);
    U1i(g_pComposite, "uTerrainM", 13);
    bool waterOn = WaterOn();
    U4f(g_pComposite, "uWater", waterOn ? 1.0f : 0.0f, g_waterLevel, g_fxNow, cfg.waterWaves);
    U4f(g_pComposite, "uWater2", cfg.waterRefl, cfg.waterFoam, g_waterForced ? 1.0f : 0.0f, 0);
    G.Uniform4fv(G.GetUniformLocation(g_pComposite, "uWaterH"), kWaterCols / 4, g_waterProf);
    Draw(g_ldr.fbo, g_w, g_h);
    Mark("composite");

    GLuint finalSrc = g_ldr.tex;
    if (cfg.fxaa) {
        Use(g_pFxaa);
        U1i(g_pFxaa, "uSrc", 0);
        U2f(g_pFxaa, "uTexel", tx, ty);
        Bind(0, g_ldr.tex);
        Draw(g_ldr2.fbo, g_w, g_h);
        finalSrc = g_ldr2.tex;
    }

    // photo mode depth of field: a soft copy of the image the final pass blends in by depth
    static Target dofA, dofB;
    bool dofOn = g_lab.dof > 0.001f && g_pBlur;
    if (dofOn) {
        int dw = std::max(g_w / 2, 1), dh = std::max(g_h / 2, 1);
        if (dofA.w != dw || dofA.h != dh) { FreeTarget(dofA); FreeTarget(dofB); MakeTarget(dofA, dw, dh, GL_RGBA8); MakeTarget(dofB, dw, dh, GL_RGBA8); }
        Use(g_pBlur);
        U1i(g_pBlur, "uSrc", 0);
        float rad = 1.0f + 3.0f * std::min(g_lab.dof, 1.0f);
        U2f(g_pBlur, "uDir", rad / dw, 0); Bind(0, finalSrc); Draw(dofA.fbo, dw, dh);
        U2f(g_pBlur, "uDir", 0, rad / dh); Bind(0, dofA.tex); Draw(dofB.fbo, dw, dh);
        U2f(g_pBlur, "uDir", rad * 0.7f / dw, rad * 0.7f / dh); Bind(0, dofB.tex); Draw(dofA.fbo, dw, dh);
    }

    // sharpen / vignette / grain -> back buffer
    Use(g_pFinal);
    U1i(g_pFinal, "uSrc", 0);
    U2f(g_pFinal, "uTexel", tx, ty);
    U4f(g_pFinal, "uF", cfg.sharpen, cfg.vignette, cfg.grain, cfg.chroma);
    U1f(g_pFinal, "uTime", g_fxNow);
    U1i(g_pFinal, "uHeat", 1); U1i(g_pFinal, "uOcc", 2); U1i(g_pFinal, "uEmis", 3); U1i(g_pFinal, "uRaw", 4);
    SetFinalFxUniforms(g_pFinal);
    U4f(g_pFinal, "uLook", cfg.splitTone, cfg.ink, cfg.posterize, g_lab.wipe >= 0 && raw.fbo ? g_lab.wipe : -1.0f);
    U4f(g_pFinal, "uShT", cfg.shadowTint[0], cfg.shadowTint[1], cfg.shadowTint[2], 0);
    U4f(g_pFinal, "uHiT", cfg.highTint[0], cfg.highTint[1], cfg.highTint[2], 0);
    Bind(4, raw.fbo ? raw.tex : g_sceneTex);
    Bind(5, dofOn ? dofA.tex : finalSrc);
    U1i(g_pFinal, "uDof", 5);
    U4f(g_pFinal, "uDofP", dofOn ? g_lab.dof : 0.0f, g_lab.focusX, g_lab.focusY, g_lab.tilt);
    U4f(g_pFinal, "uFrame", g_lab.frameAspect, 0, 0, 0);
    Bind(1, g_bloomLevels > 1 ? g_bloom[1].tex : g_bloom[0].tex); Bind(2, g_solidTex); Bind(3, g_emisTex);
    Bind(0, finalSrc);
    Draw(g_outFbo, g_w, g_h);
    if (!g_bridge) DrawFunOverlay();
    Mark("fxaa+final");

    if (g_debugView > 0) {   // control panel: show one buffer full screen
        if (lit && g_debugView <= kDebugViews) RunDebugViews();
        GLuint src = g_debugView <= kDebugViews ? (lit ? g_debug[g_debugView - 1].tex : 0) : g_debugView == 6 ? g_emisTex : g_sceneTex;
        if (src) {
            Use(g_pCopy);
            U1i(g_pCopy, "uSrc", 0);
            Bind(0, src);
            Draw(g_outFbo, g_w, g_h);
        }
    }
    PerfEndPost();
    sg.Restore(true);
    G.UseProgram(0);      // the HUD draws with fixed-function again
    g_worldBound = false;
    g_phase = kHud;
    g_postRan = true;
    if (g_lab.hideHud) {  // lab: the HUD and cursor draw nothing until the frame ends
        G.ColorMaski(0, 0, 0, 0, 0);
        g_hudMasked = true;
    }
}

// ---------------------------------------------------------------- per-frame hooks

static bool OurWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void PollKeys() {
    static bool prevPost, prevLight;
    bool focused = OurWindowFocused();
    bool post = focused && !OverlayKeyBlocked(cfg.toggleKey) && (GetAsyncKeyState(cfg.toggleKey) & 0x8000) != 0;
    bool light = focused && !OverlayKeyBlocked(cfg.lightingKey) && (GetAsyncKeyState(cfg.lightingKey) & 0x8000) != 0;
    if (post && !prevPost) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            GfxReloadSettings();
        } else {
            cfg.enabled = !cfg.enabled;
            SessionLog("gfx: post-processing %s", cfg.enabled ? "ON" : "OFF");
        }
    }
    if (light && !prevLight) {
        cfg.lighting = !cfg.lighting;
        SessionLog("gfx: lighting + fog %s", cfg.lighting ? "ON" : "OFF");
    }
    prevPost = post;
    prevLight = light;
}

static bool g_masksOff;

static void ClearMasks() {
    static const GLfloat zero[4] = { 0, 0, 0, 0 }, flat[4] = { 0.5f, 0.5f, 0, 0 };
    for (GLuint i = 1; i <= 3; i++) G.ColorMaski(i, 1, 1, 1, 1);
    G.ClearBufferfv(GL_COLOR, 1, zero);
    G.ClearBufferfv(GL_COLOR, 2, zero);
    G.ClearBufferfv(GL_COLOR, 3, flat);
    g_masksOff = false;
}

void GfxFrameBegin() {
    PerfFrame();
    g_frameNo++;
    g_postRan = false;
    g_debugRan = false;
    g_projHud = false;
    g_playfield = false;
    g_worldBound = false;
    g_drawSeq = 0;
    g_phase = kOff;
    if (!g_procsLoaded) {
        if (!R.wglGetCurrentContext || !R.wglGetCurrentContext()) return;
        g_procsLoaded = true;
        LoadConfig();
        if (!LoadProcs()) { g_failed = true; SessionLog("gfx: required GL entry points missing - disabled"); }
    }
    PollKeys();
    if (g_failed || !cfg.enabled) return;
    RECT rc{};
    HWND wnd = WindowFromDC(R.wglGetCurrentDC());
    if (!wnd || !GetClientRect(wnd, &rc)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (!g_ready || w != g_w || h != g_h || cfg.giScale != g_giScaleBuilt) {
        if (!CreateResources(w, h)) { g_failed = true; return; }
    }
    G.BindFramebuffer(GL_FRAMEBUFFER, g_worldFbo);
    ClearMasks();
    BackdropFrameBegin();
    g_phase = kWorld;
}

void GfxFrameEnd() {
    if (g_phase == kWorld) RunPost();   // no HUD this frame (menus, loading)
    if (g_hudMasked) { G.ColorMaski(0, 1, 1, 1, 1); g_hudMasked = false; }
    g_phase = kOff;
}

struct DrawClass { bool emit, excluded, solid, effect, alphaFmt, soft, beam, bg, proj; };

static DrawClass ClassifyDraw(GLint tex, bool texOn, bool blend, GLint dst) {
    DrawClass d{};
    if (texOn && tex) {
        std::lock_guard<std::mutex> lk(g_emitMx);
        d.emit = g_emitTex.count(tex) != 0;
        d.excluded = g_excludedTex.count(tex) != 0;
        d.solid = g_solidSet.count(tex) != 0;
        d.effect = g_effectSet.count(tex) != 0;
        d.alphaFmt = g_alphaFmtSet.count(tex) != 0;
        d.soft = g_softGlowSet.count(tex) != 0;
        d.beam = g_beamSet.count(tex) != 0;
        d.bg = g_bgSet.count(tex) != 0;
        d.proj = g_projSet.count(tex) != 0;
    }
    if (d.proj && cfg.gameFx && cfg.gfInFlight) { d.solid = true; d.bg = true; }   // blocks light, but nothing lands on it
    if (cfg.additiveEmits && blend && dst == GL_ONE && !d.excluded) d.emit = true;
    return d;
}

// Phase 3 path: our material shader draws instead of fixed-function. It writes the game's exact
// colour to target 0 and computes emission/coverage/normals itself, so every target simply follows
// the game's own blend state - no per-target blend overrides needed.
static void WorldShaderDraw(bool displayList) {
    GLint tex = 0, dst = 0, env = GL_MODULATE, shade = 0, afunc = 0;
    GLfloat aref = 0;
    R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    bool texOn = R.IsEnabled(GL_TEXTURE_2D) != 0 && (tex != 0 || displayList);
    bool blend = R.IsEnabled(GL_BLEND) != 0;
    R.GetIntegerv(GL_BLEND_DST_RGB, &dst);
    bool atest = R.IsEnabled(GL_ALPHA_TEST) != 0;
    if (atest) { R.GetIntegerv(GL_ALPHA_TEST_FUNC, &afunc); R.GetFloatv(GL_ALPHA_TEST_REF, &aref); }
    R.GetIntegerv(GL_SHADE_MODEL, &shade);
    if (texOn) G.GetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
    int envCode = !texOn ? 0 : env == GL_MODULATE ? 1 : env == GL_REPLACE ? 2 : env == GL_ADD ? 3 : env == GL_DECAL ? 4 : -1;
    if (envCode < 0) {
        // A texture mode we don't reproduce (e.g. GL_COMBINE): let fixed-function draw it and keep
        // it out of our targets.
        if (g_worldBound) { G.UseProgram(0); g_worldBound = false; }
        for (GLuint i = 1; i <= 3; i++) G.ColorMaski(i, 0, 0, 0, 0);
        g_masksOff = true;
        return;
    }
    if (g_masksOff) {
        for (GLuint i = 1; i <= 3; i++) G.ColorMaski(i, 1, 1, 1, 1);
        g_masksOff = false;
    }
    if (!g_worldBound) {
        G.UseProgram(g_pWorld);
        G.Uniform1i(g_locTex, 0);
        G.Uniform4f(g_locMat, cfg.bevel, cfg.bevelPx * g_h / 2160.0f, cfg.detail, cfg.specMask);   // bevel defined at 4K
        g_worldBound = true;
    }
    DrawClass d = ClassifyDraw(tex, texOn, blend, dst);
    if (displayList) { d = DrawClass{}; d.solid = true; if (g_playfield) NoteTerrainList(g_callListId); }   // terrain
    bool pf = g_playfield;
    bool normals = pf && texOn && !d.emit && !d.effect;
    uint8_t ev = (!displayList && texOn && pf) ? EventClassOf(tex) : (uint8_t)kEvNone;
    // The game's particle system draws GL_QUADS, and its sparks use the sniper_flash09 texture: a quad
    // with a flash or spark texture is a particle, not a muzzle flash. (ExplosionLarge and fireball puffs
    // are particles too but stay events: explosions keep their effects.)
    // The same flash textures serve the sniper's muzzle flash (a sprite) and the sparks of fires and
    // impacts (particles), so a particle with one glows softly: no bloom burst when something catches fire.
    bool particleSpark = (ev == kEvFlash || ev == kEvSpark) && g_beginMode == 0x0007 /* GL_QUADS */;
    if (particleSpark) {
        FxStat("flash/spark", tex, "a game particle (quads): soft glow only");
        CapHistNote(Fmt("sprite %-10s %-48s game particle (quad) -> ignored, soft glow", EvName(ev), TexShort(tex).c_str()));
        ev = kEvNone;
    }
    g_trackPuff = 0;
    if (ev == kEvSteam || ev == kEvSmoke) {   // which puff replaces it; the newer replacements only with the switch on
        bool extra = false;
        PuffOf(tex, g_trackPuff, extra);
        if (extra && !cfg.moreSimFx) { ev = kEvNone; g_trackPuff = 0; }
    }
    bool hidden = ev >= kEvBridgeLightning;        // mod-bridge marker sprites are never shown
    if ((ev == kEvSteam || ev == kEvSmoke) && cfg.simSmoke && cfg.impactFx) hidden = true;   // replaced by simulated puffs
    if (ev != kEvNone && (cfg.impactFx || ev == kEvBridgeCapture || ev == kEvBridgeTour)) { g_trackTex = tex; BeginTrack(ev); }
    bool triMesh = g_beginMode == 0x0004 /* GL_TRIANGLES */;
    if (pf && texOn && !displayList && ev == kEvNone && !d.solid && !d.emit && triMesh && cfg.water) {
        for (float& t : g_probeTop) t = -1e30f;      // maybe the sea: read its surface from the vertices
        g_trackTex = tex; BeginTrack(kEvWaterProbe);
    }
    if (pf && texOn && !displayList && ev == kEvNone && !d.solid && !d.emit && !triMesh && IsLargeTex(tex)) {
        int kind = WeatherKind(tex);
        if (kind == 0 && cfg.impactFx) { g_trackTex = tex; BeginTrack(kEvWeatherProbe); }
        if (kind == 1) {
            g_rainSeenFrame = g_fxFrame;
            if (cfg.replaceRain && cfg.weatherMode != 1) hidden = true;
        }
    }
    if (BackdropDraw(tex, texOn, displayList)) hidden = true;
    if (g_anyWaterPart && texOn) {   // diagnostics: how each water cannon texture is drawn, once
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(g_emitMx);
            first = g_waterPartTex.count(tex) && g_waterPartDrawn.insert(tex).second;
        }
        if (first) SessionLog("fun: water cannon texture %u drawn: as %s, playfield %d, display list %d, primitive %#x", tex, EvName(ev), pf ? 1 : 0, displayList ? 1 : 0, g_beginMode);
    }
    if (ev == kEvMilk && (cfg.funMiko || cfg.waterStream) && cfg.impactFx) hidden = true;   // the stream (water or the miko's milk) replaces the drops
    bool swapped = false;
    if (cfg.funMiko && g_funState == 2 && texOn) {
        if (ev == kEvMiko || ev == kEvMikoGun) {
            GLuint rep = MikoSwapFor(tex, ev);
            if (rep) { G.BindTexture(GL_TEXTURE_2D, rep); g_texRestore = tex; swapped = true; }
        }
    }
    if (hidden) { atest = true; afunc = GL_NEVER; }
    G.Uniform4i(g_locMode, envCode, shade == GL_FLAT ? 1 : 0, atest ? (afunc - GL_NEVER + 1) : 0, d.alphaFmt && !swapped ? 1 : 0);
    G.Uniform4i(g_locClass, pf ? 1 : 0, d.solid ? 1 : 0, d.emit ? 1 : 0, normals ? 1 : 0);
    // Effect glow (the effects preset) is for blasts and flashes; burning structures - dozens of flame
    // sprites and sparks each - have their own, gentler fire glow.
    uint8_t evc = (!displayList && texOn && pf) ? EventClassOf(tex) : (uint8_t)kEvNone;
    g_drawSeq++;
    if (evc == kEvFire && g_fireDraws.size() < 4096) g_fireDraws.push_back(g_drawSeq);
    float emitW = d.emit && (d.soft || particleSpark) ? cfg.softGlow : 1.0f;
    if (evc == kEvFire) emitW *= cfg.fireGlow;
    else if (d.effect && !particleSpark && (evc == kEvExplosion || evc == kEvFlash || evc >= kEvBridgeLightning)) emitW *= EffectGlowScale();
    G.Uniform4f(g_locAlpha, aref, emitW, 0, 0);
    // w: coverage counts as full wherever it is drawn - terrain, and the fort's back wall (its planks can be
    // drawn partly transparent, and then fog, snow and light treated the inside of a fort as open air)
    G.Uniform4f(g_locFx, d.beam && pf ? cfg.beamGlow : 0.0f, g_fxNow, (displayList || (d.solid && !d.bg)) ? 1.0f : 0.0f,
                (displayList || (d.solid && d.bg)) ? 1.0f : 0.0f);
}

// Phase 2 path (materials off / shader unavailable): fixed-function broadcasts one colour to every
// target, so per-target blend state turns it into the masks. Target 0 is never touched.
//   target 1 rgb: emitters add their colour, everything else darkens by its alpha
//   target 1 a  : playfield coverage (everything drawn after the background)
//   target 2 a  : solid coverage (terrain lists + materials/devices/weapons textures)
static void FixedFunctionDraw(bool displayList) {
    GLboolean pf = g_playfield ? 1 : 0;
    G.ColorMaski(3, 0, 0, 0, 0);
    if (displayList) {
        // Lists set their own blend func, which reaches every target. Its alpha result is roughly
        // coverage, so let terrain write the coverage masks but keep its colour out of emissive.
        G.ColorMaski(1, 0, 0, 0, pf);
        G.ColorMaski(2, 0, 0, 0, pf);
        return;
    }
    GLint tex = 0, dst = 0;
    R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    bool texOn = R.IsEnabled(GL_TEXTURE_2D) != 0;
    bool blend = R.IsEnabled(GL_BLEND) != 0;
    R.GetIntegerv(GL_BLEND_DST_RGB, &dst);
    DrawClass d = ClassifyDraw(tex, texOn, blend, dst);

    GLenum aSrc = pf ? GL_ONE : GL_ZERO, aDst = pf ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE;
    G.ColorMaski(1, 1, 1, 1, 1);
    G.Enablei(GL_BLEND, 1);
    if (d.emit) {
        GLint src = GL_ONE;
        if (blend) R.GetIntegerv(GL_BLEND_SRC_RGB, &src);
        G.BlendFuncSeparatei(1, blend ? src : GL_ONE, blend ? dst : GL_ZERO, aSrc, aDst);
    } else {
        G.BlendFuncSeparatei(1, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, aSrc, aDst);
    }
    G.BlendEquationi(1, GL_FUNC_ADD);

    if (pf && d.solid && !d.emit) {
        G.ColorMaski(2, 0, 0, 0, 1);
        G.Enablei(GL_BLEND, 2);
        G.BlendFuncSeparatei(2, GL_ZERO, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        G.BlendEquationi(2, GL_FUNC_ADD);
    } else {
        G.ColorMaski(2, 0, 0, 0, 0);
    }
}

static void StartPlayfield() {
    if (cfg.materials && g_pWorld) BackdropPlayfieldStart();   // every layer was replaced
    g_playfield = true;
}

void GfxBeforeDraw(bool displayList) {
    if (g_phase != kWorld) return;
    if (g_projHud) { RunPost(); return; }
    // The backdrop is all 4-vertex strips. The first other draw (a terrain list, quads, triangles) is the
    // playfield even before the game loads an object matrix: some maps draw the terrain, the sea or a
    // fort's back wall first, and as backdrop they would get no solid mask and the distance haze.
    if (!g_playfield && (displayList || g_beginMode != 0x0005 /* GL_TRIANGLE_STRIP */)) StartPlayfield();
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    if (cfg.materials && g_pWorld) WorldShaderDraw(displayList);
    else FixedFunctionDraw(displayList);
    QueryPerformanceCounter(&b);
    if (P.freq.QuadPart) P.drawMsFrame += QpcMs(a, b);
}

static bool IsHudOrtho(double l, double r, double b, double t) {
    return fabs(l) < 1e-3 && fabs(t) < 1e-3 && r > 1 && fabs(b - cfg.hudHeight) < cfg.hudHeight * 0.1;
}

void GfxOnOrtho(double l, double r, double b, double t) {
    if (g_phase != kWorld) return;
    GLint mm = 0;
    R.GetIntegerv(GL_MATRIX_MODE, &mm);
    if (mm != GL_PROJECTION) return;
    g_projHud = IsHudOrtho(l, r, b, t);
    if (!g_projHud) { g_world[0] = (float)l; g_world[1] = (float)r; g_world[2] = (float)b; g_world[3] = (float)t; }
}

// The background (sky, parallax layers) is placed with translate/scale; the first object placed
// with glLoadMatrixf on the modelview stack starts the playfield.
void GfxOnLoadMatrix() {
    if (g_phase != kWorld) return;
    GLint mm = 0;
    R.GetIntegerv(GL_MATRIX_MODE, &mm);
    if (mm == GL_PROJECTION) g_projHud = false;
    else if (mm == GL_MODELVIEW && !g_playfield) StartPlayfield();
}

void GfxAfterClear(GLbitfield mask) {
    if (g_phase == kWorld && (mask & GL_COLOR_BUFFER_BIT)) ClearMasks();
}

GLuint GfxRedirectFramebuffer(GLuint fb) {
    return (g_phase == kWorld && fb == 0) ? g_worldFbo : fb;
}

void GfxDebugTargets(std::vector<std::pair<GLuint, std::string>>& out) {
    if (!g_postRan) return;
    out.emplace_back(g_sceneTex, "post_in_scene");
    out.emplace_back(g_emisTex, "post_in_emissive");
    if (g_bloomLevels) out.emplace_back(g_bloom[0].tex, "post_bloom");
    if (g_debugRan) {
        const char* names[kDebugViews] = { "gi_masks", "gi_light", "gi_sky", "gi_distance", "gi_normals" };
        for (int i = 0; i < kDebugViews; i++) out.emplace_back(g_debug[i].tex, names[i]);
    }
}

// ---------------------------------------------------------------- Vulkan bridge (vulkan.cpp)

// Forts' Vulkan renderer: vulkan.cpp shares each finished frame with its own GL context and calls this, with `inTex`
// the frame and `outFbo` where the result goes (both the right way up for GL). Until the Vulkan draws are read - where
// the playfield, solids and lights are, and where the world is - only the image stages run: the look, bloom from the
// bright parts, grade, AA, sharpening, vignette and grain. Returns false when FortsGFX is off (F9) or can't run.
bool GfxBridgeFrame(GLuint inTex, GLuint outFbo, int w, int h) {
    if (!g_procsLoaded) {
        g_procsLoaded = true;
        LoadConfig();
        if (!LoadProcs()) { g_failed = true; SessionLog("gfx: required GL entry points missing - disabled"); }
    }
    PerfFrame();
    g_frameNo++;
    PollKeys();
    if (g_failed || !cfg.enabled || w <= 0 || h <= 0) return false;
    if (!g_ready || w != g_w || h != g_h || cfg.giScale != g_giScaleBuilt) {
        if (!CreateResources(w, h)) { g_failed = true; return false; }
    }
    // the frame becomes the world image: playfield everywhere, nothing solid or glowing yet
    G.BindVertexArray(g_vao);
    Use(g_pCopy);
    U1i(g_pCopy, "uSrc", 0);
    U1i(g_pCopy, "uAlpha", 0);
    Bind(0, inTex);
    Draw(g_worldFbo, w, h);
    G.BindFramebuffer(GL_FRAMEBUFFER, g_worldFbo);
    static const GLfloat playfield[4] = { 0, 0, 0, 1 }, zero[4] = { 0, 0, 0, 0 }, flat[4] = { 0.5f, 0.5f, 0, 0 };
    G.ClearBufferfv(GL_COLOR, 1, playfield);
    G.ClearBufferfv(GL_COLOR, 2, zero);
    G.ClearBufferfv(GL_COLOR, 3, flat);
    g_world[0] = 0; g_world[1] = (float)w; g_world[2] = 0; g_world[3] = (float)h;
    bool fog = cfg.fog, mats = cfg.materials;
    cfg.fog = false;          // both need the world: fog its height, materials the sprites' own normals
    cfg.materials = false;
    g_bridge = true;
    g_outFbo = outFbo;
    g_phase = kWorld;
    RunPost();
    g_outFbo = 0;
    g_bridge = false;
    g_phase = kOff;
    cfg.fog = fog;
    cfg.materials = mats;
    if (g_hudMasked) { G.ColorMaski(0, 1, 1, 1, 1); g_hudMasked = false; }
    return true;
}

// ---------------------------------------------------------------- offline replay (tools/replay.cpp)

bool GfxReplaySetup(int w, int h) {
    LoadConfig();
    if (!LoadProcs()) return false;
    g_procsLoaded = true;
    return CreateResources(w, h);
}

void GfxReplayTargets(GLuint out[4]) {
    out[0] = g_sceneTex; out[1] = g_emisTex; out[2] = g_solidTex; out[3] = g_normTex;
}

// Runs the post chain `frames` times (so temporal smoothing converges) into outFbo.
void GfxReplayRun(const float world[4], GLuint outFbo, bool lighting, int frames) {
    for (int i = 0; i < 4; i++) g_world[i] = world[i];
    g_outFbo = outFbo;
    cfg.lighting = lighting;
    g_histValid = false;
    for (int i = 0; i < frames; i++) { g_frameNo++; RunPost(); }
    g_outFbo = 0;
}

// Prints the GPU time of each stage of the last RunPost (replay tool only).
void GfxReplayProfile(bool on) { g_profile = on; }
void GfxReplayPrintProfile() {
    if (!g_profile || g_timerCount < 2) return;
    unsigned long long t[16] = {};
    for (int i = 0; i < g_timerCount; i++) G.GetQueryObjectui64v(g_timers[i], 0x8866, &t[i]);   // GL_QUERY_RESULT
    for (int i = 1; i < g_timerCount; i++) printf("    %-14s %6.2f ms\n", g_timerNames[i], (t[i] - t[i - 1]) / 1e6);
}

GfxStats GfxGetStats() { return { (float)P.lastFrame, (float)P.lastGpu, (float)P.lastDraw }; }
int GfxOverlayKey() { return cfg.overlayKey; }
void GfxLookKeys(int& prev, int& next) { prev = cfg.lookPrevKey; next = cfg.lookNextKey; }
int GfxLabKey() { return cfg.labKey; }
int GfxPhotoKey() { return cfg.photoKey; }

// replay tool: simulated clock, injected events and weather
void GfxReplaySetTime(double t) { g_timeOverride = t; }
void GfxReplayTimeOfDay(float h) { g_tod = h; g_todPaused = true; }
void GfxReplayWater(bool on, float level) {
    g_waterForced = true; g_hasWater = on; g_waterLevel = level;
    for (float& w : g_waterProf) w = level;
}
// replay tool: set any float/bool setting by "Section.Key"
// Replay tool: live particles of each type 0..21; [22] debris and splinters that took a surface's colour, [23] grit that did.
void GfxReplayParticleCounts(int* counts) {
    std::vector<float> pos((size_t)kMaxParticles * 4), vel((size_t)kMaxParticles * 4);
    static GLuint readFbo;
    if (!readFbo) G.GenFramebuffers(1, &readFbo);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    G.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_parts[g_partCur].tex, 0);
    R.ReadBuffer(GL_COLOR_ATTACHMENT0);
    R.ReadPixels(0, 0, kPW, kPH, GL_RGBA, 0x1406 /* GL_FLOAT */, pos.data());
    G.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_parts[g_partCur].tex2, 0);
    R.ReadPixels(0, 0, kPW, kPH, GL_RGBA, 0x1406, vel.data());
    for (int i = 0; i < 24; i++) counts[i] = 0;
    for (int i = 0; i < kMaxParticles; i++) {
        if (pos[i * 4 + 3] <= 0) continue;
        int t = (int)(vel[i * 4 + 3] + 0.5f);
        if (t >= 0 && t < 22) counts[t]++;
        if ((t == 1 || t == 13) && vel[i * 4 + 2] > 0.5f) counts[22]++;
        if (t == 18 && vel[i * 4 + 2] > 0.5f) counts[23]++;
    }
}
float GfxReplayGet(const char* id) {
    for (auto& s : g_settings)
        if (std::string(s.section) + "." + s.key == id) {
            if (s.type == GfxSetting::Float) return *(float*)s.ptr;
            if (s.type == GfxSetting::Bool) return *(bool*)s.ptr ? 1.0f : 0.0f;
            if (s.type == GfxSetting::Int) return (float)*(int*)s.ptr;
        }
    return 0.0f;
}
void GfxReplayTune(const char* id, float v) {
    for (auto& s : g_settings)
        if (std::string(s.section) + "." + s.key == id) {
            if (s.type == GfxSetting::Float) *(float*)s.ptr = v;
            else if (s.type == GfxSetting::Bool) *(bool*)s.ptr = v != 0;
            else if (s.type == GfxSetting::Int) *(int*)s.ptr = (int)v;
        }
}
void GfxReplayInject(int cls, float x, float y, float r) { g_frameEvents.push_back({ (uint8_t)cls, x, y, r, 1.0f }); }
// replay tool: a game smoke / steam sprite replaced by simulated puff `puff` (8 steam, 9 smoke, 11 dust, 12 hot gas)
void GfxReplayPuff(int puff, float x, float y, float r) { OnSpawn(kEvSmoke, x, y, r, 1.0f, (uint8_t)puff); }
// replay tool: one game particle batch (GL_QUADS, a quad per particle at world x, y, half size r) run
// through the draw tracker, as the game's particle system draws it
void GfxReplayParticleBatch(int cls, const float* xyr, int n) {
    if (g_w <= 0 || g_world[1] == g_world[0] || g_world[3] == g_world[2]) return;
    g_beginMode = 0x0007;
    BeginTrack((uint8_t)cls);
    for (int i = 0; i < 16; i++) g_trackM[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // vertices given in clip space
    g_trackVp[0] = g_trackVp[1] = 0; g_trackVp[2] = (float)g_w; g_trackVp[3] = (float)g_h;
    float sx = 2.0f / (g_world[1] - g_world[0]), sy = 2.0f / (g_world[3] - g_world[2]);
    for (int i = 0; i < n; i++) {
        float cx = (xyr[i * 3] - g_world[0]) * sx - 1.0f, cy = (xyr[i * 3 + 1] - g_world[2]) * sy - 1.0f;
        float rx = xyr[i * 3 + 2] * fabsf(sx), ry = xyr[i * 3 + 2] * fabsf(sy);
        GfxTrackColor(1.0f, 1.0f, 1.0f, 1.0f);
        GfxTrackVertex(cx - rx, cy - ry, 0); GfxTrackVertex(cx + rx, cy - ry, 0);
        GfxTrackVertex(cx + rx, cy + ry, 0); GfxTrackVertex(cx - rx, cy + ry, 0);
    }
    GfxEndTrack();
}
void GfxReplayLightning(float x) { LightningStrike(x, true); }
void GfxReplayWeather(int mode) { cfg.weatherMode = mode; cfg.thunder = false; }

// replay tool: preview the 3D backdrop behind the playfield (palette from the given layer textures)
void GfxReplayBackdrop(const GLuint* layers, int count) {
    g_bgLayer = count - 1;
    g_bgLayersPrev = count;
    for (int i = 0; i < count && i < 32; i++) g_bgLayerTex[i] = layers[i];
    g_phase = kWorld;
    RenderBackdropNow();
}
