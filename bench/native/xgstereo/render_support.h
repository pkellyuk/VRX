// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Shared Vulkan fixture support for offscreen and swapchain integration tests.
#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include "test_shaders.h"
#include "pair_transport.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace xg::render_test {
constexpr uint32_t Width = 128, Height = 96;
inline void Require(bool condition, const char* what)
{
    if (!condition) throw std::runtime_error(what);
}
inline void Vk(VkResult result, const char* what)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": VkResult " + std::to_string(result));
}

#define DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkGetDeviceQueue) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkQueueSubmit) X(vkWaitForFences) X(vkCmdCopyImage)

struct Gpu {
    HMODULE loader = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProc = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
#define MEMBER(fn) PFN_##fn fn = nullptr;
    DEVICE_FUNCTIONS(MEMBER)
#undef MEMBER
    void Init(bool presentation = false, uint32_t graphicsQueues = 1, bool inlineShader = false, bool interop = false, bool maintenance6 = false, bool integer64 = false)
    {
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(loader != nullptr, "Vulkan loader unavailable (install a graphics driver with Vulkan support)");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        Require(gipa != nullptr, "missing vkGetInstanceProcAddr");
        const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        Require(create != nullptr, "missing vkCreateInstance");
        const char* layer = "VK_LAYER_VRX_xgstereo";
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "VRX xgstereo test"; app.apiVersion = presentation ? VK_API_VERSION_1_3 : VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app; ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer;
        const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface"};
        if (presentation) { ci.enabledExtensionCount = 2; ci.ppEnabledExtensionNames = instanceExtensions; }
        Vk(create(&ci, nullptr, &instance), "vkCreateInstance (explicit VRX layer required)");
#define INSTANCE_FN(fn) const auto fn = reinterpret_cast<PFN_##fn>(gipa(instance, #fn)); Require(fn != nullptr, #fn)
        destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
        INSTANCE_FN(vkEnumeratePhysicalDevices);
        INSTANCE_FN(vkGetPhysicalDeviceProperties);
        INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
        INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
        INSTANCE_FN(vkCreateDevice);
#undef INSTANCE_FN
        uint32_t count = 0;
        Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPU count");
        Require(count > 0, "no Vulkan devices");
        std::vector<VkPhysicalDevice> devices(count);
        Vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
        for (const auto candidate : devices)
        {
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, queues.data());
            for (uint32_t i = 0; i < n; ++i)
                if (queues[i].queueCount >= graphicsQueues && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                { physical = candidate; family = i; break; }
            if (physical) break;
        }
        Require(physical != VK_NULL_HANDLE, "no graphics queue");
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        std::printf("GPU: %s\n", props.deviceName);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        const float priority[2] = {1, 1};
        Require(graphicsQueues >= 1 && graphicsQueues <= 2, "test queue count is one or two");
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family; q.queueCount = graphicsQueues; q.pQueuePriorities = priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &q;
        VkPhysicalDeviceFeatures basic{}; basic.shaderInt64=integer64 ? VK_TRUE : VK_FALSE; di.pEnabledFeatures=&basic;
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.synchronization2 = VK_TRUE; features13.pNext = &features12;
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
        library.graphicsPipelineLibrary = VK_TRUE;
        VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT identifiers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT};
        identifiers.shaderModuleIdentifier = VK_TRUE; library.pNext = &identifiers;
        if (inlineShader) { features12.pNext = &library; features13.pipelineCreationCacheControl = VK_TRUE; }
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
            VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME};
        if (presentation) { di.pNext = &features13; di.enabledExtensionCount = inlineShader ? 5 : 2; di.ppEnabledExtensionNames = extensions; }
        std::vector<const char*> interopExtensions;
        if (interop)
        {
            interopExtensions.assign(di.ppEnabledExtensionNames, di.ppEnabledExtensionNames + di.enabledExtensionCount);
            interopExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
            interopExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
            di.enabledExtensionCount = uint32_t(interopExtensions.size()); di.ppEnabledExtensionNames = interopExtensions.data();
            features12.timelineSemaphore = VK_TRUE;
        }
        VkPhysicalDeviceMaintenance6FeaturesKHR m6{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
        std::vector<const char*> maintenanceExtensions;
        if (maintenance6) {
            m6.maintenance6 = VK_TRUE; m6.pNext=features12.pNext; features12.pNext=&m6;
            maintenanceExtensions.assign(di.ppEnabledExtensionNames,di.ppEnabledExtensionNames+di.enabledExtensionCount);
            maintenanceExtensions.push_back(VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
            di.enabledExtensionCount=uint32_t(maintenanceExtensions.size()); di.ppEnabledExtensionNames=maintenanceExtensions.data();
        }
        Vk(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");
        Require(!features12.bufferDeviceAddress && features13.pNext == &features12, "layer must not mutate app-owned feature structures");
#define LOAD(fn) fn = reinterpret_cast<PFN_##fn>(getDeviceProc(device, #fn)); Require(fn != nullptr, #fn);
        DEVICE_FUNCTIONS(LOAD)
#undef LOAD
        vkGetDeviceQueue(device, family, 0, &queue);
    }
    uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags)
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no suitable GPU memory type");
    }
    void Allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags flags, VkDeviceMemory& out)
    {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = needs.size; ai.memoryTypeIndex = MemoryType(needs.memoryTypeBits, flags);
        Vk(vkAllocateMemory(device, &ai, nullptr, &out), "vkAllocateMemory");
    }
    ~Gpu()
    {
        if (device && vkDestroyDevice) vkDestroyDevice(device, nullptr);
        if (instance && destroyInstance) destroyInstance(instance, nullptr);
        if (loader) FreeLibrary(loader);
    }
};

