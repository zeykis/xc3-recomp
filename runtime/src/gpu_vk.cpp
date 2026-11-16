// Vulkan backend for the NVN layer.
//
// M3.1 scope: device + window/swapchain, NVN textures used as render targets become VkImages,
// clears are executed, and the texture the game presents is shown in a window (Windows) or
// written to frames/frame_NNNNN.ppm (headless / XRT_DUMP_FRAMES=N).
//
// Simplicity rules for now (correctness first, speed later):
//   * every image stays in VK_IMAGE_LAYOUT_GENERAL
//   * a full memory barrier is recorded between operations
//   * one command buffer per frame, submitted at present, CPU waits for it
//
// Vulkan is loaded at runtime (vulkan-1.dll / libvulkan.so.1), so building needs only the
// headers in third_party/vulkan and running needs only a GPU driver.
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#include <vulkan/vulkan.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <map>
#include <unordered_map>
#include <atomic>
#include <cwchar>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>
#include <tuple>
#include <vector>
#include "gpu.h"
#include "nvn_format.h"
#include "input.h"
#include "settings.h"
#include "ui.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
extern "C" {
#include "xrt_internal.h"
}

// ---------------------------------------------------------------- function pointers
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceFeatures2) VK_INSTANCE_WIN(X)
#ifdef _WIN32
#define VK_INSTANCE_WIN(X) X(vkCreateWin32SurfaceKHR)
#else
#define VK_INSTANCE_WIN(X)
#endif
#define VK_DEVICE(X) \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCmdPipelineBarrier) X(vkCmdSetDepthBias) X(vkCmdCopyImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdBlitImage) X(vkCmdCopyImageToBuffer) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkResetCommandBuffer) \
    X(vkDeviceWaitIdle) X(vkCreateSemaphore) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdPushConstants) X(vkCmdCopyBufferToImage) X(vkCreateBufferView) \
    X(vkGetMemoryHostPointerPropertiesEXT) X(vkDestroyDescriptorPool) X(vkCmdFillBuffer) X(vkCmdCopyBuffer) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)
#define DECL(f) static PFN_##f f;
VK_GLOBAL(DECL) VK_INSTANCE(DECL) VK_DEVICE(DECL)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCmdBeginRendering pfnCmdBeginRendering; static PFN_vkCmdEndRendering pfnCmdEndRendering;
static PFN_vkCmdDrawIndirectByteCountEXT pfnCmdDrawByteCount; static PFN_vkCmdBindTransformFeedbackBuffersEXT pfnCmdBindXfb; static PFN_vkCmdBeginTransformFeedbackEXT pfnCmdBeginXfb; static PFN_vkCmdEndTransformFeedbackEXT pfnCmdEndXfb;

const char *gpu_format_name(int nvn) { return nvn_format_name(nvn); }

// ---------------------------------------------------------------- state
struct GpuTex {
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    VkFormat format; int depth_kind; int w, h, layers, levels;
    int target = 1;
    int depth3 = 0;                      // 3D textures: number of slices (the VkImage is 3D, layers = 1)
    bool storage = false;                // created with STORAGE usage (image load/store)
    VkImageView storage_views[16] = {};  // per mip level
    std::vector<std::pair<uint32_t, VkImageView>> swz_views;   // sampled views with a non-identity swizzle, key tt | swz << 8
    VkImageView views[12] = {};          // sampled views by ShaderTexType; [9] = render-target view per layer 0
    std::vector<VkImageView> rt_views;   // one per layer, created on demand
    // feedback loops: a draw that samples one of its own render targets reads this copy instead (as Eden does)
    GpuTex *fb_copy = nullptr;
    uint64_t write_serial = 1, copy_serial = 0;   // bumped on every write as an attachment / clear
    uint64_t read_serial = 0;                     // write_serial made visible to in-place sampling (barrier done)
    uint64_t rt_frame = 0, rt_prev_frame = 0;     // frames it was a render target in (this / the one before): async pipelines
};

