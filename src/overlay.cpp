// In-game control panel and lab (Dear ImGui). The overlay key (Insert by default) opens the panel.
// While it is open it draws its own cursor and keeps the input it wants away from the game, both
// window messages and DirectInput (Forts reads devices through DINPUT8). The lab adds click tools
// (spawn effects and lights in the battle), a before/after wipe, looks and a captioned tour.
#include "common.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern "C" {
typedef void (*GL3WglProc)(void);
int imgl3wInit2(GL3WglProc (*proc)(const char*));
}

static HWND g_wnd;
static WNDPROC g_prevProc;
static bool g_init, g_open, g_failed;
static float g_scale = 1.0f;
static const int kHist = 240;
static float g_frameHist[kHist], g_gpuHist[kHist];
static int g_histPos;

// lab state
enum { kToolNone = -1, kToolLight = 7, kToolPlaceLight = 8, kToolSnow = 9, kToolCount = 10 };
static const char* kToolNames[kToolCount] = { "Explosion", "Mega blast", "Lightning", "Shockwave", "Flash", "Heat",
                                              "Sparks", "Light (hold)", "Place light", "Snow brush" };
static bool g_labOpen, g_wipeOn, g_sweep, g_cycle, g_tour, g_photo, g_photoGrid = true;
static GfxLab g_photoSaved;
static void PhotoEnter();
static void PhotoExit();
static double g_focusT = -100;
static int g_tool = kToolNone, g_pinned;
static float g_toolSize = 0.06f, g_lightSize = 0.05f, g_lightInten = 4.0f, g_lightCol[3] = { 1.0f, 0.72f, 0.42f };
static float g_cycleSecs = 4.0f;
static double g_cycleT, g_toastT = -100;
static char g_toast[192], g_newLook[64] = "My look";

static double NowS() {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER n;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - t0.QuadPart) / f.QuadPart;
}

static void Toast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    g_toastT = NowS();
}

static void* SettingPtr(const char* sec, const char* key) {
    for (auto& s : GfxSettings()) if (!strcmp(s.section, sec) && !strcmp(s.key, key)) return s.ptr;
    return nullptr;
}

// ---------------------------------------------------------------- keys
// The hotkeys, rebindable in the panel's Keys section: click one, then press the new key (Esc cancels; a key
// another hotkey has swaps with it). The new key goes into the ini at once and does nothing until it is let go.
struct HotKey { const char* section; const char* key; const char* label; int def; };
static const HotKey kHotKeys[] = {
    { "Post", "OverlayKeyVK", "Control panel", VK_INSERT },
    { "Post", "LabKeyVK", "Lab bar", VK_HOME },
    { "Post", "PhotoKeyVK", "Photo mode", VK_END },
    { "Post", "LookPrevKeyVK", "Previous look", VK_F7 },
    { "Post", "LookNextKeyVK", "Next look", VK_F8 },
    { "Post", "ToggleKeyVK", "FortsGFX on / off (with Ctrl: reload the ini)", VK_F9 },
    { "Lighting", "ToggleKeyVK", "Lighting on / off", VK_F10 },
};
enum { kKeyPanel, kKeyLab, kKeyPhoto, kKeyLookPrev, kKeyLookNext, kKeyToggle, kKeyLighting, kHotKeyCount };
static int g_bindSlot = -1, g_swallowVk;   // the hotkey waiting for a key; the key just bound, until let go
static bool g_bindHeld[256];

static int* HotKeyPtr(int i) { return (int*)SettingPtr(kHotKeys[i].section, kHotKeys[i].key); }

static const char* KeyName(int vk) {
    static char buf[8][32];
    static int n;
    char* out = buf[n++ & 7];
    // Extended keys (GetKeyNameText would name their numpad twins) and the ones it spells in capitals
    switch (vk) {
    case VK_INSERT: return "Insert";    case VK_DELETE: return "Delete";      case VK_HOME: return "Home";
    case VK_END: return "End";          case VK_PRIOR: return "Page Up";      case VK_NEXT: return "Page Down";
    case VK_LEFT: return "Left";        case VK_RIGHT: return "Right";        case VK_UP: return "Up";
    case VK_DOWN: return "Down";        case VK_PAUSE: return "Pause";        case VK_NUMLOCK: return "Num Lock";
    case VK_SNAPSHOT: return "Print Screen"; case VK_APPS: return "Menu";     case VK_SCROLL: return "Scroll Lock";
    case VK_DIVIDE: return "Num /";     case VK_MULTIPLY: return "Num *";     case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";   case VK_DECIMAL: return "Num .";      case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";          case VK_CAPITAL: return "Caps Lock";  case VK_BACK: return "Backspace";
    case VK_RETURN: return "Enter";     case VK_OEM_102: return "\\ (by Shift)";
    }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, 32, "F%d", vk - VK_F1 + 1); return out; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, 32, "Num %d", vk - VK_NUMPAD0); return out; }
    wchar_t w[32];   // letters, digits and the punctuation keys of the user's own layout
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), w, 32) > 0) snprintf(out, 32, "%s", Narrow(w).c_str());
    else snprintf(out, 32, "key %d", vk);
    return out;
}
static const char* HotKeyName(int i) { int* p = HotKeyPtr(i); return KeyName(p ? *p : kHotKeys[i].def); }

static void WriteHotKey(int i) {
    WritePrivateProfileStringW(Widen(kHotKeys[i].section).c_str(), Widen(kHotKeys[i].key).c_str(),
                               std::to_wstring(*HotKeyPtr(i)).c_str(), (g_gameDir + L"FortsGFX.ini").c_str());
}

static void BindStart(int i) {
    g_bindSlot = i;
    for (int vk = 0; vk < 256; vk++) g_bindHeld[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Waiting for a key: the first one that goes down becomes the hotkey - not the mouse buttons, Shift, Ctrl,
// Alt or Windows.
static void BindPoll(bool focused) {
    for (int vk = 8; vk < 255; vk++) {
        bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0, edge = down && !g_bindHeld[vk];
        g_bindHeld[vk] = down;
        bool modifier = vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) ||
                        vk == VK_LWIN || vk == VK_RWIN;
        if (!edge || modifier) continue;
        int slot = g_bindSlot, *p = HotKeyPtr(slot);
        g_bindSlot = -1;
        g_swallowVk = vk;
        if (vk == VK_ESCAPE || !p || *p == vk) return;
        int old = *p, other = -1;
        for (int j = 0; j < kHotKeyCount; j++)
            if (j != slot && HotKeyPtr(j) && *HotKeyPtr(j) == vk) { other = j; *HotKeyPtr(j) = old; WriteHotKey(j); }
        *p = vk;
        WriteHotKey(slot);
        if (other >= 0) Toast("%s: %s   (%s is now %s)", kHotKeys[slot].label, KeyName(vk), kHotKeys[other].label, KeyName(old));
        else Toast("%s: %s", kHotKeys[slot].label, KeyName(vk));
        SessionLog("overlay: %s key set to %s (%d)%s", kHotKeys[slot].label, KeyName(vk), vk, other >= 0 ? ", swapped" : "");
        return;
    }
}

bool OverlayKeyBlocked(int vk) { return g_bindSlot >= 0 || (vk && vk == g_swallowVk); }

// The mouse is held back from the game while it is over our UI, a lab tool is armed or photo mode is on;
// the keyboard only while you are typing into one of our text fields or binding a hotkey (camera keys
// keep working).
static bool WantsMouse() {
    if (!g_init || !(g_open || g_labOpen || g_photo)) return false;
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse || g_tool != kToolNone || g_photo;
}
static bool WantsKeys() {
    if (!g_init) return false;
    if (g_bindSlot >= 0 || g_swallowVk) return true;
    if (!(g_open || g_labOpen || g_photo)) return false;
    return ImGui::GetIO().WantTextInput;
}

// ---------------------------------------------------------------- input

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_init && (g_open || g_labOpen || g_photo)) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        ImGuiIO& io = ImGui::GetIO();
        bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        bool key = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR;
        bool toolMouse = (g_tool != kToolNone || g_photo) && msg != WM_MOUSEWHEEL;   // a lab tool / photo mode owns the clicks, zoom still works
        if ((mouse && (io.WantCaptureMouse || toolMouse)) || (key && WantsKeys())) {
            static bool logged;
            if (!logged && msg == WM_LBUTTONDOWN) { logged = true; SessionLog("overlay: a click on the panel was kept from the game"); }
            return 0;
        }
    }
    return CallWindowProcW(g_prevProc, h, msg, wp, lp);
}

// DirectInput: vtable hooks on the device interface. GetDeviceState is slot 9 and GetDeviceData
// slot 10 of IDirectInputDevice8; CreateDevice is slot 3 of IDirectInput8.
typedef HRESULT (WINAPI* GetStateFn)(void*, DWORD, void*);
typedef HRESULT (WINAPI* GetDataFn)(void*, DWORD, void*, DWORD*, DWORD);
typedef HRESULT (WINAPI* CreateDeviceFn)(void*, const GUID&, void**, void*);
static CreateDeviceFn r_CreateDevice;
DI8CreateFn r_DirectInput8Create;

// Keyboard and mouse can be different classes with their own vtables and their own GetDeviceState /
// GetDeviceData. Every patched vtable keeps its own originals, and each call goes back to the original
// of the vtable of the device it was made on - calling the mouse's code on the keyboard object broke
// every key in the game.
struct DevVtbl { void** vtbl; GetStateFn state; GetDataFn data; };
static DevVtbl g_devVtbl[8];
static int g_devVtblCount;
static std::mutex g_devMx;
static const DevVtbl* VtblOf(void* self) {
    void** v = *(void***)self;
    for (int i = 0; i < g_devVtblCount; i++) if (g_devVtbl[i].vtbl == v) return &g_devVtbl[i];
    return nullptr;
}

// Which DirectInput device is which: the hook looks at `self`.
static void* g_diKeyboard[4];
static void* g_diMouse[4];
static bool IsDev(void* const* list, void* d) { for (int i = 0; i < 4; i++) if (list[i] == d) return true; return false; }
static void AddDev(void** list, void* d) { for (int i = 0; i < 4; i++) if (!list[i] || list[i] == d) { list[i] = d; return; } }
static bool BlockDevice(void* self) {
    if (IsDev(g_diMouse, self)) return WantsMouse();
    if (IsDev(g_diKeyboard, self)) return WantsKeys();
    return false;   // gamepads and anything else always reach the game
}

