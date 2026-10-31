// Hand-written wrappers. Every `extern "C" ... WINAPI hk_<name>(` whose <name> is an
// opengl32 export replaces the generic stub for it (tools/gen_exports.ps1 scans this file).
// The rest are GL extensions (handed out by wglGetProcAddress) or Forts.exe imports (IAT).
#include "common.h"
#include <map>
#include <mutex>
#include <unordered_map>

#define DECL(ret, name, params) static ret (WINAPI* r_##name) params

// ---- opengl32 exports
DECL(HGLRC, wglCreateContext, (HDC));
DECL(BOOL, wglMakeCurrent, (HDC, HGLRC));
DECL(BOOL, wglDeleteContext, (HGLRC));
DECL(PROC, wglGetProcAddress, (LPCSTR));
DECL(BOOL, wglSwapBuffers, (HDC));
DECL(void, glBegin, (GLenum));
DECL(void, glEnd, ());
DECL(void, glVertex2f, (GLfloat, GLfloat));
DECL(void, glVertex2fv, (const GLfloat*));
DECL(void, glVertex3f, (GLfloat, GLfloat, GLfloat));
DECL(void, glVertex3fv, (const GLfloat*));
DECL(void, glTexCoord2f, (GLfloat, GLfloat));
DECL(void, glTexCoord2fv, (const GLfloat*));
DECL(void, glColor4f, (GLfloat, GLfloat, GLfloat, GLfloat));
DECL(void, glColor4fv, (const GLfloat*));
DECL(void, glBindTexture, (GLenum, GLuint));
DECL(void, glDeleteTextures, (GLsizei, const GLuint*));
DECL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
DECL(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*));
DECL(void, glTexParameteri, (GLenum, GLenum, GLint));
DECL(void, glTexParameterf, (GLenum, GLenum, GLfloat));
DECL(void, glTexEnvf, (GLenum, GLenum, GLfloat));
DECL(void, glEnable, (GLenum));
DECL(void, glDisable, (GLenum));
DECL(void, glBlendFunc, (GLenum, GLenum));
DECL(void, glAlphaFunc, (GLenum, GLclampf));
DECL(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));
DECL(void, glDepthFunc, (GLenum));
DECL(void, glClear, (GLbitfield));
DECL(void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf));
DECL(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
DECL(void, glScissor, (GLint, GLint, GLsizei, GLsizei));
DECL(void, glMatrixMode, (GLenum));
DECL(void, glLoadIdentity, ());
DECL(void, glLoadMatrixf, (const GLfloat*));
DECL(void, glMultMatrixf, (const GLfloat*));
DECL(void, glOrtho, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble));
DECL(void, glTranslatef, (GLfloat, GLfloat, GLfloat));
DECL(void, glRotatef, (GLfloat, GLfloat, GLfloat, GLfloat));
DECL(void, glScalef, (GLfloat, GLfloat, GLfloat));
DECL(void, glPushMatrix, ());
DECL(void, glPopMatrix, ());
DECL(void, glPushAttrib, (GLbitfield));
DECL(void, glPopAttrib, ());
DECL(void, glNewList, (GLuint, GLenum));
DECL(void, glEndList, ());
DECL(void, glCallList, (GLuint));
DECL(void, glLineWidth, (GLfloat));
DECL(void, glPolygonMode, (GLenum, GLenum));
DECL(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*));

// ---- extensions (resolved through wglGetProcAddress)
DECL(HGLRC, wglCreateContextAttribsARB, (HDC, HGLRC, const int*));
DECL(BOOL, wglSwapIntervalEXT, (int));
DECL(void, glActiveTexture, (GLenum));
DECL(void, glActiveTextureARB, (GLenum));
DECL(void, glUseProgram, (GLuint));
DECL(void, glUseProgramObjectARB, (GLuint));
DECL(void, glBindFramebuffer, (GLenum, GLuint));
DECL(void, glBindFramebufferEXT, (GLenum, GLuint));
DECL(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*));
DECL(void, glCompressedTexImage2DARB, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*));
DECL(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*));
DECL(void, glShaderSourceARB, (GLuint, GLsizei, const GLchar* const*, const GLint*));

// ---- Forts.exe imports
DECL(BOOL, SwapBuffers, (HDC));
DECL(HANDLE, CreateFileW, (LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE));
DECL(HANDLE, CreateFileA, (LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE));
DECL(unsigned char, ilLoadL, (unsigned int, const void*, unsigned int));
DECL(void, ilBindImage, (unsigned int));
DECL(GLuint, ilutGLBindTexImage, ());
DECL(GLuint, ilutGLBindMipmaps, ());
DECL(unsigned char, ilutGLTexImage, (GLuint));
DECL(unsigned char, ilutGLBuildMipmaps, ());

// ================================================================ display lists

struct ListInfo {
    unsigned begins = 0, verts = 0, matrixOps = 0;
    std::vector<GLuint> textures;
    std::map<std::string, unsigned> calls;
    std::vector<GeoBatch> geo;   // the vertices compiled into the list (terrain geometry)
};
static std::unordered_map<GLuint, ListInfo> g_lists;
static ListInfo* g_curList;
static DWORD g_listThread;
bool g_compiling;

// The geometry compiled into a display list (object space), or null.
const std::vector<GeoBatch>* ListGeometry(GLuint id) {
    auto it = g_lists.find(id);
    return it == g_lists.end() || it->second.geo.empty() ? nullptr : &it->second.geo;
}
GLuint g_callListId;

