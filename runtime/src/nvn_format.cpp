#include "nvn_format.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define C(n, name, bpb, vk, dx) {n, name, 1, 1, bpb, 0, vk, dx}
#define D(n, name, bpb, kind, vk) {n, name, 1, 1, bpb, kind, vk, 0}
#define BC(n, name, bpb, vk, dx) {n, name, 4, 4, bpb, 0, vk, dx}
static const NvnFormat k_formats[] = {
    C(1, "R8", 1, VK_FORMAT_R8_UNORM, 61),                  C(2, "R8SN", 1, VK_FORMAT_R8_SNORM, 63),
    C(3, "R8UI", 1, VK_FORMAT_R8_UINT, 62),                 C(4, "R8I", 1, VK_FORMAT_R8_SINT, 64),
    C(5, "R16F", 2, VK_FORMAT_R16_SFLOAT, 54),              C(6, "R16", 2, VK_FORMAT_R16_UNORM, 56),
    C(7, "R16SN", 2, VK_FORMAT_R16_SNORM, 58),              C(8, "R16UI", 2, VK_FORMAT_R16_UINT, 57),
    C(9, "R16I", 2, VK_FORMAT_R16_SINT, 59),                C(10, "R32F", 4, VK_FORMAT_R32_SFLOAT, 41),
    C(11, "R32UI", 4, VK_FORMAT_R32_UINT, 42),              C(12, "R32I", 4, VK_FORMAT_R32_SINT, 43),
    C(13, "RG8", 2, VK_FORMAT_R8G8_UNORM, 49),              C(14, "RG8SN", 2, VK_FORMAT_R8G8_SNORM, 51),
    C(15, "RG8UI", 2, VK_FORMAT_R8G8_UINT, 50),             C(16, "RG8I", 2, VK_FORMAT_R8G8_SINT, 52),
    C(17, "RG16F", 4, VK_FORMAT_R16G16_SFLOAT, 34),         C(18, "RG16", 4, VK_FORMAT_R16G16_UNORM, 35),
    C(19, "RG16SN", 4, VK_FORMAT_R16G16_SNORM, 37),         C(20, "RG16UI", 4, VK_FORMAT_R16G16_UINT, 36),
    C(21, "RG16I", 4, VK_FORMAT_R16G16_SINT, 38),           C(22, "RG32F", 8, VK_FORMAT_R32G32_SFLOAT, 16),
    C(23, "RG32UI", 8, VK_FORMAT_R32G32_UINT, 17),          C(24, "RG32I", 8, VK_FORMAT_R32G32_SINT, 18),
    C(37, "RGBA8", 4, VK_FORMAT_R8G8B8A8_UNORM, 28),        C(38, "RGBA8SN", 4, VK_FORMAT_R8G8B8A8_SNORM, 31),
    C(39, "RGBA8UI", 4, VK_FORMAT_R8G8B8A8_UINT, 30),       C(40, "RGBA8I", 4, VK_FORMAT_R8G8B8A8_SINT, 32),
    C(41, "RGBA16F", 8, VK_FORMAT_R16G16B16A16_SFLOAT, 10), C(42, "RGBA16", 8, VK_FORMAT_R16G16B16A16_UNORM, 11),
    C(43, "RGBA16SN", 8, VK_FORMAT_R16G16B16A16_SNORM, 13), C(44, "RGBA16UI", 8, VK_FORMAT_R16G16B16A16_UINT, 12),
    C(45, "RGBA16I", 8, VK_FORMAT_R16G16B16A16_SINT, 14),   C(46, "RGBA32F", 16, VK_FORMAT_R32G32B32A32_SFLOAT, 2),
    C(47, "RGBA32UI", 16, VK_FORMAT_R32G32B32A32_UINT, 3),  C(48, "RGBA32I", 16, VK_FORMAT_R32G32B32A32_SINT, 4),
    D(49, "S8", 1, 3, VK_FORMAT_S8_UINT),                   D(50, "D16", 2, 1, VK_FORMAT_D16_UNORM),
    D(51, "D24", 4, 1, VK_FORMAT_X8_D24_UNORM_PACK32),      D(52, "D32F", 4, 1, VK_FORMAT_D32_SFLOAT),
    D(53, "D24S8", 4, 2, VK_FORMAT_D24_UNORM_S8_UINT),      D(54, "D32FS8", 8, 2, VK_FORMAT_D32_SFLOAT_S8_UINT),
    C(55, "RGBX8_SRGB", 4, VK_FORMAT_R8G8B8A8_SRGB, 29),    C(56, "RGBA8_SRGB", 4, VK_FORMAT_R8G8B8A8_SRGB, 29),
    C(57, "RGBA4", 2, VK_FORMAT_R4G4B4A4_UNORM_PACK16, 0),  C(58, "RGB5", 2, VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0),
    C(59, "RGB5A1", 2, VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0), C(60, "RGB565", 2, VK_FORMAT_R5G6B5_UNORM_PACK16, 0),
    C(61, "RGB10A2", 4, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 24),
    C(62, "RGB10A2UI", 4, VK_FORMAT_A2B10G10R10_UINT_PACK32, 25),
    C(63, "R11G11B10F", 4, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 26),
    C(64, "RGB9E5F", 4, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 67),
    BC(65, "BC1_RGB", 8, VK_FORMAT_BC1_RGB_UNORM_BLOCK, 71),  BC(66, "BC1", 8, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 71),
    BC(67, "BC2", 16, VK_FORMAT_BC2_UNORM_BLOCK, 74),          BC(68, "BC3", 16, VK_FORMAT_BC3_UNORM_BLOCK, 77),
    BC(69, "BC1_RGB_SRGB", 8, VK_FORMAT_BC1_RGB_SRGB_BLOCK, 72), BC(70, "BC1_SRGB", 8, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 72),
    BC(71, "BC2_SRGB", 16, VK_FORMAT_BC2_SRGB_BLOCK, 75),      BC(72, "BC3_SRGB", 16, VK_FORMAT_BC3_SRGB_BLOCK, 78),
    BC(73, "BC4", 8, VK_FORMAT_BC4_UNORM_BLOCK, 80),           BC(74, "BC4SN", 8, VK_FORMAT_BC4_SNORM_BLOCK, 81),
    BC(75, "BC5", 16, VK_FORMAT_BC5_UNORM_BLOCK, 83),          BC(76, "BC5SN", 16, VK_FORMAT_BC5_SNORM_BLOCK, 84),
    BC(77, "BC7", 16, VK_FORMAT_BC7_UNORM_BLOCK, 98),          BC(78, "BC7_SRGB", 16, VK_FORMAT_BC7_SRGB_BLOCK, 99),
    BC(79, "BC6H_SF", 16, VK_FORMAT_BC6H_SFLOAT_BLOCK, 96),    BC(80, "BC6H_UF", 16, VK_FORMAT_BC6H_UFLOAT_BLOCK, 95),
    // 81..108 are vertex-only formats (*_UI2F / *_I2F, RGB8/16/32, RGB10A2 variants). RGBX = RGBA with alpha ignored.
    C(109, "RGBX8", 4, VK_FORMAT_R8G8B8A8_UNORM, 28),       C(110, "RGBX8SN", 4, VK_FORMAT_R8G8B8A8_SNORM, 31),
    C(111, "RGBX8UI", 4, VK_FORMAT_R8G8B8A8_UINT, 30),      C(112, "RGBX8I", 4, VK_FORMAT_R8G8B8A8_SINT, 32),
    C(113, "RGBX16F", 8, VK_FORMAT_R16G16B16A16_SFLOAT, 10), C(114, "RGBX16", 8, VK_FORMAT_R16G16B16A16_UNORM, 11),
    C(115, "RGBX16SN", 8, VK_FORMAT_R16G16B16A16_SNORM, 13), C(116, "RGBX16UI", 8, VK_FORMAT_R16G16B16A16_UINT, 12),
    C(117, "RGBX16I", 8, VK_FORMAT_R16G16B16A16_SINT, 14),  C(118, "RGBX32F", 16, VK_FORMAT_R32G32B32A32_SFLOAT, 2),
    C(119, "RGBX32UI", 16, VK_FORMAT_R32G32B32A32_UINT, 3), C(120, "RGBX32I", 16, VK_FORMAT_R32G32B32A32_SINT, 4),
};

