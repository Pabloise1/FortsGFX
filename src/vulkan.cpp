// Vulkan (Forts' EnableVulkan = true, in users\<id>\settings_hardware.lua). Forts loads vulkan-1.dll itself and asks it
// for vkGetInstanceProcAddr through GetProcAddress; that call is hooked in Forts.exe's import table, so every Vulkan
// function the game looks up can be wrapped here, from the same opengl32.dll.
//
// Phase V0 only watches. Nothing changes except that the swapchain may also be copied from (for the screenshot).
// vulkan.log: the instance, device, swapchain, render passes, shaders, pipelines and descriptor layouts as they are
// made, and every 10 s what an average frame does. loading.log: every image (VKIMG lines, with the file DevIL just
// loaded). vk_frame_NN.txt: every command of one frame, the first vertices of each draw decoded, and vk_frame_NN.png:
// that frame as presented - on Ctrl+Shift+F11, and by itself 10 s in and then once a minute, at most 8 in all.
// [Vulkan] Hook=0 in FortsGFX.ini leaves Vulkan alone.
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
struct PipeInfo { int id; std::string shaders, blend; VkPrimitiveTopology topo; uint32_t stride[4]; std::vector<VkVertexInputAttributeDescription> attrs; };
struct FbInfo { std::vector<VkImageView> views; uint32_t w, h; };
struct SwapInfo { VkFormat fmt; uint32_t w, h; bool copyable; std::vector<VkImage> images; };
struct CmdState {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkBuffer vb[4] = {};
    VkDeviceSize vbOff[4] = {};
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize ibOff = 0;
    VkIndexType ibType = VK_INDEX_TYPE_UINT16;
    std::vector<std::string> lines;   // while a frame is being dumped
};
static std::unordered_map<VkImage, ImgInfo> g_imgs;
static std::unordered_map<VkImageView, VkImage> g_views;
static std::unordered_map<VkBuffer, BufInfo> g_bufs;
static std::unordered_map<VkDeviceMemory, MapInfo> g_maps;
static std::unordered_map<VkPipeline, PipeInfo> g_pipes;
static std::unordered_map<VkShaderModule, std::string> g_shaders;
static std::unordered_map<VkDescriptorSet, std::map<uint32_t, VkImageView>> g_sets;
static std::unordered_map<VkRenderPass, std::string> g_rps;
static std::unordered_map<VkFramebuffer, FbInfo> g_fbs;
static std::unordered_map<VkQueue, uint32_t> g_queueFamily;
static std::unordered_map<VkSwapchainKHR, SwapInfo> g_swaps;
static std::unordered_map<VkCommandBuffer, CmdState> g_cmds;
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
REAL(vkCreateInstance); REAL(vkCreateDevice); REAL(vkGetDeviceQueue); REAL(vkGetDeviceQueue2);
REAL(vkCreateSwapchainKHR); REAL(vkGetSwapchainImagesKHR); REAL(vkQueuePresentKHR);
REAL(vkQueueSubmit); REAL(vkQueueSubmit2); REAL(vkQueueSubmit2KHR);
REAL(vkCreateRenderPass); REAL(vkCreateRenderPass2); REAL(vkCreateRenderPass2KHR); REAL(vkCreateFramebuffer);
REAL(vkCreateShaderModule); REAL(vkCreatePipelineLayout); REAL(vkCreateDescriptorSetLayout); REAL(vkCreateGraphicsPipelines);
REAL(vkCreateImage); REAL(vkCreateImageView); REAL(vkCreateBuffer);
REAL(vkBindBufferMemory); REAL(vkBindBufferMemory2); REAL(vkBindBufferMemory2KHR); REAL(vkMapMemory); REAL(vkUnmapMemory);
REAL(vkUpdateDescriptorSets); REAL(vkUpdateDescriptorSetWithTemplate);
REAL(vkBeginCommandBuffer); REAL(vkEndCommandBuffer);
REAL(vkCmdBeginRenderPass); REAL(vkCmdEndRenderPass);
REAL(vkCmdBeginRendering); REAL(vkCmdBeginRenderingKHR); REAL(vkCmdEndRendering); REAL(vkCmdEndRenderingKHR);
REAL(vkCmdBindPipeline); REAL(vkCmdBindDescriptorSets); REAL(vkCmdBindVertexBuffers); REAL(vkCmdBindIndexBuffer);
REAL(vkCmdPushConstants); REAL(vkCmdPushDescriptorSetKHR); REAL(vkCmdDraw); REAL(vkCmdDrawIndexed);
REAL(vkCmdSetViewport); REAL(vkCmdSetScissor); REAL(vkCmdCopyBufferToImage); REAL(vkCmdPipelineBarrier);
REAL(vkCmdClearAttachments); REAL(vkCmdBlitImage); REAL(vkCmdCopyImage);
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
    OWN(DestroyCommandPool);
