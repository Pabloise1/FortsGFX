// Tests src/vulkan.cpp without the game: like Forts, it imports opengl32.dll (FortsGFX's, next to it in build\), loads
// vulkan-1.dll and gets vkGetInstanceProcAddr through GetProcAddress, then draws ~13 s of frames into a window.
//   vktest        a render pass that clears to a colour that changes, a mapped vertex buffer bound and a barrier
//   vktest srgb   the same with an sRGB swapchain
//   vktest split  drawn the way Forts draws a battle - dynamic rendering with a depth buffer, Forts' own shaders
//                 (data\shaders\vertsscrolltex.spv + colourfrag.spv), a push-constant matrix per draw - a "world"
//                 quad over the whole window, then a yellow "HUD" quad under Forts' HUD matrix. With [Vulkan]
//                 TestSplitAt=2 the frame is split before the HUD quad: it must come out exactly yellow, the world
//                 quad with FortsGFX's look.
// Afterwards build\FortsGFX\vulkan.log describes it all, and vk_frame_01.txt / .png hold the dumped frame.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <GL/gl.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s -> %d\n", #x, r_); return 1; } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IFN(n) auto n = (PFN_##n)gipa(inst, #n)
#define DFN(n) auto n = (PFN_##n)gdpa(dev, #n)

static std::vector<char> ReadFile(const char* path) {
    std::vector<char> b;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        b.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(b.data(), 1, b.size(), f);
        fclose(f);
    }
    return b;
}

