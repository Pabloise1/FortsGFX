// FortsGFX - proxy opengl32.dll for Forts. Shared declarations.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- GL types (we deliberately don't include GL/gl.h: its dllimport declarations
// would make this DLL import from "opengl32.dll", i.e. from itself) ----
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat, GLclampf;
typedef double GLdouble;
typedef unsigned char GLboolean, GLubyte;
typedef char GLchar;

enum : GLenum {
    GL_TEXTURE_2D = 0x0DE1, GL_BLEND = 0x0BE2, GL_ALPHA_TEST = 0x0BC0,
    GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702, GL_MATRIX_MODE = 0x0BA0,
    GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_VIEWPORT = 0x0BA2,
    GL_CURRENT_COLOR = 0x0B00, GL_TEXTURE_BINDING_2D = 0x8069,
    GL_BLEND_SRC_RGB = 0x80C9, GL_BLEND_DST_RGB = 0x80C8,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
    GL_READ_BUFFER = 0x0C02, GL_DRAW_BUFFER = 0x0C01, GL_BACK = 0x0405,
    GL_RENDERBUFFER = 0x8D41, GL_RENDERBUFFER_WIDTH = 0x8D42, GL_RENDERBUFFER_HEIGHT = 0x8D43,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE = 0x8CD0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME = 0x8CD1,
    GL_SAMPLE_BUFFERS = 0x80A8,
    GL_PACK_ALIGNMENT = 0x0D05, GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_SKIP_ROWS = 0x0D03, GL_PACK_SKIP_PIXELS = 0x0D04,
    GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
    GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_SKIP_ROWS = 0x0CF3, GL_UNPACK_SKIP_PIXELS = 0x0CF4,
    GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
    GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401,
    GL_TEXTURE_WIDTH = 0x1000, GL_TEXTURE_HEIGHT = 0x1001, GL_TEXTURE_INTERNAL_FORMAT = 0x1003,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_CONTEXT_PROFILE_MASK = 0x9126,
};

// ---- pass-through entries (layout shared with thunk.asm / gen/stubs.asm: `real` must be first) ----
struct ProcEntry { void* real; const char* name; uint64_t reserved[2]; };
static_assert(sizeof(ProcEntry) == 32, "stubs.asm indexes g_entries in 32-byte steps");
extern "C" ProcEntry g_entries[];
extern "C" volatile uint8_t g_slowPath;   // nonzero while capturing or compiling a display list
extern "C" void GenericThunk();

