cmake_minimum_required(VERSION 3.16)
project(pzmodmgr VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(pzcore STATIC
  src/core/paths.cpp
  src/core/url.cpp
  src/core/steamcmd.cpp
  src/core/mod_info.cpp
  src/core/mod_manager.cpp
  src/core/commands.cpp)
target_include_directories(pzcore PUBLIC src)

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
    "${CMAKE_CURRENT_BINARY_DIR}/pzmodmgr.rc")
  target_compile_definitions(pzmodmgr PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601)
  target_compile_definitions(pzmodmgr_native_host PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
  if(MINGW)
    # The distribution must not depend on MinGW runtime DLLs.
    foreach(target pzmodmgr pzmodmgr_native_host)
      target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
  endif()
  # Add -mwindows (MinGW) or /SUBSYSTEM:WINDOWS (MSVC) to hide the console later.
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