void ListNoteCall(const char* fn) {
    if (g_curList && GetCurrentThreadId() == g_listThread) g_curList->calls[fn]++;
}

static std::string ListSummary(GLuint id, std::vector<GLuint>& tex) {
    auto it = g_lists.find(id);
    if (it == g_lists.end()) return "not recorded";
    const ListInfo& l = it->second;
    tex = l.textures;
    std::string s = Fmt("begins=%u verts=%u mtx=%u", l.begins, l.verts, l.matrixOps);
    if (!l.textures.empty()) {
        s += " tex=";
        for (size_t i = 0; i < l.textures.size() && i < 8; i++) s += Fmt(i ? ",%u" : "%u", l.textures[i]);
    }
    for (auto& c : l.calls) s += Fmt(" %s*%u", c.first.c_str(), c.second);
    return s;
}

// Bookkeeping shared by most wrappers: display-list contents and per-frame call counts.
static inline void Track(const char* fn) {
    if (g_curList) ListNoteCall(fn);
    if (CapOn()) CapCount(fn);
}

// ================================================================ texture table

struct TexInfo {
    int w = 0, h = 0;
    GLenum ifmt = 0, fmt = 0, type = 0;
    uint64_t hash = 0;
    size_t bytes = 0;
    std::string src;
    unsigned uploads = 0, mipUploads = 0;
};
static std::mutex g_texMx;
static std::unordered_map<GLuint, TexInfo> g_tex;
static std::unordered_map<unsigned, std::string> g_ilNames;   // DevIL image id -> where its data came from
static unsigned g_ilBound;
static bool g_ilFresh;                                         // an ilLoadL result not yet uploaded
static std::string g_uploadName;                               // set while an ilutGL* call uploads
static thread_local std::string t_lastImageFile;
static thread_local std::string t_triedImageFile;   // an image the game looked for on disk and didn't find: packed (DLC) content

static std::string IlName() {
    auto it = g_ilNames.find(g_ilBound);
    return it != g_ilNames.end() ? it->second : Fmt("il#%u", g_ilBound);
}

std::string TexDescribe(GLuint id) {
    std::lock_guard<std::mutex> lk(g_texMx);
    auto it = g_tex.find(id);
    if (it == g_tex.end()) return "(no upload seen)";
    const TexInfo& t = it->second;
    return Fmt("%dx%d %s data=%s/%s hash=%016llx uploads=%u mips=%u src=%s", t.w, t.h, E(t.ifmt), E(t.fmt),
               t.type ? E(t.type) : "compressed", (unsigned long long)t.hash, t.uploads, t.mipUploads,
               t.src.empty() ? "?" : t.src.c_str());
}

void TexForEach(void (*fn)(GLuint id, const std::string& src)) {
    std::vector<std::pair<GLuint, std::string>> all;
    {
        std::lock_guard<std::mutex> lk(g_texMx);
        for (auto& t : g_tex) all.emplace_back(t.first, t.second.src);
    }
    for (auto& t : all) fn(t.first, t.second);
}

bool TexSize(GLuint id, int& w, int& h) {
    std::lock_guard<std::mutex> lk(g_texMx);
    auto it = g_tex.find(id);
    if (it == g_tex.end()) return false;
    w = it->second.w; h = it->second.h;
    return w > 0 && h > 0;
}

std::string TexShort(GLuint id) {
    if (!id) return "";
    std::lock_guard<std::mutex> lk(g_texMx);
    auto it = g_tex.find(id);
    if (it == g_tex.end()) return "[?]";
    const std::string& s = it->second.src;
    std::string tail = s.size() > 48 ? "..." + s.substr(s.size() - 45) : s;
    return Fmt("[%dx%d %s]", it->second.w, it->second.h, tail.c_str());
}

static size_t UnpackedSize(GLsizei w, GLsizei h, GLenum fmt, GLenum type) {
    if (w <= 0 || h <= 0) return 0;
    int bpp;
    switch (type) {
    case 0x8035: case 0x8367: case 0x8368: bpp = 4; break;                         // packed 32-bit
    case 0x8033: case 0x8365: case 0x8034: case 0x8366: case 0x8363: case 0x8364: bpp = 2; break;
    case 0x8032: bpp = 1; break;
    default: {
        int bpc = (type == 0x1400 || type == 0x1401) ? 1
                : (type == 0x1402 || type == 0x1403 || type == 0x140B) ? 2
                : (type == 0x1404 || type == 0x1405 || type == 0x1406) ? 4 : 0;
        int comps = (fmt == 0x1908 || fmt == 0x80E1) ? 4
                  : (fmt == 0x1907 || fmt == 0x80E0) ? 3
                  : (fmt == 0x8227 || fmt == 0x190A) ? 2
                  : (fmt == 0x1903 || fmt == 0x1904 || fmt == 0x1905 || fmt == 0x1906 || fmt == 0x1909 || fmt == 0x1902) ? 1 : 0;
        bpp = bpc * comps;
    }
    }
    if (!bpp) return 0;
    GLint align = 4, rowLen = 0, skipRows = 0, skipPix = 0;
    R.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLen);
    R.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    R.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPix);
    if (skipRows || skipPix || align <= 0) return 0;
    size_t stride = (size_t)(rowLen > 0 ? rowLen : w) * bpp;
    stride = (stride + align - 1) / align * align;
    return stride * (h - 1) + (size_t)w * bpp;
}

