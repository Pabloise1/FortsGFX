// Offline check of the lighting passes: builds a synthetic working-res scene (two fires, struts,
// a ground slab), runs jump flood -> radiance cascades -> pull/push exactly like gfx.cpp, and
// writes gitest_light.png / gitest_sky.png / gitest_filled.png next to the exe.
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
enum : GLenum {
    GL_FRAGMENT_SHADER_ = 0x8B30, GL_VERTEX_SHADER_ = 0x8B31, GL_COMPILE_STATUS_ = 0x8B81, GL_LINK_STATUS_ = 0x8B82,
    GL_RGBA16F_ = 0x881A, GL_RGBA32F_ = 0x8814, GL_HALF_FLOAT_ = 0x140B, GL_FRAMEBUFFER_ = 0x8D40,
    GL_COLOR_ATTACHMENT0_ = 0x8CE0, GL_COLOR_ATTACHMENT1_ = 0x8CE1, GL_TEXTURE0_ = 0x84C0, GL_CLAMP_TO_EDGE_ = 0x812F,
};
#define F(ret, n, args) static ret (WINAPI* n) args
F(GLuint, CreateShader, (GLenum)); F(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*));
F(void, CompileShader, (GLuint)); F(void, GetShaderiv, (GLuint, GLenum, GLint*));
F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)); F(GLuint, CreateProgram, ());
F(void, AttachShader, (GLuint, GLuint)); F(void, LinkProgram, (GLuint)); F(void, UseProgram, (GLuint));
F(GLint, GetUniformLocation, (GLuint, const GLchar*)); F(void, Uniform1i, (GLint, GLint));
F(void, Uniform1f, (GLint, GLfloat)); F(void, Uniform2f, (GLint, GLfloat, GLfloat));
F(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)); F(void, ActiveTexture, (GLenum));
F(void, GenFramebuffers, (GLsizei, GLuint*)); F(void, BindFramebuffer, (GLenum, GLuint));
F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)); F(void, DrawBuffers, (GLsizei, const GLenum*));
F(void, GenVertexArrays, (GLsizei, GLuint*)); F(void, BindVertexArray, (GLuint));
#undef F

static GLuint Prog(const char* fs) {
    GLuint p = CreateProgram();
    const char* srcs[2][2] = { { kVsFullscreen, nullptr }, { kFsCommon, fs } };
    GLenum types[2] = { GL_VERTEX_SHADER_, GL_FRAGMENT_SHADER_ };
    for (int i = 0; i < 2; i++) {
        GLuint s = CreateShader(types[i]);
        ShaderSource(s, srcs[i][1] ? 2 : 1, srcs[i], nullptr);
        CompileShader(s);
        AttachShader(p, s);
    }
    LinkProgram(p);
    return p;
}

struct T { GLuint tex = 0, tex2 = 0, fbo = 0; int w, h; };
static GLuint Tex(int w, int h, GLenum fmt, const float* data = nullptr) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    return t;
}
static T Target(int w, int h, GLenum fmt, bool two = false) {
    T t; t.w = w; t.h = h;
    t.tex = Tex(w, h, fmt);
    if (two) t.tex2 = Tex(w, h, fmt);
    GenFramebuffers(1, &t.fbo);
    BindFramebuffer(GL_FRAMEBUFFER_, t.fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, t.tex, 0);
    if (two) {
        FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT1_, GL_TEXTURE_2D, t.tex2, 0);
        GLenum b[2] = { GL_COLOR_ATTACHMENT0_, GL_COLOR_ATTACHMENT1_ };
        DrawBuffers(2, b);
    }
    return t;
}
static void Bind(int u, GLuint t) { ActiveTexture(GL_TEXTURE0_ + u); glBindTexture(GL_TEXTURE_2D, t); }
static void Draw(const T& t) { BindFramebuffer(GL_FRAMEBUFFER_, t.fbo); glViewport(0, 0, t.w, t.h); glDrawArrays(GL_TRIANGLES, 0, 3); }
static void I(GLuint p, const char* n, int v) { Uniform1i(GetUniformLocation(p, n), v); }
static void F1(GLuint p, const char* n, float v) { Uniform1f(GetUniformLocation(p, n), v); }
static void F2(GLuint p, const char* n, float a, float b) { Uniform2f(GetUniformLocation(p, n), a, b); }
static void F4(GLuint p, const char* n, float a, float b, float c, float d) { Uniform4f(GetUniformLocation(p, n), a, b, c, d); }

