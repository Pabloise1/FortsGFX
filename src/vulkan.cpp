// Vulkan (Forts' EnableVulkan = true, in users\<id>\settings_hardware.lua). Forts loads vulkan-1.dll itself and asks it
// for vkGetInstanceProcAddr through GetProcAddress; that call is hooked in Forts.exe's import table, so every Vulkan
// function the game looks up can be wrapped here, from the same opengl32.dll.
//
// Recon (phase V0) watches: vulkan.log gets the instance, device, swapchain, render passes, shaders, pipelines and
// descriptor layouts as they are made, and every 10 s what an average frame does. loading.log: every image (VKIMG
// lines, with the file DevIL just loaded). vk_frame_NN.txt: every command of one frame, the first vertices of each
// draw decoded, and vk_frame_NN.png: that frame as the game drew it - on Ctrl+Shift+F11, and by itself 10 s in and
// then once a minute, at most 8 in all.
//
// The bridge (phase V1) puts FortsGFX on the Vulkan frames: each frame is shared with our own hidden GL context, where
// gfx.cpp's image stages and the control panel run, and the result is copied back before it is presented (see below).
// [Vulkan] Hook=0 in FortsGFX.ini leaves Vulkan alone; Effects=0 keeps the recon but not the bridge.
#include "common.h"
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdarg>
#include <map>
#include <mutex>
#include <unordered_map>

static std::recursive_mutex g_mx;   // the game may create and record from more than one thread
static FILE* g_vlog;

static void VkLog(const char* fmt, ...) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_vlog) g_vlog = _wfopen((g_outDir + L"vulkan.log").c_str(), L"w");
    if (!g_vlog) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_vlog, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_vlog, fmt, ap);
    va_end(ap);
    fputc('\n', g_vlog);
    fflush(g_vlog);
}

static int IniInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"Vulkan", key, def, (g_gameDir + L"FortsGFX.ini").c_str()); }

static double NowS() {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER n;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - t0.QuadPart) / f.QuadPart;
}

static bool SafeRead(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------- names

enum { kImg, kView, kBuf, kPipe, kRp, kFb, kSet, kLayout, kShader, kCmd, kKinds };
static std::unordered_map<uint64_t, int> g_ids[kKinds];
static int g_idNext[kKinds];
template <class H> static int Id(int kind, H h) {
    uint64_t k = (uint64_t)(uintptr_t)h;
    if (!k) return 0;
    auto it = g_ids[kind].find(k);
    return it != g_ids[kind].end() ? it->second : (g_ids[kind][k] = ++g_idNext[kind]);
}

static std::string FormatName(VkFormat f) {
    switch (f) {
#define F(x) case VK_FORMAT_##x: return #x;
    F(UNDEFINED) F(R8_UNORM) F(R8G8_UNORM) F(R8G8B8A8_UNORM) F(R8G8B8A8_SRGB) F(B8G8R8A8_UNORM) F(B8G8R8A8_SRGB)
    F(R8G8B8A8_UINT) F(A2B10G10R10_UNORM_PACK32) F(A2R10G10B10_UNORM_PACK32) F(R16_UINT) F(R16G16_SFLOAT)
    F(R16G16B16A16_SFLOAT) F(R32_UINT) F(R32_SFLOAT) F(R32G32_SFLOAT) F(R32G32B32_SFLOAT) F(R32G32B32A32_SFLOAT)
    F(D16_UNORM) F(D24_UNORM_S8_UINT) F(D32_SFLOAT) F(D32_SFLOAT_S8_UINT) F(S8_UINT)
    F(BC1_RGBA_UNORM_BLOCK) F(BC2_UNORM_BLOCK) F(BC3_UNORM_BLOCK)
#undef F
    default: return Fmt("format%d", (int)f);
    }
}
static int FormatBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 4;
    }
}
static const char* LayoutName(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
    case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATT";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_ATT";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC";
    default: return "other";
    }
}
static const char* LoadOpName(VkAttachmentLoadOp o) { return o == VK_ATTACHMENT_LOAD_OP_LOAD ? "LOAD" : o == VK_ATTACHMENT_LOAD_OP_CLEAR ? "CLEAR" : "DONT_CARE"; }
static const char* StoreOpName(VkAttachmentStoreOp o) { return o == VK_ATTACHMENT_STORE_OP_STORE ? "STORE" : "DONT_CARE"; }
static const char* BlendName(VkBlendFactor b) {
    static const char* n[] = { "0", "1", "sc", "1-sc", "dc", "1-dc", "sa", "1-sa", "da", "1-da", "cc", "1-cc", "ca", "1-ca",
                               "sat", "s1c", "1-s1c", "s1a", "1-s1a" };
    return (unsigned)b < sizeof(n) / sizeof(n[0]) ? n[b] : "?";
}
static const char* TopologyName(VkPrimitiveTopology t) {
    static const char* n[] = { "points", "lines", "line strip", "triangles", "triangle strip", "triangle fan" };
    return (unsigned)t < 6 ? n[t] : "other";
}
static std::string UsageName(VkImageUsageFlags u) {
    static const char* n[] = { "TRANSFER_SRC", "TRANSFER_DST", "SAMPLED", "STORAGE", "COLOR_ATT", "DEPTH_ATT", "TRANSIENT", "INPUT" };
    std::string s;
    for (int i = 0; i < 8; i++) if (u & (1u << i)) s += std::string(s.empty() ? "" : "|") + n[i];
    return s.empty() ? "0" : s;
}

// ---------------------------------------------------------------- what the game made

struct ImgInfo { int id; VkFormat fmt; uint32_t w, h, mips; VkImageUsageFlags usage; std::string name; };
struct BufInfo { VkDeviceSize size; VkBufferUsageFlags usage; VkDeviceMemory mem; VkDeviceSize memOff; };
struct MapInfo { uint8_t* ptr; VkDeviceSize off; };
struct PipeInfo {
    int id; std::string shaders, blend; VkPrimitiveTopology topo; uint32_t stride[4]; std::vector<VkVertexInputAttributeDescription> attrs;
    bool blendOn; VkBlendFactor src, dst;   // colour blend of attachment 0
    uint8_t frag;                           // the fragment shader: 0 texture x colour (frag.spv), 1 colour only, 2 scrolled texture, 3 other
    bool uboMatrix;                         // verts.spv: the matrix is in a uniform buffer (set 1), not push constants
};
struct DescBuf { VkBuffer buf; VkDeviceSize off, range; };
struct DescImg { VkImageView view; VkSampler sampler; };
struct SamplerInfo { VkFilter mag, min; VkSamplerMipmapMode mip; VkSamplerAddressMode u, v; };
struct FbInfo { std::vector<VkImageView> views; uint32_t w, h; };
struct SwapInfo { VkFormat fmt; uint32_t w, h; bool copyable, writable; HWND wnd; std::vector<VkImage> images; };
struct PushRec { VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t off, size; uint8_t data[128]; };
struct RDraw;
struct CmdState {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkBuffer vb[4] = {};
    VkDeviceSize vbOff[4] = {};
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize ibOff = 0;
    VkIndexType ibType = VK_INDEX_TYPE_UINT16;
    std::vector<std::string> lines;   // while a frame is being dumped
    // the state a HUD split carries over into our own command buffer (bound state doesn't cross command buffers)
    bool rendering = false;           // inside the game's vkCmdBeginRendering, as recorded in `ri`
    VkRenderingInfo ri = {};
    std::vector<VkRenderingAttachmentInfo> color;
    VkRenderingAttachmentInfo depth = {}, stencil = {};
    VkPipelineLayout setLayout[4] = {};
    VkDescriptorSet sets[4] = {};
    std::vector<PushRec> pushes;
    float vsPush[16] = {};            // the vertex stage's matrix (first 64 bytes of push constants)
    bool hasVsPush = false;
    VkViewport viewport = {}; VkRect2D scissor = {}; float lineWidth = 1.0f;
    bool hasViewport = false, hasScissor = false, hasLineWidth = false;
    VkCommandBuffer redirect = VK_NULL_HANDLE;   // after the split: where the game's commands for this one go
    int splitSlot = -1;
    bool secondary = false;                      // recorded once and executed from frames (the terrain): its draws kept here
    unsigned version = 0;                        // a new number each time it is recorded
    bool sbroken = false;
    const char* sWhy = nullptr;
};
static std::unordered_map<VkImage, ImgInfo> g_imgs;
static std::unordered_map<VkImageView, VkImage> g_views;
static std::unordered_map<VkBuffer, BufInfo> g_bufs;
static std::unordered_map<VkDeviceMemory, MapInfo> g_maps;
static std::unordered_map<VkPipeline, PipeInfo> g_pipes;
static std::unordered_map<VkShaderModule, std::string> g_shaders;
static std::unordered_map<VkDescriptorSet, std::map<uint32_t, DescImg>> g_sets;
static std::unordered_map<VkSampler, SamplerInfo> g_samplers;
static std::unordered_map<VkDescriptorSet, std::map<uint32_t, DescBuf>> g_setBufs;
static std::unordered_map<VkCommandBuffer, bool> g_cmdSecondary;   // allocated as secondary command buffers
static std::unordered_map<VkRenderPass, std::string> g_rps;
static std::unordered_map<VkFramebuffer, FbInfo> g_fbs;
static std::unordered_map<VkQueue, uint32_t> g_queueFamily;
static std::unordered_map<VkSwapchainKHR, SwapInfo> g_swaps;
static std::unordered_map<VkSurfaceKHR, HWND> g_surfaceWnd;
static std::unordered_map<VkCommandBuffer, CmdState> g_cmds;
static bool g_interopExt;   // the external memory / semaphore extensions are on in the game's device
static std::map<std::string, std::pair<size_t, uint64_t>> g_spv;   // the game's data\shaders\*.spv: size, hash

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;

static std::string ImgDesc(VkImage im) {
    auto it = g_imgs.find(im);
    if (it == g_imgs.end()) return "img?";
    const ImgInfo& i = it->second;
    return Fmt("img#%d %ux%u %s%s%s", i.id, i.w, i.h, FormatName(i.fmt).c_str(), i.name.empty() ? "" : " ", i.name.c_str());
}
static std::string ViewDesc(VkImageView v) {
    auto it = g_views.find(v);
    return it == g_views.end() ? Fmt("view#%d", Id(kView, v)) : ImgDesc(it->second);
}
static const uint8_t* BufPtr(VkBuffer b, VkDeviceSize off) {   // where the CPU sees a buffer, if it is mapped
    auto bi = g_bufs.find(b);
    if (bi == g_bufs.end() || !bi->second.mem) return nullptr;
    auto mi = g_maps.find(bi->second.mem);
    if (mi == g_maps.end()) return nullptr;
    VkDeviceSize at = bi->second.memOff + off;
    return at < mi->second.off ? nullptr : mi->second.ptr + (at - mi->second.off);
}

// ---------------------------------------------------------------- real functions

#define REAL(name) static PFN_##name r_##name
REAL(vkGetInstanceProcAddr); REAL(vkGetDeviceProcAddr);
REAL(vkCreateInstance); REAL(vkCreateDevice); REAL(vkDestroyDevice); REAL(vkGetDeviceQueue); REAL(vkGetDeviceQueue2);
REAL(vkCreateWin32SurfaceKHR); REAL(vkCreateSwapchainKHR); REAL(vkDestroySwapchainKHR); REAL(vkGetSwapchainImagesKHR); REAL(vkQueuePresentKHR);
REAL(vkQueueSubmit); REAL(vkQueueSubmit2); REAL(vkQueueSubmit2KHR);
REAL(vkCreateRenderPass); REAL(vkCreateRenderPass2); REAL(vkCreateRenderPass2KHR); REAL(vkCreateFramebuffer);
REAL(vkCreateShaderModule); REAL(vkCreatePipelineLayout); REAL(vkCreateDescriptorSetLayout); REAL(vkCreateGraphicsPipelines);
REAL(vkCreateImage); REAL(vkDestroyImage); REAL(vkCreateSampler); REAL(vkCreateImageView); REAL(vkCreateBuffer);
REAL(vkBindBufferMemory); REAL(vkBindBufferMemory2); REAL(vkBindBufferMemory2KHR); REAL(vkMapMemory); REAL(vkUnmapMemory);
REAL(vkUpdateDescriptorSets); REAL(vkUpdateDescriptorSetWithTemplate);
REAL(vkBeginCommandBuffer); REAL(vkEndCommandBuffer); REAL(vkAllocateCommandBuffers); REAL(vkFreeCommandBuffers);
REAL(vkCmdBeginRenderPass); REAL(vkCmdEndRenderPass);
REAL(vkCmdBeginRendering); REAL(vkCmdBeginRenderingKHR); REAL(vkCmdEndRendering); REAL(vkCmdEndRenderingKHR);
REAL(vkCmdBindPipeline); REAL(vkCmdBindDescriptorSets); REAL(vkCmdBindVertexBuffers); REAL(vkCmdBindIndexBuffer);
REAL(vkCmdPushConstants); REAL(vkCmdPushDescriptorSetKHR); REAL(vkCmdDraw); REAL(vkCmdDrawIndexed);
REAL(vkCmdSetViewport); REAL(vkCmdSetScissor); REAL(vkCmdCopyBufferToImage); REAL(vkCmdPipelineBarrier);
REAL(vkCmdClearAttachments); REAL(vkCmdBlitImage); REAL(vkCmdCopyImage); REAL(vkCmdSetLineWidth); REAL(vkCmdCopyBuffer); REAL(vkCmdExecuteCommands);
#undef REAL

// Our own device functions for the screenshot, straight from the driver (never through the hooks).
static struct {
    PFN_vkCreateCommandPool CreateCommandPool; PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkCreateBuffer CreateBuffer; PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkBindBufferMemory BindBufferMemory; PFN_vkMapMemory MapMemory;
    PFN_vkCreateSemaphore CreateSemaphore; PFN_vkCreateFence CreateFence; PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences; PFN_vkBeginCommandBuffer BeginCommandBuffer; PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier; PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkQueueSubmit QueueSubmit; PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkDestroyBuffer DestroyBuffer; PFN_vkFreeMemory FreeMemory; PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    bool ok;
    // the bridge
    PFN_vkCreateImage CreateImage; PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkDestroyImage DestroyImage; PFN_vkCmdBlitImage CmdBlitImage; PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkDestroyFence DestroyFence; PFN_vkQueueWaitIdle QueueWaitIdle; PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32HandleKHR; PFN_vkGetSemaphoreWin32HandleKHR GetSemaphoreWin32HandleKHR;
    bool bridge;
} D;

static void ResolveOwn() {
    auto gdpa = r_vkGetDeviceProcAddr ? r_vkGetDeviceProcAddr
                                      : (PFN_vkGetDeviceProcAddr)r_vkGetInstanceProcAddr(g_inst, "vkGetDeviceProcAddr");
    D.ok = gdpa != nullptr;
    if (!D.ok) return;
#define OWN(n) D.n = (PFN_vk##n)gdpa(g_dev, "vk" #n); D.ok = D.ok && D.n
    OWN(CreateCommandPool); OWN(AllocateCommandBuffers); OWN(CreateBuffer); OWN(GetBufferMemoryRequirements);
    OWN(AllocateMemory); OWN(BindBufferMemory); OWN(MapMemory); OWN(CreateSemaphore); OWN(CreateFence);
    OWN(WaitForFences); OWN(ResetFences); OWN(BeginCommandBuffer); OWN(EndCommandBuffer); OWN(CmdPipelineBarrier);
    OWN(CmdCopyImageToBuffer); OWN(QueueSubmit); OWN(ResetCommandBuffer); OWN(DestroyBuffer); OWN(FreeMemory);
    OWN(DestroyCommandPool); OWN(DestroyImage); OWN(DestroySemaphore); OWN(DestroyFence); OWN(DeviceWaitIdle);
    D.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceMemoryProperties");
    D.ok = D.ok && D.GetPhysicalDeviceMemoryProperties;
    if (!D.ok) { VkLog("screenshots and effects off: a device function is missing"); return; }
    D.bridge = g_interopExt;
#define OWN2(n) D.n = (PFN_vk##n)gdpa(g_dev, "vk" #n); D.bridge = D.bridge && D.n
    OWN2(CreateImage); OWN2(GetImageMemoryRequirements); OWN2(BindImageMemory); OWN2(CmdBlitImage); OWN2(QueueWaitIdle);
    OWN2(GetMemoryWin32HandleKHR); OWN2(GetSemaphoreWin32HandleKHR);
#undef OWN2
#undef OWN
}

// ---------------------------------------------------------------- frame statistics and the dump

struct Counts { double submits, cmdBegins, rps, rendering, draws, drawsIdx, verts, pipeBinds, descBinds, pushes, uploads, barriers; };
static Counts g_frame, g_window;
static std::map<VkPipeline, int> g_pipeDraws;   // this summary window
static double g_windowFrames, g_windowMs, g_windowMaxMs, g_lastPresent = -1, g_lastSummary;
static unsigned g_frameNo, g_dumps;
static double g_nextAutoDump = 10.0;
static bool g_dumpRec;            // the commands of this frame are being written down
static std::string g_dumpText;
static unsigned g_dumpFrame;
static const unsigned kMaxDumps = 8;

static void DL(VkCommandBuffer cb, std::string s) { g_cmds[cb].lines.push_back(std::move(s)); }   // g_mx held

