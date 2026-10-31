// FortsGFX - proxy opengl32.dll. Loads the real System32 opengl32.dll, forwards every
// export to it, and provides the shared plumbing (logging, IAT patching, thunks).
#include "common.h"
#include <cstdarg>
#include <mutex>
#include <unordered_map>

extern "C" ProcEntry g_entries[] = {
#include "gen/entries.inc"
};
static const size_t kEntryCount = sizeof(g_entries) / sizeof(g_entries[0]);

extern "C" volatile uint8_t g_slowPath = 0;
RealGL R;
std::wstring g_gameDir, g_outDir;
static HMODULE g_realDll;

void* RealProc(const char* name) {
    if (void* p = (void*)GetProcAddress(g_realDll, name)) return p;
    PROC p = R.wglGetProcAddress ? R.wglGetProcAddress(name) : nullptr;
    intptr_t v = (intptr_t)p;
    return (v > 3 && v != -1) ? (void*)p : nullptr;
}

static const char kVersion[] = "FortsGFX 1.0";

// ---------------------------------------------------------------- strings

std::string Fmt(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(buf)) return std::string(buf, n < 0 ? 0 : n);
    std::string s(n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&s[0], s.size(), fmt, ap);
    va_end(ap);
    s.resize(n);
    return s;
}

std::string Narrow(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

// ---------------------------------------------------------------- session log

// session.log: what happened (battles, settings, effects, problems). loading.log: every texture and
// script the game loads (thousands of lines), kept out of the way.
static std::mutex g_logMx;
static FILE* g_log;
static FILE* g_loadLog;

static void WriteLog(FILE* f, const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}
void SessionLog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); WriteLog(g_log, fmt, ap); va_end(ap); }
void LoadLog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); WriteLog(g_loadLog, fmt, ap); va_end(ap); }

// ---------------------------------------------------------------- enum names

