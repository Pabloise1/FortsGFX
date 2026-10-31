// FortsGFX engine watcher: what the game itself does, instead of guesses from its sprites.
//
// Four of the game's own functions are watched with the CPU's debug registers: when the game enters
// one, FortsGFX reads the arguments and the game carries on. No game code or data is changed and
// nothing is sent anywhere, so it is read-only and client-side (multiplayer sync is untouched).
// [Engine] WatchEvents=0 turns it off.
//   CreateEffect               every effect the game spawns (impacts, explosions, muzzle flashes...):
//                              its file and world position. Found by its code (it has no name).
//   OnExecuteProjectileAction  a projectile acting where it hits: position, normal, what it hit
//   OnWeaponFired              a weapon firing a projectile
//   OnDeviceDestroyed          a device destroyed
// The last three are the C++ functions that tell scripts about these events; each is found from its
// name: the name's string -> the instruction that loads it -> the function around it (x64 unwind table).
//
// Effects use the spawns to tell a real blast from a fire's own explosion-like frames: a blast sprite
// appears where the game has just spawned an impact or explosion effect; a fire's frames never do.
#include "common.h"
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum EventKind { kCreateEffect, kProjectileAction, kWeaponFired, kDeviceDestroyed, kEventCount };
const char* const kEventNames[kEventCount] = { "CreateEffect", "OnExecuteProjectileAction", "OnWeaponFired", "OnDeviceDestroyed" };
const char* const kWhat[kEventCount] = { "effect", "projectile action", "weapon fired", "device destroyed" };

uintptr_t g_fn[kEventCount];             // the game's function for each event (0 = not found)
uintptr_t g_mgrGlobal;                   // where the game keeps its effect manager pointer (from CreateEffect's callers)
std::atomic<bool> g_on{ false };
PVOID g_veh;

struct Event { int kind; int a, b, c, d; float pos[3], normal[3], t; ULONGLONG ms; char name[96], other[48]; };
std::mutex g_mx;
std::vector<Event> g_pending;            // filled on the game's threads, taken in on the render thread
std::atomic<unsigned> g_dropped{ 0 };

