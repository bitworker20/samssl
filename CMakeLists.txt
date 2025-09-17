# === Samon库项目 ===
cmake_minimum_required(VERSION 3.20)
project(samon LANGUAGES CXX)

# 通过 FetchContent 获取 i2pd 源码（替换本地硬编码路径）
include(FetchContent)
FetchContent_Declare(
    i2pd
    GIT_REPOSITORY https://github.com/bitworker20/i2pd.git
    GIT_TAG openssl
)
FetchContent_MakeAvailable(i2pd)

# i2pd 源码根目录（由 FetchContent 提供）
set(I2PD_SOURCE_DIR "${i2pd_SOURCE_DIR}")

# 预编译的 i2pd 静态库目录（仍允许用户覆盖）。
# 默认指向源码目录下的 build/（与原工程约定保持一致）。
set(I2PD_LIB_DIR "${I2PD_SOURCE_DIR}/build/" CACHE PATH "Directory containing prebuilt i2pd static libraries")
set(I2PDCLIENT_LIBRARY "${I2PD_LIB_DIR}")

# 线程包（供本地 configure_target 使用）
find_package(Threads REQUIRED)

# 引入 ExternalProject 用于在 i2pd 源码的 build 目录内构建 libi2pd.a
include(ExternalProject)

# 在 ${I2PD_SOURCE_DIR}/build 目录执行 CMake 配置与构建
ExternalProject_Add(i2pd_project
    SOURCE_DIR "${I2PD_SOURCE_DIR}/build"
    BINARY_DIR "${I2PD_SOURCE_DIR}/build"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -S "${I2PD_SOURCE_DIR}/build" -B "${I2PD_SOURCE_DIR}/build" -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DBUILD_SHARED_LIBS=OFF -DLIBRARY=ON -DBINARY=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build "${I2PD_SOURCE_DIR}/build"
    INSTALL_COMMAND ""
)

# 依赖发现：Boost / spdlog / OpenSSL / ZLIB
find_package(Boost REQUIRED COMPONENTS system context program_options thread)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(spdlog REQUIRED)

# 选择可用的 spdlog 目标
if(TARGET spdlog::spdlog_header_only)
    set(SPDLOG_TARGET spdlog::spdlog_header_only)
else()
    set(SPDLOG_TARGET spdlog::spdlog)
endif()

# 本地实现的 configure_target（源自上游）
function(configure_target target_name)
    # 设置 C++ 标准（继承全局设置）
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # 链接通用依赖
    target_link_libraries(${target_name} PRIVATE Threads::Threads)

    # macOS特定设置
    if(APPLE)
        set_target_properties(${target_name} PROPERTIES
            MACOSX_RPATH ON
        )
    endif()
endfunction()

# 库源文件
set(LIB_SOURCES
    SamConnection.cpp
    SamMessageParser.cpp
    SamService.cpp
    I2PIdentityUtils.cpp
)

add_library(samon STATIC ${LIB_SOURCES})

# 使用父项目的通用配置
configure_target(samon)
add_dependencies(samon i2pd_project)

# 设置包含目录 - 使用target_include_directories而不是include_directories
target_include_directories(samon PRIVATE
    "${I2PD_SOURCE_DIR}/libi2pd"
    "${I2PD_SOURCE_DIR}/libi2pd_client"
    "${I2PD_SOURCE_DIR}/i18n"
)

# 链接依赖 - spdlog使用PUBLIC因为头文件被暴露
target_link_libraries(samon 
    PUBLIC 
        ${SPDLOG_TARGET}  # PUBLIC因为SamConnection.h包含spdlog头文件
    PRIVATE 
        ${I2PDCLIENT_LIBRARY}/libi2pd.a
        Boost::system
        Boost::context
        Boost::program_options
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto    
        ZLIB::ZLIB
)

# === 应用程序 ===
add_executable(i2p_sam_echo_server echo_server.cpp)
add_executable(i2p_sam_echo_client echo_client.cpp)

# 配置应用程序
configure_target(i2p_sam_echo_server)
configure_target(i2p_sam_echo_client)
add_dependencies(i2p_sam_echo_server i2pd_project)
add_dependencies(i2p_sam_echo_client i2pd_project)

# 链接应用程序 - 现在spdlog会自动从samon传播，无需重复链接
foreach(target i2p_sam_echo_server i2p_sam_echo_client)
    target_link_libraries(${target} PRIVATE
        samon  # 这会自动包含spdlog::spdlog（PUBLIC传播）
        ${I2PDCLIENT_LIBRARY}/libi2pd.a     
        Boost::system
        Boost::context
        Boost::program_options
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto    
        ZLIB::ZLIB
    )
endforeach()

# 线程支持已在configure_target中处理，无需重复
# if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin") # Linux or macOS
#     find_package(Threads REQUIRED)
#     target_link_libraries(i2p_sam_echo_server PRIVATE Threads::Threads)
# endif()