const NvnFormat *nvn_format(int nvn) {
    for (const NvnFormat &f : k_formats) if (f.nvn == nvn) return &f;
    return nullptr;
}
const char *nvn_format_name(int nvn) {
    const NvnFormat *f = nvn_format(nvn);
    if (f) return f->name;
    static thread_local char buf[24]; snprintf(buf, sizeof buf, "fmt%d", nvn); return buf;
}

// ---------------------------------------------------------------- block linear
// Same heuristic as the Tegra driver (documented by the open-source tegra_swizzle project):
// pick the block height from height * 1.5, so a 72-row image uses 8 GOBs, not 16.
int bl_block_height(int h_blocks) {
    int hh = h_blocks + h_blocks / 2;
    if (hh >= 128) return 16;
    if (hh >= 64) return 8;
    if (hh >= 32) return 4;
    if (hh >= 16) return 2;
    return 1;
}
uint64_t bl_level_size(int w_blocks, int h_blocks, int bpb, int block_height) {
    uint64_t gobs_x = ((uint64_t)w_blocks * bpb + 63) / 64;
    uint64_t rows = (uint64_t)block_height * 8;
    uint64_t blocks_y = ((uint64_t)h_blocks + rows - 1) / rows;
    return gobs_x * blocks_y * block_height * 512;
}
void bl_deswizzle(const uint8_t *src, uint8_t *dst, int w_blocks, int h_blocks, int bpb, int bh) {
    uint64_t pitch = (uint64_t)w_blocks * bpb;
    uint64_t gobs_x = (pitch + 63) / 64;
    uint64_t block_size = (uint64_t)bh * 512;          // one block = 1 GOB wide, bh GOBs tall
    uint64_t row_of_blocks = gobs_x * block_size;
    for (int y = 0; y < h_blocks; y++) {
        uint64_t yb = (uint64_t)y / (8 * bh), yg = ((uint64_t)y % (8 * bh)) / 8, yi = (uint64_t)y % 8;
        uint64_t base = yb * row_of_blocks + yg * 512 + (yi / 2) * 64 + (yi % 2) * 16;
        uint8_t *out = dst + (uint64_t)y * pitch;
        for (uint64_t x = 0; x < pitch; x += 16) {          // 16-byte runs are contiguous inside a GOB
            uint64_t a = base + (x / 64) * block_size + ((x % 64) / 32) * 256 + ((x % 32) / 16) * 32;
            uint64_t n = pitch - x < 16 ? pitch - x : 16;
            memcpy(out + x, src + a, n);
        }
    }
}