static HRESULT WINAPI hk_GetDeviceState(void* self, DWORD size, void* data) {
    const DevVtbl* v = VtblOf(self);
    if (!v || !v->state) return E_FAIL;
    HRESULT hr = v->state(self, size, data);
    if (SUCCEEDED(hr) && data && BlockDevice(self)) memset(data, 0, size);
    return hr;
}
static HRESULT WINAPI hk_GetDeviceData(void* self, DWORD objSize, void* rgdod, DWORD* inOut, DWORD flags) {
    const DevVtbl* v = VtblOf(self);
    if (!v || !v->data) return E_FAIL;
    HRESULT hr = v->data(self, objSize, rgdod, inOut, flags);
    if (SUCCEEDED(hr) && inOut && BlockDevice(self)) *inOut = 0;
    return hr;
}

static bool PatchSlot(void** vtbl, int slot, void* hook) {
    if (vtbl[slot] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}

static HRESULT WINAPI hk_CreateDevice(void* self, const GUID& guid, void** dev, void* outer) {
    HRESULT hr = r_CreateDevice(self, guid, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev) {
        const char* kind = guid.Data1 == 0x6F1D2B61 ? "keyboard" : guid.Data1 == 0x6F1D2B60 ? "mouse" : "other";
        if (guid.Data1 == 0x6F1D2B61) AddDev(g_diKeyboard, *dev);        // GUID_SysKeyboard
        else if (guid.Data1 == 0x6F1D2B60) AddDev(g_diMouse, *dev);      // GUID_SysMouse
        void** vtbl = *(void***)*dev;
        std::lock_guard<std::mutex> lk(g_devMx);
        bool known = false;
        for (int i = 0; i < g_devVtblCount; i++) if (g_devVtbl[i].vtbl == vtbl) known = true;
        if (!known && g_devVtblCount < 8 && vtbl[9] != (void*)hk_GetDeviceState && vtbl[10] != (void*)hk_GetDeviceData) {
            DevVtbl& d = g_devVtbl[g_devVtblCount];
            d.vtbl = vtbl; d.state = (GetStateFn)vtbl[9]; d.data = (GetDataFn)vtbl[10];
            g_devVtblCount++;                         // published before the slots point at the hooks
            bool ok = PatchSlot(vtbl, 9, (void*)hk_GetDeviceState) && PatchSlot(vtbl, 10, (void*)hk_GetDeviceData);
            SessionLog("overlay: DirectInput %s device: vtable %p (GetDeviceState %p, GetDeviceData %p)%s", kind, (void*)vtbl, (void*)d.state, (void*)d.data, ok ? "" : " - patch failed");
        } else {
            SessionLog("overlay: DirectInput %s device: vtable %p (already hooked)", kind, (void*)vtbl);
        }
    }
    return hr;
}

extern "C" HRESULT WINAPI hk_DirectInput8Create(HINSTANCE inst, DWORD ver, const IID& iid, void** out, void* outer) {
    HRESULT hr = r_DirectInput8Create(inst, ver, iid, out, outer);
    // [Overlay] HookInput=0: never touch the game's input (the panel then can't hold clicks back)
    bool hook = GetPrivateProfileIntW(L"Overlay", L"HookInput", 1, (g_gameDir + L"FortsGFX.ini").c_str()) != 0;
    if (SUCCEEDED(hr) && out && *out && hook) {
        void** vtbl = *(void***)*out;
        if (vtbl[3] != (void*)hk_CreateDevice) {
            if (!r_CreateDevice) r_CreateDevice = (CreateDeviceFn)vtbl[3];
            PatchSlot(vtbl, 3, (void*)hk_CreateDevice);
        }
        SessionLog("overlay: DirectInput hooked (game input is held back while the panel uses it)");
    } else if (!hook) {
        SessionLog("overlay: DirectInput left alone ([Overlay] HookInput=0)");
    }
    return hr;
}

// ---------------------------------------------------------------- setup

static GL3WglProc LoadGl(const char* name) { return (GL3WglProc)RealProc(name); }

static void Theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 8; s.FrameRounding = 5; s.GrabRounding = 5; s.TabRounding = 5;
    s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(8, 5); s.ItemSpacing = ImVec2(8, 7);
    ImVec4* c = s.Colors;
    ImVec4 accent(0.96f, 0.55f, 0.16f, 1.0f), accentDim(0.70f, 0.38f, 0.10f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.16f, 0.05f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.17f, 0.06f, 0.8f);
    c[ImGuiCol_HeaderHovered] = accentDim;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Button] = ImVec4(0.25f, 0.15f, 0.07f, 1.0f);
    c[ImGuiCol_ButtonHovered] = accentDim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.14f, 0.13f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.19f, 0.13f, 1.0f);
    c[ImGuiCol_PlotLines] = accent;
    s.ScaleAllSizes(g_scale);
}

static bool Init() {
    g_wnd = WindowFromDC(R.wglGetCurrentDC());
    if (!g_wnd) return false;
    if (imgl3wInit2(LoadGl) != 0) { SessionLog("overlay: could not load GL functions"); return false; }
    RECT rc{};
    GetClientRect(g_wnd, &rc);
    g_scale = std::max(1.0f, (rc.bottom - rc.top) / 1080.0f);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                       // our settings live in FortsGFX.ini
    // no keyboard navigation: a focused panel must not take the game's keys
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * g_scale);
    else
        io.Fonts->AddFontDefault();
    Theme();
    if (!ImGui_ImplWin32_Init(g_wnd) || !ImGui_ImplOpenGL3_Init("#version 330")) {
        SessionLog("overlay: ImGui backend init failed");
        return false;
    }
    g_prevProc = (WNDPROC)SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
    SessionLog("overlay: ready (scale %.2f)", g_scale);
    return true;
}

// ---------------------------------------------------------------- widgets

static void Tip(const char* t) {
    if (t && *t && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t);
}

// A combo with up/down arrow buttons in front of it that step through the items (wrapping).
// Returns true when the value changed.
static bool ComboCycle(const char* label, int* v, const char* const* items, int count) {
    bool changed = false;
    ImGui::PushID(label);
    const char* arrow = nullptr;
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { *v = (*v + count - 1) % count; changed = true; arrow = "up"; }
    ImGui::SameLine(0, 2 * g_scale);
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { *v = (*v + 1) % count; changed = true; arrow = "down"; }
    ImGui::SameLine(0, 6 * g_scale);
    ImGui::PopID();
    if (arrow) SessionLog("overlay: %s arrow %s -> %s", label, arrow, items[*v]);
    if (ImGui::Combo(label, v, items, count)) changed = true;
    return changed;
}

static void LookToast(int i) { Toast("Look %d/%d:  %s", i + 1, GfxLookCount(), GfxLookName(i)); }

static void StepLook(int d) {
    int n = GfxLookCount();
    if (n <= 0) return;
    int i = ((*GfxCurrentLook() + d) % n + n) % n;
    GfxApplyLook(i);
    LookToast(i);
}

// The look picker: arrows + drop-down over every built-in and saved look.
static void LookCombo() {
    int n = GfxLookCount();
    if (n <= 0) return;
    std::vector<const char*> names(n);
    for (int i = 0; i < n; i++) names[i] = GfxLookName(i);
    int v = std::min(std::max(*GfxCurrentLook(), 0), n - 1);
    if (ComboCycle("Look", &v, names.data(), n)) { GfxApplyLook(v); LookToast(v); }
    Tip(Fmt("A complete style: colour grade, lighting, materials, sky, fog, weather and effects. %s / %s switch looks any time.",
            HotKeyName(kKeyLookPrev), HotKeyName(kKeyLookNext)).c_str());
    ImGui::TextDisabled("%s", GfxLookDesc(v));
}

static void SettingWidget(const GfxSetting& s) {
    ImGui::PushID(s.ptr);
    switch (s.type) {
    case GfxSetting::Bool: ImGui::Checkbox(s.label, (bool*)s.ptr); break;
    case GfxSetting::Float: ImGui::SliderFloat(s.label, (float*)s.ptr, s.lo, s.hi, "%.3g"); break;
    case GfxSetting::Int:
        if (!strcmp(s.section, "Weather") && !strcmp(s.key, "Mode")) {
            static const char* modes[] = { "Auto (follow the map)", "Off", "Rain", "Storm", "Snow", "Thick snow", "Sandstorm", "Fog banks", "Ash & embers" };
            ComboCycle("Weather", (int*)s.ptr, modes, 9);
        } else ImGui::SliderInt(s.label, (int*)s.ptr, (int)s.lo, (int)s.hi);
        break;
    case GfxSetting::Vec2: ImGui::SliderFloat2(s.label, (float*)s.ptr, s.lo, s.hi, "%.2f"); break;
    case GfxSetting::Color: ImGui::ColorEdit3(s.label, (float*)s.ptr, ImGuiColorEditFlags_Float); break;
    }
    Tip(s.tip);
    ImGui::PopID();
}

// ---------------------------------------------------------------- showcase tour
// One chapter per thing FortsGFX adds. A chapter with a switch flips it OFF / ON (2 s off, 3 s on, a
// badge at the top) so you see exactly what it adds; the first wipes between the game as it ships and
// FortsGFX. Every chapter starts from your own settings, and all of it is put back at the end.
// From the Lab bar the chapters advance by themselves, with FortsGFX's own effects standing in for a
// battle. On the FortsGFX Showcase map the map script moves them along (marker sprites) and fires
// real shells, so the OFF side is the real vanilla game.

