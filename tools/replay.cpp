// Replays the real post chain (src/gfx.cpp) offline on a FortsGFX capture folder, so lighting/fog
// tuning can be judged on an actual game frame. Usage: replay.exe <capture dir> [ini dir]
// Writes replay_off.png (lighting+fog off) and replay_on.png into the capture folder.
#include "../src/common.h"
#include <objbase.h>
#include <wincodec.h>
#include <cstdarg>
#include <fstream>
#include <regex>
#include <functional>

// ---- the DLL-side symbols gfx.cpp links against
RealGL R;
std::wstring g_gameDir, g_outDir;
bool g_capActive;
DWORD g_capThread;
void SessionLog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
void LoadLog(const char*, ...) {}
void CapLine(const char*, ...) {}
std::string Narrow(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string o(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, &o[0], n, nullptr, nullptr);
    return o;
}
std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring o(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &o[0], n);
    return o;
}
void* RealProc(const char* name) {
    void* p = (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
    return p ? p : (void*)wglGetProcAddress(name);
}
void TexForEach(void (*)(GLuint, const std::string&)) {}
std::string Fmt(const char* fmt, ...) { char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); return b; }
std::string TexShort(GLuint id) { return std::to_string(id); }
void CapHistNote(const std::string&) {}
void CapRequestPhoto() {}
void CapRequest(const char*) {}
bool EngineReady() { return false; }
EngineMatch EngineEffectAt(float, float, float) { return {}; }
bool EngineWatching() { return true; }
static std::vector<EngineFx> g_replayFx;   // game events the "gamefx" mode feeds in
void EngineTakeFx(std::vector<EngineFx>& out) { out.swap(g_replayFx); g_replayFx.clear(); }
static bool g_replayTracking;
static std::vector<EngineTrail> g_replayTrails;   // projectiles in flight the "gamefx" mode moves
bool EngineTracking() { return g_replayTracking; }
void EngineTrails(std::vector<EngineTrail>& out) { out = g_replayTrails; }
const std::vector<GeoBatch>* ListGeometry(GLuint) { return nullptr; }
bool TexSize(GLuint, int& w, int& h) { w = h = 0; return false; }
GLuint g_callListId;

bool GfxReplaySetup(int w, int h);
void GfxReplayTargets(GLuint out[4]);
void GfxReplayRun(const float world[4], GLuint outFbo, bool lighting, int frames);
void GfxReplayProfile(bool on);
void GfxReplayPrintProfile();
void GfxReplaySetTime(double t);
void GfxReplayInject(int cls, float x, float y, float r);
void GfxReplayLightning(float x);
void GfxReplayWeather(int mode);
void GfxReplayBackdrop(const GLuint* layers, int count);
void OverlayPreview(int w, int h, bool panel, bool lab, bool more);
void GfxReplayWet(float w);
void GfxReplaySnowCover(float c);
void GfxReplayTune(const char* id, float v);
void GfxReplayFuji(float worldX);
float GfxReplayGet(const char* id);
void GfxReplayParticleCounts(int* counts);
void GfxReplayTimeOfDay(float h);
void GfxReplayWater(bool on, float level);
void GfxReplayParticleBatch(int cls, const float* xyr, int n);
void GfxReplayPuff(int puff, float x, float y, float r);
void OverlayTourSetup(int w, int h, int chapter, double t);
void OverlayTourDraw(int w, int h);

// ---- image helpers
struct Img { int w = 0, h = 0; std::vector<BYTE> px; };   // RGBA, top-down

static bool LoadPng(IWICImagingFactory* f, const std::wstring& path, Img& img) {
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    dec->GetFrame(0, &fr);
    f->CreateFormatConverter(&cv);
    cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w, h; cv->GetSize(&w, &h);
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    cv->CopyPixels(nullptr, w * 4, (UINT)img.px.size(), img.px.data());
    cv->Release(); fr->Release(); dec->Release();
    return true;
}

static void SavePng(IWICImagingFactory* f, const std::wstring& path, int w, int h, const std::vector<BYTE>& bottomUpRgba) {
    std::vector<BYTE> bgra(bottomUpRgba.size());
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const BYTE* s = &bottomUpRgba[((size_t)(h - 1 - y) * w + x) * 4]; BYTE* d = &bgra[((size_t)y * w + x) * 4];
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
    }
    IWICStream* st; IWICBitmapEncoder* e; IWICBitmapFrameEncode* fr; IPropertyBag2* pb;
    f->CreateStream(&st); st->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e); e->Initialize(st, WICBitmapEncoderNoCache);
    e->CreateNewFrame(&fr, &pb); fr->Initialize(pb); fr->SetSize(w, h);
    WICPixelFormatGUID g = GUID_WICPixelFormat32bppBGRA; fr->SetPixelFormat(&g);
    fr->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data()); fr->Commit(); e->Commit();
    pb->Release(); fr->Release(); e->Release(); st->Release();
}

