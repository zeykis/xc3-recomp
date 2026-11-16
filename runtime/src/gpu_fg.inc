// Frame generation (M5, included at the end of gpu_vk.cpp): one generated image between two game frames, from the
// finished game image only (no game hooks), like the optical-flow generators of Lossless Scaling and emulators.
//   1. the game image (its crop rectangle) is copied into a history pair: the current and the previous frame
//   2. luma pyramid of both (half size, then 4 more halvings)
//   3. motion current -> previous by block matching, coarse to fine (levels 4..1, 1 = a quarter of the game image)
//   4. the halfway frame: each pixel follows the motion to both frames and blends them (static pixels: current)
// gpu_present presents the generated image at once and the game frame half a frame later (gpu_rec.inc pacer); the
// game is held at half the display's refresh rate (fps_cap in hle_nvn.cpp).

struct FgImg { VkImage im = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView v = VK_NULL_HANDLE; int w = 0, h = 0; VkFormat f = VK_FORMAT_UNDEFINED; };
static FgImg g_fg_frame[2], g_fg_out, g_fg_lum[5], g_fg_flow[5];
static int g_fg_cur; static bool g_fg_have_prev;
static const VkFormat k_fg_rgba = VK_FORMAT_R8G8B8A8_UNORM, k_fg_half = VK_FORMAT_R16G16B16A16_SFLOAT;
static std::atomic<int> g_display_hz{0};

