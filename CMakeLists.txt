cmake_minimum_required(VERSION 3.16)
project(pzmodmgr VERSION 0.2.3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# GCC/Clang release builds (Linux and MinGW) use size optimization and section
# GC. GCC's -O3 inlining is a major part of this app's binary size; -Os plus
# dead-section removal cuts both executables substantially. MinGW benefits most
# because it links libstdc++ statically. Disable this option to build a baseline
# with the toolchain's normal Release flags for benchmarking. LTO was measured
# as larger than this setup on Linux, so it is intentionally not used.
option(PZMODMGR_SIZE_OPT "Optimize GCC/Clang release builds for size" ON)
if(PZMODMGR_SIZE_OPT AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
  set(CMAKE_CXX_FLAGS_RELEASE
      "-Os -DNDEBUG -ffunction-sections -fdata-sections -fno-rtti")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections")
endif()

add_library(pzcore STATIC
  src/core/paths.cpp
  src/core/activation.cpp
  src/core/url.cpp
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

add_executable(pzmodmgr_native_host src/native/native_host.cpp)
target_link_libraries(pzmodmgr_native_host PRIVATE pzcore)

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
  target_link_libraries(pzmodmgr PRIVATE ${X11_LIBRARIES})
endif()

# Core tests; the test copies the fake steamcmd to a temp dir at runtime.
if(UNIX)
  add_executable(pzcore_tests tests/test_core.cpp)
  target_link_libraries(pzcore_tests PRIVATE pzcore)
  target_compile_definitions(pzcore_tests PRIVATE
                             FAKE_STEAMCMD_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_steamcmd.sh")
  add_test(NAME pzcore_tests COMMAND pzcore_tests)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME native_host_test COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_host.py
             $<TARGET_FILE:pzmodmgr_native_host>
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_steamcmd.sh)
  endif()
endif()