static std::atomic<bool> g_capture_req{false}, g_capture_window{false};   // F12 capture request (window thread -> present)
static int g_state;                 // 0 = not tried, 1 = ok, -1 = unavailable
static bool g_host_import;          // VK_EXT_external_memory_host available
static bool g_xfb;                  // VK_EXT_transform_feedback (geometry streams)
static VkPhysicalDeviceLimits g_limits;
static VkInstance g_inst; static VkPhysicalDevice g_phys; static VkDevice g_dev; static VkQueue g_queue;
static uint32_t g_qfam; static VkPhysicalDeviceMemoryProperties g_memprops;
static VkCommandPool g_pool; static VkCommandBuffer g_cmd; static VkFence g_fence; static bool g_recording;
static uint64_t g_rec_gen;   // command buffer recordings begun (state bound in a command buffer is only valid in that recording)
static VkSemaphore g_sem_acquire, g_sem_render, g_sem_acquire2, g_sem_render2;
static bool g_fg_mode;                 // swapchain set up for frame generation (two images per frame, gpu_fg.inc)
static bool g_fg_now;                  // this frame is presented with a generated one
static GpuTex g_fg_tex;                // the generated image (gpu_fg.inc)
static void pacer_flush();             // gpu_rec.inc
static std::vector<GpuTex *> g_dead;                 // destroyed after the current frame completes
// Frames in flight (milestone M5): present submits and returns; the CPU builds frame N+1 while the GPU runs frame N.
// Each slot has its own command buffer, fence, semaphores, timestamp pool, staging / descriptor memory and dead list;
// a slot is reused only after its fence (two frames back). nvnSyncWait waits for the frame its fence belongs to.
static VkQueryPool g_tsq;                           // current slot's frame timestamps
static bool g_ts_started, g_ts_ok;
static const int NSLOT = 2;
struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; VkSemaphore acq = VK_NULL_HANDLE, ren = VK_NULL_HANDLE;
    VkSemaphore acq2 = VK_NULL_HANDLE, ren2 = VK_NULL_HANDLE;   // frame generation: the frame's second image
    VkQueryPool tsq = VK_NULL_HANDLE; bool ts_ok = false;
    uint64_t serial = 0; bool pending = false;     // submitted, fence not yet waited
    std::vector<GpuTex *> dead;
    std::vector<VkCommandBuffer> extra; size_t extra_used = 0;   // command buffers of mid-frame submits (gpu_kick)
};
static FrameSlot g_slots[NSLOT];
static int g_slot;
static uint64_t g_serial = 1;                        // the frame being recorded (submitted at present)
static void slot_stash_resources(int slot);          // gpu_vk_draw.inc: per-slot staging rings / descriptor pools
static void slot_restore_resources(int slot);
static bool debug_needs_sync();                      // gpu_vk_draw.inc: captures / snapshots read results this frame
static void mirror_stats(double frames);              // gpu_vk_draw.inc: VRAM pool mirrors
static uint64_t g_frame;
// readback (frame dumps)
static int g_dump_every = -1;
static VkImage g_rb_image; static VkDeviceMemory g_rb_imem; static VkBuffer g_rb_buf; static VkDeviceMemory g_rb_bmem;
static int g_rb_w, g_rb_h; static void *g_rb_map;
// window
static bool g_windowed;
static VkSurfaceKHR g_surface; static VkSwapchainKHR g_swapchain; static std::vector<VkImage> g_sc_images;
static VkExtent2D g_sc_extent; static std::atomic<bool> g_resized{false};
static bool g_sc_readable; static VkFormat g_sc_format;
static std::vector<VkImageView> g_sc_views;           // (present pass: the swapchain images as attachments)
static double g_fps;                                   // presented frames per second (overlay)
// window capture: a copy of exactly what was presented (after scaling to the window size)
static VkBuffer g_wb_buf; static VkDeviceMemory g_wb_mem; static void *g_wb_map; static uint32_t g_wb_w, g_wb_h;

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { fprintf(stderr, "[gpu] %s failed: %d\n", #x, (int)_r); return false; } } while (0)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void gpu_save_caches();   // gpu_vk_draw.inc
// ---------------------------------------------------------------- window (Win32)
#ifdef _WIN32
static HWND g_hwnd; static std::atomic<bool> g_win_ready{false};
static const UINT WM_XRT_APPLY = WM_APP + 1;
// window mode / size from settings(): windowed = that client size, centred on the window's current monitor (clamped
// to its work area); borderless = a popup covering that whole monitor
static void win_apply() {
    const XrtSettings &s = settings();
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi}; GetMonitorInfoW(mon, &mi);
    if (s.window_mode == WIN_BORDERLESS) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        const RECT &m = mi.rcMonitor;
        SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        LOG(1, "[gpu] window: borderless %ldx%ld", m.right - m.left, m.bottom - m.top);
    } else {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        RECT r = {0, 0, s.window_w, s.window_h}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        const RECT &wa = mi.rcWork;
        int ww = std::min((int)(r.right - r.left), (int)(wa.right - wa.left)), wh = std::min((int)(r.bottom - r.top), (int)(wa.bottom - wa.top));
        int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        LOG(1, "[gpu] window: %dx%d", s.window_w, s.window_h);
    }
    g_resized = true;
}
void gpu_window_apply() { if (g_hwnd) PostMessageW(g_hwnd, WM_XRT_APPLY, 0, 0); }
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { fprintf(stderr, "[gpu] window closed - exiting\n"); fflush(stderr); gpu_save_caches(); _exit(0); }
    if (m == WM_SIZE) g_resized = true;
    if (m == WM_XRT_APPLY) { win_apply(); return 0; }
    if (m == WM_KILLFOCUS) input_release_all();          // no stuck keys after alt-tab
    if (ui_on_msg(h, m, (uintptr_t)w, (intptr_t)l)) return 0;   // TAB overlay (keyboard / mouse while it is open)
    if (m == WM_KEYDOWN && w == VK_F12) {   // capture the next frame for debugging: XRT_F12_CAPTURE=1 only (it takes
        static int on = -1; if (on < 0) on = getenv("XRT_F12_CAPTURE") ? 1 : 0;   // minutes; a stray F12 froze the game)
        if (on) g_capture_req = true;
        return 0;
    }
    if (m == WM_SYSCHAR) return 0;                                              // (no menu: no beep on Alt+key)
    if (m == WM_SYSKEYDOWN && w == VK_RETURN && !(l & (1 << 30))) {             // Alt+Enter: borderless <-> windowed
        settings().window_mode ^= 1; win_apply(); return 0;
    }
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) { input_key((int)w, true); return 0; }
    if (m == WM_KEYUP || m == WM_SYSKEYUP) { input_key((int)w, false); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void win_thread(void *) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"xc3recomp";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    const XrtSettings &s = settings();
    RECT r = {0, 0, s.window_w, s.window_h}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Xenoblade Chronicles 3 (recomp)", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    win_apply();   // (size clamped to the monitor, centred; or borderless)
    ShowWindow(g_hwnd, SW_SHOW);
    g_win_ready = true;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
#endif

static bool create_swapchain() {
    if (g_swapchain) pacer_flush();   // (frame generation: its presents are made first)
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps));
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;   // minimized
    uint32_t n = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n); vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &n, fmts.data());
    VkSurfaceFormatKHR sf = fmts[0];
    for (auto &f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { sf = f; break; }
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = caps.minImageCount + 1 + (g_fg_mode ? 2 : 0);   // (frame generation holds two per frame)
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = sf.format; ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = caps.currentExtent; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // window captures
    g_sc_readable = (ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    g_sc_format = sf.format;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // MAILBOX (no tearing, a free image at once: the frame's copy to the window never waits for vblank, so neither does
    // the game's per-frame fence) when the driver has it; XRT_PRESENT_MODE=fifo|mailbox|immediate to choose.
    {
        uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, nullptr);
        std::vector<VkPresentModeKHR> modes(nm); vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, modes.data());
        auto has = [&](VkPresentModeKHR m) { for (auto x : modes) if (x == m) return true; return false; };
        const int pm = settings().present_mode;   // (xc3.ini / TAB menu / XRT_PRESENT_MODE)
        VkPresentModeKHR want = pm == PM_FIFO ? VK_PRESENT_MODE_FIFO_KHR : pm == PM_IMMEDIATE ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
        if (!has(want) && want == VK_PRESENT_MODE_IMMEDIATE_KHR && has(VK_PRESENT_MODE_MAILBOX_KHR)) want = VK_PRESENT_MODE_MAILBOX_KHR;
        ci.presentMode = has(want) ? want : VK_PRESENT_MODE_FIFO_KHR;
        LOG(1, "[gpu] present mode %s", ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : ci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swapchain;
    VkSwapchainKHR sc;
    VKCHECK(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc));
    if (g_swapchain) {
        vkDeviceWaitIdle(g_dev);
        for (VkImageView v : g_sc_views) if (v) vkDestroyImageView(g_dev, v, nullptr);
        g_sc_views.clear();
        vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr);
    }
    g_swapchain = sc; g_sc_extent = caps.currentExtent;
    n = 0; vkGetSwapchainImagesKHR(g_dev, sc, &n, nullptr);
    g_sc_images.resize(n); vkGetSwapchainImagesKHR(g_dev, sc, &n, g_sc_images.data());
    g_sc_views.assign(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = g_sc_images[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = sf.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(g_dev, &vi, nullptr, &g_sc_views[i]);
    }
    LOG(1, "[gpu] swapchain %ux%u, %u images", g_sc_extent.width, g_sc_extent.height, n);
    return true;
}

