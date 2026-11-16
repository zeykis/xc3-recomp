// NVN texture formats: block size, bytes per block, Vulkan + DXGI equivalents.
// NVNformat values match the image format ids stored in Xenoblade's texture files.
#pragma once
#include <cstdint>
#include <cstddef>

struct NvnFormat {
    int nvn;
    const char *name;
    int bw, bh;        // block dimensions in texels (1x1 for uncompressed)
    int bpb;           // bytes per block (= bytes per texel when uncompressed)
    int depth;         // 0 colour, 1 depth, 2 depth+stencil, 3 stencil
    int vk;            // VkFormat
    int dxgi;          // DXGI_FORMAT (for .dds dumps), 0 = none
};

const NvnFormat *nvn_format(int nvn);              // nullptr if unknown
const char *nvn_format_name(int nvn);

// Tegra X1 block-linear layout ("GOB" = 64 bytes x 8 rows = 512 bytes)
int      bl_block_height(int height_in_blocks);    // GOBs per block vertically (1..16), as NVN picks it
uint64_t bl_level_size(int w_blocks, int h_blocks, int bpb, int block_height);
// Copy one 2D level from block-linear `src` to tightly packed linear `dst`.
void     bl_deswizzle(const uint8_t *src, uint8_t *dst, int w_blocks, int h_blocks, int bpb, int block_height);

// 3D: blocks are 1 GOB wide x block_height GOBs tall x block_depth GOBs deep; output is [z][y][x]
int      bl_block_depth(int depth);
uint64_t bl_level_size_3d(int w_blocks, int h_blocks, int depth, int bpb, int block_height, int block_depth);
void     bl_deswizzle_3d(const uint8_t *src, uint8_t *dst, int w_blocks, int h_blocks, int depth, int bpb, int block_height, int block_depth);

// Write a linear region into a block-linear level (inverse of the deswizzles; bd = 1 for 2D levels).
// x0/y0/z0 and w/h/d are in blocks; src rows are src_row bytes apart, slices src_slice bytes apart.
void     bl_write_region(uint8_t *level_base, const uint8_t *src, int x0, int y0, int z0, int w, int h, int d,
                         uint64_t src_row, uint64_t src_slice, int w_blocks, int h_blocks, int bpb, int bh, int bd);

// Write a .dds file (DX10 header) containing one 2D level. Returns false on failure.
bool write_dds(const char *path, const NvnFormat &f, int width, int height, const uint8_t *data, size_t size);