// ---- safe reads from game memory (the arguments are the game's own, but never trust a pointer)
bool SafeCopy(void* dst, const void* src, size_t n) {
    if (!src) return false;
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void SafeString(char* dst, size_t cap, const char* src) {
    dst[0] = 0;
    if (!src) return;
    for (size_t i = 0; i + 1 < cap; i++) {
        char ch;
        if (!SafeCopy(&ch, src + i, 1) || !ch) { dst[i] = 0; return; }
        dst[i] = (ch >= 32 && ch < 127) ? ch : '?';
        dst[i + 1] = 0;
    }
}
template <class T> T StackArg(const CONTEXT* c, int index) {   // at function entry; index 4 = the 5th argument
    T v{};
    SafeCopy(&v, (const void*)(c->Rsp + 8 + 8 * index), sizeof(T));
    return v;
}

// Argument layouts, read from the game's code (each matches the scripting documentation).
void Record(int kind, const CONTEXT* c) {
    Event e{};
    e.kind = kind;
    e.ms = GetTickCount64();
    switch (kind) {
    case kCreateEffect:       // (manager, const char* file, int team, const Vector3D* pos, const Vector3D* dir or null, ...)
        SafeString(e.name, sizeof(e.name), (const char*)c->Rdx);
        e.a = (int)c->R8;
        SafeCopy(e.pos, (const void*)c->R9, sizeof(e.pos));
        e.b = SafeCopy(e.normal, StackArg<const float*>(c, 4), sizeof(e.normal)) ? 1 : 0;
        break;
    case kProjectileAction:   // (int nodeId, const char* saveName, const Vector3D& pos, const Vector3D& normal, HitType, int hitTeamId, const char* hitSaveName)
        e.a = (int)c->Rcx;
        SafeString(e.name, sizeof(e.name), (const char*)c->Rdx);
        SafeCopy(e.pos, (const void*)c->R8, sizeof(e.pos));
        SafeCopy(e.normal, (const void*)c->R9, sizeof(e.normal));
        e.b = StackArg<int>(c, 4);
        e.c = StackArg<int>(c, 5);
        SafeString(e.other, sizeof(e.other), StackArg<const char*>(c, 6));
        break;
    case kWeaponFired:        // (int teamId, const char* saveName, int weaponId, int projectileNodeId, int projectileNodeIdFrom)
        e.a = (int)c->Rcx;
        SafeString(e.name, sizeof(e.name), (const char*)c->Rdx);
        e.b = (int)c->R8;
        e.c = (int)c->R9;
        e.d = StackArg<int>(c, 4);
        break;
    case kDeviceDestroyed:    // (int teamId, int deviceId, const char* saveName, int nodeA, int nodeB, float t)
        e.a = (int)c->Rcx;
        e.b = (int)c->Rdx;
        SafeString(e.name, sizeof(e.name), (const char*)c->R8);
        e.c = (int)c->R9;
        e.d = StackArg<int>(c, 4);
        e.t = StackArg<float>(c, 5);
        break;
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_pending.size() < 8192) g_pending.push_back(e);
    else g_dropped++;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_on) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    for (int i = 0; i < kEventCount; i++) {
        if (g_fn[i] && at == g_fn[i]) {
            Record(i, ep->ContextRecord);
            ep->ContextRecord->EFlags |= 0x10000;   // RF: run this instruction without stopping on it again
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- finding the functions
struct Image { const uint8_t* base; const uint8_t *code, *codeEnd; std::vector<std::pair<const uint8_t*, size_t>> data; };
Image ReadImage() {
    Image im{ (const uint8_t*)GetModuleHandleW(nullptr), nullptr, nullptr, {} };
    auto nt = (const IMAGE_NT_HEADERS64*)(im.base + ((const IMAGE_DOS_HEADER*)im.base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const uint8_t* s = im.base + sec[i].VirtualAddress;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) { if (!im.code) { im.code = s; im.codeEnd = s + sec[i].Misc.VirtualSize; } }
        else im.data.push_back({ s, sec[i].Misc.VirtualSize });
    }
    return im;
}

uintptr_t FunctionAround(const uint8_t* ins) {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)ins, &imageBase, nullptr);
    for (int guard = 0; rf && guard < 8; guard++) {   // a chained fragment: go to the function it belongs to
        const uint8_t* ui = (const uint8_t*)(imageBase + rf->UnwindData);
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
    }
    return rf ? (uintptr_t)(imageBase + rf->BeginAddress) : 0;
}

// An event's script dispatcher: "lea rcx,[name]; call HasScriptFunction" near its start.
uintptr_t FindEventFunction(const Image& im, const char* name) {
    size_t len = strlen(name) + 1;
    const uint8_t* str = nullptr;
    for (auto& d : im.data)
        for (size_t k = 1; !str && k + len <= d.second; k++)
            if (d.first[k] == (uint8_t)name[0] && d.first[k - 1] == 0 && memcmp(d.first + k, name, len) == 0) str = d.first + k;
    if (!str || !im.code) return 0;
    for (const uint8_t* p = im.code; p + 12 <= im.codeEnd; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D || p[7] != 0xE8) continue;   // lea rcx,[rip+disp32]; call
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        if (p + 7 + disp != str) continue;
        uintptr_t fn = FunctionAround(p);
        if (fn && (uintptr_t)p - fn < 0x80) return fn;
    }
    return 0;
}

