// Runtime shader translation (Maxwell -> SPIR-V) through Eden's shader recompiler.
// This header is C++17 and free of Eden types; gpu_shader.cpp (C++20) does the work.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Questions the translator asks about GPU state at translation time.
struct ShaderEnvQueries {
    void *user;
    uint32_t (*read_cbuf)(void *user, int stage, uint32_t bank, uint32_t offset);   // stage = GpuStage
    int (*texture_type)(void *user, uint32_t raw_handle);                          // ShaderTexType
    bool (*texture_integer)(void *user, uint32_t raw_handle);                      // sampled as integer (UI formats, stencil)
};

enum GpuStage { GS_VERTEX = 0, GS_TESS_CTRL = 1, GS_TESS_EVAL = 2, GS_GEOMETRY = 3, GS_FRAGMENT = 4, GS_COUNT = 5 };
enum ShaderTexType { TT_1D, TT_1D_ARRAY, TT_2D, TT_2D_ARRAY, TT_3D, TT_CUBE, TT_CUBE_ARRAY, TT_BUFFER, TT_2D_RECT };
enum ShaderAttribType { AT_FLOAT, AT_SINT, AT_UINT, AT_SSCALED, AT_USCALED, AT_DISABLED };

// One descriptor binding, in the exact order the generated SPIR-V numbers them (set 0).
struct ShaderBinding {
    enum Kind { UNIFORM, STORAGE, TEXEL_BUFFER, IMAGE_BUFFER, SAMPLED, IMAGE } kind;
    int stage;
    uint32_t binding, count;
    uint32_t bank;                                   // UNIFORM: constant buffer bank
    uint32_t cbuf_index, cbuf_offset, shift_left;    // where the handle / address lives
    bool has_secondary;
    uint32_t sec_index, sec_offset, sec_shift_left;
    int tex_type;                                    // ShaderTexType
    bool is_depth, is_written;
};

struct ShaderProgramDesc {
    bool ok = false;
    std::string error;
    std::vector<uint32_t> spirv[GS_COUNT];           // empty = stage absent
    std::vector<ShaderBinding> bindings;
    uint32_t num_bindings = 0;
    uint32_t vertex_inputs = 0;                      // bit i: generic attribute i is read
    uint32_t frag_outputs = 0;                       // bit i: colour output i is written
    uint32_t push_constant_stages = 0;               // bit per GpuStage using the rescaling/render-area push constants
    uint32_t ydir_stages = 0;                        // bit per GpuStage reading SR_Y_DIRECTION (window-origin aware code)
    bool writes_memory = false;                      // some stage stores to a storage buffer / image (later draws may read it)
};

// code[s] points at the NVN shader blob (0x30-byte header + SPH + code) of stage s, or null.
// bound_tex_bank: the constant-buffer bank the translator uses to model bound textures (see hle_nvn).
bool gpu_translate_program(const uint8_t *const code[GS_COUNT], const uint8_t *const code_end[GS_COUNT],
                           const uint8_t attrib_types[32], bool convert_depth, bool y_negate, const uint8_t *xfb_locs, int xfb_count, int xfb_stride,
                           int alpha_func, float alpha_ref,   // NVNalphaFunc 1..7, 0 = no alpha test
                           const ShaderEnvQueries &q, ShaderProgramDesc &out);

// Textures bound with nvnCommandBufferBindTexture appear to shaders as 64-bit handles in constant
// buffer bank 2 at 0x20 + 8 * slot (measured on all captured shaders; the game never uses bank 2).
static const uint32_t SHADER_BOUND_TEX_BANK = 2;
static const uint32_t SHADER_BOUND_IMG_OFF = 0x120;   // bank 2: bound image handles, 8 bytes per slot
inline int bound_tex_slot(uint32_t offset) { return offset >= 0x20 ? (int)((offset - 0x20) / 8) : -1; }