static void OnUpload(const char* fn, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                     GLenum fmt, GLenum type, const void* data, GLsizei compressedSize) {
    if (target != GL_TEXTURE_2D) {
        if (level == 0) SessionLog("%s target=%s %dx%d %s (not tracked)", fn, E(target), w, h, E(ifmt));
        return;
    }
    GLint tex = 0;
    R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    if (level != 0) {
        std::lock_guard<std::mutex> lk(g_texMx);
        g_tex[tex].mipUploads++;
        return;
    }
    size_t bytes = 0;
    uint64_t hash = 0;
    GLint unpackBuf = 0;
    R.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
    if (data && !unpackBuf) {
        bytes = compressedSize > 0 ? (size_t)compressedSize : UnpackedSize(w, h, fmt, type);
        hash = SafeHash(data, bytes);
    }
    // A direct upload only belongs to the bound DevIL image if nothing has consumed it since
    // ilLoadL; otherwise it's game-generated (e.g. font atlases).
    std::string src = !g_uploadName.empty() ? g_uploadName
                    : !data ? std::string("(allocated, no data)")
                    : g_ilFresh ? IlName() + " (direct)"
                    : std::string("(generated by game)");
    if (g_uploadName.empty() && data) g_ilFresh = false;
    unsigned n;
    {
        std::lock_guard<std::mutex> lk(g_texMx);
        TexInfo& t = g_tex[tex];
        t.w = w; t.h = h; t.ifmt = ifmt; t.fmt = fmt; t.type = type;
        t.hash = hash; t.bytes = bytes; t.src = src;
        n = ++t.uploads;
    }
    GfxClassifyTexture(tex, src);
    GfxNoteTextureFormat(tex, ifmt);
    GfxNoteTextureSize(tex, w, h);
    if (n <= 3)
        LoadLog("TEX %u %dx%d %s data=%s/%s bytes=%zu hash=%016llx upload#%u src=%s", tex, w, h, E(ifmt), E(fmt),
                   type ? E(type) : "compressed", bytes, (unsigned long long)hash, n, src.c_str());
    if (CapOn())
        CapLine("%s(tex %u, %dx%d, %s, data %s/%s) hash=%016llx", fn, tex, w, h, E(ifmt), E(fmt),
                type ? E(type) : "compressed", (unsigned long long)hash);
}

// ================================================================ frame boundary

static BOOL DoSwap(HDC dc, BOOL (WINAPI* real)(HDC), const char* via) {
    static thread_local bool inSwap;
    if (inSwap) return real(dc);
    inSwap = true;
    static bool announced;
    if (!announced) {
        announced = true;
        SessionLog("first frame presented via %s", via);
        WriteProcList();
        EngineInit();
    }
    EngineFrame();
    GfxFrameEnd();
    CapFrameEnd();
    OverlayFrame();       // after the capture read-back, so captures never contain the panel
    BOOL ok = real(dc);
    CapFrameBegin();
    GfxFrameBegin();
    inSwap = false;
    return ok;
}

extern "C" BOOL WINAPI hk_SwapBuffers(HDC dc) { return DoSwap(dc, r_SwapBuffers, "gdi32!SwapBuffers"); }
extern "C" BOOL WINAPI hk_wglSwapBuffers(HDC dc) { return DoSwap(dc, r_wglSwapBuffers, "wglSwapBuffers"); }

// ================================================================ contexts

static void LogContextInfo() {
    auto S = [](GLenum e) { const GLubyte* s = R.GetString(e); return s ? (const char*)s : "(null)"; };
    GLint profile = 0;
    R.GetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    SessionLog("context: %s | %s | GL %s | GLSL %s | profile mask 0x%X", S(GL_VENDOR), S(GL_RENDERER),
               S(GL_VERSION), S(GL_SHADING_LANGUAGE_VERSION), profile);
    R.extLoaded = false;
    LoadRealExt();
}

extern "C" HGLRC WINAPI hk_wglCreateContext(HDC dc) {
    HGLRC rc = r_wglCreateContext(dc);
    SessionLog("wglCreateContext(%p) -> %p", dc, rc);
    InstallIatHooks();
    return rc;
}

extern "C" BOOL WINAPI hk_wglMakeCurrent(HDC dc, HGLRC rc) {
    BOOL ok = r_wglMakeCurrent(dc, rc);
    static int logged;
    if (logged < 16) { logged++; SessionLog("wglMakeCurrent(%p, %p) -> %d", dc, rc, ok); }
    static HGLRC described;
    if (ok && rc && rc != described) {
        described = rc;
        LogContextInfo();
        InstallIatHooks();
    }
    return ok;
}

extern "C" BOOL WINAPI hk_wglDeleteContext(HGLRC rc) {
    SessionLog("wglDeleteContext(%p)", rc);
    return r_wglDeleteContext(rc);
}

extern "C" HGLRC WINAPI hk_wglCreateContextAttribsARB(HDC dc, HGLRC share, const int* attribs) {
    std::string a;
    for (const int* p = attribs; p && p[0]; p += 2) a += Fmt("0x%X=0x%X ", p[0], p[1]);
    HGLRC rc = r_wglCreateContextAttribsARB(dc, share, attribs);
    SessionLog("wglCreateContextAttribsARB(share=%p, %s) -> %p", share, a.c_str(), rc);
    InstallIatHooks();
    return rc;
}

extern "C" BOOL WINAPI hk_wglSwapIntervalEXT(int interval) {
    SessionLog("wglSwapIntervalEXT(%d)", interval);
    return r_wglSwapIntervalEXT(interval);
}

