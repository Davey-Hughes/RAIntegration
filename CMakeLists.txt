cmake_minimum_required(VERSION 3.20)
project(RAIntegrationPortable CXX C)

# Spike build: compile only the platform-independent parts of RAIntegration with
# a non-MSVC toolchain, to measure how much work a native Linux port would be.
# Excluded on purpose: src/ui/win32 (Win32 views), src/ui/drawing/gdi (GDI
# rendering), and the Windows* service implementations.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fail a single translation unit after 180s instead of letting it spin (a C file
# force-including a C++ header under -ferror-limit=0 can loop in error recovery).
find_program(TIMEOUT_EXE timeout)
if(TIMEOUT_EXE)
  set(CMAKE_C_COMPILER_LAUNCHER ${TIMEOUT_EXE} 180)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${TIMEOUT_EXE} 180)
endif()

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
#
# RC_CLIENT_SUPPORTS_HASH mirrors src/base.props, which sets it for every MSVC
# translation unit. Without it rc_client_internal.h skips rhash/rc_hash_internal.h
# (and with it rc_consoles.h), so RC_CONSOLE_* and rc_console_name go undeclared,
# and rc_client.h hides rc_client_begin_identify_and_load_game /
# rc_client_begin_identify_and_change_media behind the same guard.
target_compile_definitions(ra_portable PRIVATE RA_EXPORTS=1 RC_CLIENT_SUPPORTS_HASH)

target_compile_options(ra_portable PRIVATE
  -include msvc_compat.h
  -fdeclspec
  -ferror-limit=0
  -Wno-unknown-pragmas
  -Wno-pragma-pack
)