const char* E(GLenum e) {
    switch (e) {
#define X(name, val) case val: return #name;
    // capabilities
    X(TEXTURE_1D, 0x0DE0) X(TEXTURE_2D, 0x0DE1) X(BLEND, 0x0BE2) X(ALPHA_TEST, 0x0BC0) X(DEPTH_TEST, 0x0B71)
    X(SCISSOR_TEST, 0x0C11) X(CULL_FACE, 0x0B44) X(STENCIL_TEST, 0x0B90) X(LIGHTING, 0x0B50) X(FOG, 0x0B60)
    X(DITHER, 0x0BD0) X(NORMALIZE, 0x0BA1) X(COLOR_MATERIAL, 0x0B57) X(POINT_SMOOTH, 0x0B10) X(LINE_SMOOTH, 0x0B20)
    X(LINE_STIPPLE, 0x0B24) X(POLYGON_SMOOTH, 0x0B41) X(POLYGON_STIPPLE, 0x0B42) X(POLYGON_OFFSET_FILL, 0x8037)
    X(MULTISAMPLE, 0x809D) X(SAMPLE_ALPHA_TO_COVERAGE, 0x809E) X(FRAMEBUFFER_SRGB, 0x8DB9) X(COLOR_LOGIC_OP, 0x0BF2)
    X(TEXTURE_GEN_S, 0x0C60) X(TEXTURE_GEN_T, 0x0C61) X(POINT_SPRITE, 0x8861) X(TEXTURE_RECTANGLE, 0x84F5)
    X(TEXTURE_CUBE_MAP, 0x8513) X(PROXY_TEXTURE_2D, 0x8064) X(CLIP_PLANE0, 0x3000)
    X(VERTEX_ARRAY, 0x8074) X(COLOR_ARRAY, 0x8076) X(TEXTURE_COORD_ARRAY, 0x8078)
    // blend / compare
    X(SRC_COLOR, 0x0300) X(ONE_MINUS_SRC_COLOR, 0x0301) X(SRC_ALPHA, 0x0302) X(ONE_MINUS_SRC_ALPHA, 0x0303)
    X(DST_ALPHA, 0x0304) X(ONE_MINUS_DST_ALPHA, 0x0305) X(DST_COLOR, 0x0306) X(ONE_MINUS_DST_COLOR, 0x0307)
    X(SRC_ALPHA_SATURATE, 0x0308) X(CONSTANT_COLOR, 0x8001) X(ONE_MINUS_CONSTANT_COLOR, 0x8002)
    X(CONSTANT_ALPHA, 0x8003) X(ONE_MINUS_CONSTANT_ALPHA, 0x8004) X(FUNC_ADD, 0x8006) X(MIN, 0x8007) X(MAX, 0x8008)
    X(BLEND_EQUATION, 0x8009) X(FUNC_SUBTRACT, 0x800A) X(FUNC_REVERSE_SUBTRACT, 0x800B)
    X(NEVER, 0x0200) X(LESS, 0x0201) X(EQUAL, 0x0202) X(LEQUAL, 0x0203) X(GREATER, 0x0204) X(NOTEQUAL, 0x0205)
    X(GEQUAL, 0x0206) X(ALWAYS, 0x0207)
    // matrices / faces / modes
    X(MODELVIEW, 0x1700) X(PROJECTION, 0x1701) X(TEXTURE, 0x1702) X(COLOR, 0x1800)
    X(FRONT, 0x0404) X(BACK, 0x0405) X(FRONT_AND_BACK, 0x0408) X(POINT, 0x1B00) X(LINE, 0x1B01) X(FILL, 0x1B02)
    X(FLAT, 0x1D00) X(SMOOTH, 0x1D01) X(COMPILE, 0x1300) X(COMPILE_AND_EXECUTE, 0x1301)
    X(DONT_CARE, 0x1100) X(FASTEST, 0x1101) X(NICEST, 0x1102)
    X(PERSPECTIVE_CORRECTION_HINT, 0x0C50) X(POINT_SMOOTH_HINT, 0x0C51) X(LINE_SMOOTH_HINT, 0x0C52) X(POLYGON_SMOOTH_HINT, 0x0C53)
    // formats / types
    X(DEPTH_COMPONENT, 0x1902) X(RED, 0x1903) X(ALPHA, 0x1906) X(RGB, 0x1907) X(RGBA, 0x1908) X(LUMINANCE, 0x1909)
    X(LUMINANCE_ALPHA, 0x190A) X(RG, 0x8227) X(BGR, 0x80E0) X(BGRA, 0x80E1)
    X(RGBA8, 0x8058) X(RGB8, 0x8051) X(RGBA4, 0x8056) X(RGB5_A1, 0x8057) X(RGB10_A2, 0x8059) X(ALPHA8, 0x803C)
    X(LUMINANCE8, 0x8040) X(LUMINANCE8_ALPHA8, 0x8045) X(INTENSITY, 0x8049) X(INTENSITY8, 0x804B)
    X(R8, 0x8229) X(RG8, 0x822B) X(RGBA16F, 0x881A) X(RGB16F, 0x881B) X(RGBA32F, 0x8814) X(R11F_G11F_B10F, 0x8C3A)
    X(SRGB8, 0x8C41) X(SRGB8_ALPHA8, 0x8C43) X(DEPTH_COMPONENT16, 0x81A5) X(DEPTH_COMPONENT24, 0x81A6)
    X(DEPTH24_STENCIL8, 0x88F0) X(COMPRESSED_RGB, 0x84ED) X(COMPRESSED_RGBA, 0x84EE)
    X(COMPRESSED_RGB_S3TC_DXT1, 0x83F0) X(COMPRESSED_RGBA_S3TC_DXT1, 0x83F1) X(COMPRESSED_RGBA_S3TC_DXT3, 0x83F2)
    X(COMPRESSED_RGBA_S3TC_DXT5, 0x83F3) X(COMPRESSED_RGBA_BPTC, 0x8E8C)
    X(BYTE, 0x1400) X(UNSIGNED_BYTE, 0x1401) X(SHORT, 0x1402) X(UNSIGNED_SHORT, 0x1403) X(INT, 0x1404)
    X(UNSIGNED_INT, 0x1405) X(FLOAT, 0x1406) X(HALF_FLOAT, 0x140B) X(UNSIGNED_INT_8_8_8_8, 0x8035)
    X(UNSIGNED_INT_8_8_8_8_REV, 0x8367) X(UNSIGNED_SHORT_5_6_5, 0x8363) X(UNSIGNED_SHORT_4_4_4_4, 0x8033)
    // texture parameters / env
    X(TEXTURE_MAG_FILTER, 0x2800) X(TEXTURE_MIN_FILTER, 0x2801) X(TEXTURE_WRAP_S, 0x2802) X(TEXTURE_WRAP_T, 0x2803)
    X(NEAREST, 0x2600) X(LINEAR, 0x2601) X(NEAREST_MIPMAP_NEAREST, 0x2700) X(LINEAR_MIPMAP_NEAREST, 0x2701)
    X(NEAREST_MIPMAP_LINEAR, 0x2702) X(LINEAR_MIPMAP_LINEAR, 0x2703) X(CLAMP, 0x2900) X(REPEAT, 0x2901)
    X(CLAMP_TO_EDGE, 0x812F) X(CLAMP_TO_BORDER, 0x812D) X(MIRRORED_REPEAT, 0x8370) X(GENERATE_MIPMAP, 0x8191)
    X(TEXTURE_MIN_LOD, 0x813A) X(TEXTURE_MAX_LOD, 0x813B) X(TEXTURE_BASE_LEVEL, 0x813C) X(TEXTURE_MAX_LEVEL, 0x813D)
    X(TEXTURE_BORDER_COLOR, 0x1004) X(TEXTURE_MAX_ANISOTROPY, 0x84FE) X(TEXTURE_FILTER_CONTROL, 0x8500)
    X(TEXTURE_LOD_BIAS, 0x8501) X(TEXTURE_ENV_MODE, 0x2200) X(TEXTURE_ENV_COLOR, 0x2201) X(TEXTURE_ENV, 0x2300)
    X(MODULATE, 0x2100) X(DECAL, 0x2101) X(REPLACE, 0x1E01) X(ADD, 0x0104) X(COMBINE, 0x8570)
    X(COMBINE_RGB, 0x8571) X(COMBINE_ALPHA, 0x8572)
    // pixel store
    X(UNPACK_SWAP_BYTES, 0x0CF0) X(UNPACK_LSB_FIRST, 0x0CF1) X(UNPACK_ROW_LENGTH, 0x0CF2) X(UNPACK_SKIP_ROWS, 0x0CF3)
    X(UNPACK_SKIP_PIXELS, 0x0CF4) X(UNPACK_ALIGNMENT, 0x0CF5) X(PACK_ROW_LENGTH, 0x0D02) X(PACK_ALIGNMENT, 0x0D05)
    // texture units, buffers, framebuffers, shaders
    X(TEXTURE0, 0x84C0) X(TEXTURE1, 0x84C1) X(TEXTURE2, 0x84C2) X(TEXTURE3, 0x84C3) X(TEXTURE4, 0x84C4)
    X(TEXTURE5, 0x84C5) X(TEXTURE6, 0x84C6) X(TEXTURE7, 0x84C7)
    X(ARRAY_BUFFER, 0x8892) X(ELEMENT_ARRAY_BUFFER, 0x8893) X(PIXEL_PACK_BUFFER, 0x88EB) X(PIXEL_UNPACK_BUFFER, 0x88EC)
    X(FRAMEBUFFER, 0x8D40) X(READ_FRAMEBUFFER, 0x8CA8) X(DRAW_FRAMEBUFFER, 0x8CA9) X(RENDERBUFFER, 0x8D41)
    X(COLOR_ATTACHMENT0, 0x8CE0) X(COLOR_ATTACHMENT1, 0x8CE1) X(DEPTH_ATTACHMENT, 0x8D00) X(STENCIL_ATTACHMENT, 0x8D20)
    X(FRAGMENT_SHADER, 0x8B30) X(VERTEX_SHADER, 0x8B31)
    // common glGet names
    X(CURRENT_COLOR, 0x0B00) X(MATRIX_MODE, 0x0BA0) X(VIEWPORT, 0x0BA2) X(MODELVIEW_MATRIX, 0x0BA6)
    X(PROJECTION_MATRIX, 0x0BA7) X(SCISSOR_BOX, 0x0C10) X(MAX_TEXTURE_SIZE, 0x0D33) X(TEXTURE_BINDING_2D, 0x8069)
    X(FRAMEBUFFER_BINDING, 0x8CA6) X(CURRENT_PROGRAM, 0x8B8D) X(MAX_TEXTURE_UNITS, 0x84E2)
    X(MAX_TEXTURE_MAX_ANISOTROPY, 0x84FF) X(VENDOR, 0x1F00) X(RENDERER, 0x1F01) X(VERSION, 0x1F02)
    X(EXTENSIONS, 0x1F03) X(SHADING_LANGUAGE_VERSION, 0x8B8C) X(NUM_EXTENSIONS, 0x821D)
    X(MAJOR_VERSION, 0x821B) X(MINOR_VERSION, 0x821C)
#undef X
    }
    static thread_local char buf[8][16];
    static thread_local int slot;
    char* b = buf[slot++ & 7];
    snprintf(b, 16, "0x%X", e);
    return b;
}