struct TourStep {
    const char* title; const char* sub; float dur;
    const char* sec; const char* key; float onVal, offVal;   // the switch; onVal < 0: the user's own value
    bool flipFree, flipMap;                                   // flips from the Lab bar / on the showcase map
    bool needsBattle;                                         // only on the showcase map (real game smoke)
};
static const TourStep kTour[] = {
    { "FortsGFX", "The game as it ships   |   with FortsGFX", 9.0f, nullptr, nullptr, 0, 0, false, false, false },
    { "Ray-traced 2D lighting", "Every fire, laser and explosion is a real light that casts soft shadows", 10.0f, "Lighting", "Enabled", -1, 0, true, true, false },
    { "Materials & sunlight", "Lit and shadowed sides, metal shine and a sun that moves", 9.0f, "Materials", "Enabled", -1, 0, true, true, false },
    { "Explosions", "3D fireballs, shockwaves, debris and sparks that bounce off the real terrain", 10.0f, "Impact", "Enabled", -1, 0, false, true, false },
    { "Bloom & glow", "Light that blooms - held in check so a big fight never washes the base out", 8.0f, "Bloom", "Strength", -1, 0, true, true, false },
    { "Simulated smoke", "Trails and smoke that billow, drift on the wind and glow by the fire next to them", 9.0f, "Impact", "SimulatedSmoke", -1, 0, false, true, true },
    { "Rain & lightning", "Drops that splash, wet shine on every surface, lightning that finds the ground", 10.0f, "Weather", "Mode", 3, 1, true, true, false },
    { "Snow", "Flakes that drift, settle and pile up on top of the forts", 10.0f, "Weather", "Mode", 5, 1, false, false, false },
    { "Fog banks", "Rolling fog that the light glows through", 8.0f, "Weather", "Mode", 7, 1, true, true, false },
    { "Day & night", "The sun crosses the sky: golden dusk, then a moonlit night", 10.0f, nullptr, nullptr, 0, 0, false, false, false },
    { "Water", "Reflections, foam and glints on the High Seas", 8.0f, "Water", "Enabled", -1, 0, true, true, false },
    { "Looks", "", 14.0f, nullptr, nullptr, 0, 0, false, false, false },
    { "Photo mode", "Freeze, depth of field and framing - then save a clean PNG with no UI", 7.0f, nullptr, nullptr, 0, 0, false, false, false },
    { "FortsGFX", "", 6.0f, nullptr, nullptr, 0, 0, false, false, false },   // the keys (DrawCaption)
};
enum { kTourIntro, kTourLight, kTourMaterials, kTourExplosions, kTourBloom, kTourSmoke, kTourRain, kTourSnow, kTourFog,
       kTourDay, kTourWater, kTourLooks, kTourPhoto, kTourEnd };
static const int kTourSteps = sizeof(kTour) / sizeof(kTour[0]);
static const char* kTourLookNames[] = { "Vivid Cartoon", "Golden Hour", "Moonlit Night", "Comic Book", "Neon Synthwave", "Film Noir", "Hellfire" };
static int g_step, g_tourLook;
static double g_stepT0, g_nextSpawn;
static std::vector<uint8_t> g_tourSnap, g_tourBase;   // your settings / the tour's bright, neutral start
static GfxLab g_tourLab;
static bool g_tourWasOpen, g_tourMap, g_tourSnowed, g_tourTodPaused;
static float g_tourTod;
static const GfxSetting* g_flip;   // this chapter's switch
static float g_flipOnVal;
static int g_tourBadge = -1;    // -1 no switch, 0 OFF, 1 ON
static std::atomic<int> g_tourPending{ -2 };
static uint32_t g_rng = 0x2545F491u;
static float Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.0f; }

void GfxTourChapter(int chapter) { g_tourPending = chapter < 0 ? -1 : chapter; }

static int FindLook(const char* name) {
    for (int i = 0; i < GfxLookCount(); i++) if (!strcmp(GfxLookName(i), name)) return i;
    return -1;
}
static float TourGet(const GfxSetting& s) {
    switch (s.type) {
    case GfxSetting::Bool: return *(bool*)s.ptr ? 1.0f : 0.0f;
    case GfxSetting::Int: return (float)*(int*)s.ptr;
    case GfxSetting::Float: return *(float*)s.ptr;
    default: return 0.0f;
    }
}
static void TourSet(const GfxSetting& s, float v) {
    switch (s.type) {
    case GfxSetting::Bool: *(bool*)s.ptr = v > 0.5f; break;
    case GfxSetting::Int: *(int*)s.ptr = (int)lroundf(v); break;
    case GfxSetting::Float: *(float*)s.ptr = v; break;
    default: break;
    }
}

static void TourStop();
static void TourEnter(int s) {
    // skip what can't be shown here: real game smoke needs the showcase map, water needs a sea
    while (s < kTourSteps && ((kTour[s].needsBattle && !g_tourMap) || (s == kTourWater && !GfxWaterVisible()))) s++;
    if (s >= kTourSteps) { TourStop(); return; }
    g_step = s;
    g_stepT0 = NowS();
    g_nextSpawn = 0;
    g_tourLook = -1;
    g_tourBadge = -1;
    GfxRestore(g_tourBase);
    *GfxTimeOfDay() = 12.5f; *GfxTimePaused() = true;   // midday unless the chapter is about the time of day
    GfxLab& lab = GfxLabState();
    lab.wipe = -1; lab.slowMo = 1; lab.hideHud = true; lab.dof = 0; lab.frameAspect = 0; lab.freeze = false;
    GfxLabClearLights();
    *GfxDebugView() = 0;
    const TourStep& st = kTour[s];
    g_flip = nullptr;
    if (st.sec) for (auto& gs : GfxSettings()) if (!strcmp(gs.section, st.sec) && !strcmp(gs.key, st.key)) { g_flip = &gs; break; }
    if (g_flip) {
        if (st.onVal >= 0) TourSet(*g_flip, st.onVal);
        else if (g_flip->type == GfxSetting::Bool || TourGet(*g_flip) <= 0.01f) TourSet(*g_flip, 1.0f);   // a switch the user had off
        g_flipOnVal = TourGet(*g_flip);
    }
    auto setF = [](const char* sec, const char* key, float v) { if (float* p = (float*)SettingPtr(sec, key)) *p = v; };
    switch (s) {
    case kTourLight: *GfxTimeOfDay() = 17.8f; break;   // a low evening sun: the lights stand out, the fort stays visible
    case kTourExplosions: lab.slowMo = 0.6f; break;
    case kTourSnow: setF("Weather", "SnowBuildUp", 25.0f); setF("Weather", "SnowAmount", 6.0f); g_tourSnowed = true; break;
    case kTourDay: if (bool* dn = (bool*)SettingPtr("DayNight", "Enabled")) *dn = true; *GfxTimePaused() = true; break;
    default: break;
    }
    SessionLog("overlay: tour chapter %d \"%s\"%s", s, st.title, g_flip ? " (switch)" : "");
}

static void TourStart(bool map) {
    g_tourSnap = GfxSnapshot();
    g_tourLab = GfxLabState();
    g_tourTod = *GfxTimeOfDay(); g_tourTodPaused = *GfxTimePaused();
    g_tourWasOpen = g_open;
    g_tour = true;
    g_tourMap = map;
    g_tourSnowed = false;
    g_open = false;
    g_tool = kToolNone;
    g_wipeOn = g_sweep = g_cycle = false;
    // The tour's own start: the Showcase look at midday, no weather or fog, whatever look you play with
    // (a night look made the whole tour too dark). Your settings come back when it ends.
    int showcase = FindLook("Showcase");
    if (showcase >= 0) GfxApplyLook(showcase, false);
    if (int* wm = (int*)SettingPtr("Weather", "Mode")) *wm = 1;
    if (bool* fog = (bool*)SettingPtr("Fog", "Enabled")) *fog = false;
    if (bool* dn = (bool*)SettingPtr("DayNight", "Enabled")) *dn = true;
    g_tourBase = GfxSnapshot();
    SessionLog("overlay: showcase tour started%s", map ? " by the showcase map" : "");
    if (!map) TourEnter(0);
}

static void TourStop() {
    if (!g_tour) return;
    g_tour = false;
    g_tourBadge = -1;
    GfxRestore(g_tourSnap);
    GfxLabState() = g_tourLab;
    *GfxTimeOfDay() = g_tourTod; *GfxTimePaused() = g_tourTodPaused;
    GfxLabClearLights();
    if (g_tourSnowed) GfxLabClearSnow();
    g_open = g_tourWasOpen;
    SessionLog("overlay: showcase tour ended");
}