// CreateEffect has no name; it is found by its first instructions (one match in the game), and checked
// by how many places call it (about 200).
uintptr_t FindCreateEffect(const Image& im, int* callers) {
    static const int16_t sig[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF9,
        0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00, 0x0F, 0x29, 0xB4, 0x24, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, -1, -1, -1, -1,
        0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0xE7, 0x4D, 0x8B, 0xE9, 0x44, 0x89, 0x44, 0x24, 0x60, 0x48, 0x8B, 0xDA };
    const size_t n = sizeof(sig) / sizeof(sig[0]);
    const uint8_t* hit = nullptr;
    int hits = 0;
    for (const uint8_t* p = im.code; p + n <= im.codeEnd; p++) {
        if (*p != 0x40 || p[1] != 0x55) continue;
        size_t k = 2;
        while (k < n && (sig[k] < 0 || p[k] == (uint8_t)sig[k])) k++;
        if (k == n) { hit = p; hits++; }
    }
    *callers = 0;
    if (hits != 1) return 0;
    std::map<uintptr_t, int> globals;
    for (const uint8_t* p = im.code; p + 5 <= im.codeEnd; p++) {
        if (*p != 0xE8) continue;
        int32_t disp;
        memcpy(&disp, p + 1, 4);
        if (p + 5 + disp != hit) continue;
        ++*callers;
        for (const uint8_t* q = p - 7; q >= p - 64 && q > im.code; q--)   // mov r64, [rip+disp32] shortly before the call
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC7) == 0x05) {
                int32_t d2;
                memcpy(&d2, q + 3, 4);
                globals[(uintptr_t)(q + 7 + d2)]++;
                break;
            }
    }
    int best = 0;
    for (auto& kv : globals) if (kv.second > best) { best = kv.second; g_mgrGlobal = kv.first; }
    if (best < 20) g_mgrGlobal = 0;
    return *callers >= 50 ? (uintptr_t)hit : 0;
}

// ---- the debug registers, set on every thread of the game (from our own thread, so the main one too)
std::set<DWORD> g_doneThreads;
void ApplyToThreads(bool all) {
    if (all) g_doneThreads.clear();
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    std::vector<DWORD> ids;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me && !g_doneThreads.count(te.th32ThreadID)) ids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    for (DWORD id : ids) {
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, id);
        if (!t) continue;
        bool set = false;   // nothing between suspend and resume may allocate (the thread may hold the heap lock)
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                DWORD64* dr[4] = { &ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3 };
                bool on = g_on;
                for (int i = 0; i < kEventCount; i++) {
                    *dr[i] = on ? g_fn[i] : 0;
                    ctx.Dr7 &= ~((3ull << (i * 2)) | (0xFull << (16 + i * 4)));   // slot i: break on execute, 1 byte
                    if (on && g_fn[i]) ctx.Dr7 |= 1ull << (i * 2);                   // local enable
                }
                set = SetThreadContext(t, &ctx) != 0;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        if (set) g_doneThreads.insert(id);
    }
}

DWORD WINAPI Worker(void*) {
    for (int n = 0; g_on; n++) {   // new threads get the registers too; every 30 s all of them again
        ApplyToThreads(n % 15 == 0);
        Sleep(2000);
    }
    return 0;
}

// ---- this frame's events for the effects, and what the game spawned recently for matching with sprites
// (render thread only)
std::vector<EngineFx> g_fxOut;
bool g_fxTaken = true;

// The game's live effects: its effect manager keeps them in a std::list (manager + 0xB8: head, size; a node
// is next, prev, effect*), an effect's world position is at +0x134 and its id at +0x14C (read from
// CreateEffect and the position setter it calls). Each new effect is named by the CreateEffect call that
// made it, at the same spot - which also checks the layout: tracking only starts once 5 line up exactly.
// A projectile carries its trail effect (projectile_list.lua TrailEffect), so the trails are the
// projectiles in flight, with their kind.
const uintptr_t kMgrList = 0xB8, kEffPos = 0x134, kEffId = 0x14C;
struct Tracked { std::string name; int id; float x, y, vx, vy; double tMove; ULONGLONG seen; };
std::unordered_map<uintptr_t, Tracked> g_tracked;
int g_trackMatches, g_trackNew;
bool g_trackOn, g_trackOff;
std::vector<EngineTrail> g_trailsOut;
std::set<std::string> g_trailLogged;

double NowSeconds() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

std::string Short(const char* s);

