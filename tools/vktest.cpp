// Tests the Vulkan recon (src/vulkan.cpp) without the game: like Forts, it imports opengl32.dll (FortsGFX's, next to
// it in build\), loads vulkan-1.dll and gets vkGetInstanceProcAddr through GetProcAddress, then draws ~13 s of frames
// into a window: a render pass that clears to a colour that changes, a mapped vertex buffer bound and a barrier.
// Afterwards build\FortsGFX\vulkan.log should describe it all, and vk_frame_01.txt / .png hold the frame dumped 10 s in.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <GL/gl.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s -> %d\n", #x, r_); return 1; } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IFN(n) auto n = (PFN_##n)gipa(inst, #n)
#define DFN(n) auto n = (PFN_##n)gdpa(dev, #n)

int main(int argc, char** argv) {
    VkFormat fmt = argc > 1 && !strcmp(argv[1], "srgb") ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;   // vktest srgb: an sRGB swapchain
    glFlush();   // a real import of opengl32.dll, so FortsGFX's loads as it does in Forts
    HWND wnd = CreateWindowExW(0, L"STATIC", L"FortsGFX vktest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 450, nullptr, nullptr, nullptr, nullptr);
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr\n"); return 1; }

    auto vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    const char* iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "vktest", 1, "none", 1, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, iext };
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkCreateWin32SurfaceKHR); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceSurfaceSupportKHR); IFN(vkCreateDevice); IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(vkGetPhysicalDeviceMemoryProperties);
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
    VkPhysicalDevice pd = pds[0];
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, GetModuleHandleW(nullptr), wnd };
    VkSurfaceKHR surf;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surf));
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t fam = ~0u;
    for (uint32_t i = 0; i < nq && fam == ~0u; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &ok);
        if (ok && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) fam = i;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, fam, 1, &prio };
    const char* dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, dext, nullptr };
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateSwapchainKHR); DFN(vkGetSwapchainImagesKHR); DFN(vkCreateImageView); DFN(vkCreateRenderPass);
    DFN(vkCreateFramebuffer); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkCreateSemaphore); DFN(vkCreateFence);
    DFN(vkAcquireNextImageKHR); DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
    DFN(vkCmdBeginRenderPass); DFN(vkCmdEndRenderPass); DFN(vkQueueSubmit); DFN(vkQueuePresentKHR); DFN(vkDeviceWaitIdle);
    DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory);
    DFN(vkCmdBindVertexBuffers); DFN(vkCmdPipelineBarrier); DFN(vkResetCommandBuffer);
    VkQueue q;
    vkGetDeviceQueue(dev, fam, 0, &q);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    VkExtent2D ext = caps.currentExtent;
    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr, 0, surf, std::max(caps.minImageCount, 2u),
                                      fmt, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                      VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                      VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE };
    VkSwapchainKHR sc;
    CHECK(vkCreateSwapchainKHR(dev, &swci, nullptr, &sc));
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(dev, sc, &ni, nullptr);
    std::vector<VkImage> imgs(ni);
    vkGetSwapchainImagesKHR(dev, sc, &ni, imgs.data());

    VkAttachmentDescription att = { 0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref };
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));
    std::vector<VkFramebuffer> fbs(ni);
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, imgs[i], VK_IMAGE_VIEW_TYPE_2D, fmt,
                                      {}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView v;
        CHECK(vkCreateImageView(dev, &vci, nullptr, &v));
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, rp, 1, &v, ext.width, ext.height, 1 };
        CHECK(vkCreateFramebuffer(dev, &fci, nullptr, &fbs[i]));
    }

    // a host-visible vertex buffer, as the game streams its sprites
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 65536, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer vb;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &vb));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, vb, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CHECK(vkBindBufferMemory(dev, vb, mem, 0));
    float* vp;
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&vp));
    for (int i = 0; i < 64; i++) vp[i] = (float)i;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired, rendered;
    vkCreateSemaphore(dev, &semci, nullptr, &acquired);
    vkCreateSemaphore(dev, &semci, nullptr, &rendered);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
    VkFence fence;
    vkCreateFence(dev, &fci, nullptr, &fence);

    DWORD t0 = GetTickCount();
    unsigned frames = 0;
    while (GetTickCount() - t0 < 13000) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        uint32_t idx;
        CHECK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx));
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cb, &bi);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        float t = (GetTickCount() - t0) / 13000.0f;
        VkClearValue cv;
        cv.color = { { 0.2f + 0.6f * t, 0.35f, 0.8f - 0.6f * t, 1.0f } };
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, rp, fbs[idx], { { 0, 0 }, ext }, 1, &cv };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        VkDeviceSize off = 16;
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acquired, &ws, 1, &cb, 1, &rendered };
        CHECK(vkQueueSubmit(q, 1, &si, fence));
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &rendered, 1, &sc, &idx };
        CHECK(vkQueuePresentKHR(q, &pi));
        frames++;
    }
    vkDeviceWaitIdle(dev);
    DFN(vkDestroySwapchainKHR); DFN(vkDestroyDevice);
    vkDestroySwapchainKHR(dev, sc, nullptr);   // FortsGFX releases its own objects on the device first
    vkDestroyDevice(dev, nullptr);
    printf("vktest: %u frames presented in %ux%u\n", frames, ext.width, ext.height);
    Sleep(1500);   // the PNG is written on a worker thread
    return 0;
}
