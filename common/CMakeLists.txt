# 协议契约层：纯头文件，唯一的外部依赖 nlohmann/json 由消费方提供 include 路径
add_library(echonode_common STATIC util/Uuid.cpp util/Sha256.cpp)
target_include_directories(echonode_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(echonode_common PUBLIC cxx_std_20)
