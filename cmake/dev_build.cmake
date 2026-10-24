# Initial cache for fork development builds (scripts/dev_build.py, Ninja).
# Cache entries only. Compile flags otherwise stay those of a from-ROM build.

# Debug info goes into each object (/Z7) instead of a shared PDB (/Zi): a
# compiler cache such as sccache can only cache /Z7 objects. The top-level
# cmake_minimum_required(3.20) leaves CMP0141 unset, so it is set here.
set(CMAKE_POLICY_DEFAULT_CMP0141 NEW CACHE STRING "" FORCE)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE STRING "" FORCE)

# JFG_SUPPORT_SYMBOLS adds an explicit /Zi after CMake's /Z7, which would win
# (cl warning D9025). Keep it off and link the way it does instead, so the
# exe still gets a PDB for support stack traces and debuggers.
set(JFG_SUPPORT_SYMBOLS OFF CACHE BOOL "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO /DEBUG" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO /DEBUG" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO /DEBUG" CACHE STRING "" FORCE)