static void TourUpdate(float W, float H) {
    if (!g_tour) return;
    double t = NowS() - g_stepT0;
    if (!g_tourMap && t >= kTour[g_step].dur) {
        TourEnter(g_step + 1);
        if (!g_tour) return;
        t = 0;
    }
    if (g_tourMap && t > 90.0) { TourStop(); return; }   // the map stopped moving the chapters along
    const TourStep& st = kTour[g_step];
    float u = (float)std::min(t / st.dur, 1.0);
    GfxLab& lab = GfxLabState();
    bool flip = g_flip && (g_tourMap ? st.flipMap : st.flipFree);
    g_tourBadge = flip ? (fmod(t, 5.0) >= 2.0 ? 1 : 0) : -1;   // OFF for 2 s, then ON for 3 s
    if (flip) {
        float want = g_tourBadge == 1 ? g_flipOnVal : st.offVal;
        if (TourGet(*g_flip) != want) TourSet(*g_flip, want);
    }
    bool demo = !g_tourMap;   // no battle: FortsGFX's own effects stand in
    switch (g_step) {
    case kTourIntro: {   // the wipe sweeps across from "as it ships" to FortsGFX, then settles in the middle
        float k = std::min(std::max((u - 0.08f) / 0.5f, 0.0f), 1.0f), b = std::min(std::max((u - 0.62f) / 0.25f, 0.0f), 1.0f);
        float sweep = 0.98f - 0.96f * k * k * (3.0f - 2.0f * k);
        lab.wipe = sweep + (0.5f - sweep) * b * b * (3.0f - 2.0f * b);
        if (demo && t >= g_nextSpawn) { GfxLabSpawn(kLabExplosion, W * (0.25f + 0.5f * Rnd()), H * (0.4f + 0.3f * Rnd()), 0.05f); g_nextSpawn = t + 1.4; }
        break;
    }
    case kTourLight: {   // two coloured lights orbit through the fort at night
        float a = (float)t * 0.85f;
        float warm[3] = { 1.0f, 0.6f, 0.25f }, cold[3] = { 0.35f, 0.6f, 1.0f };
        GfxLabSetLight(0, true, W * 0.5f + cosf(a) * W * 0.28f, H * 0.5f + sinf(a) * H * 0.2f, 0.05f, 6.0f, warm);
        GfxLabSetLight(1, true, W * 0.5f - cosf(a * 0.8f) * W * 0.22f, H * 0.52f - sinf(a * 0.8f) * H * 0.18f, 0.05f, 6.0f, cold);
        break;
    }
    case kTourMaterials: {
        float* sd = (float*)SettingPtr("Sun", "Direction");
        float ang = (160.0f - 140.0f * u) / 57.29578f;
        if (sd) { sd[0] = cosf(ang); sd[1] = sinf(ang); }
        break;
    }
    case kTourExplosions:
        if (demo && t >= g_nextSpawn) {
            GfxLabSpawn(kLabExplosion, W * (0.2f + 0.6f * Rnd()), H * (0.35f + 0.4f * Rnd()), 0.04f + 0.07f * Rnd());
            g_nextSpawn = t + 0.6;
        }
        break;
    case kTourBloom:
        if (demo && t >= g_nextSpawn) {
            GfxLabSpawn(Rnd() < 0.3f ? kLabMega : kLabExplosion, W * (0.25f + 0.5f * Rnd()), H * (0.4f + 0.3f * Rnd()), 0.06f + 0.05f * Rnd());
            g_nextSpawn = t + 0.75;
        }
        break;
    case kTourRain:
        if (g_tourBadge == 1 && t >= g_nextSpawn) {
            GfxLabSpawn(kLabLightning, W * (0.15f + 0.7f * Rnd()), H * 0.5f, 0.05f);
            g_nextSpawn = t + 2.2 + Rnd();
        }
        break;
    case kTourDay: *GfxTimeOfDay() = 6.0f + 15.5f * u; break;   // dawn to an early night, 06:00 to 21:30
    case kTourLooks: {
        int k = std::min((int)(u * 7), 6);
        if (k != g_tourLook) { g_tourLook = k; int i = FindLook(kTourLookNames[k]); if (i >= 0) GfxApplyLook(i, false); }
        break;
    }
    case kTourPhoto: lab.dof = 0.75f; lab.focusX = 0.5f; lab.focusY = 0.55f; lab.frameAspect = 2.39f; break;
    case kTourEnd:
        if (g_tourLook != -2) { GfxRestore(g_tourBase); g_tourLook = -2; }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------- screen overlays (captions, toast, wipe labels, reticle)

static void ShadowText(ImDrawList* dl, float size, ImVec2 p, ImU32 col, const char* text, float alpha) {
    ImFont* f = ImGui::GetFont();
    dl->AddText(f, size, ImVec2(p.x + size * 0.05f, p.y + size * 0.05f), IM_COL32(0, 0, 0, (int)(200 * alpha)), text);
    dl->AddText(f, size, p, col, text);
}

static void DrawCaption(ImDrawList* dl, ImVec2 ds) {
    if (!g_tour) return;
    const TourStep& st = kTour[g_step];
    double t = NowS() - g_stepT0;
    float a = (float)std::min(std::min(t / 0.45, (st.dur - t) / 0.45), 1.0);
    a = std::max(a, 0.0f);
    const char* sub = st.sub;
    char keySub[160];
    if (g_step == kTourLooks && g_tourLook >= 0) {
        snprintf(keySub, sizeof(keySub), "%s   (%d complete styles  -  %s / %s to switch)", kTourLookNames[g_tourLook], GfxLookCount(),
                 HotKeyName(kKeyLookPrev), HotKeyName(kKeyLookNext));
        sub = keySub;
    } else if (g_step == kTourPhoto) {
        snprintf(keySub, sizeof(keySub), "%s (%s)", st.sub, HotKeyName(kKeyPhoto));
        sub = keySub;
    } else if (g_step == kTourEnd) {
        snprintf(keySub, sizeof(keySub), "%s: panel     %s: Lab     %s / %s: looks     %s: photo mode", HotKeyName(kKeyPanel),
                 HotKeyName(kKeyLab), HotKeyName(kKeyLookPrev), HotKeyName(kKeyLookNext), HotKeyName(kKeyPhoto));
        sub = keySub;
    }
    ImFont* f = ImGui::GetFont();
    float ts = 60.0f * g_scale, ss = 26.0f * g_scale;
    ImVec2 tsz = f->CalcTextSizeA(ts, FLT_MAX, 0, st.title), ssz = f->CalcTextSizeA(ss, FLT_MAX, 0, sub);
    float y = ds.y * 0.76f;
    dl->AddRectFilledMultiColor(ImVec2(0, y - 40 * g_scale), ImVec2(ds.x, ds.y), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, (int)(170 * a)), IM_COL32(0, 0, 0, (int)(170 * a)));
    ShadowText(dl, ts, ImVec2((ds.x - tsz.x) * 0.5f, y), IM_COL32(255, 255, 255, (int)(255 * a)), st.title, a);
    float ly = y + tsz.y + 4 * g_scale, lw = std::max(tsz.x * 0.35f, 120 * g_scale);
    dl->AddRectFilled(ImVec2((ds.x - lw) * 0.5f, ly), ImVec2((ds.x + lw) * 0.5f, ly + 3 * g_scale), IM_COL32(245, 140, 40, (int)(255 * a)));
    ShadowText(dl, ss, ImVec2((ds.x - ssz.x) * 0.5f, ly + 14 * g_scale), IM_COL32(235, 225, 210, (int)(255 * a)), sub, a);
    if (g_tourBadge >= 0) {   // the switch: OFF (the game without it) / ON (with FortsGFX), and how long until it flips
        const char* txt = g_tourBadge ? "ON" : "OFF";
        const char* under = g_tourBadge ? "with FortsGFX" : "without it";
        float bs = 46.0f * g_scale, us = 22.0f * g_scale;
        ImVec2 bsz = f->CalcTextSizeA(bs, FLT_MAX, 0, txt), usz = f->CalcTextSizeA(us, FLT_MAX, 0, under);
        float cx = ds.x * 0.5f, cy = 78.0f * g_scale;
        float pw = std::max(bsz.x, 70.0f * g_scale) * 0.5f + 30.0f * g_scale, ph = bsz.y * 0.5f + 8.0f * g_scale;
        dl->AddRectFilled(ImVec2(cx - pw, cy - ph), ImVec2(cx + pw, cy + ph),
                          g_tourBadge ? IM_COL32(235, 130, 35, (int)(235 * a)) : IM_COL32(55, 55, 60, (int)(225 * a)), ph);
        ShadowText(dl, bs, ImVec2(cx - bsz.x * 0.5f, cy - bsz.y * 0.5f), IM_COL32(255, 255, 255, (int)(255 * a)), txt, a);
        ShadowText(dl, us, ImVec2(cx - usz.x * 0.5f, cy + ph + 6.0f * g_scale), IM_COL32(235, 225, 210, (int)(255 * a)), under, a);
        double p5 = fmod(t, 5.0);
        float done = (float)(g_tourBadge ? (p5 - 2.0) / 3.0 : p5 / 2.0);
        float by = cy + ph + usz.y + 14.0f * g_scale;
        dl->AddRectFilled(ImVec2(cx - pw, by), ImVec2(cx + pw, by + 3.0f * g_scale), IM_COL32(255, 255, 255, (int)(60 * a)));
        dl->AddRectFilled(ImVec2(cx - pw, by), ImVec2(cx - pw + 2.0f * pw * done, by + 3.0f * g_scale), IM_COL32(255, 255, 255, (int)(200 * a)));
    }
    for (int i = 0; i < kTourSteps; i++) {   // progress dots
        float cx = ds.x * 0.5f + (i - (kTourSteps - 1) * 0.5f) * 22 * g_scale, cy = ds.y - 26 * g_scale;
        dl->AddCircleFilled(ImVec2(cx, cy), (i == g_step ? 6.0f : 4.0f) * g_scale,
                            i == g_step ? IM_COL32(245, 140, 40, 255) : IM_COL32(255, 255, 255, i < g_step ? 200 : 80));
    }
}

static void DrawToast(ImDrawList* dl, ImVec2 ds) {
    double t = NowS() - g_toastT;
    if (t > 2.4 || g_tour) return;
    float a = (float)std::min(1.0, (2.4 - t) / 0.4);
    ImFont* f = ImGui::GetFont();
    float sz = 30.0f * g_scale;
    ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0, g_toast);
    ImVec2 p((ds.x - ts.x) * 0.5f, 70 * g_scale), pad(22 * g_scale, 12 * g_scale);
    dl->AddRectFilled(ImVec2(p.x - pad.x, p.y - pad.y), ImVec2(p.x + ts.x + pad.x, p.y + ts.y + pad.y), IM_COL32(15, 12, 10, (int)(215 * a)), 10 * g_scale);
    dl->AddRect(ImVec2(p.x - pad.x, p.y - pad.y), ImVec2(p.x + ts.x + pad.x, p.y + ts.y + pad.y), IM_COL32(245, 140, 40, (int)(220 * a)), 10 * g_scale, 0, 2 * g_scale);
    dl->AddText(f, sz, p, IM_COL32(255, 245, 230, (int)(255 * a)), g_toast);
}

static void DrawWipeLabels(ImDrawList* dl, ImVec2 ds) {
    float w = GfxLabState().wipe;
    if (w < 0) return;
    ImFont* f = ImGui::GetFont();
    float sz = 24.0f * g_scale, x = w * ds.x, y = 24 * g_scale;
    ImVec2 a = f->CalcTextSizeA(sz, FLT_MAX, 0, "BEFORE");
    if (x - a.x - 16 * g_scale > 0) ShadowText(dl, sz, ImVec2(x - a.x - 16 * g_scale, y), IM_COL32(255, 255, 255, 230), "BEFORE", 1);
    if (x + 16 * g_scale + 140 * g_scale < ds.x) ShadowText(dl, sz, ImVec2(x + 16 * g_scale, y), IM_COL32(255, 200, 120, 255), "FORTSGFX", 1);
}

// ---------------------------------------------------------------- lab window + click tools

static void ToolButton(int id) {
    bool sel = g_tool == id;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(kToolNames[id], ImVec2(160 * g_scale, 0))) g_tool = sel ? kToolNone : id;
    if (sel) ImGui::PopStyleColor();
}

