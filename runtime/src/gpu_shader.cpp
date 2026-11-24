// Runtime Maxwell -> SPIR-V translation through Eden's shader recompiler (GPL-3.0-or-later).
// C++20 translation unit; everything Eden-specific stays in here.
#include "gpu_shader.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

using namespace Shader;

namespace {

class GuestEnvironment final : public Environment {
public:
    GuestEnvironment(const u8 *blob_, const u8 *end_, Stage stage_, int gstage_, const ShaderEnvQueries &q_)
        : blob{blob_}, end{end_}, gstage{gstage_}, q{q_} {
        std::memcpy(&sph, blob + 0x30, sizeof(sph));
        stage = stage_;
        start_address = 0x80;            // blob offsets: code follows the 0x30 NVN header + 0x50 SPH
    }
    u64 ReadInstruction(u32 address) override {
        if (blob + address + 8 > end) throw LogicError("shader read past the end at {:#x}", address);
        u64 v; std::memcpy(&v, blob + address, 8); return v;
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override { return q.read_cbuf(q.user, gstage, bank, offset); }
    TextureType ReadTextureType(u32 handle) override { return (TextureType)q.texture_type(q.user, handle); }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return TexturePixelFormat::A8B8G8R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32 handle) override { return q.texture_integer && q.texture_integer(q.user, handle); }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return SHADER_BOUND_TEX_BANK; }
    u32 LocalMemorySize() const override { return (u32)sph.LocalMemorySize(); }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
private:
    const u8 *blob, *end;
    int gstage;
    const ShaderEnvQueries &q;
};

Profile make_profile() {
    Profile p{};
    p.supported_spirv = 0x00010500;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_float_controls = false;
    p.support_vote = true;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_derivative_control = true;
    p.support_multi_viewport = true;
    p.support_geometry_streams = true;          // needs VK_EXT_transform_feedback geometryStreams (enabled in gpu_vk.cpp)
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = true;
    p.max_user_clip_distances = 8;
    return p;
}

const Stage k_stage[GS_COUNT] = {Stage::VertexB, Stage::TessellationControl, Stage::TessellationEval, Stage::Geometry, Stage::Fragment};

}  // namespace

