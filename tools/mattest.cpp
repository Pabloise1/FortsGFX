// Offline check of the world material shader: draws the same sprites (rotated, flipped, alpha-tested,
// untextured, alpha-only texture) with fixed-function and with kVsWorld/kFsWorld under a Forts-style
// y-down glOrtho, compares target 0 pixel-for-pixel, and writes mattest_scene.png / mattest_normals.png.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <objbase.h>
#include <wincodec.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/shaders.h"

typedef char GLchar;
enum : GLenum { VS = 0x8B31, FS = 0x8B30, FBO = 0x8D40, CA0 = 0x8CE0, RGBA8_ = 0x8058, ALPHA8_ = 0x803C };
#define F(ret, n, args) static ret (WINAPI* n) args
F(GLuint, CreateShader, (GLenum)); F(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*));
F(void, CompileShader, (GLuint)); F(GLuint, CreateProgram, ()); F(void, AttachShader, (GLuint, GLuint));
F(void, LinkProgram, (GLuint)); F(void, UseProgram, (GLuint)); F(GLint, GetUniformLocation, (GLuint, const GLchar*));
F(void, Uniform1i, (GLint, GLint)); F(void, Uniform4i, (GLint, GLint, GLint, GLint, GLint));
F(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)); F(void, GenFramebuffers, (GLsizei, GLuint*));
F(void, BindFramebuffer, (GLenum, GLuint)); F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
F(void, DrawBuffers, (GLsizei, const GLenum*)); F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*));
F(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*));
#undef F

static const int W = 640, H = 360;

static GLuint Tex(int w, int h, GLenum ifmt, GLenum fmt, const void* data) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    return t;
}

static void Save(const wchar_t* path, const std::vector<BYTE>& rgba) {
    std::vector<BYTE> bgra(rgba.size());
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const BYTE* s = &rgba[((size_t)(H - 1 - y) * W + x) * 4]; BYTE* d = &bgra[((size_t)y * W + x) * 4];
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
    }
    IWICImagingFactory* f; IWICStream* st; IWICBitmapEncoder* e; IWICBitmapFrameEncode* fr; IPropertyBag2* pb;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    f->CreateStream(&st); st->InitializeFromFilename(path, GENERIC_WRITE);
    f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e); e->Initialize(st, WICBitmapEncoderNoCache);
    e->CreateNewFrame(&fr, &pb); fr->Initialize(pb); fr->SetSize(W, H);
    WICPixelFormatGUID g = GUID_WICPixelFormat32bppBGRA; fr->SetPixelFormat(&g);
    fr->WritePixels(H, W * 4, (UINT)bgra.size(), bgra.data()); fr->Commit(); e->Commit();
    pb->Release(); fr->Release(); e->Release(); st->Release(); f->Release();
}

static GLuint g_sprite, g_alphaTex, g_prog;
static GLint lMode, lClass, lAlpha, lMat, lTex;

// One sprite = one quad, positioned like Forts does (glLoadMatrixf/translate/rotate in world units).
static void Quad(float x, float y, float w, float h, float rot, bool flipU, float r, float g, float b, float a) {
    glPushMatrix();
    glTranslatef(x, y, 0); glRotatef(rot, 0, 0, 1);
    glColor4f(r, g, b, a);
    float u0 = flipU ? 1.0f : 0.0f, u1 = 1.0f - u0;
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(u0, 0); glVertex2f(-w / 2, -h / 2);
    glTexCoord2f(u1, 0); glVertex2f(w / 2, -h / 2);
    glTexCoord2f(u0, 1); glVertex2f(-w / 2, h / 2);
    glTexCoord2f(u1, 1); glVertex2f(w / 2, h / 2);
    glEnd();
    glPopMatrix();
}

static void Scene(bool shader) {
    glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-1000, 1000, 562.5, -562.5, -1000, 1000);   // y grows downward, like Forts' world
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    auto set = [&](int env, int atest, int alphaFmt, int normals) {
        if (!shader) return;
        UseProgram(g_prog);
        Uniform1i(lTex, 0);
        Uniform4i(lMode, env, 0, atest ? (GL_GREATER - GL_NEVER + 1) : 0, alphaFmt);
        Uniform4i(lClass, 1, 1, 0, normals);
        Uniform4f(lAlpha, 0.1f, 0, 0, 0);
        Uniform4f(lMat, 1.6f, 6.0f, 0.35f, 0.8f);
    };
    glBindTexture(GL_TEXTURE_2D, g_sprite);
    set(1, 0, 0, 1); Quad(-600, -150, 360, 120, 0, false, 1, 1, 1, 1);
    set(1, 0, 0, 1); Quad(-100, -150, 360, 120, 35, false, 1, 0.8f, 0.6f, 1);
    set(1, 0, 0, 1); Quad(450, -150, 360, 120, -60, true, 0.7f, 1, 0.7f, 0.8f);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.1f);
    set(1, 1, 0, 1); Quad(-600, 250, 300, 300, 90, false, 1, 1, 1, 1);
    glDisable(GL_ALPHA_TEST);
    glBindTexture(GL_TEXTURE_2D, g_alphaTex);                 // alpha-only (font-style) texture
    set(1, 0, 1, 0); Quad(-100, 250, 300, 200, 0, false, 1, 0.9f, 0.2f, 1);
    glDisable(GL_TEXTURE_2D);                                 // untextured quad
    set(0, 0, 0, 0); Quad(450, 250, 300, 150, 10, false, 0.9f, 0.2f, 0.3f, 0.6f);
    UseProgram(0);
}