// ================================================================ immediate mode

extern "C" void WINAPI hk_glBegin(GLenum mode) {
    g_beginMode = mode;
    if (g_curList && GetCurrentThreadId() == g_listThread) g_curList->geo.push_back({ (unsigned)mode, {} });
    if (!g_curList) GfxBeforeDraw(false);
    if (g_slowPath) {
        if (g_curList) g_curList->begins++;
        if (CapOn()) { CapCount("glBegin"); CapBeginBlock(mode, g_curList != nullptr); }
    }
    r_glBegin(mode);
}

extern "C" void WINAPI hk_glEnd() {
    r_glEnd();
    if (g_texRestore) GfxRestoreTexture();   // a draw that used one of our textures (the milk miko)
    if (g_trackDraw) GfxEndTrack();
    if (g_slowPath && CapOn()) { CapCount("glEnd"); CapEndBlock(); }
}

static inline void Vtx(const char* fn, float x, float y, float z) {
    if (g_curList) {
        g_curList->verts++;
        if (!g_curList->geo.empty() && g_curList->geo.back().xy.size() < (1u << 22)) {
            g_curList->geo.back().xy.push_back(x);
            g_curList->geo.back().xy.push_back(y);
        }
    }
    if (CapOn()) CapVertex(fn, x, y, z);
}
extern "C" void WINAPI hk_glVertex2f(GLfloat x, GLfloat y) { if (g_trackDraw) GfxTrackVertex(x, y, 0); if (g_slowPath) Vtx("glVertex2f", x, y, 0); r_glVertex2f(x, y); }
extern "C" void WINAPI hk_glVertex2fv(const GLfloat* v) { if (g_trackDraw) GfxTrackVertex(v[0], v[1], 0); if (g_slowPath) Vtx("glVertex2fv", v[0], v[1], 0); r_glVertex2fv(v); }
extern "C" void WINAPI hk_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { if (g_trackDraw) GfxTrackVertex(x, y, z); if (g_slowPath) Vtx("glVertex3f", x, y, z); r_glVertex3f(x, y, z); }
extern "C" void WINAPI hk_glVertex3fv(const GLfloat* v) { if (g_trackDraw) GfxTrackVertex(v[0], v[1], v[2]); if (g_slowPath) Vtx("glVertex3fv", v[0], v[1], v[2]); r_glVertex3fv(v); }

extern "C" void WINAPI hk_glTexCoord2f(GLfloat s, GLfloat t) {
    if (g_slowPath && CapOn()) CapTexCoord("glTexCoord2f", s, t);
    r_glTexCoord2f(s, t);
}
extern "C" void WINAPI hk_glTexCoord2fv(const GLfloat* v) {
    if (g_slowPath && CapOn()) CapTexCoord("glTexCoord2fv", v[0], v[1]);
    r_glTexCoord2fv(v);
}

extern "C" void WINAPI hk_glColor4f(GLfloat cr, GLfloat cg, GLfloat cb, GLfloat ca) {
    if (g_trackDraw) GfxTrackColor(cr, cg, cb, ca);
    if (g_slowPath) {
        if (g_curList) ListNoteCall("glColor4f");
        if (CapOn()) { float c[4] = { cr, cg, cb, ca }; CapColor("glColor4f", c); }
    }
    r_glColor4f(cr, cg, cb, ca);
}
extern "C" void WINAPI hk_glColor4fv(const GLfloat* c) {
    if (g_trackDraw) GfxTrackColor(c[0], c[1], c[2], c[3]);
    if (g_slowPath) {
        if (g_curList) ListNoteCall("glColor4fv");
        if (CapOn()) CapColor("glColor4fv", c);
    }
    r_glColor4fv(c);
}

// ================================================================ state

extern "C" void WINAPI hk_glEnable(GLenum cap) {
    if (g_slowPath) { Track("glEnable"); if (CapOn()) CapLine("glEnable(%s)", E(cap)); }
    r_glEnable(cap);
}
extern "C" void WINAPI hk_glDisable(GLenum cap) {
    if (g_slowPath) { Track("glDisable"); if (CapOn()) CapLine("glDisable(%s)", E(cap)); }
    r_glDisable(cap);
}
extern "C" void WINAPI hk_glBlendFunc(GLenum s, GLenum d) {
    if (g_slowPath) { Track("glBlendFunc"); if (CapOn()) CapLine("glBlendFunc(%s, %s)", BlendName(s), BlendName(d)); }
    r_glBlendFunc(s, d);
}
extern "C" void WINAPI hk_glAlphaFunc(GLenum f, GLclampf ref) {
    if (g_slowPath) { Track("glAlphaFunc"); if (CapOn()) CapLine("glAlphaFunc(%s, %g)", E(f), ref); }
    r_glAlphaFunc(f, ref);
}
extern "C" void WINAPI hk_glColorMask(GLboolean cr, GLboolean cg, GLboolean cb, GLboolean ca) {
    if (g_slowPath) { Track("glColorMask"); if (CapOn()) CapLine("glColorMask(%d, %d, %d, %d)", cr, cg, cb, ca); }
    if (GfxHudMasked()) return;   // lab "hide HUD" keeps colour writes off until the swap
    r_glColorMask(cr, cg, cb, ca);
}
extern "C" void WINAPI hk_glDepthFunc(GLenum f) {
    if (g_slowPath) { Track("glDepthFunc"); if (CapOn()) CapLine("glDepthFunc(%s)", E(f)); }
    r_glDepthFunc(f);
}
extern "C" void WINAPI hk_glLineWidth(GLfloat w) {
    if (g_slowPath) { Track("glLineWidth"); if (CapOn()) CapLine("glLineWidth(%g)", w); }
    r_glLineWidth(w);
}
extern "C" void WINAPI hk_glPolygonMode(GLenum face, GLenum mode) {
    if (g_slowPath) { Track("glPolygonMode"); if (CapOn()) CapLine("glPolygonMode(%s, %s)", E(face), E(mode)); }
    r_glPolygonMode(face, mode);
}
extern "C" void WINAPI hk_glPushAttrib(GLbitfield mask) {
    if (g_slowPath) { Track("glPushAttrib"); if (CapOn()) CapLine("glPushAttrib(0x%X)", mask); }
    r_glPushAttrib(mask);
}
extern "C" void WINAPI hk_glPopAttrib() {
    if (g_slowPath) { Track("glPopAttrib"); if (CapOn()) CapLine("glPopAttrib()"); }
    r_glPopAttrib();
}