void TrackEffects(const std::vector<Event>& got, ULONGLONG nowMs) {
    if (!g_mgrGlobal || g_trackOff) return;
    uintptr_t mgr = 0, head = 0;
    uint64_t size = 0;
    if (!SafeCopy(&mgr, (const void*)g_mgrGlobal, 8) || !mgr) return;
    if (!SafeCopy(&head, (const void*)(mgr + kMgrList), 8) || !SafeCopy(&size, (const void*)(mgr + kMgrList + 8), 8) || !head || size > 100000) return;
    std::vector<const Event*> spawns;   // this frame's CreateEffect calls, to name the new ones
    for (auto& e : got) if (e.kind == kCreateEffect) spawns.push_back(&e);
    std::vector<bool> used(spawns.size());
    double t = NowSeconds();
    uintptr_t node = 0;
    SafeCopy(&node, (const void*)head, 8);
    for (int guard = 0; node && node != head && guard < 4096; guard++) {
        uintptr_t eff = 0, next = 0;
        float pos[2];
        int id = 0;
        if (!SafeCopy(&eff, (const void*)(node + 0x10), 8) || !SafeCopy(&next, (const void*)node, 8)) break;
        if (eff && SafeCopy(pos, (const void*)(eff + kEffPos), 8) && SafeCopy(&id, (const void*)(eff + kEffId), 4)) {
            auto it = g_tracked.find(eff);
            if (it == g_tracked.end() || it->second.id != id) {   // new (or a new effect at a reused address)
                Tracked tr{};
                tr.id = id; tr.x = pos[0]; tr.y = pos[1]; tr.tMove = t;
                float bd = 60.0f;
                int bi = -1;
                for (size_t i = 0; i < spawns.size(); i++) {
                    if (used[i]) continue;
                    float d = hypotf(spawns[i]->pos[0] - pos[0], spawns[i]->pos[1] - pos[1]);
                    if (d < bd) { bd = d; bi = (int)i; }
                }
                if (bi >= 0) {
                    used[bi] = true;
                    tr.name = Short(spawns[bi]->name);
                    if (bd < 1.0f) g_trackMatches++;
                }
                g_trackNew++;
                it = g_tracked.insert_or_assign(eff, tr).first;
            } else if (pos[0] != it->second.x || pos[1] != it->second.y) {   // moved (the game updates it on its ticks)
                Tracked& tr = it->second;
                float dt = (float)std::max(t - tr.tMove, 1e-3);
                tr.vx = (pos[0] - tr.x) / dt; tr.vy = (pos[1] - tr.y) / dt;
                tr.x = pos[0]; tr.y = pos[1]; tr.tMove = t;
            }
            it->second.seen = nowMs;
        }
        node = next;
    }
    for (auto it = g_tracked.begin(); it != g_tracked.end();)
        it = it->second.seen != nowMs ? g_tracked.erase(it) : std::next(it);
    if (!g_trackOn && g_trackMatches >= 5) {
        g_trackOn = true;
        SessionLog("engine: the game's effect list lines up (%d of %d new effects matched exactly) - tracking projectiles in flight by their trails",
                   g_trackMatches, g_trackNew);
    }
    if (!g_trackOn && g_trackNew >= 300) {
        g_trackOff = true;
        SessionLog("engine: the game's effect list doesn't line up (%d of %d new effects matched) - not tracking it", g_trackMatches, g_trackNew);
    }
    g_trailsOut.clear();
    if (!g_trackOn) return;
    for (auto& kv : g_tracked) {
        const Tracked& tr = kv.second;
        if (tr.name.find("trail") == std::string::npos || g_trailsOut.size() >= 256) continue;
        EngineTrail o;
        o.key = (uint32_t)tr.id; o.x = tr.x; o.y = tr.y; o.vx = tr.vx; o.vy = tr.vy;
        snprintf(o.name, sizeof(o.name), "%s", tr.name.c_str());
        g_trailsOut.push_back(o);
        float sp = hypotf(tr.vx, tr.vy);
        if (sp > 1.0f && g_trailLogged.size() < 12 && g_trailLogged.insert(tr.name).second)
            SessionLog("engine: projectile in flight: %s at (%.0f, %.0f) moving %.0f units/s", tr.name.c_str(), tr.x, tr.y, sp);
    }
}
struct Spawn { float x, y; ULONGLONG ms; bool blastLike, claimed; std::string file; };
std::vector<Spawn> g_spawns;
int g_matches;                           // sprites that lined up with a spawn (positions confirmed after 3)
const int kConfirm = 3;