static void FlushCmd(const char* how, uint32_t family, VkCommandBuffer cb) {
    auto it = g_cmds.find(cb);
    size_t n = it == g_cmds.end() ? 0 : it->second.lines.size();
    g_dumpText += Fmt("== %s on queue family %u: cmd#%d, %zu commands%s\n", how, family, Id(kCmd, cb), n,
                      n ? "" : " (recorded before this frame - not written down)");
    if (!n) return;
    for (auto& l : it->second.lines) { g_dumpText += "  "; g_dumpText += l; g_dumpText += '\n'; }
    it->second.lines.clear();
}

static std::string DecodeAttr(const uint8_t* p, VkFormat f) {
    float v[4];
    switch (f) {
    case VK_FORMAT_R32_SFLOAT: memcpy(v, p, 4); return Fmt("(%.5g)", v[0]);
    case VK_FORMAT_R32G32_SFLOAT: memcpy(v, p, 8); return Fmt("(%.5g %.5g)", v[0], v[1]);
    case VK_FORMAT_R32G32B32_SFLOAT: memcpy(v, p, 12); return Fmt("(%.5g %.5g %.5g)", v[0], v[1], v[2]);
    case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(v, p, 16); return Fmt("(%.5g %.5g %.5g %.5g)", v[0], v[1], v[2], v[3]);
    case VK_FORMAT_R32_UINT: { uint32_t u; memcpy(&u, p, 4); return Fmt("(%u)", u); }
    default: return Fmt("(%u %u %u %u)", p[0], p[1], p[2], p[3]);   // 8-bit colours and anything else
    }
}

// The first vertices of a draw as the game wrote them (read while it is recorded: the vertex data is written first).
static std::string Vertices(const CmdState& c, const uint32_t* index, uint32_t n) {
    auto pi = g_pipes.find(c.pipe);
    if (pi == g_pipes.end()) return "";
    std::string s;
    for (uint32_t k = 0; k < n; k++) {
        s += Fmt(" | v%u", index[k]);
        for (auto& a : pi->second.attrs) {
            if (a.binding >= 4 || !c.vb[a.binding]) continue;
            const uint8_t* p = BufPtr(c.vb[a.binding], c.vbOff[a.binding] + (VkDeviceSize)index[k] * pi->second.stride[a.binding] + a.offset);
            uint8_t tmp[16];
            if (!p || !SafeRead(tmp, p, FormatBytes(a.format))) { s += " (unmapped)"; break; }
            s += Fmt(" L%u%s", a.location, DecodeAttr(tmp, a.format).c_str());
        }
    }
    return s;
}

// ---------------------------------------------------------------- screenshot of the presented frame

static VkCommandPool g_shotPool;
static uint32_t g_shotFamily = ~0u;
static VkCommandBuffer g_shotCmd;
static VkFence g_shotFence;
static VkSemaphore g_shotSem;
static VkBuffer g_shotBuf;
static VkDeviceMemory g_shotMem;
static VkDeviceSize g_shotSize;
static uint8_t* g_shotPtr;
static uint32_t g_shotW, g_shotH;
static VkFormat g_shotFmt;

static bool ShotResources(uint32_t family, VkDeviceSize bytes) {
    if (g_shotFamily != family) {
        if (g_shotPool) D.DestroyCommandPool(g_dev, g_shotPool, nullptr);
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family };
        if (D.CreateCommandPool(g_dev, &pci, nullptr, &g_shotPool) != VK_SUCCESS) { g_shotPool = VK_NULL_HANDLE; return false; }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_shotPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        if (D.AllocateCommandBuffers(g_dev, &ai, &g_shotCmd) != VK_SUCCESS) return false;
        *(void**)g_shotCmd = *(void**)g_dev;   // a command buffer we allocate ourselves needs the loader's dispatch table
        g_shotFamily = family;
    }
    if (!g_shotFence) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (D.CreateFence(g_dev, &fci, nullptr, &g_shotFence) != VK_SUCCESS) return false;
        if (D.CreateSemaphore(g_dev, &sci, nullptr, &g_shotSem) != VK_SUCCESS) return false;
    }
    if (g_shotSize < bytes) {
        if (g_shotBuf) { D.DestroyBuffer(g_dev, g_shotBuf, nullptr); D.FreeMemory(g_dev, g_shotMem, nullptr); g_shotBuf = VK_NULL_HANDLE; g_shotSize = 0; }
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
        if (D.CreateBuffer(g_dev, &bci, nullptr, &g_shotBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        D.GetBufferMemoryRequirements(g_dev, g_shotBuf, &req);
        VkPhysicalDeviceMemoryProperties mp;
        D.GetPhysicalDeviceMemoryProperties(g_phys, &mp);
        uint32_t type = ~0u;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == ~0u; i++)
            if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
        if (type == ~0u) return false;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type };
        if (D.AllocateMemory(g_dev, &mai, nullptr, &g_shotMem) != VK_SUCCESS) return false;
        if (D.BindBufferMemory(g_dev, g_shotBuf, g_shotMem, 0) != VK_SUCCESS) return false;
        if (D.MapMemory(g_dev, g_shotMem, 0, VK_WHOLE_SIZE, 0, (void**)&g_shotPtr) != VK_SUCCESS) return false;
        g_shotSize = bytes;
    }
    return true;
}

// Copies the image about to be presented into our buffer, after everything the present waits for. The present then
// waits for the copy instead.
static bool ShotStart(VkQueue q, const VkPresentInfoKHR* pi, VkPresentInfoKHR& mod) {
    if (!D.ok || pi->swapchainCount < 1) return false;
    auto sw = g_swaps.find(pi->pSwapchains[0]);
    if (sw == g_swaps.end() || !sw->second.copyable || pi->pImageIndices[0] >= sw->second.images.size()) return false;
    const SwapInfo& s = sw->second;
    if (FormatBytes(s.fmt) != 4) return false;
    auto qf = g_queueFamily.find(q);
    if (qf == g_queueFamily.end() || !ShotResources(qf->second, (VkDeviceSize)s.w * s.h * 4)) return false;
    VkImage img = s.images[pi->pImageIndices[0]];

    D.ResetCommandBuffer(g_shotCmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    D.BeginCommandBuffer(g_shotCmd, &bi);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_READ_BIT,
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    D.CmdPipelineBarrier(g_shotCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy r = {};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = { s.w, s.h, 1 };
    D.CmdCopyImageToBuffer(g_shotCmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shotBuf, 1, &r);
    b.srcAccessMask = 0; b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_shotBuf, 0, VK_WHOLE_SIZE };
    D.CmdPipelineBarrier(g_shotCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &bb, 1, &b);
    D.EndCommandBuffer(g_shotCmd);

    std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, pi->waitSemaphoreCount, pi->pWaitSemaphores, stages.data(),
                        1, &g_shotCmd, 1, &g_shotSem };
    if (D.QueueSubmit(q, 1, &si, g_shotFence) != VK_SUCCESS) return false;
    mod = *pi;
    mod.waitSemaphoreCount = 1;
    mod.pWaitSemaphores = &g_shotSem;
    g_shotW = s.w; g_shotH = s.h; g_shotFmt = s.fmt;
    return true;
}

static void ShotFinish(unsigned n) {
    if (D.WaitForFences(g_dev, 1, &g_shotFence, VK_TRUE, 2000000000ull) != VK_SUCCESS) { VkLog("screenshot: the copy did not finish"); return; }
    D.ResetFences(g_dev, 1, &g_shotFence);
    std::vector<uint8_t> px((size_t)g_shotW * g_shotH * 4);
    memcpy(px.data(), g_shotPtr, px.size());
    bool bgr = g_shotFmt == VK_FORMAT_B8G8R8A8_UNORM || g_shotFmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (bgr) for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    CapSaveRgbaPng(g_outDir + Widen(Fmt("vk_frame_%02u.png", n)), (int)g_shotW, (int)g_shotH, std::move(px));
}

// ---------------------------------------------------------------- the bridge: FortsGFX on the Vulkan frames
// Every frame, the image about to be presented is blitted (flipped, so GL sees it the right way up) into IN, an image
// whose memory is shared with our own hidden GL context. There gfx.cpp runs its image stages from IN into OUT, the
// second shared image, and the control panel draws over OUT; then OUT is blitted back (flipped again) into the
// swapchain image and that is presented. Semaphores shared between the two APIs order it on the GPU: the game's
// render -> [Vulkan: copy in] -A-> [GL: effects, panel] -B-> [Vulkan: copy back] -C-> present. Nothing waits on the
// CPU except for a command buffer slot from three frames ago. Any failure turns the bridge off for the session.

enum : GLenum {   // GL_EXT_memory_object(_win32) / GL_EXT_semaphore(_win32) and the few GL names used here
    kGlTex2D = 0x0DE1, kGlRgba8 = 0x8058, kGlFramebuffer = 0x8D40, kGlReadFb = 0x8CA8, kGlDrawFb = 0x8CA9,
    kGlColor0 = 0x8CE0, kGlFbComplete = 0x8CD5, kGlColorBit = 0x4000, kGlNearest = 0x2600, kGlLinear = 0x2601,
    kGlMinFilter = 0x2801, kGlMagFilter = 0x2800, kGlScissorTest = 0x0C11,
    kGlTilingExt = 0x9580, kGlOptimalTilingExt = 0x9584, kGlDedicatedExt = 0x9581, kGlHandleOpaqueWin32 = 0x9587,
    kGlDeviceUuid = 0x9597, kGlLayoutTransferSrc = 0x9592, kGlLayoutTransferDst = 0x9593,
    kGlSrgb8Alpha8 = 0x8C43, kGlSrgbDecodeExt = 0x8A48, kGlSkipDecodeExt = 0x8A4A,
};
static struct {
    void (WINAPI* CreateMemoryObjectsEXT)(GLsizei, GLuint*);
    void (WINAPI* DeleteMemoryObjectsEXT)(GLsizei, const GLuint*);
    void (WINAPI* MemoryObjectParameterivEXT)(GLuint, GLenum, const GLint*);
    void (WINAPI* ImportMemoryWin32HandleEXT)(GLuint, unsigned long long, GLenum, void*);
    void (WINAPI* TexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, unsigned long long);
    void (WINAPI* GenSemaphoresEXT)(GLsizei, GLuint*);
    void (WINAPI* DeleteSemaphoresEXT)(GLsizei, const GLuint*);
    void (WINAPI* ImportSemaphoreWin32HandleEXT)(GLuint, GLenum, void*);
    void (WINAPI* WaitSemaphoreEXT)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    void (WINAPI* SignalSemaphoreEXT)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    void (WINAPI* GetUnsignedBytei_vEXT)(GLenum, GLuint, GLubyte*);
    void (WINAPI* GenTextures)(GLsizei, GLuint*);
    void (WINAPI* DeleteTextures)(GLsizei, const GLuint*);
    void (WINAPI* BindTexture)(GLenum, GLuint);
    void (WINAPI* TexParameteri)(GLenum, GLenum, GLint);
    void (WINAPI* GenFramebuffers)(GLsizei, GLuint*);
    void (WINAPI* DeleteFramebuffers)(GLsizei, const GLuint*);
    void (WINAPI* BindFramebuffer)(GLenum, GLuint);
    void (WINAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (WINAPI* CheckFramebufferStatus)(GLenum);
    void (WINAPI* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (WINAPI* Enable)(GLenum);
    void (WINAPI* Disable)(GLenum);
    void (WINAPI* Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (WINAPI* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (WINAPI* Clear)(GLbitfield);
    void (WINAPI* Flush)();
    GLenum (WINAPI* GetError)();
    HGLRC (WINAPI* CreateContext)(HDC);
    BOOL (WINAPI* MakeCurrent)(HDC, HGLRC);
} GL;

static struct {
    bool tried, failed;
    HWND glWnd; HDC glDC; HGLRC glRC;                 // our hidden GL context
    VkSemaphore semA, semB, semC; GLuint glSemA, glSemB;
    VkCommandPool pool; uint32_t family = ~0u;
    VkCommandBuffer cmd1[3], cmd2[3]; VkFence fence[3]; int slot;                    // the present pass, three slots
    VkCommandBuffer cmdIn[3], cmdOut[3], cmdHud[3]; VkFence fenceS[3]; int slotS;  // the HUD split, three slots
    bool splitLogged;
    uint32_t w, h;                                    // the shared images, 0 = none yet
    bool srgb;                                        // ... in an sRGB format, as the swapchain is
    VkImage img[2]; VkDeviceMemory mem[2]; GLuint glMem[2], glTex[2], glFbo[2];
    bool outKnown;                                    // OUT has been given a layout
    bool testMark, shotAfter;                         // [Vulkan] TestMark: an orange square top left; ShotAfterEffects
    unsigned frames;
} B;

static void BridgeFail(const char* why) {
    B.failed = true;
    VkLog("effects off: %s", why);
    SessionLog("vulkan: FortsGFX effects off - %s", why);
}

// Our current GL context for the duration of a scope, the caller's back afterwards.
struct GlCurrent {
    HDC dc; HGLRC rc; bool ok;
    GlCurrent() : dc(R.wglGetCurrentDC()), rc(R.wglGetCurrentContext()), ok(GL.MakeCurrent(B.glDC, B.glRC) != FALSE) {}
    ~GlCurrent() { GL.MakeCurrent(dc, rc); }
};

// The hidden window and GL context, once per session (they outlive a device the game remakes).
static bool BridgeGlContext() {
    if (B.glRC) return true;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FortsGFXVulkanBridge";
    RegisterClassW(&wc);
    B.glWnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    B.glDC = B.glWnd ? GetDC(B.glWnd) : nullptr;
    if (!B.glDC) { BridgeFail("could not make a window for GL"); return false; }
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    int pf = ChoosePixelFormat(B.glDC, &pfd);
    if (!pf || !SetPixelFormat(B.glDC, pf, &pfd)) { BridgeFail("no GL pixel format"); return false; }
    GL.CreateContext = (decltype(GL.CreateContext))RealProc("wglCreateContext");
    GL.MakeCurrent = (decltype(GL.MakeCurrent))RealProc("wglMakeCurrent");
    B.glRC = GL.CreateContext ? GL.CreateContext(B.glDC) : nullptr;
    if (!B.glRC) { BridgeFail("could not make a GL context"); return false; }
    GlCurrent cur;
    if (!cur.ok) { BridgeFail("could not use the GL context"); return false; }
    LoadRealExt();   // capture.cpp's reads (photo mode) in this context
    bool ok = true;
#define P(n) GL.n = (decltype(GL.n))RealProc("gl" #n); ok = ok && GL.n
    P(CreateMemoryObjectsEXT); P(DeleteMemoryObjectsEXT); P(MemoryObjectParameterivEXT); P(ImportMemoryWin32HandleEXT);
    P(TexStorageMem2DEXT); P(GenSemaphoresEXT); P(DeleteSemaphoresEXT); P(ImportSemaphoreWin32HandleEXT); P(WaitSemaphoreEXT);
    P(SignalSemaphoreEXT); P(GetUnsignedBytei_vEXT); P(GenTextures); P(DeleteTextures); P(BindTexture); P(TexParameteri);
    P(GenFramebuffers); P(DeleteFramebuffers); P(BindFramebuffer); P(FramebufferTexture2D); P(CheckFramebufferStatus);
    P(BlitFramebuffer); P(Enable); P(Disable); P(Scissor); P(ClearColor); P(Clear); P(Flush); P(GetError);
#undef P
    const char* renderer = (const char*)R.GetString(0x1F01);
    VkLog("effects: GL context on %s", renderer ? renderer : "?");
    if (!ok) { BridgeFail("the GL driver can't share images with Vulkan (GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32)"); return false; }
    return true;
}

// Per device: the GPUs must match, and the semaphores are made and shared.
static bool BridgeGlInit() {
    if (!BridgeGlContext()) return false;
    GlCurrent cur;

    // GL and Vulkan must be on the same GPU (laptops have two)
    VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
    auto gpp2 = (PFN_vkGetPhysicalDeviceProperties2)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties2");
    if (!gpp2) gpp2 = (PFN_vkGetPhysicalDeviceProperties2)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties2KHR");
    GLubyte glUuid[16] = {};
    GL.GetUnsignedBytei_vEXT(kGlDeviceUuid, 0, glUuid);
    if (gpp2) {
        gpp2(g_phys, &p2);
        if (memcmp(idp.deviceUUID, glUuid, 16) != 0) { BridgeFail("GL and Vulkan are on different GPUs"); return false; }
        VkLog("effects: GL and Vulkan are on the same GPU");
    } else VkLog("effects: could not compare the GPUs of GL and Vulkan (no vkGetPhysicalDeviceProperties2)");

    // the semaphores A (Vulkan -> GL) and B (GL -> Vulkan) are shared, C (-> present) is Vulkan's own
    VkSemaphoreCreateInfo plain = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkExportSemaphoreCreateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT };
    VkSemaphoreCreateInfo shared = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exp };
    VkSemaphore* vs[2] = { &B.semA, &B.semB };
    GLuint* gs[2] = { &B.glSemA, &B.glSemB };
    for (int i = 0; i < 2; i++) {
        if (D.CreateSemaphore(g_dev, &shared, nullptr, vs[i]) != VK_SUCCESS) { BridgeFail("could not make a shared semaphore"); return false; }
        VkSemaphoreGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, *vs[i], VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT };
        HANDLE h = nullptr;
        if (D.GetSemaphoreWin32HandleKHR(g_dev, &gi, &h) != VK_SUCCESS || !h) { BridgeFail("could not share a semaphore"); return false; }
        GL.GenSemaphoresEXT(1, gs[i]);
        GL.ImportSemaphoreWin32HandleEXT(*gs[i], kGlHandleOpaqueWin32, h);
        CloseHandle(h);   // importing doesn't take the handle over
    }
    if (D.CreateSemaphore(g_dev, &plain, nullptr, &B.semC) != VK_SUCCESS) { BridgeFail("could not make a semaphore"); return false; }
    if (GLenum e = GL.GetError()) { BridgeFail(Fmt("GL error 0x%x sharing the semaphores", e).c_str()); return false; }
    B.testMark = IniInt(L"TestMark", 0) != 0;
    B.shotAfter = IniInt(L"ShotAfterEffects", 0) != 0;
    return true;
}

static bool BridgeCommands(uint32_t family) {
    if (B.family == family) return true;
    if (B.pool) { BridgeFail("the present queue changed family"); return false; }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family };
    if (D.CreateCommandPool(g_dev, &pci, nullptr, &B.pool) != VK_SUCCESS) { BridgeFail("could not make a command pool"); return false; }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, B.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3 };
    VkCommandBuffer* sets[5] = { B.cmd1, B.cmd2, B.cmdIn, B.cmdOut, B.cmdHud };
    for (VkCommandBuffer* s : sets) {
        if (D.AllocateCommandBuffers(g_dev, &ai, s) != VK_SUCCESS) { BridgeFail("could not make command buffers"); return false; }
        for (int i = 0; i < 3; i++) *(void**)s[i] = *(void**)g_dev;   // our own command buffers need the loader's dispatch table
    }
    for (int i = 0; i < 3; i++) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
        if (D.CreateFence(g_dev, &fci, nullptr, &B.fence[i]) != VK_SUCCESS || D.CreateFence(g_dev, &fci, nullptr, &B.fenceS[i]) != VK_SUCCESS) {
            BridgeFail("could not make a fence");
            return false;
        }
    }
    B.family = family;
    return true;
}

