// Final pass into the window (M5, included at the end of gpu_vk.cpp): the game image's crop rectangle is scaled into
// a (letterboxed) rectangle of the swapchain image with bilinear filtering or AMD FSR 1 (EASU upscale into an
// intermediate image, then RCAS sharpening), and the TAB overlay (ImGui) is drawn on top. Dynamic rendering.
#include "present_spv.h"

static VkDescriptorSetLayout g_pr_dsl; static VkPipelineLayout g_pr_pl; static VkSampler g_pr_smp;
enum { PR_BLIT, PR_EASU, PR_RCAS, PR_FG_LUMA, PR_FG_DOWN, PR_FG_FLOW, PR_FG_INTERP, PR_N };
static VkShaderModule g_pr_vs, g_pr_fs[PR_N];
static std::map<std::pair<int, int>, VkPipeline> g_pr_pipes;   // (shader, target format)
static VkDescriptorSetLayout g_fg_dsl; static VkPipelineLayout g_fg_pl;   // frame generation: 3 textures
static int g_pr_state;                               // 0 = not tried, 1 = ok, -1 = unavailable (old blit path)
static VkImage g_fsr_img; static VkDeviceMemory g_fsr_mem; static VkImageView g_fsr_view; static int g_fsr_w, g_fsr_h;
static const VkFormat k_fsr_fmt = VK_FORMAT_R8G8B8A8_UNORM;
static bool g_ui_ok, g_ui_tried;