int bl_block_depth(int d) {
    int dd = d + d / 2;
    if (dd >= 16) return 16;
    if (dd >= 8) return 8;
    if (dd >= 4) return 4;
    if (dd >= 2) return 2;
    return 1;
}
uint64_t bl_level_size_3d(int w_blocks, int h_blocks, int depth, int bpb, int bh, int bd) {
    uint64_t gobs_x = ((uint64_t)w_blocks * bpb + 63) / 64;
    uint64_t blocks_y = ((uint64_t)h_blocks + 8 * bh - 1) / (8 * bh);
    uint64_t blocks_z = ((uint64_t)depth + bd - 1) / bd;
    return gobs_x * blocks_y * blocks_z * 512ULL * bh * bd;
}
void bl_deswizzle_3d(const uint8_t *src, uint8_t *dst, int w_blocks, int h_blocks, int depth, int bpb, int bh, int bd) {
    uint64_t pitch = (uint64_t)w_blocks * bpb;
    uint64_t gobs_x = (pitch + 63) / 64;
    uint64_t block_size = 512ULL * bh * bd;
    uint64_t blocks_y = ((uint64_t)h_blocks + 8 * bh - 1) / (8 * bh);
    uint64_t slice_size = gobs_x * block_size * blocks_y;      // one row of blocks in z
    for (int z = 0; z < depth; z++)
        for (int y = 0; y < h_blocks; y++) {
            uint64_t base = (uint64_t)(z / bd) * slice_size + (uint64_t)(z % bd) * 512ULL * bh
                          + ((uint64_t)y / (8 * bh)) * gobs_x * block_size + (((uint64_t)y % (8 * bh)) / 8) * 512
                          + ((y % 8) / 2) * 64 + (y % 2) * 16;
            uint8_t *out = dst + ((uint64_t)z * h_blocks + y) * pitch;
            for (uint64_t x = 0; x < pitch; x += 16) {
                uint64_t a = base + (x / 64) * block_size + ((x % 64) / 32) * 256 + ((x % 32) / 16) * 32;
                uint64_t n = pitch - x < 16 ? pitch - x : 16;
                memcpy(out + x, src + a, n);
            }
        }
}