bool gpu_translate_program(const uint8_t *const code[GS_COUNT], const uint8_t *const code_end[GS_COUNT],
                           const uint8_t attrib_types[32], bool convert_depth, bool y_negate, const uint8_t *xfb_locs, int xfb_count, int xfb_stride,
                           int alpha_func, float alpha_ref,
                           const ShaderEnvQueries &q, ShaderProgramDesc &out) {
    static const Profile profile = make_profile();
    HostTranslateInfo host{};
    host.support_float64 = host.support_float16 = host.support_int64 = true;
    host.support_snorm_render_buffer = true;
    host.support_viewport_index_layer = true;
    host.min_ssbo_alignment = 64;
    host.max_per_stage_descriptor_sampled_images = HostTranslateInfo::DEFAULT_DESCRIPTOR_LIMIT;
    host.max_per_stage_resources = HostTranslateInfo::DEFAULT_DESCRIPTOR_LIMIT * 4;

    out = ShaderProgramDesc{};
    try {
        ObjectPool<Maxwell::Flow::Block> flow_blocks;
        ObjectPool<IR::Inst> insts;
        ObjectPool<IR::Block> blocks;
        std::optional<IR::Program> programs[GS_COUNT];
        std::optional<GuestEnvironment> envs[GS_COUNT];
        for (int s = 0; s < GS_COUNT; s++) {
            if (!code[s]) continue;
            envs[s].emplace(code[s], code_end[s], k_stage[s], s, q);
            Maxwell::Flow::CFG cfg(*envs[s], flow_blocks, envs[s]->StartAddress());
            programs[s] = Maxwell::TranslateProgram(insts, blocks, *envs[s], cfg, host);
        }
        Backend::Bindings bindings{};
        const IR::Program *prev = nullptr;
        for (int s = 0; s < GS_COUNT; s++) {
            if (!programs[s]) continue;
            IR::Program &p = *programs[s];
            RuntimeInfo rt{};
            // vertex-buffer formats only describe the vertex shader's inputs; later stages read float varyings
            for (int i = 0; i < 32; i++) rt.generic_input_types[i] = s == GS_VERTEX ? (AttributeType)attrib_types[i] : AttributeType::Float;
            if (prev) { rt.previous_stage_stores = prev->info.stores; rt.previous_stage_legacy_stores_mapping = prev->info.legacy_stores_mapping; }
            else rt.previous_stage_stores.mask.set();   // first stage: every vertex attribute is "provided"
            bool last_geometry_stage = s == GS_GEOMETRY || (s == GS_TESS_EVAL && !programs[GS_GEOMETRY]) || (s == GS_VERTEX && !programs[GS_TESS_EVAL] && !programs[GS_GEOMETRY]);
            rt.convert_depth_mode = convert_depth && last_geometry_stage;
            rt.y_negate = y_negate;
            if (s == GS_FRAGMENT && alpha_func >= 1 && alpha_func <= 7) {   // NVN 1 NEVER .. 7 GEQUAL = Eden order + 1
                rt.alpha_test_func = (CompareFunction)(alpha_func - 1); rt.alpha_test_reference = alpha_ref;
            }
            if (last_geometry_stage && xfb_count > 0) {
                // same grouping as Eden's MakeTransformFeedbackVaryings: consecutive words of one vec4 form a varying
                const u32 stride = xfb_stride > 0 ? (u32)xfb_stride : (u32)xfb_count * 4;
                u32 hi = 0;
                for (int o = 0; o < xfb_count;) {
                    const u32 attr = xfb_locs[o];
                    int n = 1;
                    while (o + n < xfb_count && xfb_locs[o + n] == attr + (u32)n && (attr % 4) + (u32)n < 4) n++;
                    if (attr < 256) { rt.xfb_varyings[attr] = TransformFeedbackVarying{0, 0, stride, (u32)o * 4, (u32)n}; hi = std::max(hi, attr + (u32)n); }
                    o += n;
                }
                rt.xfb_count = hi;
            }
            rt.input_topology = InputTopology::Triangles;
            const u32 first = bindings.unified;
            out.spirv[s] = Backend::SPIRV::EmitSPIRV(profile, rt, p, bindings);
            // record bindings in the order emit_spirv_context assigns them
            u32 b = first;
            const Info &info = p.info;
            for (const auto &d : info.constant_buffer_descriptors) {
                ShaderBinding x{}; x.kind = ShaderBinding::UNIFORM; x.stage = s; x.binding = b; x.count = d.count; x.bank = d.index;
                out.bindings.push_back(x); b += d.count;
            }
            for (const auto &d : info.storage_buffers_descriptors) {
                ShaderBinding x{}; x.kind = ShaderBinding::STORAGE; x.stage = s; x.binding = b; x.count = d.count;
                x.cbuf_index = d.cbuf_index; x.cbuf_offset = d.cbuf_offset; x.is_written = d.is_written;
                out.bindings.push_back(x); b += d.count;
            }
            for (const auto &d : info.texture_buffer_descriptors) {
                ShaderBinding x{}; x.kind = ShaderBinding::TEXEL_BUFFER; x.stage = s; x.binding = b; x.count = d.count;
                x.cbuf_index = d.cbuf_index; x.cbuf_offset = d.cbuf_offset; x.shift_left = d.shift_left;
                x.has_secondary = d.has_secondary; x.sec_index = d.secondary_cbuf_index; x.sec_offset = d.secondary_cbuf_offset; x.sec_shift_left = d.secondary_shift_left;
                out.bindings.push_back(x); b += d.count;
            }
            for (const auto &d : info.image_buffer_descriptors) {
                ShaderBinding x{}; x.kind = ShaderBinding::IMAGE_BUFFER; x.stage = s; x.binding = b; x.count = d.count;
                x.cbuf_index = d.cbuf_index; x.cbuf_offset = d.cbuf_offset; x.is_written = d.is_written;
                out.bindings.push_back(x); b += d.count;
            }
            for (const auto &d : info.texture_descriptors) {
                ShaderBinding x{}; x.kind = ShaderBinding::SAMPLED; x.stage = s; x.binding = b; x.count = d.count;
                x.cbuf_index = d.cbuf_index; x.cbuf_offset = d.cbuf_offset; x.shift_left = d.shift_left;
                x.has_secondary = d.has_secondary; x.sec_index = d.secondary_cbuf_index; x.sec_offset = d.secondary_cbuf_offset; x.sec_shift_left = d.secondary_shift_left;
                x.tex_type = (int)d.type; x.is_depth = d.is_depth;
                out.bindings.push_back(x); b += d.count;
            }
            for (const auto &d : info.image_descriptors) {
                ShaderBinding x{}; x.kind = ShaderBinding::IMAGE; x.stage = s; x.binding = b; x.count = d.count;
                x.cbuf_index = d.cbuf_index; x.cbuf_offset = d.cbuf_offset; x.tex_type = (int)d.type; x.is_written = d.is_written;
                out.bindings.push_back(x); b += d.count;
            }
            if (b != bindings.unified) throw LogicError("binding count mismatch ({} vs {})", b, bindings.unified);
            if (info.uses_rescaling_uniform || info.uses_render_area) out.push_constant_stages |= 1u << s;
            for (IR::Block *blk : p.post_order_blocks)
                for (IR::Inst &inst : blk->Instructions())
                    if (inst.GetOpcode() == IR::Opcode::YDirection) out.ydir_stages |= 1u << s;
            if (s == GS_VERTEX)
                for (int i = 0; i < 32; i++) if (info.loads.Generic(i)) out.vertex_inputs |= 1u << i;
            if (s == GS_FRAGMENT)
                for (int i = 0; i < 8; i++) if (info.stores_frag_color[i]) out.frag_outputs |= 1u << i;
            prev = &p;
        }
        out.num_bindings = bindings.unified;
        for (const ShaderBinding &x : out.bindings) if (x.is_written) out.writes_memory = true;
        out.ok = true;
    } catch (const std::exception &e) {
        out.ok = false;
        out.error = e.what();
    }
    return out.ok;
}

// identifies this translator build: the translated-shader cache on disk is dropped when it changes (gpu_vk_draw.inc)
// shader_cache.bin is kept while this is the same: the translator's source id (CMake) - not the build time any more
// (every rebuild started the cache over). Bump "xc3-sc-N" when what the runtime feeds the translator changes.
#if __has_include("shader_version.h")
#include "shader_version.h"
#endif
#ifndef XC3_SHADER_TRANSLATOR_ID
#define XC3_SHADER_TRANSLATOR_ID __DATE__ " " __TIME__
#endif
const char *gpu_shader_version() { return "xc3-sc-2 " XC3_SHADER_TRANSLATOR_ID; }
