// xc3-shaderc: offline Maxwell -> SPIR-V translation of shaders captured with XRT_DUMP_SHADERS=1.
//
//   xc3-shaderc <shaders dir> [out dir]
//
// Uses Eden's shader recompiler (third_party/shader_recompiler, GPL-3.0-or-later). Each captured
// .code blob is: 0x30-byte NVN header | 0x50-byte shader program header (SPH) | Maxwell code.
// Addresses given to the translator are offsets into the blob, so code starts at 0x80 and the
// scheduling words stay 32-byte aligned as on hardware.
//
// The environment answers questions the translator would normally ask the GPU state (constant
// buffer values, texture types) with defaults: good enough to measure coverage; the runtime
// version will answer from the real bound state.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace fs = std::filesystem;
using namespace Shader;

class BlobEnvironment final : public Environment {
public:
    BlobEnvironment(std::vector<u8> blob_, Stage stage_) : blob{std::move(blob_)} {
        std::memcpy(&sph, blob.data() + 0x30, sizeof(sph));
        stage = stage_;
        start_address = 0x80;
        is_proprietary_driver = false;
    }
    u64 ReadInstruction(u32 address) override {
        if (address + 8 > blob.size()) throw LogicError("read past the end of the shader at {:#x}", address);
        u64 v; std::memcpy(&v, blob.data() + address, 8); return v;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return TexturePixelFormat::A8B8G8R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return sph.LocalMemorySize(); }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
private:
    std::vector<u8> blob;
};

static Profile host_profile() {
    Profile p{};
    p.supported_spirv = 0x00010500;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp32_signed_zero_nan_preserve = true;
    p.support_explicit_workgroup_layout = true;
    p.support_vote = true;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_derivative_control = true;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = true;
    p.support_geometry_streams = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = true;
    p.lower_left_origin_mode = false;
    p.has_broken_spirv_clamp = false;
    p.max_user_clip_distances = 8;
    return p;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <shaders dir> [out dir]\n", argv[0]); return 1; }
    fs::path in = argv[1], out = argc > 2 ? argv[2] : "";
    if (!out.empty()) fs::create_directories(out);
    const Profile profile = host_profile();
    HostTranslateInfo host{};
    host.support_float64 = host.support_float16 = host.support_int64 = true;
    host.support_snorm_render_buffer = true;
    host.support_viewport_index_layer = true;
    host.min_ssbo_alignment = 64;

    std::vector<fs::path> files;
    for (auto &e : fs::directory_iterator(in)) if (e.path().extension() == ".code") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    int ok = 0, fail = 0, skipped = 0; size_t spirv_words = 0;
    std::map<std::pair<u32, u32>, int> tex_hist, ssbo_hist, img_hist, cbsize_hist; std::map<u32, int> cb_hist;
    std::map<std::string, int> errors; std::map<std::string, std::string> example;
    for (const auto &f : files) {
        std::ifstream s(f, std::ios::binary);
        std::vector<u8> blob((std::istreambuf_iterator<char>(s)), {});
        if (blob.size() < 0x88 || *(u32 *)blob.data() != 0x12345678) { skipped++; continue; }
        u32 sph0; std::memcpy(&sph0, blob.data() + 0x30, 4);
        static const Stage stages[] = {Stage::VertexB, Stage::VertexB, Stage::TessellationControl, Stage::TessellationEval, Stage::Geometry, Stage::Fragment};
        u32 type = (sph0 >> 10) & 0xF;
        if (type < 1 || type > 5) { skipped++; continue; }
        try {
            BlobEnvironment env(std::move(blob), stages[type]);
            ObjectPool<Maxwell::Flow::Block> flow_blocks;
            ObjectPool<IR::Inst> insts;
            ObjectPool<IR::Block> blocks;
            Maxwell::Flow::CFG cfg(env, flow_blocks, env.StartAddress());
            IR::Program program = Maxwell::TranslateProgram(insts, blocks, env, cfg, host);
            RuntimeInfo rt{};
            Backend::Bindings bindings{};
            std::vector<u32> spv = Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
            spirv_words += spv.size();
            for (const auto &t : program.info.texture_descriptors) tex_hist[{t.cbuf_index, t.cbuf_offset}]++;
            for (const auto &cb : program.info.constant_buffer_descriptors) cb_hist[cb.index]++;
            for (const auto &sb : program.info.storage_buffers_descriptors) ssbo_hist[{sb.cbuf_index + 100 * type, sb.cbuf_offset}]++;
            for (const auto &cb : program.info.constant_buffer_descriptors) if (cb.index < 2) cbsize_hist[{cb.index + 100 * type, program.info.constant_buffer_used_sizes[cb.index]}]++;
            for (const auto &im : program.info.image_descriptors) img_hist[{im.cbuf_index, im.cbuf_offset}]++;
            if (!out.empty()) {
                std::ofstream o(out / (f.stem().string() + ".spv"), std::ios::binary);
                o.write((const char *)spv.data(), spv.size() * 4);
            }
            ok++;
        } catch (const std::exception &e) {
            fail++;
            std::string m = e.what();
            for (char &ch : m) if (ch >= '0' && ch <= '9') ch = '#';   // group messages that only differ by numbers
            if (!errors[m]++) example[m] = f.filename().string();
        }
    }
    std::printf("%d translated, %d failed, %d skipped (no shader header) - %zu KB of SPIR-V\n", ok, fail, skipped, spirv_words * 4 / 1024);
    std::printf("constant buffer banks used:"); for (auto &kv : cb_hist) std::printf(" c%u:%d", kv.first, kv.second); std::printf("\n");
    std::printf("texture handle locations (bank, offset): ");
    int shown = 0; for (auto &kv : tex_hist) if (shown++ < 40) std::printf(" (%u,%#x):%d", kv.first.first, kv.first.second, kv.second);
    std::printf("  [%zu distinct]\n", tex_hist.size());
    std::printf("storage buffer address locations:"); for (auto &kv : ssbo_hist) std::printf(" (%u,%#x):%d", kv.first.first, kv.first.second, kv.second); std::printf("\n");
    std::printf("driver banks c0/c1 used size (100*stage+bank, bytes):"); for (auto &kv : cbsize_hist) std::printf(" (%u,%#x):%d", kv.first.first, kv.first.second, kv.second); std::printf("\n");
    std::printf("image handle locations:"); for (auto &kv : img_hist) std::printf(" (%u,%#x):%d", kv.first.first, kv.first.second, kv.second); std::printf("\n");
    std::vector<std::pair<int, std::string>> top;
    for (auto &kv : errors) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 25; i++) std::printf("  %5d  %s   (e.g. %s)\n", top[i].first, top[i].second.c_str(), example[top[i].second].c_str());
    return 0;
}