static std::vector<BYTE> Read(GLuint fbo, GLenum att) {
    std::vector<BYTE> px((size_t)W * H * 4);
    BindFramebuffer(FBO, fbo); glReadBuffer(att);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "mattest";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("mattest", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
#define P(n) n = (decltype(n))wglGetProcAddress("gl" #n)
    P(CreateShader); P(ShaderSource); P(CompileShader); P(CreateProgram); P(AttachShader); P(LinkProgram); P(UseProgram);
    P(GetUniformLocation); P(Uniform1i); P(Uniform4i); P(Uniform4f); P(GenFramebuffers); P(BindFramebuffer);
    P(FramebufferTexture2D); P(DrawBuffers); P(GetShaderInfoLog); P(GetProgramInfoLog);
#undef P
    // sprite: rounded plank with alpha edge, wood grain and a riveted metal band
    const int TS = 128;
    std::vector<BYTE> spr(TS * TS * 4), alpha(TS * TS);
    for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++) {
        float dx = fabsf(x - 63.5f) / 60.0f, dy = fabsf(y - 63.5f) / 40.0f;
        float a = fminf(fmaxf((1.0f - fmaxf(powf(dx, 6), powf(dy, 6))) * 8.0f, 0.0f), 1.0f);
        bool metal = x > 52 && x < 76;
        float grain = 0.75f + 0.25f * sinf(y * 0.9f + sinf(x * 0.15f) * 2.0f);
        BYTE* p = &spr[(y * TS + x) * 4];
        if (metal) { BYTE v = (BYTE)(150 + ((x - 64) * (x - 64) + (y % 16 - 8) * (y % 16 - 8) < 12 ? 80 : 0)); p[0] = v; p[1] = v; p[2] = (BYTE)(v + 10); }
        else { p[0] = (BYTE)(150 * grain); p[1] = (BYTE)(95 * grain); p[2] = (BYTE)(45 * grain); }
        p[3] = (BYTE)(a * 255);
        alpha[y * TS + x] = (BYTE)(((x / 16 + y / 16) % 2) ? 255 : 60);
    }
    g_sprite = Tex(TS, TS, RGBA8_, GL_RGBA, spr.data());
    g_alphaTex = Tex(TS, TS, ALPHA8_, GL_ALPHA, alpha.data());

    GLuint v = CreateShader(VS), f = CreateShader(FS);
    ShaderSource(v, 1, &kVsWorld, nullptr); CompileShader(v);
    ShaderSource(f, 1, &kFsWorld, nullptr); CompileShader(f);
    g_prog = CreateProgram(); AttachShader(g_prog, v); AttachShader(g_prog, f); LinkProgram(g_prog);
    char log[4096] = {}; GetProgramInfoLog(g_prog, sizeof(log), nullptr, log); if (log[0]) printf("link log: %s\n", log);
    lMode = GetUniformLocation(g_prog, "uMode"); lClass = GetUniformLocation(g_prog, "uClass");
    lAlpha = GetUniformLocation(g_prog, "uAlpha"); lMat = GetUniformLocation(g_prog, "uMat"); lTex = GetUniformLocation(g_prog, "uTex");

    GLuint ffTex = Tex(W, H, RGBA8_, GL_RGBA, nullptr), sTex[4];
    for (auto& t : sTex) t = Tex(W, H, RGBA8_, GL_RGBA, nullptr);
    GLuint ffFbo, sFbo;
    GenFramebuffers(1, &ffFbo); BindFramebuffer(FBO, ffFbo); FramebufferTexture2D(FBO, CA0, GL_TEXTURE_2D, ffTex, 0);
    GenFramebuffers(1, &sFbo); BindFramebuffer(FBO, sFbo);
    for (int i = 0; i < 4; i++) FramebufferTexture2D(FBO, CA0 + i, GL_TEXTURE_2D, sTex[i], 0);
    glViewport(0, 0, W, H);

    BindFramebuffer(FBO, ffFbo); Scene(false);
    BindFramebuffer(FBO, sFbo);
    GLenum bufs[4] = { CA0, CA0 + 1, CA0 + 2, CA0 + 3 };
    DrawBuffers(4, bufs);
    float flatN[4] = { 0.5f, 0.5f, 0, 0 };
    Scene(true);   // clears all 4 targets with the scene clear colour; target 3 is only inspected where sprites wrote
    std::vector<BYTE> ff = Read(ffFbo, CA0), sh = Read(sFbo, CA0), nrm = Read(sFbo, CA0 + 3);
    int maxd = 0; size_t bad = 0;
    for (size_t i = 0; i < ff.size(); i++) { int d = abs((int)ff[i] - (int)sh[i]); maxd = d > maxd ? d : maxd; bad += d > 2; }
    printf("target 0 vs fixed-function: max channel diff %d, %zu channels differ by >2 (of %zu)\n", maxd, bad, ff.size());

    // normals of the unrotated plank: left edge should point left (r<128), right edge right, top up (g>128)
    auto at = [&](int x, int y) { const BYTE* p = &nrm[((size_t)y * W + x) * 4]; return std::make_pair((int)p[0], (int)p[1]); };
    auto L = at(W / 2 - 300 + 4 + 60 - 88, H / 2 + 75), R = at(W / 2 - 300 + 4 + 60 + 82, H / 2 + 75);
    printf("plank left edge normal (r,g) = (%d,%d)  right edge = (%d,%d)\n", L.first, L.second, R.first, R.second);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe); dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    Save((dir + L"mattest_ff.png").c_str(), ff);
    Save((dir + L"mattest_scene.png").c_str(), sh);
    Save((dir + L"mattest_normals.png").c_str(), nrm);
    printf("GL error 0x%X; wrote mattest_ff.png, mattest_scene.png, mattest_normals.png\n", glGetError());
    (void)flatN;
    return bad == 0 ? 0 : 1;
}
