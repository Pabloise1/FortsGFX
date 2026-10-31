// One-frame capture: logs every GL call between two SwapBuffers, snapshots the render
// target whenever the game is about to clear / switch framebuffer / reload the projection,
// and dumps every texture the frame drew with. Output goes to FortsGFX\capture_NNN\.
#include "common.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <thread>
#include <unordered_map>
#include <objbase.h>
#include <wincodec.h>

bool g_capActive;
DWORD g_capThread;
static unsigned g_frame;
static std::atomic<bool> g_writerBusy{ false };

static const unsigned kMaxSnapshots = 48;
static const int kSnapScale = 2;                        // intermediate snapshots are stored at 1/2 resolution
static const size_t kTexDumpBudget = (size_t)512 << 20; // stop dumping textures past 512 MB of pixels
static const uint64_t kMinFreeDisk = 3ull << 30;          // refuse to capture with less than 3 GB free

struct Image {
    std::string name;   // relative to the capture folder, without extension
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA
    bool flip = false, opaque = false;
};

struct GLDrawState {
    GLint tex = 0, src = 0, dst = 0, prog = 0, fbo = 0;
    bool texOn = false, blend = false, alphaTest = false;
    float color[4] = { 1, 1, 1, 1 };
};

struct Block {
    bool open = false, compiling = false, multiColor = false, haveUV = false;
    GLenum mode = 0;
    unsigned verts = 0, colorCalls = 0, other = 0;
    float M[16] = {};
    GLint vp[4] = {};
    float fbH = 0;
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    float u0 = FLT_MAX, v0 = FLT_MAX, u1 = -FLT_MAX, v1 = -FLT_MAX;
    float col[4] = { 1, 1, 1, 1 };
    GLDrawState st;
};

struct Capture {
    int index = 0;
    unsigned frame = 0;
    std::wstring dir;
    std::string log;
    unsigned seq = 0, draws = 0, drawsSinceSnap = 0, snaps = 0;
    int indent = 0, winW = 0, winH = 0;
    std::vector<Image> images;
    std::unordered_map<const char*, unsigned> counts;
    std::unordered_map<GLuint, unsigned> texUse;
    Block blk;
    std::string timeline;    // flight recorder: the 3 s before the key press
};
static Capture C;

// ---------------------------------------------------------------- log lines

void CapLine(const char* fmt, ...) {
    char buf[1536];
    int ind = std::min(std::max(C.indent, 0), 12) * 2;
    int n = snprintf(buf, sizeof(buf), "%6u  %*s", ++C.seq, ind, "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    C.log += buf;
    C.log += '\n';
}

void CapCount(const char* fn) { C.counts[fn]++; }
void CapIndent(int d) { C.indent += d; }

// ---------------------------------------------------------------- draw state

static void QueryState(GLDrawState& s) {
    R.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    s.texOn = R.IsEnabled(GL_TEXTURE_2D) != 0;
    s.blend = R.IsEnabled(GL_BLEND) != 0;
    R.GetIntegerv(GL_BLEND_SRC_RGB, &s.src);
    R.GetIntegerv(GL_BLEND_DST_RGB, &s.dst);
    R.GetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    R.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.fbo);
    s.alphaTest = R.IsEnabled(GL_ALPHA_TEST) != 0;
    R.GetFloatv(GL_CURRENT_COLOR, s.color);
}

static void NoteTexture(const GLDrawState& s) {
    if (s.tex && (s.texOn || s.prog)) C.texUse[s.tex]++;
}

static std::string StateStr(const GLDrawState& s) {
    std::string t = s.texOn || s.prog ? Fmt("tex=%u%s", s.tex, TexShort(s.tex).c_str()) : std::string("tex=off");
    std::string b = s.blend ? Fmt("%s/%s", BlendName(s.src), BlendName(s.dst)) : std::string("off");
    return Fmt("%s blend=%s%s prog=%d fbo=%d", t.c_str(), b.c_str(), s.alphaTest ? " alphatest" : "", s.prog, s.fbo);
}

static void Mul(float* out, const float* a, const float* b) {   // column-major out = a * b
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = v;
        }
}

// ---------------------------------------------------------------- glBegin/glEnd blocks

void CapBeginBlock(GLenum mode, bool compiling) {
    Block& b = C.blk;
    b = Block{};
    b.open = true;
    b.mode = mode;
    b.compiling = compiling;
    if (compiling) return;
    QueryState(b.st);
    memcpy(b.col, b.st.color, sizeof(b.col));
    float mv[16], p[16];
    R.GetFloatv(GL_MODELVIEW_MATRIX, mv);
    R.GetFloatv(GL_PROJECTION_MATRIX, p);
    Mul(b.M, p, mv);
    R.GetIntegerv(GL_VIEWPORT, b.vp);
    b.fbH = b.st.fbo == 0 ? (float)C.winH : (float)(b.vp[1] + b.vp[3]);
}