// bilinear sample of a top-down image at full-res pixel (x, y)
static const BYTE* Px(const Img& im, int x, int y) {
    x = std::min(std::max(x, 0), im.w - 1); y = std::min(std::max(y, 0), im.h - 1);
    return &im.px[((size_t)y * im.w + x) * 4];
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: replay <capture dir> [ini dir]\n"); return 2; }
    std::wstring dir = argv[1];
    if (dir.back() != L'\\') dir += L'\\';
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    g_gameDir = argc > 2 ? std::wstring(argv[2]) + L"\\" : std::wstring(tmp) + L"fortsgfx_replay\\";
    CreateDirectoryW(g_gameDir.c_str(), nullptr);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic; CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    Img scene, emis, masks, norms;
    if (!LoadPng(wic, dir + L"post_in_scene.png", scene) || !LoadPng(wic, dir + L"post_in_emissive.png", emis) ||
        !LoadPng(wic, dir + L"gi_masks.png", masks) || !LoadPng(wic, dir + L"gi_normals.png", norms)) {
        printf("capture is missing post_in_scene / post_in_emissive / gi_masks / gi_normals\n"); return 1;
    }
    const int W = scene.w, H = scene.h;

    // world projection: first non-HUD glOrtho in frame.txt
    float world[4] = { -1000, 1000, 1000, -1000 };
    std::ifstream ft(dir + L"frame.txt");
    std::string line;
    std::regex re("glOrtho\\(([-0-9.e]+), ([-0-9.e]+), ([-0-9.e]+), ([-0-9.e]+),");
    while (std::getline(ft, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re) && std::stof(m[1]) != 0.0f) {
            for (int i = 0; i < 4; i++) world[i] = std::stof(m[i + 1]);
            break;
        }
    }
    printf("frame %dx%d, world ortho %.1f %.1f %.1f %.1f\n", W, H, world[0], world[1], world[2], world[3]);

    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "replay";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("replay", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
#define RES(field, name) R.field = (decltype(R.field))RealProc(name)
    RES(GetIntegerv, "glGetIntegerv"); RES(GetFloatv, "glGetFloatv"); RES(IsEnabled, "glIsEnabled"); RES(GetString, "glGetString");
    RES(ReadPixels, "glReadPixels"); RES(ReadBuffer, "glReadBuffer"); RES(PixelStorei, "glPixelStorei");
    RES(wglGetCurrentDC, "wglGetCurrentDC"); RES(wglGetCurrentContext, "wglGetCurrentContext"); RES(wglGetProcAddress, "wglGetProcAddress");
#undef RES
    if (!GfxReplaySetup(W, H)) { printf("renderer setup failed\n"); return 1; }

    // rebuild the world targets from the capture (PNG rows are top-down, GL rows bottom-up)
    std::vector<BYTE> bufScene((size_t)W * H * 4), bufEmis(bufScene.size()), bufSolid(bufScene.size()), bufNorm(bufScene.size());
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sy = H - 1 - y;
        size_t o = ((size_t)y * W + x) * 4;
        const BYTE* s = Px(scene, x, sy);
        const BYTE* e = Px(emis, x, sy);
        const BYTE* m = Px(masks, x * masks.w / W, sy * masks.h / H);
        const BYTE* nn = Px(norms, x * norms.w / W, sy * norms.h / H);
        BYTE pf = (BYTE)std::min(255, (int)(m[1] / 0.6f));
        for (int c = 0; c < 3; c++) { bufScene[o + c] = s[c]; bufEmis[o + c] = e[c]; bufNorm[o + c] = nn[c]; }
        bufScene[o + 3] = 255; bufEmis[o + 3] = pf;
        bufSolid[o] = bufSolid[o + 1] = bufSolid[o + 2] = 0; bufSolid[o + 3] = m[0];
        bufNorm[o + 3] = pf;
    }
    GLuint tex[4];
    GfxReplayTargets(tex);
    auto bindTex = (void (WINAPI*)(GLenum, GLuint))RealProc("glBindTexture");
    auto subImage = (void (WINAPI*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))RealProc("glTexSubImage2D");
    const std::vector<BYTE>* bufs[4] = { &bufScene, &bufEmis, &bufSolid, &bufNorm };
    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }

    // output target
    auto genFb = (void (WINAPI*)(GLsizei, GLuint*))RealProc("glGenFramebuffers");
    auto bindFb = (void (WINAPI*)(GLenum, GLuint))RealProc("glBindFramebuffer");
    auto fbTex = (void (WINAPI*)(GLenum, GLenum, GLenum, GLuint, GLint))RealProc("glFramebufferTexture2D");
    auto genTex = (void (WINAPI*)(GLsizei, GLuint*))RealProc("glGenTextures");
    auto texImage = (void (WINAPI*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))RealProc("glTexImage2D");
    auto finish = (void (WINAPI*)())RealProc("glFinish");
    GLuint outTex, outFbo;
    genTex(1, &outTex); bindTex(GL_TEXTURE_2D, outTex);
    texImage(GL_TEXTURE_2D, 0, 0x8058, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    genFb(1, &outFbo); bindFb(GL_FRAMEBUFFER, outFbo); fbTex(GL_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, outTex, 0);

    std::vector<BYTE> out((size_t)W * H * 4);
    for (int pass = 0; pass < 2; pass++) {
        bool lit = pass == 1;
        LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0);
        GfxReplayRun(world, outFbo, lit, 120);   // warm up: let the GPU reach full clocks
        finish();
        QueryPerformanceCounter(&t0);
        GfxReplayRun(world, outFbo, lit, 60);   // time it
        finish();
        QueryPerformanceCounter(&t1);
        printf("%s: %.2f ms per frame (post chain only)\n", lit ? "lighting on " : "lighting off", (t1.QuadPart - t0.QuadPart) * 1000.0 / f0.QuadPart / 60);
        GfxReplayProfile(true);
        GfxReplayRun(world, outFbo, lit, 1);
        finish();
        GfxReplayPrintProfile();
        GfxReplayProfile(false);
        bindFb(GL_FRAMEBUFFER, outFbo);
        R.ReadBuffer(0x8CE0);
        R.PixelStorei(GL_PACK_ALIGNMENT, 4);
        R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        SavePng(wic, dir + (lit ? L"replay_on.png" : L"replay_off.png"), W, H, out);
    }
    // effects sequence on the same frame: an explosion, then a lightning strike in a storm
    {
        GfxReplayWeather(3);
        float wx = world[0] + 0.62f * (world[1] - world[0]), wy = world[2] + 0.62f * (world[3] - world[2]);
        float r = 0.08f * fabsf(world[3] - world[2]);
        double tm = 10.0;
        const int shots[] = { 3, 4, 6, 12, 24, 45, 61, 64, 80 };
        for (int fr = 0; fr <= 80; fr++) {
            GfxReplaySetTime(tm);
            if (fr < 3) GfxReplayInject(1, wx, wy, r);
            if (fr == 60) GfxReplayLightning(world[0] + 0.33f * (world[1] - world[0]));
            for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }   // the game redraws the world every frame
            GfxReplayRun(world, outFbo, true, 1);
            for (int s : shots) if (s == fr) {
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                SavePng(wic, dir + L"replay_fx_" + std::to_wstring(fr) + L".png", W, H, out);
            }
            tm += 1.0 / 60.0;
        }
        printf("wrote replay_fx_*.png\n");
    }
    // 3D backdrop preview: palette from the captured backdrop layer textures, playfield put back on top
    if (argc > 3 && !wcscmp(argv[3], L"miko")) { GfxReplayFuji((world[0] + world[1]) * 0.5f); GfxReplayTune("Fun.Voice", 0); }   // with Mount Fuji, quietly
    {
        std::ifstream ft2(dir + L"frame.txt");
        std::string ln;
        std::vector<unsigned> ids;
        std::regex drawRe("DRAW .*tex=(\\d+)");
        while (std::getline(ft2, ln)) {
            if (ln.find("glLoadMatrixf") != std::string::npos) break;
            std::smatch m;
            if (std::regex_search(ln, m, drawRe)) {
                unsigned id = (unsigned)std::stoul(m[1]);
                if (ids.empty() || ids.back() != id) ids.push_back(id);
            }
        }
        std::vector<GLuint> layerTex;
        for (unsigned id : ids) {
            WIN32_FIND_DATAW fd;
            wchar_t pat[64]; swprintf(pat, 64, L"tex\\tex_%05u_*.png", id);
            HANDLE fh = FindFirstFileW((dir + pat).c_str(), &fd);
            if (fh == INVALID_HANDLE_VALUE) continue;
            FindClose(fh);
            Img im;
            if (!LoadPng(wic, dir + L"tex\\" + fd.cFileName, im)) continue;
            std::vector<BYTE> flipped(im.px.size());   // GL rows bottom-up
            for (int y = 0; y < im.h; y++) memcpy(&flipped[(size_t)y * im.w * 4], &im.px[(size_t)(im.h - 1 - y) * im.w * 4], (size_t)im.w * 4);
            GLuint t2; genTex(1, &t2); bindTex(GL_TEXTURE_2D, t2);
            texImage(GL_TEXTURE_2D, 0, 0x8058, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
            auto texParam = (void (WINAPI*)(GLenum, GLenum, GLint))RealProc("glTexParameteri");
            texParam(GL_TEXTURE_2D, 0x2801, 0x2601); texParam(GL_TEXTURE_2D, 0x2800, 0x2601);
            layerTex.push_back(t2);
        }
        printf("backdrop preview: %zu backdrop layers found in the capture\n", layerTex.size());
        for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
        GfxReplaySetTime(30.0);
        GfxReplayWeather(1);
        GfxReplayBackdrop(layerTex.data(), (int)layerTex.size());
        auto getTex = (void (WINAPI*)(GLenum, GLint, GLenum, GLenum, void*))RealProc("glGetTexImage");
        std::vector<BYTE> bd((size_t)W * H * 4);
        bindTex(GL_TEXTURE_2D, tex[0]);
        getTex(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bd.data());
        std::vector<BYTE> mixed(bufScene);
        for (size_t p = 0; p < (size_t)W * H; p++) {
            float pf = bufEmis[p * 4 + 3] / 255.0f;
            for (int c = 0; c < 3; c++) mixed[p * 4 + c] = (BYTE)(bd[p * 4 + c] * (1 - pf) + bufScene[p * 4 + c] * pf);
        }
        bindTex(GL_TEXTURE_2D, tex[0]);
        subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, mixed.data());
        GfxReplayRun(world, outFbo, true, 1);
        finish();
        bindFb(GL_FRAMEBUFFER, outFbo);
        R.ReadBuffer(0x8CE0);
        R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        SavePng(wic, dir + L"replay_backdrop.png", W, H, out);
        printf("wrote replay_backdrop.png\n");

        // the control panel and lab bar over the frame, at the capture's resolution: replay_ui.png
        if (argc > 3 && !wcscmp(argv[3], L"ui")) {
            for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
            GfxReplayRun(world, outFbo, true, 1);
            bindFb(GL_FRAMEBUFFER, outFbo);
            OverlayPreview(W, H, true, true, true);
            finish();
            bindFb(GL_FRAMEBUFFER, outFbo);
            R.ReadBuffer(0x8CE0);
            R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            SavePng(wic, dir + L"replay_ui.png", W, H, out);
            printf("wrote replay_ui.png\n");
        }
        // the simulated puffs side by side - steam, smoke, dust, a gun's hot-gas blast: replay_puffs.png
        if (argc > 3 && !wcscmp(argv[3], L"puffs")) {
            GfxReplayWeather(1);
            double tm = 1500.0;
            float vw = world[1] - world[0], vh = world[3] - world[2];
            const int kinds[4] = { 8, 9, 11, 12 };
            for (int f = 0; f < 110; f++) {
                GfxReplaySetTime(tm);
                if (f % 12 == 0 && f < 80)
                    for (int k = 0; k < 4; k++) GfxReplayPuff(kinds[k], world[0] + vw * (0.2f + 0.2f * k), world[2] + vh * 0.45f, fabsf(vh) * 0.035f);
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
            }
            finish();
            bindFb(GL_FRAMEBUFFER, outFbo);
            R.ReadBuffer(0x8CE0);
            R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            SavePng(wic, dir + L"replay_puffs.png", W, H, out);
            printf("wrote replay_puffs.png\n");
        }
        // calibrate: for each built-in look, the ambient level that brings its structures to a target
        // brightness relative to the original game (neutral looks ~1, moody ones lower but readable).
        if (argc > 3 && !wcscmp(argv[3], L"calibrate")) {
            static const float target[24] = { 1.0f, 0.85f, 0.97f, 0.97f, 0.92f, 0.82f, 0.45f, 0.65f, 0.6f, 0.55f, 0.6f, 0.55f,
                                               0.0f, 0.92f, 0.95f, 0.6f, 0.9f, 1.0f, 0.95f, 0.92f, 0.8f, 0.92f, 0.6f, 0.8f };
            GfxReplayWeather(1);
            auto measure = [&]() {
                double tm = 1000.0;
                for (int f = 0; f < 40; f++) {
                    GfxReplaySetTime(tm);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                double lit = 0, base = 0;
                for (size_t o = 0; o < out.size(); o += 4)
                    if (bufSolid[o + 3] > 200) {
                        lit += out[o] * 0.2126 + out[o + 1] * 0.7152 + out[o + 2] * 0.0722;
                        base += bufScene[o] * 0.2126 + bufScene[o + 1] * 0.7152 + bufScene[o + 2] * 0.0722;
                    }
                return base > 0 ? lit / base : 0.0;
            };
            for (int li = 0; li < GfxLookCount() && li < 24; li++) {
                GfxApplyLook(li, false);
                GfxReplayTune("DayNight.Enabled", 0);
                GfxReplayTune("Weather.Mode", 1);
                float a0 = GfxReplayGet("Materials.AmbientLevel");
                double r0 = measure();
                float lo = a0, hi = 2.0f, best = a0;
                double rb = r0;
                if (target[li] > 0 && r0 < target[li] - 0.02) {
                    for (int it = 0; it < 7; it++) {
                        float mid = (lo + hi) * 0.5f;
                        GfxReplayTune("Materials.AmbientLevel", mid);
                        double r = measure();
                        if (r < target[li]) lo = mid; else hi = mid;
                        if (fabs(r - target[li]) < fabs(rb - target[li])) { rb = r; best = mid; }
                    }
                }
                printf("calib %2d %-18s ambient %.3f -> %.3f   structures %.3f -> %.3f (target %.2f)\n", li, GfxLookName(li), a0, best, r0, rb, target[li]);
            }
        }
        // water: the water cannon's jet as the game reports it (its shots' water_trail effects), fired from the left
        // at the game's speeds: the continuous stream. replay_water_a/b.png (b: with the milk miko on).
        if (argc > 3 && !wcscmp(argv[3], L"water")) {
            GfxReplayWeather(1);
            GfxReplayTune("DayNight.Enabled", 0);
            GfxReplayTune("Fun.Voice", 0);
            float vw = world[1] - world[0], vh = world[3] - world[2];
            float nx = world[0] + 0.15f * vw, ny = world[2] + 0.55f * vh;   // the nozzle
            float g = 1000.0f, sp = 2200.0f, ang = -0.35f;                   // world y grows down: up and to the right
            for (int pass = 0; pass < 2; pass++) {
                GfxReplayTune("Fun.MilkMiko", pass == 1 ? 1.0f : 0.0f);
                struct Shot { uint32_t key; float t0; };
                std::vector<Shot> shots;
                uint32_t key = 1000 + pass * 10000;
                double tm = 1300.0 + pass * 100.0;
                g_replayTracking = true;
                for (int f = 0; f < 150; f++) {
                    GfxReplaySetTime(tm);
                    float now = f / 60.0f;
                    if (f < 120 && f % 3 == 0) shots.push_back({ key++, now });   // a shot every 50 ms
                    g_replayTrails.clear();
                    for (auto& s : shots) {
                        float t = now - s.t0;
                        if (t > 0.9f) continue;   // landed
                        EngineTrail tr;
                        tr.key = s.key;
                        tr.vx = cosf(ang) * sp; tr.vy = sinf(ang) * sp + g * t;
                        tr.x = nx + cosf(ang) * sp * t; tr.y = ny + sinf(ang) * sp * t + 0.5f * g * t * t;
                        snprintf(tr.name, sizeof(tr.name), "mods/dlc3/effects/water_trail.lua");
                        g_replayTrails.push_back(tr);
                    }
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                    if (f == 100) {
                        finish();
                        bindFb(GL_FRAMEBUFFER, outFbo);
                        R.ReadBuffer(0x8CE0);
                        R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                        std::wstring name = pass == 0 ? L"replay_water_a.png" : L"replay_water_b.png";
                        SavePng(wic, dir + name, W, H, out);
                        printf("wrote %ls\n", name.c_str());
                    }
                }
                g_replayTracking = false;
                g_replayTrails.clear();
            }
        }
        // miko: the milk miko joke without the game's water cannon - a miko standing on a surface on the left,
        // her water shots (as the game draws them) arcing to the right and splashing: the milk, sakura, manga
        // words, sparkles, Mount Fuji. Needs <ini dir>\FortsGFX_fun\ (fun\anime). replay_miko_a/b.png
        if (argc > 3 && !wcscmp(argv[3], L"miko")) {
            GfxReplayWeather(1);
            GfxReplayTune("DayNight.Enabled", 0);
            float vw = world[1] - world[0], vh = world[3] - world[2];
            int mx = (int)(W * 0.25f), my = (int)(H * 0.5f);   // a surface point for her, near the left third
            for (int y = H / 2; y > 0; y -= 2) if (bufSolid[((size_t)y * W + mx) * 4 + 3] > 200) { my = y; break; }
            float kx = world[0] + (mx + 0.5f) / W * vw, ky = world[2] + (my + 0.5f) / H * vh;   // world: her feet
            float r = fabsf(vh) * 0.05f;
            struct Shot { float t0; };
            std::vector<Shot> shots;
            double tm = 1200.0;
            for (int f = 0; f < 260; f++) {
                GfxReplaySetTime(tm);
                GfxReplayInject(9, kx, ky - r, r);                        // the miko (her body sprite)
                if (f >= 20 && f < 200 && f % 3 == 0) shots.push_back({ (float)f });
                for (auto& s : shots) {                                    // each shot: an arc to the right
                    float t = (f - s.t0) / 60.0f;
                    if (t < 0 || t > 0.9f) continue;
                    float x = kx + t * vw * 0.45f, y = ky - r * 1.5f + (-0.5f * t + 0.75f * t * t) * fabsf(vh) * (vh < 0 ? -1.0f : 1.0f) * -1.0f;
                    GfxReplayInject(11, x, y, r * 0.25f);
                }
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
                if (f == 70 || f == 230) {
                    finish();
                    bindFb(GL_FRAMEBUFFER, outFbo);
                    R.ReadBuffer(0x8CE0);
                    R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                    std::wstring name = f == 70 ? L"replay_miko_a.png" : L"replay_miko_b.png";
                    SavePng(wic, dir + name, W, H, out);
                    printf("wrote %ls (miko at px %d, %d)\n", name.c_str(), mx, H - 1 - my);
                }
            }
        }
        // looks: every built-in look on this frame at noon - how bright the structures come out after
        // lighting and grading, relative to the game's own unlit frame (1.0 = as bright as the original).
        if (argc > 3 && !wcscmp(argv[3], L"looks")) {
            GfxReplayWeather(1);
            for (int li = 0; li < GfxLookCount(); li++) {
                GfxApplyLook(li, false);
                GfxReplayTune("DayNight.Enabled", 0);
                GfxReplayTune("Weather.Mode", 1);
                double tm = 1000.0;
                for (int f = 0; f < 60; f++) {
                    GfxReplaySetTime(tm);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                double lit = 0, base = 0;
                for (size_t o = 0; o < out.size(); o += 4)
                    if (bufSolid[o + 3] > 200) {
                        lit += out[o] * 0.2126 + out[o + 1] * 0.7152 + out[o + 2] * 0.0722;
                        base += bufScene[o] * 0.2126 + bufScene[o + 1] * 0.7152 + bufScene[o + 2] * 0.0722;
                    }
                printf("look %2d %-18s structures %.3f\n", li, GfxLookName(li), base > 0 ? lit / base : 0.0);
            }
        }
        // zoom: the same frame shrunk into the middle, as if zoomed out 2x and 3x, to see whether the lit
        // structures stay as bright. Prints the lit/unlit brightness of the solid parts at each zoom and writes
        // replay_zoom_<n>.png. A 5th argument tunes one setting first, e.g. Lighting.EmitterBrightness=0.
        if (argc > 3 && !wcscmp(argv[3], L"zoom")) {
            GfxReplayWeather(1);
            GfxReplayTune("DayNight.Enabled", 0);
            if (argc > 4) {   // settings, "Section.Key=value;..."
                std::string all = Narrow(argv[4]);
                for (size_t s = 0; s < all.size();) {
                    size_t e = all.find(';', s);
                    std::string a = all.substr(s, e == std::string::npos ? std::string::npos : e - s);
                    size_t eq = a.find('=');
                    if (eq != std::string::npos) { GfxReplayTune(a.substr(0, eq).c_str(), std::stof(a.substr(eq + 1))); printf("zoom: %s\n", a.c_str()); }
                    if (e == std::string::npos) break;
                    s = e + 1;
                }
            }
            std::vector<BYTE> glowScene = bufScene, glowEmis = bufEmis;   // "glow": a reactor-like light on the fort
            bool glow = argc > 5 && !wcscmp(argv[5], L"glow");
            if (glow) {
                int gx = W / 2, gy = H / 2, bd = INT_MAX;
                for (int y = 0; y < H; y += 4) for (int x = 0; x < W; x += 4)
                    if (bufSolid[((size_t)y * W + x) * 4 + 3] > 200) {
                        int d = (x - W / 2) * (x - W / 2) + (y - H / 2) * (y - H / 2);
                        if (d < bd) { bd = d; gx = x; gy = y; }
                    }
                for (int y = gy - 40; y <= gy + 40; y++) for (int x = gx - 40; x <= gx + 40; x++) {
                    if (x < 0 || y < 0 || x >= W || y >= H || (x - gx) * (x - gx) + (y - gy) * (y - gy) > 1600) continue;
                    size_t o = ((size_t)y * W + x) * 4;
                    glowEmis[o] = 255; glowEmis[o + 1] = 170; glowEmis[o + 2] = 80;
                    glowScene[o] = 255; glowScene[o + 1] = 200; glowScene[o + 2] = 120;
                }
                printf("zoom: glow at px (%d, %d)\n", gx, H - 1 - gy);
                bufs[0] = &glowScene; bufs[1] = &glowEmis;
            }
            const float zooms[3] = { 1.0f, 0.5f, 0.33f };
            std::vector<BYTE> zs[4];
            for (int zi = 0; zi < 3; zi++) {
                float z = zooms[zi], cx = W * 0.5f, cy = H * 0.5f;
                int blk = std::max(1, (int)lroundf(1.0f / z));
                for (int i = 0; i < 4; i++) zs[i].assign((size_t)W * H * 4, 0);
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                    float sx = cx + (x - cx) / z, sy = cy + (y - cy) / z;
                    size_t o = ((size_t)y * W + x) * 4;
                    bool in = sx >= 0 && sy >= 0 && sx < W && sy < H;
                    int bx = std::min(std::max((int)sx, 0), W - 1), by = std::min(std::max((int)sy, 0), H - 1);
                    for (int i = 0; i < 4; i++) {
                        const std::vector<BYTE>& src = *bufs[i];
                        if (!in) {   // outside the shrunk frame: the backdrop stretched, nothing solid or glowing
                            size_t so = ((size_t)by * W + bx) * 4;
                            if (i == 0) { for (int c = 0; c < 4; c++) zs[0][o + c] = src[so + c]; }
                            else if (i == 3) { zs[3][o] = zs[3][o + 1] = 128; }
                            continue;
                        }
                        int acc[4] = {}, n = 0;   // box-average the source pixels that land here
                        for (int dy = 0; dy < blk; dy++) for (int dx = 0; dx < blk; dx++) {
                            int px = std::min(bx + dx, W - 1), py = std::min(by + dy, H - 1);
                            size_t so = ((size_t)py * W + px) * 4;
                            for (int c = 0; c < 4; c++) acc[c] += src[so + c];
                            n++;
                        }
                        for (int c = 0; c < 4; c++) zs[i][o + c] = (BYTE)(acc[c] / n);
                    }
                }
                float zw[4] = { (world[0] + world[1]) * 0.5f - (world[1] - world[0]) * 0.5f / z, (world[0] + world[1]) * 0.5f + (world[1] - world[0]) * 0.5f / z,
                                (world[2] + world[3]) * 0.5f - (world[3] - world[2]) * 0.5f / z, (world[2] + world[3]) * 0.5f + (world[3] - world[2]) * 0.5f / z };
                double tm = 900.0;
                for (int f = 0; f < 90; f++) {   // let the lighting settle
                    GfxReplaySetTime(tm);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, zs[i].data()); }
                    GfxReplayRun(zw, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                double lit = 0, base = 0, litBg = 0, baseBg = 0;
                long n = 0, nbg = 0;
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                    size_t o = ((size_t)y * W + x) * 4;
                    double lo = out[o] * 0.2126 + out[o + 1] * 0.7152 + out[o + 2] * 0.0722, ls = zs[0][o] * 0.2126 + zs[0][o + 1] * 0.7152 + zs[0][o + 2] * 0.0722;
                    if (zs[2][o + 3] > 200) { lit += lo; base += ls; n++; }
                    else if (zs[2][o + 3] < 10) { litBg += lo; baseBg += ls; nbg++; }
                }
                printf("zoom %.2f: solid parts lit/unlit %.3f (%ld px, mean lit %.1f), open parts %.3f\n", z, base > 0 ? lit / base : 0.0, n, n ? lit / n : 0.0,
                       baseBg > 0 ? litBg / baseBg : 0.0);
                SavePng(wic, dir + L"replay_zoom_" + std::to_wstring(zi) + L".png", W, H, out);
            }
        }
        // effects from the game's own events, fed in as the engine watch would report them: impacts on each
        // material, muzzles, a laser cutting, device deaths, a heavy hit. replay_gamefx_a.png (in the middle
        // of it) and replay_gamefx_b.png (the aftermath: scorch marks, smoke, glows).
        if (argc > 3 && !wcscmp(argv[3], L"gamefx")) {
            bool off = argc > 4 && !wcscmp(argv[4], L"off");   // the same, without these effects (for comparing)
            if (off) GfxReplayTune("GameFx.Enabled", 0.0f);
            GfxReplayWeather(1);
            double tm = 1700.0;
            float vw = world[1] - world[0], vh = world[3] - world[2];
            auto at = [&](float fx, float fy, bool solid, float& wx, float& wy) {   // a point near (fx, fy) of the view; on a surface if asked
                int cx = (int)(fx * W), cy = (int)((1.0f - fy) * H), bx = cx, by = cy;
                float bd = 1e30f;
                if (solid)
                    for (int y = std::max(0, cy - 900); y < std::min(H, cy + 900); y += 4)
                        for (int x = std::max(0, cx - 900); x < std::min(W, cx + 900); x += 4)
                            if (bufSolid[((size_t)y * W + x) * 4 + 3] > 200) {
                                float d = (float)((x - cx) * (x - cx) + (y - cy) * (y - cy));
                                if (d < bd) { bd = d; bx = x; by = y; }
                            }
                wx = world[0] + (bx + 0.5f) / W * vw;
                wy = world[2] + (by + 0.5f) / H * vh;
                printf("gamefx point (%.2f, %.2f) -> image px (%d, %d)%s\n", fx, fy, bx, H - 1 - by, solid && bd > 1e29f ? " (no surface near)" : "");
            };
            auto fx = [&](uint8_t kind, const char* name, float x, float y, float dx = 0, float dy = 0, int team = 1, const char* hit = "") {
                EngineFx e;
                e.kind = kind; e.x = x; e.y = y; e.dx = dx; e.dy = dy; e.hasDir = dx != 0 || dy != 0; e.team = team;
                snprintf(e.name, sizeof(e.name), "%s", name);
                snprintf(e.hit, sizeof(e.hit), "%s", hit);
                g_replayFx.push_back(e);
            };
            float ax, ay, bx2, by2, cx2, cy2, dx2, dy2, ex, ey, lx, ly, gx, gy, hx, hy, ix, iy, jx, jy, kx, ky;
            at(0.18f, 0.55f, true, ax, ay);   // metal
            at(0.35f, 0.45f, true, bx2, by2); // wood
            at(0.55f, 0.35f, false, cx2, cy2);// shield (in the air)
            at(0.50f, 0.85f, true, dx2, dy2); // ground
            at(0.70f, 0.40f, true, ex, ey);   // cannon
            at(0.12f, 0.30f, false, lx, ly);  // laser emitter
            at(0.30f, 0.62f, true, gx, gy);   // where the laser cuts
            at(0.82f, 0.60f, true, hx, hy);   // a battery dies
            at(0.62f, 0.62f, true, ix, iy);   // a reactor dies
            at(0.88f, 0.30f, true, jx, jy);   // a minigun keeps firing
            at(0.42f, 0.70f, true, kx, ky);   // a strut breaks
            for (int f = 0; f < 200; f++) {
                GfxReplaySetTime(tm);
                if (f == 2) {
                    fx(EngineFx::Impact, "cannon", ax, ay, 0, -1, 2, "armour");
                    fx(EngineFx::Effect, "effects/impact_heavy.lua", ax, ay, 0, -1);
                    fx(EngineFx::Impact, "mortar", bx2, by2, 0, -1, 2, "bracing");
                    fx(EngineFx::Effect, "effects/impact_medium.lua", bx2, by2, 0, -1);
                    fx(EngineFx::Effect, "effects/impact_shield.lua", cx2, cy2, -1, 0);
                    fx(EngineFx::Effect, "effects/impact_heavy_ground.lua", dx2, dy2, 0, -1);
                    fx(EngineFx::Effect, "effects/fire_cannon.lua", ex, ey, -0.95f, -0.3f);
                }
                if (f == 0) {   // a laser charging (its reload effect), a warhead silo warming up
                    fx(EngineFx::Effect, "effects/laser_reload.lua", lx, ly, 0, 0, 1);
                    fx(EngineFx::Effect, "effects/warhead_warmup.lua", dx2, dy2 - 40.0f, 0, -1, 1);
                }
                if (f == 150) fx(EngineFx::Effect, "effects/fire_laser.lua", lx, ly, 0, 0, 1);
                if (f >= 152 && f < 190) fx(EngineFx::Effect, "effects/beam_hit_armour.lua", gx, gy, 0, 0, 1);
                if (f == 160) fx(EngineFx::Effect, "effects/warhead_launch.lua", dx2, dy2 - 40.0f, 0, -1, 1);
                g_replayTracking = !off;   // a cannon shell crossing the view at the game's 6000 units/s
                g_replayTrails.clear();
                if (f >= 30 && f < 60) {
                    EngineTrail t;
                    t.key = 77; t.x = world[0] + (f - 30) / 60.0f * 6000.0f * 0.25f; t.y = world[2] + 0.5f * vh; t.vx = 6000.0f; t.vy = 0;
                    snprintf(t.name, sizeof(t.name), "effects/cannon_trail.lua");
                    g_replayTrails.push_back(t);
                }
                if (f == 12) {
                    fx(EngineFx::Device, "battery", 0, 0, 0, 0, 2);
                    fx(EngineFx::Effect, "effects/battery_explode.lua", hx, hy);
                    fx(EngineFx::Device, "reactor", 0, 0, 0, 0, 2);
                    fx(EngineFx::Effect, "effects/device_explode_large_util.lua", ix, iy);
                }
                if (f < 120 && f % 3 == 0) fx(EngineFx::Effect, "effects/fire_minigun.lua", jx, jy, -1, 0);
                if (f == 20) fx(EngineFx::Effect, "effects/bracing_destroy.lua", kx, ky);
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
                if (f == 20 || f == 199 || f == 140 || f == 175) {
                    finish();
                    bindFb(GL_FRAMEBUFFER, outFbo);
                    R.ReadBuffer(0x8CE0);
                    R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                    std::wstring name = std::wstring(f == 20 ? L"replay_gamefx_a" : f == 140 ? L"replay_gamefx_charge" : f == 175 ? L"replay_gamefx_beam" : L"replay_gamefx_b") + (off ? L"_off.png" : L".png");
                    SavePng(wic, dir + name, W, H, out);
                    printf("wrote %ls\n", name.c_str());
                }
            }
        }
        // blast: what a hit leaves behind instead of the shockwave - smoke hanging about is shoved by the blast wind,
        // dust and grit come off the structure, debris in the colours of what was hit, a dust haze, a ground blast.
        // Three hits at once (a missile into bracing, a cannon shell on armour, a heavy shell into the ground), the
        // shockwave off. replay_blast_<n>.png: 0 before, 1 the push, 2 the shower, 3 the haze, 4 settled
        // ("off": the same without these effects).
        if (argc > 3 && !wcscmp(argv[3], L"blast")) {
            bool off = argc > 4 && !wcscmp(argv[4], L"off");
            GfxReplayWeather(1);
            GfxReplayTune("DayNight.Enabled", 0);
            GfxReplayTune("Impact.Shockwave", 0);
            if (off) {
                GfxReplayTune("GameFx.BlastWind", 0); GfxReplayTune("GameFx.ShakeOff", 0); GfxReplayTune("GameFx.HitColour", 0);
                GfxReplayTune("GameFx.DustHaze", 0); GfxReplayTune("GameFx.GroundBlast", 0);
            }
            double tm = 1800.0;
            float vw = world[1] - world[0], vh = world[3] - world[2];
            auto at = [&](float fx, float fy, float& wx, float& wy) {   // the surface pixel nearest (fx, fy) of the view
                int cx = (int)(fx * W), cy = (int)((1.0f - fy) * H), bx = cx, by = cy;
                float bd = 1e30f;
                for (int y = std::max(0, cy - 900); y < std::min(H, cy + 900); y += 4)
                    for (int x = std::max(0, cx - 900); x < std::min(W, cx + 900); x += 4)
                        if (bufSolid[((size_t)y * W + x) * 4 + 3] > 200) {
                            float d = (float)((x - cx) * (x - cx) + (y - cy) * (y - cy));
                            if (d < bd) { bd = d; bx = x; by = y; }
                        }
                wx = world[0] + (bx + 0.5f) / W * vw;
                wy = world[2] + (by + 0.5f) / H * vh;
                printf("blast point (%.2f, %.2f) -> image px (%d, %d)\n", fx, fy, bx, H - 1 - by);
            };
            auto fx = [&](uint8_t kind, const char* name, float x, float y, float dx, float dy, const char* hit) {
                EngineFx e;
                e.kind = kind; e.x = x; e.y = y; e.dx = dx; e.dy = dy; e.hasDir = dx != 0 || dy != 0; e.team = 2;
                snprintf(e.name, sizeof(e.name), "%s", name);
                snprintf(e.hit, sizeof(e.hit), "%s", hit);
                g_replayFx.push_back(e);
            };
            for (size_t q = 0; q < (size_t)W * H; q++) bufSolid[q * 4] = bufSolid[q * 4 + 3];   // old captures: every solid is front
            float ax, ay, bx2, by2, cx2, cy2, pt[6] = { 0.35f, 0.45f, 0.70f, 0.40f, 0.50f, 0.85f };   // or given: blast on|off ax ay bx by cx cy
            for (int i = 0; i < 6 && argc > 5 + i; i++) pt[i] = (float)_wtof(argv[5 + i]);
            at(pt[0], pt[1], ax, ay);    // bracing
            at(pt[2], pt[3], bx2, by2);  // armour
            at(pt[4], pt[5], cx2, cy2);  // ground
            float sr = fabsf(vh) * 0.03f;
            for (int f = 0; f < 320; f++) {
                GfxReplaySetTime(tm);
                if (f < 58 && f % 6 == 0) {   // smoke hanging about near each (a damaged device, a mortar's trail)
                    GfxReplayPuff(9, ax - 0.06f * vw, ay - 0.04f * fabsf(vh), sr);
                    GfxReplayPuff(9, ax + 0.05f * vw, ay - 0.08f * fabsf(vh), sr);
                    GfxReplayPuff(8, bx2 - 0.05f * vw, by2 - 0.02f * fabsf(vh), sr);
                    GfxReplayPuff(9, cx2 + 0.07f * vw, cy2 - 0.05f * fabsf(vh), sr);
                }
                if (f == 60) {
                    fx(EngineFx::Impact, "missile", ax, ay, 0, -1, "bracing");
                    fx(EngineFx::Effect, "effects/impact_heavy.lua", ax, ay, 0, -1, "");
                    fx(EngineFx::Impact, "cannon", bx2, by2, -1, 0, "armour");
                    fx(EngineFx::Effect, "effects/impact_medium_armor.lua", bx2, by2, -1, 0, "");
                    fx(EngineFx::Effect, "effects/impact_heavy_ground.lua", cx2, cy2, 0, -1, "");
                }
                if (f >= 60 && f < 62) {   // the game's blast sprites over the first two
                    GfxReplayInject(1, ax, ay, fabsf(vh) * 0.04f);
                    GfxReplayInject(1, bx2, by2, fabsf(vh) * 0.03f);
                }
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
                if (f == 59 || f == 60 || f == 61 || f == 66 || f == 90 || f == 180) {
                    int n[24];
                    GfxReplayParticleCounts(n);
                    printf("frame %3d particles: debris %d (coloured %d), splinters %d, grit %d (coloured %d), candidates %d, dust %d, smoke %d, hot gas %d, embers %d\n",
                           f, n[1], n[22], n[13], n[18], n[23], n[19], n[11], n[9], n[12], n[0]);
                }
                int shot = f == 58 ? 0 : f == 66 ? 1 : f == 90 ? 2 : f == 180 ? 3 : f == 319 ? 4 : -1;
                if (shot >= 0) {
                    finish();
                    bindFb(GL_FRAMEBUFFER, outFbo);
                    R.ReadBuffer(0x8CE0);
                    R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                    std::wstring name = L"replay_blast_" + std::to_wstring(shot) + (off ? L"_off.png" : L".png");
                    SavePng(wic, dir + name, W, H, out);
                    printf("wrote %ls\n", name.c_str());
                }
            }
        }
        // the showcase tour: a few chapters OFF and ON with their captions: replay_tour_*.png
        if (argc > 3 && !wcscmp(argv[3], L"tour")) {
            struct Shot { int chapter; double t; const char* name; };
            const Shot shots[] = { { 0, 4.0, "00_intro_wipe" }, { 1, 1.0, "01_lighting_off" }, { 1, 3.5, "01_lighting_on" },
                                   { 2, 1.0, "02_materials_off" }, { 2, 3.5, "02_materials_on" }, { 4, 3.5, "04_bloom_on" },
                                   { 13, 2.0, "13_end" } };
            double tm = 1200.0;
            for (const Shot& s : shots) {
                for (int f = 0; f < 30; f++) {   // let the lighting settle on the new settings
                    GfxReplaySetTime(tm);
                    OverlayTourSetup(W, H, s.chapter, s.t);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                bindFb(GL_FRAMEBUFFER, outFbo);
                OverlayTourDraw(W, H);
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                SavePng(wic, dir + L"replay_tour_" + std::wstring(s.name, s.name + strlen(s.name)) + L".png", W, H, out);
                printf("wrote replay_tour_%s.png\n", s.name);
            }
        }
        // weather on a real frame: wet surfaces + puddles, snow, blizzard: replay_rain/snow/blizzard.png
        if (argc > 3 && !wcscmp(argv[3], L"weather")) {
            const wchar_t* names[3] = { L"replay_rain.png", L"replay_snow.png", L"replay_blizzard.png" };
            const int modes[3] = { 2, 4, 5 };
            for (int w = 0; w < 3; w++) {
                GfxReplayWeather(modes[w]);
                GfxReplayWet(w == 0 ? 1.0f : 0.0f);
                GfxReplaySnowCover(w == 0 ? 0.0f : 1.0f);
                GfxReplayTune("Weather.SnowBuildUp", 5.0f);   // a minute of snowfall in a few seconds
                // old captures have no front-surface mask: treat every solid as front
                for (size_t q = 0; q < (size_t)W * H; q++) bufSolid[q * 4] = bufSolid[q * 4 + 3];
                double tm = 700.0 + w * 50.0;
                int frames = w == 0 ? 150 : 900;
                for (int f = 0; f < frames; f++) {   // let rain and snow fill the view (and snow pile up)
                    GfxReplaySetTime(tm);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                SavePng(wic, dir + names[w], W, H, out);
                printf("wrote %ls\n", names[w]);
            }
        }
        // a High Seas capture: the sea counted as playfield (as the game's wave mesh is now classified) with
        // the water effects at its line, in rain, snow and blizzard: replay_sea_*.png. Arg 4: the water line.
        if (argc > 3 && !wcscmp(argv[3], L"sea")) {
            float level = argc > 4 ? (float)_wtof(argv[4]) : 5190.0f;
            for (int y = 0; y < H; y++) {
                float wy = world[2] + (y + 0.5f) / H * (world[3] - world[2]);
                if (wy > level) for (int x = 0; x < W; x++) bufEmis[((size_t)y * W + x) * 4 + 3] = 255;
            }
            for (size_t q = 0; q < (size_t)W * H; q++) bufSolid[q * 4] = bufSolid[q * 4 + 3];
            GfxReplayWater(true, level);
            const char* names[3] = { "rain", "snow", "blizzard" };
            const int modes[3] = { 2, 4, 5 };
            double tm = 900.0;
            for (int w = 0; w < 3; w++) {
                GfxReplayWeather(modes[w]);
                GfxReplayWet(w == 0 ? 1.0f : 0.0f);
                GfxReplaySnowCover(w == 0 ? 0.0f : 1.0f);
                for (int f = 0; f < 240; f++) {
                    GfxReplaySetTime(tm);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                SavePng(wic, dir + L"replay_sea_" + std::wstring(names[w], names[w] + strlen(names[w])) + L".png", W, H, out);
                printf("wrote replay_sea_%s.png\n", names[w]);
            }
        }
        // a missile flying across with its engine plume, and a steam vent: replay_rocket.png
        if (argc > 3 && !wcscmp(argv[3], L"rocket")) {
            GfxReplayWeather(1);
            double tm = 900.0;
            float vw = world[1] - world[0], vh = world[3] - world[2];
            for (int f = 0; f < 120; f++) {
                GfxReplaySetTime(tm);
                float u = f / 120.0f;
                float mx = world[0] + vw * (0.15f + 0.7f * u), my = world[2] + vh * (0.55f + 0.25f * sinf(u * 3.0f));
                GfxReplayInject(5, mx, my, fabsf(vh) * 0.012f);                                            // plume (missiletail)
                if (f % 6 == 0) GfxReplayInject(6, world[0] + vw * 0.8f, world[2] + vh * 0.35f, fabsf(vh) * 0.03f);   // steam
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
            }
            finish();
            bindFb(GL_FRAMEBUFFER, outFbo);
            R.ReadBuffer(0x8CE0);
            R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            SavePng(wic, dir + L"replay_rocket.png", W, H, out);
            printf("wrote replay_rocket.png\n");
            // a warhead coming down with the game's own trail: its Steam puffs drawn as one GL_QUADS batch
            // (missile_trail.lua), which the simulated puffs must follow puff by puff: replay_rocket_trail.png
            struct Puff { float x, y, t0; };
            std::vector<Puff> trail;
            for (int f = 0; f < 150; f++) {
                GfxReplaySetTime(tm);
                float u = f / 150.0f;
                float mx = world[0] + vw * (0.3f + 0.35f * u), my = world[2] + vh * (0.9f - 0.6f * u);   // diving down and right
                GfxReplayInject(5, mx, my, fabsf(vh) * 0.012f);
                trail.push_back({ mx, my, (float)tm });
                std::vector<float> xyr;
                for (auto& p : trail) {
                    float age = (float)tm - p.t0;
                    if (age < 1.5f) { xyr.push_back(p.x); xyr.push_back(p.y); xyr.push_back(fabsf(vh) * (0.008f + 0.012f * age / 1.5f)); }
                }
                GfxReplayParticleBatch(6, xyr.data(), (int)xyr.size() / 3);
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
            }
            finish();
            bindFb(GL_FRAMEBUFFER, outFbo);
            R.ReadBuffer(0x8CE0);
            R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            SavePng(wic, dir + L"replay_rocket_trail.png", W, H, out);
            printf("wrote replay_rocket_trail.png\n");
        }
        // snow persistence: pile up, pan a whole screen away and back: replay_snowpan_before/after.png
        if (argc > 3 && !wcscmp(argv[3], L"snowpan")) {
            GfxReplayWeather(5);
            GfxReplayTune("Weather.SnowBuildUp", 5.0f);
            for (size_t q = 0; q < (size_t)W * H; q++) bufSolid[q * 4] = bufSolid[q * 4 + 3];
            double tm = 1200.0;
            float away[4] = { world[0] + (world[1] - world[0]), world[1] + (world[1] - world[0]), world[2], world[3] };
            auto run = [&](const float* wr, int frames) {
                for (int f = 0; f < frames; f++) {
                    GfxReplaySetTime(tm);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(wr, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
            };
            auto shot = [&](const wchar_t* name) {
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                SavePng(wic, dir + name, W, H, out);
                printf("wrote %ls\n", name);
            };
            run(world, 900);
            GfxReplayWeather(1);   // stop snowing so nothing new lands while we compare
            run(world, 1);
            shot(L"replay_snowpan_before.png");
            run(away, 120);
            run(world, 1);
            shot(L"replay_snowpan_after.png");
        }
        // round 5 showcase: time of day, new weather, water, photo mode, tracers + lit smoke: replay_r5_*.png
        if (argc > 3 && !wcscmp(argv[3], L"r5")) {
            for (size_t q = 0; q < (size_t)W * H; q++) bufSolid[q * 4] = bufSolid[q * 4 + 3];
            double tm = 1500.0;
            float vw = world[1] - world[0], vh = world[3] - world[2];
            auto run = [&](int frames, std::function<void(int)> each) {
                for (int f = 0; f < frames; f++) {
                    GfxReplaySetTime(tm);
                    if (each) each(f);
                    for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
            };
            auto shot = [&](const char* name) {
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                SavePng(wic, dir + L"replay_r5_" + std::wstring(name, name + strlen(name)) + L".png", W, H, out);
                printf("wrote replay_r5_%s.png\n", name);
            };
            GfxReplayWeather(1);
            GfxReplayTune("DayNight.Enabled", 1);
            const float hours[4] = { 6.3f, 12.0f, 18.2f, 23.0f };
            const char* hn[4] = { "dawn", "noon", "dusk", "night" };
            for (int i = 0; i < 4; i++) { GfxReplayTimeOfDay(hours[i]); run(40, nullptr); shot(hn[i]); }
            GfxReplayTimeOfDay(13.0f);
            GfxReplayTune("Weather.SnowBuildUp", 5.0f);
            const int wm[3] = { 6, 7, 8 };
            const char* wn[3] = { "sandstorm", "fogbanks", "ash" };
            for (int i = 0; i < 3; i++) { printf("weather %s...\n", wn[i]); GfxReplayWeather(wm[i]); run(i == 1 ? 300 : 600, [&](int f) { if (f % 100 == 0 || f > 500) printf("  frame %d\n", f); }); shot(wn[i]); }
            GfxReplayWeather(1);
            GfxLabClearSnow();
            GfxReplayWater(true, world[2] + vh * 0.22f);   // a sea across the lower part of the view
            run(60, nullptr); shot("water");
            GfxReplayWater(false, 0);
            GfxLab& lab = GfxLabState();
            lab.dof = 0.8f; lab.focusX = 0.35f; lab.focusY = 0.6f; lab.frameAspect = 2.39f;
            run(4, nullptr); shot("photo");
            lab.dof = 0; lab.frameAspect = 0;
            run(150, [&](int f) {   // a burst of tracers across the view and an explosion making smoke
                float u = (f % 30) / 30.0f;
                for (int k = 0; k < 5; k++) GfxReplayInject(8, world[0] + vw * (0.1f + 0.8f * u), world[2] + vh * (0.3f + 0.08f * k), fabsf(vh) * 0.004f);
                if (f == 20) GfxReplayInject(1, world[0] + vw * 0.6f, world[2] + vh * 0.45f, fabsf(vh) * 0.06f);
                if (f > 20 && f % 3 == 0) GfxReplayInject(7, world[0] + vw * 0.6f, world[2] + vh * 0.5f, fabsf(vh) * 0.04f);
            });
            shot("tracers_smoke");
        }
        // a burning strut: 40 small flame sprites for 2.5 s (fire embers, heat haze): replay_fire.png
        if (argc > 3 && !wcscmp(argv[3], L"fire")) {
            GfxReplayWeather(1);
            float fx0 = world[0] + 0.30f * (world[1] - world[0]), fy0 = world[2] + 0.45f * (world[3] - world[2]);
            float span = 0.12f * (world[1] - world[0]), fr = 0.006f * fabsf(world[3] - world[2]);
            double tm = 500.0;
            for (int f = 0; f < 150; f++) {
                GfxReplaySetTime(tm);
                for (int k = 0; k < 40; k++) GfxReplayInject(4, fx0 + span * k / 40.0f, fy0 + span * 0.25f * sinf(k * 0.7f), fr);
                for (int k = 0; k < 12; k++) {   // mods' fire effects flicker with dozens of flash sprites
                    float u = (float)((f * 7 + k * 13) % 97) / 97.0f, v = (float)((f * 11 + k * 5) % 89) / 89.0f;
                    GfxReplayInject(2, fx0 + span * u, fy0 + span * 0.3f * (v - 0.5f), fr * (2.0f + 4.0f * v));
                }
                if (f >= 100 && f < 103) GfxReplayInject(1, fx0 + span * 0.5f, fy0, fr * 6.0f);   // ExplosionSmall-sized blast
                if (f == 106) { finish(); bindFb(GL_FRAMEBUFFER, outFbo); R.ReadBuffer(0x8CE0); R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data()); SavePng(wic, dir + L"replay_fire_blast.png", W, H, out); }
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplayRun(world, outFbo, true, 1);
                tm += 1.0 / 60.0;
            }
            finish();
            bindFb(GL_FRAMEBUFFER, outFbo);
            R.ReadBuffer(0x8CE0);
            R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            SavePng(wic, dir + L"replay_fire.png", W, H, out);
            printf("wrote replay_fire.png\n");
        }

        // every look on the same frame, with its own backdrop colours and weather: replay_look_NN.png
        if (argc > 3 && !wcscmp(argv[3], L"looks")) {
            int n = GfxLookCount();
            for (int li = 0; li < n; li++) {
                GfxApplyLook(li, false);
                for (int i = 0; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                GfxReplaySetTime(200.0 + li * 20.0);
                GfxReplayBackdrop(layerTex.data(), (int)layerTex.size());
                bindTex(GL_TEXTURE_2D, tex[0]);
                getTex(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bd.data());
                for (size_t p = 0; p < (size_t)W * H; p++) {
                    float pf = bufEmis[p * 4 + 3] / 255.0f;
                    for (int c = 0; c < 3; c++) mixed[p * 4 + c] = (BYTE)(bd[p * 4 + c] * (1 - pf) + bufScene[p * 4 + c] * pf);
                }
                double tm = 200.0 + li * 20.0;
                for (int fr = 0; fr < 70; fr++) {   // let rain fall and lighting settle
                    GfxReplaySetTime(tm);
                    bindTex(GL_TEXTURE_2D, tex[0]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, mixed.data());
                    for (int i = 1; i < 4; i++) { bindTex(GL_TEXTURE_2D, tex[i]); subImage(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, bufs[i]->data()); }
                    GfxReplayRun(world, outFbo, true, 1);
                    tm += 1.0 / 60.0;
                }
                finish();
                bindFb(GL_FRAMEBUFFER, outFbo);
                R.ReadBuffer(0x8CE0);
                R.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                wchar_t nm[64]; swprintf(nm, 64, L"replay_look_%02d.png", li);
                SavePng(wic, dir + nm, W, H, out);
                printf("look %2d: %s\n", li, GfxLookName(li));
            }
        }
    }
    printf("wrote replay_off.png and replay_on.png\n");
    return 0;
}