static bool fg_img(FgImg &m, int w, int h, VkFormat f) {
    if (m.im && m.w == w && m.h == h && m.f == f) return true;
    if (m.im) { vkDestroyImageView(g_dev, m.v, nullptr); vkDestroyImage(g_dev, m.im, nullptr); vkFreeMemory(g_dev, m.mem, nullptr); m = FgImg(); }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent = {(uint32_t)w, (uint32_t)h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(g_dev, &ci, nullptr, &m.im) != VK_SUCCESS) { m.im = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, m.im, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_dev, &mai, nullptr, &m.mem) != VK_SUCCESS) { vkDestroyImage(g_dev, m.im, nullptr); m.im = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(g_dev, m.im, m.mem, 0);
    m.v = make_view(m.im, VK_IMAGE_VIEW_TYPE_2D, f, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    m.w = w; m.h = h; m.f = f;
    return m.v != VK_NULL_HANDLE;
}
static VkDescriptorSet fg_set(VkImageView a, VkImageView b = VK_NULL_HANDLE, VkImageView c = VK_NULL_HANDLE) {
    VkDescriptorSet s = alloc_set(g_fg_dsl);
    if (!s) return s;
    VkDescriptorImageInfo ii[3] = {{g_pr_smp, a, VK_IMAGE_LAYOUT_GENERAL}, {g_pr_smp, b ? b : a, VK_IMAGE_LAYOUT_GENERAL}, {g_pr_smp, c ? c : a, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < 3; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = s; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(g_dev, 3, w, 0, nullptr);
    return s;
}
// one full-screen pass into m (its old contents discarded); m ends in GENERAL, ready to be sampled
static void fg_pass(int which, FgImg &m, VkDescriptorSet set, const float pc[12]) {
    image_barrier(m.im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pr_begin(m.v, (uint32_t)m.w, (uint32_t)m.h, false);
    pr_draw(which, m.f, set, pc, 0, 0, m.w, m.h);
    pfnCmdEndRendering(g_cmd);
    image_barrier(m.im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

// the display's refresh rate (the window's monitor), checked about once a second
static void fg_update_display_hz() {
#ifdef _WIN32
    static uint64_t last;
    uint64_t now = plat_ticks_ns();
    if (last && now - last < 1000000000ULL) return;
    last = now;
    if (!g_hwnd) return;
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        int hz = (int)dm.dmDisplayFrequency;
        if (hz != g_display_hz.exchange(hz)) LOG(1, "[fg] display %d Hz", hz);
    }
#endif
}
// frame generation wanted and possible now (the recorder thread presents through the pacer)
static bool fg_wanted() { return settings().frame_gen && g_rec_on && g_windowed && pfnCmdBeginRendering; }
int gpu_fg_cap_hz() { return g_fg_now || (fg_wanted() && g_fg_have_prev) ? g_display_hz.load() / 2 : 0; }   // (hle_nvn fps_cap)
bool gpu_fg_active() { return g_fg_now; }
int gpu_display_hz() { return g_display_hz.load(); }

// Records this frame's passes; true when g_fg_tex holds the generated (halfway) image.
static bool fg_generate(GpuTex *t) {
    if (!present_init()) return false;
    int cx, cy, cw, ch;
    present_crop(t, cx, cy, cw, ch);
    if (cw < 64 || ch < 64) { g_fg_have_prev = false; return false; }
    VkImageView src = tex_view(t, TT_2D);
    if (!src) return false;
    bool resized = !g_fg_frame[0].im || g_fg_frame[0].w != cw || g_fg_frame[0].h != ch;
    if (resized) {
        vkDeviceWaitIdle(g_dev); g_fg_have_prev = false;
        for (VkImageView &v : g_fg_tex.views) if (v) { vkDestroyImageView(g_dev, v, nullptr); v = VK_NULL_HANDLE; }
    }
    if (!fg_img(g_fg_frame[0], cw, ch, k_fg_rgba) || !fg_img(g_fg_frame[1], cw, ch, k_fg_rgba) || !fg_img(g_fg_out, cw, ch, k_fg_rgba)) return false;
    int lw[5], lh[5];
    for (int i = 0; i < 5; i++) {
        lw[i] = std::max(4, (cw + (2 << i) - 1) >> (i + 1)); lh[i] = std::max(4, (ch + (2 << i) - 1) >> (i + 1));
        if (!fg_img(g_fg_lum[i], lw[i], lh[i], k_fg_half)) return false;
        if (i && !fg_img(g_fg_flow[i], lw[i], lh[i], k_fg_half)) return false;
    }
    FgImg &cur = g_fg_frame[g_fg_cur], &prv = g_fg_frame[g_fg_cur ^ 1];
    {   // 1. the game image into the history
        float pc[12] = {(float)cx, (float)cy, (float)cw, (float)ch, (float)t->w, (float)t->h, 0, 0, 0, 0, (float)cw, (float)ch};
        VkDescriptorSet s = alloc_set(g_pr_dsl);
        if (!s) return false;
        VkDescriptorImageInfo ii = {g_pr_smp, src, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
        image_barrier(cur.im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        pr_begin(cur.v, (uint32_t)cw, (uint32_t)ch, false);
        pr_draw(PR_BLIT, k_fg_rgba, s, pc, 0, 0, cw, ch);
        pfnCmdEndRendering(g_cmd);
        image_barrier(cur.im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    bool have = g_fg_have_prev;
    g_fg_have_prev = true;
    g_fg_cur ^= 1;   // (next frame: this one is the previous)
    if (!have) return false;
    {   // 2. luma pyramid
        float pc[12] = {(float)lw[0], (float)lh[0]};
        fg_pass(PR_FG_LUMA, g_fg_lum[0], fg_set(cur.v, prv.v), pc);
        for (int i = 1; i < 5; i++) { float p2[12] = {(float)lw[i], (float)lh[i]}; fg_pass(PR_FG_DOWN, g_fg_lum[i], fg_set(g_fg_lum[i - 1].v), p2); }
    }
    // 3. motion, coarse to fine (search radius 3 at 1/32, then 2, 1, 1)
    static const float radius[5] = {0, 1, 1, 2, 3};
    for (int i = 4; i >= 1; i--) {
        float pc[12] = {(float)lw[i], (float)lh[i], i < 4 ? 1.0f : 0.0f, radius[i]};
        fg_pass(PR_FG_FLOW, g_fg_flow[i], fg_set(g_fg_lum[i].v, i < 4 ? g_fg_flow[i + 1].v : g_fg_lum[i].v), pc);
    }
    {   // 4. the halfway frame
        float pc[12] = {(float)cw, (float)ch, (float)cw / lw[1], 0, (float)lw[1], (float)lh[1]};
        fg_pass(PR_FG_INTERP, g_fg_out, fg_set(cur.v, prv.v, g_fg_flow[1].v), pc);
    }
    if (g_fg_tex.image != g_fg_out.im) {   // (a texture view of it for present_pass)
        for (VkImageView &v : g_fg_tex.views) v = VK_NULL_HANDLE;   // (the old views died with the old image)
        g_fg_tex.image = g_fg_out.im;
    }
    g_fg_tex.format = k_fg_rgba; g_fg_tex.depth_kind = 0; g_fg_tex.w = cw; g_fg_tex.h = ch; g_fg_tex.layers = 1; g_fg_tex.levels = 1;
    return true;
}