static void BridgeFreeImages() {
    if (!B.w) return;
    D.DeviceWaitIdle(g_dev);
    {
        GlCurrent cur;
        GL.DeleteFramebuffers(2, B.glFbo);
        GL.DeleteTextures(2, B.glTex);
        GL.DeleteMemoryObjectsEXT(2, B.glMem);
    }
    for (int i = 0; i < 2; i++) { D.DestroyImage(g_dev, B.img[i], nullptr); D.FreeMemory(g_dev, B.mem[i], nullptr); }
    B.w = B.h = 0;
    B.outKnown = false;
}

// IN and OUT, the swapchain's size. With an sRGB swapchain they are sRGB too, so the blits keep the game's values as
// they are, and GL reads and writes them raw (no decoding, no GL_FRAMEBUFFER_SRGB): FortsGFX sees what it sees in GL.
static bool BridgeImages(uint32_t w, uint32_t h, bool srgb) {
    if (B.w == w && B.h == h && B.srgb == srgb) return true;
    BridgeFreeImages();
    VkPhysicalDeviceMemoryProperties mp;
    D.GetPhysicalDeviceMemoryProperties(g_phys, &mp);
    GlCurrent cur;
    for (int i = 0; i < 2; i++) {
        VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT };
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D,
                                  srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, { w, h, 1 }, 1, 1,
                                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED };
        if (D.CreateImage(g_dev, &ici, nullptr, &B.img[i]) != VK_SUCCESS) { BridgeFail("could not make a shared image"); return false; }
        VkMemoryRequirements req;
        D.GetImageMemoryRequirements(g_dev, B.img[i], &req);
        uint32_t type = ~0u;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == ~0u; t++)
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
        VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT };
        VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &exp, B.img[i], VK_NULL_HANDLE };
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, req.size, type };
        if (type == ~0u || D.AllocateMemory(g_dev, &mai, nullptr, &B.mem[i]) != VK_SUCCESS || D.BindImageMemory(g_dev, B.img[i], B.mem[i], 0) != VK_SUCCESS) {
            BridgeFail("could not give a shared image memory");
            return false;
        }
        VkMemoryGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, B.mem[i], VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT };
        HANDLE hnd = nullptr;
        if (D.GetMemoryWin32HandleKHR(g_dev, &gi, &hnd) != VK_SUCCESS || !hnd) { BridgeFail("could not share an image's memory"); return false; }
        while (GL.GetError()) {}
        GLenum e1, e2, e3, e4;
        GL.CreateMemoryObjectsEXT(1, &B.glMem[i]);
        GLint dedicated = 1;
        GL.MemoryObjectParameterivEXT(B.glMem[i], kGlDedicatedExt, &dedicated);
        e1 = GL.GetError();
        GL.ImportMemoryWin32HandleEXT(B.glMem[i], req.size, kGlHandleOpaqueWin32, hnd);
        e2 = GL.GetError();
        CloseHandle(hnd);
        GL.GenTextures(1, &B.glTex[i]);
        GL.BindTexture(kGlTex2D, B.glTex[i]);
        GL.TexParameteri(kGlTex2D, kGlTilingExt, kGlOptimalTilingExt);
        GL.TexParameteri(kGlTex2D, kGlMinFilter, kGlLinear);
        GL.TexParameteri(kGlTex2D, kGlMagFilter, kGlLinear);
        e3 = GL.GetError();
        GL.TexStorageMem2DEXT(kGlTex2D, 1, srgb ? kGlSrgb8Alpha8 : kGlRgba8, (GLsizei)w, (GLsizei)h, B.glMem[i], 0);
        e4 = GL.GetError();
        if (srgb) GL.TexParameteri(kGlTex2D, kGlSrgbDecodeExt, kGlSkipDecodeExt);
        GL.GenFramebuffers(1, &B.glFbo[i]);
        GL.BindFramebuffer(kGlFramebuffer, B.glFbo[i]);
        GL.FramebufferTexture2D(kGlFramebuffer, kGlColor0, kGlTex2D, B.glTex[i], 0);
        GLenum status = GL.CheckFramebufferStatus(kGlFramebuffer);
        GL.BindFramebuffer(kGlFramebuffer, 0);
        GL.BindTexture(kGlTex2D, 0);
        if (status != kGlFbComplete || e1 || e2 || e3 || e4) {
            VkLog("effects: shared image %d (%llu bytes): GL errors 0x%x / import 0x%x / parameters 0x%x / storage 0x%x, framebuffer 0x%x",
                  i, (unsigned long long)req.size, e1, e2, e3, e4, status);
            if (status != kGlFbComplete) { BridgeFail("a shared image can't be drawn into in GL"); return false; }
        }
    }
    if (GLenum e = GL.GetError()) { BridgeFail(Fmt("GL error 0x%x sharing the images", e).c_str()); return false; }
    B.w = w; B.h = h; B.srgb = srgb;
    VkLog("effects: shared images %ux%u%s ready", w, h, srgb ? " sRGB" : "");
    return true;
}

static void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA,
                    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, srcA, dstA, from, to, VK_QUEUE_FAMILY_IGNORED,
                               VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    D.CmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}
static void BlitFlipped(VkCommandBuffer cb, VkImage src, VkImage dst, int32_t w, int32_t h) {
    VkImageBlit r = {};
    r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.srcOffsets[1] = { w, h, 1 };
    r.dstOffsets[0] = { 0, h, 0 };
    r.dstOffsets[1] = { w, 0, 1 };
    D.CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
}

// Sets the bridge up (once) and the shared images for this swapchain. False: no effects.
static bool BridgeEnsure(uint32_t family, const SwapInfo& s) {
    if (B.failed) return false;
    if (!B.tried) {
        B.tried = true;
        if (!IniInt(L"Effects", 1)) { B.failed = true; VkLog("effects: [Vulkan] Effects=0"); return false; }
        if (!D.ok || !D.bridge) { BridgeFail("the Vulkan driver can't share images with GL"); return false; }
        if (!BridgeGlInit()) return false;
        VkLog("effects: bridge ready");
        SessionLog("vulkan: FortsGFX effects on (image stages and the control panel)");
    }
    bool srgb = s.fmt == VK_FORMAT_B8G8R8A8_SRGB || s.fmt == VK_FORMAT_R8G8B8A8_SRGB;
    if (!srgb && s.fmt != VK_FORMAT_B8G8R8A8_UNORM && s.fmt != VK_FORMAT_R8G8B8A8_UNORM) {
        BridgeFail(Fmt("swapchain format %s not handled yet", FormatName(s.fmt).c_str()).c_str());
        return false;
    }
    if (!s.copyable || !s.writable) { BridgeFail("the swapchain can't be copied to and from"); return false; }
    return BridgeCommands(family) && BridgeImages(s.w, s.h, srgb);
}

static bool ReplayOn();
static void MirrorFlush();
static bool Replay(int32_t w, int32_t h);

// GL's part, between semaphores A and B: the world drawn again in GL through FortsGFX (`replay`, at the HUD split), or
// IN -> FortsGFX's image stages, or IN as it was -> OUT; then the control panel over OUT.
static void GlStage(bool effects, bool panel, int32_t w, int32_t h, bool replay = false) {
    GlCurrent cur;
    MirrorFlush();
    const GLenum inLayouts[2] = { kGlLayoutTransferDst, kGlLayoutTransferSrc };
    GL.WaitSemaphoreEXT(B.glSemA, 0, nullptr, 2, B.glTex, inLayouts);
    bool done = effects && replay && ReplayOn() && Replay(w, h);
    if (!done && effects) done = GfxBridgeFrame(B.glTex[0], B.glFbo[1], w, h);
    if (!done) {   // only the panel, or FortsGFX off (F9)
        GL.BindFramebuffer(kGlReadFb, B.glFbo[0]);
        GL.BindFramebuffer(kGlDrawFb, B.glFbo[1]);
        GL.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, kGlColorBit, kGlNearest);
        GL.BindFramebuffer(kGlFramebuffer, 0);
    }
    if (effects) CapBridgePhoto(B.glFbo[1]);
    if (panel) OverlayBridgeDraw(B.glFbo[1]);
    if (B.testMark) {   // tools/vktest: which way up the round trip comes out
        GL.BindFramebuffer(kGlFramebuffer, B.glFbo[1]);
        GL.Enable(kGlScissorTest);
        GL.Scissor(16, h - 80, 64, 64);
        GL.ClearColor(1.0f, 0.55f, 0.1f, 1.0f);
        GL.Clear(kGlColorBit);
        GL.Disable(kGlScissorTest);
        GL.BindFramebuffer(kGlFramebuffer, 0);
    }
    const GLenum outLayouts[2] = { kGlLayoutTransferDst, kGlLayoutTransferSrc };
    GL.SignalSemaphoreEXT(B.glSemB, 0, nullptr, 2, B.glTex, outLayouts);
    GL.Flush();
    if (effects && ++B.frames == 1) {
        VkLog("effects: first frame through FortsGFX");
        OverlayToast("FortsGFX on Vulkan: looks, bloom and the control panel");
    }
}

