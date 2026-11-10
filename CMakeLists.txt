cmake_minimum_required(VERSION 3.16)
project(pzmodmgr VERSION 0.2.7 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# GCC/Clang release builds (Linux and MinGW) use size optimization and section
# GC. GCC's -O3 inlining is a major part of this app's binary size; -Os plus
# dead-section removal cuts both executables substantially. MinGW benefits most
# because it links libstdc++ statically. Disable this option to build a baseline
# with the toolchain's normal Release flags for benchmarking. LTO is enabled
# separately below; after the exception/unwinder runtime was removed it now
# gives a further measurable reduction rather than growing the image.
option(PZMODMGR_SIZE_OPT "Optimize GCC/Clang release builds for size" ON)
if(PZMODMGR_SIZE_OPT AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
  # Use generator expressions and add_*_options instead of assigning the
  # CMAKE_*_FLAGS_RELEASE cache entries: a plain set() here does not override
  # the toolchain's cached "-O3 -DNDEBUG".
  add_compile_options(
      $<$<CONFIG:Release>:-Os>
      $<$<CONFIG:Release>:-ffunction-sections>
      $<$<CONFIG:Release>:-fdata-sections>
      $<$<CONFIG:Release>:-fvisibility=hidden>
      $<$<CONFIG:Release>:-fno-unwind-tables>
      $<$<CONFIG:Release>:-fno-asynchronous-unwind-tables>
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:-fno-rtti>
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions>)
  add_link_options($<$<CONFIG:Release>:-Wl,--gc-sections>)
endif()

# Linux-only size tuning. -fno-plt calls imported functions through the GOT
# instead of per-symbol PLT stubs; -z noseparate-code merges code and read-only
# data into one load segment (still read+execute, never writable); build-id and
# ident notes add metadata but no runtime value. Measured together: 8,480 bytes
# off each stripped release binary.
if(PZMODMGR_SIZE_OPT AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
      $<$<CONFIG:Release>:-fno-plt>
      $<$<CONFIG:Release>:-fno-ident>)
  add_link_options(
      $<$<CONFIG:Release>:-Wl,-z,noseparate-code>
      $<$<CONFIG:Release>:-Wl,--build-id=none>)
endif()

# Keep the shipped Linux binaries independent of the system C++ runtime. The
# source-level C99 conversion is in progress; this removes the external
# libstdc++/libgcc_s dependency for builds in the meantime.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
endif()

if(WIN32)
  set(PZMODMGR_FS_SOURCE src/core/fs_win.cpp)
else()
  set(PZMODMGR_FS_SOURCE src/core/fs_posix.cpp)
endif()

add_library(pzcore STATIC
  src/core/runtime_min.cpp
  src/core/error_code.cpp
  src/core/paths.cpp
  ${PZMODMGR_FS_SOURCE}
  src/core/activation.cpp
  src/core/url.c
  src/core/steamcmd.cpp
  src/core/mod_info.cpp
  src/core/mod_manager.cpp
  src/core/updater.cpp
  src/core/commands.cpp)
target_include_directories(pzcore PUBLIC src)
target_link_libraries(pzcore PUBLIC Threads::Threads)
target_compile_definitions(pzcore PUBLIC PZMODMGR_VERSION="${PROJECT_VERSION}")

add_executable(pzmodmgr src/main.cpp src/ui/mod_icon.cpp)
target_include_directories(pzmodmgr PRIVATE third_party)
target_link_libraries(pzmodmgr PRIVATE pzcore Threads::Threads)

add_executable(pzmodmgr_native_host src/native/native_host.c src/core/url.c)
target_include_directories(pzmodmgr_native_host PRIVATE src)

install(TARGETS pzmodmgr pzmodmgr_native_host RUNTIME DESTINATION .)
install(FILES README.md LICENSE DESTINATION .)

enable_testing()

if(WIN32)
  set(PZMODMGR_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/win32/pzmodmgr.ico")
  set(PZMODMGR_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/win32/pzmodmgr.manifest")
  configure_file(src/ui/win32/pzmodmgr.rc.in
                 "${CMAKE_CURRENT_BINARY_DIR}/pzmodmgr.rc" @ONLY)
  target_sources(pzmodmgr PRIVATE
    src/ui/win32/win32_ui.cpp
    src/ui/win32/win32_entry.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/pzmodmgr.rc")
  # CommandLineToArgvW, used by the wide entry point.
  target_link_libraries(pzmodmgr PRIVATE shell32)
  target_compile_definitions(pzmodmgr PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601)
  target_compile_definitions(pzmodmgr_native_host PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
  # pzmodmgr is a GUI-subsystem program, so starting it from Explorer or a
  # shortcut opens the manager window alone instead of the manager window plus
  # an empty console. Its entry point is therefore the GUI one provided by
  # src/ui/win32/win32_entry.cpp, which re-attaches a console when the CLI is
  # used. pzmodmgr_native_host must stay console-subsystem: it speaks stdio
  # with the browser.
  set_target_properties(pzmodmgr PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MINGW)
    # MinGW needs -municode to make the CRT call the wide wWinMain.
    target_link_options(pzmodmgr PRIVATE -municode)
    # The distribution must not depend on MinGW runtime DLLs.
    foreach(target pzmodmgr pzmodmgr_native_host)
      target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
  endif()
  if(MSVC)
    # pzmodmgr.rc already embeds pzmodmgr.manifest as resource 1; the MSVC
    # linker's own manifest would be a duplicate.
    target_link_options(pzmodmgr PRIVATE /MANIFEST:NO)
  endif()
else()
  find_package(X11 REQUIRED)
  target_sources(pzmodmgr PRIVATE src/ui/x11/x11_ui.cpp)
  target_include_directories(pzmodmgr PRIVATE ${X11_INCLUDE_DIR})
  # Link only Xlib itself. The legacy X11_LIBRARIES variable also lists SM,
  # ICE and Xext, none of which this UI uses; leaving them as direct NEEDED
  # entries made the dynamic loader map three unnecessary libraries.
  target_link_libraries(pzmodmgr PRIVATE X11::X11)
  add_link_options(-Wl,--as-needed)
endif()

# Link-time optimization. Keep it opt-out for reproducible size comparisons.
# MinGW GCC 15.2 ICEs in binds_to_current_def_p() with the default parallel
# partitioning; the single-partition link below avoids the compiler bug, so
# LTO is enabled by default on MinGW too.
set(PZMODMGR_LTO_DEFAULT ON)
option(PZMODMGR_LTO "Use LTO for release builds" ${PZMODMGR_LTO_DEFAULT})
if(PZMODMGR_LTO AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT PZMODMGR_IPO_SUPPORTED OUTPUT PZMODMGR_IPO_ERROR
                      LANGUAGES C CXX)
  if(PZMODMGR_IPO_SUPPORTED)
    set(PZMODMGR_LTO_TARGETS pzcore pzmodmgr pzmodmgr_native_host)
    if(TARGET pzcore_tests)
      list(APPEND PZMODMGR_LTO_TARGETS pzcore_tests)
    endif()
    foreach(target IN LISTS PZMODMGR_LTO_TARGETS)
      set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
      if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # GCC 15.2's MinGW LTO ICEs in binds_to_current_def_p() when the
        # default parallel partitioning is used. One partition avoids the
        # compiler bug and still gives the size win.
        target_link_options(${target} PRIVATE -flto=1 -flto-partition=one)
      endif()
    endforeach()
  else()
    message(STATUS "LTO unavailable: ${PZMODMGR_IPO_ERROR}")
  endif()
endif()

# Core tests; the test copies the fake steamcmd to a temp dir at runtime.
if(UNIX)
  add_executable(pzcore_tests tests/test_core.cpp)
  target_link_libraries(pzcore_tests PRIVATE pzcore)
  target_compile_definitions(pzcore_tests PRIVATE
                             FAKE_STEAMCMD_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_steamcmd.sh"
                             PZMODMGR_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME pzcore_tests COMMAND pzcore_tests)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME native_host_test COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_host.py
             $<TARGET_FILE:pzmodmgr_native_host>
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_steamcmd.sh)
  endif()
endif()
