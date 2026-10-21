// Ghost Recon: Future Soldier - native PS5 port: the AI module
// (AICLASS_X360_F.xex), linked into the app (see CMakeLists.txt), handed to
// the runtime under the shared library name the generated registry gives it.
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

extern "C" void grfs_ai_ReXModule_Register(rex::runtime::IModuleRegistrar* registrar);
extern "C" const rex::PPCImageInfo* grfs_ai_ReXModule_GetImageInfo();

namespace {
const bool registered = [] {
  rex::system::KernelState::RegisterStaticModuleLibrary(
      "grfs_AICLASS_X360_F", grfs_ai_ReXModule_Register, grfs_ai_ReXModule_GetImageInfo);
  return true;
}();
}  // namespace