// The present pass: the image about to be presented goes through GL and back - with FortsGFX over all of it when the
// frame isn't split at the HUD, and/or the control panel. `mod` is then what to present (it waits for the copy back).
static bool BridgeFrame(VkQueue q, const VkPresentInfoKHR* pi, VkPresentInfoKHR& mod, bool effects, bool panel) {
    if (B.failed || pi->swapchainCount != 1 || (!effects && !panel)) return false;
    SwapInfo s;
    uint32_t family;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto sw = g_swaps.find(pi->pSwapchains[0]);
        auto qf = g_queueFamily.find(q);
        if (sw == g_swaps.end() || qf == g_queueFamily.end() || pi->pImageIndices[0] >= sw->second.images.size()) return false;
        s = sw->second;
        family = qf->second;
    }
    if (!BridgeEnsure(family, s)) return false;
    VkImage swapImg = s.images[pi->pImageIndices[0]];
    int32_t w = (int32_t)s.w, h = (int32_t)s.h;
    int k = B.slot = (B.slot + 1) % 3;
    D.WaitForFences(g_dev, 1, &B.fence[k], VK_TRUE, 2000000000ull);
    D.ResetFences(g_dev, 1, &B.fence[k]);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

    // 1. the game's image -> IN
    VkCommandBuffer c1 = B.cmd1[k];
    D.ResetCommandBuffer(c1, 0);
    D.BeginCommandBuffer(c1, &bi);
    Barrier(c1, swapImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(c1, B.img[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (!B.outKnown) {
        Barrier(c1, B.img[1], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        B.outKnown = true;
    }
    BlitFlipped(c1, swapImg, B.img[0], w, h);
    D.EndCommandBuffer(c1);
    std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo s1 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, pi->waitSemaphoreCount, pi->pWaitSemaphores, stages.data(), 1, &c1, 1, &B.semA };
    if (D.QueueSubmit(q, 1, &s1, VK_NULL_HANDLE) != VK_SUCCESS) { BridgeFail("a submit failed"); return false; }

    // 2. GL
    GlStage(effects, panel, w, h);

    // 3. OUT -> the game's image, which is then presented
    VkCommandBuffer c2 = B.cmd2[k];
    D.ResetCommandBuffer(c2, 0);
    D.BeginCommandBuffer(c2, &bi);
    Barrier(c2, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    BlitFlipped(c2, B.img[1], swapImg, w, h);
    Barrier(c2, swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    D.EndCommandBuffer(c2);
    VkPipelineStageFlags st2 = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &B.semB, &st2, 1, &c2, 1, &B.semC };
    if (D.QueueSubmit(q, 1, &s2, B.fence[k]) != VK_SUCCESS) { BridgeFail("a submit failed"); return false; }
    mod = *pi;
    mod.waitSemaphoreCount = 1;
    mod.pWaitSemaphores = &B.semC;
    return true;
}

// ---------------------------------------------------------------- the HUD split
// In a battle Forts draws the background, then the forts, then - in the same pass as the last world sprites - the HUD:
// sprites from ui/ under a fixed screen matrix (a virtual screen 600 units high with its origin top left: scale
// (2/width, -2/600), offset (-1, 1)), then text and the cursor. At the first such draw, once the playfield has been
// drawn this frame, the game's pass is ended there and the rest of that command buffer's commands are recorded into our
// cmdHud instead, with the pass begun again on the same attachments (loaded) and the bound state carried over. When the
// game submits it: the part before runs and is copied into IN (signals A), GL runs FortsGFX into OUT (A -> B), then
// OUT is copied back and cmdHud draws the HUD over it (waits for B), with the game's own semaphores and fence where
// they were. So FortsGFX goes under the HUD, as in GL. [Vulkan] SplitHud=0: over the whole frame at present instead.

static int g_redirects;                 // game command buffers whose commands are going to our cmdHud
static bool g_worldSeen, g_splitDone;   // this frame: the playfield has been drawn; the frame has been split
static uint32_t g_mainFamily = ~0u;     // the game's queue family

static bool SplitOn() {
    static int on = -1;
    if (on < 0) on = IniInt(L"Effects", 1) && IniInt(L"SplitHud", 1);
    return on && !B.failed;
}

static std::string DrawTexture(const CmdState& c) {   // what binding 0 of set 0 shows: the sprite's image file
    auto s = g_sets.find(c.sets[0]);
    if (s == g_sets.end()) return "";
    auto b = s->second.find(0);
    if (b == s->second.end()) return "";
    auto v = g_views.find(b->second.view);
    if (v == g_views.end()) return "";
    auto i = g_imgs.find(v->second);
    if (i == g_imgs.end()) return "";
    std::string n = i->second.name;
    for (char& ch : n) ch = (char)tolower((unsigned char)ch);
    std::replace(n.begin(), n.end(), '\\', '/');
    return n;
}
static bool IsPlayfield(const std::string& t) {   // the map's backdrop or the forts: this is a battle
    return t.find("environment/") != std::string::npos || t.find("materials/") != std::string::npos ||
           t.find("devices/") != std::string::npos || t.find("weapons/") != std::string::npos;
}
static bool IsHudStart(const CmdState& c, const std::string& t) {
    const float* m = c.vsPush;
    return c.hasVsPush && t.compare(0, 3, "ui/") == 0 && m[0] > 0.0f && fabsf(m[5] + 1.0f / 300.0f) < 2e-6f &&
           fabsf(m[12] + 1.0f) < 1e-4f && fabsf(m[13] - 1.0f) < 1e-4f;
}

static VkCommandBuffer Redir(VkCommandBuffer cb) {   // where a game command for `cb` goes
    if (!g_redirects) return cb;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_cmds.find(cb);
    return it != g_cmds.end() && it->second.redirect ? it->second.redirect : cb;
}

static void ReplayState(VkCommandBuffer to, const CmdState& c) {
    if (c.hasViewport) r_vkCmdSetViewport(to, 0, 1, &c.viewport);
    if (c.hasScissor) r_vkCmdSetScissor(to, 0, 1, &c.scissor);
    if (r_vkCmdSetLineWidth) r_vkCmdSetLineWidth(to, c.hasLineWidth ? c.lineWidth : 1.0f);
    if (c.pipe) r_vkCmdBindPipeline(to, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipe);
    for (uint32_t i = 0; i < 4; i++)
        if (c.sets[i]) r_vkCmdBindDescriptorSets(to, VK_PIPELINE_BIND_POINT_GRAPHICS, c.setLayout[i], i, 1, &c.sets[i], 0, nullptr);
    for (uint32_t i = 0; i < 4; i++)
        if (c.vb[i]) r_vkCmdBindVertexBuffers(to, i, 1, &c.vb[i], &c.vbOff[i]);
    if (c.ib && r_vkCmdBindIndexBuffer) r_vkCmdBindIndexBuffer(to, c.ib, c.ibOff, c.ibType);
    for (const PushRec& p : c.pushes) r_vkCmdPushConstants(to, p.layout, p.stages, p.off, p.size, p.data);
}

static uint32_t g_splitW, g_splitH;

// At the first HUD draw: ends the game's pass in `cb` and records the copies and our cmdHud for slot k. g_mx held.
static bool Split(VkCommandBuffer cb, CmdState& c) {
    if (c.color.empty() || !r_vkCmdBeginRendering && !r_vkCmdBeginRenderingKHR) return false;
    auto v = g_views.find(c.color[0].imageView);
    if (v == g_views.end()) return false;
    VkImage target = v->second;
    const SwapInfo* sw = nullptr;
    for (auto& s : g_swaps)
        for (VkImage im : s.second.images) if (im == target) sw = &s.second;
    if (!sw) {
        if (!B.splitLogged) { B.splitLogged = true; VkLog("effects: the HUD is drawn into an image that isn't presented - not split"); }
        return false;
    }
    if (g_mainFamily == ~0u || !BridgeEnsure(g_mainFamily, *sw)) return false;
    int32_t w = (int32_t)sw->w, h = (int32_t)sw->h;
    int k = B.slotS = (B.slotS + 1) % 3;
    D.WaitForFences(g_dev, 1, &B.fenceS[k], VK_TRUE, 2000000000ull);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkImageLayout drawn = c.color[0].imageLayout;

    // the world -> IN
    VkCommandBuffer ci = B.cmdIn[k];
    D.ResetCommandBuffer(ci, 0);
    D.BeginCommandBuffer(ci, &bi);
    Barrier(ci, target, drawn, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(ci, B.img[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (!B.outKnown) {
        Barrier(ci, B.img[1], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        B.outKnown = true;
    }
    BlitFlipped(ci, target, B.img[0], w, h);
    D.EndCommandBuffer(ci);

    // OUT -> the image, ready to be drawn on again
    VkCommandBuffer co = B.cmdOut[k];
    D.ResetCommandBuffer(co, 0);
    D.BeginCommandBuffer(co, &bi);
    Barrier(co, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    BlitFlipped(co, B.img[1], target, w, h);
    Barrier(co, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, drawn, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    D.EndCommandBuffer(co);

    // the game's pass ends here; the HUD's begins in cmdHud on the same attachments, as they are
    (r_vkCmdEndRendering ? r_vkCmdEndRendering : r_vkCmdEndRenderingKHR)(cb);
    VkCommandBuffer hud = B.cmdHud[k];
    D.ResetCommandBuffer(hud, 0);
    D.BeginCommandBuffer(hud, &bi);
    std::vector<VkRenderingAttachmentInfo> col = c.color;
    for (auto& a : col) { a.pNext = nullptr; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; }
    VkRenderingAttachmentInfo dep = c.depth, sten = c.stencil;
    dep.pNext = sten.pNext = nullptr;
    dep.loadOp = sten.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingInfo ri = c.ri;
    ri.pNext = nullptr;
    ri.colorAttachmentCount = (uint32_t)col.size();
    ri.pColorAttachments = col.data();
    ri.pDepthAttachment = c.depth.imageView ? &dep : nullptr;
    ri.pStencilAttachment = c.stencil.imageView ? &sten : nullptr;
    (r_vkCmdBeginRendering ? r_vkCmdBeginRendering : r_vkCmdBeginRenderingKHR)(hud, &ri);
    ReplayState(hud, c);
    c.redirect = hud;
    c.splitSlot = k;
    g_redirects++;
    g_splitW = (uint32_t)w; g_splitH = (uint32_t)h;
    if (!B.splitLogged) {
        B.splitLogged = true;
        VkLog("effects: battle frame split at the HUD (first HUD draw in cmd#%d): FortsGFX goes under the HUD", Id(kCmd, cb));
    }
    return true;
}

static int g_testSplitAt = -1, g_frameDraws;   // [Vulkan] TestSplitAt=N (tools/vktest): split at the frame's Nth draw

static void MaybeSplit(VkCommandBuffer cb, CmdState& c) {   // before a draw; g_mx held
    if (g_splitDone || c.redirect || !c.rendering || !SplitOn()) return;
    if (g_testSplitAt < 0) g_testSplitAt = IniInt(L"TestSplitAt", 0);
    if (g_testSplitAt > 0) {
        if (++g_frameDraws == g_testSplitAt) { g_splitDone = true; Split(cb, c); }
        return;
    }
    std::string t = DrawTexture(c);
    if (!g_worldSeen) { g_worldSeen = IsPlayfield(t); return; }
    if (!IsHudStart(c, t)) return;
    g_splitDone = true;   // one try a frame
    Split(cb, c);
}

// Submits a batch list in which a command buffer was split (see above). False: nothing in it was split.
static bool SubmitSplit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f, VkResult& r) {
    int bi = -1, ci = -1, k = -1;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (!g_redirects) return false;
        for (uint32_t i = 0; i < n && bi < 0; i++)
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
                auto it = g_cmds.find(si[i].pCommandBuffers[j]);
                if (it != g_cmds.end() && it->second.splitSlot >= 0) {
                    bi = (int)i; ci = (int)j; k = it->second.splitSlot;
                    it->second.splitSlot = -1;
                    it->second.redirect = VK_NULL_HANDLE;
                    g_redirects--;
                    break;
                }
            }
    }
    if (bi < 0) return false;
    const VkSubmitInfo& b = si[bi];
    // 1. everything up to the split command buffer, then the copy into IN
    std::vector<VkSubmitInfo> part1(si, si + bi);
    std::vector<VkCommandBuffer> cmds1(b.pCommandBuffers, b.pCommandBuffers + ci + 1);
    cmds1.push_back(B.cmdIn[k]);
    VkSubmitInfo s1 = b;
    s1.commandBufferCount = (uint32_t)cmds1.size();
    s1.pCommandBuffers = cmds1.data();
    s1.signalSemaphoreCount = 1;
    s1.pSignalSemaphores = &B.semA;
    part1.push_back(s1);
    r = r_vkQueueSubmit(q, (uint32_t)part1.size(), part1.data(), VK_NULL_HANDLE);
    // 2. GL
    if (r == VK_SUCCESS) GlStage(true, false, (int32_t)g_splitW, (int32_t)g_splitH, true);
    // 3. the copy back, the HUD and the rest, with the game's semaphores and fence
    std::vector<VkCommandBuffer> cmds2 = { B.cmdOut[k], B.cmdHud[k] };
    cmds2.insert(cmds2.end(), b.pCommandBuffers + ci + 1, b.pCommandBuffers + b.commandBufferCount);
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, r == VK_SUCCESS ? 1u : 0u, &B.semB, &st, (uint32_t)cmds2.size(), cmds2.data(),
                        b.signalSemaphoreCount, b.pSignalSemaphores };
    std::vector<VkSubmitInfo> part2 = { s2 };
    part2.insert(part2.end(), si + bi + 1, si + n);
    VkResult r2 = r_vkQueueSubmit(q, (uint32_t)part2.size(), part2.data(), f);
    D.ResetFences(g_dev, 1, &B.fenceS[k]);
    r_vkQueueSubmit(q, 0, nullptr, B.fenceS[k]);   // signals once all of it is done: the slot can be used again
    if (r == VK_SUCCESS) r = r2;
    if (r != VK_SUCCESS) BridgeFail(Fmt("a split submit failed (%d)", r).c_str());
    return true;
}

// ---------------------------------------------------------------- the world drawn again in GL
// The surest way to put all of FortsGFX on Vulkan is to give its GL renderer the same frame. Every texture the game
// uploads (vkCmdCopyBufferToImage from a mapped staging buffer) is copied into a GL texture of ours, a mirror,
// registered under its file name as the GL hooks register an upload. Every draw of a battle frame up to the HUD is kept:
// its vertices (read from mapped memory as it is recorded), matrix, texture, sampler, blend and primitive. At the split
// it is drawn again in our GL context through the GL hooks themselves (hk_glBegin, hk_glVertex2f...), in the order the
// game drew it: the backdrop pass with each layer's matrix as the projection, the playfield with the camera's ortho as
// the projection and each object's matrix (the camera's inverse times the draw's) as the modelview - as the game draws
// with GL. So gfx.cpp sees what it sees with GL: masks, light sources, the world's position, the game's smoke and rain
// to replace, its own effects. Its post chain draws the result, which replaces the Vulkan world under the game's HUD.
// [Vulkan] Replay=0: only the image stages, on Vulkan's own frame.

extern "C" {   // hooks.cpp: the GL renderer's entry points, as the game calls them
void WINAPI hk_glBegin(GLenum); void WINAPI hk_glEnd(); void WINAPI hk_glVertex2f(GLfloat, GLfloat);
void WINAPI hk_glTexCoord2f(GLfloat, GLfloat); void WINAPI hk_glColor4f(GLfloat, GLfloat, GLfloat, GLfloat);
void WINAPI hk_glEnable(GLenum); void WINAPI hk_glDisable(GLenum); void WINAPI hk_glBlendFunc(GLenum, GLenum);
void WINAPI hk_glClear(GLbitfield); void WINAPI hk_glClearColor(GLclampf, GLclampf, GLclampf, GLclampf);
void WINAPI hk_glViewport(GLint, GLint, GLsizei, GLsizei); void WINAPI hk_glMatrixMode(GLenum); void WINAPI hk_glLoadIdentity();
void WINAPI hk_glLoadMatrixf(const GLfloat*); void WINAPI hk_glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void WINAPI hk_glBindTexture(GLenum, GLuint); void WINAPI hk_glDeleteTextures(GLsizei, const GLuint*);
void WINAPI hk_glTexEnvf(GLenum, GLenum, GLfloat); void WINAPI hk_glLineWidth(GLfloat);
}
enum : GLenum {
    kGlTexEnv = 0x2300, kGlTexEnvMode = 0x2200, kGlModulate = 0x2100, kGlSrcAlpha = 0x0302, kGlOneMinusSrcAlpha = 0x0303,
    kGlDepthTest = 0x0B71, kGlCullFace = 0x0B44, kGlWrapS = 0x2802, kGlWrapT = 0x2803, kGlRepeat = 0x2901, kGlClampEdge = 0x812F,
    kGlMirrored = 0x8370, kGlLinearMipLinear = 0x2703, kGlLinearMipNearest = 0x2701, kGlNearestMipNearest = 0x2700,
    kGlNearestMipLinear = 0x2702, kGlBaseLevel = 0x813C, kGlMaxLevel = 0x813D, kGlRgba = 0x1908, kGlBgra = 0x80E1,
    kGlUnsignedByte = 0x1401, kGlUnpackAlignment = 0x0CF5, kGlBptc = 0x8E8C, kGlDxt1 = 0x83F0, kGlDxt1a = 0x83F1,
    kGlDxt3 = 0x83F2, kGlDxt5 = 0x83F3, kGlRgtc1 = 0x8DBB, kGlRgtc2 = 0x8DBD,
    kGlR8 = 0x8229, kGlRed = 0x1903, kGlAlpha = 0x1906, kGlSwizzleR = 0x8E42, kGlSwizzleG = 0x8E43, kGlSwizzleB = 0x8E44, kGlSwizzleA = 0x8E45,
};
static void (WINAPI* glTexStorage2D_)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (WINAPI* glTexSubImage2D_)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
static void (WINAPI* glCompressedTexSubImage2D_)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*);

struct Upload { VkImage img; uint32_t mip; int32_t x, y; uint32_t w, h; std::vector<uint8_t> data; };
struct RDraw {
    VkImage img; VkSampler sampler;
    uint8_t frag;              // PipeInfo::frag
    bool blendOn, additive;
    VkPrimitiveTopology topo;
    float m[16];               // the draw's matrix (push constants)
    float scroll[2];           // fragscrolltex's texture offset
    float lineWidth;
    uint32_t first, count;     // in g_rverts, 8 floats a vertex: x y u v r g b a
    int pass;                  // the rendering pass of the frame it was in
    VkDescriptorSet ubo;       // verts.spv: where the matrix is (read when drawn again: the game updates it each frame)
    VkCommandBuffer list;      // an executed secondary command buffer (the terrain): drawn again as a GL display list
};
// A secondary command buffer's draws, kept when it is recorded, and the GL display list made of them.
struct SecList { unsigned version = 0; std::vector<RDraw> draws; std::vector<float> verts; GLuint glList = 0; unsigned glVersion = 0; };
static std::unordered_map<VkCommandBuffer, SecList> g_secLists;
static std::vector<Upload> g_uploads;          // texture data from the game's staging buffers, for GL
static std::vector<GLuint> g_mirrorGone;       // mirrors of images the game destroyed
static std::unordered_map<VkImage, GLuint> g_mirror;
static std::vector<RDraw> g_rdraws;            // this frame's draws so far
static std::vector<float> g_rverts;
static int g_framePass;                        // rendering passes begun this frame
static bool g_captureBroken;                   // a draw this frame couldn't be kept: no replay this frame
static const char* g_brokenWhy;
static int g_replayOn = -1;
static unsigned g_mirrorCount, g_replayFrames, g_replayMisses;

static bool ReplayOn() {
    if (g_replayOn < 0) g_replayOn = IniInt(L"Effects", 1) && IniInt(L"Replay", 1);
    return g_replayOn && !B.failed;
}

// Bytes per 4x4 block (block compressed) or per texel; 0: not mirrored.
static uint32_t TexelBytes(VkFormat f, bool& bc, GLenum& glFmt, GLenum& glData) {
    bc = true; glData = 0;
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: glFmt = kGlDxt1; return 8;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: glFmt = kGlDxt1a; return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK: glFmt = kGlDxt3; return 16;
    case VK_FORMAT_BC3_UNORM_BLOCK: glFmt = kGlDxt5; return 16;
    case VK_FORMAT_BC4_UNORM_BLOCK: glFmt = kGlRgtc1; return 8;
    case VK_FORMAT_BC5_UNORM_BLOCK: glFmt = kGlRgtc2; return 16;
    case VK_FORMAT_BC7_UNORM_BLOCK: glFmt = kGlBptc; return 16;
    default: break;
    }
    bc = false; glFmt = kGlRgba8;
    if (f == VK_FORMAT_R8G8B8A8_UNORM) { glData = kGlRgba; return 4; }
    if (f == VK_FORMAT_B8G8R8A8_UNORM) { glData = kGlBgra; return 4; }
    if (f == VK_FORMAT_R8_UNORM) { glFmt = kGlR8; glData = kGlRed; return 1; }   // font atlases: coverage only
    return 0;
}

static void CaptureUpload(VkBuffer b, VkImage im, uint32_t n, const VkBufferImageCopy* r) {   // g_mx held
    if (!ReplayOn()) return;
    auto ii = g_imgs.find(im);
    if (ii == g_imgs.end() || !(ii->second.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || ii->second.name.compare(0, 3, "ui/") == 0) return;
    bool bc;
    GLenum fmt, data;
    uint32_t tb = TexelBytes(ii->second.fmt, bc, fmt, data);
    if (!tb) return;
    for (uint32_t i = 0; i < n; i++) {
        const VkBufferImageCopy& c = r[i];
        uint32_t w = c.imageExtent.width, h = c.imageExtent.height;
        if (c.imageSubresource.baseArrayLayer || c.imageSubresource.layerCount != 1) continue;
        if (c.bufferRowLength && c.bufferRowLength != w) continue;   // not tightly packed: not seen from Forts
        size_t size = bc ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * tb : (size_t)w * h * tb;
        const uint8_t* p = BufPtr(b, c.bufferOffset);
        Upload u = { im, c.imageSubresource.mipLevel, c.imageOffset.x, c.imageOffset.y, w, h, {} };
        u.data.resize(size);
        if (!p || !SafeRead(u.data.data(), p, size)) {
            static bool logged;
            if (!logged) { logged = true; VkLog("replay: an upload's data could not be read (staging buffer not mapped)"); }
            continue;
        }
        g_uploads.push_back(std::move(u));
    }
}

// The uploads since last time, into the mirrors (GL current).
static void MirrorFlush() {
    std::vector<Upload> ups;
    std::vector<GLuint> gone;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        ups.swap(g_uploads);
        gone.swap(g_mirrorGone);
    }
    for (GLuint t : gone) hk_glDeleteTextures(1, &t);   // gfx.cpp forgets them too
    if (ups.empty()) return;
    if (!glTexStorage2D_) {
        glTexStorage2D_ = (decltype(glTexStorage2D_))RealProc("glTexStorage2D");
        glTexSubImage2D_ = (decltype(glTexSubImage2D_))RealProc("glTexSubImage2D");
        glCompressedTexSubImage2D_ = (decltype(glCompressedTexSubImage2D_))RealProc("glCompressedTexSubImage2D");
        if (!glTexStorage2D_ || !glTexSubImage2D_ || !glCompressedTexSubImage2D_) { g_replayOn = 0; VkLog("replay off: GL can't make the textures"); return; }
    }
    R.PixelStorei(kGlUnpackAlignment, 1);
    unsigned made = 0;
    for (Upload& u : ups) {
        ImgInfo info;
        GLuint tex;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            auto ii = g_imgs.find(u.img);
            if (ii == g_imgs.end()) continue;   // destroyed meanwhile
            info = ii->second;
            tex = g_mirror[u.img];
        }
        bool bc;
        GLenum fmt, data;
        TexelBytes(info.fmt, bc, fmt, data);
        if (!tex) {
            GL.GenTextures(1, &tex);
            GL.BindTexture(kGlTex2D, tex);
            glTexStorage2D_(kGlTex2D, (GLsizei)std::max(info.mips, 1u), fmt, (GLsizei)info.w, (GLsizei)info.h);
            GL.TexParameteri(kGlTex2D, kGlBaseLevel, 0);
            GL.TexParameteri(kGlTex2D, kGlMaxLevel, (GLint)std::max(info.mips, 1u) - 1);
            if (fmt == kGlR8) {   // read as white with the coverage in alpha, like the GL renderer's alpha textures
                GL.TexParameteri(kGlTex2D, kGlSwizzleR, 1); GL.TexParameteri(kGlTex2D, kGlSwizzleG, 1);
                GL.TexParameteri(kGlTex2D, kGlSwizzleB, 1); GL.TexParameteri(kGlTex2D, kGlSwizzleA, kGlRed);
            }
            GfxClassifyTexture(tex, info.name.empty() ? "(generated by game)" : info.name);
            GfxNoteTextureFormat(tex, (GLint)(fmt == kGlR8 ? kGlAlpha : fmt));
            GfxNoteTextureSize(tex, (int)info.w, (int)info.h);
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            g_mirror[u.img] = tex;
            made++;
        } else GL.BindTexture(kGlTex2D, tex);
        if (bc) glCompressedTexSubImage2D_(kGlTex2D, (GLint)u.mip, u.x, u.y, (GLsizei)u.w, (GLsizei)u.h, fmt, (GLsizei)u.data.size(), u.data.data());
        else glTexSubImage2D_(kGlTex2D, (GLint)u.mip, u.x, u.y, (GLsizei)u.w, (GLsizei)u.h, data, kGlUnsignedByte, u.data.data());
    }
    GL.BindTexture(kGlTex2D, 0);
    if (GLenum e = GL.GetError()) VkLog("replay: GL error 0x%x making the mirrors", e);
    g_mirrorCount += made;
    if (made) VkLog("replay: %u textures mirrored in GL (%u in all), %zu uploads", made, g_mirrorCount, ups.size());
}

static void CaptureDraw(CmdState& c, VkCommandBuffer cb, uint32_t first, uint32_t count, uint32_t instances) {   // g_mx held
    if (!ReplayOn()) return;
    SecList* sl = c.secondary ? &g_secLists[cb] : nullptr;
    bool& broken = sl ? c.sbroken : g_captureBroken;
    const char*& why = sl ? c.sWhy : g_brokenWhy;
    if (broken || (!sl && g_splitDone)) return;
    std::vector<RDraw>& out = sl ? sl->draws : g_rdraws;
    std::vector<float>& verts = sl ? sl->verts : g_rverts;
    auto pi = g_pipes.find(c.pipe);
    bool fortsVertex = pi != g_pipes.end() && pi->second.stride[0] == 32 && pi->second.attrs.size() == 3 &&
                       pi->second.attrs[0].offset == 0 && pi->second.attrs[1].offset == 8 && pi->second.attrs[2].offset == 16 &&
                       pi->second.attrs[2].format == VK_FORMAT_R32G32B32A32_SFLOAT;
    bool ubo = fortsVertex && pi->second.uboMatrix;
    if (!fortsVertex || (!ubo && !c.hasVsPush) || (ubo && !c.sets[1]) || !c.vb[0] || instances != 1) {
        broken = true;
        why = !fortsVertex ? "a draw without Forts' 32-byte vertex" : (!ubo && !c.hasVsPush) ? "a draw without a matrix" : ubo && !c.sets[1] ? "a draw without its matrix's uniform buffer"
            : !c.vb[0] ? "a draw without a vertex buffer" : "an instanced draw";
        return;
    }
    RDraw d = {};
    if (ubo) d.ubo = c.sets[1];
    d.frag = pi->second.frag;
    if (d.frag != 1) {
        auto s = g_sets.find(c.sets[0]);
        auto b = s != g_sets.end() ? s->second.find(0) : decltype(s->second.end()){};
        if (s == g_sets.end() || b == s->second.end()) { broken = true; why = "a textured draw without its texture's descriptor"; return; }
        auto v = g_views.find(b->second.view);
        if (v == g_views.end()) { broken = true; why = "a texture view not seen made"; return; }
        d.img = v->second;
        d.sampler = b->second.sampler;
    }
    d.blendOn = pi->second.blendOn;
    d.additive = pi->second.dst == VK_BLEND_FACTOR_ONE;
    d.topo = pi->second.topo;
    memcpy(d.m, c.vsPush, sizeof(d.m));
    for (const PushRec& p : c.pushes)
        if ((p.stages & VK_SHADER_STAGE_FRAGMENT_BIT) && p.off == 64 && p.size >= 8) memcpy(d.scroll, p.data, 8);
    d.lineWidth = c.hasLineWidth ? c.lineWidth : 1.0f;
    d.pass = g_framePass;
    const uint8_t* p = BufPtr(c.vb[0], c.vbOff[0] + (VkDeviceSize)first * 32);
    size_t at = verts.size();
    verts.resize(at + (size_t)count * 8);
    if (!p || !SafeRead(&verts[at], p, (size_t)count * 32)) { verts.resize(at); broken = true; why = "vertices not readable (buffer not mapped)"; return; }
    d.first = (uint32_t)(at / 8);
    d.count = count;
    out.push_back(d);
}

// The game executes secondary command buffers (recorded once: the terrain) in a frame's pass: each goes into the
// frame's draws as one display list, at its place.
static void CaptureExecute(uint32_t n, const VkCommandBuffer* cbs) {   // g_mx held
    if (!ReplayOn() || g_splitDone || g_captureBroken) return;
    for (uint32_t i = 0; i < n; i++) {
        auto c = g_cmds.find(cbs[i]);
        auto s = g_secLists.find(cbs[i]);
        if (c == g_cmds.end() || !c->second.secondary || c->second.sbroken || s == g_secLists.end()) {
            g_captureBroken = true;
            g_brokenWhy = c != g_cmds.end() && c->second.sWhy ? c->second.sWhy : "a secondary command buffer that wasn't kept";
            return;
        }
        s->second.version = c->second.version;
        if (s->second.draws.empty()) continue;
        RDraw d = {};
        d.list = cbs[i];
        d.pass = g_framePass;
        d.ubo = s->second.draws[0].ubo;
        memcpy(d.m, s->second.draws[0].m, sizeof(d.m));
        d.img = s->second.draws[0].img;
        d.sampler = s->second.draws[0].sampler;
        d.frag = s->second.draws[0].frag;
        d.blendOn = s->second.draws[0].blendOn;
        d.additive = s->second.draws[0].additive;
        g_rdraws.push_back(d);
    }
}

static bool UboMatrix(VkDescriptorSet set, float* m) {   // the matrix a verts.spv draw is drawn with, as the uniform buffer holds it now
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto s = g_setBufs.find(set);
    if (s == g_setBufs.end()) return false;
    auto b = s->second.find(0);
    if (b == s->second.end()) return false;
    const uint8_t* p = BufPtr(b->second.buf, b->second.off);
    return p && SafeRead(m, p, 64);
}

static void ApplySampler(VkSampler s) {   // the draw's filtering and wrapping, on its mirror (bound)
    SamplerInfo si = { VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto it = g_samplers.find(s);
        if (it != g_samplers.end()) si = it->second;
    }
    auto wrap = [](VkSamplerAddressMode m) -> GLint {
        return m == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || m == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ? kGlClampEdge
             : m == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT ? kGlMirrored : kGlRepeat;
    };
    bool lin = si.min == VK_FILTER_LINEAR, mipLin = si.mip == VK_SAMPLER_MIPMAP_MODE_LINEAR;
    GL.TexParameteri(kGlTex2D, kGlWrapS, wrap(si.u));
    GL.TexParameteri(kGlTex2D, kGlWrapT, wrap(si.v));
    GL.TexParameteri(kGlTex2D, kGlMinFilter, lin ? (mipLin ? kGlLinearMipLinear : kGlLinearMipNearest) : (mipLin ? kGlNearestMipLinear : kGlNearestMipNearest));
    GL.TexParameteri(kGlTex2D, kGlMagFilter, si.mag == VK_FILTER_LINEAR ? kGlLinear : kGlNearest);
}

static void Ortho(const float* m) {   // the projection: an ortho as the game's GL renderer sets it (gfx.cpp reads the world from it)
    if (fabsf(m[1]) + fabsf(m[4]) > 1e-9f || m[0] == 0.0f || m[5] == 0.0f || m[10] == 0.0f) { hk_glLoadMatrixf(m); return; }
    double l = (-1.0 - m[12]) / m[0], r = (1.0 - m[12]) / m[0], b = (-1.0 - m[13]) / m[5], t = (1.0 - m[13]) / m[5];
    double fn = -2.0 / m[10], sum = -m[14] * fn;   // far - near, far + near
    hk_glOrtho(l, r, b, t, (sum - fn) / 2.0, (sum + fn) / 2.0);
}

static GLuint (WINAPI* glGenLists_)(GLsizei);
extern "C" { void WINAPI hk_glNewList(GLuint, GLenum); void WINAPI hk_glEndList(); void WINAPI hk_glCallList(GLuint); }

// One draw's texture, blend and vertices, through the GL hooks (also while a display list is compiled).
static void EmitDraw(const RDraw& d, const float* v, GLuint tex) {
    if (tex) {
        hk_glEnable(kGlTex2D);
        hk_glBindTexture(kGlTex2D, tex);
        ApplySampler(d.sampler);
    } else hk_glDisable(kGlTex2D);
    if (d.blendOn) { hk_glEnable(GL_BLEND); hk_glBlendFunc(kGlSrcAlpha, d.additive ? (GLenum)1 : (GLenum)kGlOneMinusSrcAlpha); }
    else hk_glDisable(GL_BLEND);
    uint32_t n = d.count;
    float su = d.frag == 2 ? d.scroll[0] : 0.0f, sv = d.frag == 2 ? d.scroll[1] : 0.0f;
    auto vtx = [&](uint32_t k) {
        const float* p = v + (size_t)k * 8;
        hk_glTexCoord2f(p[2] + su, p[3] + sv);
        hk_glColor4f(p[4], p[5], p[6], p[7]);
        hk_glVertex2f(p[0], p[1]);
    };
    // The game's particles are quads, which Vulkan gets as two triangles each (q0 q1 q2, q1 q3 q2): as quads again,
    // gfx.cpp tells them apart as it does with GL.
    bool quads = d.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && n >= 6 && n % 6 == 0;
    for (uint32_t k = 0; quads && k < n; k += 6)
        quads = !memcmp(v + (k + 3) * 8, v + (k + 1) * 8, 4 * sizeof(float)) && !memcmp(v + (k + 5) * 8, v + (k + 2) * 8, 4 * sizeof(float));
    if (d.topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || d.topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) hk_glLineWidth(d.lineWidth);
    if (quads) {
        hk_glBegin(0x0007);   // GL_QUADS
        for (uint32_t k = 0; k < n; k += 6) { vtx(k); vtx(k + 1); vtx(k + 4); vtx(k + 2); }
    } else {
        static const GLenum modes[] = { 0x0000, 0x0001, 0x0003, 0x0004, 0x0005, 0x0006 };   // points .. triangle fan
        hk_glBegin((unsigned)d.topo < 6 ? modes[d.topo] : 0x0004);
        for (uint32_t k = 0; k < n; k++) vtx(k);
    }
    hk_glEnd();
}

static GLuint MirrorOf(VkImage img) {   // g_mx held
    auto m = g_mirror.find(img);
    return m == g_mirror.end() ? 0 : m->second;
}

// The kept draws, through gfx.cpp's world pass into OUT. False: not possible this frame (OUT untouched).
static bool Replay(int32_t w, int32_t h) {
    std::vector<RDraw> draws;
    std::vector<float> verts;
    bool broken;
    const char* brokenWhy;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        draws.swap(g_rdraws);
        verts.swap(g_rverts);
        broken = g_captureBroken;
        brokenWhy = g_brokenWhy;
    }
    auto skip = [](const char* why) { if (g_replayMisses++ < 8) VkLog("replay: not this frame - %s; Vulkan's own frame gets the image stages", why); return false; };
    if (broken) return skip(brokenWhy ? brokenWhy : "a draw could not be kept");
    if (draws.empty()) return skip("no draws kept before the HUD");
    // matrices from uniform buffers as they are now; the textures' mirrors; the display lists' own draws
    std::vector<GLuint> tex(draws.size(), 0);
    std::unordered_map<VkCommandBuffer, SecList> lists;
    for (RDraw& d : draws)
        if (d.ubo && !UboMatrix(d.ubo, d.m)) return skip("a matrix's uniform buffer is not readable");
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto missing = [&](VkImage img) {
            if (g_replayMisses++ < 8) VkLog("replay: not this frame - no mirror of %s", g_imgs.count(img) ? ImgDesc(img).c_str() : "an image");
            return false;
        };
        for (size_t i = 0; i < draws.size(); i++) {
            if (draws[i].list) {
                SecList& sl = g_secLists[draws[i].list];
                for (const RDraw& s : sl.draws) if (s.frag != 1 && !MirrorOf(s.img)) return missing(s.img);
                if (sl.glVersion != sl.version || !sl.glList) lists[draws[i].list] = sl;   // to compile
            }
            if (draws[i].frag != 1 && !(tex[i] = MirrorOf(draws[i].img))) return missing(draws[i].img);
        }
    }
    // the camera: the matrix the playfield's world-space struts are drawn with
    int backPass = draws[0].pass;
    const float* cam = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        for (const RDraw& d : draws) {
            if (d.pass == backPass || d.list || d.frag == 1) continue;
            auto ii = g_imgs.find(d.img);
            if (ii != g_imgs.end() && ii->second.name.find("materials/") != std::string::npos) { cam = d.m; break; }
        }
    }
    if (!cam) return skip("no struts (materials/) drawn after the backdrop pass to place the world by");
    float inv[16] = { 1 / cam[0], 0, 0, 0, 0, 1 / cam[5], 0, 0, 0, 0, 1 / cam[10], 0, -cam[12] / cam[0], -cam[13] / cam[5], -cam[14] / cam[10], 1 };
    auto objectMatrix = [&](const float* m, float* mv) {   // the camera's inverse x the draw's
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += inv[k * 4 + r] * m[c * 4 + k];
                mv[c * 4 + r] = s;
            }
    };
    if (!GfxBridgeBegin(w, h)) return false;   // FortsGFX off (F9)
    // display lists made from secondary command buffers recorded since last time
    if (!lists.empty()) {
        if (!glGenLists_) glGenLists_ = (decltype(glGenLists_))RealProc("glGenLists");
        for (auto& kv : lists) {
            SecList& sl = kv.second;
            if (!sl.glList) sl.glList = glGenLists_ ? glGenLists_(1) : 0;
            if (!sl.glList) continue;
            hk_glNewList(sl.glList, 0x1300);   // GL_COMPILE: the hooks keep its vertices (the terrain's shape)
            for (const RDraw& s : sl.draws) {
                GLuint t;
                { std::lock_guard<std::recursive_mutex> lk(g_mx); t = s.frag == 1 ? 0 : MirrorOf(s.img); }
                EmitDraw(s, &sl.verts[(size_t)s.first * 8], t);
            }
            hk_glEndList();
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            SecList& live = g_secLists[kv.first];
            live.glList = sl.glList;
            live.glVersion = sl.version;
        }
    }
    hk_glViewport(0, 0, w, h);
    hk_glClearColor(0, 0, 0, 0);
    hk_glClear(kGlColorBit);
    hk_glDisable(kGlDepthTest);
    hk_glDisable(kGlCullFace);
    hk_glDisable(kGlScissorTest);
    hk_glTexEnvf(kGlTexEnv, kGlTexEnvMode, (GLfloat)kGlModulate);
    unsigned listsDrawn = 0;
    for (size_t i = 0; i < draws.size(); i++) {
        const RDraw& d = draws[i];
        bool back = d.pass == backPass && !d.list;
        hk_glMatrixMode(GL_PROJECTION);
        hk_glLoadIdentity();
        Ortho(back ? d.m : cam);
        hk_glMatrixMode(GL_MODELVIEW);
        if (back) hk_glLoadIdentity();
        else { float mv[16]; objectMatrix(d.m, mv); hk_glLoadMatrixf(mv); }
        if (d.list) {   // as the GL renderer draws terrain: a display list, with its first texture bound
            GLuint gl;
            { std::lock_guard<std::recursive_mutex> lk(g_mx); gl = g_secLists[d.list].glList; }
            if (!gl) continue;
            if (tex[i]) { hk_glEnable(kGlTex2D); hk_glBindTexture(kGlTex2D, tex[i]); } else hk_glDisable(kGlTex2D);
            hk_glCallList(gl);
            listsDrawn++;
            continue;
        }
        EmitDraw(d, &verts[(size_t)d.first * 8], tex[i]);
    }
    hk_glDisable(GL_BLEND);
    GfxBridgeEnd(B.glFbo[1]);
    if (++g_replayFrames == 1)
        VkLog("replay: first frame drawn again in GL (%zu draws, %u of them display lists from secondary command buffers, %zu vertices)",
              draws.size(), listsDrawn, verts.size() / 8);
    return true;
}

