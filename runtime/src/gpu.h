// GPU backend interface used by the NVN layer (hle_nvn.cpp). Implemented with Vulkan in gpu_vk.cpp.
// All calls must be made with the NVN lock held (single-threaded use).
#pragma once
#include <cstdint>

struct GpuTex;

struct GpuTexDesc {
    int nvn_format;          // NVNformat value
    int width, height;
    int layers;              // array layers (or depth for 3D, treated as layers for now)
    int levels;
    int samples;
    int target = 1;          // NVNtextureTarget (1 = 2D, 4 = 2D array, 8 = cube, 9 = cube array)
};

// ---------------------------------------------------------------- draws
// All enum values are NVN's (NVNblendFunc, NVNdepthFunc, NVNdrawPrimitive, ...), converted in gpu_vk.cpp.
struct GpuVertexAttrib { int format = 0; uint32_t offset = 0; int stream = 0; };
struct GpuVertexStream { uint32_t stride = 0, divisor = 0; };
struct GpuBlend { int src_c = 2, dst_c = 1, src_a = 2, dst_a = 1, eq_c = 1, eq_a = 1; };
// wrap: NVNwrapMode (0 CLAMP, 1 REPEAT, 2 MIRROR_CLAMP, 3 MIRROR_CLAMP_TO_EDGE, 4 MIRROR_CLAMP_TO_BORDER, 5 CLAMP_TO_BORDER,
//       6 MIRRORED_REPEAT, 7 CLAMP_TO_EDGE); NVN defaults to REPEAT
struct GpuSampler { float border[4] = {0, 0, 0, 0}; int wrap[3] = {1, 1, 1}; int min = 1, mag = 1; float max_aniso = 1, lod_min = 0, lod_max = 1000, lod_bias = 0; int compare = 0, compare_func = 1; };

struct GpuDrawState {
    const uint8_t *shader[5] = {}, *shader_end[5] = {};   // per GpuStage (gpu_shader.h): NVN shader blobs
    uint64_t program = 0;
    GpuVertexAttrib attribs[16]; int num_attribs = 0;
    GpuVertexStream streams[16]; int num_streams = 0;
    uint64_t vb_addr[16] = {}, vb_size[16] = {};
    uint64_t ubo_addr[5][18] = {}, ubo_size[5][18] = {};
    uint64_t bound_tex[5][32] = {};                        // NVNtextureHandle per stage/slot
    uint64_t bound_img[5][8] = {};                         // NVNimageHandle per stage/slot (image load/store)
    GpuBlend blend[8]; uint8_t blend_enable = 0; uint8_t channel_mask[8] = {15, 15, 15, 15, 15, 15, 15, 15};
    float blend_color[4] = {};
    bool depth_test = false, depth_write = false, stencil_test = false; int depth_func = 2;
    // stencil, [0] front / [1] back faces; NVN enums (func like depth func, op 1 KEEP .. 8 DECR_WRAP)
    int stencil_func[2] = {8, 8}, stencil_op[2][3] = {{1, 1, 1}, {1, 1, 1}};   // op: fail, depth fail, pass
    uint8_t stencil_ref[2] = {0, 0}, stencil_vmask[2] = {255, 255}, stencil_wmask[2] = {255, 255};
    int cull_face = 0, front_face = 1, polygon_mode = 2;
    int viewport[4] = {}; float depth_range[2] = {0, 1}; int scissor[4] = {0, 0, 0x7fffffff, 0x7fffffff};
    GpuTex *color[8] = {}; int color_layer[8] = {}; int num_colors = 0; GpuTex *depth = nullptr; int depth_layer = 0;
    int color_level[8] = {}; int depth_level = 0;                // mip level rendered to (texture view base level)
    bool lower_left = true;                                // NVN window origin: OpenGL-style lower-left
    // transform feedback: capture layout of the bound vertex shader (GPU attribute word per 4-byte slot of
    // buffer 0, from the NVN shader control data) and the buffer bound while nvnCommandBufferBeginTransformFeedback is active
    uint8_t xfb_locs[128] = {}; int xfb_count = 0, xfb_stride = 0;
    bool xfb_active = false; uint64_t xfb_addr = 0, xfb_size = 0;
    uint64_t xfb_control = 0; bool xfb_fresh = false;   // control buffer of the active Begin (byte counter at +4); fresh = first draw since Begin
    int logic_op = 3;                                      // NVNlogicOp (GL order = VkLogicOp order); 3 COPY = off
    int alpha_func = 8;                                    // NVNalphaFunc 1 NEVER .. 8 ALWAYS (8 = no alpha test)
    float alpha_ref = 0;
    int poly_offset_enables = 0;                           // NVNpolygonOffsetEnable bits: 1 POINT, 2 LINE, 4 FILL
    float poly_offset[3] = {0, 0, 0};                      // factor, units, clamp (nvnCommandBufferSetPolygonOffsetClamp)
    bool rast_discard = false;                             // nvnCommandBufferSetRasterizerDiscard: vertex shaders run for their buffer stores
    uint64_t ssbo_addr[5][16] = {}, ssbo_size[5][16] = {};  // nvnCommandBufferBindStorageBuffer (per GpuStage)
};
struct GpuDraw { int prim = 4; bool indexed = false; int index_type = 1; uint32_t count = 0, first = 0; uint64_t index_addr = 0;
                 int32_t base_vertex = 0; uint32_t base_instance = 0, instances = 1;
                 uint64_t xfb_count_addr = 0; };   // nvnCommandBufferDrawTransformFeedback: vertex count = bytes written (u32 at +4) / stream 0 stride