static VkShaderModule pr_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes; mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_dev, &mi, nullptr, &m);
    return m;
}
static bool present_init() {
    if (g_pr_state) return g_pr_state > 0;
    g_pr_state = -1;
    if (!pfnCmdBeginRendering) { LOG(1, "[gpu] no dynamic rendering: window copy without FSR / overlay"); return false; }
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR; sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sc.maxLod = 0;
    if (vkCreateSampler(g_dev, &sc, nullptr, &g_pr_smp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 1; dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_pr_dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &g_pr_dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_pr_pl) != VK_SUCCESS) return false;
    g_pr_vs = pr_module(k_spv_fullscreen_vert, sizeof k_spv_fullscreen_vert);
    g_pr_fs[0] = pr_module(k_spv_blit_frag, sizeof k_spv_blit_frag);
    g_pr_fs[1] = pr_module(k_spv_easu_frag, sizeof k_spv_easu_frag);
    g_pr_fs[2] = pr_module(k_spv_rcas_frag, sizeof k_spv_rcas_frag);
    g_pr_fs[PR_FG_LUMA] = pr_module(k_spv_fg_luma_frag, sizeof k_spv_fg_luma_frag);
    g_pr_fs[PR_FG_DOWN] = pr_module(k_spv_fg_down_frag, sizeof k_spv_fg_down_frag);
    g_pr_fs[PR_FG_FLOW] = pr_module(k_spv_fg_flow_frag, sizeof k_spv_fg_flow_frag);
    g_pr_fs[PR_FG_INTERP] = pr_module(k_spv_fg_interp_frag, sizeof k_spv_fg_interp_frag);
    for (int i = 0; i < PR_N; i++) if (!g_pr_fs[i]) { LOG(0, "[gpu] present shader %d failed to load", i); return false; }
    if (!g_pr_vs) { LOG(0, "[gpu] present shaders failed to load"); return false; }
    {   // frame generation passes: up to 3 textures
        VkDescriptorSetLayoutBinding fb[3];
        for (uint32_t i = 0; i < 3; i++) fb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo fl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; fl.bindingCount = 3; fl.pBindings = fb;
        if (vkCreateDescriptorSetLayout(g_dev, &fl, nullptr, &g_fg_dsl) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo fpl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        fpl.setLayoutCount = 1; fpl.pSetLayouts = &g_fg_dsl; fpl.pushConstantRangeCount = 1; fpl.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(g_dev, &fpl, nullptr, &g_fg_pl) != VK_SUCCESS) return false;
    }
    g_pr_state = 1;
    LOG(1, "[gpu] present pass: bilinear / FSR 1 + overlay");
    return true;
}
static VkPipeline pr_pipeline(int which, VkFormat fmt) {
    auto it = g_pr_pipes.find({which, (int)fmt});
    if (it != g_pr_pipes.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_pr_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_pr_fs[which]; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {}; ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState ds[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = ds;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = which >= PR_FG_LUMA ? g_fg_pl : g_pr_pl;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) { LOG(0, "[gpu] present pipeline %d failed", which); return VK_NULL_HANDLE; }
    g_pr_pipes[{which, (int)fmt}] = p;
    return p;
}
static bool fsr_target(int w, int h) {
    if (g_fsr_img && g_fsr_w == w && g_fsr_h == h) return true;
    if (g_fsr_img) {
        vkDeviceWaitIdle(g_dev);
        vkDestroyImageView(g_dev, g_fsr_view, nullptr); vkDestroyImage(g_dev, g_fsr_img, nullptr); vkFreeMemory(g_dev, g_fsr_mem, nullptr);
        g_fsr_img = VK_NULL_HANDLE; g_fsr_view = VK_NULL_HANDLE; g_fsr_mem = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = k_fsr_fmt; ci.extent = {(uint32_t)w, (uint32_t)h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(g_dev, &ci, nullptr, &g_fsr_img) != VK_SUCCESS) { g_fsr_img = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, g_fsr_img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_dev, &mai, nullptr, &g_fsr_mem) != VK_SUCCESS) { vkDestroyImage(g_dev, g_fsr_img, nullptr); g_fsr_img = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(g_dev, g_fsr_img, g_fsr_mem, 0);
    g_fsr_view = make_view(g_fsr_img, VK_IMAGE_VIEW_TYPE_2D, k_fsr_fmt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    g_fsr_w = w; g_fsr_h = h;
    LOG(1, "[gpu] FSR intermediate %dx%d", w, h);
    return g_fsr_view != VK_NULL_HANDLE;
}
static VkDescriptorSet pr_set(VkImageView v, VkImageLayout layout) {
    VkDescriptorSet s = alloc_set(g_pr_dsl);
    if (!s) return s;
    VkDescriptorImageInfo ii = {g_pr_smp, v, layout};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
    return s;
}
static void pr_draw(int which, VkFormat fmt, VkDescriptorSet set, const float pc[12], int x, int y, int w, int h) {
    VkPipeline p = pr_pipeline(which, fmt);
    if (!p || !set) return;
    VkPipelineLayout pl = which >= PR_FG_LUMA ? g_fg_pl : g_pr_pl;
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    VkViewport vp = {(float)x, (float)y, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{x, y}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(g_cmd, 0, 1, &vp); vkCmdSetScissor(g_cmd, 0, 1, &sc);
    vkCmdDraw(g_cmd, 3, 1, 0, 0);
}
static void pr_begin(VkImageView view, uint32_t w, uint32_t h, bool clear) {
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    pfnCmdBeginRendering(g_cmd, &ri);
}

// ImGui's Vulkan backend loads its functions through us: command recording goes through the recorder stubs
static VKAPI_ATTR VkResult VKAPI_CALL ui_queue_wait_idle(VkQueue q) {
    pacer_flush(); rec_drain();
    static PFN_vkQueueWaitIdle real = (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(g_dev, "vkQueueWaitIdle");
    std::lock_guard<std::mutex> l(g_qm);
    return real(q);
}
static PFN_vkVoidFunction ui_loader(const char *name, void *) {
#define UI_LOOK(f) if (!strcmp(name, #f)) return (PFN_vkVoidFunction)f;
    VK_DEVICE(UI_LOOK) VK_INSTANCE(UI_LOOK)
#undef UI_LOOK
    if (!strcmp(name, "vkCmdBeginRendering") || !strcmp(name, "vkCmdBeginRenderingKHR")) return (PFN_vkVoidFunction)pfnCmdBeginRendering;
    if (!strcmp(name, "vkCmdEndRendering") || !strcmp(name, "vkCmdEndRenderingKHR")) return (PFN_vkVoidFunction)pfnCmdEndRendering;
    if (!strcmp(name, "vkQueueWaitIdle")) return (PFN_vkVoidFunction)ui_queue_wait_idle;
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(g_dev, name);
    if (!f) f = vkGetInstanceProcAddr(g_inst, name);
    if (f && !strncmp(name, "vkCmd", 5)) LOG(0, "[ui] warning: %s bypasses the command recorder", name);
    return f;
}
static void ui_vk_init() {
    if (g_ui_tried) return;
    g_ui_tried = true;
    if (getenv("XRT_NO_UI")) return;
    ui_init((int)g_sc_extent.height);
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, ui_loader)) { LOG(0, "[ui] ImGui Vulkan functions not loaded: no overlay"); return; }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3; ii.Instance = g_inst; ii.PhysicalDevice = g_phys; ii.Device = g_dev;
    ii.QueueFamily = g_qfam; ii.Queue = g_queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 8;
    ii.MinImageCount = 2; ii.ImageCount = 8;   // (vertex buffers per frame index: well past the frames in flight)
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    static VkFormat fmt; fmt = g_sc_format;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1; ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &fmt;
    g_ui_ok = ImGui_ImplVulkan_Init(&ii);
    LOG(1, "[ui] ImGui Vulkan backend %s", g_ui_ok ? "ready" : "failed");
}

// Records the window image for this frame (swapchain image idx, layout UNDEFINED -> PRESENT_SRC, or TRANSFER_SRC
// when the caller copies it out). false = not available: the caller uses the old blit.
// the game image's rectangle shown in the window (nvnWindowSetCrop), as blit_crop
static void present_crop(const GpuTex *t, int &cx, int &cy, int &cw, int &ch) {
    cx = g_crop[0]; cy = g_crop[1]; cw = g_crop[2]; ch = g_crop[3];
    if (cw <= 0 || ch <= 0 || cx < 0 || cy < 0 || cx + cw > t->w || cy + ch > t->h) { cx = 0; cy = 0; cw = t->w; ch = t->h; }
    else if (g_crop_ll && getenv("XRT_VP_MODE") && atoi(getenv("XRT_VP_MODE")) == 1) cy = t->h - cy - ch;
}
static bool present_pass(GpuTex *t, bool color, uint32_t idx, bool readback, ImDrawData *dd, bool whole = false) {
    if (!present_init() || idx >= g_sc_views.size() || !g_sc_views[idx]) return false;
    const XrtSettings &st = settings();
    const int W = (int)g_sc_extent.width, H = (int)g_sc_extent.height;
    // source rectangle (nvnWindowSetCrop), as blit_crop
    int cx = 0, cy = 0, cw = 0, ch = 0;
    VkImageView src = VK_NULL_HANDLE;
    if (color) {
        if (whole) { cx = 0; cy = 0; cw = t->w; ch = t->h; } else present_crop(t, cx, cy, cw, ch);
        src = tex_view(t, TT_2D);
    }
    // destination rectangle: fit with black bars, or the whole window
    int dx = 0, dy = 0, dw = W, dh = H;
    if (src && st.keep_aspect) {
        double a = (double)cw / ch;
        if ((double)W / H > a) { dw = (int)(H * a + 0.5); dx = (W - dw) / 2; }
        else { dh = (int)(W / a + 0.5); dy = (H - dh) / 2; }
    }
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    const bool fsr = src && st.upscaler == UP_FSR && (dw > cw || dh > ch) && fsr_target(dw, dh);
    if (fsr) {   // EASU: crop rectangle -> dw x dh intermediate
        image_barrier(g_fsr_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        pr_begin(g_fsr_view, (uint32_t)dw, (uint32_t)dh, false);
        float sx = (float)cw / dw, sy = (float)ch / dh;
        float pc[12] = {sx, sy, 0.5f * sx - 0.5f + cx, 0.5f * sy - 0.5f + cy, (float)t->w, (float)t->h, 0, 0, 0, 0, 0, 0};
        pr_draw(1, k_fsr_fmt, pr_set(src, VK_IMAGE_LAYOUT_GENERAL), pc, 0, 0, dw, dh);
        pfnCmdEndRendering(g_cmd);
        image_barrier(g_fsr_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkImage si = g_sc_images[idx];
    image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pr_begin(g_sc_views[idx], (uint32_t)W, (uint32_t)H, true);
    if (fsr) {   // RCAS: intermediate -> window rectangle
        float pc[12] = {exp2f(-st.fsr_sharpness), 0, 0, 0, (float)dw, (float)dh, 0, 0, (float)dx, (float)dy, 0, 0};
        pr_draw(2, g_sc_format, pr_set(g_fsr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), pc, dx, dy, dw, dh);
    } else if (src) {
        float pc[12] = {(float)cx, (float)cy, (float)cw, (float)ch, (float)t->w, (float)t->h, 0, 0, (float)dx, (float)dy, (float)dw, (float)dh};
        pr_draw(0, g_sc_format, pr_set(src, VK_IMAGE_LAYOUT_GENERAL), pc, dx, dy, dw, dh);
    }
    if (g_ui_ok && dd) {
        VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
        vkCmdSetViewport(g_cmd, 0, 1, &vp);
        ImGui_ImplVulkan_RenderDrawData(dd, g_cmd);
    }
    pfnCmdEndRendering(g_cmd);
    image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  readback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    return true;
}

// the overlay for this frame (built once, drawn into each image presented for it)
static ImDrawData *present_overlay(GpuTex *t, bool color) {
    if (!present_init()) return nullptr;
    ui_vk_init();
    if (!g_ui_ok) return nullptr;
    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (color) present_crop(t, cx, cy, cw, ch);
    ImGui_ImplVulkan_NewFrame();
    return ui_frame((int)g_sc_extent.width, (int)g_sc_extent.height, g_fps, cw, ch);
}
