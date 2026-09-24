cmake_minimum_required(VERSION 3.20)
project(RAIntegrationPortable CXX C)

# Spike build: compile only the platform-independent parts of RAIntegration with
# a non-MSVC toolchain, to measure how much work a native Linux port would be.
# Excluded on purpose: src/ui/win32 (Win32 views), src/ui/drawing/gdi (GDI
# rendering), the Windows* service implementations, and the Win32 half of the
# platform service factory.

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

# Two of the eight top-level src/*.cpp files carry no Win32 dependency:
# RA_Defs.cpp holds ra::Parse* and RA_md5factory.cpp holds RAGenerateMD5, both
# of which the test suite calls directly.
list(APPEND PORTABLE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/RA_Defs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/RA_md5factory.cpp
)

# rc_snprintf is a product need, not a test-only one: every C++ translation
# unit that includes rcheevos/src/rc_compat.h takes its clang-only C89 branch
# (see the comment in rc_compat_shim.cpp) and calls the renamed symbol, so
# ra_portable's own objects (e.g. AchievementRuntime.cpp) are left with an
# undefined reference unless this compiles into ra_portable itself.
list(APPEND PORTABLE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/rc_compat_shim.cpp
)

list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "/ui/win32/")
list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "/ui/drawing/gdi/")
list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "/services/impl/Windows")
# PlatformServices_Win32.cpp names the Windows*, win32 and gdi types, but its
# filename does not start with "Windows", so the filter above does not reach it.
list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "PlatformServices_Win32")

find_package(CURL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Gui Multimedia)

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

# Not ${CURL_INCLUDE_DIRS}: FindCURL tries config mode first, and where a
# CURLConfig.cmake exists (Debian, Ubuntu, Fedora, vcpkg) it returns having set
# only CURL_VERSION_STRING, leaving CURL_INCLUDE_DIRS empty. The imported target
# carries the include directories in both modes. Linking a library into an
# OBJECT library for its usage requirements is legal from CMake 3.12.
target_link_libraries(ra_portable PRIVATE CURL::libcurl)
target_link_libraries(ra_portable PUBLIC Qt6::Gui Qt6::Multimedia)

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

# The rcheevos C library, with the same file set and defines as
# src/rcheevos.vcxproj. Nothing linked before now, so it had never been built.
add_library(rcheevos STATIC
  rcheevos/src/rapi/rc_api_common.c
  rcheevos/src/rapi/rc_api_editor.c
  rcheevos/src/rapi/rc_api_info.c
  rcheevos/src/rapi/rc_api_runtime.c
  rcheevos/src/rapi/rc_api_user.c
  rcheevos/src/rcheevos/alloc.c
  rcheevos/src/rcheevos/condition.c
  rcheevos/src/rcheevos/condset.c
  rcheevos/src/rcheevos/consoleinfo.c
  rcheevos/src/rcheevos/format.c
  rcheevos/src/rcheevos/lboard.c
  rcheevos/src/rcheevos/memref.c
  rcheevos/src/rcheevos/operand.c
  rcheevos/src/rcheevos/rc_validate.c
  rcheevos/src/rcheevos/richpresence.c
  rcheevos/src/rcheevos/runtime.c
  rcheevos/src/rcheevos/runtime_progress.c
  rcheevos/src/rcheevos/trigger.c
  rcheevos/src/rcheevos/value.c
  rcheevos/src/rc_client.c
  rcheevos/src/rc_compat.c
  rcheevos/src/rc_util.c
  rcheevos/src/rhash/aes.c
  rcheevos/src/rhash/cdreader.c
  rcheevos/src/rhash/hash.c
  rcheevos/src/rhash/hash_disc.c
  rcheevos/src/rhash/hash_encrypted.c
  rcheevos/src/rhash/hash_rom.c
  rcheevos/src/rhash/hash_zip.c
  rcheevos/src/rhash/md5.c
)

target_include_directories(rcheevos PUBLIC rcheevos/include PRIVATE rcheevos/src)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH)