// The game is about to destroy its device: everything of ours made on it goes first. The GL context stays for the
// next device, and the bridge sets itself up again on that one's first frame.
static void DeviceGone() {
    if (!D.ok) return;
    D.DeviceWaitIdle(g_dev);
    if (B.glRC && (B.w || B.glSemA)) {
        GlCurrent cur;
        if (B.w) { GL.DeleteFramebuffers(2, B.glFbo); GL.DeleteTextures(2, B.glTex); GL.DeleteMemoryObjectsEXT(2, B.glMem); }
        const GLuint sems[2] = { B.glSemA, B.glSemB };
        if (B.glSemA) GL.DeleteSemaphoresEXT(2, sems);
    }
    for (int i = 0; i < 2 && B.w; i++) { D.DestroyImage(g_dev, B.img[i], nullptr); D.FreeMemory(g_dev, B.mem[i], nullptr); }
    for (VkSemaphore s : { B.semA, B.semB, B.semC, g_shotSem }) if (s) D.DestroySemaphore(g_dev, s, nullptr);
    for (VkFence f : { B.fence[0], B.fence[1], B.fence[2], B.fenceS[0], B.fenceS[1], B.fenceS[2], g_shotFence }) if (f) D.DestroyFence(g_dev, f, nullptr);
    for (VkCommandPool p : { B.pool, g_shotPool }) if (p) D.DestroyCommandPool(g_dev, p, nullptr);
    if (g_shotBuf) { D.DestroyBuffer(g_dev, g_shotBuf, nullptr); D.FreeMemory(g_dev, g_shotMem, nullptr); }
    HWND wnd = B.glWnd; HDC dc = B.glDC; HGLRC rc = B.glRC;
    bool mark = B.testMark, after = B.shotAfter;
    B = decltype(B)();
    B.glWnd = wnd; B.glDC = dc; B.glRC = rc; B.testMark = mark; B.shotAfter = after;
    g_shotPool = VK_NULL_HANDLE; g_shotFamily = ~0u; g_shotCmd = VK_NULL_HANDLE; g_shotFence = VK_NULL_HANDLE;
    g_shotSem = VK_NULL_HANDLE; g_shotBuf = VK_NULL_HANDLE; g_shotMem = VK_NULL_HANDLE; g_shotSize = 0; g_shotPtr = nullptr;
    D.ok = D.bridge = false;
    VkLog("the game destroys its device: FortsGFX's objects on it released");
}

