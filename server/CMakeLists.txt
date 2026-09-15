add_library(server_deps INTERFACE)
target_include_directories(server_deps INTERFACE
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio)
target_compile_definitions(server_deps INTERFACE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(server_deps INTERFACE _WIN32_WINNT=0x0601)
    target_link_libraries(server_deps INTERFACE ws2_32 mswsock)
else()
    target_link_libraries(server_deps INTERFACE pthread)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
            PROPERTIES COMPILE_OPTIONS "-Wno-stringop-overread")
endif()

add_executable(server
        main.cpp
        core/ServerConfig.cpp
        auth/SessionManager.cpp
        db/Database.cpp
        hub/TaskRouter.cpp
        auth/SessionManager.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ws/WsGateway.cpp
        ws/OperatorHub.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c)
target_include_directories(server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR})
target_link_libraries(server PRIVATE echonode_common server_deps)

add_executable(integration_s1
        tests/integration_s1.cpp
        core/ServerConfig.cpp
        auth/SessionManager.cpp
        db/Database.cpp
        hub/TaskRouter.cpp
        auth/SessionManager.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ws/WsGateway.cpp
        ../client/executor/Dispatcher.cpp
        ../client/executor/ProcessExecutor.cpp
        ../client/executor/ShellExecutor.cpp
        ../client/executor/ShellSessionManager.cpp
        ../client/core/Client.cpp
        ../client/net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(integration_s1 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client/platform)
target_compile_definitions(integration_s1 PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(integration_s1 PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(integration_s1 PRIVATE echonode_common server_deps ws2_32 mswsock ${PLATFORM_LIBS})
else()
    target_link_libraries(integration_s1 PRIVATE echonode_common server_deps pthread X11)
endif()

add_executable(integration_s2
        tests/integration_s2.cpp
        auth/SessionManager.cpp
        core/ServerConfig.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ws/WsGateway.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ../client/executor/Dispatcher.cpp
        ../client/executor/ProcessExecutor.cpp
        ../client/executor/ShellExecutor.cpp
        ../client/executor/ShellSessionManager.cpp
        ../client/core/Client.cpp
        ../client/net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(integration_s2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client/platform)
target_compile_definitions(integration_s2 PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(integration_s2 PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(integration_s2 PRIVATE echonode_common server_deps ws2_32 mswsock ${PLATFORM_LIBS})
else()
    target_link_libraries(integration_s2 PRIVATE echonode_common server_deps pthread X11)
endif()

add_executable(integration_s3
        tests/integration_s3.cpp
        auth/SessionManager.cpp
        core/ServerConfig.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ws/WsGateway.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ../client/core/Client.cpp
        ../client/executor/Dispatcher.cpp
        ../client/executor/ProcessExecutor.cpp
        ../client/executor/ShellExecutor.cpp
        ../client/executor/ShellSessionManager.cpp
        ../client/net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(integration_s3 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client/platform)
target_compile_definitions(integration_s3 PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(integration_s3 PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(integration_s3 PRIVATE echonode_common server_deps ws2_32 mswsock ${PLATFORM_LIBS})
else()
    target_link_libraries(integration_s3 PRIVATE echonode_common server_deps pthread X11)
endif()

add_executable(integration_s4
        tests/integration_s4.cpp
        auth/SessionManager.cpp
        core/ServerConfig.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ws/WsGateway.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ../client/core/Client.cpp
        ../client/executor/Dispatcher.cpp
        ../client/executor/FileExecutor.cpp
        ../client/executor/ProcessExecutor.cpp
        ../client/executor/ShellSessionManager.cpp
        ../client/net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(integration_s4 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client/platform)
target_compile_definitions(integration_s4 PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(integration_s4 PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(integration_s4 PRIVATE echonode_common server_deps ws2_32 mswsock ${PLATFORM_LIBS})
else()
    target_link_libraries(integration_s4 PRIVATE echonode_common server_deps pthread X11)
endif()

add_executable(integration_s5
        tests/integration_s5.cpp
        auth/SessionManager.cpp
        core/ServerConfig.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ws/WsGateway.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ../client/core/Client.cpp
        ../client/executor/Dispatcher.cpp
        ../client/executor/ScreenshotExecutor.cpp
        ../client/executor/StbImageWrite.cpp
        ../client/executor/ProcessExecutor.cpp
        ../client/executor/ShellSessionManager.cpp
        ../client/net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(integration_s5 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client/platform)
target_compile_definitions(integration_s5 PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(integration_s5 PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(integration_s5 PRIVATE echonode_common server_deps ws2_32 mswsock ${PLATFORM_LIBS})
else()
    target_link_libraries(integration_s5 PRIVATE echonode_common server_deps pthread X11)
endif()

add_executable(integration_s6
        tests/integration_s6.cpp
        auth/SessionManager.cpp
        core/ServerConfig.cpp
        db/Database.cpp
        hub/AgentHub.cpp
        hub/TaskRouter.cpp
        http/HttpApi.cpp
        ws/OperatorHub.cpp
        ws/WsGateway.cpp
        ${ECHONODE_THIRD_PARTY}/sqlite3/sqlite3.c
        ../client/core/Client.cpp
        ../client/net/WsClient.cpp
        ${PLATFORM_SOURCES})
target_include_directories(integration_s6 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${ECHONODE_THIRD_PARTY}
        ${ECHONODE_THIRD_PARTY}/asio
        ${CMAKE_SOURCE_DIR}/client
        ${CMAKE_SOURCE_DIR}/client/platform)
target_compile_definitions(integration_s6 PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
        NOMINMAX)
if(WIN32)
    target_compile_definitions(integration_s6 PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(integration_s6 PRIVATE echonode_common server_deps ws2_32 mswsock ${PLATFORM_LIBS})
else()
    target_link_libraries(integration_s6 PRIVATE echonode_common server_deps pthread X11)
endif()
