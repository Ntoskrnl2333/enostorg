cmake_minimum_required(VERSION 3.16)

project(enostorg)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Skip install rules for FetchContent dependencies (avoids export-set errors)
set(CMAKE_SKIP_INSTALL_RULES ON)

# Drogon build options (applied before FetchContent)
set(BUILD_SHARED_LIBS OFF  CACHE BOOL "Build drogon as static libraries")
set(BUILD_CTL OFF          CACHE BOOL "Skip building drogon_ctl tool")
set(BUILD_EXAMPLES OFF     CACHE BOOL "Skip building examples")
set(BUILD_TESTING OFF      CACHE BOOL "Skip building tests")
set(BUILD_ORM OFF          CACHE BOOL "Skip building ORM (reduces dependencies)")
set(BUILD_YAML_CONFIG OFF  CACHE BOOL "Skip yaml config support")
set(BUILD_BROTLI OFF       CACHE BOOL "Skip Brotli compression")

include(FetchContent)

# sqlite3: download amalgamation directly
set(SQLITE_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/sqlite-amalgamation.zip")
set(SQLITE_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite3-amalgamation")
file(DOWNLOAD
    "https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip"
    "${SQLITE_ARCHIVE}"
    SHOW_PROGRESS
)
file(ARCHIVE_EXTRACT INPUT "${SQLITE_ARCHIVE}" DESTINATION "${SQLITE_EXTRACT_DIR}")

add_library(sqlite3 STATIC
    "${SQLITE_EXTRACT_DIR}/sqlite-amalgamation-3490100/sqlite3.c"
)
target_include_directories(sqlite3 PUBLIC
    "${SQLITE_EXTRACT_DIR}/sqlite-amalgamation-3490100"
)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
)
if(MSVC)
    target_compile_definitions(sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# zlib: required by Drogon
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
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
FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG        1.9.6
    GIT_SHALLOW    TRUE
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

add_executable(${PROJECT_NAME} main.cpp FileTable.cpp page.inc)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE drogon sqlite3)
