set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The UI uses std::thread, which requires MinGW's POSIX thread model. Ubuntu's
# default x86_64-w64-mingw32-g++ is the win32 flavor, so prefer the explicit
# -posix compiler when the distribution provides both.
find_program(PZ_MINGW_CXX NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++)
find_program(PZ_MINGW_CC NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc)
find_program(PZ_MINGW_RC NAMES x86_64-w64-mingw32-windres)
if(NOT PZ_MINGW_CXX OR NOT PZ_MINGW_CC OR NOT PZ_MINGW_RC)
  message(FATAL_ERROR "MinGW-w64 cross compiler not found")
endif()

set(CMAKE_C_COMPILER "${PZ_MINGW_CC}")
set(CMAKE_CXX_COMPILER "${PZ_MINGW_CXX}")
set(CMAKE_RC_COMPILER "${PZ_MINGW_RC}")

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# Keep the portable Windows build small. LTO triggers a GCC 15 MinGW ICE in
# this tree, so use section GC and size optimization instead.
set(CMAKE_CXX_FLAGS_RELEASE "-Os -ffunction-sections -fdata-sections -fno-rtti"
    CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections" CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