#undef OWN
    D.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceMemoryProperties");
    D.ok = D.ok && D.GetPhysicalDeviceMemoryProperties;
    if (!D.ok) VkLog("screenshots off: a device function is missing");
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

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkResult r = r_vkCreateDevice(pd, ci, a, out);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
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
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hk_vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue* q) {
    r_vkGetDeviceQueue(d, family, index, q);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (*q && !g_queueFamily.count(*q)) VkLog("queue: family %u index %u", family, index);
    if (*q) g_queueFamily[*q] = family;
}
static VKAPI_ATTR void VKAPI_CALL hk_vkGetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* qi, VkQueue* q) {
    r_vkGetDeviceQueue2(d, qi, q);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (*q) g_queueFamily[*q] = qi->queueFamilyIndex;
}

// ---------------------------------------------------------------- swapchain and frames

static bool SurfaceAllowsCopy(VkSurfaceKHR surface) {
    auto caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)r_vkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR c = {};
    return caps && g_phys && caps(g_phys, surface, &c) == VK_SUCCESS && (c.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR c = *ci;
    bool copyable = (c.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (!copyable && SurfaceAllowsCopy(c.surface)) { c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; copyable = true; }
    VkResult r = r_vkCreateSwapchainKHR(d, &c, a, out);
    if (r != VK_SUCCESS && c.imageUsage != ci->imageUsage) { r = r_vkCreateSwapchainKHR(d, ci, a, out); copyable = false; }   // as asked
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    VkLog("vkCreateSwapchainKHR -> %d: %ux%u %s colour space %d, %u images min, usage %s%s, present mode %d, composite %d, clipped %u",
          r, ci->imageExtent.width, ci->imageExtent.height, FormatName(ci->imageFormat).c_str(), (int)ci->imageColorSpace,
          ci->minImageCount, UsageName(ci->imageUsage).c_str(), copyable && !(ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? " (+TRANSFER_SRC for screenshots)" : "",
          (int)ci->presentMode, (int)ci->compositeAlpha, ci->clipped);
    if (r == VK_SUCCESS) g_swaps[*out] = { ci->imageFormat, ci->imageExtent.width, ci->imageExtent.height, copyable, {} };
    return r;
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
    if (g_lastPresent < 0) { VkLog("first frame presented"); SessionLog("vulkan: Forts is drawing with Vulkan - FortsGFX recon in vulkan.log"); g_lastSummary = now; }
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

    VkPresentInfoKHR mod;
    bool shot = false;
    unsigned dumped = 0;
    if (g_dumpRec) {   // the dumped frame is complete: its commands, then this image
        for (auto& c : g_cmds) if (!c.second.lines.empty()) FlushCmd("recorded, not submitted", ~0u, c.first);
        dumped = ++g_dumps;
        std::wstring path = g_outDir + Widen(Fmt("vk_frame_%02u.txt", dumped));
        if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(g_dumpText.data(), 1, g_dumpText.size(), f); fclose(f); }
        g_dumpText.clear();
        g_dumpRec = false;
        shot = ShotStart(q, pi, mod);
        VkLog("frame %u written to vk_frame_%02u.txt%s", g_dumpFrame, dumped, shot ? " (+ .png)" : " (no screenshot)");
    }
    lk.unlock();
    VkResult r = r_vkQueuePresentKHR(q, shot ? &mod : pi);
    lk.lock();
    if (shot) ShotFinish(dumped);

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
        for (uint32_t s = 0; s < p.stageCount; s++) {
            auto sh = g_shaders.find(p.pStages[s].module);
            pi.shaders += Fmt("%s%s %s", s ? " + " : "", p.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT ? "vs" : p.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT ? "fs" : "stage",
                              sh != g_shaders.end() ? sh->second.c_str() : "?");
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
    LoadLog("VKIMG img#%d %ux%u %s mips %u layers %u usage %s%s%s", ii.id, ii.w, ii.h, FormatName(ii.fmt).c_str(), ii.mips,
            ci->arrayLayers, UsageName(ii.usage).c_str(), ii.name.empty() ? "" : " <- ", ii.name.c_str());
    g_imgs[*out] = ii;
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
                for (uint32_t j = 0; j < w[i].descriptorCount; j++) g_sets[w[i].dstSet][w[i].dstBinding + j] = w[i].pImageInfo[j].imageView;
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

static VKAPI_ATTR VkResult VKAPI_CALL hk_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        c = CmdState();
        g_frame.cmdBegins++;
        if (g_dumpRec) DL(cb, Fmt("BEGIN cmd#%d flags 0x%x", Id(kCmd, cb), bi->flags));
    }
    return r_vkBeginCommandBuffer(cb, bi);
}
static VKAPI_ATTR VkResult VKAPI_CALL hk_vkEndCommandBuffer(VkCommandBuffer cb) {
    { LOCK; if (g_dumpRec) DL(cb, "END"); }
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
    r_vkCmdBeginRenderPass(cb, bi, sc);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdEndRenderPass(VkCommandBuffer cb) {
    { LOCK; if (g_dumpRec) DL(cb, "END RENDERPASS"); }
    r_vkCmdEndRenderPass(cb);
}
static void BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri, PFN_vkCmdBeginRendering real) {
    {
        LOCK;
        g_frame.rendering++;
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
    real(cb, ri);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) { BeginRendering(cb, ri, r_vkCmdBeginRendering); }
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* ri) { BeginRendering(cb, ri, r_vkCmdBeginRenderingKHR); }
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdEndRendering(VkCommandBuffer cb) { { LOCK; if (g_dumpRec) DL(cb, "END RENDERING"); } r_vkCmdEndRendering(cb); }
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdEndRenderingKHR(VkCommandBuffer cb) { { LOCK; if (g_dumpRec) DL(cb, "END RENDERING"); } r_vkCmdEndRenderingKHR(cb); }

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
    r_vkCmdBindPipeline(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                                                             uint32_t n, const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    {
        LOCK;
        g_frame.descBinds++;
        if (g_dumpRec) {
            std::string s;
            for (uint32_t i = 0; i < n; i++) {
                s += Fmt("%sset%u=set#%d {", i ? " " : "", first + i, Id(kSet, sets[i]));
                auto it = g_sets.find(sets[i]);
                if (it != g_sets.end())
                    for (auto& b : it->second) s += Fmt(" b%u: %s", b.first, ViewDesc(b.second).c_str());
                s += " }";
            }
            for (uint32_t i = 0; i < nd; i++) s += Fmt("%s%u", i ? "," : " dynamic offsets ", dyn[i]);
            DL(cb, Fmt("DESCRIPTORS layout#%d %s", Id(kLayout, layout), s.c_str()));
        }
    }
    r_vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, nd, dyn);
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
    r_vkCmdBindVertexBuffers(cb, first, n, bufs, offs);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    {
        LOCK;
        CmdState& c = g_cmds[cb];
        c.ib = b; c.ibOff = off; c.ibType = t;
        if (g_dumpRec) DL(cb, Fmt("INDEX BUFFER buf#%d +%llu %s%s", Id(kBuf, b), (unsigned long long)off, t == VK_INDEX_TYPE_UINT16 ? "u16" : "u32", BufPtr(b, 0) ? " (mapped)" : ""));
    }
    r_vkCmdBindIndexBuffer(cb, b, off, t);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* v) {
    {
        LOCK;
        g_frame.pushes++;
        if (g_dumpRec) {
            std::string s;
            const float* f = (const float*)v;
            for (uint32_t i = 0; i < size / 4 && i < 32; i++) s += Fmt("%s%.5g", i ? " " : "", f[i]);
            DL(cb, Fmt("PUSH layout#%d stages 0x%x +%u %u bytes: %s", Id(kLayout, layout), st, off, size, s.c_str()));
        }
    }
    r_vkCmdPushConstants(cb, layout, st, off, size, v);
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
    r_vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, w);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
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
    }
    r_vkCmdDraw(cb, vc, ic, fv, fi);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fidx, int32_t voff, uint32_t fi) {
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
    }
    r_vkCmdDrawIndexed(cb, ic, inst, fidx, voff, fi);
}

