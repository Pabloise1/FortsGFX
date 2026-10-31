// Compiles and links every post-processing shader against the local GL driver, so GLSL errors
// show up at build time instead of silently disabling the effects in game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include "../src/shaders.h"

typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

static GLuint (WINAPI* CreateShader)(GLenum);
static void (WINAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
static void (WINAPI* CompileShader)(GLuint);
static void (WINAPI* GetShaderiv)(GLuint, GLenum, GLint*);
static void (WINAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
static GLuint (WINAPI* CreateProgram)();
static void (WINAPI* AttachShader)(GLuint, GLuint);
static void (WINAPI* LinkProgram)(GLuint);
static void (WINAPI* GetProgramiv)(GLuint, GLenum, GLint*);
static void (WINAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);

static GLuint Compile(GLenum type, const char* a, const char* b, const char* label, bool& ok) {
    GLuint s = CreateShader(type);
    const GLchar* parts[2] = { a, b };
    ShaderSource(s, b ? 2 : 1, parts, nullptr);
    CompileShader(s);
    GLint st = 0;
    GetShaderiv(s, GL_COMPILE_STATUS, &st);
    char log[8192] = {};
    GetShaderInfoLog(s, sizeof(log), nullptr, log);
    if (!st) { printf("FAIL compile %s:\n%s\n", label, log); ok = false; }
    else if (log[0]) printf("warn %s:\n%s\n", label, log);
    return s;
}

int main() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fortsgfx_shadertest";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
#define P(n) n = (decltype(n))wglGetProcAddress("gl" #n)
    P(CreateShader); P(ShaderSource); P(CompileShader); P(GetShaderiv); P(GetShaderInfoLog);
    P(CreateProgram); P(AttachShader); P(LinkProgram); P(GetProgramiv); P(GetProgramInfoLog);
#undef P
    struct { const char* name; const char* src; } fs[] = {
        { "prefilter", kFsPrefilter }, { "down", kFsDown }, { "up", kFsUp }, { "bloom_avg", kFsBloomAvg }, { "composite", kFsComposite },
        { "fxaa", kFsFxaa }, { "final", kFsFinal }, { "gi_prep", kFsGiPrep }, { "jfa_seed", kFsJfaSeed },
        { "jfa_step", kFsJfaStep }, { "jfa_dist", kFsJfaDist }, { "cascade", kFsCascade }, { "pull0", kFsPull0 },
        { "pull", kFsPull }, { "temporal", kFsTemporal }, { "p_update", kFsParticleUpdate }, { "copy", kFsCopy }, { "milk", kFsMilk }, { "backdrop", kFsBackdrop }, { "bg_down", kFsBgDown }, { "push", kFsPush }, { "debug", kFsDebug }, { "snow_step", kFsSnowStep }, { "snow_copy", kFsSnowCopy }, { "dof_blur", kFsBlur },
    };
    bool all = true;
    for (auto& f : fs) {
        bool ok = true;
        GLuint v = Compile(GL_VERTEX_SHADER, kVsFullscreen, nullptr, "fullscreen.vs", ok);
        GLuint s = Compile(GL_FRAGMENT_SHADER, kFsCommon, f.src, f.name, ok);
        if (ok) {
            GLuint p = CreateProgram();
            AttachShader(p, v);
            AttachShader(p, s);
            LinkProgram(p);
            GLint st = 0;
            GetProgramiv(p, GL_LINK_STATUS, &st);
            if (!st) {
                char log[8192] = {};
                GetProgramInfoLog(p, sizeof(log), nullptr, log);
                printf("FAIL link %s:\n%s\n", f.name, log);
                ok = false;
            }
        }
        printf("%-10s %s\n", f.name, ok ? "ok" : "FAILED");
        all = all && ok;
    }
    {   // world material shader: compatibility-profile vertex + fragment
        bool ok = true;
        GLuint v = Compile(GL_VERTEX_SHADER, kVsWorld, nullptr, "world.vs", ok);
        GLuint s = Compile(GL_FRAGMENT_SHADER, kFsWorld, nullptr, "world.fs", ok);
        if (ok) {
            GLuint p = CreateProgram(); AttachShader(p, v); AttachShader(p, s); LinkProgram(p);
            GLint st = 0; GetProgramiv(p, GL_LINK_STATUS, &st);
            if (!st) { char log[8192] = {}; GetProgramInfoLog(p, sizeof(log), nullptr, log); printf("FAIL link world:\n%s\n", log); ok = false; }
        }
        printf("%-10s %s\n", "world", ok ? "ok" : "FAILED");
        all = all && ok;
    }
    {   // effect programs with their own vertex shaders
        struct { const char* name; const char* vs; const char* fs; } vf[] = {
            { "fireball", kVsFireball, kFsFireball }, { "decal", kVsDecal, kFsDecal }, { "sprite", kVsSprite, kFsSprite }, { "stream", kVsStream, kFsStream }, { "beam_fx", kVsStream, kFsBeam }, { "particles", kVsParticle, kFsParticle }, { "bolt", kVsBolt, kFsBolt }, { "snow_deposit", kVsSnowDeposit, kFsSnowDeposit }, { "terrain_mask", kVsTerrain, kFsTerrain } };
        for (auto& p : vf) {
            bool ok = true;
            GLuint v = Compile(GL_VERTEX_SHADER, p.vs, nullptr, p.name, ok);
            GLuint s2 = Compile(GL_FRAGMENT_SHADER, kFsCommon, p.fs, p.name, ok);
            if (ok) {
                GLuint pr = CreateProgram(); AttachShader(pr, v); AttachShader(pr, s2); LinkProgram(pr);
                GLint st = 0; GetProgramiv(pr, GL_LINK_STATUS, &st);
                if (!st) { char log[8192] = {}; GetProgramInfoLog(pr, sizeof(log), nullptr, log); printf("FAIL link %s:\n%s\n", p.name, log); ok = false; }
            }
            printf("%-10s %s\n", p.name, ok ? "ok" : "FAILED");
            all = all && ok;
        }
    }    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    DestroyWindow(wnd);
    return all ? 0 : 1;
}