// ---------------------------------------------------------------- init
#include "gpu_rec.inc"
static PFN_vkAllocateMemory r_alloc_counted; static PFN_vkFreeMemory r_free_counted;
static std::mutex g_mem_m; static std::unordered_map<uint64_t, std::pair<uint64_t, bool>> g_mem_live;   // memory -> (size, device-local)
static uint64_t g_mem_dev, g_mem_host, g_mem_n;
static VKAPI_ATTR VkResult VKAPI_CALL counted_alloc(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *cb, VkDeviceMemory *m) {
    VkResult r = r_alloc_counted(d, ai, cb, m);
    if (r == VK_SUCCESS) {
        bool dev = (g_memprops.memoryTypes[ai->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        std::lock_guard<std::mutex> l(g_mem_m);
        g_mem_live[(uint64_t)*m] = {ai->allocationSize, dev}; (dev ? g_mem_dev : g_mem_host) += ai->allocationSize; g_mem_n++;
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL counted_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *cb) {
    if (m) {
        std::lock_guard<std::mutex> l(g_mem_m);
        auto it = g_mem_live.find((uint64_t)m);
        if (it != g_mem_live.end()) { (it->second.second ? g_mem_dev : g_mem_host) -= it->second.first; g_mem_n--; g_mem_live.erase(it); }
    }
    r_free_counted(d, m, cb);
}
static bool init_vulkan() {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { fprintf(stderr, "[gpu] vulkan-1.dll not found (no Vulkan driver?)\n"); return false; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
    g_windowed = !getenv("XRT_HEADLESS");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "[gpu] libvulkan.so.1 not found (no Vulkan driver?)\n"); return false; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    g_windowed = false;
#endif
    if (!vkGetInstanceProcAddr) return false;
#define LOADG(f) f = (PFN_##f)vkGetInstanceProcAddr(nullptr, #f);
    VK_GLOBAL(LOADG)

    std::vector<const char *> iext;
    if (g_windowed) { iext.push_back("VK_KHR_surface"); iext.push_back("VK_KHR_win32_surface"); }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xc3-recomp"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app; ici.enabledExtensionCount = (uint32_t)iext.size(); ici.ppEnabledExtensionNames = iext.data();
    std::vector<const char *> layers;
    if (getenv("XRT_VK_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");
    ici.enabledLayerCount = (uint32_t)layers.size(); ici.ppEnabledLayerNames = layers.data();
    VKCHECK(vkCreateInstance(&ici, nullptr, &g_inst));
#define LOADI(f) f = (PFN_##f)vkGetInstanceProcAddr(g_inst, #f);
    VK_INSTANCE(LOADI)

    uint32_t n = 0; vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    if (!n) { fprintf(stderr, "[gpu] no Vulkan devices\n"); return false; }
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(g_inst, &n, devs.data());
    g_phys = devs[0];
    for (auto d : devs) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_phys = d; break; } }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(g_phys, &props);
    LOG(1, "[gpu] Vulkan device: %s (API %u.%u)", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);

#ifdef _WIN32
    if (g_windowed) {
        plat_thread_create(win_thread, nullptr, 1 << 20);
        while (!g_win_ready) Sleep(1);
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr); sci.hwnd = g_hwnd;
        VKCHECK(vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &g_surface));
    }
#endif
    n = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n); vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, qf.data());
    g_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 pres = VK_TRUE;
        if (g_windowed) vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, i, g_surface, &pres);
        if (pres) { g_qfam = i; break; }
    }
    if (g_qfam == UINT32_MAX) { fprintf(stderr, "[gpu] no graphics queue\n"); return false; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    std::vector<const char *> dext;
    if (g_windowed) dext.push_back("VK_KHR_swapchain");
    {   // optional extensions
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne); vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &ne, exts.data());
        for (auto &e : exts) {
            if (!strcmp(e.extensionName, "VK_EXT_external_memory_host")) { dext.push_back("VK_EXT_external_memory_host"); g_host_import = true; }
            if (!strcmp(e.extensionName, "VK_KHR_dynamic_rendering") && VK_VERSION_MINOR(props.apiVersion) < 3) dext.push_back("VK_KHR_dynamic_rendering");
            if (!strcmp(e.extensionName, "VK_EXT_transform_feedback")) { dext.push_back("VK_EXT_transform_feedback"); g_xfb = true; }
        }
    }
    // Enable every feature the device has: Eden's SPIR-V relies on int8/16/64, scalar block layout,
    // 8/16-bit storage, shader draw parameters, ... and on dynamic rendering for our render passes.
    static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    static VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    static VkPhysicalDeviceTransformFeedbackFeaturesEXT fxfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    f2.pNext = &f11; f11.pNext = &f12; f12.pNext = VK_VERSION_MINOR(props.apiVersion) >= 3 ? (void *)&f13 : nullptr;
    if (g_xfb) { fxfb.pNext = f12.pNext; f12.pNext = &fxfb; }
    vkGetPhysicalDeviceFeatures2(g_phys, &f2);
    f2.features.robustBufferAccess = VK_FALSE;
    if (!f12.scalarBlockLayout) LOG(1, "[gpu] warning: scalarBlockLayout unsupported - translated shaders may fail");
    if (VK_VERSION_MINOR(props.apiVersion) >= 3 && !f13.dynamicRendering) LOG(1, "[gpu] warning: dynamicRendering unsupported");
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = dext.data();
    VKCHECK(vkCreateDevice(g_phys, &dci, nullptr, &g_dev));
    plat_reinstall_crash_filter();   // (the driver may have put its own unhandled-exception filter in)
#define LOADD(f) f = (PFN_##f)vkGetDeviceProcAddr(g_dev, #f);
    VK_DEVICE(LOADD)
    if (g_xfb) {
        pfnCmdBindXfb = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(g_dev, "vkCmdBindTransformFeedbackBuffersEXT");
        pfnCmdBeginXfb = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(g_dev, "vkCmdBeginTransformFeedbackEXT");
        pfnCmdEndXfb = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(g_dev, "vkCmdEndTransformFeedbackEXT");
        pfnCmdDrawByteCount = (PFN_vkCmdDrawIndirectByteCountEXT)vkGetDeviceProcAddr(g_dev, "vkCmdDrawIndirectByteCountEXT");
    }
    pfnCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g_dev, "vkCmdBeginRendering");
    pfnCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g_dev, "vkCmdEndRendering");
    if (!pfnCmdBeginRendering) {
        pfnCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g_dev, "vkCmdBeginRenderingKHR");
        pfnCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g_dev, "vkCmdEndRenderingKHR");
    }
    rec_install();
    // device memory accounting (memory log): every allocation and free, by heap
    r_alloc_counted = vkAllocateMemory; vkAllocateMemory = counted_alloc;
    r_free_counted = vkFreeMemory; vkFreeMemory = counted_free;
    g_limits = props.limits;
    LOG(1, "[gpu] host memory import: %s, dynamic rendering: %s", g_host_import ? "yes" : "no", pfnCmdBeginRendering ? "yes" : "no");
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = g_qfam;
    VKCHECK(vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    for (FrameSlot &fs : g_slots) {
        VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &fs.cmd));
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKCHECK(vkCreateFence(g_dev, &fci, nullptr, &fs.fence));
        VkSemaphoreCreateInfo sci2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(g_dev, &sci2, nullptr, &fs.acq));
        VKCHECK(vkCreateSemaphore(g_dev, &sci2, nullptr, &fs.ren));
        VKCHECK(vkCreateSemaphore(g_dev, &sci2, nullptr, &fs.acq2));
        VKCHECK(vkCreateSemaphore(g_dev, &sci2, nullptr, &fs.ren2));
        if (g_limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo q = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; q.queryType = VK_QUERY_TYPE_TIMESTAMP; q.queryCount = 2;
            if (vkCreateQueryPool(g_dev, &q, nullptr, &fs.tsq) != VK_SUCCESS) fs.tsq = VK_NULL_HANDLE;
        }
    }
    g_slot = 0;
    g_cmd = g_slots[0].cmd; g_fence = g_slots[0].fence; g_sem_acquire = g_slots[0].acq; g_sem_render = g_slots[0].ren; g_tsq = g_slots[0].tsq;
    g_sem_acquire2 = g_slots[0].acq2; g_sem_render2 = g_slots[0].ren2;
    if (g_windowed && !create_swapchain()) LOG(1, "[gpu] swapchain not created yet (window minimized?)");

    const char *d = getenv("XRT_DUMP_FRAMES");
    g_dump_every = d ? atoi(d) : (g_windowed ? -1 : 0);
    if (g_dump_every > 0) {
#ifdef _WIN32
        CreateDirectoryA("frames", nullptr);
#else
        mkdir("frames", 0755);
#endif
        LOG(1, "[gpu] dumping every %d presented frames to frames/", g_dump_every);
    }
    return true;
}