static void SavePng(const wchar_t* path, GLuint tex, int w, int h, int mode) {
    std::vector<float> px((size_t)w * h * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, px.data());
    std::vector<BYTE> out((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float* s = &px[((size_t)(h - 1 - y) * w + x) * 4];
            BYTE* d = &out[((size_t)y * w + x) * 4];
            float c[3];
            for (int i = 0; i < 3; i++) {
                float len = sqrtf(s[0] * s[0] + s[1] * s[1]) + 1e-6f;
                float v = mode == 1 ? s[3] / 0.7f : mode == 2 ? s[0] : mode == 3 ? (i < 2 ? s[i] / len * 0.5f + 0.5f : fminf(len * 8.0f, 1.0f)) : s[i] / (1.0f + s[i]);
                c[i] = powf(fminf(fmaxf(v, 0.0f), 1.0f), 1.0f / 2.2f);
            }
            d[0] = (BYTE)(c[2] * 255); d[1] = (BYTE)(c[1] * 255); d[2] = (BYTE)(c[0] * 255); d[3] = 255;
        }
    IWICImagingFactory* f; IWICStream* st; IWICBitmapEncoder* e; IWICBitmapFrameEncode* fr; IPropertyBag2* pb;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    f->CreateStream(&st); st->InitializeFromFilename(path, GENERIC_WRITE);
    f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e); e->Initialize(st, WICBitmapEncoderNoCache);
    e->CreateNewFrame(&fr, &pb); fr->Initialize(pb); fr->SetSize(w, h);
    WICPixelFormatGUID g = GUID_WICPixelFormat32bppBGRA; fr->SetPixelFormat(&g);
    fr->WritePixels(h, w * 4, (UINT)out.size(), out.data()); fr->Commit(); e->Commit();
    pb->Release(); fr->Release(); e->Release(); st->Release(); f->Release();
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "gitest";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("gitest", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
#define P(n) n = (decltype(n))wglGetProcAddress("gl" #n)
    P(CreateShader); P(ShaderSource); P(CompileShader); P(GetShaderiv); P(GetShaderInfoLog); P(CreateProgram);
    P(AttachShader); P(LinkProgram); P(UseProgram); P(GetUniformLocation); P(Uniform1i); P(Uniform1f); P(Uniform2f);
    P(Uniform4f); P(ActiveTexture); P(GenFramebuffers); P(BindFramebuffer); P(FramebufferTexture2D); P(DrawBuffers);
    P(GenVertexArrays); P(BindVertexArray);
#undef P
    GLuint vao; GenVertexArrays(1, &vao); BindVertexArray(vao);

    const int W = 960, H = 544, N = 6;
    std::vector<float> scene((size_t)W * H * 4, 0.0f);
    auto put = [&](int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        float* p = &scene[((size_t)y * W + x) * 4]; p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    };
    for (int y = 0; y < 90; y++) for (int x = 0; x < W; x++) put(x, y, 0, 0, 0, 1);              // ground
    for (int i = 0; i < 6; i++) for (int y = 90; y < 330; y++) for (int x = 0; x < 6; x++) put(300 + i * 60 + x, y, 0, 0, 0, 1);  // struts
    for (int x = 300; x < 606; x++) for (int y = 330; y < 338; y++) put(x, y, 0, 0, 0, 1);     // roof
    for (int y = -14; y <= 14; y++) for (int x = -14; x <= 14; x++)                             // fire inside the fort
        if (x * x + y * y <= 196) put(450 + x, 140 + y, 6.0f, 2.4f, 0.4f, 0.5f);
    for (int y = -8; y <= 8; y++) for (int x = -8; x <= 8; x++)                                 // blue light outside
        if (x * x + y * y <= 64) put(800 + x, 250 + y, 0.4f, 1.2f, 5.0f, 0.5f);
    for (int x = 640; x < 940; x++) for (int y = 300; y < 303; y++) put(x, y, 3.0f, 0.2f, 0.2f, 0.5f);   // thin laser beam (light source, not solid)

    GLuint gi = Tex(W, H, GL_RGBA16F_, scene.data());
    T jfa[2] = { Target(W, H, GL_RGBA32F_), Target(W, H, GL_RGBA32F_) }, sdf = Target(W, H, GL_RGBA16F_);
    T cas[2] = { Target(W, H, GL_RGBA16F_, true), Target(W, H, GL_RGBA16F_, true) };
    GLuint pSeed = Prog(kFsJfaSeed), pStep = Prog(kFsJfaStep), pDist = Prog(kFsJfaDist), pCas = Prog(kFsCascade);
    GLuint pPull0 = Prog(kFsPull0), pPull = Prog(kFsPull), pPush = Prog(kFsPush);

    UseProgram(pSeed); I(pSeed, "uSrc", 0); Bind(0, gi); Draw(jfa[0]);
    UseProgram(pStep); I(pStep, "uSrc", 0); F2(pStep, "uRes", W, H);
    int cur = 0;
    for (int step = 512; step >= 1; step /= 2) { F1(pStep, "uStep", (float)step); Bind(0, jfa[cur].tex); Draw(jfa[cur ^ 1]); cur ^= 1; }
    UseProgram(pDist); I(pDist, "uSrc", 0); Bind(0, jfa[cur].tex); Draw(sdf);

    UseProgram(pCas); I(pCas, "uGi", 0); I(pCas, "uSdf", 1); I(pCas, "uUpper", 2);
    F2(pCas, "uRes", W, H); F2(pCas, "uSky", 1.0f, 0.4f); F1(pCas, "uOpacity", 0.15f);
    F4(pCas, "uSun", -0.64f, 0.77f, cosf(25.0f * 3.14159265f / 180.0f), 1.6f);
    Uniform4f(GetUniformLocation(pCas, "uSunColor"), 1.0f, 0.92f, 0.78f, 0.0f);
    Bind(0, gi); Bind(1, sdf.tex);
    int w = 0, out = 0;
    for (int i = N - 1; i >= 0; i--) {
        F4(pCas, "uC", (float)i, (float)N, 1.0f, 0.0f);
        Bind(2, cas[w ^ 1].tex); Draw(cas[w]); out = w; w ^= 1;
    }

    std::vector<T> pull, push;
    for (int pw = W, ph = H; pw >= 8 && ph >= 8 && pull.size() < 6; pw /= 2, ph /= 2) {
        pull.push_back(Target(pw, ph, GL_RGBA16F_, true)); push.push_back(Target(pw, ph, GL_RGBA16F_, true));
    }
    UseProgram(pPull0); I(pPull0, "uRC", 0); I(pPull0, "uGi", 1); F1(pPull0, "uEmisThreshold", 0.02f);
    Bind(0, cas[out].tex); Bind(1, gi); Draw(pull[0]);
    UseProgram(pPull); I(pPull, "uA", 0); I(pPull, "uB", 1);
    for (size_t l = 1; l < pull.size(); l++) { Bind(0, pull[l - 1].tex); Bind(1, pull[l - 1].tex2); Draw(pull[l]); }
    UseProgram(pPush); I(pPush, "uA", 0); I(pPush, "uB", 1); I(pPush, "uCA", 2); I(pPush, "uCB", 3);
    for (int l = (int)pull.size() - 1; l >= 0; l--) {
        F1(pPush, "uCoarsest", l == (int)pull.size() - 1 ? 1.0f : 0.0f);
        Bind(0, pull[l].tex); Bind(1, pull[l].tex2);
        if (l + 1 < (int)pull.size()) { Bind(2, push[l + 1].tex); Bind(3, push[l + 1].tex2); }
        Draw(push[l]);
    }
    glFinish();

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    SavePng((dir + L"gitest_light.png").c_str(), cas[out].tex, W, H, 0);
    SavePng((dir + L"gitest_sky.png").c_str(), cas[out].tex, W, H, 1);
    SavePng((dir + L"gitest_filled.png").c_str(), push[0].tex, W, H, 0);
    SavePng((dir + L"gitest_direction.png").c_str(), cas[out].tex2, W, H, 3);
    printf("GL error: 0x%X\nwrote gitest_light.png, gitest_sky.png, gitest_filled.png\n", glGetError());
    return 0;
}