extern "C" void WINAPI hk_glClear(GLbitfield mask) {
    if (g_slowPath) {
        Track("glClear");
        if (CapOn()) {
            if (!g_curList) CapMaybeSnapshot("before_clear");
            CapLine("glClear(%s%s%s%s)", (mask & 0x4000) ? "COLOR " : "", (mask & 0x100) ? "DEPTH " : "",
                    (mask & 0x400) ? "STENCIL " : "", (mask & 0x200) ? "ACCUM " : "");
        }
    }
    r_glClear(mask);
    if (!g_curList) GfxAfterClear(mask);
}
extern "C" void WINAPI hk_glClearColor(GLclampf cr, GLclampf cg, GLclampf cb, GLclampf ca) {
    if (g_slowPath) { Track("glClearColor"); if (CapOn()) CapLine("glClearColor(%.3f, %.3f, %.3f, %.3f)", cr, cg, cb, ca); }
    r_glClearColor(cr, cg, cb, ca);
}
extern "C" void WINAPI hk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (g_slowPath) { Track("glViewport"); if (CapOn()) CapLine("glViewport(%d, %d, %d, %d)", x, y, w, h); }
    r_glViewport(x, y, w, h);
}
extern "C" void WINAPI hk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (g_slowPath) { Track("glScissor"); if (CapOn()) CapLine("glScissor(%d, %d, %d, %d)", x, y, w, h); }
    r_glScissor(x, y, w, h);
}
extern "C" void WINAPI hk_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* px) {
    if (g_slowPath) { Track("glReadPixels"); if (CapOn()) CapLine("glReadPixels(%d, %d, %d, %d, %s, %s)", x, y, w, h, E(fmt), E(type)); }
    r_glReadPixels(x, y, w, h, fmt, type, px);
}

// ================================================================ matrices

static inline void MatrixOp(const char* fn) {
    if (g_curList) g_curList->matrixOps++;
    if (CapOn()) CapCount(fn);
}

// The world -> HUD switch is most likely a projection reload, so snapshot just before one.
static void ProjectionLoad(const char* fn) {
    if (!CapOn() || g_curList) return;
    GLint mm = 0;
    R.GetIntegerv(GL_MATRIX_MODE, &mm);
    if (mm == GL_PROJECTION) CapMaybeSnapshot(Fmt("before_%s", fn).c_str());
}

static std::string MatStr(const GLfloat* m) {
    std::string s;
    for (int i = 0; i < 16; i++) s += Fmt(i ? (i % 4 ? " %.5g" : " | %.5g") : "%.5g", m[i]);
    return s;
}

extern "C" void WINAPI hk_glMatrixMode(GLenum m) {
    if (g_slowPath) { MatrixOp("glMatrixMode"); if (CapOn()) CapLine("glMatrixMode(%s)", E(m)); }
    r_glMatrixMode(m);
}
extern "C" void WINAPI hk_glLoadIdentity() {
    if (g_slowPath) { MatrixOp("glLoadIdentity"); ProjectionLoad("glLoadIdentity"); if (CapOn()) CapLine("glLoadIdentity()"); }
    r_glLoadIdentity();
}
extern "C" void WINAPI hk_glLoadMatrixf(const GLfloat* m) {
    if (g_slowPath) { MatrixOp("glLoadMatrixf"); ProjectionLoad("glLoadMatrixf"); if (CapOn()) CapLine("glLoadMatrixf(%s)", MatStr(m).c_str()); }
    r_glLoadMatrixf(m);
    if (!g_curList) GfxOnLoadMatrix();
}
extern "C" void WINAPI hk_glMultMatrixf(const GLfloat* m) {
    if (g_slowPath) { MatrixOp("glMultMatrixf"); if (CapOn()) CapLine("glMultMatrixf(%s)", MatStr(m).c_str()); }
    r_glMultMatrixf(m);
}
extern "C" void WINAPI hk_glOrtho(GLdouble l, GLdouble rt, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (g_slowPath) { MatrixOp("glOrtho"); ProjectionLoad("glOrtho"); if (CapOn()) CapLine("glOrtho(%g, %g, %g, %g, %g, %g)", l, rt, b, t, n, f); }
    r_glOrtho(l, rt, b, t, n, f);
    if (!g_curList) GfxOnOrtho(l, rt, b, t);
}
extern "C" void WINAPI hk_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    if (g_slowPath) { MatrixOp("glTranslatef"); if (CapOn()) CapLine("glTranslatef(%g, %g, %g)", x, y, z); }
    r_glTranslatef(x, y, z);
}
extern "C" void WINAPI hk_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
    if (g_slowPath) { MatrixOp("glRotatef"); if (CapOn()) CapLine("glRotatef(%g, %g, %g, %g)", a, x, y, z); }
    r_glRotatef(a, x, y, z);
}
extern "C" void WINAPI hk_glScalef(GLfloat x, GLfloat y, GLfloat z) {
    if (g_slowPath) { MatrixOp("glScalef"); if (CapOn()) CapLine("glScalef(%g, %g, %g)", x, y, z); }
    r_glScalef(x, y, z);
}
extern "C" void WINAPI hk_glPushMatrix() {
    if (g_slowPath) { MatrixOp("glPushMatrix"); if (CapOn()) { CapLine("glPushMatrix()"); CapIndent(1); } }
    r_glPushMatrix();
}
extern "C" void WINAPI hk_glPopMatrix() {
    if (g_slowPath) { MatrixOp("glPopMatrix"); if (CapOn()) { CapIndent(-1); CapLine("glPopMatrix()"); } }
    r_glPopMatrix();
}

