// xc3-recomp shim implementations for Eden's common library.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/settings.h"
namespace Settings { Values values{}; }
namespace Common::Log { bool Enabled() { static int e = getenv("XC3_SHADER_LOG") ? 1 : 0; return e; } }
void AssertFailSoftImpl() {}
// a fatal assert inside the translator becomes an exception the caller reports per shader
[[noreturn]] void AssertFatalImpl() { throw std::runtime_error("shader recompiler: fatal assert"); }