// hle_nvn answers resource lookups for the backend
struct GpuResolver {
    void *user;
    GpuTex *(*texture)(void *user, uint32_t texture_id);          // creates + uploads on first use
    int (*texture_target)(void *user, uint32_t texture_id);        // NVNtextureTarget, -1 = unknown
    bool (*sampler)(void *user, uint32_t sampler_id, GpuSampler *out);
    int (*texture_flags)(void *user, uint32_t texture_id);         // bit 0 integer, bit 1 stencil-mode sampling
    GpuTex *(*image)(void *user, uint32_t image_id, int *level);   // nvnTexturePoolRegisterImage entries
    uint32_t (*texture_swizzle)(void *user, uint32_t texture_id);  // NVN swizzle per channel, packed r|g<<8|b<<16|a<<24
    // NVN_TEXTURE_TARGET_BUFFER textures (texel buffers): guest address, size in bytes, NVNformat
    bool (*texture_buffer)(void *user, uint32_t texture_id, uint64_t *addr, uint64_t *size, int *nvn_format);
    uint64_t (*texture_addr)(void *user, uint32_t texture_id);      // guest address of the texture's level 0 (diagnostics)
};
void gpu_set_resolver(const GpuResolver &r);
void gpu_pool_register(uint64_t base, uint64_t size);              // NVN memory pool = GPU-visible host memory
void gpu_draw(const GpuDrawState &s, const GpuDraw &d);
bool gpu_tex_upload(GpuTex *t, int level, int layer, const void *data, size_t size);   // tightly packed linear data
bool gpu_tex_upload_3d(GpuTex *t, int level, int depth, const void *data, size_t size); // 3D level, [z][y][x]
void gpu_end_pass();
uint64_t nvn_tex_generation();                                     // hle_nvn.cpp: changes when texture IDs may resolve differently
// Frame-time breakdown (milestone M5): ns accumulated per phase, reported every 300 frames by gpu_prof_report.
int gpu_fg_cap_hz();     // frame generation on: the game frame rate it needs (half the display refresh), else 0
bool gpu_fg_active();   // this frame was shown with a generated one
int gpu_display_hz();
enum GpuProf { PROF_EXEC, PROF_DRAW, PROF_TEXUP, PROF_PROG, PROF_PIPE, PROF_ACQUIRE, PROF_WAIT, PROF_PRESENT, PROF_GPU, PROF_TIMER, PROF_N };
void gpu_prof_add(int phase, uint64_t ns);
void gpu_note_io(uint64_t ns, uint64_t bytes, bool missing);   // game file reads / missing-file probes (hitch log)
void gpu_prof_report(uint64_t frames, double wall_s);
void gpu_barrier(uint32_t nvn_bits);                               // nvnCommandBufferBarrier
uint64_t gpu_mirrors_prescan();                                     // game thread, queue submit: start the write-watch scan (0 = none)
uint64_t gpu_nl_wait_ns(bool reset);                                 // (hle_nvn.cpp) worker time yielded to game threads
void gpu_set_snapshots(bool on);                                     // frame lag: pool writes are captured at the game's submit
void *gpu_mirrors_snapshot();                                        // game thread, queue submit (frame lag): the written pages
void gpu_mirrors_sync(uint64_t scan, void *snapshot);                // render worker: VRAM pool mirrors take the CPU writes
void gpu_kick();                                                   // submit what is recorded so far, don't wait (frame continues)
void gpu_flush();                                                  // run all recorded GPU work now and wait
uint64_t gpu_sync_point();                                         // nvnQueueFenceSync: the frame its fence belongs to
void gpu_sync_wait(uint64_t serial);                               // nvnSyncWait: wait for that frame on the GPU
void gpu_finish();                                                 // nvnQueueFinish: wait for everything
void gpu_clear_color_int(GpuTex *t, int base_layer, int num_layers, int level, const uint32_t v[4]);
void gpu_fill_buffer(uint64_t addr, uint64_t size, uint32_t value);

bool     gpu_available();                                   // Vulkan initialised (lazily) and usable
GpuTex  *gpu_tex_create(const GpuTexDesc &d);              // nullptr if the format/size can't be created
void     gpu_tex_destroy(GpuTex *t);                        // deferred until the GPU is done with it
bool     gpu_tex_is_depth(const GpuTex *t);

void gpu_clear_color(GpuTex *t, int base_layer, int num_layers, int level, const float rgba[4]);
void gpu_clear_depth_stencil(GpuTex *t, int base_layer, int num_layers, int level,
                             bool clear_depth, float depth, bool clear_stencil, int stencil);
void gpu_set_depth_mode(int nvn_depth_mode);                // nvnDeviceSetDepthMode (0 = -1..1, 1 = 0..1)
void gpu_set_present_crop(int x, int y, int w, int h, bool lower_left);   // nvnWindowSetCrop (0 size = whole texture)
void gpu_present(GpuTex *t);                                // show t in the window, end the frame

const char *gpu_format_name(int nvn_format);