void CapVertex(const char* fn, float x, float y, float z) {
    C.counts[fn]++;
    Block& b = C.blk;
    if (!b.open) { CapLine("%s(%g, %g, %g) outside glBegin", fn, x, y, z); return; }
    b.verts++;
    float sx = x, sy = y;
    if (!b.compiling) {   // project to window pixels, top-left origin
        const float* M = b.M;
        float cx = M[0] * x + M[4] * y + M[8] * z + M[12];
        float cy = M[1] * x + M[5] * y + M[9] * z + M[13];
        float cw = M[3] * x + M[7] * y + M[11] * z + M[15];
        if (cw == 0) return;
        sx = b.vp[0] + (cx / cw + 1) * 0.5f * b.vp[2];
        sy = b.fbH - (b.vp[1] + (cy / cw + 1) * 0.5f * b.vp[3]);
    }
    b.x0 = std::min(b.x0, sx); b.x1 = std::max(b.x1, sx);
    b.y0 = std::min(b.y0, sy); b.y1 = std::max(b.y1, sy);
}

void CapTexCoord(const char* fn, float s, float t) {
    C.counts[fn]++;
    Block& b = C.blk;
    if (!b.open) { CapLine("%s(%g, %g)", fn, s, t); return; }
    b.haveUV = true;
    b.u0 = std::min(b.u0, s); b.u1 = std::max(b.u1, s);
    b.v0 = std::min(b.v0, t); b.v1 = std::max(b.v1, t);
}

void CapColor(const char* fn, const float c[4]) {
    C.counts[fn]++;
    Block& b = C.blk;
    if (!b.open) { CapLine("%s(%.3f, %.3f, %.3f, %.3f)", fn, c[0], c[1], c[2], c[3]); return; }
    if (++b.colorCalls == 1 && b.verts == 0) memcpy(b.col, c, sizeof(b.col));
    else if (memcmp(b.col, c, sizeof(b.col)) != 0) b.multiColor = true;
}

void CapEndBlock() {
    Block& b = C.blk;
    if (!b.open) { CapLine("glEnd() without glBegin"); return; }
    b.open = false;
    std::string uv = b.haveUV ? Fmt(" uv=(%.3f,%.3f)-(%.3f,%.3f)", b.u0, b.v0, b.u1, b.v1) : std::string(" uv=none");
    std::string col = Fmt(" col=(%.2f,%.2f,%.2f,%.2f)%s", b.col[0], b.col[1], b.col[2], b.col[3], b.multiColor ? "+gradient" : "");
    std::string other = b.other ? Fmt(" +%u other calls", b.other) : std::string();
    if (b.verts == 0) { CapLine("DRAW %s (no vertices)", PrimName(b.mode)); return; }
    if (b.compiling) {
        CapLine("LISTDRAW %s v=%u obj=(%.1f,%.1f)-(%.1f,%.1f)%s%s%s", PrimName(b.mode), b.verts, b.x0, b.y0, b.x1, b.y1,
                uv.c_str(), col.c_str(), other.c_str());
        return;
    }
    C.draws++;
    C.drawsSinceSnap++;
    NoteTexture(b.st);
    CapLine("DRAW %s v=%u scr=(%.0f,%.0f)-(%.0f,%.0f)%s%s %s%s", PrimName(b.mode), b.verts, b.x0, b.y0, b.x1, b.y1,
            uv.c_str(), col.c_str(), StateStr(b.st).c_str(), other.c_str());
}

void CapCallList(GLuint id, const std::string& summary, const std::vector<GLuint>& tex, bool compiling) {
    C.counts["glCallList"]++;
    if (compiling) { CapLine("glCallList(%u) {%s}", id, summary.c_str()); return; }
    GLDrawState s;
    QueryState(s);
    C.draws++;
    C.drawsSinceSnap++;
    NoteTexture(s);
    for (GLuint t : tex) if (t) C.texUse[t]++;
    CapLine("CALLLIST %u {%s} %s", id, summary.c_str(), StateStr(s).c_str());
}

// ---------------------------------------------------------------- generic (unwrapped) calls

