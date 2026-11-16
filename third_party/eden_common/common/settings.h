// xc3-recomp shim: the few emulator settings the shader recompiler reads.
#pragma once
#include "common/common_types.h"
namespace Settings {
struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{false};
};
struct Values {
    ResolutionScalingInfo resolution_info{};
    bool renderer_debug{false};
    bool disable_shader_loop_safety_checks{false};
};
extern Values values;
}