// ---- real functions we call ourselves (never routed through our own exports) ----
struct RealGL {
    void (WINAPI* GetIntegerv)(GLenum, GLint*);
    void (WINAPI* GetFloatv)(GLenum, GLfloat*);
    GLboolean (WINAPI* IsEnabled)(GLenum);
    const GLubyte* (WINAPI* GetString)(GLenum);
    void (WINAPI* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    void (WINAPI* ReadBuffer)(GLenum);
    void (WINAPI* PixelStorei)(GLenum, GLint);
    HDC (WINAPI* wglGetCurrentDC)();
    HGLRC (WINAPI* wglGetCurrentContext)();
    PROC (WINAPI* wglGetProcAddress)(LPCSTR);
    // GL 3.0+/4.5, resolved per context by LoadRealExt()
    bool extLoaded;
    void (WINAPI* BindFramebuffer)(GLenum, GLuint);
    void (WINAPI* BindBuffer)(GLenum, GLuint);
    void (WINAPI* GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint*);
    void (WINAPI* GetTextureLevelParameteriv)(GLuint, GLint, GLenum, GLint*);
    void (WINAPI* GetTextureImage)(GLuint, GLint, GLenum, GLenum, GLsizei, void*);
    void (WINAPI* GetNamedRenderbufferParameteriv)(GLuint, GLenum, GLint*);
};
extern RealGL R;
extern std::wstring g_gameDir;  // folder containing Forts.exe (with trailing slash)
extern std::wstring g_outDir;   // logs/captures: <game dir>\FortsGFX\ or FortsGFX.ini OutputDir (trailing slash)

// ---- proxy.cpp ----
void* RealProc(const char* name);   // real opengl32 export, else real wglGetProcAddress
void SessionLog(const char* fmt, ...);
void LoadLog(const char* fmt, ...);     // loading.log: textures and scripts as the game loads them
std::string Fmt(const char* fmt, ...);
std::string Narrow(const wchar_t* s);
std::wstring Widen(const std::string& s);
const char* E(GLenum e);            // enum name, or hex
const char* PrimName(GLenum mode);
const char* BlendName(GLenum f);
uint64_t SafeHash(const void* p, size_t n);   // 0 if the memory isn't readable
bool PatchIat(HMODULE mod, const char* dll, const char* fn, void* hook, void** orig);
PROC MakeGenericProc(const char* name, PROC real);
void RecordProcName(const char* name, bool resolved);
void WriteProcList();
void UpdateSlowPath();
bool LoadRealExt();

// ---- hooks.cpp ----
extern bool g_compiling;            // inside glNewList/glEndList
void ResolveStaticHooks(HMODULE real);
void InstallIatHooks();
std::string HookFreshImageName();   // the image DevIL just loaded, if nothing has used it yet ("" otherwise); used up
void ListNoteCall(const char* fn);
std::string TexDescribe(GLuint id);
std::string TexShort(GLuint id);
bool TexSize(GLuint id, int& w, int& h);   // the size the game uploaded it at
extern GLuint g_texRestore;                // gfx: the game's texture to rebind after a draw drawn with ours (0 = none)
void GfxRestoreTexture();
void TexForEach(void (*fn)(GLuint id, const std::string& src));

// ---- gfx.cpp (Phase 1 renderer) ----
void GfxFrameBegin();                   // after SwapBuffers: redirect the world pass into our HDR targets
void GfxFrameEnd();                     // before SwapBuffers: post-process if no HUD was drawn this frame
void GfxBeforeDraw(bool displayList);   // before glBegin / glCallList (not while compiling a list)
void GfxOnOrtho(double l, double r, double b, double t);
void GfxOnLoadMatrix();
void GfxAfterClear(GLbitfield mask);
GLuint GfxRedirectFramebuffer(GLuint fb);
void GfxClassifyTexture(GLuint tex, const std::string& src);
void GfxForgetTexture(GLuint tex);
void GfxNoteTextureFormat(GLuint tex, GLint ifmt);
void GfxNoteTextureSize(GLuint tex, int w, int h);
void GfxNoteLuaFile(const std::string& path);   // the game opened a .lua: a map's main file sets the water level
void GfxDebugTargets(std::vector<std::pair<GLuint, std::string>>& out);
extern bool g_trackDraw;
extern GLenum g_beginMode;           // primitive of the current glBegin
struct GeoBatch { unsigned mode; std::vector<float> xy; };   // one glBegin..glEnd compiled into a display list
const std::vector<GeoBatch>* ListGeometry(GLuint id);
extern GLuint g_callListId;           // the list of the current glCallList             // the current glBegin..glEnd is an effect-event sprite
void GfxTrackVertex(float x, float y, float z);
void GfxEndTrack();
void GfxTrackColor(float r, float g, float b, float a);   // glColor inside a tracked block

// settings table shared by the ini code and the overlay
struct GfxSetting {
    enum Type { Bool, Float, Int, Vec2, Color } type;
    const char* section;
    const char* key;
    const char* label;
    void* ptr;
    float lo, hi;
    const char* tip;
};
const std::vector<GfxSetting>& GfxSettings();
void GfxSaveSettings();
void GfxReloadSettings();
void GfxApplyStyle(int preset);     // 0 Showcase, 1 Cinematic, 2 Clean
void GfxApplyFx(int preset);        // explosion strength: 0 Showcase, 1 Cinematic, 2 Subtle
void GfxApplyQuality(int preset);   // 0 Low, 1 High, 2 Ultra
int* GfxDebugView();                // buffer shown full screen, 0 = none
struct GfxStats { float frameMs, gpuMs, drawMs; };
GfxStats GfxGetStats();
int GfxOverlayKey();
// looks: complete style configurations (built-in + saved in [CustomLooks])
int GfxLookCount();
const char* GfxLookName(int i);
const char* GfxLookDesc(int i);
bool GfxLookIsCustom(int i);
int* GfxCurrentLook();
void GfxApplyLook(int i, bool keep = true);   // keep: also save to the ini
int GfxSaveCustomLook(const char* name);   // returns the new look's index, -1 if full
void GfxDeleteCustomLook(int i);
void GfxLookKeys(int& prev, int& next);
int GfxLabKey();
int GfxPhotoKey();
std::vector<uint8_t> GfxSnapshot();          // every setting, for the tour to restore afterwards
void GfxRestore(const std::vector<uint8_t>& snap);
// lab: effects on demand, comparison wipe, slow motion, HUD hiding
enum GfxLabKind { kLabExplosion, kLabMega, kLabLightning, kLabShock, kLabFlash, kLabHeat, kLabSparks, kLabSnow };
struct GfxLab {
    float wipe = -1.0f;     // split-screen before/after position (0..1 of the width), < 0 = off
    float slowMo = 1.0f;    // time scale of FortsGFX effects
    bool hideHud = false;   // hide the game's HUD and cursor
    // photo mode
    bool freeze = false;    // every FortsGFX effect stops where it is
    float dof = 0.0f;       // depth-of-field blur strength (0 = off)
    float focusX = 0.5f, focusY = 0.5f;   // focus point (uv, GL: y up)
    float tilt = 0.0f;      // tilt-shift: blur away from the focus line
    float frameAspect = 0.0f;   // framing bars (0 = none), e.g. 2.39 or 1.0
};
GfxLab& GfxLabState();
bool GfxWorldVisible();                                         // a world frame was drawn this frame
void GfxLabSpawn(int kind, float px, float py, float size);    // client px (top-left origin); size = fraction of view height
void GfxLabSetLight(int slot, bool on, float px, float py, float size, float intensity, const float col[3]);
void GfxLabClearLights();
void GfxLabClearSnow();
float* GfxTimeOfDay();              // hours 0..24 (day / night cycle)
bool* GfxTimePaused();
int GfxLabLightCount();
bool GfxHudMasked();                // hooks: the HUD is hidden, ignore the game's glColorMask

// ---- overlay.cpp ----
void OverlayFrame();                // just before SwapBuffers, after the capture read-back
void OverlayBridgeFrame(HWND wnd, GLuint fbo);   // Vulkan: the panel over the shared frame, input from wnd
void OverlayToast(const char* text);             // a message at the bottom of the screen for a moment
bool OverlayKeyBlocked(int vk);     // a hotkey is being bound in the panel, or vk was just bound: no hotkey fires
typedef HRESULT (WINAPI* DI8CreateFn)(HINSTANCE, DWORD, const IID&, void**, void*);
extern DI8CreateFn r_DirectInput8Create;
extern "C" HRESULT WINAPI hk_DirectInput8Create(HINSTANCE, DWORD, const IID&, void**, void*);

// ---- capture.cpp ----
extern bool g_capActive;
extern DWORD g_capThread;
inline bool CapOn() { return g_capActive && GetCurrentThreadId() == g_capThread; }
void CapLine(const char* fmt, ...);
void CapCount(const char* fn);
void CapIndent(int d);
void CapBeginBlock(GLenum mode, bool compiling);
void CapVertex(const char* fn, float x, float y, float z);
void CapTexCoord(const char* fn, float s, float t);
void CapColor(const char* fn, const float c[4]);
void CapEndBlock();
void CapCallList(GLuint id, const std::string& summary, const std::vector<GLuint>& tex, bool compiling);
void CapGenericCall(const char* name, const uint64_t* ints, const uint8_t* xmm);
void CapMaybeSnapshot(const char* why);
void CapFrameEnd();     // just before the real SwapBuffers
void CapFrameBegin();   // just after it
void CapHistNote(const std::string& line);   // flight recorder: one effect line for this frame
void CapRequest(const char* label);           // test bench: capture this frame, named after the beat
void GfxTourChapter(int chapter);              // showcase map: show this tour chapter (< 0: end the tour)
bool GfxWaterVisible();                       // a sea is on screen (water effects running)
void CapRequestPhoto();                      // photo mode: save this frame (no UI) to the Photos folder
void CapSaveRgbaPng(const std::wstring& path, int w, int h, std::vector<uint8_t>&& rgba);   // on a worker thread
void CapBridgePhoto(GLuint fbo);             // Vulkan: photo mode's save, from the bridge's shared frame

bool GfxBridgeFrame(GLuint inTex, GLuint outFbo, int w, int h);   // Vulkan: FortsGFX's image stages on a shared frame

// ---- vulkan.cpp ----
void VkInstallHooks();               // Forts' Vulkan renderer: watch it (GetProcAddress in Forts.exe's imports)

// engine.cpp: the game's own events, read from its functions (read-only)
void EngineInit();    // first frame: find the functions and start watching
void EngineFrame();   // once a frame: take in what the game did
bool EngineReady();   // the game's effect spawns are watched and confirmed to line up with its sprites
struct EngineMatch {
    int state = -1;   // -1: no engine data yet, 0: no fresh impact/explosion here, 1: one here (claimed for this blast)
    float dist = -1, age = 0;   // to the nearest unclaimed one (world units, seconds); dist < 0: none
    char file[96] = "";
};
EngineMatch EngineEffectAt(float x, float y, float radius);
bool EngineWatching();   // the watch is running (not yet necessarily confirmed)
// One thing the game did this frame, for the effects (gfx.cpp, render thread).
struct EngineFx {
    enum Kind : uint8_t { Effect, Impact, Fired, Device };
    uint8_t kind = Effect;
    bool hasDir = false;
    float x = 0, y = 0;     // world position (Effect, Impact)
    float dx = 0, dy = 0;   // Effect: the direction the game gave it; Impact: the surface normal
    int team = -1;          // Effect / Fired / Device: the team; Impact: the team that was hit
    int a = 0;              // Impact: hit type; Fired: projectile id; Device: device id
    char name[96] = "";     // Effect: its file ("effects/impact_heavy.lua"); Impact: projectile; Fired: weapon; Device: device
    char hit[48] = "";      // Impact: what it hit ("bracing", "armour"...)
};
void EngineTakeFx(std::vector<EngineFx>& out);
// A projectile in flight, from the game's own effect list: the trail effect it carries.
struct EngineTrail {
    uint32_t key = 0;              // the effect's id (stable while it flies)
    float x = 0, y = 0, vx = 0, vy = 0;   // world position and velocity (units/s)
    char name[48] = "";            // its trail effect, e.g. "effects/cannon_trail.lua"
};
bool EngineTracking();   // the effect list was found and confirmed to line up
void EngineTrails(std::vector<EngineTrail>& out);
