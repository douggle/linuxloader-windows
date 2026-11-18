cmake_minimum_required(VERSION 3.10)
project(linuxloader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR}/src/log)
include_directories(${PROJECT_SOURCE_DIR}/src/elfio)
include_directories(${PROJECT_SOURCE_DIR}/src/glad/include)
include_directories(${PROJECT_SOURCE_DIR}/src/minhook/include)
include_directories(${PROJECT_SOURCE_DIR}/src/SDL3)

# ------------------------------------------------------------------------------
# Common Static Libraries (Built for whichever platform CMake is targeting)
# ------------------------------------------------------------------------------

# 1. MinHook (Static)
file(GLOB_RECURSE MINHOOK_SRC src/minhook/src/*.c)
set(MINHOOK_C_FLAGS -fPIC -m32)
add_library(minhook STATIC ${MINHOOK_SRC})
target_include_directories(minhook PUBLIC src/minhook/include)
target_compile_options(minhook PRIVATE ${MINHOOK_C_FLAGS})

# 2. LibXdiff (Static)
set(LIBXDIFF_C_FLAGS -DHAVE_CONFIG_H -fPIC -m32)
file(GLOB_RECURSE XDIFF_SRC src/libxdiff/xdiff/*.c)
add_library(xdiff STATIC ${XDIFF_SRC})
target_include_directories(xdiff PUBLIC src/libxdiff/xdiff)
target_compile_options(xdiff PRIVATE ${LIBXDIFF_C_FLAGS})

# ------------------------------------------------------------------------------
# Platform-Specific Projects
# ------------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND NOT MINGW AND NOT CMAKE_C_COMPILER MATCHES "mingw")
    message(STATUS "Configuring Project 1: Linux Build")

    # 32-bit SDL3_ttf and SDL3_image are not packaged everywhere (Arch Linux
    # has lib32-sdl3 but no lib32-sdl3_ttf or lib32-sdl3_image). The repository
    # carries 32-bit builds of both; they are linked by path, and shipped in
    # ll-deps for the loader to run against (see the linuxloader target).
    set(LL_SDL3_TTF_LIB ${PROJECT_SOURCE_DIR}/libs/linux_x86/libSDL3_ttf.so.0)
    set(LL_SDL3_IMAGE_LIB ${PROJECT_SOURCE_DIR}/libs/linux_x86/libSDL3_image.so.0)

    # The linuxloader executable
    set(LINUXLOADER_EXE_C_FLAGS -m32 -DCOMPILING_LINUXLOADER_ELF)
    add_executable(linuxloader_exe
        src/loader/linuxExe.c
        src/loader/loader64/game64.c
        src/loader/mainShared.c
        src/loader/config/iniParser.c
        src/loader/input/sdlInput.c
        src/loader/input/controlIniGen.c
        src/loader/input/controlMapper.c
        src/loader/log/log.c
        src/loader/hardware/lindbergh/output.c
        src/loader/hardware/lindbergh/jvs.c
        src/loader/config/config.c
        src/loader/input/evdevInput.c
        src/loader/config/gameData.c
        src/loader/config/configIni.c
        src/loader/config/squashfs.c
        src/loader/rawthrills/rtGames.c
        src/loader/rawthrills/rtLaunch.c
        src/loader/namco/namcoEs1Games.c
        src/loader/namco/namcoEs1Launch.c
        src/loader/namco/namcoN2Games.c
        src/loader/namco/namcoN2Launch.c
        src/loader/teamplay/teamplayGames.c
        src/loader/globalvr/gvrGames.c
        src/loader/roxor/roxorGames.c
        src/loader/hostLinker.c)

    set_target_properties(linuxloader_exe PROPERTIES LINK_FLAGS "-m32")
    target_link_libraries(linuxloader_exe PRIVATE m SDL3)
    set_target_properties(linuxloader_exe PROPERTIES OUTPUT_NAME "linuxloader")
    target_compile_options(linuxloader_exe PRIVATE ${LINUXLOADER_EXE_C_FLAGS})

    # Common options for shared libraries
    set(LIBS_C_FLAGS -m32 -fPIC -shared)
    set(SHARED_LINKER_FLAGS -m32 -Wl,-z,defs -rdynamic)

    # libkswapapi.so
    add_library(kswapapi SHARED src/loader/extraLibs/lindbergh/libkswapapi/libkswapapi.c)
    target_link_libraries(kswapapi ${SHARED_LINKER_FLAGS})
    target_compile_options(kswapapi PRIVATE ${LIBS_C_FLAGS})

    # libposixtime.so
    add_library(posixtime SHARED src/loader/extraLibs/lindbergh/libposixtime/libposixtime.c)
    target_link_libraries(posixtime ${SHARED_LINKER_FLAGS})
    target_compile_options(posixtime PRIVATE ${LIBS_C_FLAGS})
    add_custom_command(TARGET posixtime POST_BUILD COMMAND ln -s -f libposixtime.so ${CMAKE_BINARY_DIR}/libposixtime.so.1 COMMAND ln -s -f libposixtime.so ${CMAKE_BINARY_DIR}/libposixtime.so.2.4)

    # libGLEW.so.1.3 (Global VR games)
    add_library(glew_gvr SHARED src/loader/extraLibs/globalvr/libglew/libglew.c)
    set_target_properties(glew_gvr PROPERTIES OUTPUT_NAME "GLEW")
    target_link_libraries(glew_gvr ${SHARED_LINKER_FLAGS} dl)
    target_compile_options(glew_gvr PRIVATE ${LIBS_C_FLAGS})
    add_custom_command(TARGET glew_gvr POST_BUILD COMMAND ln -s -f libGLEW.so ${CMAKE_BINARY_DIR}/libGLEW.so.1.3)

    # libarcaderegistry.so.1 (Namco ES1: Gundam Senjo no Kizuna)
    add_library(arcaderegistry SHARED src/loader/extraLibs/namco/libarcaderegistry/libarcaderegistry.c)
    target_link_libraries(arcaderegistry ${SHARED_LINKER_FLAGS} pthread)
    target_compile_options(arcaderegistry PRIVATE ${LIBS_C_FLAGS})
    add_custom_command(TARGET arcaderegistry POST_BUILD COMMAND ln -s -f libarcaderegistry.so ${CMAKE_BINARY_DIR}/libarcaderegistry.so.1)

    # libsegaapi.so
    add_library(segaapi SHARED src/loader/audio/lindbergh/libsegaapi/libsegaapi.c)
    target_include_directories(segaapi PRIVATE ${PROJECT_SOURCE_DIR}/src/FAudio)
    set(LIBSEGAAPI_C_FLAGS -m32 -fPIC -shared)
    set(LIBSEGAAPI_LINKER_FLAGS -O0 -g -m32 -fPIC -shared)
    target_link_libraries(segaapi ${LIBSEGAAPI_LINKER_FLAGS})
    target_link_libraries(segaapi ${PROJECT_SOURCE_DIR}/libs/linux_x86/libFAudio.a)
    target_compile_options(segaapi PRIVATE ${LIBSEGAAPI_C_FLAGS})

    # linuxloader.so
    set(LINUX_LOADER_SO_LINKER_FLAGS -m32 -Wl,-z,defs -rdynamic -Wl,-rpath,$ORIGIN/ll-deps -Wl,-rpath,$ORIGIN -static-libstdc++ -static-libgcc -lc -ldl -lGL -lglut -lX11 -lXcursor -lSDL3 -ludev -lm -lpthread -shared -lstdc++ -nostdlib)
    set(LINUX_LOADER_SO_C_FLAGS -g -fPIC -m32 -Wall -Wno-unused-variable -Wno-unused-function -D_GNU_SOURCE -Wno-format-truncation -Wno-strict-aliasing -Wno-discarded-qualifiers -Wno-stringop-truncation -Wno-format-overflow)
    file(GLOB LINUX_LOADER_OBJS
        src/loader/init.c
        src/loader/mainL.c
        src/loader/mainShared.c
        src/loader/hostLinker.c
        src/loader/config/*.c
        src/loader/graphics/*.c
        src/loader/hardware/lindbergh/*.c
        src/loader/input/*.c
        src/loader/log/*.c
        src/loader/patching/*.c
        src/loader/redirections/*.c
        src/loader/common/*.c
        src/loader/rawthrills/*.c
        src/loader/namco/*.c
        src/loader/teamplay/*.c
        src/loader/globalvr/*.c
        src/loader/roxor/*.c
        src/loader/graphics/shaderWork/*.c
        src/glad/src/gl.c)

    add_library(linuxloader SHARED ${LINUX_LOADER_OBJS})
    # The runpath (above) finds ll-deps wherever the loader is copied to, and
    # is the whole of it: leave CMake's own rpath out.
    set_target_properties(linuxloader PROPERTIES PREFIX "" SKIP_BUILD_RPATH TRUE)
    # gcc, last: the 64-bit divisions of the Namco ES1 sources need libgcc's
    # helpers, which -nostdlib leaves out of the link.
    target_link_libraries(linuxloader ${LINUX_LOADER_SO_LINKER_FLAGS} xdiff minhook dl gcc
        ${LL_SDL3_TTF_LIB} ${LL_SDL3_IMAGE_LIB})
    # The 32-bit SDL3_ttf and SDL3_image the loader is linked against, shipped
    # beside it: Arch Linux packages neither (lib32-sdl3 has SDL3 alone).
    add_custom_command(TARGET linuxloader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/ll-deps
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/libs/linux_x86/libSDL3_ttf.so.0
            ${PROJECT_SOURCE_DIR}/libs/linux_x86/libSDL3_image.so.0
            ${CMAKE_BINARY_DIR}/ll-deps/)
    target_compile_options(linuxloader PRIVATE ${LINUX_LOADER_SO_C_FLAGS})

    # The 64-bit games (the Raw Thrills g7 dumps, the Unity games), which
    # linuxloader hands to linuxloader64: it starts them with
    # linuxloader64.so, their library, preloaded.  The library shares the
    # loader's ini parser and gun border, and reads the same linuxloader.ini.
    # g7_rt.so and halo_rt.so, its names when it was the g7 games' alone,
    # link to it.
    add_executable(linuxloader64
        src/loader/loader64/linuxloader64.c
        src/loader/loader64/game64.c
        src/loader/config/iniParser.c)
    target_compile_options(linuxloader64 PRIVATE -m64 -Wall -Wextra -Wno-format-truncation)
    target_link_options(linuxloader64 PRIVATE -m64)

    file(GLOB LOADER64_SO_SRC
        src/loader/loader64/rt64.c
        src/loader/rawthrills/g7/*.c
        src/loader/rawthrills/nerf/*.c
        src/loader/rawthrills/sb3/*.c)
    add_library(loader64 SHARED
        ${LOADER64_SO_SRC}
        src/loader/config/iniParser.c
        src/loader/input/evdevFfb.c
        src/loader/graphics/border.c
        src/loader/graphics/borderFrame.c
        src/loader/graphics/frameScale.c
        src/glad/src/gl.c)
    set_target_properties(loader64 PROPERTIES PREFIX "" OUTPUT_NAME "linuxloader64")
    target_compile_options(loader64 PRIVATE -m64 -fPIC -Wall -Wextra)
    target_link_options(loader64 PRIVATE -m64)
    target_link_libraries(loader64 PRIVATE dl pthread m)
    add_custom_command(TARGET loader64 POST_BUILD
        COMMAND ln -s -f linuxloader64.so ${CMAKE_BINARY_DIR}/g7_rt.so
        COMMAND ln -s -f linuxloader64.so ${CMAKE_BINARY_DIR}/halo_rt.so)

elseif(WIN32 OR MINGW OR CMAKE_C_COMPILER MATCHES "mingw")

    enable_language(RC)

    include_directories(${PROJECT_SOURCE_DIR}/src/elfio)
    include_directories(${PROJECT_SOURCE_DIR}/src/SDL3)
    include_directories(${PROJECT_SOURCE_DIR}/src/FAudio)
    # <elf.h>, <dlfcn.h> and <link.h>: MinGW has none, and the arcade support
    # reads the game's images the way they let it.
    include_directories(${PROJECT_SOURCE_DIR}/src/loader/platform/win32)

    # Gather all source files
    file(GLOB SRC_FILES
        src/loader/init.c
        src/loader/mainW.cpp
        src/loader/selfDebug.cpp
        src/loader/mainShared.c
        src/loader/audio/lindbergh/libsegaapi/libsegaapi.c
        src/loader/audio/lindbergh/alsa2sdl/alsa2sdl.c
        src/loader/config/*.c
        src/loader/graphics/*.c
        src/loader/hardware/lindbergh/*.c
        src/loader/input/*.c
        src/loader/log/*.c
        src/loader/patching/*.c
        src/loader/redirections/*.c
        src/loader/graphics/shaderWork/*.c
        src/loader/rawthrills/*.c
        src/loader/namco/*.c
        src/loader/teamplay/*.c
        src/loader/globalvr/*.c
        src/loader/roxor/*.c
        src/loader/common/*.c
        src/loader/platform/win32/win32Posix.c
        src/loader/platform/win32/win32Link.cpp
        src/loader/platform/win32/win32Devices.c
        src/loader/elfLoader/*.cpp
        src/loader/elfLoader/pthread/*.cpp
        src/glad/src/gl.c
        src/icon.rc
    )

    # Sources of the platform-specific layers above: the squashfs dumps are
    # mounted with squashfuse, and the interactive controls mapper and the
    # evdev force feedback talk to the Linux input devices. Only Linux builds
    # them, and only Linux sources call them.
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/config/squashfs\\.c$")
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/input/controlMapper\\.c$")
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/input/evdevFfb\\.c$")
    # The WMMT4 emulation patches the game's x86 code through Linux's /proc and
    # the ELF layout, and its assembly trampolines are written for ELF's symbol
    # spelling, so it belongs with the other Linux-only layers.
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/namco/namcoEs1Wmmt4\\.c$")
    # The evdev controllers themselves: they are the Linux input devices.
    # platform/win32/win32Devices.c answers their entry points with "none",
    # so the games use SDL's input.
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/input/evdevInput\\.c$")
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/input/wiimoteEvdev\\.c$")
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/common/webcam\\.c$")
    # The devices: V4L2 cameras and OSS /dev/dsp. platform/win32/win32Devices.c
    # answers for both, so the games find no camera and no sound device.
    list(FILTER SRC_FILES EXCLUDE REGEX "/loader/common/ossDsp\\.c$")

    # Executable target
    add_executable(linuxloader ${SRC_FILES})

    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _WIN32_WINNT=0x0600
        WINVER=0x0600
    )

    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -fpermissive
        -Wno-unused-parameter
        -Wunused-variable
        -Wno-unused-but-set-variable
        -Wimplicit-fallthrough=0
        -Wstrict-aliasing=0
    )

    # Increase stack size to 4MB to prevent stack overflow on startup
    target_link_options(${PROJECT_NAME} PRIVATE
        -s
        -Wl,--stack,4194304
        -Wl,--large-address-aware
    )

    target_link_directories(${PROJECT_NAME} PRIVATE "libs/win32")

    # Link dependencies
    target_link_directories(${PROJECT_NAME} PRIVATE "libs/win32")
    target_link_libraries(${PROJECT_NAME}
        minhook xdiff -Wl,-Bstatic SDL3 SDL3_ttf SDL3_image FAudio freetype harfbuzz winpthread stdc++ gcc gcc_eh -Wl,-Bdynamic gdi32 winmm ole32 oleaut32 shell32 advapi32 cfgmgr32 uuid ws2_32 psapi usp10 rpcrt4 setupapi version imm32 hid iphlpapi delayimp
        # input/hapticFfb.c: the wheels' force feedback.
        dinput8 dxguid
    )

else()
    message(WARNING "Unknown platform, doing nothing. Please use Linux or MinGW.")
endif()