std::string Lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
// An effect that can make a blast: an impact or explosion (not muzzle flashes, trails, bullet hits, UI...).
bool BlastLike(const std::string& file) {
    std::string f = Lower(file);
    if (f.find("bullet") != std::string::npos || f.find("ricochet") != std::string::npos) return false;
    for (const char* k : { "explo", "impact", "hit", "destroy", "burst", "blast", "detonat", "nuke", "mushroom", "death", "destruct", "break" })
        if (f.find(k) != std::string::npos) return true;
    return false;
}
std::string Short(const char* s) {   // "data/effects/impact_heavy.lua" -> "effects/impact_heavy.lua"
    std::string f = s[0] ? s : "?";
    for (auto& ch : f) if (ch == '\\') ch = '/';
    size_t d = Lower(f).find("data/");
    return d != std::string::npos ? f.substr(d + 5) : f;
}

void LogEvent(const Event& e) {
    switch (e.kind) {
    case kCreateEffect:
        SessionLog("engine: effect %s spawned at (%.0f, %.0f, %.0f), team %d%s", Short(e.name).c_str(), e.pos[0], e.pos[1], e.pos[2], e.a,
                   e.b ? Fmt(", direction (%.2f, %.2f, %.2f)", e.normal[0], e.normal[1], e.normal[2]).c_str() : "");
        break;
    case kProjectileAction:
        SessionLog("engine: projectile %s %d acts at (%.0f, %.0f) normal (%.2f, %.2f), hit type %d, on team %d \"%s\"", e.name[0] ? e.name : "?", e.a,
                   e.pos[0], e.pos[1], e.normal[0], e.normal[1], e.b, e.c, e.other);
        break;
    case kWeaponFired:
        SessionLog("engine: weapon %s (id %d, team %d) fired projectile %d (from %d)", e.name[0] ? e.name : "?", e.b, e.a, e.c, e.d);
        break;
    case kDeviceDestroyed:
        SessionLog("engine: device %s %d (team %d) destroyed on nodes %d-%d at %.2f", e.name[0] ? e.name : "?", e.b, e.a, e.c, e.d, e.t);
        break;
    }
}

}   // namespace

// First frame: find the functions and start watching.
void EngineInit() {
    if (g_veh || !GetPrivateProfileIntW(L"Engine", L"WatchEvents", 1, (g_gameDir + L"FortsGFX.ini").c_str())) {
        if (!g_veh) SessionLog("engine: watching the game's events is off ([Engine] WatchEvents=0)");
        return;
    }
    Image im = ReadImage();
    if (!im.code) return;
    int callers = 0, found = 0;
    g_fn[kCreateEffect] = FindCreateEffect(im, &callers);
    for (int i = kProjectileAction; i < kEventCount; i++) g_fn[i] = FindEventFunction(im, kEventNames[i]);
    std::string where;
    for (int i = 0; i < kEventCount; i++) {
        if (g_fn[i]) found++;
        where += Fmt("%s%s %s", i ? ", " : "", kEventNames[i],
                     g_fn[i] ? Fmt("+%#llx", (unsigned long long)(g_fn[i] - (uintptr_t)im.base)).c_str() : "not found");
    }
    SessionLog("engine: watching the game's own functions (read-only): %s; CreateEffect has %d callers; effect manager pointer %s", where.c_str(), callers,
               g_mgrGlobal ? Fmt("at +%#llx", (unsigned long long)(g_mgrGlobal - (uintptr_t)im.base)).c_str() : "not found");
    if (!found) return;
    g_veh = AddVectoredExceptionHandler(1, OnException);
    if (!g_veh) { SessionLog("engine: could not add the exception handler - off"); return; }
    g_on = true;
    CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
}

