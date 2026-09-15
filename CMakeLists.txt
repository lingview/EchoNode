cmake_minimum_required(VERSION 3.20)
project(EchoNode)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型 (Debug/Release/RelWithDebInfo)" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -s")
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-flto=auto)
    add_link_options(-flto=auto)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_link_options(-fuse-linker-plugin)
    endif()
endif()

message(STATUS "EchoNode build type: ${CMAKE_BUILD_TYPE}")

set(ECHONODE_THIRD_PARTY ${CMAKE_SOURCE_DIR}/third_party)

if(WIN32)
    set(PLATFORM_SOURCES
            ${CMAKE_SOURCE_DIR}/client/platform/windows/SystemInfoWin.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/windows/ProcessOpsWin.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/windows/ShellSpawnWin.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/windows/ScreenCaptureWin.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/windows/InputInjectorWin.cpp)
    set(PLATFORM_LIBS user32 gdi32 d3d11 dxgi winmm)
elseif(UNIX)
    set(PLATFORM_SOURCES
            ${CMAKE_SOURCE_DIR}/client/platform/linux/SystemInfoLinux.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/linux/ProcessOpsLinux.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/linux/ShellSpawnLinux.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/linux/ScreenCaptureLinux.cpp
            ${CMAKE_SOURCE_DIR}/client/platform/linux/InputInjectorLinux.cpp)
    set(PLATFORM_LIBS X11 Xtst util)
endif()

add_subdirectory(common)
add_subdirectory(client)
add_subdirectory(server)