static const struct { const char* n; int a; } kArity[] = {
    { "glFlush", 0 }, { "glFinish", 0 }, { "glHint", 2 }, { "glShadeModel", 1 }, { "glPixelStorei", 2 },
    { "glGenLists", 1 }, { "glDeleteLists", 2 }, { "glGetFloatv", 2 }, { "glGetIntegerv", 2 }, { "glGetString", 1 },
    { "glGetError", 0 }, { "glIsEnabled", 1 }, { "glPushClientAttrib", 1 }, { "glPopClientAttrib", 0 },
    { "glGenTextures", 2 }, { "glReadBuffer", 1 }, { "glDrawBuffer", 1 }, { "glDepthMask", 1 }, { "glCullFace", 1 },
    { "glStencilFunc", 3 }, { "glStencilOp", 3 }, { "glStencilMask", 1 }, { "glPointSize", 1 }, { "glLineStipple", 2 },
    { "glEnableClientState", 1 }, { "glDisableClientState", 1 }, { "glColor4ub", 4 }, { "glColor3f", 3 },
    { "glNormal3f", 3 }, { "glTexEnvi", 3 }, { "glTexGeni", 3 }, { "glFogf", 2 }, { "glFogi", 2 },
    { "wglGetCurrentDC", 0 }, { "wglGetCurrentContext", 0 },
    { "glBlendFuncSeparate", 4 }, { "glBlendFuncSeparateEXT", 4 }, { "glBlendEquation", 1 }, { "glBlendEquationEXT", 1 },
    { "glBlendEquationSeparate", 2 }, { "glBlendColor", 4 }, { "glBindBuffer", 2 }, { "glBindBufferARB", 2 },
    { "glGenerateMipmap", 1 }, { "glGenerateMipmapEXT", 1 }, { "glClientActiveTexture", 1 }, { "glClientActiveTextureARB", 1 },
    { "glMultiTexCoord2f", 3 }, { "glMultiTexCoord2fARB", 3 }, { "glBindRenderbuffer", 2 }, { "glBindRenderbufferEXT", 2 },
    { "glFramebufferTexture2D", 5 }, { "glFramebufferTexture2DEXT", 5 }, { "glDrawBuffers", 2 },
    { "glGenFramebuffers", 2 }, { "glGenFramebuffersEXT", 2 }, { "glCheckFramebufferStatus", 1 },
    { "glCheckFramebufferStatusEXT", 1 }, { "glBlitFramebuffer", 10 }, { "glDrawArrays", 3 }, { "glDrawElements", 4 },
    { "glDrawArraysInstanced", 4 }, { "glDrawElementsInstanced", 5 }, { "glVertexAttribPointer", 6 },
    { "glEnableVertexAttribArray", 1 }, { "glDisableVertexAttribArray", 1 }, { "glBindVertexArray", 1 },
    { "glUniform1i", 2 }, { "glUniform1f", 2 }, { "glUniform2f", 3 }, { "glUniform3f", 4 }, { "glUniform4f", 5 },
    { "glUniform1iARB", 2 }, { "glUniform1fARB", 2 }, { "glUniform4fARB", 5 }, { "glUniformMatrix4fv", 4 },
    { "glGetUniformLocation", 2 }, { "glGetUniformLocationARB", 2 }, { "glCreateShader", 1 }, { "glCompileShader", 1 },
    { "glCreateProgram", 0 }, { "glAttachShader", 2 }, { "glLinkProgram", 1 }, { "glDeleteShader", 1 },
    { "glSecondaryColor3f", 3 }, { "glActiveStencilFaceEXT", 1 },
};

void CapGenericCall(const char* name, const uint64_t* ints, const uint8_t* xmm) {
    C.counts[name]++;
    if (C.blk.open) { C.blk.other++; return; }
    int arity = -1;
    for (auto& a : kArity) if (!strcmp(a.n, name)) { arity = a.a; break; }
    int shown = arity < 0 ? 4 : std::min(arity, 4);
    std::string args;
    for (int i = 0; i < shown; i++) args += Fmt(i ? ", 0x%llx" : "0x%llx", (unsigned long long)ints[i]);
    if (arity > 4) args += ", ...";
    std::string floats;
    for (int i = 0; i < shown; i++) {
        float v;
        memcpy(&v, xmm + 16 * i, 4);
        if (v == v && fabsf(v) > 1e-6f && fabsf(v) < 1e7f) floats += Fmt(" a%d=%g", i, v);
    }
    if (!floats.empty()) args += " | as float:" + floats;
    if (arity < 0) args += "  (arity unknown, raw registers)";

    if (!strncmp(name, "glDraw", 6) || !strncmp(name, "glMultiDraw", 11)) {
        GLDrawState s;
        QueryState(s);
        C.draws++;
        C.drawsSinceSnap++;
        NoteTexture(s);
        CapLine("DRAW* %s(%s) %s", name, args.c_str(), StateStr(s).c_str());
    } else {
        CapLine("%s(%s)", name, args.c_str());
    }
}

// ---------------------------------------------------------------- readback

struct PackState {
    GLint align = 4, rowLen = 0, skipRows = 0, skipPix = 0, buf = 0;
    void Save() {
        R.GetIntegerv(GL_PACK_ALIGNMENT, &align);
        R.GetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
        R.GetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        R.GetIntegerv(GL_PACK_SKIP_PIXELS, &skipPix);
        if (R.BindBuffer) R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buf);
        R.PixelStorei(GL_PACK_ALIGNMENT, 4);
        R.PixelStorei(GL_PACK_ROW_LENGTH, 0);
        R.PixelStorei(GL_PACK_SKIP_ROWS, 0);
        R.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
        if (buf) R.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    void Restore() {
        R.PixelStorei(GL_PACK_ALIGNMENT, align);
        R.PixelStorei(GL_PACK_ROW_LENGTH, rowLen);
        R.PixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        R.PixelStorei(GL_PACK_SKIP_PIXELS, skipPix);
        if (buf) R.BindBuffer(GL_PIXEL_PACK_BUFFER, buf);
    }
};