// ---------------------------------------------------------------- instance and device

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    VkResult r = r_vkCreateInstance(ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (r == VK_SUCCESS) g_inst = *out;
    std::string ext, lay;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) ext += std::string(i ? ", " : "") + ci->ppEnabledExtensionNames[i];
    for (uint32_t i = 0; i < ci->enabledLayerCount; i++) lay += std::string(i ? ", " : "") + ci->ppEnabledLayerNames[i];
    const VkApplicationInfo* ai = ci->pApplicationInfo;
    uint32_t v = ai ? ai->apiVersion : 0;
    VkLog("vkCreateInstance -> %d: app '%s' engine '%s', api %u.%u.%u, layers [%s], extensions [%s]", r,
          ai && ai->pApplicationName ? ai->pApplicationName : "", ai && ai->pEngineName ? ai->pEngineName : "",
          VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), lay.c_str(), ext.c_str());
    return r;
}

// The bridge shares images and semaphores with GL: the game's device gets the extensions for that when the driver has
// them (and is made as the game asked if that fails).
static const char* kInteropExt[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore",
                                     "VK_KHR_external_semaphore_win32" };

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkDeviceCreateInfo c = *ci;
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    bool interop = false;
    if (IniInt(L"Effects", 1)) {
        std::vector<VkExtensionProperties> have;
        uint32_t n = 0;
        auto enumExt = (PFN_vkEnumerateDeviceExtensionProperties)r_vkGetInstanceProcAddr(g_inst, "vkEnumerateDeviceExtensionProperties");
        if (enumExt && enumExt(pd, nullptr, &n, nullptr) == VK_SUCCESS) { have.resize(n); enumExt(pd, nullptr, &n, have.data()); }
        interop = true;
        for (const char* e : kInteropExt) {
            bool supported = false, on = false;
            for (auto& h : have) supported = supported || !strcmp(h.extensionName, e);
            for (const char* x : exts) on = on || !strcmp(x, e);
            if (!supported) { interop = false; VkLog("effects off: the driver has no %s", e); break; }
            if (!on) exts.push_back(e);
        }
        if (interop) { c.enabledExtensionCount = (uint32_t)exts.size(); c.ppEnabledExtensionNames = exts.data(); }
    }
    VkResult r = r_vkCreateDevice(pd, &c, a, out);
    if (r != VK_SUCCESS && interop) {
        VkLog("vkCreateDevice with the sharing extensions -> %d: made as the game asked, effects off", r);
        r = r_vkCreateDevice(pd, ci, a, out);
        interop = false;
    }
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_interopExt = interop && r == VK_SUCCESS;
    VkPhysicalDeviceProperties props = {};
    if (auto gpp = (PFN_vkGetPhysicalDeviceProperties)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties")) gpp(pd, &props);
    std::string ext, queues, chain;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) ext += std::string(i ? ", " : "") + ci->ppEnabledExtensionNames[i];
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        queues += Fmt("%sfamily %u x%u", i ? ", " : "", ci->pQueueCreateInfos[i].queueFamilyIndex, ci->pQueueCreateInfos[i].queueCount);
    for (auto p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext) {
        chain += Fmt(" sType %d", (int)p->sType);
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
            chain += ((const VkPhysicalDeviceVulkan13Features*)p)->dynamicRendering ? "(dynamicRendering on)" : "(dynamicRendering off)";
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
            chain += ((const VkPhysicalDeviceDynamicRenderingFeatures*)p)->dynamicRendering ? "(dynamicRendering on)" : "(dynamicRendering off)";
    }
    VkLog("vkCreateDevice -> %d: %s (vendor %04x device %04x, driver 0x%x, api %u.%u.%u), queues [%s], extensions [%s], features:%s",
          r, props.deviceName, props.vendorID, props.deviceID, props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), queues.c_str(), ext.c_str(),
          chain.empty() ? " none" : chain.c_str());
    if (r == VK_SUCCESS) { g_phys = pd; g_dev = *out; ResolveOwn(); }
    if (g_interopExt) VkLog("effects: the device can share images and semaphores with GL%s", D.bridge ? "" : " - but a function is missing");
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hk_vkDestroyDevice(VkDevice d, const VkAllocationCallbacks* a) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (d && d == g_dev) {
            DeviceGone();
            g_dev = VK_NULL_HANDLE;
            g_swaps.clear();
            g_queueFamily.clear();
            g_cmds.clear();
            g_redirects = 0;
            g_mainFamily = ~0u;
        }
    }
    r_vkDestroyDevice(d, a);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue* q) {
    r_vkGetDeviceQueue(d, family, index, q);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (*q && !g_queueFamily.count(*q)) VkLog("queue: family %u index %u", family, index);
    if (*q) g_queueFamily[*q] = family;
    if (*q && g_mainFamily == ~0u) g_mainFamily = family;
}
static VKAPI_ATTR void VKAPI_CALL hk_vkGetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* qi, VkQueue* q) {
    r_vkGetDeviceQueue2(d, qi, q);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (*q) g_queueFamily[*q] = qi->queueFamilyIndex;
}

// ---------------------------------------------------------------- swapchain and frames

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateWin32SurfaceKHR(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSurfaceKHR* out) {
    VkResult r = r_vkCreateWin32SurfaceKHR(inst, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (r == VK_SUCCESS) g_surfaceWnd[*out] = ci->hwnd;
    char title[128] = "";
    GetWindowTextA(ci->hwnd, title, sizeof(title));
    VkLog("vkCreateWin32SurfaceKHR -> %d: window '%s'", r, title);
    return r;
}

static VkImageUsageFlags SurfaceUsage(VkSurfaceKHR surface) {
    auto caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR c = {};
    return caps && g_phys && caps(g_phys, surface, &c) == VK_SUCCESS ? c.supportedUsageFlags : 0;
}

// The swapchain images can also be copied from (screenshots, the bridge) and to (the bridge), when the surface allows it.
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR c = *ci;
    c.imageUsage |= SurfaceUsage(c.surface) & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkResult r = r_vkCreateSwapchainKHR(d, &c, a, out);
    if (r != VK_SUCCESS && c.imageUsage != ci->imageUsage) { r = r_vkCreateSwapchainKHR(d, ci, a, out); c.imageUsage = ci->imageUsage; }   // as asked
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    VkImageUsageFlags added = c.imageUsage & ~ci->imageUsage;
    VkLog("vkCreateSwapchainKHR -> %d: %ux%u %s colour space %d, %u images min, usage %s%s%s, present mode %d, composite %d, clipped %u",
          r, ci->imageExtent.width, ci->imageExtent.height, FormatName(ci->imageFormat).c_str(), (int)ci->imageColorSpace,
          ci->minImageCount, UsageName(ci->imageUsage).c_str(), added ? " + " : "", added ? UsageName(added).c_str() : "",
          (int)ci->presentMode, (int)ci->compositeAlpha, ci->clipped);
    if (r == VK_SUCCESS) {
        auto w = g_surfaceWnd.find(ci->surface);
        g_swaps[*out] = { ci->imageFormat, ci->imageExtent.width, ci->imageExtent.height, (c.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0,
                          (c.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, w != g_surfaceWnd.end() ? w->second : nullptr, {} };
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hk_vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    { std::lock_guard<std::recursive_mutex> lk(g_mx); g_swaps.erase(sc); }
    r_vkDestroySwapchainKHR(d, sc, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
    VkResult r = r_vkGetSwapchainImagesKHR(d, sc, n, imgs);
    if (!imgs || (r != VK_SUCCESS && r != VK_INCOMPLETE)) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_swaps.find(sc);
    if (it == g_swaps.end()) return r;
    it->second.images.assign(imgs, imgs + *n);
    for (uint32_t i = 0; i < *n; i++)
        g_imgs[imgs[i]] = { Id(kImg, imgs[i]), it->second.fmt, it->second.w, it->second.h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, Fmt("swapchain[%u]", i) };
    VkLog("swapchain images: %u", *n);
    return r;
}

static void Summary(double now) {
    double f = std::max(g_windowFrames, 1.0);
    std::vector<std::pair<int, VkPipeline>> top;
    for (auto& p : g_pipeDraws) top.push_back({ p.second, p.first });
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string pipes;
    for (size_t i = 0; i < top.size() && i < 6; i++) {
        auto pi = g_pipes.find(top[i].second);
        pipes += Fmt("%s pipe#%d %.0f/frame (%s)", i ? "," : "", Id(kPipe, top[i].second), top[i].first / f,
                     pi != g_pipes.end() ? pi->second.shaders.c_str() : "?");
    }
    const Counts& w = g_window;
    VkLog("frames: %.0f in %.1f s, %.2f ms avg (worst %.1f) | per frame: %.1f submits, %.1f cmd buffers, %.1f render passes + %.1f dynamic, "
          "%.0f draws (%.0f indexed), %.0f vertices, %.0f pipeline binds, %.0f descriptor binds, %.0f push constants, %.1f uploads, "
          "%.1f barriers | %zu pipelines used:%s",
          g_windowFrames, now - g_lastSummary, g_windowMs / f, g_windowMaxMs, w.submits / f, w.cmdBegins / f, w.rps / f, w.rendering / f,
          w.draws / f, w.drawsIdx / f, w.verts / f, w.pipeBinds / f, w.descBinds / f, w.pushes / f, w.uploads / f, w.barriers / f,
          g_pipeDraws.size(), pipes.c_str());
    g_window = {};
    g_pipeDraws.clear();
    g_windowFrames = g_windowMs = g_windowMaxMs = 0;
    g_lastSummary = now;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
    std::unique_lock<std::recursive_mutex> lk(g_mx);
    double now = NowS();
    if (g_lastPresent < 0) {
        VkLog("first frame presented");
        SessionLog("vulkan: Forts is drawing with Vulkan - FortsGFX recon in vulkan.log");
        g_lastSummary = now;
        g_nextAutoDump = now + IniInt(L"DumpAt", 10);   // seconds to the first automatic dump
    }
    else {
        double ms = (now - g_lastPresent) * 1000.0;
        g_windowMs += ms;
        g_windowMaxMs = std::max(g_windowMaxMs, ms);
    }
    g_lastPresent = now;
    g_windowFrames++;
    const double* fc = &g_frame.submits;
    double* wc = &g_window.submits;
    for (size_t i = 0; i < sizeof(Counts) / sizeof(double); i++) wc[i] += fc[i];
    g_frame = {};
    g_frameNo++;

    // what is presented: the game's image, copied out for a dump first, then through the bridge (or the other way
    // round with [Vulkan] ShotAfterEffects=1); each step waits for the one before
    const VkPresentInfoKHR* cur = pi;
    VkPresentInfoKHR shotMod, bridgeMod;
    bool shot = false, shotLater = false;
    unsigned dumped = 0;
    if (g_dumpRec) {   // the dumped frame is complete: its commands, then this image
        for (auto& c : g_cmds) if (!c.second.lines.empty()) FlushCmd("recorded, not submitted", ~0u, c.first);
        dumped = ++g_dumps;
        std::wstring path = g_outDir + Widen(Fmt("vk_frame_%02u.txt", dumped));
        if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(g_dumpText.data(), 1, g_dumpText.size(), f); fclose(f); }
        g_dumpText.clear();
        g_dumpRec = false;
        if (B.shotAfter) shotLater = true;
        else if ((shot = ShotStart(q, cur, shotMod))) cur = &shotMod;
        VkLog("frame %u written to vk_frame_%02u.txt", g_dumpFrame, dumped);
    }
    HWND wnd = nullptr;
    if (pi->swapchainCount) { auto sw = g_swaps.find(pi->pSwapchains[0]); if (sw != g_swaps.end()) wnd = sw->second.wnd; }
    lk.unlock();
    // the control panel's keys every frame; the present pass when there is something to draw over the frame (or the
    // effects themselves, when the frame isn't split at the HUD)
    bool panel = OverlayBridgeUpdate(wnd);
    if (BridgeFrame(q, cur, bridgeMod, !SplitOn(), panel)) cur = &bridgeMod;
    if (shotLater) { lk.lock(); if ((shot = ShotStart(q, cur, shotMod))) cur = &shotMod; lk.unlock(); }
    VkResult r = r_vkQueuePresentKHR(q, cur);
    lk.lock();
    if (shot) ShotFinish(dumped);
    else if (dumped) VkLog("no screenshot of that frame");
    g_worldSeen = g_splitDone = false;   // the next frame
    g_frameDraws = 0;
    g_rdraws.clear();
    g_rverts.clear();
    g_captureBroken = false;
    g_brokenWhy = nullptr;
    g_framePass = 0;
    bool mirror = !g_uploads.empty() && ReplayOn();
    lk.unlock();
    if (mirror && BridgeGlContext()) { GlCurrent cur; MirrorFlush(); }   // textures loaded in menus and loading screens
    lk.lock();

    static bool keyPrev;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool key = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F11) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
               (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool want = key && !keyPrev;
    keyPrev = key;
    if (now >= g_nextAutoDump) { want = true; g_nextAutoDump = now + 60.0; }
    if (want && g_dumps < kMaxDumps && !g_dumpRec) {
        g_dumpRec = true;
        g_dumpFrame = g_frameNo + 1;
        for (auto& c : g_cmds) c.second.lines.clear();
        g_dumpText = Fmt("FortsGFX Vulkan recon: frame %u (%s)\n", g_dumpFrame, key ? "Ctrl+Shift+F11" : "automatic");
        if (key) MessageBeep(MB_OK);
    }
    if (now - g_lastSummary >= 10.0) Summary(now);
    return r;
}

static void Submitted(VkQueue q, VkCommandBuffer cb) {   // g_mx held
    auto qf = g_queueFamily.find(q);
    FlushCmd("SUBMIT", qf != g_queueFamily.end() ? qf->second : ~0u, cb);
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        g_frame.submits++;
        if (g_dumpRec)
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t j = 0; j < si[i].commandBufferCount; j++) Submitted(q, si[i].pCommandBuffers[j]);
    }
    VkResult r;
    if (SubmitSplit(q, n, si, f, r)) return r;
    return r_vkQueueSubmit(q, n, si, f);
}
static VkResult Submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f, PFN_vkQueueSubmit2 real) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        g_frame.submits++;
        if (g_dumpRec)
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t j = 0; j < si[i].commandBufferInfoCount; j++) Submitted(q, si[i].pCommandBufferInfos[j].commandBuffer);
    }
    return real(q, n, si, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) { return Submit2(q, n, si, f, r_vkQueueSubmit2); }
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkQueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) { return Submit2(q, n, si, f, r_vkQueueSubmit2KHR); }