// ================================================================ display lists

extern "C" void WINAPI hk_glNewList(GLuint list, GLenum mode) {
    r_glNewList(list, mode);
    g_lists[list] = ListInfo{};
    g_curList = &g_lists[list];
    g_listThread = GetCurrentThreadId();
    g_compiling = true;
    UpdateSlowPath();
    if (CapOn()) { CapLine("glNewList(%u, %s)", list, E(mode)); CapIndent(1); }
}

extern "C" void WINAPI hk_glEndList() {
    r_glEndList();
    g_curList = nullptr;
    g_compiling = false;
    UpdateSlowPath();
    if (CapOn()) { CapIndent(-1); CapLine("glEndList()"); }
}

extern "C" void WINAPI hk_glCallList(GLuint list) {
    g_callListId = list;
    if (!g_curList) GfxBeforeDraw(true);
    if (g_slowPath) {
        if (g_curList) ListNoteCall("glCallList");
        if (CapOn()) {
            std::vector<GLuint> tex;
            std::string sum = ListSummary(list, tex);
            CapCallList(list, sum, tex, g_curList != nullptr);
        }
    }
    r_glCallList(list);
    if (g_texRestore) GfxRestoreTexture();
}

// ================================================================ textures

extern "C" void WINAPI hk_glBindTexture(GLenum target, GLuint tex) {
    if (g_slowPath) {
        Track("glBindTexture");
        if (g_curList && target == GL_TEXTURE_2D) g_curList->textures.push_back(tex);
        if (CapOn()) CapLine("glBindTexture(%s, %u) %s", E(target), tex, TexShort(tex).c_str());
    }
    r_glBindTexture(target, tex);
}

extern "C" void WINAPI hk_glDeleteTextures(GLsizei n, const GLuint* ids) {
    if (g_slowPath) { Track("glDeleteTextures"); if (CapOn()) CapLine("glDeleteTextures(%d, first=%u)", n, n > 0 ? ids[0] : 0); }
    {
        std::lock_guard<std::mutex> lk(g_texMx);
        for (GLsizei i = 0; i < n; i++) g_tex.erase(ids[i]);
    }
    for (GLsizei i = 0; i < n; i++) GfxForgetTexture(ids[i]);
    r_glDeleteTextures(n, ids);
}

extern "C" void WINAPI hk_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                                       GLenum fmt, GLenum type, const void* data) {
    r_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
    if (g_curList) { Track("glTexImage2D"); return; }
    OnUpload("glTexImage2D", target, level, ifmt, w, h, fmt, type, data, 0);
}

extern "C" void WINAPI hk_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                          GLenum fmt, GLenum type, const void* data) {
    r_glTexSubImage2D(target, level, x, y, w, h, fmt, type, data);
    if (g_slowPath) {
        Track("glTexSubImage2D");
        if (CapOn()) {
            GLint tex = 0;
            R.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
            CapLine("glTexSubImage2D(%s, level %d, tex %u, at %d,%d size %dx%d, %s/%s)", E(target), level, tex, x, y, w, h, E(fmt), E(type));
        }
    }
}

extern "C" void WINAPI hk_glTexParameteri(GLenum target, GLenum pname, GLint v) {
    if (g_slowPath) { Track("glTexParameteri"); if (CapOn()) CapLine("glTexParameteri(%s, %s, %s)", E(target), E(pname), E((GLenum)v)); }
    r_glTexParameteri(target, pname, v);
}
extern "C" void WINAPI hk_glTexParameterf(GLenum target, GLenum pname, GLfloat v) {
    if (g_slowPath) { Track("glTexParameterf"); if (CapOn()) CapLine("glTexParameterf(%s, %s, %g = %s)", E(target), E(pname), v, E((GLenum)v)); }
    r_glTexParameterf(target, pname, v);
}
extern "C" void WINAPI hk_glTexEnvf(GLenum target, GLenum pname, GLfloat v) {
    if (g_slowPath) { Track("glTexEnvf"); if (CapOn()) CapLine("glTexEnvf(%s, %s, %g = %s)", E(target), E(pname), v, E((GLenum)v)); }
    r_glTexEnvf(target, pname, v);
}