static VKAPI_ATTR void VKAPI_CALL hk_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    { LOCK; if (g_dumpRec && n) DL(cb, Fmt("VIEWPORT %.0f,%.0f %.0fx%.0f", v[0].x, v[0].y, v[0].width, v[0].height)); }
    r_vkCmdSetViewport(cb, first, n, v);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    { LOCK; if (g_dumpRec && n) DL(cb, Fmt("SCISSOR %d,%d %ux%u", r[0].offset.x, r[0].offset.y, r[0].extent.width, r[0].extent.height)); }
    r_vkCmdSetScissor(cb, first, n, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) {
    {
        LOCK;
        g_frame.uploads++;
        if (g_dumpRec && n)
            DL(cb, Fmt("UPLOAD buf#%d -> %s, %u regions (first %ux%u at %d,%d mip %u)", Id(kBuf, b), ImgDesc(im).c_str(), n, r[0].imageExtent.width,
                       r[0].imageExtent.height, r[0].imageOffset.x, r[0].imageOffset.y, r[0].imageSubresource.mipLevel));
    }
    r_vkCmdCopyBufferToImage(cb, b, im, l, n, r);
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
    r_vkCmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("CLEAR ATTACHMENTS %u, %u rects", n, nr)); }
    r_vkCmdClearAttachments(cb, n, a, nr, r);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                                    const VkImageBlit* r, VkFilter f) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("BLIT %s -> %s", ImgDesc(s).c_str(), ImgDesc(d).c_str())); }
    r_vkCmdBlitImage(cb, s, sl, d, dl, n, r, f);
}
static VKAPI_ATTR void VKAPI_CALL hk_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    { LOCK; if (g_dumpRec) DL(cb, Fmt("COPY IMAGE %s -> %s", ImgDesc(s).c_str(), ImgDesc(d).c_str())); }
    r_vkCmdCopyImage(cb, s, sl, d, dl, n, r);
}
#undef LOCK