bool gpu_available() {
    if (g_state == 0) {
        if (getenv("XRT_NO_GPU")) g_state = -1;
        else g_state = init_vulkan() ? 1 : -1;
        if (g_state < 0) LOG(1, "[gpu] running without a GPU backend (headless NVN)");
    }
    return g_state > 0;
}

void gpu_end_pass();
static void frame_done();          // gpu_vk_draw.inc: recycle per-frame descriptor pools / staging
static void pipeline_cache_save(bool now = false);  // gpu_vk_draw.inc: driver pipeline cache to disk
static VkResult submit_frame(const VkSubmitInfo *si, VkFence f);   // gpu_vk_draw.inc: pending mirror copies first
void gpu_draw_stats();
static void dump_frame_rts();
static void capture_next_frame();               // F12: dump RTs + draw list of the next frame (gpu_vk_draw.inc)

// ---------------------------------------------------------------- recording helpers
// ---------------------------------------------------------------- profiling
#include <atomic>
// XRT_PROF_PASSES=1: GPU time per render pass (timestamps around each pass), grouped by pass shape
static int g_pp_on = -1;
static VkQueryPool g_ppq;
static const uint32_t PP_MAX = 1024;   // passes per frame
struct PassRec { std::string key; uint32_t draws; };
static std::vector<PassRec> g_pp_frame;
static std::map<std::string, std::pair<double, uint64_t>> g_pp_sum;   // key -> (ms, draws)
static bool pp_on() { if (g_pp_on < 0) g_pp_on = getenv("XRT_PROF_PASSES") ? 1 : 0; return g_pp_on == 1; }
// timed GPU operations outside render passes (copies, clears): same records as passes
static bool g_pp_op;
static void pp_op_begin(const char *what, int w, int h) {
    if (!g_ppq || g_pp_frame.size() >= PP_MAX) return;
    char k[96]; snprintf(k, sizeof k, "[%s] %dx%d", what, w, h);
    vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ppq, (uint32_t)g_pp_frame.size() * 2);
    g_pp_frame.push_back(PassRec{k, 1});
    g_pp_op = true;
}
static void pp_op_end() {
    if (!g_pp_op) return;
    vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ppq, (uint32_t)g_pp_frame.size() * 2 - 1);
    g_pp_op = false;
}
static std::atomic<uint64_t> g_cnt_fbcopy, g_cnt_clear, g_cnt_pass;
static std::atomic<uint64_t> g_prof[PROF_N];
static uint64_t g_dsub[5];   // gpu_draw sub-steps: program, descriptors, buffers, pipeline, descriptor update (render thread only)
static std::atomic<uint64_t> g_prof_frame[PROF_N];   // the same, for the current frame (hitch log)
std::atomic<uint64_t> g_io_ns{0}, g_io_bytes{0}, g_io_reads{0}, g_io_missing{0}, g_tex_new{0}, g_tex_new_ns{0};   // per frame (hitch log)
void gpu_prof_add(int phase, uint64_t ns) { if (phase >= 0 && phase < PROF_N) { g_prof[phase] += ns; g_prof_frame[phase] += ns; } }
void gpu_note_io(uint64_t ns, uint64_t bytes, bool missing) { g_io_ns += ns; g_io_bytes += bytes; if (missing) g_io_missing++; else g_io_reads++; }
void gpu_prof_report(uint64_t frames, double wall_s) {
    if (!frames) return;
    static const char *names[PROF_N] = {"translate", "  draws", "  tex upload", "  shader xlate", "  pipelines", "acquire", "gpu wait", "present", "GPU busy", "timer waits (all threads)"};
    double f = (double)frames, frame_ms = wall_s * 1000.0 / f;
    char line[512]; int n = snprintf(line, sizeof line, "[prof] %.1f fps, %.2f ms/frame:", frames / wall_s, frame_ms);
    for (int i = 0; i < PROF_N; i++) n += snprintf(line + n, sizeof line - n, " %s %.2f%s", names[i], g_prof[i].exchange(0) / 1e6 / f, i + 1 < PROF_N ? "," : "");
    LOG(1, "%s", line);
    LOG(1, "[prof] draw steps per frame: program %.2f, descriptors %.2f, vertex/index %.2f, pipeline %.2f, descriptor update %.2f ms",
        g_dsub[0] / 1e6 / f, g_dsub[1] / 1e6 / f, g_dsub[2] / 1e6 / f, g_dsub[3] / 1e6 / f, g_dsub[4] / 1e6 / f);
    memset(g_dsub, 0, sizeof g_dsub);
    mirror_stats(f);
    LOG(1, "[prof] per frame: %.0f render passes, %.1f feedback copies, %.1f clears", g_cnt_pass.exchange(0) / f, g_cnt_fbcopy.exchange(0) / f, g_cnt_clear.exchange(0) / f);
    if (!g_pp_sum.empty()) {   // top passes by GPU time
        std::vector<std::pair<double, std::string>> v;
        double tot = 0;
        for (auto &kv : g_pp_sum) { v.push_back({kv.second.first, kv.first}); tot += kv.second.first; }
        std::sort(v.rbegin(), v.rend());
        LOG(1, "[prof] passes: %.2f ms/frame inside render passes", tot / f);
        for (size_t i = 0; i < v.size() && i < 14; i++)
            LOG(1, "[prof]   %6.2f ms  %5.0f draws  %s", v[i].first / f, g_pp_sum[v[i].second].second / f, v[i].second.c_str());
        g_pp_sum.clear();
    }
}
// GPU busy time per frame: timestamps at the start of a frame's first command buffer and at the end of present
static uint64_t prof_now() { return plat_ticks_ns(); }
static void begin_frame() {
    if (g_recording) return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);
    g_recording = true; g_rec_gen++;
    if (pp_on() && !g_ppq) {
        VkQueryPoolCreateInfo q = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; q.queryType = VK_QUERY_TYPE_TIMESTAMP; q.queryCount = PP_MAX * 2;
        if (vkCreateQueryPool(g_dev, &q, nullptr, &g_ppq) != VK_SUCCESS) g_pp_on = 0;
    }
    if (g_tsq && !g_ts_started) {
        if (g_ppq) { vkCmdResetQueryPool(g_cmd, g_ppq, 0, PP_MAX * 2); g_pp_frame.clear(); }
        vkCmdResetQueryPool(g_cmd, g_tsq, 0, 2);
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, 0);
        g_ts_started = true;
    }
}
static void full_barrier() {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
static VkImageAspectFlags aspect_of(const GpuTex *t) {
    switch (t->depth_kind) {
    case 1: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case 2: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case 3: return VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static void image_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------- textures
static bool format_ok(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(g_phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}

GpuTex *gpu_tex_create(const GpuTexDesc &d) {
    if (!gpu_available()) return nullptr;
    struct TexScope { uint64_t t0 = plat_ticks_ns(); ~TexScope() { g_tex_new++; g_tex_new_ns += plat_ticks_ns() - t0; } } tex_scope;
    const NvnFormat *f = nvn_format(d.nvn_format);
    if (!f) { static int warned[512]; if (d.nvn_format >= 0 && d.nvn_format < 512 && !warned[d.nvn_format]++) LOG(1, "[gpu] unmapped NVN format %d (%dx%d)", d.nvn_format, d.width, d.height); return nullptr; }
    VkFormat vf = (VkFormat)f->vk; int dk = f->depth;
    if (dk) {
        if (!format_ok(vf, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            // D24 formats are missing on AMD: fall back to 32-bit float depth
            vf = dk == 2 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
            if (!format_ok(vf, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return nullptr;
        }
    }
    GpuTex *t = new GpuTex{};
    t->format = vf; t->depth_kind = dk;
    t->w = d.width > 0 ? d.width : 1; t->h = d.height > 0 ? d.height : 1;
    t->layers = d.layers > 0 ? d.layers : 1; t->levels = d.levels > 0 ? d.levels : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vf;
    t->target = d.target;
    if (t->layers >= 6 && (d.target == 8 || d.target == 9) && t->w == t->h) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.extent = {(uint32_t)t->w, (uint32_t)t->h, 1};
    if (d.target == 2 && !dk) {
        // 3D texture (e.g. the colour-grading LUT the game renders slice by slice): a real 3D image whose
        // slices can be bound as 2D render targets
        t->depth3 = t->layers; t->layers = 1;
        ci.imageType = VK_IMAGE_TYPE_3D; ci.extent.depth = (uint32_t)t->depth3;
        if (format_ok(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   // slices as RTs
    }
    ci.mipLevels = (uint32_t)t->levels; ci.arrayLayers = (uint32_t)t->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;     // MSAA comes later
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (dk ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!dk && !format_ok(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!dk && format_ok(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT; t->storage = true; }
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    gpu_end_pass();
    if (VkResult r = vkCreateImage(g_dev, &ci, nullptr, &t->image); r != VK_SUCCESS) {
        LOG(1, "[gpu] vkCreateImage failed (%d): %s %dx%dx%d layers %d levels %d target %d", (int)r, f->name, t->w, t->h, t->depth3, t->layers, t->levels, d.target);
        delete t; return nullptr;
    }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, t->image, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(g_dev, &mai, nullptr, &t->mem) != VK_SUCCESS) { vkDestroyImage(g_dev, t->image, nullptr); delete t; return nullptr; }
    vkBindImageMemory(g_dev, t->image, t->mem, 0);
    begin_frame();
    image_barrier(t->image, aspect_of(t), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return t;
}
void gpu_tex_destroy(GpuTex *t) { if (t) { if (t->fb_copy) g_dead.push_back(t->fb_copy); t->fb_copy = nullptr; g_dead.push_back(t); } }
// same image shape and format, sampled + copy destination only (feedback-loop copies)
static GpuTex *tex_clone(const GpuTex *src) {
    if (src->depth3) return nullptr;
    GpuTex *t = new GpuTex{};
    t->format = src->format; t->depth_kind = src->depth_kind; t->w = src->w; t->h = src->h;
    t->layers = src->layers; t->levels = src->levels; t->target = src->target;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = src->format;
    if (t->layers >= 6 && (t->target == 8 || t->target == 9) && t->w == t->h) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.extent = {(uint32_t)t->w, (uint32_t)t->h, 1};
    ci.mipLevels = (uint32_t)t->levels; ci.arrayLayers = (uint32_t)t->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_dev, &ci, nullptr, &t->image) != VK_SUCCESS) { delete t; return nullptr; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, t->image, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(g_dev, &mai, nullptr, &t->mem) != VK_SUCCESS) { vkDestroyImage(g_dev, t->image, nullptr); delete t; return nullptr; }
    vkBindImageMemory(g_dev, t->image, t->mem, 0);
    begin_frame();
    image_barrier(t->image, aspect_of(t), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return t;
}
bool gpu_tex_is_depth(const GpuTex *t) { return t && t->depth_kind; }

static VkImageSubresourceRange range(const GpuTex *t, VkImageAspectFlags aspect, int base_layer, int num_layers, int level) {
    if (base_layer < 0 || base_layer >= t->layers) base_layer = 0;
    if (num_layers <= 0 || base_layer + num_layers > t->layers) num_layers = t->layers - base_layer;
    if (level < 0 || level >= t->levels) level = 0;
    return {aspect, (uint32_t)level, 1, (uint32_t)base_layer, (uint32_t)num_layers};
}

void gpu_clear_color(GpuTex *t, int base_layer, int num_layers, int level, const float rgba[4]) {
    if (!t || t->depth_kind) return;
    t->write_serial++;
    gpu_end_pass();
    begin_frame(); full_barrier();
    VkClearColorValue v; memcpy(v.float32, rgba, 16);
    VkImageSubresourceRange r = range(t, VK_IMAGE_ASPECT_COLOR_BIT, base_layer, num_layers, level);
    g_cnt_clear++; pp_op_begin("clear", t->w, t->h);
    vkCmdClearColorImage(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    pp_op_end();
}
void gpu_clear_color_int(GpuTex *t, int base_layer, int num_layers, int level, const uint32_t v4[4]) {
    if (!t || t->depth_kind) return;
    t->write_serial++;
    gpu_end_pass();
    begin_frame(); full_barrier();
    VkClearColorValue v; memcpy(v.uint32, v4, 16);
    VkImageSubresourceRange r = range(t, VK_IMAGE_ASPECT_COLOR_BIT, base_layer, num_layers, level);
    g_cnt_clear++; pp_op_begin("clear", t->w, t->h);
    vkCmdClearColorImage(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    pp_op_end();
}
void gpu_clear_depth_stencil(GpuTex *t, int base_layer, int num_layers, int level, bool cd, float depth, bool cs, int stencil) {
    if (!t || !t->depth_kind) return;
    t->write_serial++;
    VkImageAspectFlags a = 0;
    if (cd && (t->depth_kind == 1 || t->depth_kind == 2)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (cs && (t->depth_kind == 2 || t->depth_kind == 3)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a) return;
    gpu_end_pass();
    begin_frame(); full_barrier();
    VkClearDepthStencilValue v = {depth, (uint32_t)stencil};
    VkImageSubresourceRange r = range(t, a, base_layer, num_layers, level);
    g_cnt_clear++; pp_op_begin("clear", t->w, t->h);
    vkCmdClearDepthStencilImage(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    pp_op_end();
}

// ---------------------------------------------------------------- present
static bool ensure_readback(int w, int h) {
    if (g_rb_image && g_rb_w == w && g_rb_h == h) return true;
    vkDeviceWaitIdle(g_dev);
    if (g_rb_image) { vkDestroyImage(g_dev, g_rb_image, nullptr); vkFreeMemory(g_dev, g_rb_imem, nullptr); vkDestroyBuffer(g_dev, g_rb_buf, nullptr); vkFreeMemory(g_dev, g_rb_bmem, nullptr); g_rb_image = VK_NULL_HANDLE; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VKCHECK(vkCreateImage(g_dev, &ci, nullptr, &g_rb_image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, g_rb_image, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    VKCHECK(vkAllocateMemory(g_dev, &mai, nullptr, &g_rb_imem));
    vkBindImageMemory(g_dev, g_rb_image, g_rb_imem, 0);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VKCHECK(vkCreateBuffer(g_dev, &bci, nullptr, &g_rb_buf));
    vkGetBufferMemoryRequirements(g_dev, g_rb_buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(g_dev, &mai, nullptr, &g_rb_bmem));
    vkBindBufferMemory(g_dev, g_rb_buf, g_rb_bmem, 0);
    VKCHECK(vkMapMemory(g_dev, g_rb_bmem, 0, VK_WHOLE_SIZE, 0, &g_rb_map));
    g_rb_w = w; g_rb_h = h;
    return true;
}

static int g_crop[4]; static bool g_crop_ll;   // nvnWindowSetCrop: region of the presented texture shown in the window (w = 0: all)
void gpu_set_present_crop(int x, int y, int w, int h, bool lower_left) { g_crop[0] = x; g_crop[1] = y; g_crop[2] = w; g_crop[3] = h; g_crop_ll = lower_left; }
static void blit_crop(const GpuTex *src, VkImage dst, VkImageLayout dst_layout, int dw, int dh) {
    int x = g_crop[0], y = g_crop[1], w = g_crop[2], h = g_crop[3];
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > src->w || y + h > src->h) { x = 0; y = 0; w = src->w; h = src->h; }
    // window origin mode 1 is UPPER_LEFT: the crop rectangle is top-left based like viewports (720p cutscenes
    // render into the top-left of the 1080p texture). XRT_VP_MODE=1 restores the old bottom-anchored mapping.
    else if (g_crop_ll && getenv("XRT_VP_MODE") && atoi(getenv("XRT_VP_MODE")) == 1) y = src->h - y - h;
    VkImageBlit b = {};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[0] = {x, y, 0}; b.srcOffsets[1] = {x + w, y + h, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {dw, dh, 1};
    vkCmdBlitImage(g_cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &b, VK_FILTER_LINEAR);
}
struct ImDrawData;
static bool present_pass(GpuTex *t, bool color, uint32_t idx, bool readback, ImDrawData *dd, bool whole);   // gpu_present.inc
static ImDrawData *present_overlay(GpuTex *t, bool color);
static void fg_update_display_hz();       // gpu_fg.inc
static bool fg_wanted();
static bool fg_generate(GpuTex *t);
static void hitch_report(uint64_t frame);   // gpu_vk_draw.inc
static void blit_full(const GpuTex *src, VkImage dst, VkImageLayout dst_layout, int dw, int dh) {
    VkImageBlit b = {};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {src->w, src->h, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {dw, dh, 1};
    vkCmdBlitImage(g_cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &b, VK_FILTER_LINEAR);
}

static void write_ppm(uint64_t frame) {
    char name[64]; snprintf(name, sizeof name, "frames/frame_%05llu.ppm", (unsigned long long)frame);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g_rb_w, g_rb_h);
    const uint8_t *p = (const uint8_t *)g_rb_map;
    std::vector<uint8_t> row((size_t)g_rb_w * 3);
    for (int y = 0; y < g_rb_h; y++) {
        for (int x = 0; x < g_rb_w; x++) memcpy(&row[x * 3], p + ((size_t)y * g_rb_w + x) * 4, 3);
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    LOG(1, "[gpu] wrote %s", name);
}

static bool ensure_window_readback(uint32_t w, uint32_t h) {
    if (g_wb_buf && g_wb_w == w && g_wb_h == h) return true;
    if (g_wb_buf) { vkDeviceWaitIdle(g_dev); vkDestroyBuffer(g_dev, g_wb_buf, nullptr); vkFreeMemory(g_dev, g_wb_mem, nullptr); g_wb_buf = VK_NULL_HANDLE; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_dev, &bci, nullptr, &g_wb_buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, g_wb_buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_dev, &mai, nullptr, &g_wb_mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_dev, g_wb_buf, g_wb_mem, 0);
    vkMapMemory(g_dev, g_wb_mem, 0, VK_WHOLE_SIZE, 0, &g_wb_map);
    g_wb_w = w; g_wb_h = h;
    return true;
}
static void write_window_ppm(uint64_t frame) {
    char name[64]; snprintf(name, sizeof name, "frames/window_%05llu.ppm", (unsigned long long)frame);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", g_wb_w, g_wb_h);
    const uint8_t *p = (const uint8_t *)g_wb_map;
    bool bgra = g_sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<uint8_t> row((size_t)g_wb_w * 3);
    for (uint32_t y = 0; y < g_wb_h; y++) {
        for (uint32_t x = 0; x < g_wb_w; x++) {
            const uint8_t *px = p + ((size_t)y * g_wb_w + x) * 4;
            row[x * 3 + 0] = bgra ? px[2] : px[0]; row[x * 3 + 1] = px[1]; row[x * 3 + 2] = bgra ? px[0] : px[2];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    LOG(1, "[gpu] wrote %s (window %ux%u)", name, g_wb_w, g_wb_h);
}

// Wait for a slot's submitted frame (once); collect its GPU time.
static void slot_wait(int i) {
    FrameSlot &fs = g_slots[i];
    if (!fs.pending) return;
    vkWaitForFences(g_dev, 1, &fs.fence, VK_TRUE, UINT64_MAX);
    fs.pending = false;
    if (fs.ts_ok && fs.tsq) {   // (a frame split by a mid-frame flush measures from its start anyway)
        uint64_t ts[2];
        if (vkGetQueryPoolResults(g_dev, fs.tsq, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
            gpu_prof_add(PROF_GPU, (uint64_t)((double)(ts[1] - ts[0]) * g_limits.timestampPeriod));
        fs.ts_ok = false;
    }
}
// NVN syncs: a fence placed now belongs to the frame being recorded; waiting on it waits for that frame's GPU work
// (running what is recorded so far when the frame hasn't been submitted yet).
uint64_t gpu_sync_point() { return g_serial; }
void gpu_sync_wait(uint64_t serial) {
    if (!gpu_available() || !serial) return;
    if (serial >= g_serial) { gpu_flush(); for (int i = 0; i < NSLOT; i++) slot_wait(i); return; }
    for (int i = 0; i < NSLOT; i++) if (g_slots[i].pending && g_slots[i].serial <= serial) slot_wait(i);
}
void gpu_finish() {   // nvnQueueFinish: everything submitted or recorded
    if (!gpu_available()) return;
    gpu_flush();
    for (int i = 0; i < NSLOT; i++) slot_wait(i);
}
void gpu_present(GpuTex *t) {
    if (!gpu_available()) return;
    uint64_t prof_t0 = prof_now();
    g_frame++;
    gpu_end_pass();
    begin_frame(); full_barrier();
    bool color = t && !t->depth_kind && format_ok(t->format, VK_FORMAT_FEATURE_BLIT_SRC_BIT);
    bool acquired = false; uint32_t idx = 0;
    const bool dump_frame = (g_dump_every > 0 && g_frame % (uint64_t)g_dump_every == 0) || g_capture_window.exchange(false);
    bool window_dump = false;
    bool acquired2 = false; uint32_t idx2 = 0;
    if (g_windowed) {
        fg_update_display_hz();
        { static int seen; int gen = g_settings_gen.load(); if (gen != seen) { seen = gen; g_resized = true; } }   // (VSync changed)
        {   // frame generation on / off: the swapchain's image count changes, and the presents move to the pacer
            bool want = fg_wanted();
            if (want != g_fg_mode) { if (g_fg_mode) pacer_flush(); g_fg_mode = want; g_fg_now = false; g_resized = true; LOG(1, "[fg] frame generation %s", want ? "on" : "off"); }
        }
        if (g_resized.exchange(false) || !g_swapchain) create_swapchain();
        if (g_swapchain) {
            uint64_t ta = prof_now();
            if (g_fg_mode) {   // two images; the pacer may be presenting: try without blocking, under the queue lock
                auto try_acquire = [&](uint32_t &ix, VkSemaphore sem) {
                    for (int i = 0; i < 5000; i++) {
                        VkResult r;
                        { std::lock_guard<std::mutex> l(g_qm); r = vkAcquireNextImageKHR(g_dev, g_swapchain, 0, sem, VK_NULL_HANDLE, &ix); }
                        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return true;
                        if (r == VK_ERROR_OUT_OF_DATE_KHR) { g_resized = true; return false; }
                        if (r != VK_NOT_READY && r != VK_TIMEOUT) return false;
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                    return false;
                };
                acquired2 = try_acquire(idx2, g_sem_acquire2);
                acquired = acquired2 && try_acquire(idx, g_sem_acquire);
                if (acquired2 && !acquired) { acquired = true; idx = idx2; acquired2 = false; std::swap(g_sem_acquire, g_sem_acquire2); }   // (one image: just this frame)
            } else {
                VkResult r = vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, g_sem_acquire, VK_NULL_HANDLE, &idx);
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) acquired = true;
                else if (r == VK_ERROR_OUT_OF_DATE_KHR) g_resized = true;
            }
            gpu_prof_add(PROF_ACQUIRE, prof_now() - ta);
        }
        if (acquired) {
            VkImage si = g_sc_images[idx];
            const bool rb = dump_frame && g_sc_readable && ensure_window_readback(g_sc_extent.width, g_sc_extent.height);
            ImDrawData *dd = present_overlay(t, color);
            g_fg_now = false;
            static int dump_gen = -1; if (dump_gen < 0) dump_gen = getenv("XRT_FG_DUMP_GENERATED") ? 1 : 0;   // (window dumps: the generated image)
            bool rb_gen = false;
            if (acquired2) {   // frame generation: the halfway image first (or this frame again when there is none yet)
                bool gen = color && fg_generate(t);
                g_fg_now = gen;
                rb_gen = rb && dump_gen && gen;
                bool ok2 = gen ? present_pass(&g_fg_tex, true, idx2, rb_gen, dd, true) : present_pass(t, color, idx2, false, dd, false);
                if (rb_gen && ok2) {
                    VkImage s2 = g_sc_images[idx2];
                    VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    bc.imageExtent = {g_sc_extent.width, g_sc_extent.height, 1};
                    vkCmdCopyImageToBuffer(g_cmd, s2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_wb_buf, 1, &bc);
                    image_barrier(s2, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                    window_dump = true;
                }
            }
            if (present_pass(t, color, idx, rb && !rb_gen, dd, false)) {   // bilinear / FSR + overlay
                if (rb && !rb_gen) {
                    VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    bc.imageExtent = {g_sc_extent.width, g_sc_extent.height, 1};
                    vkCmdCopyImageToBuffer(g_cmd, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_wb_buf, 1, &bc);
                    image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                    window_dump = true;
                }
            } else {
            image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (color) blit_crop(t, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (int)g_sc_extent.width, (int)g_sc_extent.height);
            else { VkClearColorValue v = {}; VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                   vkCmdClearColorImage(g_cmd, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r); }
            if (dump_frame && g_sc_readable && ensure_window_readback(g_sc_extent.width, g_sc_extent.height)) {
                image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                bc.imageExtent = {g_sc_extent.width, g_sc_extent.height, 1};
                vkCmdCopyImageToBuffer(g_cmd, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_wb_buf, 1, &bc);
                image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                window_dump = true;
            } else
                image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }
        }
    }
    bool dump = dump_frame && color && ensure_readback(t->w, t->h);
    if (dump) {
        image_barrier(g_rb_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        blit_full(t, g_rb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->w, t->h);
        image_barrier(g_rb_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
        vkCmdCopyImageToBuffer(g_cmd, g_rb_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb_buf, 1, &bc);
    }
    if (g_tsq && g_ts_started) { vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 1); g_ts_ok = true; }
    vkEndCommandBuffer(g_cmd); g_recording = false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    VkSemaphore waits[2] = {g_sem_acquire, g_sem_acquire2}, sigs[2] = {g_sem_render, g_sem_render2};
    VkPipelineStageFlags wss[2] = {ws, ws};
    if (acquired) { si.waitSemaphoreCount = acquired2 ? 2 : 1; si.pWaitSemaphores = waits; si.pWaitDstStageMask = wss;
                    si.signalSemaphoreCount = acquired2 ? 2 : 1; si.pSignalSemaphores = sigs; }
    submit_frame(&si, g_fence);
    FrameSlot &cur = g_slots[g_slot];
    cur.serial = g_serial++; cur.pending = true; cur.ts_ok = g_ts_ok;
    {   // frame interval (frame generation: the second present comes half of it later)
        static uint64_t last; static double ema;
        uint64_t now = plat_ticks_ns();
        if (last) { double d = (double)(now - last); ema = ema ? ema * 0.9 + std::min(d, 100e6) * 0.1 : d; }
        last = now;
        if (acquired && g_fg_mode) {
            PacerJob j{};
            j.pos = g_rhead.load(std::memory_order_acquire); j.sc = g_swapchain;
            if (acquired2) { j.n = 2; j.idx[0] = idx2; j.sem[0] = g_sem_render2; j.idx[1] = idx; j.sem[1] = g_sem_render; }
            else { j.n = 1; j.idx[0] = idx; j.sem[0] = g_sem_render; }
            j.gap_ns = (uint64_t)std::max(1e6, std::min(ema * 0.5, 40e6));
            pacer_submit(j);
            acquired = false;   // (presented by the pacer)
        }
    }
    if (acquired) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_sem_render;
        pi.swapchainCount = 1; pi.pSwapchains = &g_swapchain; pi.pImageIndices = &idx;
        VkResult r = vkQueuePresentKHR(g_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_resized = true;
    }
    // XRT_SYNC_PRESENT=1: wait for every frame here (no frames in flight). Debug captures / dumps / the per-pass
    // profiler read this frame's results right after: they wait too.
    static int force_sync = -1; if (force_sync < 0) force_sync = getenv("XRT_SYNC_PRESENT") ? 1 : 0;
    const bool sync_now = force_sync || dump || window_dump || pp_on() || debug_needs_sync();
    if (sync_now) slot_wait(g_slot);
    if (g_ppq && sync_now && g_ts_ok && !g_pp_frame.empty()) {
        std::vector<uint64_t> q(g_pp_frame.size() * 2);
        VkResult qr = vkGetQueryPoolResults(g_dev, g_ppq, 0, (uint32_t)q.size(), q.size() * 8, q.data(), 8, VK_QUERY_RESULT_64_BIT);
        { static int n; if (n++ < 3) LOG(1, "[prof] pass queries: %zu passes, result %d", g_pp_frame.size(), (int)qr); }
        if (qr == VK_SUCCESS)
            for (size_t i = 0; i < g_pp_frame.size(); i++) if (q[2 * i + 1] > q[2 * i]) {
                auto &e = g_pp_sum[g_pp_frame[i].key];
                e.first += (double)(q[2 * i + 1] - q[2 * i]) * g_limits.timestampPeriod / 1e6; e.second += g_pp_frame[i].draws;
            }
    }
    { static int n; if (g_ppq && n++ < 3) LOG(1, "[prof] frame end: ppq %d, ts_ok %d, %zu passes", g_ppq != VK_NULL_HANDLE, (int)g_ts_ok, g_pp_frame.size()); }
    g_pp_frame.clear();
    g_ts_started = g_ts_ok = false;
    gpu_prof_add(PROF_PRESENT, prof_now() - prof_t0);
    dump_frame_rts();
    {   // unattended runs: XRT_CAPTURE_AT=s1,s2,... captures (like F12) at those seconds after start;
        // XRT_EXIT_AT=s exits cleanly (files flushed) at that second
        static std::vector<double> cap_at; static double exit_at = -1; static bool init;
        if (!init) {
            init = true;
            if (const char *e = getenv("XRT_CAPTURE_AT")) for (const char *q = e; *q;) { char *end; double v = strtod(q, &end); if (end == q) break; cap_at.push_back(v); q = *end ? end + 1 : end; }
            if (const char *e = getenv("XRT_EXIT_AT")) exit_at = atof(e);
        }
        double t = plat_ticks_ns() / 1e9;
        for (double &c : cap_at) if (c >= 0 && t >= c) { c = -1; g_capture_req = true; LOG(0, "[gpu] scheduled capture at %.0f s", t); }
        if (exit_at >= 0 && t >= exit_at) { LOG(0, "[gpu] XRT_EXIT_AT reached (%.0f s, frame %llu) - exiting", t, (unsigned long long)g_frame); gpu_draw_stats(); fflush(stderr); fflush(stdout); _exit(0); }
    }
    if (g_capture_req.exchange(false)) { capture_next_frame(); g_capture_window = true; }
    if (dump) write_ppm(g_frame);
    if (window_dump) write_window_ppm(g_frame);
    // next slot: wait for the frame it ran two presents ago, then recycle its memory and destroyed textures
    cur.dead.swap(g_dead); g_dead.clear();
    slot_stash_resources(g_slot);
    g_slot = (g_slot + 1) % NSLOT;
    {
        uint64_t tw = prof_now();
        slot_wait(g_slot);
        gpu_prof_add(PROF_WAIT, prof_now() - tw);
    }
    FrameSlot &nx = g_slots[g_slot];
    vkResetFences(g_dev, 1, &nx.fence);
    vkResetCommandBuffer(nx.cmd, 0);
    for (size_t i = 0; i < nx.extra_used; i++) vkResetCommandBuffer(nx.extra[i], 0);
    nx.extra_used = 0;
    g_cmd = nx.cmd; g_fence = nx.fence; g_sem_acquire = nx.acq; g_sem_render = nx.ren; g_tsq = nx.tsq;
    g_sem_acquire2 = nx.acq2; g_sem_render2 = nx.ren2;
    for (GpuTex *d : nx.dead) {
        for (VkImageView v : d->views) if (v) vkDestroyImageView(g_dev, v, nullptr);
        for (auto &sv : d->swz_views) if (sv.second) vkDestroyImageView(g_dev, sv.second, nullptr);
        for (VkImageView v : d->rt_views) if (v) vkDestroyImageView(g_dev, v, nullptr);
        for (VkImageView v : d->storage_views) if (v) vkDestroyImageView(g_dev, v, nullptr);
        vkDestroyImage(g_dev, d->image, nullptr); vkFreeMemory(g_dev, d->mem, nullptr); delete d;
    }
    nx.dead.clear();
    slot_restore_resources(g_slot);
    frame_done();
#ifdef _WIN32
    {   // frame rate in the window title, once a second
        static uint64_t t0 = plat_ticks_ns(), f0 = 0;
        uint64_t now = plat_ticks_ns();
        if (g_hwnd && now - t0 >= 1000000000ULL) {
            wchar_t title[128];
            g_fps = (g_frame - f0) * 1e9 / (double)(now - t0);
            if (g_fg_now) swprintf(title, 128, L"Xenoblade Chronicles 3 (recomp) - %.1f fps (x2 frame generation) - TAB: settings", g_fps);
            else swprintf(title, 128, L"Xenoblade Chronicles 3 (recomp)%s - %.1f fps - TAB: settings", settings().profile == "randomized" ? L" - Randomized" : L"", g_fps);
            SetWindowTextW(g_hwnd, title);
            t0 = now; f0 = g_frame;
        }
    }
#endif
    hitch_report(g_frame);
    { static uint64_t tf; uint64_t now = plat_ticks_ns(); if (now - tf > 1000000000ULL) { tf = now; fflush(stderr); fflush(stdout); } }   // (buffered log, main.cpp)
    if (g_frame % 300 == 0) gpu_draw_stats();
    if (g_frame % 600 == 0) pipeline_cache_save();   // (at most once a minute, on another thread)
#ifdef _WIN32
    if (g_frame % 1800 == 0) {   // memory use (a leak shows here before it ends the game)
        PROCESS_MEMORY_COUNTERS_EX pm = {sizeof pm}; MEMORYSTATUSEX ms = {sizeof ms};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm) && GlobalMemoryStatusEx(&ms))
        {
            uint64_t pa_n = 0, pa = plat_alloc_total(&pa_n);
            HEAP_SUMMARY hs = {}; hs.cb = sizeof hs; HeapSummary(GetProcessHeap(), 0, &hs);
            uint64_t md, mh, mn; { std::lock_guard<std::mutex> l(g_mem_m); md = g_mem_dev; mh = g_mem_host; mn = g_mem_n; }
            extern uint64_t g_fibers_made, g_fibers_ended; uint64_t hle_guest_committed(); uint64_t nvn_handles_live();
            LOG(1, "[mem] frame %llu: private %.0f MB, working set %.0f MB; system commit %.1f of %.1f GB, RAM free %.1f GB", (unsigned long long)g_frame,
                pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0, (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1073741824.0,
                ms.ullTotalPageFile / 1073741824.0, ms.ullAvailPhys / 1073741824.0);
            LOG(1, "[mem]   guest heap %.0f MB, runtime blocks %.0f MB (%llu), C heap %.0f MB committed, Vulkan memory %.0f MB device + %.0f MB host (%llu), fibers %llu made / %llu ended, command lists kept %llu",
                hle_guest_committed() / 1048576.0, pa / 1048576.0, (unsigned long long)pa_n, hs.cbCommitted / 1048576.0, md / 1048576.0, mh / 1048576.0,
                (unsigned long long)mn, (unsigned long long)g_fibers_made, (unsigned long long)g_fibers_ended, (unsigned long long)nvn_handles_live());
            static int hist = getenv("XRT_HEAP_HIST") ? 1 : 0;
            if (hist) {   // which block sizes hold the C heap (a leak shows as one size growing)
                std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> by;   // size -> (count, bytes)
                PROCESS_HEAP_ENTRY e = {}; HANDLE h = GetProcessHeap();
                uint64_t t0 = plat_ticks_ns();
                if (HeapLock(h)) {
                    while (HeapWalk(h, &e)) if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) { auto &v = by[e.cbData]; v.first++; v.second += e.cbData; }
                    HeapUnlock(h);
                }
                std::vector<std::pair<uint64_t, uint64_t>> top;   // (bytes, size)
                for (auto &kv : by) top.push_back({kv.second.second, kv.first});
                std::sort(top.rbegin(), top.rend());
                char line[1024]; int n = snprintf(line, sizeof line, "[mem]   C heap by block size (%.0f ms):", (plat_ticks_ns() - t0) / 1e6);
                for (size_t i = 0; i < top.size() && i < 10; i++) n += snprintf(line + n, sizeof line - n, " %llu B x%llu = %.0f MB;", (unsigned long long)top[i].second, (unsigned long long)by[top[i].second].first, top[i].first / 1048576.0);
                LOG(1, "%s", line);
            }
        }
    }
#endif
}

#include "gpu_vk_draw.inc"
#include "gpu_present.inc"
#include "gpu_fg.inc"