static void ClientSize(int& w, int& h) {
    RECT rc{};
    HWND wnd = WindowFromDC(R.wglGetCurrentDC());
    if (wnd && GetClientRect(wnd, &rc)) { w = rc.right - rc.left; h = rc.bottom - rc.top; }
}

static bool TargetSize(GLint fbo, GLint drawBuf, int& w, int& h) {
    w = h = 0;
    if (fbo == 0) { ClientSize(w, h); return w > 0 && h > 0; }
    if (!R.GetFramebufferAttachmentParameteriv || !drawBuf) return false;
    GLint type = 0, name = 0;
    R.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, drawBuf, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    R.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, drawBuf, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    if (type == GL_TEXTURE && R.GetTextureLevelParameteriv) {
        R.GetTextureLevelParameteriv(name, 0, GL_TEXTURE_WIDTH, &w);
        R.GetTextureLevelParameteriv(name, 0, GL_TEXTURE_HEIGHT, &h);
    } else if (type == GL_RENDERBUFFER && R.GetNamedRenderbufferParameteriv) {
        R.GetNamedRenderbufferParameteriv(name, GL_RENDERBUFFER_WIDTH, &w);
        R.GetNamedRenderbufferParameteriv(name, GL_RENDERBUFFER_HEIGHT, &h);
    }
    return w > 0 && h > 0;
}

// Reads the current draw target (or the back buffer) without disturbing the game's state.
static bool ReadTarget(Image& img, int scale, bool backBuffer) {
    GLint drawFbo = 0, readFbo = 0, drawBuf = GL_BACK, readBuf = 0;
    if (R.BindFramebuffer) {
        R.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        R.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    }
    GLint src = backBuffer ? 0 : drawFbo;
    if (!backBuffer) R.GetIntegerv(GL_DRAW_BUFFER, &drawBuf);
    int w, h;
    if (!TargetSize(src, drawBuf, w, h)) return false;
    if (src != 0) {
        GLint samples = 0;
        R.GetIntegerv(GL_SAMPLE_BUFFERS, &samples);
        if (samples > 0) return false;   // multisampled FBOs can't be read directly
    }
    if (src != readFbo) R.BindFramebuffer(GL_READ_FRAMEBUFFER, src);
    R.GetIntegerv(GL_READ_BUFFER, &readBuf);
    R.ReadBuffer(drawBuf);
    PackState ps;
    ps.Save();
    std::vector<uint8_t> px((size_t)w * h * 4);
    R.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    ps.Restore();
    R.ReadBuffer(readBuf);
    if (src != readFbo) R.BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);

    img.flip = true;
    img.opaque = true;
    if (scale <= 1) { img.w = w; img.h = h; img.px.swap(px); return true; }
    img.w = w / scale;
    img.h = h / scale;
    img.px.assign((size_t)img.w * img.h * 4, 0);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
            for (int c = 0; c < 4; c++) {
                unsigned sum = 0;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        sum += px[((size_t)(y * scale + dy) * w + (x * scale + dx)) * 4 + c];
                img.px[((size_t)y * img.w + x) * 4 + c] = (uint8_t)(sum / (scale * scale));
            }
    return true;
}

static std::string Sanitize(const char* s) {
    std::string out;
    for (; *s; s++) out += isalnum((unsigned char)*s) ? *s : '_';
    return out;
}

void CapMaybeSnapshot(const char* why) {
    if (!CapOn() || C.drawsSinceSnap == 0) return;
    unsigned draws = C.drawsSinceSnap;
    C.drawsSinceSnap = 0;
    if (C.snaps >= kMaxSnapshots) { CapLine("SNAPSHOT skipped (%s): limit of %u reached", why, kMaxSnapshots); return; }
    Image img;
    if (!ReadTarget(img, kSnapScale, false)) { CapLine("SNAPSHOT failed (%s)", why); return; }
    img.name = Fmt("snap_%02u_%s", ++C.snaps, Sanitize(why).c_str());
    CapLine("SNAPSHOT %s.png (%dx%d, %u draws since previous)", img.name.c_str(), img.w, img.h, draws);
    C.images.push_back(std::move(img));
}

// ---------------------------------------------------------------- writing