static void LabTools(ImDrawList* dl) {
    if (!(g_open || g_labOpen) || g_tool == kToolNone) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) {                 // over a panel: release a held light, no reticle
        if (g_tool == kToolLight) GfxLabSetLight(0, false, 0, 0, 0, 0, g_lightCol);
        return;
    }
    ImVec2 m = io.MousePos;
    float H = io.DisplaySize.y;
    bool light = g_tool == kToolLight || g_tool == kToolPlaceLight;
    float rad = (light ? g_lightSize : g_toolSize) * H;
    if (g_tool == kToolSnow) rad *= 1.0f;
    dl->AddCircle(m, rad, IM_COL32(255, 190, 90, 200), 48, 2 * g_scale);
    dl->AddText(ImVec2(m.x + 14 * g_scale, m.y + 14 * g_scale), IM_COL32(255, 230, 200, 230), kToolNames[g_tool]);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { g_tool = kToolNone; return; }
    if (g_tool < kToolLight) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) GfxLabSpawn(g_tool, m.x, m.y, g_toolSize);
    } else if (g_tool == kToolSnow) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) GfxLabSpawn(kLabSnow, m.x, m.y, g_toolSize);
    } else if (g_tool == kToolLight) {
        GfxLabSetLight(0, ImGui::IsMouseDown(ImGuiMouseButton_Left), m.x, m.y, g_lightSize, g_lightInten, g_lightCol);
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        GfxLabSetLight(1 + g_pinned % 7, true, m.x, m.y, g_lightSize, g_lightInten, g_lightCol);
        g_pinned++;
    }
}

// The lab is a HUD bar along the top of the screen (Home opens it on its own). One row of buttons;
// "More" adds a second row of sliders and switches.
static bool g_more;

static bool BarToggle(const char* label, bool on, const char* tip) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.02f, 1.0f));
    }
    bool clicked = ImGui::Button(label);
    if (on) ImGui::PopStyleColor(2);
    Tip(tip);
    return clicked;
}

static void BarGap() {
    ImGui::SameLine(0, 10 * g_scale);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 3 * g_scale), ImVec2(p.x, p.y + h - 3 * g_scale), IM_COL32(245, 140, 40, 120), 1.5f * g_scale);
    ImGui::Dummy(ImVec2(1, h));
    ImGui::SameLine(0, 10 * g_scale);
}