int main(int argc, char** argv) {
    bool srgb = argc > 1 && !strcmp(argv[1], "srgb"), replay = argc > 1 && !strcmp(argv[1], "replay");
    bool split = replay || (argc > 1 && !strcmp(argv[1], "split"));
    VkFormat fmt = srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    glFlush();   // a real import of opengl32.dll, so FortsGFX's loads as it does in Forts
    HWND wnd = CreateWindowExW(0, L"STATIC", L"FortsGFX vktest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 450, nullptr, nullptr, nullptr, nullptr);
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr\n"); return 1; }

    auto vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    const char* iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "vktest", 1, "none", 1, split ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2 };
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
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, split ? &f13 : nullptr, 0, 1, &qci, 0, nullptr, 1, dext, nullptr };
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateSwapchainKHR); DFN(vkGetSwapchainImagesKHR); DFN(vkCreateImageView); DFN(vkCreateRenderPass);
    DFN(vkCreateFramebuffer); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkCreateSemaphore); DFN(vkCreateFence);
    DFN(vkAcquireNextImageKHR); DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
    DFN(vkCmdBeginRenderPass); DFN(vkCmdEndRenderPass); DFN(vkQueueSubmit); DFN(vkQueuePresentKHR); DFN(vkDeviceWaitIdle);
    DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory);
    DFN(vkCmdBindVertexBuffers); DFN(vkCmdPipelineBarrier); DFN(vkResetCommandBuffer);
    DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkBindImageMemory); DFN(vkCreateShaderModule);
    DFN(vkCreateDescriptorSetLayout); DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines);
    DFN(vkCmdBeginRendering); DFN(vkCmdEndRendering); DFN(vkCmdBindPipeline); DFN(vkCmdPushConstants); DFN(vkCmdDraw);
    DFN(vkCmdSetViewport); DFN(vkCmdSetScissor); DFN(vkCmdSetLineWidth);
    DFN(vkCreateSampler); DFN(vkCreateDescriptorPool); DFN(vkAllocateDescriptorSets); DFN(vkUpdateDescriptorSets);
    DFN(vkCmdBindDescriptorSets); DFN(vkCmdCopyBufferToImage); DFN(vkQueueWaitIdle); DFN(vkCmdExecuteCommands);
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
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    auto memType = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return 0u;
    };

    VkAttachmentDescription att = { 0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref };
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));
    std::vector<VkFramebuffer> fbs(ni);
    std::vector<VkImageView> views(ni);
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, imgs[i], VK_IMAGE_VIEW_TYPE_2D, fmt,
                                      {}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dev, &vci, nullptr, &views[i]));
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, rp, 1, &views[i], ext.width, ext.height, 1 };
        CHECK(vkCreateFramebuffer(dev, &fci, nullptr, &fbs[i]));
    }

    // a host-visible vertex buffer, as the game streams its sprites: pos.xy, uv.xy, colour.rgba (Forts' 32-byte vertex)
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 65536, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer vb;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &vb));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, vb, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CHECK(vkBindBufferMemory(dev, vb, mem, 0));
    float* vp;
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&vp));
    const float W = (float)ext.width, H = (float)ext.height;
    const float quads[8][8] = {   // world quad over the window (pixels), HUD quad in Forts' 1066 x 600 HUD space
        { 0, 0, 0, 0, 0.35f, 0.5f, 0.75f, 1 }, { 0, H, 0, 1, 0.35f, 0.5f, 0.75f, 1 }, { W, 0, 1, 0, 0.35f, 0.5f, 0.75f, 1 }, { W, H, 1, 1, 0.35f, 0.5f, 0.75f, 1 },
        { 100, 60, 0, 0, 1, 1, 0, 1 }, { 100, 200, 0, 1, 1, 1, 0, 1 }, { 400, 60, 1, 0, 1, 1, 0, 1 }, { 400, 200, 1, 1, 1, 1, 0, 1 } };
    memcpy(vp, quads, sizeof(quads));
    // vktest replay: a backdrop quad (pixels) at vertex 8, then struts and ground as triangles in world units at 12 - the
    // camera shows x -1700..1700, y -900..900 (down), as Forts' does
    float* rv = vp + 8 * 8;
    auto put = [&](float x, float y, float u, float v) { float p[8] = { x, y, u, v, 1, 1, 1, 1 }; memcpy(rv, p, sizeof(p)); rv += 8; };
    put(0, 0, 0, 0); put(0, H, 0, 1); put(W, 0, 1, 0); put(W, H, 1, 1);
    const float rects[4][4] = { { -600, -500, -500, 700 }, { 500, -500, 600, 700 }, { -600, -550, 600, -450 }, { -1700, 700, 1700, 900 } };
    for (auto& q : rects) {
        put(q[0], q[1], q[0] / 100, q[1] / 100); put(q[2], q[1], q[2] / 100, q[1] / 100); put(q[0], q[3], q[0] / 100, q[3] / 100);
        put(q[0], q[3], q[0] / 100, q[3] / 100); put(q[2], q[1], q[2] / 100, q[1] / 100); put(q[2], q[3], q[2] / 100, q[3] / 100);
    }

    // Forts' pipeline for a battle sprite (split mode)
    VkImage depth = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE, pipeTex = VK_NULL_HANDLE, pipeTri = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslReplay = VK_NULL_HANDLE, dslUbo = VK_NULL_HANDLE;
    VkPipelineLayout layoutTerrain = VK_NULL_HANDLE;
    VkPipeline pipeTerrain = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (split) {
        VkImageCreateInfo dic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_D16_UNORM, { ext.width, ext.height, 1 }, 1, 1,
                                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE };
        CHECK(vkCreateImage(dev, &dic, nullptr, &depth));
        VkMemoryRequirements dr;
        vkGetImageMemoryRequirements(dev, depth, &dr);
        VkMemoryAllocateInfo dai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, dr.size, memType(dr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        VkDeviceMemory dmem;
        CHECK(vkAllocateMemory(dev, &dai, nullptr, &dmem));
        CHECK(vkBindImageMemory(dev, depth, dmem, 0));
        VkImageViewCreateInfo dvc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, depth, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D16_UNORM,
                                      {}, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dev, &dvc, nullptr, &depthView));
        const char* dir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Forts\\data\\shaders\\";
        std::vector<char> vs = ReadFile((std::string(dir) + "vertsscrolltex.spv").c_str()), fs = ReadFile((std::string(dir) + "colourfrag.spv").c_str());
        if (vs.empty() || fs.empty()) { printf("Forts' shaders not found\n"); return 1; }
        VkShaderModule vsm, fsm;
        VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, vs.size(), (const uint32_t*)vs.data() };
        CHECK(vkCreateShaderModule(dev, &smi, nullptr, &vsm));
        smi.codeSize = fs.size(); smi.pCode = (const uint32_t*)fs.data();
        CHECK(vkCreateShaderModule(dev, &smi, nullptr, &fsm));
        VkDescriptorSetLayoutBinding dsb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
        VkDescriptorSetLayoutCreateInfo dsli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsb };
        VkDescriptorSetLayout dsl;
        CHECK(vkCreateDescriptorSetLayout(dev, &dsli, nullptr, &dsl));
        VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pcr };
        CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &layout));
        VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main" },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main" } };
        VkVertexInputBindingDescription vbd = { 0, 32, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vad[3] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 }, { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 } };
        VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &vbd, 3, vad };
        VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA; cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba };
        VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH };
        VkPipelineDynamicStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 3, dyn };
        VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, 1, &fmt, VK_FORMAT_D16_UNORM };
        VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, st, &vis, &ias, nullptr, &vps, &rs, &ms, &ds, &cbs, &dss, layout };
        CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
        if (replay) {   // textured: frag.spv, as a strip (the backdrop) and as triangles (struts)
            std::vector<char> ft = ReadFile((std::string(dir) + "frag.spv").c_str());
            VkShaderModule ftm;
            smi.codeSize = ft.size(); smi.pCode = (const uint32_t*)ft.data();
            CHECK(vkCreateShaderModule(dev, &smi, nullptr, &ftm));
            st[1].module = ftm;
            CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeTex));
            ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeTri));
            dslReplay = dsl;
            std::vector<char> vt = ReadFile((std::string(dir) + "verts.spv").c_str());
            VkShaderModule vtm;
            smi.codeSize = vt.size(); smi.pCode = (const uint32_t*)vt.data();
            CHECK(vkCreateShaderModule(dev, &smi, nullptr, &vtm));
            VkDescriptorSetLayoutBinding ub = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT };
            VkDescriptorSetLayoutCreateInfo uli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &ub };
            CHECK(vkCreateDescriptorSetLayout(dev, &uli, nullptr, &dslUbo));
            VkDescriptorSetLayout two[2] = { dsl, dslUbo };
            VkPipelineLayoutCreateInfo tli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 2, two, 0, nullptr };
            CHECK(vkCreatePipelineLayout(dev, &tli, nullptr, &layoutTerrain));
            st[0].module = vtm;
            gpi.layout = layoutTerrain;
            CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeTerrain));
        }
    }

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

    // vktest replay: two 64x64 textures uploaded through a staging buffer as Forts uploads (FortsGFX mirrors them in GL) -
    // a sky gradient for the backdrop and a strut texture - and a descriptor set for each
    VkDescriptorSet texSet[2] = {}, uboSet = VK_NULL_HANDLE;
    VkCommandBuffer terrainCmd = VK_NULL_HANDLE;
    if (replay) {
        const uint32_t TS = 64;
        VkBufferCreateInfo sbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 2 * TS * TS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE };
        VkBuffer stage;
        CHECK(vkCreateBuffer(dev, &sbi, nullptr, &stage));
        VkMemoryRequirements sr;
        vkGetBufferMemoryRequirements(dev, stage, &sr);
        VkMemoryAllocateInfo sai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, sr.size, memType(sr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory smem;
        CHECK(vkAllocateMemory(dev, &sai, nullptr, &smem));
        CHECK(vkBindBufferMemory(dev, stage, smem, 0));
        uint8_t* px;
        CHECK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void**)&px));
        for (uint32_t y = 0; y < TS; y++)
            for (uint32_t x = 0; x < TS; x++) {
                uint8_t* a = px + (y * TS + x) * 4, *b = a + TS * TS * 4;
                a[0] = (uint8_t)(70 + 100 * y / TS); a[1] = (uint8_t)(110 + 70 * y / TS); a[2] = 190; a[3] = 255;   // sky
                bool edge = x < 6 || x >= TS - 6 || y < 6 || y >= TS - 6;
                b[0] = edge ? 90 : 150; b[1] = edge ? 60 : 100; b[2] = edge ? 30 : 55; b[3] = 255;                   // strut
            }
        VkImage timg[2];
        VkImageView tview[2];
        VkCommandBuffer up;
        CHECK(vkAllocateCommandBuffers(dev, &cai, &up));
        VkCommandBufferBeginInfo ubi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(up, &ubi);
        for (int i = 0; i < 2; i++) {
            VkImageCreateInfo tic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { TS, TS, 1 }, 1, 1,
                                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
            CHECK(vkCreateImage(dev, &tic, nullptr, &timg[i]));
            VkMemoryRequirements tr;
            vkGetImageMemoryRequirements(dev, timg[i], &tr);
            VkMemoryAllocateInfo tai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, tr.size, memType(tr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
            VkDeviceMemory tm;
            CHECK(vkAllocateMemory(dev, &tai, nullptr, &tm));
            CHECK(vkBindImageMemory(dev, timg[i], tm, 0));
            VkImageViewCreateInfo tvc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, timg[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                          {}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            CHECK(vkCreateImageView(dev, &tvc, nullptr, &tview[i]));
            VkImageMemoryBarrier tb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, timg[i], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
            VkBufferImageCopy reg = {};
            reg.bufferOffset = (VkDeviceSize)i * TS * TS * 4;
            reg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            reg.imageExtent = { TS, TS, 1 };
            vkCmdCopyBufferToImage(up, stage, timg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
            tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
        }
        vkEndCommandBuffer(up);
        VkSubmitInfo usi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &up };
        CHECK(vkQueueSubmit(q, 1, &usi, VK_NULL_HANDLE));
        vkQueueWaitIdle(q);
        VkSamplerCreateInfo sci2 = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                     VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
        sci2.maxLod = 16.0f;
        VkSampler smp;
        CHECK(vkCreateSampler(dev, &sci2, nullptr, &smp));
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2, 1, &ps };
        VkDescriptorPool dp;
        CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
        VkDescriptorSetLayout two[2] = { dslReplay, dslReplay };
        VkDescriptorSetAllocateInfo dai2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, dp, 2, two };
        CHECK(vkAllocateDescriptorSets(dev, &dai2, texSet));
        for (int i = 0; i < 2; i++) {
            VkDescriptorImageInfo dii = { smp, tview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, texSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii };
            vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);
        }
        // the ground as Forts draws terrain: a secondary command buffer recorded once, verts.spv with the camera in a
        // uniform buffer (set 1)
        VkBufferCreateInfo ubi2 = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE };
        VkBuffer ubo;
        CHECK(vkCreateBuffer(dev, &ubi2, nullptr, &ubo));
        VkMemoryRequirements ur;
        vkGetBufferMemoryRequirements(dev, ubo, &ur);
        VkMemoryAllocateInfo uai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, ur.size, memType(ur.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory umem;
        CHECK(vkAllocateMemory(dev, &uai, nullptr, &umem));
        CHECK(vkBindBufferMemory(dev, ubo, umem, 0));
        float* um;
        CHECK(vkMapMemory(dev, umem, 0, VK_WHOLE_SIZE, 0, (void**)&um));
        const float camU[16] = { 2 / 3400.0f, 0, 0, 0, 0, -2 / 1800.0f, 0, 0, 0, 0, -0.001f, 0, 0, 0, 0, 1 };
        memcpy(um, camU, sizeof(camU));
        VkDescriptorPoolSize ups = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
        VkDescriptorPoolCreateInfo udp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &ups };
        VkDescriptorPool upool;
        CHECK(vkCreateDescriptorPool(dev, &udp, nullptr, &upool));
        VkDescriptorSetAllocateInfo uda = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, upool, 1, &dslUbo };
        CHECK(vkAllocateDescriptorSets(dev, &uda, &uboSet));
        VkDescriptorBufferInfo dbi = { ubo, 0, 64 };
        VkWriteDescriptorSet uw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, uboSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &dbi };
        vkUpdateDescriptorSets(dev, 1, &uw, 0, nullptr);
        VkCommandBufferAllocateInfo sai2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1 };
        CHECK(vkAllocateCommandBuffers(dev, &sai2, &terrainCmd));
        VkCommandBufferInheritanceRenderingInfo iri = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, nullptr, 0, 0, 1, &fmt, VK_FORMAT_D16_UNORM };
        iri.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkCommandBufferInheritanceInfo ii = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &iri };
        VkCommandBufferBeginInfo sbi2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &ii };
        vkBeginCommandBuffer(terrainCmd, &sbi2);
        VkViewport tvp = { 0, H, W, -H, 0, 1 };
        VkRect2D tsc = { { 0, 0 }, ext };
        vkCmdSetViewport(terrainCmd, 0, 1, &tvp);
        vkCmdSetScissor(terrainCmd, 0, 1, &tsc);
        vkCmdSetLineWidth(terrainCmd, 1.0f);
        vkCmdBindPipeline(terrainCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeTerrain);
        VkDescriptorSet both[2] = { texSet[1], uboSet };
        vkCmdBindDescriptorSets(terrainCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutTerrain, 0, 2, both, 0, nullptr);
        VkDeviceSize toff = 0;
        vkCmdBindVertexBuffers(terrainCmd, 0, 1, &vb, &toff);
        vkCmdDraw(terrainCmd, 6, 1, 30, 0);   // the ground rectangle
        vkEndCommandBuffer(terrainCmd);
    }

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
        if (split) {
            VkImageMemoryBarrier ib[2] = {
                { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[idx], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
                { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, depth, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 2, ib);
            VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, nullptr, views[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, nullptr, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = { 1.0f, 0 };
            VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, nullptr, 0, { { 0, 0 }, ext }, 1, 0, 1, &ca, &da };
            vkCmdBeginRendering(cb, &ri);
            VkViewport vpt = { 0, H, W, -H, 0, 1 };   // flipped, as Forts sets it
            VkRect2D scr = { { 0, 0 }, ext };
            VkDeviceSize off = 0;
            auto state = [&] {
                vkCmdSetViewport(cb, 0, 1, &vpt);
                vkCmdSetScissor(cb, 0, 1, &scr);
                vkCmdSetLineWidth(cb, 1.0f);
                vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
            };
            state();
            const float world[16] = { 2 / W, 0, 0, 0, 0, -2 / H, 0, 0, 0, 0, -0.001f, 0, -1, 1, 0, 1 };
            const float hud[16] = { 2 / (600.0f * W / H), 0, 0, 0, 0, -2 / 600.0f, 0, 0, 0, 0, -0.001f, 0, -1, 1, 0, 1 };
            if (replay) {
                const float back[16] = { 2 / W, 0, 0, 0, 0, -2 / H, 0, 0, 0, 0, -0.001f, 0, -1.02f, 1.03f, 0, 1 };   // a backdrop layer
                const float cam[16] = { 2 / 3400.0f, 0, 0, 0, 0, -2 / 1800.0f, 0, 0, 0, 0, -0.001f, 0, 0, 0, 0, 1 };   // the camera
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeTex);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &texSet[0], 0, nullptr);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, back);
                vkCmdDraw(cb, 4, 1, 8, 0);
                vkCmdEndRendering(cb);
                ca.loadOp = da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;   // the terrain pass
                vkCmdBeginRendering(cb, &ri);
                vkCmdExecuteCommands(cb, 1, &terrainCmd);
                vkCmdEndRendering(cb);
                ri.flags = 0;
                vkCmdBeginRendering(cb, &ri);
                state();
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeTri);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &texSet[1], 0, nullptr);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, cam);
                vkCmdDraw(cb, 18, 1, 12, 0);
                vkCmdEndRendering(cb);
                vkCmdBeginRendering(cb, &ri);
                state();
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, hud);
                vkCmdDraw(cb, 4, 1, 4, 0);   // the "HUD" (draw 3): FortsGFX splits the frame before it
            } else {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, world);
                vkCmdDraw(cb, 4, 1, 0, 0);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, hud);
                vkCmdDraw(cb, 4, 1, 4, 0);   // the "HUD": FortsGFX splits the frame before this one
            }
            vkCmdEndRendering(cb);
            VkImageMemoryBarrier pb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED,
                                        VK_QUEUE_FAMILY_IGNORED, imgs[idx], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
        } else {
            float t = (GetTickCount() - t0) / 13000.0f;
            VkClearValue cv;
            cv.color = { { 0.2f + 0.6f * t, 0.35f, 0.8f - 0.6f * t, 1.0f } };
            VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, rp, fbs[idx], { { 0, 0 }, ext }, 1, &cv };
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            VkDeviceSize off = 16;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
            vkCmdEndRenderPass(cb);
        }
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
