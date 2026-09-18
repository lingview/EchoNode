cmake_minimum_required(VERSION 3.20)
project(client)

# 从仓库根构建时使用共享 third_party；独立构建（如 CLion 直接打开本目录）时补齐仓库级依赖
if(NOT ECHONODE_THIRD_PARTY)
    set(ECHONODE_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

    # platform 双平台源与链接库（根 CMakeLists 中亦有同名定义）
    if(WIN32)
        set(PLATFORM_SOURCES
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/SystemInfoWin.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/ProcessOpsWin.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/ShellSpawnWin.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/ScreenCaptureWin.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/InputInjectorWin.cpp)
        # d3d11/dxgi：远程桌面抓屏走 Desktop Duplication（GDI 读回 1440p 需 33ms/帧，是帧率天花板）
        set(PLATFORM_LIBS user32 gdi32 d3d11 dxgi winmm)
    elseif(UNIX)
        set(PLATFORM_SOURCES
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/SystemInfoLinux.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/ProcessOpsLinux.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/ShellSpawnLinux.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/ScreenCaptureLinux.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/InputInjectorLinux.cpp)
        set(PLATFORM_LIBS X11 Xtst util)
    endif()

    # common（协议契约层）：仓库根构建时由 common/CMakeLists.txt提供
    add_library(echonode_common STATIC
            ../common/util/Uuid.cpp
            ../common/util/Sha256.cpp)
    target_include_directories(echonode_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../common)
    target_compile_features(echonode_common PUBLIC cxx_std_20)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(net_deps INTERFACE)
target_include_directories(net_deps INTERFACE
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${PROJECT_SOURCE_DIR}/net)
target_compile_definitions(net_deps INTERFACE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(net_deps INTERFACE _WIN32_WINNT=0x0601)
    target_link_libraries(net_deps INTERFACE ws2_32 mswsock)
else()
    target_link_libraries(net_deps INTERFACE pthread)
endif()

add_executable(client
        main.cpp
        core/Client.cpp
        executor/Dispatcher.cpp
        executor/FileExecutor.cpp
        executor/ProcessExecutor.cpp
        executor/ShellExecutor.cpp
        executor/ScreenshotExecutor.cpp
        executor/RemoteDeskExecutor.cpp
        executor/KeylogExecutor.cpp
        executor/StbImageWrite.cpp
        executor/ShellSessionManager.cpp
        net/WsClient.cpp
        ${PLATFORM_SOURCES}
        executor/KeylogExecutor.hpp)
target_include_directories(client PRIVATE
        ${ECHONODE_THIRD_PARTY}
        ${PROJECT_SOURCE_DIR}/platform
        ${PROJECT_SOURCE_DIR})
set_target_properties(client PROPERTIES OUTPUT_NAME echonode_agent)
target_link_libraries(client PRIVATE echonode_common net_deps ${PLATFORM_LIBS} ${ECHONODE_TLS_LIBS})
if(WIN32)
    target_link_options(client PRIVATE -mwindows)
endif()

add_executable(platform_test
        tests/test_platform.cpp
        ${PLATFORM_SOURCES})
target_include_directories(platform_test PRIVATE
        ${ECHONODE_THIRD_PARTY}
        ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(platform_test PRIVATE echonode_common ${PLATFORM_LIBS})

add_executable(net_test
        tests/test_net.cpp
        net/WsClient.cpp)
target_link_libraries(net_test PRIVATE echonode_common net_deps ${ECHONODE_TLS_LIBS})

add_executable(config_test
        tests/test_config.cpp)
target_include_directories(config_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY})
target_compile_features(config_test PRIVATE cxx_std_20)

add_executable(core_test
        tests/test_core.cpp
        core/Client.cpp
        net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(core_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(core_test PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(core_test PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(core_test PRIVATE echonode_common ws2_32 mswsock ${PLATFORM_LIBS} ${ECHONODE_TLS_LIBS})
else()
    target_link_libraries(core_test PRIVATE echonode_common pthread X11 ${ECHONODE_TLS_LIBS})
endif()

add_executable(executor_test
        tests/test_executor.cpp
        core/Client.cpp
        executor/Dispatcher.cpp
        executor/FileExecutor.cpp
        executor/ProcessExecutor.cpp
        executor/KeylogExecutor.cpp
        executor/ShellExecutor.cpp
        executor/ScreenshotExecutor.cpp
        executor/RemoteDeskExecutor.cpp
        executor/StbImageWrite.cpp
        executor/ShellSessionManager.cpp
        net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(executor_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(executor_test PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(executor_test PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(executor_test PRIVATE echonode_common ws2_32 mswsock ${PLATFORM_LIBS} ${ECHONODE_TLS_LIBS})
else()
    target_link_libraries(executor_test PRIVATE echonode_common pthread X11 ${ECHONODE_TLS_LIBS})
endif()

add_executable(session_test
        tests/test_session.cpp
        executor/ShellSessionManager.cpp
        ${PLATFORM_SOURCES})
target_include_directories(session_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(session_test PRIVATE echonode_common ${PLATFORM_LIBS})

add_executable(ps_test
        tests/test_process.cpp
        core/Client.cpp
        executor/Dispatcher.cpp
        executor/ProcessExecutor.cpp
        net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(ps_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(ps_test PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(ps_test PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(ps_test PRIVATE echonode_common ws2_32 mswsock ${PLATFORM_LIBS} ${ECHONODE_TLS_LIBS})
else()
    target_link_libraries(ps_test PRIVATE echonode_common pthread X11 ${ECHONODE_TLS_LIBS})
endif()

add_executable(file_test
        tests/test_file.cpp
        core/Client.cpp
        executor/Dispatcher.cpp
        executor/FileExecutor.cpp
        executor/ProcessExecutor.cpp
        executor/ShellExecutor.cpp
        executor/KeylogExecutor.cpp
        executor/ScreenshotExecutor.cpp
        executor/RemoteDeskExecutor.cpp
        executor/StbImageWrite.cpp
        executor/ShellSessionManager.cpp
        net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(file_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(file_test PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(file_test PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(file_test PRIVATE echonode_common ws2_32 mswsock ${PLATFORM_LIBS} ${ECHONODE_TLS_LIBS})
else()
    target_link_libraries(file_test PRIVATE echonode_common pthread X11 ${ECHONODE_TLS_LIBS})
endif()

add_executable(shot_test
        tests/test_shot.cpp
        core/Client.cpp
        executor/Dispatcher.cpp
        executor/KeylogExecutor.cpp
        executor/ScreenshotExecutor.cpp
        executor/RemoteDeskExecutor.cpp
        executor/StbImageWrite.cpp
        net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(shot_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/platform)
target_compile_definitions(shot_test PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(shot_test PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(shot_test PRIVATE echonode_common ws2_32 mswsock ${PLATFORM_LIBS} ${ECHONODE_TLS_LIBS})
else()
    target_link_libraries(shot_test PRIVATE echonode_common pthread X11 ${ECHONODE_TLS_LIBS})
endif()
