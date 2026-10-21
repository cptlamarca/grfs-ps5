#!/usr/bin/env python3
"""Make the generated code of Ghost Recon: Future Soldier buildable for the PS5.

Adapted from the SvR 2011 PS5 port's tools/ps5_patch_generated.py:
- rexglue codegen runs on the host (it writes the same C++ for every platform);
  in a cross build the codegen step is skipped, as its tool would be a PS5
  program.
- The PS5's 16 KB pages put guest addresses from 0xE0000000 0x1000 higher in
  host memory, as on Windows and arm64 macOS; the codegen templates leave the
  PS5 out of that.
Idempotent: run it after every codegen.

usage: ps5_patch_generated.py [generated dir]
"""
import pathlib
import sys

gen = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "generated")

old_pch = "#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)\n"
new_pch = "#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64) || REX_PLATFORM_PS5\n"
for pch in gen.glob("*/grfs_pch.h"):
    t = pch.read_text()
    if old_pch in t:
        pch.write_text(t.replace(old_pch, new_pch))
        print(f"{pch}: 0xE0000000 host offset on the PS5")

path = gen / "rexglue.cmake"
s = path.read_text()
if "[ps5 port]" in s:
    print(f"{path}: already patched")
    sys.exit(0)


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f"{path}: expected text not found once: {old[:60]!r}")
    s = s.replace(old, new)


rep("        add_dependencies(${target_name}_recomp grfs_codegen)\n",
    "        if(NOT CMAKE_CROSSCOMPILING)  # [ps5 port]\n"
    "            add_dependencies(${target_name}_recomp grfs_codegen)\n"
    "        endif()\n")
rep("    add_dependencies(${target_name} grfs_codegen)\n",
    "    if(NOT CMAKE_CROSSCOMPILING)  # [ps5 port]\n"
    "        add_dependencies(${target_name} grfs_codegen)\n"
    "    endif()\n")
rep("add_custom_command(\n    OUTPUT \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\"",
    "if(CMAKE_CROSSCOMPILING)\n"
    "    # [ps5 port] the codegen tool would be a PS5 program: run codegen on the host.\n"
    "    add_custom_target(grfs_codegen)\n"
    "else()\n"
    "add_custom_command(\n    OUTPUT \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\"")
rep("add_custom_target(grfs_codegen\n    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\")\n",
    "add_custom_target(grfs_codegen\n    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\")\n"
    "endif()\n")
# The DLL module (AI) is linked into the app on the PS5 (no dlopen of our own
# libraries there): CMakeLists.txt builds it, not dll_targets.cmake.
rep("if(EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dll_targets.cmake\")",
    "if(NOT CMAKE_CROSSCOMPILING AND EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dll_targets.cmake\")  # [ps5 port]")
path.write_text(s)
print(f"{path}: patched for cross builds")