enable_testing()

file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
list(FILTER TEST_SOURCES EXCLUDE REGEX "/compat/")

# Four test sources that no MSVC project builds either, and that cannot be
# built at all as the tree stands:
#
#   RA_Achievement_Tests.cpp    #include "RA_Achievement.h"
#   RA_MemValue_Tests.cpp       #include "RA_MemValue.h"
#   RcheevosClient_Tests.cpp    #include "services/RcheevosClient.hh"
#
# all three subjects were deleted by 0cd8bead ("use rc_client for runtime
# (#1029)") and the tests were left behind. They are in no .vcxproj.
#
# RcheevosClient_Tests.cpp also declared TEST_CLASS(AchievementRuntime_Tests),
# colliding with the real one in the same namespace; it has been renamed to
# match its file, so reviving the file does not silently replace five of
# AchievementRuntime_Tests' methods with its own.
#
#   RA_Interface_Tests.cpp      belongs to tests/RA_Interface.Tests.vcxproj, a
#                               separate project that compiles the emulator-side
#                               RAInterface/RA_Interface.cpp; that needs
#                               winhttp.h and is not part of this target.
#
# Between them they account for 68 of the 1743 TEST_METHODs in tests/*.cpp.
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/RA_Achievement_Tests\\.cpp$")
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/RA_MemValue_Tests\\.cpp$")
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/RA_Interface_Tests\\.cpp$")
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/services/RcheevosClient_Tests\\.cpp$")

# The test binary compiles the portable sources itself rather than linking
# ra_portable's objects, for two reasons:
#
#  * RA_UTEST changes the code under test. It adds a data member (m_bDisposed)
#    to ModelCollectionBase, and switches off whole blocks of Exports.cpp,
#    GameContext.cpp and EmulatorContext.cpp. Linking objects built without it
#    into a binary built with it is a class-layout mismatch, not just a
#    behaviour difference. The MSVC test project does the same thing: it lists
#    67 of the src .cpp files in its own ClCompile set.
#
#  * src/services/Initialization.cpp is the composition root. No .vcxproj under
#    tests/ compiles it either, so the test binary leaves it out to match;
#    ra_portable keeps it, which is what makes the measurement cover it.
#    Nothing had to be stubbed for it - every reference to
#    Initialization::RegisterServices is itself inside an #ifndef RA_UTEST.
set(PORTABLE_TEST_SOURCES ${PORTABLE_SOURCES})
list(FILTER PORTABLE_TEST_SOURCES EXCLUDE REGEX "/services/Initialization\\.cpp$")

add_executable(ra_tests
  ${TEST_SOURCES}
  ${PORTABLE_TEST_SOURCES}
  # The _RA_* entry points and OnStateRestored(), which the suite calls
  # directly and services/AchievementRuntimeExports.cpp references.
  # tests/RA_Integration.Tests.vcxproj lists it for the same reason.
  #
  # Left out of PORTABLE_SOURCES on purpose, so that count keeps meaning
  # "translation units with no platform branch in them". Not because every
  # branch in here is Win32: of the eleven RA_UTEST-bearing guards, seven say
  # !defined(RA_UTEST) && defined(_WIN32) and are genuinely Win32 - the GDI
  # surface, the win32 Desktop and OverlayWindow, ControlBinding's repaints -
  # but four say plain #ifndef RA_UTEST and select portable code that a
  # non-test Linux build compiles and runs: service registration in
  # InitCommon, and UpdateUIForFrameChange with its two callers. The
  # ra_linux_smoke target below is what compiles this file without RA_UTEST
  # for Linux, and what exercises those four.
  src/Exports.cpp
  tests/compat/Runner.cpp
)

target_include_directories(ra_tests PRIVATE
  . compat src src/devkit RAInterface
  rapidjson/include GSL/include rcheevos/include rcheevos/src
  tests/compat tests/devkit
)