// ---------------------------------------------------------------- render passes, shaders, pipelines

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    VkResult r = r_vkCreateRenderPass(d, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string att;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        const VkAttachmentDescription& x = ci->pAttachments[i];
        att += Fmt("%s%s x%d %s/%s %s->%s", i ? "; " : "", FormatName(x.format).c_str(), (int)x.samples, LoadOpName(x.loadOp),
                   StoreOpName(x.storeOp), LayoutName(x.initialLayout), LayoutName(x.finalLayout));
    }
    if (r == VK_SUCCESS) g_rps[*out] = att;
    VkLog("render pass rp#%d: %u subpasses, attachments [%s]", r == VK_SUCCESS ? Id(kRp, *out) : 0, ci->subpassCount, att.c_str());
    return r;
}
static VkResult RenderPass2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out, PFN_vkCreateRenderPass2 real) {
    VkResult r = real(d, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string att;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        const VkAttachmentDescription2& x = ci->pAttachments[i];
        att += Fmt("%s%s x%d %s/%s %s->%s", i ? "; " : "", FormatName(x.format).c_str(), (int)x.samples, LoadOpName(x.loadOp),
                   StoreOpName(x.storeOp), LayoutName(x.initialLayout), LayoutName(x.finalLayout));
    }
    if (r == VK_SUCCESS) g_rps[*out] = att;
    VkLog("render pass (2) rp#%d: %u subpasses, attachments [%s]", r == VK_SUCCESS ? Id(kRp, *out) : 0, ci->subpassCount, att.c_str());
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out) { return RenderPass2(d, ci, a, out, r_vkCreateRenderPass2); }
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateRenderPass2KHR(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out) { return RenderPass2(d, ci, a, out, r_vkCreateRenderPass2KHR); }

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* out) {
    VkResult r = r_vkCreateFramebuffer(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    FbInfo fb = { {}, ci->width, ci->height };
    std::string views;
    if (ci->pAttachments && !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
        for (uint32_t i = 0; i < ci->attachmentCount; i++) { fb.views.push_back(ci->pAttachments[i]); views += (i ? "; " : "") + ViewDesc(ci->pAttachments[i]); }
    g_fbs[*out] = fb;
    VkLog("framebuffer fb#%d %ux%u for rp#%d: [%s]", Id(kFb, *out), ci->width, ci->height, Id(kRp, ci->renderPass), views.c_str());
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* out) {
    VkResult r = r_vkCreateShaderModule(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_spv.empty()) {   // the game's own shaders, to name the modules
        WIN32_FIND_DATAW fd;
        std::wstring dir = g_gameDir + L"data\\shaders\\";
        HANDLE h = FindFirstFileW((dir + L"*.spv").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (FILE* f = _wfopen((dir + fd.cFileName).c_str(), L"rb")) {
                    std::vector<uint8_t> b(fd.nFileSizeLow);
                    size_t got = fread(b.data(), 1, b.size(), f);
                    fclose(f);
                    g_spv[Narrow(fd.cFileName)] = { got, SafeHash(b.data(), got) };
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (g_spv.empty()) g_spv[""] = { 0, 0 };
    }
    uint64_t hash = SafeHash(ci->pCode, ci->codeSize);
    std::string name = Fmt("spv#%zu", ci->codeSize);
    for (auto& s : g_spv) if (s.second.first == ci->codeSize && s.second.second == hash) name = s.first;
    g_shaders[*out] = name;
    VkLog("shader module sh#%d: %s (%zu bytes)", Id(kShader, *out), name.c_str(), ci->codeSize);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* ci, const VkAllocationCallbacks* a, VkDescriptorSetLayout* out) {
    VkResult r = r_vkCreateDescriptorSetLayout(d, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string b;
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        b += Fmt("%sbinding %u: type %d x%u stages 0x%x", i ? "; " : "", ci->pBindings[i].binding, (int)ci->pBindings[i].descriptorType,
                 ci->pBindings[i].descriptorCount, ci->pBindings[i].stageFlags);
    VkLog("descriptor set layout (flags 0x%x): [%s]", ci->flags, b.c_str());
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks* a, VkPipelineLayout* out) {
    VkResult r = r_vkCreatePipelineLayout(d, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string pc;
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++)
        pc += Fmt("%sstages 0x%x +%u %u bytes", i ? "; " : "", ci->pPushConstantRanges[i].stageFlags, ci->pPushConstantRanges[i].offset, ci->pPushConstantRanges[i].size);
    VkLog("pipeline layout layout#%d: %u set layouts, push constants [%s]", r == VK_SUCCESS ? Id(kLayout, *out) : 0, ci->setLayoutCount, pc.c_str());
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                                   const VkAllocationCallbacks* a, VkPipeline* out) {
    VkResult r = r_vkCreateGraphicsPipelines(d, cache, n, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (uint32_t i = 0; i < n; i++) {
        const VkGraphicsPipelineCreateInfo& p = ci[i];
        PipeInfo pi = {};
        pi.id = Id(kPipe, out[i]);
        pi.frag = 3;
        for (uint32_t s = 0; s < p.stageCount; s++) {
            auto sh = g_shaders.find(p.pStages[s].module);
            pi.shaders += Fmt("%s%s %s", s ? " + " : "", p.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT ? "vs" : p.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT ? "fs" : "stage",
                              sh != g_shaders.end() ? sh->second.c_str() : "?");
            if (p.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT && sh != g_shaders.end()) pi.uboMatrix = sh->second == "verts.spv";
            if (p.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && sh != g_shaders.end())
                pi.frag = sh->second == "frag.spv" ? 0 : sh->second == "colourfrag.spv" ? 1 : sh->second == "fragscrolltex.spv" ? 2 : 3;
        }
        std::string attrs;
        if (const VkPipelineVertexInputStateCreateInfo* vi = p.pVertexInputState) {
            for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++)
                if (vi->pVertexBindingDescriptions[b].binding < 4) pi.stride[vi->pVertexBindingDescriptions[b].binding] = vi->pVertexBindingDescriptions[b].stride;
            for (uint32_t k = 0; k < vi->vertexAttributeDescriptionCount; k++) {
                const VkVertexInputAttributeDescription& x = vi->pVertexAttributeDescriptions[k];
                pi.attrs.push_back(x);
                attrs += Fmt("%sloc%u b%u %s +%u", k ? ", " : "", x.location, x.binding, FormatName(x.format).c_str(), x.offset);
            }
        }
        pi.topo = p.pInputAssemblyState ? p.pInputAssemblyState->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pi.blend = "none";
        if (p.pColorBlendState && p.pColorBlendState->attachmentCount) {
            const VkPipelineColorBlendAttachmentState& b = p.pColorBlendState->pAttachments[0];
            pi.blendOn = b.blendEnable != VK_FALSE;
            pi.src = b.srcColorBlendFactor;
            pi.dst = b.dstColorBlendFactor;
            pi.blend = b.blendEnable ? Fmt("%s/%s op %d, alpha %s/%s", BlendName(b.srcColorBlendFactor), BlendName(b.dstColorBlendFactor),
                                           (int)b.colorBlendOp, BlendName(b.srcAlphaBlendFactor), BlendName(b.dstAlphaBlendFactor))
                                     : std::string("off");
            if (b.colorWriteMask != 0xF) pi.blend += Fmt(", write mask 0x%x", b.colorWriteMask);
        }
        std::string target = Fmt("rp#%d subpass %u", Id(kRp, p.renderPass), p.subpass);
        for (auto x = (const VkBaseInStructure*)p.pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                auto pr = (const VkPipelineRenderingCreateInfo*)x;
                target = "dynamic rendering [";
                for (uint32_t c = 0; c < pr->colorAttachmentCount; c++) target += (c ? ", " : "") + FormatName(pr->pColorAttachmentFormats[c]);
                target += "]";
            }
        std::string dyn;
        if (p.pDynamicState)
            for (uint32_t k = 0; k < p.pDynamicState->dynamicStateCount; k++) dyn += Fmt("%s%d", k ? "," : "", (int)p.pDynamicState->pDynamicStates[k]);
        bool depth = p.pDepthStencilState && (p.pDepthStencilState->depthTestEnable || p.pDepthStencilState->depthWriteEnable);
        VkLog("pipeline pipe#%d: %s | %s, strides %u/%u, attributes [%s] | blend %s | %s | layout#%d | dynamic [%s]%s", pi.id,
              pi.shaders.c_str(), TopologyName(pi.topo), pi.stride[0], pi.stride[1], attrs.c_str(), pi.blend.c_str(), target.c_str(),
              Id(kLayout, p.layout), dyn.c_str(), depth ? " | depth test/write" : "");
        if (out[i]) g_pipes[out[i]] = pi;
    }
    return r;
}

// ---------------------------------------------------------------- images, buffers, memory, descriptors

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateImage(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
    VkResult r = r_vkCreateImage(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    ImgInfo ii = { Id(kImg, *out), ci->format, ci->extent.width, ci->extent.height, ci->mipLevels, ci->usage, HookFreshImageName() };
    static std::string testName = Narrow([] { wchar_t b[64] = L""; GetPrivateProfileStringW(L"Vulkan", L"TestName", L"", b, 64, (g_gameDir + L"FortsGFX.ini").c_str()); return std::wstring(b); }().c_str());
    if (ii.name.empty() && !testName.empty() && (ci->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) ii.name = Fmt("%stest%d.tga", testName.c_str(), ii.id);   // tools/vktest
    LoadLog("VKIMG img#%d %ux%u %s mips %u layers %u usage %s%s%s", ii.id, ii.w, ii.h, FormatName(ii.fmt).c_str(), ii.mips,
            ci->arrayLayers, UsageName(ii.usage).c_str(), ii.name.empty() ? "" : " <- ", ii.name.c_str());
    g_imgs[*out] = ii;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hk_vkDestroyImage(VkDevice d, VkImage im, const VkAllocationCallbacks* a) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto m = g_mirror.find(im);
        if (m != g_mirror.end()) { g_mirrorGone.push_back(m->second); g_mirror.erase(m); }
        g_uploads.erase(std::remove_if(g_uploads.begin(), g_uploads.end(), [im](const Upload& u) { return u.img == im; }), g_uploads.end());
        g_imgs.erase(im);   // a later image may get the same handle
    }
    r_vkDestroyImage(d, im, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateSampler(VkDevice d, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks* a, VkSampler* out) {
    VkResult r = r_vkCreateSampler(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_samplers[*out] = { ci->magFilter, ci->minFilter, ci->mipmapMode, ci->addressModeU, ci->addressModeV };
    VkLog("sampler: mag %d min %d mip %d, address %d/%d", (int)ci->magFilter, (int)ci->minFilter, (int)ci->mipmapMode, (int)ci->addressModeU, (int)ci->addressModeV);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    VkResult r = r_vkCreateImageView(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_views[*out] = ci->image;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a, VkBuffer* out) {
    VkResult r = r_vkCreateBuffer(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_bufs[*out] = { ci->size, ci->usage, VK_NULL_HANDLE, 0 };
    if (ci->usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
        VkLog("buffer buf#%d: %llu bytes, usage 0x%x", Id(kBuf, *out), (unsigned long long)ci->size, ci->usage);
    return r;
}

static void BoundBuffer(VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_bufs.find(b);
    if (it != g_bufs.end()) { it->second.mem = m; it->second.memOff = off; }
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    VkResult r = r_vkBindBufferMemory(d, b, m, off);
    if (r == VK_SUCCESS) BoundBuffer(b, m, off);
    return r;
}
static VkResult BindBuffers2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* bi, PFN_vkBindBufferMemory2 real) {
    VkResult r = real(d, n, bi);
    if (r == VK_SUCCESS) for (uint32_t i = 0; i < n; i++) BoundBuffer(bi[i].buffer, bi[i].memory, bi[i].memoryOffset);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkBindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* bi) { return BindBuffers2(d, n, bi, r_vkBindBufferMemory2); }
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkBindBufferMemory2KHR(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* bi) { return BindBuffers2(d, n, bi, r_vkBindBufferMemory2KHR); }

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags fl, void** pp) {
    VkResult r = r_vkMapMemory(d, m, off, size, fl, pp);
    if (r == VK_SUCCESS && pp && *pp) { std::lock_guard<std::recursive_mutex> lk(g_mx); g_maps[m] = { (uint8_t*)*pp, off }; }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL hk_vkUnmapMemory(VkDevice d, VkDeviceMemory m) {
    { std::lock_guard<std::recursive_mutex> lk(g_mx); g_maps.erase(m); }
    r_vkUnmapMemory(d, m);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkUpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        static bool logged;
        for (uint32_t i = 0; i < nw; i++) {
            if ((w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) && w[i].pImageInfo)
                for (uint32_t j = 0; j < w[i].descriptorCount; j++) g_sets[w[i].dstSet][w[i].dstBinding + j] = { w[i].pImageInfo[j].imageView, w[i].pImageInfo[j].sampler };
            if ((w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) && w[i].pBufferInfo)
                for (uint32_t j = 0; j < w[i].descriptorCount; j++)
                    g_setBufs[w[i].dstSet][w[i].dstBinding + j] = { w[i].pBufferInfo[j].buffer, w[i].pBufferInfo[j].offset, w[i].pBufferInfo[j].range };
            if (!logged && w[i].descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                logged = true;
                VkLog("descriptor writes also of type %d (binding %u)", (int)w[i].descriptorType, w[i].dstBinding);
            }
        }
    }
    r_vkUpdateDescriptorSets(d, nw, w, nc, c);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkUpdateDescriptorSetWithTemplate(VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void* data) {
    static bool logged;
    if (!logged) { logged = true; VkLog("the game updates descriptor sets through templates (not followed)"); }
    r_vkUpdateDescriptorSetWithTemplate(d, s, t, data);
}

// ---------------------------------------------------------------- command recording

#define LOCK std::lock_guard<std::recursive_mutex> lk(g_mx)
// Every command goes to Redir(cb): the game's command buffer, or our cmdHud once a split has happened in it.

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    VkResult r = r_vkAllocateCommandBuffers(d, ai, out);
    if (r != VK_SUCCESS) return r;
    LOCK;
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) g_cmdSecondary[out[i]] = ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    if (ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
        static bool logged;
        if (!logged) { logged = true; VkLog("the game makes secondary command buffers (recorded once, executed from frames)"); }
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL hk_vkFreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
    {
        LOCK;
        for (uint32_t i = 0; i < n; i++) { g_cmdSecondary.erase(cbs[i]); g_secLists.erase(cbs[i]); g_cmds.erase(cbs[i]); }
    }
    r_vkFreeCommandBuffers(d, pool, n, cbs);
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        if (c.splitSlot >= 0) g_redirects--;   // recorded again before it was submitted: that split is dropped
        static unsigned versions;
        c = CmdState();
        auto sec = g_cmdSecondary.find(cb);
        c.secondary = sec != g_cmdSecondary.end() && sec->second;
        c.version = ++versions;
        if (c.secondary) { SecList& sl = g_secLists[cb]; sl.draws.clear(); sl.verts.clear(); sl.version = c.version; }
        g_frame.cmdBegins++;
        if (g_dumpRec) DL(cb, Fmt("BEGIN cmd#%d flags 0x%x", Id(kCmd, cb), bi->flags));
    }
    return r_vkBeginCommandBuffer(cb, bi);
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkEndCommandBuffer(VkCommandBuffer cb) {
    VkCommandBuffer hud = VK_NULL_HANDLE;
    {
        LOCK;
        if (g_dumpRec) DL(cb, "END");
        auto it = g_cmds.find(cb);
        if (it != g_cmds.end()) hud = it->second.redirect;
    }
    if (hud) r_vkEndCommandBuffer(hud);
    return r_vkEndCommandBuffer(cb);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents sc) {
    {
        LOCK;
        g_frame.rps++;
        if (g_dumpRec) {
            std::string views, clear;
            auto fb = g_fbs.find(bi->framebuffer);
            if (fb != g_fbs.end()) for (size_t i = 0; i < fb->second.views.size(); i++) views += (i ? "; " : "") + ViewDesc(fb->second.views[i]);
            if (bi->clearValueCount && bi->pClearValues)
                clear = Fmt(" clear (%.3g %.3g %.3g %.3g)", bi->pClearValues[0].color.float32[0], bi->pClearValues[0].color.float32[1],
                            bi->pClearValues[0].color.float32[2], bi->pClearValues[0].color.float32[3]);
            DL(cb, Fmt("RENDERPASS rp#%d fb#%d area %d,%d %ux%u%s -> [%s]", Id(kRp, bi->renderPass), Id(kFb, bi->framebuffer), bi->renderArea.offset.x,
                       bi->renderArea.offset.y, bi->renderArea.extent.width, bi->renderArea.extent.height, clear.c_str(), views.c_str()));
        }
    }
    r_vkCmdBeginRenderPass(Redir(cb), bi, sc);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdEndRenderPass(VkCommandBuffer cb) {
    { LOCK; if (g_dumpRec) DL(cb, "END RENDERPASS"); }
    r_vkCmdEndRenderPass(Redir(cb));
}
static void BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri, PFN_vkCmdBeginRendering real) {
    {
        LOCK;
        g_frame.rendering++;
        if (!g_splitDone) g_framePass++;
        CmdState& c = g_cmds[cb];   // kept for a split: the pass begun again on the same attachments
        c.rendering = true;
        c.ri = *ri;
        c.color.assign(ri->pColorAttachments, ri->pColorAttachments + ri->colorAttachmentCount);
        c.depth = ri->pDepthAttachment ? *ri->pDepthAttachment : VkRenderingAttachmentInfo{};
        c.stencil = ri->pStencilAttachment ? *ri->pStencilAttachment : VkRenderingAttachmentInfo{};
        if (g_dumpRec) {
            std::string att;
            for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) {
                const VkRenderingAttachmentInfo& x = ri->pColorAttachments[i];
                att += Fmt("%s%s %s", i ? "; " : "", ViewDesc(x.imageView).c_str(), LoadOpName(x.loadOp));
                if (x.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
                    att += Fmt(" (%.3g %.3g %.3g %.3g)", x.clearValue.color.float32[0], x.clearValue.color.float32[1], x.clearValue.color.float32[2], x.clearValue.color.float32[3]);
                if (x.resolveImageView) att += " resolve -> " + ViewDesc(x.resolveImageView);
            }
            DL(cb, Fmt("RENDERING area %d,%d %ux%u -> [%s]%s", ri->renderArea.offset.x, ri->renderArea.offset.y, ri->renderArea.extent.width,
                       ri->renderArea.extent.height, att.c_str(), ri->pDepthAttachment ? " + depth" : ""));
        }
    }
    real(Redir(cb), ri);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) { BeginRendering(cb, ri, r_vkCmdBeginRendering); }
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* ri) { BeginRendering(cb, ri, r_vkCmdBeginRenderingKHR); }
static void EndRendering(VkCommandBuffer cb, PFN_vkCmdEndRendering real) {
    { LOCK; g_cmds[cb].rendering = false; if (g_dumpRec) DL(cb, "END RENDERING"); }
    real(Redir(cb));
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdEndRendering(VkCommandBuffer cb) { EndRendering(cb, r_vkCmdEndRendering); }
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdEndRenderingKHR(VkCommandBuffer cb) { EndRendering(cb, r_vkCmdEndRenderingKHR); }

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    {
        LOCK;
        if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) g_cmds[cb].pipe = p;
        g_frame.pipeBinds++;
        if (g_dumpRec) {
            auto pi = g_pipes.find(p);
            DL(cb, Fmt("PIPELINE pipe#%d %s | blend %s", Id(kPipe, p), pi != g_pipes.end() ? pi->second.shaders.c_str() : "?",
                       pi != g_pipes.end() ? pi->second.blend.c_str() : "?"));
        }
    }
    r_vkCmdBindPipeline(Redir(cb), bp, p);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                                                             uint32_t n, const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    {
        LOCK;
        g_frame.descBinds++;
        if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
            CmdState& c = g_cmds[cb];
            for (uint32_t i = 0; i < n; i++) if (first + i < 4) { c.sets[first + i] = sets[i]; c.setLayout[first + i] = layout; }
        }
        if (g_dumpRec) {
            std::string s;
            for (uint32_t i = 0; i < n; i++) {
                s += Fmt("%sset%u=set#%d {", i ? " " : "", first + i, Id(kSet, sets[i]));
                auto it = g_sets.find(sets[i]);
                if (it != g_sets.end())
                    for (auto& b : it->second) s += Fmt(" b%u: %s", b.first, ViewDesc(b.second.view).c_str());
                s += " }";
            }
            for (uint32_t i = 0; i < nd; i++) s += Fmt("%s%u", i ? "," : " dynamic offsets ", dyn[i]);
            DL(cb, Fmt("DESCRIPTORS layout#%d %s", Id(kLayout, layout), s.c_str()));
        }
    }
    r_vkCmdBindDescriptorSets(Redir(cb), bp, layout, first, n, sets, nd, dyn);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs, const VkDeviceSize* offs) {
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        for (uint32_t i = 0; i < n; i++) if (first + i < 4) { c.vb[first + i] = bufs[i]; c.vbOff[first + i] = offs[i]; }
        if (g_dumpRec)
            for (uint32_t i = 0; i < n; i++)
                DL(cb, Fmt("VERTEX BUFFER b%u buf#%d +%llu%s", first + i, Id(kBuf, bufs[i]), (unsigned long long)offs[i], BufPtr(bufs[i], 0) ? " (mapped)" : ""));
    }
    r_vkCmdBindVertexBuffers(Redir(cb), first, n, bufs, offs);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        c.ib = b; c.ibOff = off; c.ibType = t;
        if (g_dumpRec) DL(cb, Fmt("INDEX BUFFER buf#%d +%llu %s%s", Id(kBuf, b), (unsigned long long)off, t == VK_INDEX_TYPE_UINT16 ? "u16" : "u32", BufPtr(b, 0) ? " (mapped)" : ""));
    }
    r_vkCmdBindIndexBuffer(Redir(cb), b, off, t);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* v) {
    {
        LOCK;
        g_frame.pushes++;
        CmdState& c = g_cmds[cb];
        if (size <= sizeof(PushRec::data)) {   // the latest values per range, for a split to set again
            PushRec* p = nullptr;
            for (auto& x : c.pushes) if (x.stages == st && x.off == off) p = &x;
            if (!p) { c.pushes.push_back({}); p = &c.pushes.back(); }
            p->layout = layout; p->stages = st; p->off = off; p->size = size;
            memcpy(p->data, v, size);
        }
        if ((st & VK_SHADER_STAGE_VERTEX_BIT) && off == 0 && size >= 64) { memcpy(c.vsPush, v, 64); c.hasVsPush = true; }
        if (g_dumpRec) {
            std::string s;
            const float* f = (const float*)v;
            for (uint32_t i = 0; i < size / 4 && i < 32; i++) s += Fmt("%s%.5g", i ? " " : "", f[i]);
            DL(cb, Fmt("PUSH layout#%d stages 0x%x +%u %u bytes: %s", Id(kLayout, layout), st, off, size, s.c_str()));
        }
    }
    r_vkCmdPushConstants(Redir(cb), layout, st, off, size, v);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                                               uint32_t n, const VkWriteDescriptorSet* w) {
    {
        LOCK;
        g_frame.descBinds++;
        if (g_dumpRec) {
            std::string s;
            for (uint32_t i = 0; i < n; i++)
                if (w[i].pImageInfo) s += Fmt(" b%u: %s", w[i].dstBinding, ViewDesc(w[i].pImageInfo[0].imageView).c_str());
            DL(cb, Fmt("PUSH DESCRIPTORS layout#%d set%u%s", Id(kLayout, layout), set, s.c_str()));
        }
    }
    r_vkCmdPushDescriptorSetKHR(Redir(cb), bp, layout, set, n, w);
}

// Draws: the HUD split happens before the first HUD draw; once split, the lab's "Hide HUD" (and photo mode) drops
// what the game draws after it.
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    bool drop;
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        g_frame.draws++;
        g_frame.verts += (double)vc * ic;
        g_pipeDraws[c.pipe]++;
        if (g_dumpRec) {
            uint32_t idx[4], n = std::min(vc, 4u);
            for (uint32_t i = 0; i < n; i++) idx[i] = fv + i;
            DL(cb, Fmt("DRAW %u vertices from %u x%u%s", vc, fv, ic, Vertices(c, idx, n).c_str()));
        }
        MaybeSplit(cb, c);
        if (!c.redirect) CaptureDraw(c, cb, fv, vc, ic);
        drop = c.redirect && GfxLabState().hideHud;
    }
    if (!drop) r_vkCmdDraw(Redir(cb), vc, ic, fv, fi);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fidx, int32_t voff, uint32_t fi) {
    bool drop;
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        g_frame.draws++;
        g_frame.drawsIdx++;
        g_frame.verts += (double)ic * inst;
        g_pipeDraws[c.pipe]++;
        if (g_dumpRec) {
            uint32_t idx[4], n = 0;
            size_t isz = c.ibType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
            if (const uint8_t* p = BufPtr(c.ib, c.ibOff + (VkDeviceSize)fidx * isz))
                for (; n < 4 && n < ic; n++) {
                    uint32_t v = 0;
                    if (!SafeRead(&v, p + n * isz, isz)) break;
                    idx[n] = (uint32_t)((int32_t)v + voff);
                }
            DL(cb, Fmt("DRAW INDEXED %u indices from %u, vertex offset %d x%u%s", ic, fidx, voff, inst, Vertices(c, idx, n).c_str()));
        }
        MaybeSplit(cb, c);
        if (!c.redirect && !g_splitDone) { g_captureBroken = true; g_brokenWhy = "an indexed draw"; }   // Forts draws nothing indexed
        drop = c.redirect && GfxLabState().hideHud;
    }
    if (!drop) r_vkCmdDrawIndexed(Redir(cb), ic, inst, fidx, voff, fi);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    {
        LOCK;
        if (first == 0 && n) { CmdState& c = g_cmds[cb]; c.viewport = v[0]; c.hasViewport = true; }
        if (g_dumpRec && n) DL(cb, Fmt("VIEWPORT %.0f,%.0f %.0fx%.0f", v[0].x, v[0].y, v[0].width, v[0].height));
    }
    r_vkCmdSetViewport(Redir(cb), first, n, v);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    {
        LOCK;
        if (first == 0 && n) { CmdState& c = g_cmds[cb]; c.scissor = r[0]; c.hasScissor = true; }
        if (g_dumpRec && n) DL(cb, Fmt("SCISSOR %d,%d %ux%u", r[0].offset.x, r[0].offset.y, r[0].extent.width, r[0].extent.height));
    }
    r_vkCmdSetScissor(Redir(cb), first, n, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdSetLineWidth(VkCommandBuffer cb, float wdt) {
    { LOCK; CmdState& c = g_cmds[cb]; c.lineWidth = wdt; c.hasLineWidth = true; }
    r_vkCmdSetLineWidth(Redir(cb), wdt);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) {
    {
        LOCK;
        g_frame.uploads++;
        CaptureUpload(b, im, n, r);
        if (g_dumpRec && n)
            DL(cb, Fmt("UPLOAD buf#%d -> %s, %u regions (first %ux%u at %d,%d mip %u)", Id(kBuf, b), ImgDesc(im).c_str(), n, r[0].imageExtent.width,
                       r[0].imageExtent.height, r[0].imageOffset.x, r[0].imageOffset.y, r[0].imageSubresource.mipLevel));
    }
    r_vkCmdCopyBufferToImage(Redir(cb), b, im, l, n, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                                          uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b,
                                                          uint32_t ni, const VkImageMemoryBarrier* im) {
    {
        LOCK;
        g_frame.barriers++;
        if (g_dumpRec) {
            std::string s;
            for (uint32_t i = 0; i < ni && i < 4; i++)
                s += Fmt("; %s %s->%s", ImgDesc(im[i].image).c_str(), LayoutName(im[i].oldLayout), LayoutName(im[i].newLayout));
            DL(cb, Fmt("BARRIER stages 0x%x->0x%x, %u memory, %u buffer, %u image%s", src, dst, nm, nb, ni, s.c_str()));
        }
    }
    r_vkCmdPipelineBarrier(Redir(cb), src, dst, dep, nm, m, nb, b, ni, im);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("CLEAR ATTACHMENTS %u, %u rects", n, nr)); }
    r_vkCmdClearAttachments(Redir(cb), n, a, nr, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                                    const VkImageBlit* r, VkFilter f) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("BLIT %s -> %s", ImgDesc(s).c_str(), ImgDesc(d).c_str())); }
    r_vkCmdBlitImage(Redir(cb), s, sl, d, dl, n, r, f);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("COPY IMAGE %s -> %s", ImgDesc(s).c_str(), ImgDesc(d).c_str())); }
    r_vkCmdCopyImage(Redir(cb), s, sl, d, dl, n, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("COPY BUFFER buf#%d -> buf#%d, %u regions", Id(kBuf, s), Id(kBuf, d), n)); }
    r_vkCmdCopyBuffer(Redir(cb), s, d, n, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* cbs) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("EXECUTE %u secondary command buffers", n)); if (!g_cmds[cb].redirect) CaptureExecute(n, cbs); }
    r_vkCmdExecuteCommands(Redir(cb), n, cbs);
}
#undef LOCK