static bool WritePng(IWICImagingFactory* f, const std::wstring& path, const Image& img) {
    std::vector<uint8_t> bgra(img.px.size());
    for (int y = 0; y < img.h; y++) {
        const uint8_t* s = &img.px[(size_t)(img.flip ? img.h - 1 - y : y) * img.w * 4];
        uint8_t* d = &bgra[(size_t)y * img.w * 4];
        for (int x = 0; x < img.w; x++, s += 4, d += 4) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = img.opaque ? 255 : s[3];
        }
    }
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &props)) &&
              SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize(img.w, img.h)) &&
              SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(fr->WritePixels(img.h, img.w * 4, (UINT)bgra.size(), bgra.data())) &&
              SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    if (props) props->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}

static void WriteText(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

struct Job {
    int index;
    std::wstring dir;
    std::string frameTxt, texTxt, timelineTxt;
    std::vector<Image> images;
};

static void RunJob(Job* j) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    WriteText(j->dir + L"frame.txt", j->frameTxt);
    WriteText(j->dir + L"textures.txt", j->texTxt);
    if (!j->timelineTxt.empty()) WriteText(j->dir + L"history\\timeline.txt", j->timelineTxt);
    size_t ok = 0;
    for (auto& img : j->images)
        if (f && WritePng(f, j->dir + Widen(img.name) + L".png", img)) ok++;
    if (f) f->Release();
    CoUninitialize();
    SessionLog("capture %03d written: %zu/%zu images in %s", j->index, ok, j->images.size(), Narrow(j->dir.c_str()).c_str());
    delete j;
    g_writerBusy = false;
    MessageBeep(MB_OK);
}

static std::string DumpTextures(std::vector<Image>& out) {
    std::vector<std::pair<GLuint, unsigned>> used(C.texUse.begin(), C.texUse.end());
    std::sort(used.begin(), used.end());
    std::string txt = Fmt("# %zu textures drawn with in capture %03d (frame %u)\n# id  draws  size  gl-format  upload info\n",
                          used.size(), C.index, C.frame);
    PackState ps;
    ps.Save();
    size_t budget = kTexDumpBudget;
    for (auto& u : used) {
        GLint w = 0, h = 0, ifmt = 0;
        if (R.GetTextureLevelParameteriv) {
            R.GetTextureLevelParameteriv(u.first, 0, GL_TEXTURE_WIDTH, &w);
            R.GetTextureLevelParameteriv(u.first, 0, GL_TEXTURE_HEIGHT, &h);
            R.GetTextureLevelParameteriv(u.first, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        }
        std::string note;
        size_t bytes = (size_t)w * h * 4;
        if (w > 0 && h > 0 && R.GetTextureImage && bytes <= budget) {
            Image img;
            img.name = Fmt("tex\\tex_%05u_%dx%d", u.first, w, h);
            img.w = w; img.h = h;
            img.px.assign(bytes, 0);
            R.GetTextureImage(u.first, 0, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)bytes, img.px.data());
            budget -= bytes;
            note = img.name + ".png";
            out.push_back(std::move(img));
        } else {
            note = w > 0 ? "not dumped (budget)" : "not dumped (not a 2D texture?)";
        }
        txt += Fmt("%5u  %5u  %dx%d  %s  %s  -> %s\n", u.first, u.second, w, h, E((GLenum)ifmt),
                   TexDescribe(u.first).c_str(), note.c_str());
    }
    ps.Restore();
    return txt;
}

// ---------------------------------------------------------------- flight recorder
// Always running, so a capture also has the moment before the key press: the last ~2.5 s of frames
// (about 960 px wide, every 50 ms, read back asynchronously through pixel buffers so the GPU never
// waits) and a timeline of the effect sprites drawn and the effects FortsGFX spawned in each frame.
static const int kHistFrames = 50;
static const double kHistInterval = 0.05;
struct HistFrame { double t = 0; unsigned frame = 0; int w = 0, h = 0; std::vector<uint8_t> px; };
static HistFrame g_hist[kHistFrames];
static int g_histHead, g_histCount;
static std::deque<std::pair<double, std::string>> g_timeline;   // per frame: time, effect lines
static std::string g_evFrame;                                    // this frame's lines (CapHistNote)
struct Pbo { GLuint buf = 0; bool full = false; unsigned frame = 0; double t = 0; int w = 0, h = 0; };
static Pbo g_pbo[3];
static int g_pboNext, g_histW, g_histH;
static GLuint g_histFbo, g_histTex;
static double g_histLastT = -1;

static double NowSeconds() {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER n;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - t0.QuadPart) / f.QuadPart;
}

void CapHistNote(const std::string& line) {
    if (g_evFrame.size() < 16384) { g_evFrame += "    "; g_evFrame += line; g_evFrame += '\n'; }
}

