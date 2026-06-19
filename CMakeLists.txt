cmake_minimum_required(VERSION 3.16)

project(enostorg)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Drogon build options (applied before FetchContent)
set(BUILD_SHARED_LIBS OFF  CACHE BOOL "Build drogon as static libraries")
set(BUILD_CTL OFF          CACHE BOOL "Skip building drogon_ctl tool")
set(BUILD_EXAMPLES OFF     CACHE BOOL "Skip building examples")
set(BUILD_TESTING OFF      CACHE BOOL "Skip building tests")
set(BUILD_ORM OFF          CACHE BOOL "Skip building ORM (reduces dependencies)")
set(BUILD_YAML_CONFIG OFF  CACHE BOOL "Skip yaml config support")
set(BUILD_BROTLI OFF       CACHE BOOL "Skip Brotli compression")

include(FetchContent)

# zlib: required by Drogon
# Use native Windows path for file:// URL
set(_zlib_tar "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib.tar.gz")
string(REPLACE "\\" "/" _zlib_tar "${_zlib_tar}")
FetchContent_Declare(
    zlib
    URL  "file://${_zlib_tar}"
)
FetchContent_MakeAvailable(zlib)
if(NOT TARGET ZLIB::ZLIB)
    # The zlib CMake target may be named differently; create alias
    if(TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    elseif(TARGET zlib)
        add_library(ZLIB::ZLIB ALIAS zlib)
    endif()
endif()
# Make FindZLIB work for Drogon's find_package(ZLIB REQUIRED)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}"
    CACHE INTERNAL "zlib include dir")
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}"
    CACHE INTERNAL "zlib include dirs")
set(ZLIB_LIBRARY zlibstatic CACHE INTERNAL "zlib library")
set(ZLIB_LIBRARIES zlibstatic CACHE INTERNAL "zlib library")
set(ZLIB_FOUND TRUE CACHE INTERNAL "")

# jsoncpp: required by Drogon
set(_jsoncpp_tar "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsoncpp.tar.gz")
string(REPLACE "\\" "/" _jsoncpp_tar "${_jsoncpp_tar}")
FetchContent_Declare(
    jsoncpp
    URL  "file://${_jsoncpp_tar}"
)
FetchContent_MakeAvailable(jsoncpp)

# Expose jsoncpp paths for Drogon's FindJsoncpp.cmake
set(JSONCPP_INCLUDE_DIRS
    "${jsoncpp_SOURCE_DIR}/include"
    CACHE INTERNAL "jsoncpp include dirs")
set(JSONCPP_LIBRARIES
    jsoncpp_static
    CACHE INTERNAL "jsoncpp libraries")
set(Jsoncpp_FOUND TRUE CACHE INTERNAL "")

# Drogon
set(USE_SUBMODULE ON CACHE BOOL "Use trantor as a submodule")

FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        v1.9.9
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES "trantor"
)
FetchContent_MakeAvailable(drogon)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