void bl_write_region(uint8_t *dst, const uint8_t *src, int x0, int y0, int z0, int w, int h, int d,
                     uint64_t src_row, uint64_t src_slice, int w_blocks, int h_blocks, int bpb, int bh, int bd) {
    uint64_t pitch = (uint64_t)w_blocks * bpb;
    uint64_t gobs_x = (pitch + 63) / 64;
    uint64_t block_size = 512ULL * bh * bd;
    uint64_t blocks_y = ((uint64_t)h_blocks + 8 * bh - 1) / (8 * bh);
    uint64_t slice_size = gobs_x * block_size * blocks_y;
    for (int dz = 0; dz < d; dz++)
        for (int dy = 0; dy < h; dy++) {
            int z = z0 + dz, y = y0 + dy;
            if (y < 0 || y >= h_blocks) continue;
            uint64_t base = (uint64_t)(z / bd) * slice_size + (uint64_t)(z % bd) * 512ULL * bh
                          + ((uint64_t)y / (8 * bh)) * gobs_x * block_size + (((uint64_t)y % (8 * bh)) / 8) * 512
                          + ((y % 8) / 2) * 64 + (y % 2) * 16;
            const uint8_t *in = src + dz * src_slice + dy * src_row;
            uint64_t xs = (uint64_t)x0 * bpb, xe = std::min<uint64_t>((uint64_t)(x0 + w) * bpb, pitch);
            for (uint64_t x = xs; x < xe;) {
                uint64_t run = std::min<uint64_t>(16 - x % 16, xe - x);      // stay inside one 16-byte run
                uint64_t a = base + (x / 64) * block_size + ((x % 64) / 32) * 256 + ((x % 32) / 16) * 32 + x % 16;
                memcpy(dst + a, in + (x - xs), run);
                x += run;
            }
        }
}

// ---------------------------------------------------------------- DDS
bool write_dds(const char *path, const NvnFormat &f, int w, int h, const uint8_t *data, size_t size) {
    if (!f.dxgi) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    uint32_t hdr[32] = {};
    hdr[0] = 0x20534444;                 // "DDS "
    hdr[1] = 124;                        // header size
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (f.bw > 1 ? 0x80000 : 0x8);   // CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE/PITCH
    hdr[3] = (uint32_t)h; hdr[4] = (uint32_t)w;
    hdr[5] = f.bw > 1 ? (uint32_t)size : (uint32_t)(w * f.bpb);
    hdr[7] = 1;                          // mip count
    hdr[19] = 32;                        // pixel format size
    hdr[20] = 0x4;                       // DDPF_FOURCC
    hdr[21] = 0x30315844;                // "DX10"
    hdr[27] = 0x1000;                    // DDSCAPS_TEXTURE
    uint32_t dx10[5] = {(uint32_t)f.dxgi, 3 /* TEXTURE2D */, 0, 1, 0};
    fwrite(hdr, 4, 32, fp); fwrite(dx10, 4, 5, fp); fwrite(data, 1, size, fp);
    fclose(fp);
    return true;
}