struct Scene {
    Gpu& gpu;
    VkBuffer vertices = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE, readMemory = VK_NULL_HANDLE, imageMemory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE; // replayed unchanged to verify live eye updates
    explicit Scene(Gpu& g) : gpu(g) {}
    void Buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = usage;
        Vk(gpu.vkCreateBuffer(gpu.device, &bi, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements needs{}; gpu.vkGetBufferMemoryRequirements(gpu.device, buffer, &needs);
        gpu.Allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory);
        Vk(gpu.vkBindBufferMemory(gpu.device, buffer, memory, 0), "vkBindBufferMemory");
    }
    VkShaderModule Shader(const std::vector<uint32_t>& code)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * sizeof(uint32_t); ci.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        Vk(gpu.vkCreateShaderModule(gpu.device, &ci, nullptr, &shader), "vkCreateShaderModule");
        return shader;
    }
    std::vector<uint8_t> Draw(bool block, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, bool unsupportedShader = false, bool inlineShader = false,
        float fixedEyeOffset = 0, float simulationX = 0)
    {
        std::vector<float> positions;
        const int corners[6][2] = {{0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1}};
        const float depth[3] = {2, 4, 1};
        for (int post = 0; post < 3; ++post)
            for (const auto& corner : corners)
            {
                const float w = depth[post];
                const float x = corner[0] ? .0625f : -.0625f;
                const float y = -.6f + .6f * post + (corner[1] ? .1f : -.1f);
                positions.insert(positions.end(), {(x + simulationX) * w, y * w, .5f * w, w});
            }
        Buffer(positions.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, vertexMemory);
        void* mapped = nullptr;
        Vk(gpu.vkMapMemory(gpu.device, vertexMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "map vertices");
        std::memcpy(mapped, positions.data(), positions.size() * sizeof(float));
        gpu.vkUnmapMemory(gpu.device, vertexMemory);
        Buffer(Width * Height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readMemory);
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format;
        ii.extent = {Width, Height, 1}; ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Vk(gpu.vkCreateImage(gpu.device, &ii, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements needs{}; gpu.vkGetImageMemoryRequirements(gpu.device, image, &needs);
        gpu.Allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemory);
        Vk(gpu.vkBindImageMemory(gpu.device, image, imageMemory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Vk(gpu.vkCreateImageView(gpu.device, &vi, nullptr, &view), "vkCreateImageView");
        VkAttachmentDescription attachment{};
        attachment.format = ii.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
        VkSubpassDependency dep{};
        dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 1; ri.pAttachments = &attachment; ri.subpassCount = 1; ri.pSubpasses = &subpass;
        ri.dependencyCount = 1; ri.pDependencies = &dep;
        Vk(gpu.vkCreateRenderPass(gpu.device, &ri, nullptr, &pass), "vkCreateRenderPass");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = pass; fi.attachmentCount = 1; fi.pAttachments = &view; fi.width = Width; fi.height = Height; fi.layers = 1;
        Vk(gpu.vkCreateFramebuffer(gpu.device, &fi, nullptr, &framebuffer), "vkCreateFramebuffer");
        auto vertexCode = unsupportedShader ? xg::test::MultipleVertexEntries() : xg::test::Vertex(block);
        if (fixedEyeOffset) Require(xg::PatchClipX(vertexCode, fixedEyeOffset).changed, "patch cooperative eye pipeline");
        const auto fragmentCode = xg::test::Fragment();
        VkShaderModuleCreateInfo inlineVert{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        inlineVert.codeSize = vertexCode.size() * sizeof(uint32_t); inlineVert.pCode = vertexCode.data();
        VkShaderModuleCreateInfo inlineFrag{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        inlineFrag.codeSize = fragmentCode.size() * sizeof(uint32_t); inlineFrag.pCode = fragmentCode.data();
        if (!inlineShader) { vert = Shader(vertexCode); frag = Shader(fragmentCode); }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";
        if (inlineShader) { stages[0].pNext = &inlineVert; stages[1].pNext = &inlineFrag; }
        const VkVertexInputBindingDescription binding{0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 1; input.pVertexAttributeDescriptions = &attribute;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const VkViewport viewport{0, 0, float(Width), float(Height), 0, 1};
        const VkRect2D scissor{{0, 0}, {Width, Height}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1; vp.pViewports = &viewport; vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bs.attachmentCount = 1; bs.pAttachments = &blend;
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        Vk(gpu.vkCreatePipelineLayout(gpu.device, &li, nullptr, &layout), "vkCreatePipelineLayout");
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount = 2; pi.pStages = stages; pi.pVertexInputState = &input; pi.pInputAssemblyState = &assembly;
        pi.pViewportState = &vp; pi.pRasterizationState = &raster; pi.pMultisampleState = &ms; pi.pColorBlendState = &bs;
        pi.layout = layout; pi.renderPass = pass;
        Vk(gpu.vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "vkCreateGraphicsPipelines");
        Require(inlineVert.pCode == vertexCode.data() && inlineVert.codeSize == vertexCode.size() * sizeof(uint32_t) &&
            (!inlineShader || (stages[0].pNext == &inlineVert && !stages[0].module)), "inline app shader structures remain untouched");
        if (inlineShader)
        {
            const auto fromCode = reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(gpu.getDeviceProc(gpu.device, "vkGetShaderModuleCreateInfoIdentifierEXT"));
            const auto fromModule = reinterpret_cast<PFN_vkGetShaderModuleIdentifierEXT>(gpu.getDeviceProc(gpu.device, "vkGetShaderModuleIdentifierEXT"));
            Require(fromCode && fromModule, "shader identifier functions available");
            const auto module = Shader(vertexCode);
            VkShaderModuleIdentifierEXT a{VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT}, b{VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
            fromCode(gpu.device, &inlineVert, &a); fromModule(gpu.device, module, &b);
            gpu.vkDestroyShaderModule(gpu.device, module, nullptr);
            Require(a.identifierSize && a.identifierSize == b.identifierSize && !std::memcmp(a.identifier, b.identifier, a.identifierSize),
                "inline code identifiers match the patched real module, including live eye address");
            Require(vertexCode == (unsupportedShader ? xg::test::MultipleVertexEntries() : xg::test::Vertex(block)), "app-owned shader code remains unchanged");
        }
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = gpu.family;
        Vk(gpu.vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        Vk(gpu.vkAllocateCommandBuffers(gpu.device, &ca, &command), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Vk(gpu.vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        const VkClearValue clear{};
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = pass; rb.framebuffer = framebuffer; rb.renderArea = scissor; rb.clearValueCount = 1; rb.pClearValues = &clear;
        gpu.vkCmdBeginRenderPass(command, &rb, VK_SUBPASS_CONTENTS_INLINE);
        gpu.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkDeviceSize zero = 0;
        gpu.vkCmdBindVertexBuffers(command, 0, 1, &vertices, &zero);
        gpu.vkCmdDraw(command, 18, 1, 0, 0);
        gpu.vkCmdEndRenderPass(command);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {Width, Height, 1};
        gpu.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        gpu.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        Vk(gpu.vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Vk(gpu.vkCreateFence(gpu.device, &fc, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        Vk(gpu.vkQueueSubmit(gpu.queue, 1, &submit, fence), "vkQueueSubmit");
        Vk(gpu.vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 10000000000ull), "vkWaitForFences");
        Vk(gpu.vkMapMemory(gpu.device, readMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "map result");
        const auto* pixels = static_cast<const uint8_t*>(mapped);
        std::vector<uint8_t> result(pixels, pixels + Width * Height * 4);
        gpu.vkUnmapMemory(gpu.device, readMemory);
        return result;
    }
    ~Scene()
    {
        gpu.vkDeviceWaitIdle(gpu.device);
        if (fence) gpu.vkDestroyFence(gpu.device, fence, nullptr);
        if (pool) gpu.vkDestroyCommandPool(gpu.device, pool, nullptr);
        if (pipeline) gpu.vkDestroyPipeline(gpu.device, pipeline, nullptr);
        if (layout) gpu.vkDestroyPipelineLayout(gpu.device, layout, nullptr);
        if (vert) gpu.vkDestroyShaderModule(gpu.device, vert, nullptr);
        if (frag) gpu.vkDestroyShaderModule(gpu.device, frag, nullptr);
        if (framebuffer) gpu.vkDestroyFramebuffer(gpu.device, framebuffer, nullptr);
        if (pass) gpu.vkDestroyRenderPass(gpu.device, pass, nullptr);
        if (view) gpu.vkDestroyImageView(gpu.device, view, nullptr);
        if (image) gpu.vkDestroyImage(gpu.device, image, nullptr);
        if (vertices) gpu.vkDestroyBuffer(gpu.device, vertices, nullptr);
        if (readback) gpu.vkDestroyBuffer(gpu.device, readback, nullptr);
        if (vertexMemory) gpu.vkFreeMemory(gpu.device, vertexMemory, nullptr);
        if (readMemory) gpu.vkFreeMemory(gpu.device, readMemory, nullptr);
        if (imageMemory) gpu.vkFreeMemory(gpu.device, imageMemory, nullptr);
    }
};

inline std::vector<uint8_t> Render(const char* eye, bool block)
{
    Require(SetEnvironmentVariableA("VRX_XG_EYE", eye) != 0, "set test eye");
    Require(SetEnvironmentVariableA("VRX_XG_SHIFT", "0.125") != 0, "set test shift");
    Gpu gpu; gpu.Init();
    Scene scene(gpu);
    return scene.Draw(block);
}

inline double Centroid(const std::vector<uint8_t>& pixels, uint32_t post)
{
    double sum = 0; uint32_t count = 0;
    for (uint32_t y = post * 32; y < (post + 1) * 32; ++y)
        for (uint32_t x = 0; x < Width; ++x)
        {
            const size_t at = (size_t(y) * Width + x) * 4;
            if (pixels[at] > 200 && pixels[at + 1] < 10) { sum += x; ++count; }
        }
    Require(count >= 64, "post missing from rendered image");
    return sum / count;
}

inline void SavePair(const char* path, const std::vector<uint8_t>& left, const std::vector<uint8_t>& right)
{
    FILE* file = nullptr;
    Require(fopen_s(&file, path, "wb") == 0 && file, "open SBS test snapshot");
    bool ok = std::fprintf(file, "P6\n%u %u\n255\n", Width * 2, Height) > 0;
    for (uint32_t y = 0; y < Height; ++y)
        for (const auto* eye : {&left, &right})
            for (uint32_t x = 0; x < Width; ++x)
                ok = (std::fwrite(eye->data() + (size_t(y) * Width + x) * 4, 1, 3, file) == 3) && ok;
    ok = (std::fclose(file) == 0) && ok;
    Require(ok, "write SBS test snapshot");
}
} // namespace