const char* PrimName(GLenum m) {
    static const char* names[] = { "POINTS", "LINES", "LINE_LOOP", "LINE_STRIP", "TRIANGLES",
                                   "TRIANGLE_STRIP", "TRIANGLE_FAN", "QUADS", "QUAD_STRIP", "POLYGON" };
    return m < 10 ? names[m] : E(m);
}

const char* BlendName(GLenum f) {
    return f == 0 ? "ZERO" : f == 1 ? "ONE" : E(f);
}

// ---------------------------------------------------------------- hashing

static uint64_t HashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (n * 0xFF51AFD7ED558CCDull);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9FB21C651E98DF25ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x9FB21C651E98DF25ull;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

uint64_t SafeHash(const void* p, size_t n) {
    if (!p || !n) return 0;
    __try {
        return HashBytes((const uint8_t*)p, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---------------------------------------------------------------- IAT patching

bool PatchIat(HMODULE mod, const char* dll, const char* fn, void* hook, void** orig) {
    auto base = (uint8_t*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((const char*)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) != 0) continue;
            if ((void*)slots->u1.Function == hook) return true;
            if (!*orig) *orig = (void*)GetProcAddress(GetModuleHandleA(dll), fn);
            if (!*orig) return false;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            slots->u1.Function = (ULONGLONG)hook;
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- generic procs from wglGetProcAddress

static std::mutex g_procMx;
static std::unordered_map<std::string, std::pair<ProcEntry*, void*>> g_dynProcs;
static std::vector<std::pair<std::string, bool>> g_procNames;
static uint8_t* g_thunkPage;
static size_t g_thunkUsed, g_thunkCap;

// Each thunk is: mov r10, <entry>; mov rax, GenericThunk; jmp rax
PROC MakeGenericProc(const char* name, PROC real) {
    std::lock_guard<std::mutex> lk(g_procMx);
    auto it = g_dynProcs.find(name);
    if (it != g_dynProcs.end()) {
        it->second.first->real = (void*)real;
        return (PROC)it->second.second;
    }
    if (!g_thunkPage || g_thunkUsed + 32 > g_thunkCap) {
        g_thunkPage = (uint8_t*)VirtualAlloc(nullptr, 1 << 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_thunkCap = 1 << 16;
        g_thunkUsed = 0;
        if (!g_thunkPage) return real;
    }
    auto e = new ProcEntry{ (void*)real, _strdup(name), {} };
    uint8_t* t = g_thunkPage + g_thunkUsed;
    g_thunkUsed += 32;
    void* target = (void*)&GenericThunk;
    t[0] = 0x49; t[1] = 0xBA; memcpy(t + 2, &e, 8);
    t[10] = 0x48; t[11] = 0xB8; memcpy(t + 12, &target, 8);
    t[20] = 0xFF; t[21] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    g_dynProcs.emplace(name, std::make_pair(e, (void*)t));
    return (PROC)t;
}

void RecordProcName(const char* name, bool resolved) {
    if (!name) return;
    std::lock_guard<std::mutex> lk(g_procMx);
    g_procNames.emplace_back(name, resolved);
}

void WriteProcList() {
    std::lock_guard<std::mutex> lk(g_procMx);
    FILE* f = _wfopen((g_outDir + L"procs.txt").c_str(), L"w");
    if (!f) return;
    size_t ok = 0;
    for (auto& p : g_procNames) ok += p.second;
    fprintf(f, "# wglGetProcAddress requests: %zu (%zu resolved)\n", g_procNames.size(), ok);
    for (auto& p : g_procNames) fprintf(f, "%s %s\n", p.second ? "+" : "-", p.first.c_str());
    fclose(f);
}

// ---------------------------------------------------------------- misc

extern "C" void LogGenericCall(ProcEntry* e, const uint64_t* ints, const uint8_t* xmm) {
    if (g_compiling) ListNoteCall(e->name);
    if (CapOn()) CapGenericCall(e->name, ints, xmm);
}

void UpdateSlowPath() {
    g_slowPath = (g_capActive || g_compiling) ? 1 : 0;
}

bool LoadRealExt() {
    if (R.extLoaded) return true;
    if (!R.wglGetCurrentContext || !R.wglGetCurrentContext()) return false;
    auto P = [](const char* n) -> void* {
        PROC p = R.wglGetProcAddress(n);
        intptr_t v = (intptr_t)p;
        return (v > 3 && v != -1) ? (void*)p : nullptr;
    };
    R.BindFramebuffer = (decltype(R.BindFramebuffer))P("glBindFramebuffer");
    R.BindBuffer = (decltype(R.BindBuffer))P("glBindBuffer");
    R.GetFramebufferAttachmentParameteriv = (decltype(R.GetFramebufferAttachmentParameteriv))P("glGetFramebufferAttachmentParameteriv");
    R.GetTextureLevelParameteriv = (decltype(R.GetTextureLevelParameteriv))P("glGetTextureLevelParameteriv");
    R.GetTextureImage = (decltype(R.GetTextureImage))P("glGetTextureImage");
    R.GetNamedRenderbufferParameteriv = (decltype(R.GetNamedRenderbufferParameteriv))P("glGetNamedRenderbufferParameteriv");
    R.extLoaded = true;
    SessionLog("real GL 3+/4.5 entry points: BindFramebuffer=%p GetTextureImage=%p", R.BindFramebuffer, R.GetTextureImage);
    return true;
}

// ---------------------------------------------------------------- DllMain

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe = path;
    g_gameDir = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    wchar_t cfg[MAX_PATH] = L"";   // optional <game>\FortsGFX.ini: [FortsGFX] OutputDir=E:\FortsGFX
    GetPrivateProfileStringW(L"FortsGFX", L"OutputDir", L"", cfg, MAX_PATH, (g_gameDir + L"FortsGFX.ini").c_str());
    g_outDir = cfg[0] ? cfg : g_gameDir + L"FortsGFX";
    if (g_outDir.back() != L'\\' && g_outDir.back() != L'/') g_outDir += L'\\';
    CreateDirectoryW(g_outDir.c_str(), nullptr);
    g_log = _wfopen((g_outDir + L"session.log").c_str(), L"w");
    g_loadLog = _wfopen((g_outDir + L"loading.log").c_str(), L"w");

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring realPath = std::wstring(sys) + L"\\opengl32.dll";
    g_realDll = LoadLibraryW(realPath.c_str());
    SessionLog("%s (built %s %s) loaded into %s", kVersion, __DATE__, __TIME__, Narrow(exe.c_str()).c_str());
    SessionLog("this log is what happened; loading.log has every texture and script the game loads");
    if (!g_realDll) {
        SessionLog("FATAL: could not load %s (error %lu)", Narrow(realPath.c_str()).c_str(), GetLastError());
        return FALSE;
    }
    SessionLog("real opengl32: %s", Narrow(realPath.c_str()).c_str());

    size_t missing = 0;
    for (size_t i = 0; i < kEntryCount; i++) {
        g_entries[i].real = (void*)GetProcAddress(g_realDll, g_entries[i].name);
        if (!g_entries[i].real) { missing++; SessionLog("missing real export %s", g_entries[i].name); }
    }
#define RES(field, name) R.field = (decltype(R.field))GetProcAddress(g_realDll, name)
    RES(GetIntegerv, "glGetIntegerv"); RES(GetFloatv, "glGetFloatv"); RES(IsEnabled, "glIsEnabled");
    RES(GetString, "glGetString"); RES(ReadPixels, "glReadPixels"); RES(ReadBuffer, "glReadBuffer");
    RES(PixelStorei, "glPixelStorei"); RES(wglGetCurrentDC, "wglGetCurrentDC");
    RES(wglGetCurrentContext, "wglGetCurrentContext"); RES(wglGetProcAddress, "wglGetProcAddress");
#undef RES
    ResolveStaticHooks(g_realDll);
    SessionLog("%zu generic exports forwarded (%zu missing)", kEntryCount, missing);

    InstallIatHooks();
    SessionLog("capture: press Ctrl+Shift+F11 in game, or create %scapture.trigger", Narrow(g_outDir.c_str()).c_str());
    return TRUE;
}
