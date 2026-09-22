cmake_minimum_required(VERSION 3.20)
project(RAIntegrationPortable CXX C)

# Spike build: compile only the platform-independent parts of RAIntegration with
# a non-MSVC toolchain, to measure how much work a native Linux port would be.
# Excluded on purpose: src/ui/win32 (Win32 views), src/ui/drawing/gdi (GDI
# rendering), and the Windows* service implementations.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE PORTABLE_SOURCES
  src/api/*.cpp
  src/data/*.cpp
  src/services/*.cpp
  src/ui/*.cpp
  src/util/*.cpp
  src/devkit/*.cpp
)

list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "/ui/win32/")
list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "/ui/drawing/gdi/")
list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "/services/impl/Windows")

add_library(ra_portable OBJECT ${PORTABLE_SOURCES})

target_include_directories(ra_portable PRIVATE
  .
  compat
  src
  src/devkit
  RAInterface
  rapidjson/include
  GSL/include
  rcheevos/include
  rcheevos/src
)

# RA_EXPORTS is what the DLL build defines; in RA_Defs.h it also selects the
# branch that does not pull in <Windows.h>.
target_compile_definitions(ra_portable PRIVATE RA_EXPORTS=1)

target_compile_options(ra_portable PRIVATE
  -include msvc_compat.h
  -fdeclspec
  -ferror-limit=0
  -Wno-unknown-pragmas
  -Wno-pragma-pack
)