// ---------------------------------------------------------------- lookups

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance inst, const char* name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetDeviceProcAddr(VkDevice dev, const char* name);

struct Hook { const char* name; PFN_vkVoidFunction hook; PFN_vkVoidFunction* real; };
#define HK(n) { #n, (PFN_vkVoidFunction)hk_##n, (PFN_vkVoidFunction*)&r_##n }
static const Hook kHooks[] = {
    HK(vkGetDeviceProcAddr), HK(vkCreateInstance), HK(vkCreateDevice), HK(vkGetDeviceQueue), HK(vkGetDeviceQueue2),
    HK(vkCreateSwapchainKHR), HK(vkGetSwapchainImagesKHR), HK(vkQueuePresentKHR),
    HK(vkQueueSubmit), HK(vkQueueSubmit2), HK(vkQueueSubmit2KHR),
    HK(vkCreateRenderPass), HK(vkCreateRenderPass2), HK(vkCreateRenderPass2KHR), HK(vkCreateFramebuffer),
    HK(vkCreateShaderModule), HK(vkCreatePipelineLayout), HK(vkCreateDescriptorSetLayout), HK(vkCreateGraphicsPipelines),
    HK(vkCreateImage), HK(vkCreateImageView), HK(vkCreateBuffer),
    HK(vkBindBufferMemory), HK(vkBindBufferMemory2), HK(vkBindBufferMemory2KHR), HK(vkMapMemory), HK(vkUnmapMemory),
    HK(vkUpdateDescriptorSets), HK(vkUpdateDescriptorSetWithTemplate),
    HK(vkBeginCommandBuffer), HK(vkEndCommandBuffer), HK(vkCmdBeginRenderPass), HK(vkCmdEndRenderPass),
    HK(vkCmdBeginRendering), HK(vkCmdBeginRenderingKHR), HK(vkCmdEndRendering), HK(vkCmdEndRenderingKHR),
    HK(vkCmdBindPipeline), HK(vkCmdBindDescriptorSets), HK(vkCmdBindVertexBuffers), HK(vkCmdBindIndexBuffer),
    HK(vkCmdPushConstants), HK(vkCmdPushDescriptorSetKHR), HK(vkCmdDraw), HK(vkCmdDrawIndexed),
    HK(vkCmdSetViewport), HK(vkCmdSetScissor), HK(vkCmdCopyBufferToImage), HK(vkCmdPipelineBarrier),
    HK(vkCmdClearAttachments), HK(vkCmdBlitImage), HK(vkCmdCopyImage),
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