target_compile_definitions(ra_tests PRIVATE RA_EXPORTS=1 RC_CLIENT_SUPPORTS_HASH RA_UTEST)

target_link_libraries(ra_tests PRIVATE rcheevos CURL::libcurl)
target_link_libraries(ra_tests PRIVATE Qt6::Gui Qt6::Multimedia)

# src/pch.h is force-included by the MSVC test project and, under RA_UTEST,
# force-includes CppUnitTest.h with it. Several test sources rely on that and
# never include the framework themselves, so mirror it here.
# SHELL: keeps CMake from de-duplicating the second -include out of the list.
target_compile_options(ra_tests PRIVATE
  "SHELL:-include msvc_compat.h" "SHELL:-include CppUnitTest.h"
  -fdeclspec -Wno-unknown-pragmas -Wno-pragma-pack)

# One CTest entry per TEST_CLASS, resolved at configure time so a crash in one
# class cannot take the rest of the suite with it.
foreach(TEST_FILE ${TEST_SOURCES})
  file(STRINGS ${TEST_FILE} TEST_CLASS_LINES REGEX "^TEST_CLASS\\(")
  foreach(TEST_CLASS_LINE ${TEST_CLASS_LINES})
    string(REGEX REPLACE "^TEST_CLASS\\(([A-Za-z0-9_]+)\\).*$" "\\1" TEST_CLASS_NAME "${TEST_CLASS_LINE}")
    add_test(NAME ${TEST_CLASS_NAME} COMMAND ra_tests --filter=${TEST_CLASS_NAME})
  endforeach()
endforeach()

# The end-to-end harness. Unlike ra_tests it registers the real services and
# needs a live QGuiApplication, so it is a separate executable rather than
# another test class.
#
# It links ra_portable's objects, which is not enough on its own:
#
#  * src/Exports.cpp defines the _RA_* entry points and OnStateRestored(),
#    which AchievementRuntime.cpp, AchievementRuntimeExports.cpp and
#    EmulatorContext.cpp all reference. It is not in PORTABLE_SOURCES because
#    it could not be compiled without RA_UTEST until its Win32 blocks named
#    _WIN32; it still is not, so that the measured ratio keeps meaning the
#    same thing, and it is listed here instead. Compiling it WITH RA_UTEST is
#    not an option - that flag adds a data member to ModelCollectionBase, so
#    linking it against objects built without it is a layout mismatch.
#
#  * src/RA_Core.cpp defines _RA_Shutdown, _RA_ConfirmLoadNewRom and
#    _RA_OnReset. Its DllMain, HMODULE/HWND globals and GSL contract handler
#    sit behind #ifdef _WIN32 and the rest is portable. Like Exports.cpp it is
#    listed here rather than in PORTABLE_SOURCES so that the measured count
#    stays comparable with earlier runs, and it is not in ra_tests because no
#    MSVC test project compiles it either.
#
#  * rcheevos. ra_portable is an OBJECT library and never linked it; every
#    rc_client_* call in those objects is unresolved until this pulls it in.
#
# services/Initialization.cpp (ServiceLocator::IsInitialized /
# IsShuttingDown, and RegisterServices itself) is already in PORTABLE_SOURCES
# and comes in with the objects.
add_executable(ra_linux_smoke tools/ra_linux_smoke.cpp src/Exports.cpp src/RA_Core.cpp
  $<TARGET_OBJECTS:ra_portable>)

target_include_directories(ra_linux_smoke PRIVATE
  . compat src src/devkit RAInterface
  rapidjson/include GSL/include rcheevos/include rcheevos/src)

target_compile_definitions(ra_linux_smoke PRIVATE RA_EXPORTS=1 RC_CLIENT_SUPPORTS_HASH)
target_compile_options(ra_linux_smoke PRIVATE -include msvc_compat.h -fdeclspec
  -Wno-unknown-pragmas -Wno-pragma-pack)
target_link_libraries(ra_linux_smoke PRIVATE rcheevos CURL::libcurl Qt6::Gui Qt6::Multimedia)