// Once a frame (render thread): take in what the game did. The log gets the first events of each
// kind in full, and every 20 s a summary.
void EngineFrame() {
    if (!g_on) return;
    std::vector<Event> got;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        got.swap(g_pending);
    }
    static int detailed[kEventCount];
    static std::map<std::string, int> counts;
    static ULONGLONG t0 = GetTickCount64();
    ULONGLONG now = GetTickCount64();
    if (!g_fxTaken) g_fxOut.clear();   // nobody took the last frame's (not in a battle): don't let them pile up
    g_fxTaken = false;
    TrackEffects(got, now);
    for (auto& e : got) {
        if (g_fxOut.size() < 2048) {
            EngineFx f{};
            f.kind = (uint8_t)(e.kind == kCreateEffect ? EngineFx::Effect : e.kind == kProjectileAction ? EngineFx::Impact
                               : e.kind == kWeaponFired ? EngineFx::Fired : EngineFx::Device);
            f.x = e.pos[0]; f.y = e.pos[1];
            if (e.kind == kProjectileAction || (e.kind == kCreateEffect && e.b)) { f.dx = e.normal[0]; f.dy = e.normal[1]; f.hasDir = true; }
            f.team = e.kind == kProjectileAction ? e.c : e.a;
            f.a = e.kind == kProjectileAction ? e.b : e.kind == kDeviceDestroyed ? e.b : e.c;
            snprintf(f.name, sizeof(f.name), "%s", e.kind == kCreateEffect ? Short(e.name).c_str() : e.name);
            snprintf(f.hit, sizeof(f.hit), "%s", e.other);
            g_fxOut.push_back(f);
        }
        if (detailed[e.kind] < (e.kind == kCreateEffect ? 30 : 12)) { detailed[e.kind]++; LogEvent(e); }
        std::string name = e.kind == kCreateEffect ? Short(e.name) : std::string(e.name[0] ? e.name : "?");
        if (counts.size() < 300) counts[std::string(kWhat[e.kind]) + " " + name]++;
        if (e.kind == kCreateEffect)
            g_spawns.push_back({ e.pos[0], e.pos[1], e.ms, BlastLike(name), false, name });
    }
    g_spawns.erase(std::remove_if(g_spawns.begin(), g_spawns.end(), [&](const Spawn& s) { return now - s.ms > 3000; }), g_spawns.end());
    if (g_spawns.size() > 1024) g_spawns.erase(g_spawns.begin(), g_spawns.end() - 1024);
    if (now - t0 < 20000) return;
    if (!counts.empty()) {
        std::vector<std::pair<int, std::string>> rows;
        for (auto& kv : counts) rows.push_back({ kv.second, kv.first });
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        SessionLog("engine, last %.0f s: what the game did%s", (now - t0) / 1000.0, g_dropped ? Fmt(" (%u not recorded: too many)", g_dropped.load()).c_str() : "");
        int shown = 0;
        for (auto& r : rows) {
            if (shown++ == 20) { SessionLog("    ... %zu more kinds", rows.size() - 20); break; }
            SessionLog("    %5d x %s", r.first, r.second.c_str());
        }
    }
    counts.clear();
    g_dropped = 0;
    t0 = now;
}

bool EngineReady() { return g_on && g_matches >= kConfirm; }
bool EngineWatching() { return g_on && g_fn[kCreateEffect]; }
bool EngineTracking() { return g_on && g_trackOn; }
void EngineTrails(std::vector<EngineTrail>& out) { out = g_trailsOut; }

void EngineTakeFx(std::vector<EngineFx>& out) {
    out.swap(g_fxOut);
    g_fxOut.clear();
    g_fxTaken = true;
}

// The nearest recent impact/explosion the game spawned around (x, y). A blast sprite claims the one it
// came from, so one spawn makes one blast.
EngineMatch EngineEffectAt(float x, float y, float radius) {
    EngineMatch m;
    if (!g_on) return m;
    const ULONGLONG kFresh = 800;   // ms: a blast sprite shows within a few frames of its effect
    ULONGLONG now = GetTickCount64();
    Spawn* best = nullptr;
    float bestD = 1e30f;
    for (int pass = 0; pass < 2 && !best; pass++)   // the nearest fresh one; else the nearest at all (for the log)
        for (auto& s : g_spawns) {
            if (!s.blastLike || s.claimed || (pass == 0 && now - s.ms > kFresh)) continue;
            float d = hypotf(s.x - x, s.y - y);
            if (d < bestD) { bestD = d; best = &s; }
        }
    if (best) {
        m.dist = bestD;
        m.age = (now - best->ms) / 1000.0f;
        snprintf(m.file, sizeof(m.file), "%s", best->file.c_str());
    }
    bool here = best && bestD <= radius && now - best->ms <= kFresh;
    if (here) {
        best->claimed = true;
        if (g_matches < kConfirm && ++g_matches == kConfirm)
            SessionLog("engine: the game's effect positions line up with its sprites - blasts now follow the game's own impacts and explosions");
    }
    m.state = !EngineReady() ? -1 : here ? 1 : 0;
    return m;
}