static void HistRecord() {
    typedef void (WINAPI* GenFn)(GLsizei, GLuint*);
    typedef void (WINAPI* BlitFn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    typedef void (WINAPI* FbTexFn)(GLenum, GLenum, GLenum, GLuint, GLint);
    typedef void (WINAPI* TexImgFn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    typedef void (WINAPI* BufDataFn)(GLenum, ptrdiff_t, const void*, GLenum);
    typedef void* (WINAPI* MapFn)(GLenum, GLenum);
    typedef GLboolean (WINAPI* UnmapFn)(GLenum);
    typedef void (WINAPI* BindTexFn)(GLenum, GLuint);
    typedef void (WINAPI* EnDisFn)(GLenum);
    static GenFn genFb, genBuf, genTex;
    static BlitFn blit;
    static FbTexFn fbTex;
    static TexImgFn texImage;
    static BufDataFn bufData;
    static MapFn map;
    static UnmapFn unmap;
    static BindTexFn bindTex;
    static EnDisFn enable, disable;
    static bool loaded, ok;
    double now = NowSeconds();
    // timeline: one entry per frame with anything in it, kept for 3 s
    if (!g_evFrame.empty()) { g_timeline.emplace_back(now, Fmt("frame %u\n", g_frame) + g_evFrame); g_evFrame.clear(); }
    while (!g_timeline.empty() && now - g_timeline.front().first > 3.0) g_timeline.pop_front();
    if (!loaded) {
        loaded = true;
        genFb = (GenFn)RealProc("glGenFramebuffers"); genBuf = (GenFn)RealProc("glGenBuffers"); genTex = (GenFn)RealProc("glGenTextures");
        blit = (BlitFn)RealProc("glBlitFramebuffer"); fbTex = (FbTexFn)RealProc("glFramebufferTexture2D");
        texImage = (TexImgFn)RealProc("glTexImage2D"); bufData = (BufDataFn)RealProc("glBufferData");
        map = (MapFn)RealProc("glMapBuffer"); unmap = (UnmapFn)RealProc("glUnmapBuffer"); bindTex = (BindTexFn)RealProc("glBindTexture");
        enable = (EnDisFn)RealProc("glEnable"); disable = (EnDisFn)RealProc("glDisable");
        ok = genFb && genBuf && genTex && blit && fbTex && texImage && bufData && map && unmap && bindTex && enable && disable &&
             R.BindFramebuffer && R.BindBuffer;
        if (!ok) SessionLog("capture: flight recorder unavailable (GL functions missing)");
    }
    if (!ok) return;
    const GLenum PACK = 0x88EB, READFB = 0x8CA8, DRAWFB = 0x8CA9, COL0 = 0x8CE0, SCISSOR = 0x0C11;
    GLint drawFbo = 0, readFbo = 0, readBuf = 0, packBuf = 0, tex2d = 0;
    R.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    R.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    R.GetIntegerv(GL_READ_BUFFER, &readBuf);
    R.GetIntegerv(0x88ED /* GL_PIXEL_PACK_BUFFER_BINDING */, &packBuf);
    // collect finished readbacks (two frames old) into the ring
    for (auto& p : g_pbo) {
        if (!p.full || g_frame - p.frame < 2) continue;
        R.BindBuffer(PACK, p.buf);
        if (const void* src = map(PACK, 0x88B8 /* GL_READ_ONLY */)) {
            HistFrame& h = g_hist[g_histHead];
            h.t = p.t; h.frame = p.frame; h.w = p.w; h.h = p.h;
            h.px.assign((const uint8_t*)src, (const uint8_t*)src + (size_t)p.w * p.h * 4);
            unmap(PACK);
            g_histHead = (g_histHead + 1) % kHistFrames;
            g_histCount = std::min(g_histCount + 1, kHistFrames);
        }
        p.full = false;
    }
    if (now - g_histLastT >= kHistInterval && !g_pbo[g_pboNext].full) {
        int w = 0, h = 0;
        ClientSize(w, h);
        if (w > 0 && h > 0) {
            int sw = std::min(w, 960), sh = std::max(1, (int)((long long)h * sw / w));
            if (sw != g_histW || sh != g_histH) {
                if (!g_histFbo) { genFb(1, &g_histFbo); genTex(1, &g_histTex); for (auto& p : g_pbo) genBuf(1, &p.buf); }
                R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
                bindTex(GL_TEXTURE_2D, g_histTex);
                texImage(GL_TEXTURE_2D, 0, 0x8058 /* GL_RGBA8 */, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                bindTex(GL_TEXTURE_2D, tex2d);
                R.BindFramebuffer(DRAWFB, g_histFbo);
                fbTex(DRAWFB, COL0, GL_TEXTURE_2D, g_histTex, 0);
                for (auto& p : g_pbo) { R.BindBuffer(PACK, p.buf); bufData(PACK, (ptrdiff_t)sw * sh * 4, nullptr, 0x88E1 /* GL_STREAM_READ */); p.full = false; }
                g_histW = sw; g_histH = sh;
            }
            GLboolean sc = R.IsEnabled(SCISSOR);
            if (sc) disable(SCISSOR);
            R.BindFramebuffer(READFB, 0);
            R.ReadBuffer(GL_BACK);
            R.BindFramebuffer(DRAWFB, g_histFbo);
            blit(0, 0, w, h, 0, 0, sw, sh, 0x4000 /* GL_COLOR_BUFFER_BIT */, 0x2601 /* GL_LINEAR */);
            R.BindFramebuffer(READFB, g_histFbo);
            R.ReadBuffer(COL0);
            Pbo& p = g_pbo[g_pboNext];
            R.BindBuffer(PACK, p.buf);
            PackState ps;
            ps.Save();
            R.BindBuffer(PACK, p.buf);   // Save() may unbind it
            R.PixelStorei(GL_PACK_ALIGNMENT, 4);
            R.ReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            ps.Restore();
            p.full = true; p.frame = g_frame; p.t = now; p.w = sw; p.h = sh;
            g_pboNext = (g_pboNext + 1) % 3;
            g_histLastT = now;
            if (sc) enable(SCISSOR);
        }
    }
    R.BindBuffer(PACK, packBuf);
    R.BindFramebuffer(READFB, readFbo);
    R.BindFramebuffer(DRAWFB, drawFbo);
    R.ReadBuffer(readBuf);
}

// Copies the recorder into a capture: history\frame_NN_-1.23s.png and history\timeline.txt.
static void HistDump(std::vector<Image>& images, std::string& timeline) {
    double now = NowSeconds();
    for (int i = 0; i < g_histCount; i++) {
        HistFrame& h = g_hist[(g_histHead - g_histCount + i + kHistFrames) % kHistFrames];
        if (h.px.empty()) continue;
        Image img;
        img.w = h.w; img.h = h.h; img.px = h.px; img.flip = true; img.opaque = true;
        img.name = Fmt("history\\frame_%02d_%+.2fs", i, h.t - now);
        images.push_back(std::move(img));
    }
    timeline = "# FortsGFX flight recorder: the last 3 s before the capture. For every frame, the effect sprites\n"
               "# the game drew (class, texture, screen box, opacity, primitive) and the effects FortsGFX spawned.\n\n";
    for (auto& e : g_timeline) timeline += Fmt("[%+.2fs] ", e.first - now) + e.second;
}

static void StartCapture() {
    LoadRealExt();
    int idx = 1;
    while (idx < 1000 && GetFileAttributesW((g_outDir + Widen(Fmt("capture_%03d", idx))).c_str()) != INVALID_FILE_ATTRIBUTES) idx++;
    C = Capture{};
    C.index = idx;
    C.frame = g_frame + 1;
    C.dir = g_outDir + Widen(Fmt("capture_%03d\\", idx));
    CreateDirectoryW(C.dir.c_str(), nullptr);
    CreateDirectoryW((C.dir + L"tex").c_str(), nullptr);
    CreateDirectoryW((C.dir + L"history").c_str(), nullptr);
    HistDump(C.images, C.timeline);   // the frames and effects leading up to the key press
    ClientSize(C.winW, C.winH);

    GLint mm = 0, vp[4] = {}, fbo = 0, prog = 0, tex = 0;
    R.GetIntegerv(GL_MATRIX_MODE, &mm);
    R.GetIntegerv(GL_VIEWPORT, vp);
    R.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    R.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    const GLubyte* ver = R.GetString(GL_VERSION);
    C.log += Fmt("# FortsGFX capture %03d - frame %u - window %dx%d - GL %s\n", idx, C.frame, C.winW, C.winH, ver ? (const char*)ver : "?");
    C.log += Fmt("# state at frame start: matrix=%s viewport=(%d,%d,%d,%d) fbo=%d program=%d tex2D=%d\n", E(mm), vp[0], vp[1],
                 vp[2], vp[3], fbo, prog, tex);
    C.log += "# DRAW lines summarise one glBegin..glEnd: v=vertex count, scr=window-pixel bbox (top-left origin),\n"
             "#   uv=texcoord range, col=vertex colour, then the bound texture / blend / program / framebuffer.\n"
             "# CALLLIST = display list execution (contents recorded when the list was compiled).\n"
             "# SNAPSHOT = what the current render target looked like at that point (half resolution).\n\n";

    g_capThread = GetCurrentThreadId();
    g_capActive = true;
    UpdateSlowPath();
    WriteProcList();
    SessionLog("capture %03d started (frame %u)", idx, C.frame);
    MessageBeep(MB_ICONASTERISK);
}

// ---------------------------------------------------------------- photo mode
static bool g_photoReq;
void CapRequestPhoto() { g_photoReq = true; }

// A scripted test bench (maps\fortsgfx testbench) asks for a capture at each beat. It waits while an
// earlier capture is still being written instead of being dropped.
static std::string g_benchLabel;
static bool g_benchReq;
void CapRequest(const char* label) { g_benchLabel = label ? label : ""; g_benchReq = true; }

static void SavePhoto() {
    Image* img = new Image;
    if (!ReadTarget(*img, 1, true)) { delete img; SessionLog("photo: could not read the frame"); return; }
    std::wstring dir = g_outDir + L"Photos\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring path = dir + Widen(Fmt("FortsGFX_%04d-%02d-%02d_%02d-%02d-%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
    std::thread([img, path] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        bool ok = f && WritePng(f, path, *img);
        if (f) f->Release();
        CoUninitialize();
        SessionLog("photo %s: %s (%dx%d)", ok ? "saved" : "FAILED", Narrow(path.c_str()).c_str(), img->w, img->h);
        delete img;
        MessageBeep(ok ? MB_OK : MB_ICONHAND);
    }).detach();
}

// A frame read back by the Vulkan recon (vulkan.cpp): RGBA, top row first.
void CapSaveRgbaPng(const std::wstring& path, int w, int h, std::vector<uint8_t>&& rgba) {
    Image* img = new Image;
    img->w = w; img->h = h; img->px = std::move(rgba); img->opaque = true;
    std::thread([img, path] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        bool ok = f && WritePng(f, path, *img);
        if (f) f->Release();
        CoUninitialize();
        if (!ok) SessionLog("vulkan: could not write %s", Narrow(path.c_str()).c_str());
        delete img;
    }).detach();
}

void CapFrameEnd() {
    g_frame++;
    HistRecord();
    if (g_photoReq) { g_photoReq = false; SavePhoto(); }
    if (!CapOn()) return;
    if (C.blk.open) CapLine("!! frame ended inside glBegin");
    Image fin;
    if (ReadTarget(fin, 1, true)) { fin.name = "final"; C.images.push_back(std::move(fin)); }
    CapLine("SwapBuffers -> final.png");

    std::vector<std::pair<GLuint, std::string>> dbg;   // renderer intermediates (world before post, emissive, bloom)
    GfxDebugTargets(dbg);
    if (R.GetTextureImage && R.GetTextureLevelParameteriv) {
        PackState ps;
        ps.Save();
        for (auto& d : dbg) {
            Image img;
            R.GetTextureLevelParameteriv(d.first, 0, GL_TEXTURE_WIDTH, &img.w);
            R.GetTextureLevelParameteriv(d.first, 0, GL_TEXTURE_HEIGHT, &img.h);
            if (img.w <= 0 || img.h <= 0) continue;
            img.px.assign((size_t)img.w * img.h * 4, 0);
            R.GetTextureImage(d.first, 0, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)img.px.size(), img.px.data());
            img.name = d.second;
            img.flip = true;
            img.opaque = true;
            C.images.push_back(std::move(img));
        }
        ps.Restore();
    }

    auto job = new Job;
    job->texTxt = DumpTextures(C.images);

    std::vector<std::pair<const char*, unsigned>> counts(C.counts.begin(), C.counts.end());
    std::sort(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.second > b.second; });
    C.log += Fmt("\n# ---- totals: %u draws, %u snapshots, %zu textures ----\n", C.draws, C.snaps, C.texUse.size());
    for (auto& c : counts) C.log += Fmt("# %8u  %s\n", c.second, c.first);

    g_capActive = false;
    UpdateSlowPath();
    job->index = C.index;
    job->dir = C.dir;
    job->frameTxt = std::move(C.log);
    job->timelineTxt = std::move(C.timeline);
    job->images = std::move(C.images);
    C = Capture{};
    g_writerBusy = true;
    std::thread(RunJob, job).detach();
}