static void CompressedUpload(const char* fn, GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                             GLsizei size, const void* data) {
    if (g_curList) { Track(fn); return; }
    OnUpload(fn, target, level, ifmt, w, h, ifmt, 0, data, size);
}
extern "C" void WINAPI hk_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                                 GLint border, GLsizei size, const void* data) {
    r_glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
    CompressedUpload("glCompressedTexImage2D", target, level, ifmt, w, h, size, data);
}
extern "C" void WINAPI hk_glCompressedTexImage2DARB(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                                    GLint border, GLsizei size, const void* data) {
    r_glCompressedTexImage2DARB(target, level, ifmt, w, h, border, size, data);
    CompressedUpload("glCompressedTexImage2DARB", target, level, ifmt, w, h, size, data);
}

// ================================================================ extension state

static void ActiveTexture(GLenum t) {
    if (g_slowPath) { Track("glActiveTexture"); if (CapOn()) CapLine("glActiveTexture(%s)", E(t)); }
}
extern "C" void WINAPI hk_glActiveTexture(GLenum t) { ActiveTexture(t); r_glActiveTexture(t); }
extern "C" void WINAPI hk_glActiveTextureARB(GLenum t) { ActiveTexture(t); r_glActiveTextureARB(t); }

static void UseProgram(GLuint p) {
    if (g_slowPath) { Track("glUseProgram"); if (CapOn()) CapLine("glUseProgram(%u)", p); }
}
extern "C" void WINAPI hk_glUseProgram(GLuint p) { UseProgram(p); r_glUseProgram(p); }
extern "C" void WINAPI hk_glUseProgramObjectARB(GLuint p) { UseProgram(p); r_glUseProgramObjectARB(p); }

static void BindFramebuffer(GLenum target, GLuint fb) {
    if (!g_slowPath) return;
    Track("glBindFramebuffer");
    if (!CapOn()) return;
    if (target != GL_READ_FRAMEBUFFER) CapMaybeSnapshot(Fmt("before_bind_fbo%u", fb).c_str());
    CapLine("glBindFramebuffer(%s, %u)", E(target), fb);
}
extern "C" void WINAPI hk_glBindFramebuffer(GLenum t, GLuint fb) {
    BindFramebuffer(t, fb);
    r_glBindFramebuffer(t, GfxRedirectFramebuffer(fb));
}
extern "C" void WINAPI hk_glBindFramebufferEXT(GLenum t, GLuint fb) {
    BindFramebuffer(t, fb);
    r_glBindFramebufferEXT(t, GfxRedirectFramebuffer(fb));
}

static void ShaderSource(const char* fn, GLuint sh, GLsizei count, const GLchar* const* str, const GLint* len) {
    std::string src;
    for (GLsizei i = 0; str && i < count; i++)
        if (str[i]) src.append(str[i], (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]));
    SessionLog("%s(shader %u, %d part(s), %zu chars)\n-----8<-----\n%s\n-----8<-----", fn, sh, count, src.size(), src.c_str());
}
extern "C" void WINAPI hk_glShaderSource(GLuint sh, GLsizei n, const GLchar* const* s, const GLint* l) {
    ShaderSource("glShaderSource", sh, n, s, l);
    r_glShaderSource(sh, n, s, l);
}
extern "C" void WINAPI hk_glShaderSourceARB(GLuint sh, GLsizei n, const GLchar* const* s, const GLint* l) {
    ShaderSource("glShaderSourceARB", sh, n, s, l);
    r_glShaderSourceARB(sh, n, s, l);
}

struct ExtHook { const char* name; void* hook; void** real; };
#define EXT(n) { #n, (void*)hk_##n, (void**)&r_##n }
static const ExtHook kExtHooks[] = {
    EXT(wglCreateContextAttribsARB), EXT(wglSwapIntervalEXT),
    EXT(glActiveTexture), EXT(glActiveTextureARB), EXT(glUseProgram), EXT(glUseProgramObjectARB),
    EXT(glBindFramebuffer), EXT(glBindFramebufferEXT), EXT(glCompressedTexImage2D), EXT(glCompressedTexImage2DARB),
    EXT(glShaderSource), EXT(glShaderSourceARB),
};
#undef EXT

extern "C" PROC WINAPI hk_wglGetProcAddress(LPCSTR name) {
    PROC p = r_wglGetProcAddress(name);
    intptr_t v = (intptr_t)p;
    if (!name || v <= 3 || v == -1) { RecordProcName(name, false); return p; }
    RecordProcName(name, true);
    for (auto& h : kExtHooks)
        if (strcmp(name, h.name) == 0) { *h.real = (void*)p; return (PROC)h.hook; }
    return MakeGenericProc(name, p);
}

// ================================================================ Forts.exe imports: file + DevIL naming

static bool IsImageExt(const char* dot) {
    return dot && (!_stricmp(dot, ".png") || !_stricmp(dot, ".tga") || !_stricmp(dot, ".dds") ||
                   !_stricmp(dot, ".jpg") || !_stricmp(dot, ".jpeg") || !_stricmp(dot, ".bmp"));
}

extern "C" HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                        DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = r_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (name && h != INVALID_HANDLE_VALUE) {
        std::string n = Narrow(name);
        if (IsImageExt(strrchr(n.c_str(), '.'))) t_lastImageFile = n;
        else if (n.size() > 4 && _stricmp(n.c_str() + n.size() - 4, ".lua") == 0) GfxNoteLuaFile(n);   // maps: water level
    } else if (name) {
        std::string n = Narrow(name);
        if (IsImageExt(strrchr(n.c_str(), '.'))) t_triedImageFile = n;
    }
    return h;
}

