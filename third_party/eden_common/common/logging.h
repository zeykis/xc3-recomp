// xc3-recomp shim: replaces Eden's logging system for the shader recompiler.
// Messages go to stderr when XC3_SHADER_LOG is set; otherwise they are dropped.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/swap.h"

namespace Common::Log { bool Enabled(); }
// Arguments are not formatted (Eden formats enum types through its own logging backend).
#define XC3_LOG(level, cls, ...) do { if (::Common::Log::Enabled()) { std::fprintf(stderr, "[shader %s] %s:%d\n", level, __FILE__, __LINE__); } } while (0)
#define LOG_TRACE(cls, ...) do {} while (0)
#define LOG_DEBUG(cls, ...) do {} while (0)
#define LOG_INFO(cls, ...) XC3_LOG("info", cls, __VA_ARGS__)
#define LOG_WARNING(cls, ...) XC3_LOG("warning", cls, __VA_ARGS__)
#define LOG_ERROR(cls, ...) XC3_LOG("error", cls, __VA_ARGS__)
#define LOG_CRITICAL(cls, ...) XC3_LOG("critical", cls, __VA_ARGS__)