// ---------------------------------------------------------------- lookups

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance inst, const char* name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetDeviceProcAddr(VkDevice dev, const char* name);

struct Hook { const char* name; PFN_vkVoidFunction hook; PFN_vkVoidFunction* real; };
#define HK(n) { #n, (PFN_vkVoidFunction)hk_##n, (PFN_vkVoidFunction*)&r_##n }
static const Hook kHooks[] = {
    HK(vkGetDeviceProcAddr), HK(vkCreateInstance), HK(vkCreateDevice), HK(vkDestroyDevice), HK(vkGetDeviceQueue), HK(vkGetDeviceQueue2),
    HK(vkCreateWin32SurfaceKHR), HK(vkCreateSwapchainKHR), HK(vkDestroySwapchainKHR), HK(vkGetSwapchainImagesKHR), HK(vkQueuePresentKHR),
    HK(vkQueueSubmit), HK(vkQueueSubmit2), HK(vkQueueSubmit2KHR),
    HK(vkCreateRenderPass), HK(vkCreateRenderPass2), HK(vkCreateRenderPass2KHR), HK(vkCreateFramebuffer),
    HK(vkCreateShaderModule), HK(vkCreatePipelineLayout), HK(vkCreateDescriptorSetLayout), HK(vkCreateGraphicsPipelines),
    HK(vkCreateImage), HK(vkDestroyImage), HK(vkCreateSampler), HK(vkCreateImageView), HK(vkCreateBuffer),
    HK(vkBindBufferMemory), HK(vkBindBufferMemory2), HK(vkBindBufferMemory2KHR), HK(vkMapMemory), HK(vkUnmapMemory),
    HK(vkUpdateDescriptorSets), HK(vkUpdateDescriptorSetWithTemplate),
    HK(vkBeginCommandBuffer), HK(vkEndCommandBuffer), HK(vkAllocateCommandBuffers), HK(vkFreeCommandBuffers), HK(vkCmdBeginRenderPass), HK(vkCmdEndRenderPass),
    HK(vkCmdBeginRendering), HK(vkCmdBeginRenderingKHR), HK(vkCmdEndRendering), HK(vkCmdEndRenderingKHR),
    HK(vkCmdBindPipeline), HK(vkCmdBindDescriptorSets), HK(vkCmdBindVertexBuffers), HK(vkCmdBindIndexBuffer),
    HK(vkCmdPushConstants), HK(vkCmdPushDescriptorSetKHR), HK(vkCmdDraw), HK(vkCmdDrawIndexed),
    HK(vkCmdSetViewport), HK(vkCmdSetScissor), HK(vkCmdCopyBufferToImage), HK(vkCmdPipelineBarrier),
    HK(vkCmdClearAttachments), HK(vkCmdBlitImage), HK(vkCmdCopyImage), HK(vkCmdSetLineWidth), HK(vkCmdCopyBuffer), HK(vkCmdExecuteCommands),
};
#undef HK

// Hands out our wrapper and keeps the driver's function. A device's own pointer replaces the instance's trampoline.
static PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real, bool device) {
    if (!real || !name) return real;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)hk_vkGetInstanceProcAddr;
    for (auto& h : kHooks)
        if (!strcmp(name, h.name)) {
            if (!*h.real || device) *h.real = real;
            return h.hook;
        }
    return real;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance inst, const char* name) {
    return Wrap(name, r_vkGetInstanceProcAddr(inst, name), false);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetDeviceProcAddr(VkDevice dev, const char* name) {
    return Wrap(name, r_vkGetDeviceProcAddr(dev, name), true);
}

static FARPROC (WINAPI* r_GetProcAddress)(HMODULE, LPCSTR);
static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = r_GetProcAddress(m, name);
    if (!p || !HIWORD((ULONG_PTR)name) || strncmp(name, "vk", 2) != 0 || m != GetModuleHandleW(L"vulkan-1.dll")) return p;
    if (!strcmp(name, "vkGetInstanceProcAddr")) {
        if (!r_vkGetInstanceProcAddr) {
            r_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)p;
            VkLog("Forts asked vulkan-1.dll for vkGetInstanceProcAddr: watching its Vulkan calls (FortsGFX recon V0)");
        }
        return (FARPROC)hk_vkGetInstanceProcAddr;
    }
    return (FARPROC)Wrap(name, (PFN_vkVoidFunction)p, false);
}

void VkInstallHooks() {
    if (!GetPrivateProfileIntW(L"Vulkan", L"Hook", 1, (g_gameDir + L"FortsGFX.ini").c_str())) { SessionLog("vulkan: [Vulkan] Hook=0, left alone"); return; }
    bool ok = PatchIat(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetProcAddress", (void*)hk_GetProcAddress, (void**)&r_GetProcAddress);
    SessionLog("vulkan: GetProcAddress hook %s (only used when Forts runs with EnableVulkan = true)", ok ? "installed" : "NOT FOUND");
}
