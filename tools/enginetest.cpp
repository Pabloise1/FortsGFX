// Offline test of the engine watcher (src/engine.cpp) without the game: the watch is pointed at stand-in
// functions with the game's argument layouts, which are called from the main thread and a thread started
// later. Checks that every call is caught with the right arguments, the spawn matching, and the cost.
//   cl /nologo /O2 /MT /EHsc /std:c++17 /I..\src enginetest.cpp && enginetest.exe
#include "../src/engine.cpp"
#include <cstdarg>
#include <thread>

std::wstring g_gameDir = L".\\";
std::string Fmt(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}
static int g_lines;
void SessionLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_lines++ < 60) { vprintf(fmt, ap); printf("\n"); }
    va_end(ap);
}

struct V3 { float x, y, z; };
static volatile int g_sink;
extern "C" __declspec(noinline) void* FakeCreateEffect(void* mgr, const char* file, int team, const V3* pos, void* a5, void* a6, char a7, float a8,
                                                        int a9, void* a10, char a11, int a12, void* a13) {
    g_sink += team + (int)pos->x + a9 + a12 + (int)a8 + (mgr ? 1 : 0) + (file[0]) + (a5 ? 1 : 0) + (a6 ? 1 : 0) + a7 + a11 + (a10 ? 1 : 0) + (a13 ? 1 : 0);
    return nullptr;
}
extern "C" __declspec(noinline) void FakeProjectileAction(int nodeId, const char* saveName, const V3& pos, const V3& normal, int hitType, int hitTeam,
                                                          const char* hitSaveName) {
    g_sink += nodeId + saveName[0] + (int)pos.x + (int)normal.y + hitType + hitTeam + hitSaveName[0];
}

int main() {
    g_fn[kCreateEffect] = (uintptr_t)&FakeCreateEffect;
    g_fn[kProjectileAction] = (uintptr_t)&FakeProjectileAction;
    g_veh = AddVectoredExceptionHandler(1, OnException);
    g_on = true;
    CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    Sleep(300);   // the worker sets the registers on this thread

    V3 p{ 1234.5f, -678.0f, 0 }, n{ 0, -1, 0 };
    const int kCalls = 5000;
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < kCalls; i++) FakeCreateEffect((void*)1, "data/effects/impact_heavy.lua", 2, &p, nullptr, nullptr, 0, 0.5f, 1, nullptr, 0, -1, nullptr);
    QueryPerformanceCounter(&b);
    double us = (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / kCalls;
    FakeProjectileAction(77, "cannon", p, n, 3, 1, "bracing");

    size_t pending;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        pending = g_pending.size();
    }
    printf("main thread: %zu of %d calls caught (%u dropped: queue full), %.2f us per caught call\n", pending, kCalls + 1, g_dropped.load(), us);
    EngineFrame();
    printf("spawns kept for matching: %zu\n", g_spawns.size());

    // a thread started after the watch: caught once the worker has been round (every 2 s)
    std::thread late([&] {
        Sleep(2500);
        for (int i = 0; i < 10; i++) FakeCreateEffect((void*)1, "data/effects/mortar_explode.lua", 1, &p, nullptr, nullptr, 0, 0, 1, nullptr, 0, -1, nullptr);
    });
    late.join();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        printf("late thread: %zu of 10 calls caught\n", g_pending.size());
    }
    EngineFrame();

    // matching: confirm after 3, then claim once per spawn
    for (int i = 0; i < 5; i++) {
        EngineMatch m = EngineEffectAt(1240, -670, 100);
        printf("match %d: state %d, dist %.1f, age %.2f, file %s\n", i, m.state, m.dist, m.age, m.file);
    }
    EngineMatch away = EngineEffectAt(9000, 9000, 100);
    printf("far away: state %d (want 0), nearest %.0f away\n", away.state, away.dist);
    printf("ready %d\n", EngineReady());

    // the effect list tracker, on a fake effect manager laid out like the game's: a std::list at +0xB8
    // (head, size; node = next, prev, effect*), effect position at +0x134, id at +0x14C
    struct Node { Node *next, *prev; uint8_t* eff; };
    static uint8_t mgr[0x200] = {};
    static uint8_t* mgrPtr = mgr;
    Node head{ &head, &head, nullptr };
    std::vector<Node*> nodes;
    std::vector<uint8_t*> effs;
    auto setPos = [](uint8_t* e, float x, float y) { memcpy(e + 0x134, &x, 4); memcpy(e + 0x138, &y, 4); };
    auto add = [&](int id, float x, float y) {
        uint8_t* e = new uint8_t[0x200]();
        setPos(e, x, y);
        memcpy(e + 0x14C, &id, 4);
        Node* n = new Node{ &head, head.prev, e };
        head.prev->next = n; head.prev = n;
        nodes.push_back(n); effs.push_back(e);
    };
    auto setHead = [&]() { Node* h = &head; uint64_t sz = nodes.size(); memcpy(mgr + 0xB8, &h, 8); memcpy(mgr + 0xC0, &sz, 8); };
    g_mgrGlobal = (uintptr_t)&mgrPtr;
    const char* names[6] = { "data/effects/cannon_trail.lua", "data/effects/impact_heavy.lua", "data/effects/mortar_trail.lua",
                             "data/effects/fire_cannon.lua", "data/effects/sniper_trail.lua", "data/effects/missile_trail.lua" };
    std::vector<Event> got;
    for (int i = 0; i < 6; i++) {
        add(100 + i, 500.0f * i, -200.0f);
        Event e{};
        e.kind = kCreateEffect; e.pos[0] = 500.0f * i; e.pos[1] = -200.0f;
        snprintf(e.name, sizeof(e.name), "%s", names[i]);
        got.push_back(e);
    }
    setHead();
    ULONGLONG ms = 1000;
    TrackEffects(got, ms);
    printf("tracker: on %d, %zu tracked, %zu trails out (want 4)\n", g_trackOn, g_tracked.size(), g_trailsOut.size());
    for (int f = 1; f <= 5; f++) {   // the cannon shell flies at 6000 units/s, the mortar at 2000
        Sleep(20);
        double dt = 0.02;
        float cx, mx;
        memcpy(&cx, effs[0] + 0x134, 4); memcpy(&mx, effs[2] + 0x134, 4);
        setPos(effs[0], cx + 6000.0f * (float)dt, -200.0f);
        setPos(effs[2], mx + 2000.0f * (float)dt, -200.0f);
        TrackEffects({}, ms + f * 20);
    }
    for (auto& t : g_trailsOut) printf("  trail %s id %u at (%.0f, %.0f) moving %.0f units/s\n", t.name, t.key, t.x, t.y, hypotf(t.vx, t.vy));
    head.next = nodes[1]; nodes[1]->prev = &head;   // the cannon shell's effect ends
    TrackEffects({}, ms + 200);
    printf("after one ended: %zu tracked (want 5)\n", g_tracked.size());
    return 0;
}
