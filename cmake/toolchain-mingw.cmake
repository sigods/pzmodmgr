set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

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