static void DrawLabBar() {
    ImGuiIO& io = ImGui::GetIO();
    GfxLab& lab = GfxLabState();
    if (g_open)   // the panel sits top-left: keep the bar clear of it
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10 * g_scale, 10 * g_scale), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    else
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 10 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * g_scale, 8 * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5 * g_scale, 6 * g_scale));
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##labbar", nullptr, fl)) { ImGui::End(); ImGui::PopStyleVar(2); return; }

    // ---- row 1: tools | look | compare | tech view | time | weather | presentation
    ImGui::TextColored(ImVec4(0.96f, 0.55f, 0.16f, 1.0f), "LAB");
    BarGap();
    static const char* toolTips[kToolCount] = {
        "Click in the battle: an explosion (fireball, shockwave, debris, light)",
        "Click: a huge blast with an impact frame",
        "Click: a lightning strike that finds the ground below",
        "Click: a shockwave ring",
        "Click: a flash of light",
        "Click: heat haze and rising embers",
        "Click: a burst of sparks that bounce off the terrain",
        "Hold the left button: a ray-traced light that follows the mouse",
        "Click: leave a light there (up to 7)",
        "Hold the left button: spray snow around the mouse (sand or ash in those weathers); it settles and piles up" };
    static const char* shortNames[kToolCount] = { "Explosion", "Mega", "Lightning", "Shock", "Flash", "Heat", "Sparks", "Light", "Pin light", "Snow" };
    for (int i = 0; i < kToolCount; i++) {
        if (BarToggle(shortNames[i], g_tool == i, toolTips[i])) g_tool = g_tool == i ? kToolNone : i;
        ImGui::SameLine();
    }
    if (ImGui::Button("Clear")) { GfxLabClearLights(); g_pinned = 0; }
    Tip("Remove all placed lights");
    BarGap();
    ImGui::SetNextItemWidth(170 * g_scale);
    ImGui::SliderFloat("##esize1", &g_toolSize, 0.01f, 0.3f, "size %.2f");
    Tip("Size of explosions, shockwaves, flashes and heat (fraction of the view height)");
    // ---- row 2: look | compare | tech view | time | weather | presentation

    int n = GfxLookCount();
    if (n > 0) {
        int cur = std::min(std::max(*GfxCurrentLook(), 0), n - 1);
        if (ImGui::ArrowButton("##lookprev", ImGuiDir_Left)) StepLook(-1);
        Tip(Fmt("Previous look (%s)", HotKeyName(kKeyLookPrev)).c_str());
        ImGui::SameLine();
        char lbl[96];
        snprintf(lbl, sizeof(lbl), "%s##lookname", GfxLookName(cur));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::Button(lbl, ImVec2(190 * g_scale, 0))) StepLook(+1);
        ImGui::PopStyleColor();
        Tip(GfxLookDesc(cur));
        ImGui::SameLine();
        if (ImGui::ArrowButton("##looknext", ImGuiDir_Right)) StepLook(+1);
        Tip(Fmt("Next look (%s)", HotKeyName(kKeyLookNext)).c_str());
        ImGui::SameLine();
        if (BarToggle("Auto", g_cycle, "Cycle through the looks automatically")) { g_cycle = !g_cycle; g_cycleT = NowS(); }
    }
    BarGap();

    if (BarToggle("Before/After", lab.wipe >= 0, "Split screen: left of the line the game's own colours, right FortsGFX")) {
        g_wipeOn = lab.wipe < 0;
        lab.wipe = g_wipeOn ? 0.5f : -1.0f;
        g_sweep = false;
    }
    if (lab.wipe >= 0) {
        ImGui::SameLine();
        if (BarToggle("Sweep", g_sweep, "Move the split line back and forth")) g_sweep = !g_sweep;
    }
    BarGap();

    static const char* views[] = { "Final image", "Masks", "Traced light", "Sky visibility", "Distance field", "Normals", "Light sources", "No effects" };
    int* dv = GfxDebugView();
    if (ImGui::ArrowButton("##viewprev", ImGuiDir_Left)) *dv = (*dv + 7) % 8;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(*dv ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
    if (*dv) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.02f, 1.0f));
    if (ImGui::Button(views[*dv], ImVec2(150 * g_scale, 0))) *dv = 0;
    if (*dv) ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    Tip("What the renderer sees: masks, ray-traced light, sky visibility, distance field, normals... (click: back to the final image)");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##viewnext", ImGuiDir_Right)) *dv = (*dv + 1) % 8;
    BarGap();

    if (int* fxp = (int*)SettingPtr("Presets", "Effects")) {
        static const char* fxShort[] = { "FX: Showcase", "FX: Cinematic", "FX: Subtle" };
        int v = std::min(std::max(*fxp, 0), 2);
        if (ImGui::Button(fxShort[v], ImVec2(160 * g_scale, 0))) { GfxApplyFx((v + 1) % 3); Toast("Effects: %s", fxShort[(v + 1) % 3] + 4); }
        Tip("How hard explosions hit (click to cycle). Looks never change this.");
        BarGap();
    }
    if (BarToggle("1x", lab.slowMo >= 0.99f, "Effects at real speed")) lab.slowMo = 1.0f;
    ImGui::SameLine();
    if (BarToggle("0.5x", lab.slowMo > 0.3f && lab.slowMo < 0.99f, "Effects at half speed")) lab.slowMo = 0.5f;
    ImGui::SameLine();
    if (BarToggle("0.2x", lab.slowMo <= 0.3f, "Slow motion for FortsGFX's effects (the game's own sprites keep their speed)")) lab.slowMo = 0.2f;
    BarGap();

    if (int* wm = (int*)SettingPtr("Weather", "Mode")) {
        static const char* wn[] = { "Weather: map", "Weather: off", "Weather: rain", "Weather: storm", "Weather: snow", "Weather: blizzard",
                                    "Weather: sand", "Weather: fog", "Weather: ash" };
        int w = std::min(std::max(*wm, 0), 8);
        if (ImGui::Button(wn[w], ImVec2(170 * g_scale, 0))) *wm = (w + 1) % 9;
        Tip("Click to cycle: follow the map, off, rain, storm, snow, thick snow, sandstorm, fog banks, ash & embers");
        ImGui::SameLine();
    }
    if (ImGui::Button("Strike")) GfxLabSpawn(kLabLightning, io.DisplaySize.x * (0.2f + 0.6f * Rnd()), io.DisplaySize.y * 0.5f, 0.05f);
    Tip("A lightning strike somewhere in view");
    ImGui::SameLine();
    if (ImGui::Button("Clear snow")) GfxLabClearSnow();
    Tip("Remove all the snow, sand or ash lying in the scene");
    BarGap();
    {   // day / night
        float* tod = GfxTimeOfDay();
        bool* paused = GfxTimePaused();
        if (ImGui::ArrowButton("##todp", ImGuiDir_Left)) *tod = fmodf(*tod + 23.0f, 24.0f);
        Tip("An hour earlier");
        ImGui::SameLine();
        int hh = (int)*tod, mm = (int)((*tod - hh) * 60.0f);
        char tl[48];
        snprintf(tl, sizeof(tl), "%s %02d:%02d%s", (hh >= 6 && hh < 18) ? "Day" : "Night", hh, mm, *paused ? " ||" : "");
        if (BarToggle(tl, !*paused, "Time of day - click to pause / resume the cycle")) *paused = !*paused;
        ImGui::SameLine();
        if (ImGui::ArrowButton("##todn", ImGuiDir_Right)) *tod = fmodf(*tod + 1.0f, 24.0f);
        Tip("An hour later");
    }
    BarGap();

    if (BarToggle("Photo", g_photo, "Photo mode (End): freeze, depth of field, framing, save a clean PNG")) { if (g_photo) PhotoExit(); else PhotoEnter(); }
    ImGui::SameLine();
    if (BarToggle("Hide HUD", lab.hideHud, "Hide the game's HUD and cursor (screenshots, videos)")) lab.hideHud = !lab.hideHud;
    ImGui::SameLine();
    if (BarToggle(g_tour ? "Stop tour" : "Tour", g_tour, "A ~2 min showcase: every feature switched OFF, then ON; your settings are put back afterwards")) {
        if (g_tour) TourStop(); else TourStart(false);
    }
    BarGap();
    if (BarToggle("More", g_more, "Sliders and switches")) g_more = !g_more;
    ImGui::SameLine();
    if (BarToggle("Panel", g_open, Fmt("The full control panel (%s)", HotKeyName(kKeyPanel)).c_str())) g_open = !g_open;
    ImGui::SameLine();
    if (ImGui::Button("X")) { g_labOpen = false; g_tool = kToolNone; GfxLabSetLight(0, false, 0, 0, 0, 0, g_lightCol); }
    Tip(Fmt("Close the lab bar (%s)", HotKeyName(kKeyLab)).c_str());

    // ---- rows 3-4 (More): light, sun, looks, systems
    if (g_more) {
        float w = 150 * g_scale;
        ImGui::SetNextItemWidth(w); ImGui::SliderFloat("##lsize", &g_lightSize, 0.01f, 0.25f, "light size %.2f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(w); ImGui::SliderFloat("##lbright", &g_lightInten, 0.5f, 12.0f, "light power %.1f");
        ImGui::SameLine(); ImGui::ColorEdit3("##lcol", g_lightCol, ImGuiColorEditFlags_NoInputs);
        Tip("Light colour");
        BarGap();
        if (float* sdp = (float*)SettingPtr("Weather", "SnowDepth")) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##snowdepth", sdp, 1.0f, 24.0f, "snow depth %.0f");
            Tip("How deep snow can pile up (steps of 4 px at 4K)");
            ImGui::SameLine();
        }
        if (float* sb = (float*)SettingPtr("Weather", "SnowBuildUp")) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##snowbuild", sb, 0.0f, 40.0f, "build-up %.1f", ImGuiSliderFlags_Logarithmic);
            Tip("How fast snow, sand or ash piles up (1 gentle, 10 fast, 40 instant)");
            ImGui::SameLine();
        }
        if (float* sa = (float*)SettingPtr("Weather", "SnowAmount")) {
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##snowamt", sa, 0.0f, 12.0f, "snowfall %.1f", ImGuiSliderFlags_Logarithmic);
            Tip("How many flakes fall");
            ImGui::SameLine();
        }
        if (float* sd = (float*)SettingPtr("Sun", "Direction")) {
            float ang = atan2f(sd[1], sd[0]) * 57.29578f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##sun", &ang, 5.0f, 175.0f, "sun %.0f deg")) { sd[0] = cosf(ang / 57.29578f); sd[1] = sinf(ang / 57.29578f); }
            Tip("Sun angle: lighting direction, highlights, shadows and the sun in the sky");
            ImGui::SameLine();
        }
        if (lab.wipe >= 0) {
            float wp = lab.wipe;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##wipe", &wp, 0.0f, 1.0f, "split %.2f")) { lab.wipe = wp; g_sweep = false; }
            ImGui::SameLine();
        }
        if (g_cycle) { ImGui::SetNextItemWidth(w); ImGui::SliderFloat("##secs", &g_cycleSecs, 1.5f, 20.0f, "%.0f s per look"); ImGui::SameLine(); }
        ImGui::SetNextItemWidth(170 * g_scale);
        ImGui::InputText("##lookname2", g_newLook, sizeof(g_newLook));
        ImGui::SameLine();
        if (ImGui::Button("Save look")) {
            int i = GfxSaveCustomLook(g_newLook);
            if (i >= 0) Toast("Saved look \"%s\"", GfxLookName(i)); else Toast("No room for more saved looks");
        }
        Tip("Save every look setting as it is now under this name");
        int cur = *GfxCurrentLook();
        if (GfxLookIsCustom(cur)) { ImGui::SameLine(); if (ImGui::Button("Delete look")) GfxDeleteCustomLook(cur); }
        ImGui::TextDisabled("Water");     // row 4: water and sky tuning
        ImGui::SameLine();
        struct Sl { const char *sec, *key, *fmt, *tip; float lo, hi; };
        static const Sl sliders[] = {
            { "Water", "Reflections", "reflections %.2f", "How strongly the water mirrors what is above it (0 = none)", 0.0f, 1.5f },
            { "Water", "Waves", "ripples %.2f", "Ripples and glints on the water", 0.0f, 3.0f },
            { "Water", "Foam", "foam %.2f", "Foam along the waterline", 0.0f, 2.0f },
            { "DayNight", "DuskGlow", "dusk glow %.2f", "How orange sunrise and sunset get (day / night cycle)", 0.0f, 1.0f },
        };
        for (auto& sl : sliders) {
            float* v = (float*)SettingPtr(sl.sec, sl.key);
            if (!v) continue;
            ImGui::SetNextItemWidth(w);
            ImGui::PushID(sl.key);
            ImGui::SliderFloat("##s", v, sl.lo, sl.hi, sl.fmt);
            ImGui::PopID();
            Tip(sl.tip);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::TextDisabled("Bloom");     // row 5: bloom tuning (kept when you change looks)
        ImGui::SameLine();
        static const Sl bloom[] = {
            { "Bloom", "Strength", "bloom %.2f", "How much bloom is laid over the image (its opacity)", 0.0f, 2.0f },
            { "Bloom", "Spread", "spread %.2f", "Lower: glows stay tight around each light instead of a haze over the base", 0.3f, 1.0f },
            { "Bloom", "Limit", "limit %.1f", "The brightest bloom can get anywhere (0 = no limit)", 0.0f, 8.0f },
            { "Bloom", "AutoDim", "auto-dim %.2f", "Turns bloom down while much of the screen glows (big fights)", 0.0f, 2.0f },
            { "Impact", "EffectGlow", "effect glow %.2f", "How much explosions and flashes glow (the effects preset's)", 0.0f, 4.0f },
            { "Bloom", "FireGlow", "fire glow %.2f", "How much burning structures and their sparks glow and light things up", 0.0f, 2.0f },
            { "Impact", "FireHeat", "fire heat %.2f", "How strongly burning structures shimmer with heat", 0.0f, 1.5f },
            { "GameFx", "BeamLook", "beam glow %.2f", "The layered glow over beam lasers and firebeams (halo, glow, white-hot core); 0 = off", 0.0f, 3.0f },
        };
        for (auto& sl : bloom) {
            float* v = (float*)SettingPtr(sl.sec, sl.key);
            if (!v) continue;
            ImGui::SetNextItemWidth(w);
            ImGui::PushID(sl.key);
            ImGui::SliderFloat("##b", v, sl.lo, sl.hi, sl.fmt);
            ImGui::PopID();
            Tip(sl.tip);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::TextDisabled("Explosion"); // row 5b: the blast itself - try it with the Explosion tool above
        ImGui::SameLine();
        static const Sl blast[] = {
            { "Impact", "FireballSize", "fireball %.2f", "Size of the 3D fireball (x the explosion)", 0.5f, 4.0f },
            { "Impact", "FireballBrightness", "fireball glow %.2f", "How bright the 3D fireball burns", 0.0f, 6.0f },
            { "Impact", "FlashLight", "flash %.2f", "The burst of light an explosion throws on the fort", 0.0f, 6.0f },
            { "Impact", "Shockwave", "shockwave %.2f", "How strongly the shockwave ripples the image", 0.0f, 3.0f },
            { "Impact", "ShockwaveSize", "shock size %.1f", "How far the shockwave spreads (x the explosion)", 1.0f, 20.0f },
            { "GameFx", "BlastWind", "blast wind %.2f", "Blasts shove the smoke, dust, rain, snow and embers around them outward, then the air rushing back draws them in a little", 0.0f, 3.0f },
            { "Impact", "HeatHaze", "heat haze %.2f", "Shimmer over explosions, fire and hot guns", 0.0f, 3.0f },
            { "Impact", "ColourPulse", "pulse %.2f", "White flash and colour punch on big hits", 0.0f, 2.0f },
            { "Impact", "ScreenShake", "shake %.2f", "Screen shake", 0.0f, 2.0f },
        };
        for (auto& sl : blast) {
            float* v = (float*)SettingPtr(sl.sec, sl.key);
            if (!v) continue;
            ImGui::SetNextItemWidth(w);
            ImGui::PushID(sl.key);
            ImGui::SliderFloat("##x", v, sl.lo, sl.hi, sl.fmt);
            ImGui::PopID();
            Tip(sl.tip);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::TextDisabled("Lighting");  // row 6: how much the ray-traced light stands out
        ImGui::SameLine();
        static const Sl light[] = {
            { "Materials", "AmbientLevel", "ambient %.2f", "Brightness of surfaces lit only by the sky: lower it to see the ray-traced light and shadows", 0.2f, 2.0f },
            { "Lighting", "Intensity", "light %.2f", "How strongly fires, lasers and explosions light the surfaces around them", 0.0f, 4.0f },
            { "Lighting", "AmbientOcclusion", "shadows %.2f", "How dark enclosed places get (inside forts, under overhangs)", 0.0f, 1.0f },
            { "Sun", "OpenAreaBrightening", "sun %.2f", "How much the sun brightens open surfaces", 0.0f, 1.5f },
        };
        for (auto& sl : light) {
            float* v = (float*)SettingPtr(sl.sec, sl.key);
            if (!v) continue;
            ImGui::SetNextItemWidth(w);
            ImGui::PushID(sl.key);
            ImGui::SliderFloat("##l", v, sl.lo, sl.hi, sl.fmt);
            ImGui::PopID();
            Tip(sl.tip);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::TextDisabled("Systems");   // row 7: on/off for each system
        ImGui::SameLine();
        struct Tog { const char *sec, *key, *label, *tip; };
        static const Tog toggles[] = {
            { "Post", "Enabled", "All", "Every FortsGFX effect (F9)" },
            { "Lighting", "Enabled", "Lighting", "Ray-traced lighting (F10)" },
            { "Materials", "Enabled", "Materials", "Normals, shading, metal shine" },
            { "Fog", "Enabled", "Fog", "Volumetric fog" },
            { "Impact", "Enabled", "Impacts", "Explosion effects" },
            { "Impact", "ImpactFrames", "Impact frames", "Anime-style flash on big hits" },
            { "Backdrop", "Enabled", "3D sky", "Replace the far background with the 3D backdrop" },
            { "Backdrop", "HideOriginal", "Only 3D sky", "Hide every original background layer, near ones too (needs 3D sky on)" },
            { "Weather", "ReplaceGameRain", "Rain", "Replace the game's rain with FortsGFX rain" },
            { "DayNight", "Enabled", "Day/night", "The day / night cycle" },
            { "Water", "Enabled", "Water", "Water reflections, waves, foam (maps with water)" },
            { "Impact", "SimulatedSmoke", "Sim smoke", "Simulated steam and smoke" },
            { "Impact", "SimulateMoreEffects", "More sim fx", "The newer replacements: deck gun blast clouds (hot gas), dust, smoke clouds" },
            { "Fun", "MilkMiko", "Milk miko", "A joke: the water cannon is an anime shrine maiden spraying milk" },
        };
        const int nSys = (int)(sizeof(toggles) / sizeof(toggles[0]));
        for (int i = 0; i < nSys; i++) {
            bool* b = (bool*)SettingPtr(toggles[i].sec, toggles[i].key);
            if (!b) continue;
            if (BarToggle(toggles[i].label, *b, toggles[i].tip)) *b = !*b;
            if (i < nSys - 1) ImGui::SameLine();
        }
        ImGui::TextDisabled("Game events");   // row 8: effects driven by what the game reports (engine watch)
        ImGui::SameLine();
        static const Tog gameFx[] = {
            { "GameFx", "Enabled", "All", "Effects from the game's own events: what was hit and by what, which weapon fired, which device died" },
            { "GameFx", "MaterialImpacts", "Materials", "Sparks and slag off armour, splinters off bracing, shield ripples, splashes, earth" },
            { "GameFx", "Muzzles", "Muzzles", "Cannon ring and smoke, mortar puff, sniper shimmer, hot machine-gun barrels, launches" },
            { "GameFx", "Lasers", "Lasers", "Emitter glow, light along the beam, molten glow where it cuts" },
            { "GameFx", "HeavyWeapons", "Heavy", "Warheads, nukes, reactors: shockwave ring, camera exposure, dust wave" },
            { "GameFx", "ScorchMarks", "Scorch", "Scorch marks and craters where things really hit" },
            { "GameFx", "DeviceDeaths", "Devices", "Reactor glow, battery arcs, mine dust, smoking wrecks" },
            { "GameFx", "CollapseDust", "Collapse", "Dust and rubble where struts break" },
            { "GameFx", "Weather", "Weather", "Rain steams on fires, snow melts near them, storm wind bends smoke" },
            { "GameFx", "EventLights", "Hit lights", "Shots and hits light the structures around them" },
            { "GameFx", "InFlight", "In flight", "Shell heat shimmer, sound-barrier rings, missile shadows" },
        };
        const int nGame = (int)(sizeof(gameFx) / sizeof(gameFx[0]));
        for (int i = 0; i < nGame; i++) {
            bool* b = (bool*)SettingPtr(gameFx[i].sec, gameFx[i].key);
            if (!b) continue;
            if (BarToggle(gameFx[i].label, *b, gameFx[i].tip)) *b = !*b;
            if (i < nGame - 1) ImGui::SameLine();
        }
        ImGui::TextDisabled("After a hit");   // row 8b: what a hit leaves behind (instead of leaning on the shockwave)
        ImGui::SameLine();
        static const Tog after[] = {
            { "GameFx", "ShakeOff", "Dust shaken off", "Dust and grit shaken off the struts and surfaces above and around a hit; bigger hits loosen more, wider" },
            { "GameFx", "HitColour", "Debris colour", "Chunks and splinters take the colour of what they broke off" },
            { "GameFx", "DustHaze", "Dust haze", "Heavy hits leave a thin haze of dust that settles over a few seconds" },
            { "GameFx", "GroundBlast", "Ground blast", "Ground hits blow a ring of dark smoke out along the ground; blasts kick dust off the surfaces around them" },
        };
        const int nAfter = (int)(sizeof(after) / sizeof(after[0]));
        for (int i = 0; i < nAfter; i++) {
            bool* b = (bool*)SettingPtr(after[i].sec, after[i].key);
            if (!b) continue;
            if (BarToggle(after[i].label, *b, after[i].tip)) *b = !*b;
            if (i < nAfter - 1) ImGui::SameLine();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
// ---------------------------------------------------------------- photo mode
// End (PhotoKeyVK) or the lab bar: everything FortsGFX freezes in place, the HUD and our UI hide behind a
// small toolbar, click to focus the depth of field, frame the shot and save a clean full-resolution PNG.
static void PhotoEnter() {
    if (g_tour) TourStop();
    g_photoSaved = GfxLabState();
    GfxLab& lab = GfxLabState();
    lab.hideHud = true; lab.freeze = true;
    g_tool = kToolNone;
    g_photo = true;
    SessionLog("overlay: photo mode on");
}
static void PhotoExit() {
    GfxLab& lab = GfxLabState();
    GfxLab keep = lab;
    lab = g_photoSaved;
    lab.wipe = keep.wipe;
    g_photo = false;
    SessionLog("overlay: photo mode off");
}

static void FrameRect(ImVec2 ds, float aspect, ImVec2& a, ImVec2& b) {
    a = ImVec2(0, 0); b = ds;
    if (aspect <= 0) return;
    float s = ds.x / ds.y;
    if (aspect > s) { float h = ds.x / aspect; a.y = (ds.y - h) * 0.5f; b.y = a.y + h; }
    else { float w = ds.y * aspect; a.x = (ds.x - w) * 0.5f; b.x = a.x + w; }
}

static void DrawPhotoBar(ImDrawList* fg) {
    ImGuiIO& io = ImGui::GetIO();
    GfxLab& lab = GfxLabState();
    ImVec2 ds = io.DisplaySize;
    // guides (never in the photo: our UI draws after the frame is saved)
    ImVec2 fa, fb;
    FrameRect(ds, lab.frameAspect, fa, fb);
    if (g_photoGrid)
        for (int i = 1; i < 3; i++) {
            float x = fa.x + (fb.x - fa.x) * i / 3.0f, y = fa.y + (fb.y - fa.y) * i / 3.0f;
            fg->AddLine(ImVec2(x, fa.y), ImVec2(x, fb.y), IM_COL32(255, 255, 255, 70), 1.5f * g_scale);
            fg->AddLine(ImVec2(fa.x, y), ImVec2(fb.x, y), IM_COL32(255, 255, 255, 70), 1.5f * g_scale);
        }
    if (lab.dof > 0 && NowS() - g_focusT < 1.2) {
        ImVec2 fp(lab.focusX * ds.x, (1.0f - lab.focusY) * ds.y);
        fg->AddCircle(fp, 26 * g_scale, IM_COL32(255, 200, 90, 220), 32, 2 * g_scale);
        fg->AddCircle(fp, 4 * g_scale, IM_COL32(255, 200, 90, 220), 12, 2 * g_scale);
    }
    // click in the scene: focus there
    if (!io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        lab.focusX = io.MousePos.x / ds.x; lab.focusY = 1.0f - io.MousePos.y / ds.y;
        if (lab.dof <= 0) lab.dof = 0.6f;
        g_focusT = NowS();
    }
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 16 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("##photobar", nullptr, fl)) { ImGui::End(); return; }
    ImGui::TextColored(ImVec4(0.96f, 0.55f, 0.16f, 1.0f), "PHOTO");
    ImGui::SameLine();
    if (BarToggle("Freeze", lab.freeze, "Stop every FortsGFX effect where it is (pause the game too for a perfect still)")) lab.freeze = !lab.freeze;
    ImGui::SameLine();
    float w = 160 * g_scale;
    ImGui::SetNextItemWidth(w); ImGui::SliderFloat("##dof", &lab.dof, 0.0f, 1.0f, lab.dof > 0 ? "depth of field %.2f" : "depth of field off");
    Tip("Click in the scene to focus there: the other layer (forts or backdrop) blurs");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w); ImGui::SliderFloat("##tilt", &lab.tilt, 0.0f, 1.0f, lab.tilt > 0 ? "tilt-shift %.2f" : "tilt-shift off");
    Tip("Blur above and below the focus point: a miniature look");
    ImGui::SameLine();
    static const float aspects[] = { 0.0f, 16.0f / 9.0f, 21.0f / 9.0f, 2.39f, 0.8f, 1.0f };
    static const char* aspectNames[] = { "Frame: full", "Frame: 16:9", "Frame: 21:9", "Frame: 2.39:1", "Frame: 4:5", "Frame: 1:1" };
    int fi = 0;
    for (int i = 0; i < 6; i++) if (fabsf(aspects[i] - lab.frameAspect) < 0.01f) fi = i;
    if (ImGui::Button(aspectNames[fi], ImVec2(150 * g_scale, 0))) lab.frameAspect = aspects[(fi + 1) % 6];
    Tip("Framing bars (they are part of the photo)");
    ImGui::SameLine();
    if (BarToggle("Grid", g_photoGrid, "Rule-of-thirds guides (not in the photo)")) g_photoGrid = !g_photoGrid;
    ImGui::SameLine();
    if (ImGui::ArrowButton("##plookp", ImGuiDir_Left)) StepLook(-1);
    ImGui::SameLine();
    ImGui::TextUnformatted(GfxLookName(*GfxCurrentLook()));
    ImGui::SameLine();
    if (ImGui::ArrowButton("##plookn", ImGuiDir_Right)) StepLook(+1);
    ImGui::SameLine();
    float* tod = GfxTimeOfDay();
    ImGui::SetNextItemWidth(w);
    int hh = (int)*tod, mm = (int)((*tod - hh) * 60.0f);
    char tl[32]; snprintf(tl, sizeof(tl), "time %02d:%02d", hh, mm);
    if (ImGui::SliderFloat("##ptime", tod, 0.0f, 23.99f, tl)) *GfxTimePaused() = true;
    Tip("Time of day (pauses the cycle)");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.02f, 1.0f));
    if (ImGui::Button("  SAVE PHOTO  ")) { CapRequestPhoto(); Toast("Photo saved to FortsGFX\\Photos"); }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Exit")) PhotoExit();
    Tip(Fmt("Leave photo mode (%s)", HotKeyName(kKeyPhoto)).c_str());
    ImGui::End();
}

// ---------------------------------------------------------------- main panel

static void DrawPanel() {
    ImGui::SetNextWindowPos(ImVec2(40 * g_scale, 190 * g_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560 * g_scale, 800 * g_scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("FortsGFX", &g_open)) { ImGui::End(); return; }

    GfxStats st = GfxGetStats();
    ImGui::Text("%.0f fps   frame %.2f ms   effects GPU %.2f ms   draw hooks %.2f ms",
                st.frameMs > 0 ? 1000.0f / st.frameMs : 0.0f, st.frameMs, st.gpuMs, st.drawMs);
    float maxMs = 4.0f;
    for (float v : g_frameHist) maxMs = std::max(maxMs, v);
    ImGui::PlotLines("##frame", g_frameHist, kHist, g_histPos, "frame time", 0.0f, maxMs * 1.1f, ImVec2(-1, 50 * g_scale));
    ImGui::PlotLines("##gpu", g_gpuHist, kHist, g_histPos, "effects GPU time", 0.0f, maxMs * 1.1f, ImVec2(-1, 40 * g_scale));
    ImGui::Separator();

    if (ImGui::Button(Fmt("%s the Lab bar (%s)###lab", g_labOpen ? "Close" : "Open", HotKeyName(kKeyLab)).c_str(), ImVec2(-1, 34 * g_scale)))
        g_labOpen = !g_labOpen;
    Tip("Spawn explosions, lightning and lights, compare before/after, try looks, run the showcase tour");
    LookCombo();
    if (int* fxp = (int*)SettingPtr("Presets", "Effects")) {
        static const char* fxNames[] = { "Showcase (over the top)", "Cinematic", "Subtle" };
        int v = *fxp;
        if (ComboCycle("Effects preset", &v, fxNames, 3)) { GfxApplyFx(v); Toast("Effects: %s", fxNames[v]); }
        Tip("How hard explosions hit: shockwaves, shake, flashes, fireballs, debris, impact frames. Looks never change this.");
    }
    if (int* quality = (int*)SettingPtr("Presets", "Quality")) {
        static const char* qualities[] = { "Low", "High", "Ultra" };
        int v = *quality;
        if (ComboCycle("Quality preset", &v, qualities, 3)) GfxApplyQuality(v);
        Tip("Sets the cost-related sliders below (lighting resolution, AA)");
    }
    static const char* views[] = { "None", "Masks (red solid, green playfield, blue light)", "Ray-traced light",
                                   "Sky visibility", "Distance field", "Surface normals", "Light sources (emissive)",
                                   "World before effects" };
    ComboCycle("Debug view", GfxDebugView(), views, 8);
    if (ImGui::Button("Save to ini")) GfxSaveSettings();
    ImGui::SameLine();
    if (ImGui::Button("Reload ini")) GfxReloadSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("%s lab   %s/%s looks   %s effects   %s lighting   Ctrl+Shift+F11 capture", HotKeyName(kKeyLab),
                        HotKeyName(kKeyLookPrev), HotKeyName(kKeyLookNext), HotKeyName(kKeyToggle), HotKeyName(kKeyLighting));
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Keys")) {
        for (int i = 0; i < kHotKeyCount; i++) {
            if (!HotKeyPtr(i)) continue;
            ImGui::PushID(i);
            bool waiting = g_bindSlot == i;
            if (waiting) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.02f, 1.0f));
            }
            if (ImGui::Button(Fmt("%s###bind", waiting ? "press a key..." : HotKeyName(i)).c_str(), ImVec2(170 * g_scale, 0))) {
                if (waiting) g_bindSlot = -1;
                else BindStart(i);
            }
            if (waiting) ImGui::PopStyleColor(2);
            Tip(waiting ? "Press the new key (Esc cancels)" : "Click, then press the key you want");
            ImGui::SameLine();
            ImGui::TextUnformatted(kHotKeys[i].label);
            ImGui::PopID();
        }
        if (ImGui::Button("Default keys")) {
            g_bindSlot = -1;
            for (int i = 0; i < kHotKeyCount; i++) if (int* p = HotKeyPtr(i)) { *p = kHotKeys[i].def; WriteHotKey(i); }
            Toast("Keys back to the defaults");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Forts sees these keys too: pick ones the game doesn't use.");
    }

    const auto& all = GfxSettings();
    static const char* order[] = { "Impact", "Weather", "Backdrop", "Post", "Lighting", "Materials", "Sun", "Fog", "Emitters" };
    static const char* titles[] = { "Impact effects (explosions)", "Weather (rain, lightning)", "3D backdrop",
                                    "Image (bloom, colour, AA)", "Ray-traced lighting", "Materials & surfaces", "Sun",
                                    "Fog & depth", "Light sources" };
    for (int i = 0; i < 9; i++) {
        if (!ImGui::CollapsingHeader(titles[i], i < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
        ImGui::PushItemWidth(-260 * g_scale);
        for (auto& s : all) if (!strcmp(s.section, order[i]) && !strstr(s.key, "KeyVK")) SettingWidget(s);   // keys: Keys section
        ImGui::PopItemWidth();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- per frame

static bool KeyEdge(int vk, bool focused, bool& prev) {
    bool down = focused && vk > 0 && !OverlayKeyBlocked(vk) && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    return edge;
}

void OverlayFrame() {
    GfxStats st = GfxGetStats();
    g_frameHist[g_histPos] = st.frameMs;
    g_gpuHist[g_histPos] = st.gpuMs;
    g_histPos = (g_histPos + 1) % kHist;
    if (g_failed) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool focused = pid == GetCurrentProcessId();
    if (!g_open) g_bindSlot = -1;
    if (g_bindSlot >= 0) BindPoll(focused);
    if (g_swallowVk && !(GetAsyncKeyState(g_swallowVk) & 0x8000)) g_swallowVk = 0;
    static bool pIns, pPrev, pNext, pLab, pPhoto;
    int prevKey = 0, nextKey = 0;
    GfxLookKeys(prevKey, nextKey);
    if (KeyEdge(GfxOverlayKey(), focused, pIns)) {
        if (g_tour) TourStop();
        else {
            g_open = !g_open;
            if (!g_open && !g_labOpen) { g_tool = kToolNone; GfxLabSetLight(0, false, 0, 0, 0, 0, g_lightCol); }
            if (g_init) SessionLog("overlay: panel %s", g_open ? "opened" : "closed");
        }
    }
    if (KeyEdge(GfxPhotoKey(), focused, pPhoto)) { if (g_photo) PhotoExit(); else PhotoEnter(); }
    if (KeyEdge(GfxLabKey(), focused, pLab)) {
        if (g_tour) TourStop();
        g_labOpen = !g_labOpen;
        if (!g_labOpen && !g_open) { g_tool = kToolNone; GfxLabSetLight(0, false, 0, 0, 0, 0, g_lightCol); }
        SessionLog("overlay: lab bar %s", g_labOpen ? "opened" : "closed");
    }
    bool typing = g_init && (g_open || g_labOpen) && ImGui::GetIO().WantTextInput;
    if (!typing && KeyEdge(prevKey, focused, pPrev)) StepLook(-1);
    if (!typing && KeyEdge(nextKey, focused, pNext)) StepLook(+1);
    if (g_cycle && NowS() - g_cycleT > g_cycleSecs) { g_cycleT = NowS(); StepLook(+1); }

    int chapter = g_tourPending.exchange(-2);   // the showcase map moved the tour along
    if (chapter != -2) {
        if (chapter < 0 || chapter >= kTourSteps) TourStop();
        else { if (!g_tour) TourStart(true); TourEnter(chapter); }
    }
    bool need = g_open || g_labOpen || g_photo || g_tour || g_cycle || GfxLabState().wipe >= 0 || NowS() - g_toastT < 2.5;
    if (!need) return;
    if (!g_init) {
        if (!Init()) { g_failed = true; g_open = false; return; }
        g_init = true;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_open || g_photo || (g_labOpen && !g_tour);
    if (g_sweep && GfxLabState().wipe >= 0) GfxLabState().wipe = 0.5f + 0.47f * sinf((float)NowS() * 0.9f);
    TourUpdate(io.DisplaySize.x, io.DisplaySize.y);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (g_photo) DrawPhotoBar(fg);
    else {
        if (g_open) DrawPanel();
        if (g_labOpen && !g_tour) DrawLabBar();
        if (g_open || g_labOpen) LabTools(fg);
    }
    DrawWipeLabels(fg, io.DisplaySize);
    DrawCaption(fg, io.DisplaySize);
    DrawToast(fg, io.DisplaySize);
    ImGui::Render();
    auto bindFb = (void (WINAPI*)(GLenum, GLuint))RealProc("glBindFramebuffer");
    if (bindFb) bindFb(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

#ifdef FORTSGFX_PREVIEW
// replay tool: draw the panel and the lab bar over the bound framebuffer at w x h (no window, no input)
void OverlayPreview(int w, int h, bool panel, bool lab, bool more) {
    static bool inited;
    if (!inited) {
        inited = true;
        imgl3wInit2(LoadGl);
        g_scale = std::max(1.0f, h / 1080.0f);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * g_scale);
        Theme();
        ImGui_ImplOpenGL3_Init("#version 330");
        g_init = true;
    }
    g_open = panel; g_labOpen = lab; g_more = more;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 60.0f;
    for (int i = 0; i < 3; i++) {   // a few frames so auto-sized windows settle
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        if (g_open) DrawPanel();
        if (g_labOpen) DrawLabBar();
        ImGui::Render();
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// replay tool: tour chapter `chapter`, `t` seconds in, as the showcase map would show it. Setup applies
// the chapter's settings (before the frame is rendered); draw adds the caption and the OFF / ON badge.
void OverlayTourSetup(int w, int h, int chapter, double t) {
    if (!g_tour) TourStart(true);
    if (g_step != chapter || NowS() - g_stepT0 > t + 1.0) TourEnter(chapter);
    g_stepT0 = NowS() - t;
    TourUpdate((float)w, (float)h);
}
void OverlayTourDraw(int w, int h) {
    OverlayPreview(w, h, false, false, false);   // makes sure ImGui is set up
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    // "hide HUD" masks colour writes until the swap; in the game they are back on before the overlay draws
    typedef void (WINAPI* ColorMaskFn)(unsigned char, unsigned char, unsigned char, unsigned char);
    if (auto colorMask = (ColorMaskFn)LoadGl("glColorMask")) colorMask(1, 1, 1, 1);
    DrawCaption(fg, io.DisplaySize);
    DrawWipeLabels(fg, io.DisplaySize);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
#endif