void CapFrameBegin() {
    bool want = false;
    static bool prevKey;
    bool key = (GetAsyncKeyState(VK_F11) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
               (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (key && !prevKey) want = true;
    prevKey = key;
    if (g_frame % 30 == 0) {
        std::wstring trigger = g_outDir + L"capture.trigger";
        if (GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(trigger.c_str());
            want = true;
        }
    }
    std::string label;
    if (g_benchReq && !g_writerBusy) { g_benchReq = false; want = true; label = g_benchLabel; }
    if (!want) return;
    if (g_writerBusy) { SessionLog("capture request ignored: previous capture still being written"); return; }
    ULARGE_INTEGER avail{};
    if (GetDiskFreeSpaceExW(g_outDir.c_str(), &avail, nullptr, nullptr) && avail.QuadPart < kMinFreeDisk) {
        SessionLog("capture refused: only %.2f GB free on the output drive (need %.0f GB) - set OutputDir in FortsGFX.ini",
                   avail.QuadPart / 1073741824.0, kMinFreeDisk / 1073741824.0);
        MessageBeep(MB_ICONHAND);
        return;
    }
    StartCapture();
    if (!label.empty()) {   // name the capture after the bench beat
        C.log = Fmt("# test bench beat: %s\n", label.c_str()) + C.log;
        HANDLE f = CreateFileW((C.dir + L"label.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD n = 0; WriteFile(f, label.data(), (DWORD)label.size(), &n, nullptr); CloseHandle(f); }
        SessionLog("capture %03d is test bench beat %s", C.index, label.c_str());
    }
}