extern "C" HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                        DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = r_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    if (name && h != INVALID_HANDLE_VALUE && IsImageExt(strrchr(name, '.'))) t_lastImageFile = name;
    else if (name && h == INVALID_HANDLE_VALUE && IsImageExt(strrchr(name, '.'))) t_triedImageFile = name;
    else if (name && h != INVALID_HANDLE_VALUE && strlen(name) > 4 && _stricmp(name + strlen(name) - 4, ".lua") == 0) GfxNoteLuaFile(name);
    return h;
}

extern "C" void WINAPI hk_ilBindImage(unsigned int img) {
    g_ilBound = img;
    r_ilBindImage(img);
}

extern "C" unsigned char WINAPI hk_ilLoadL(unsigned int type, const void* lump, unsigned int size) {
    std::string file = t_lastImageFile, tried = t_triedImageFile;
    t_lastImageFile.clear();
    t_triedImageFile.clear();
    uint64_t h = SafeHash(lump, size);
    unsigned char ok = r_ilLoadL(type, lump, size);
    // Packed content (the DLCs) has no file of its own; the name the game just looked for on disk is it.
    std::string label = !file.empty() ? file : !tried.empty() ? tried : Fmt("pack#%016llx", (unsigned long long)h);
    if (file.empty() && !tried.empty()) LoadLog("  packed image named from the game's lookup: %s", tried.c_str());
    g_ilNames[g_ilBound] = label;
    g_ilFresh = true;
    LoadLog("ilLoadL(il %u, type 0x%X, %u bytes) ok=%d -> %s", g_ilBound, type, size, ok, label.c_str());
    return ok;
}

extern "C" GLuint WINAPI hk_ilutGLBindTexImage() {
    g_uploadName = IlName();
    GLuint t = r_ilutGLBindTexImage();
    g_uploadName.clear();
    return t;
}
extern "C" GLuint WINAPI hk_ilutGLBindMipmaps() {
    g_uploadName = IlName();
    GLuint t = r_ilutGLBindMipmaps();
    g_uploadName.clear();
    return t;
}
extern "C" unsigned char WINAPI hk_ilutGLTexImage(GLuint level) {
    g_uploadName = IlName();
    unsigned char ok = r_ilutGLTexImage(level);
    g_uploadName.clear();
    return ok;
}
extern "C" unsigned char WINAPI hk_ilutGLBuildMipmaps() {
    g_uploadName = IlName();
    unsigned char ok = r_ilutGLBuildMipmaps();
    g_uploadName.clear();
    return ok;
}

// ================================================================ installation

void ResolveStaticHooks(HMODULE real) {
#define S(n) { #n, (void**)&r_##n }
    static const struct { const char* name; void** slot; } kStatic[] = {
        S(wglCreateContext), S(wglMakeCurrent), S(wglDeleteContext), S(wglGetProcAddress), S(wglSwapBuffers),
        S(glBegin), S(glEnd), S(glVertex2f), S(glVertex2fv), S(glVertex3f), S(glVertex3fv),
        S(glTexCoord2f), S(glTexCoord2fv), S(glColor4f), S(glColor4fv),
        S(glBindTexture), S(glDeleteTextures), S(glTexImage2D), S(glTexSubImage2D),
        S(glTexParameteri), S(glTexParameterf), S(glTexEnvf),
        S(glEnable), S(glDisable), S(glBlendFunc), S(glAlphaFunc), S(glColorMask), S(glDepthFunc),
        S(glClear), S(glClearColor), S(glViewport), S(glScissor),
        S(glMatrixMode), S(glLoadIdentity), S(glLoadMatrixf), S(glMultMatrixf), S(glOrtho),
        S(glTranslatef), S(glRotatef), S(glScalef), S(glPushMatrix), S(glPopMatrix),
        S(glPushAttrib), S(glPopAttrib), S(glNewList), S(glEndList), S(glCallList),
        S(glLineWidth), S(glPolygonMode), S(glReadPixels),
    };
#undef S
    for (auto& s : kStatic) {
        *s.slot = (void*)GetProcAddress(real, s.name);
        if (!*s.slot) SessionLog("ERROR: real %s not found", s.name);
    }
}

void InstallIatHooks() {
    struct IatHook { const char* dll; const char* fn; void* hook; void** real; int state; };
#define H(dll, n) { dll, #n, (void*)hk_##n, (void**)&r_##n, -1 }
    static IatHook kIat[] = {
        H("GDI32.dll", SwapBuffers), H("KERNEL32.dll", CreateFileW), H("KERNEL32.dll", CreateFileA),
        H("DevIL.dll", ilLoadL), H("DevIL.dll", ilBindImage),
        H("ILUT.dll", ilutGLBindTexImage), H("ILUT.dll", ilutGLBindMipmaps),
        H("ILUT.dll", ilutGLTexImage), H("ILUT.dll", ilutGLBuildMipmaps),
        H("DINPUT8.dll", DirectInput8Create),
    };
#undef H
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    HMODULE exe = GetModuleHandleW(nullptr);
    for (auto& h : kIat) {
        int ok = PatchIat(exe, h.dll, h.fn, h.hook, h.real) ? 1 : 0;
        if (ok != h.state) SessionLog("IAT hook %s!%s: %s", h.dll, h.fn, ok ? "installed" : "NOT FOUND");
        h.state = ok;
    }
}
